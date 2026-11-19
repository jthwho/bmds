/**
 * @file      update.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Checking and applying update packages (devplan/update.md §3): everything that touches the flash
 * runs in one job on an internal stack, reading the package from the card in chunks.
 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_attr.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_flash_internal.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <hal/efuse_hal.h>
#include <sdkconfig.h>
#include <bmds/blake2s.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/input.h>
#include <bmds/inputevent.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/notificationlog.h>
#include <bmds/notifications.h>
#include <bmds/romdisk.h>
#include <bmds/romeditor.h>
#include <bmds/romimage.h>
#include <bmds/rommanifest.h>
#include <bmds/storage.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/thread.h>
#include <bmds/update.h>
#include <bmds/updatekeys.h>
#include <bmds/version.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using UpdatePackage::SectionType;

        // Card reads and flash writes go through this, in internal RAM (flash is written from it
        // with the caches off).
        constexpr size_t   ChunkBytes = 16 * 1024;
        constexpr size_t   JobStackBytes = 12 * 1024;
        constexpr uint32_t SectorBytes = 4096;
        constexpr uint32_t EraseStepBytes = 256 * 1024; // a multiple of the 64 KB erase block
        // How long recovery shows how an update ended before it restarts.
        constexpr unsigned RestartPauseMs = 2000;
        // How long the main firmware's balloon about an update shows (a failure's stays: critical).
        constexpr int32_t ResultBalloonMs = 15000;

        std::atomic<bool> busy{false};

        std::atomic<const Update::Observer *> observer{nullptr};

        // Where packages are kept: what tools/bmds-update copies to, and (step 4) what the
        // Updater lists.
        BMDS_SYSCONF(updateDir, "update/dir", Config::Type::Path, "/sd0/p0/update",
                     "Where update packages (.bmup) are kept: tools/bmds-update copies them here", 0, 255, nullptr,
                     nullptr)
        BMDS_SYSCONF(romBackupDir, "update/rom-backup", Config::Type::Path, "/sd0/p0/rom-backup",
                     "Where an update keeps the user's ROM files it replaces (rom install's): a folder for each "
                     "update, by its version",
                     0, 255, nullptr, nullptr)

        enum class Mode : uint8_t {
                Check,
                CheckHead, // the head in `head` alone (`Update::checkHead()`)
                ApplyMain,
                ApplyRecovery
        };

        struct Job {
                        Mode             mode = Mode::Check;
                        StringView       path;
                        bool             force = false;
                        bool             bootloader = false;   // the user confirmed writing one (§20)
                        const uint8_t   *expectHead = nullptr; // the intent's head hash, or null
                        Update::Report  *report = nullptr;
                        Update::Progress progress = nullptr;
                        void            *arg = nullptr;
                        FileHandle::Ptr  file;
                        uint8_t         *chunk = nullptr;
                        uint8_t          head[UpdatePackage::MaxHeadBytes];
                        size_t           headBytes = 0; // CheckHead: what `head` holds
                        uint64_t         fileBytes = 0; // CheckHead: the whole file's size
                        Error            result;
                        String           resultNote; // more lines for the result (a ROM backup)
                        bool             romMerged = false;
        };

        // The bootloader's place (the P4's: from its offset to the partition table), which the
        // partition table doesn't list: registered as IDF's own bootloader updates do, once.
        const esp_partition_t *bootloaderPartition() {
                static const esp_partition_t *partition = nullptr; // jobs run one at a time (busy)
                if (partition != nullptr) return partition;
                const esp_err_t err = esp_partition_register_external(
                        esp_flash_default_chip, CONFIG_BOOTLOADER_OFFSET_IN_FLASH,
                        CONFIG_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH, "bootloader",
                        ESP_PARTITION_TYPE_BOOTLOADER, ESP_PARTITION_SUBTYPE_BOOTLOADER_PRIMARY, &partition);
                if (err != ESP_OK) {
                        bmdsErr("Update: can't register the bootloader's partition: %d", int(err));
                        partition = nullptr;
                }
                return partition;
        }

        const esp_partition_t *partitionFor(uint32_t type) {
                switch (static_cast<SectionType>(type)) {
                        case SectionType::Firmware:
                                return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                                                nullptr);
                        case SectionType::Recovery:
                                return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                                                nullptr);
                        case SectionType::Rom:
                                return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                                "rom");
                        case SectionType::Bootloader: return bootloaderPartition();
                        default: return nullptr;
                }
        }

        const char *sectionName(uint32_t type) {
                switch (static_cast<SectionType>(type)) {
                        case SectionType::Firmware: return "the firmware";
                        case SectionType::Recovery: return "the recovery firmware";
                        case SectionType::Rom: return "the ROM";
                        case SectionType::Notes: return "the release notes";
                        case SectionType::Bootloader: return "the bootloader";
                }
                return "a section";
        }

        void report(Job &job, Update::Stage stage, uint64_t done, uint64_t total) {
                if (job.progress != nullptr) job.progress(job.arg, stage, done, total);
                const Update::Observer *o = observer.load(std::memory_order_acquire);
                if (o != nullptr && o->progress != nullptr && job.mode == Mode::ApplyRecovery) {
                        o->progress(o->arg, stage, done, total);
                }
                return;
        }

        Error readAt(Job &job, uint64_t offset, uint8_t *to, size_t bytes) {
                auto [at, serr] = job.file->stream().seek(int64_t(offset), Stream::SeekFrom::Start);
                if (serr.isError()) return serr;
                (void)at;
                while (bytes > 0) {
                        auto [n, err] = job.file->read(WritableByteSpan(to, bytes), WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::EndOfFile;
                        to += n;
                        bytes -= n;
                }
                return Error::Ok;
        }

        // A check's verdict: false with the problem noted.
        Result<bool> refuse(Job &job, const char *problem) {
                job.report->problem = problem;
                return makeResult(false);
        }

        // Hashes @p bytes of @p partition from its start.
        Error hashPartition(Job &job, const esp_partition_t *partition, uint32_t bytes, uint8_t (&digest)[32],
                            Update::Stage stage, uint64_t &done, uint64_t total) {
                Blake2s h;
                for (uint32_t at = 0; at < bytes;) {
                        const uint32_t n = bytes - at < ChunkBytes ? bytes - at : uint32_t(ChunkBytes);
                        if (esp_partition_read(partition, at, job.chunk, n) != ESP_OK) return Error::IoError;
                        h.update(job.chunk, n);
                        at += n;
                        done += n;
                        report(job, stage, done, total);
                }
                h.final(digest);
                return Error::Ok;
        }

        Error writeChecked(Job &job, size_t index, Update::Stage stage);

        // --- The ROM, file by file (devplan/rom.md §12g) ------------------------------------------

        // The ROM's partition mapped and written, for a RomEditor. Each write turns the caches off:
        // on the job's internal stack.
        class PartitionRom final : public RomFlash {
                public:
                        explicit PartitionRom(const esp_partition_t *p) : _p(p) {
                                const void *mapped = nullptr;
                                if (p != nullptr && esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &mapped,
                                                                       &_map) == ESP_OK)
                                        _bytes = ByteSpan(static_cast<const uint8_t *>(mapped), p->size);
                        }
                        ~PartitionRom() override {
                                if (!_bytes.isEmpty()) esp_partition_munmap(_map);
                        }
                        PartitionRom(const PartitionRom &) = delete;
                        PartitionRom &operator=(const PartitionRom &) = delete;

                        bool     isMapped() const { return !_bytes.isEmpty(); }
                        ByteSpan bytes() const override { return _bytes; }
                        Error    erase(uint32_t offset, uint32_t bytes) override {
                                return esp_partition_erase_range(_p, offset, bytes) == ESP_OK ? Error::Ok
                                                                                                 : Error::IoError;
                        }
                        Error write(uint32_t offset, ByteSpan data) override {
                                return esp_partition_write(_p, offset, data.data(), data.size()) == ESP_OK
                                               ? Error::Ok
                                               : Error::IoError;
                        }
                        Error read(uint32_t offset, WritableByteSpan out) override {
                                return esp_partition_read(_p, offset, out.data(), out.size()) == ESP_OK
                                               ? Error::Ok
                                               : Error::IoError;
                        }

                private:
                        const esp_partition_t      *_p;
                        esp_partition_mmap_handle_t _map = 0;
                        ByteSpan                    _bytes;
        };

        // The build id a ROM tree is written for: the package's firmware (none: any).
        Span<const uint8_t> romFirmware(const Update::Report &r) {
                bool any = true;
                for (uint8_t b : r.package.buildId) any = any && b == 0;
                return any ? Span<const uint8_t>() : Span<const uint8_t>(r.package.buildId, sizeof(r.package.buildId));
        }

        // The tree the installed main firmware (ota_0's) boots with (rom.md §12e): kept whole by an
        // update's writes, so if the new firmware doesn't make it the old one still has its own.
        // Empty if there is none.
        RomImage installedTree(const PartitionRom &rom) {
                const esp_partition_t *main = partitionFor(uint32_t(SectionType::Firmware));
                esp_app_desc_t         desc;
                if (main == nullptr || esp_ota_get_partition_description(main, &desc) != ESP_OK) return RomImage();
                auto [tree, err] = RomImage::open(rom.bytes(), Span<const uint8_t>(desc.app_elf_sha256, 32));
                return err.isOk() ? tree : RomImage();
        }

        // The ROM section read whole into PSRAM (its hash checked), freed by its owner.
        // Progress is told as @p base + bytes read of @p total.
        Result<uint8_t *> readRomSection(Job &job, const UpdatePackage::Section &s, Update::Stage stage, uint64_t base,
                                         uint64_t total) {
                auto *bytes = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, s.size + 1));
                if (bytes == nullptr) return makeError<uint8_t *>(Error::NoMem);
                Blake2s h;
                for (uint32_t at = 0; at < s.size;) {
                        const uint32_t n = s.size - at < ChunkBytes ? s.size - at : uint32_t(ChunkBytes);
                        const Error    err = readAt(job, uint64_t(s.offset) + at, bytes + at, n);
                        if (err.isError()) {
                                Memory::free(bytes);
                                return makeError<uint8_t *>(err);
                        }
                        h.update(bytes + at, n);
                        at += n;
                        report(job, stage, base + at, total);
                }
                uint8_t digest[32];
                h.final(digest);
                if (std::memcmp(digest, s.hash, sizeof(digest)) != 0) {
                        Memory::free(bytes);
                        return makeError<uint8_t *>(Error::CorruptData);
                }
                return makeResult(bytes);
        }

        // How the package's ROM compares with the partition's tree: the same when its system files
        // are the package's and it is for this firmware or any; else it differs, and must fit
        // beside the user's files (refused otherwise, nothing written: §12i Q2).
        Result<bool> checkRom(Job &job, size_t index, const uint8_t *bytes) {
                Update::Report               &r = *job.report;
                const UpdatePackage::Section &s = r.package.sections[index];
                auto [package, perr] = RomImage::open(ByteSpan(bytes, s.size));
                if (perr.isError()) return refuse(job, "the ROM's image is damaged");
                PartitionRom              rom(partitionFor(uint32_t(SectionType::Rom)));
                const Span<const uint8_t> firmware = romFirmware(r);
                r.flashed[index] = Update::Flashed::Differs;
                if (!rom.isMapped()) return makeResult(true);
                // An update builds on the newest tree, whatever firmware it is for: the user's last
                // changes, or the last update's.
                auto [tree, terr] = RomImage::open(rom.bytes());
                if (terr.isError()) return makeResult(true); // no tree there: written whole
                bool tied = true;                            // for this firmware, or any
                for (uint8_t b : tree.version().firmware) tied = tied && b == 0;
                if (!tied && !firmware.isEmpty())
                        tied = std::memcmp(tree.version().firmware.data(), firmware.data(), firmware.size()) == 0;
                if (tied && RomManifest::sameSystemFiles(tree, package)) {
                        r.flashed[index] = Update::Flashed::Same;
                        return makeResult(true);
                }
                const RomImage installed = installedTree(rom);
                RomEditor      editor(rom, installed.version().bytes > 0 ? &installed : nullptr, {});
                if (editor.load().isError()) return makeResult(true);
                const Error fits = editor.checkApply(package);
                if (fits == Error::NoSpace) {
                        const RomEditor::Shortfall &f = editor.shortfall();
                        bmdsWarn("Update: the ROM's new files need %u KB together; %u KB free beside the user's",
                                 static_cast<unsigned>(f.needed / 1024), static_cast<unsigned>(f.freeAfterBoot / 1024));
                        return refuse(job, "the ROM's new files don't fit beside the user's (rom remove some, rom "
                                           "compact, then try again)");
                }
                if (fits.isError()) return makeError<bool>(fits);
                return makeResult(true);
        }

        // Recovery's ROM on one bar: the section read (the first half), then the files written.
        struct RomProgress {
                        Job     *job;
                        uint64_t read;
        };

        void romProgress(void *context, uint64_t done, uint64_t total) {
                const auto &p = *static_cast<RomProgress *>(context);
                report(*p.job, Update::Stage::WritingRom, p.read + (total > 0 ? done * p.read / total : 0), 2 * p.read);
                return;
        }

        void romDisplaced(void *, StringView path) {
                bmdsWarn("Update: the system's /rom/%.*s takes the place of the user's", static_cast<int>(path.size()),
                         path.data());
                return;
        }

        // The user's ROM files an update would replace (rom.md §12g): copied to the card first, into
        // update/rom-backup/VERSION/ by their paths, unless the package's file is the same bytes.
        // A system file is only ever an update's, so nothing else is backed up.
        struct Backup {
                        Job            *job;
                        const RomImage *tree;
                        const RomImage *package;
                        String          dir; // this update's folder, once made
                        uint32_t        files = 0;
                        Error           err;
        };

        Error makeDirs(StringView path) {
                Vfs::Context context;
                for (size_t i = 1; i <= path.size(); ++i) {
                        if (i < path.size() && path[i] != '/') continue;
                        const Error made = Vfs::mkdir(path.left(i), context);
                        if (made.isError() && made != Error::Exists) return made;
                }
                return Error::Ok;
        }

        void backUp(void *context, StringView path) {
                auto &b = *static_cast<Backup *>(context);
                if (b.err.isError()) return;
                bool       noPath = false;
                const auto have = b.tree->find(path, noPath);
                if (have.error().isError() || have.value().directory) return;
                const RomImage::Entry e = b.tree->entry(have.value().index);
                const auto            want = b.package->find(path, noPath);
                if (want.error().isOk() && !want.value().directory) {
                        const RomImage::Entry p = b.package->entry(want.value().index);
                        if (p.data.size() == e.data.size() &&
                            std::memcmp(p.hash.data(), e.hash.data(), RomImage::HashBytes) == 0)
                                return; // the same bytes: nothing of the user's is lost
                }
                Vfs::Context context2;
                if (b.dir.isEmpty()) {
                        // update/rom-backup/VERSION, or VERSION-2 ... if that is there already.
                        String base;
                        if (SysConf::get(StringView("update/rom-backup"), base).isError() || base.isEmpty()) {
                                b.err = Error::NotExist;
                                return;
                        }
                        for (unsigned n = 1; n < 100 && b.dir.isEmpty(); ++n) {
                                String candidate;
                                (void)candidate.appendf("%s/%s", base.cStr(), b.job->report->package.versionText);
                                if (n > 1) (void)candidate.appendf("-%u", n);
                                if (Vfs::stat(candidate.view(), context2).error() == Error::NotExist)
                                        b.dir = std::move(candidate);
                        }
                        if (b.dir.isEmpty()) {
                                b.err = Error::Exists;
                                return;
                        }
                }
                String target;
                (void)target.appendf("%s/%.*s", b.dir.cStr(), int(path.size()), path.data());
                const size_t slash = target.view().lastIndexOf('/');
                b.err = makeDirs(target.view().left(slash));
                if (b.err.isError()) return;
                auto [file, oerr] = Vfs::open(target.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, context2);
                if (oerr.isError()) {
                        b.err = oerr;
                        return;
                }
                // Through internal RAM: the card's driver isn't given flash addresses.
                for (size_t at = 0; at < e.data.size() && b.err.isOk();) {
                        const size_t n = std::min(e.data.size() - at, ChunkBytes);
                        std::memcpy(b.job->chunk, e.data.data() + at, n);
                        auto [w, werr] = file->write(ByteSpan(b.job->chunk, n), WaitForever);
                        if (werr.isOk() && w != n) werr = Error::IoError;
                        b.err = werr;
                        at += n;
                }
                if (b.err.isOk()) {
                        ++b.files;
                        bmdsInfo("Update: the user's /rom/%.*s backed up to %s", int(path.size()), path.data(),
                                 target.cStr());
                }
                return;
        }

        // After an update (its firmware written too): the ROM's holes filled (rom.md §12p). In
        // recovery nothing is mapped and the new tree is the one that boots, so what moved is free at
        // once and it goes again until nothing moves.
        void compactRom(Job &job) {
                PartitionRom rom(partitionFor(uint32_t(SectionType::Rom)));
                RomEditor    editor(rom, nullptr, {});
                editor.setTime(Clock::now());
                if (!rom.isMapped() || editor.load().isError()) return;
                uint32_t moved = 0;
                for (int round = 0; round < 16; ++round) {
                        RomEditor::Report r;
                        bool              more = false;
                        const Error       err = editor.compact(r, more);
                        if (err.isError()) {
                                bmdsWarn("Update: compacting the ROM: %s", err.desc());
                                break;
                        }
                        if (r.moved == 0) break;
                        moved += r.moved;
                        report(job, Update::Stage::WritingRom, uint64_t(round) + 1, 16);
                }
                if (moved > 0) bmdsInfo("Update: the ROM compacted: %u files moved", static_cast<unsigned>(moved));
                return;
        }

        // Recovery's ROM: the package's tree merged into the partition's (its system files written
        // where they differ, the user's kept), compacted first if that makes it fit; written whole
        // when the partition holds no tree (blank, or another format).
        Error writeRom(Job &job, size_t index) {
                const UpdatePackage::Section &s = job.report->package.sections[index];
                auto [bytes, rerr] = readRomSection(job, s, Update::Stage::WritingRom, 0, 2 * uint64_t(s.size));
                RomProgress progress{&job, s.size};
                if (rerr.isError()) return rerr;
                auto [package, perr] = RomImage::open(ByteSpan(bytes, s.size));
                Error err = perr.isError() ? Error(Error::CorruptData) : Error(Error::Ok);
                bool  whole = false;
                if (err.isOk()) {
                        PartitionRom              rom(partitionFor(uint32_t(SectionType::Rom)));
                        const Span<const uint8_t> firmware = romFirmware(*job.report);
                        const RomImage            installed = installedTree(rom);
                        RomEditor                 editor(rom, installed.version().bytes > 0 ? &installed : nullptr, {});
                        editor.setTime(Clock::now());
                        editor.setProgress(romProgress, &progress);
                        if (!rom.isMapped() || editor.load().isError()) {
                                whole = true;
                        } else {
                                // The user's files it would replace: to the card first; if that
                                // fails, nothing is changed.
                                Backup backup{&job, &editor.committed(), &package, {}, 0, Error::Ok};
                                err = editor.checkApply(package, backUp, &backup);
                                if (err == Error::NoSpace) err = Error::Ok; // compacted below, then checked
                                if (err.isOk() && backup.err.isError()) {
                                        bmdsErr("Update: backing up the user's ROM files: %s", backup.err.desc());
                                        err = Error::Cancelled;
                                }
                                if (err.isOk() && backup.files > 0) {
                                        (void)Vfs::sync();
                                        (void)job.resultNote.appendf("backup: %u of the user's ROM files to %s\n",
                                                                     static_cast<unsigned>(backup.files),
                                                                     backup.dir.cStr());
                                }
                                RomEditor::Report done;
                                if (err.isOk()) err = editor.apply(package, firmware, romDisplaced, nullptr, done);
                                // Too little room together: the files moved down (free at once in
                                // recovery), then again.
                                for (int round = 0; err == Error::NoSpace && round < 8; ++round) {
                                        RomEditor::Report moved;
                                        bool              more = false;
                                        if (editor.compact(moved, more).isError() || moved.moved == 0) break;
                                        bmdsInfo("Update: the ROM compacted: %u files moved",
                                                 static_cast<unsigned>(moved.moved));
                                        err = editor.apply(package, firmware, romDisplaced, nullptr, done);
                                }
                                if (err.isOk()) {
                                        bmdsInfo("Update: the ROM: %u files written (%u KB), %u kept, %u removed, %u "
                                                 "of the "
                                                 "user's displaced; tree version %u",
                                                 static_cast<unsigned>(done.written),
                                                 static_cast<unsigned>(done.bytesWritten / 1024),
                                                 static_cast<unsigned>(done.kept), static_cast<unsigned>(done.removed),
                                                 static_cast<unsigned>(done.displaced),
                                                 static_cast<unsigned>(done.sequence));
                                        job.report->flashed[index] = Update::Flashed::Same;
                                        job.romMerged = true;
                                } else if (err == Error::NoSpace) {
                                        bmdsErr("Update: the ROM's new files don't fit beside the user's");
                                }
                        }
                }
                Memory::free(bytes);
                if (whole) {
                        bmdsInfo("Update: no ROM tree to merge into: the ROM written whole");
                        return writeChecked(job, index, Update::Stage::WritingRom);
                }
                return err;
        }

        // The head's checks (devplan/update.md §2): the signature, the board, the chip revision, the
        // version against the main firmware's. True for a good head.
        Result<bool> checkHead(Job &job, Span<const uint8_t> head, uint64_t fileBytes) {
                Update::Report &r = *job.report;
                UpdatePackage::Verdict v = UpdatePackage::parse(head, fileBytes, r.package);
                if (v != UpdatePackage::Verdict::Ok) return refuse(job, UpdatePackage::verdictText(v));
                const Span<const UpdatePackage::Key> keys = UpdateKeys::accepted();
                size_t                               keyIndex = 0;
                v = UpdatePackage::verify(r.package, head, keys, keyIndex);
                if (v == UpdatePackage::Verdict::Ok) r.devKey = UpdateKeys::isDevKey(keyIndex);
                if (v != UpdatePackage::Verdict::Ok) {
                        return refuse(job, UpdateKeys::isDevMode() ? "it isn't signed by a key this firmware accepts"
                                                                   : "it isn't signed by a trusted key");
                }
                {
                        Blake2s h;
                        h.update(job.head, r.package.signedBytes);
                        h.final(r.headHash);
                }

                // The main firmware it would replace, to compare versions with.
                const esp_partition_t *main = partitionFor(uint32_t(SectionType::Firmware));
                esp_app_desc_t         desc;
                UpdatePackage::Version mainVersion;
                if (main != nullptr && esp_ota_get_partition_description(main, &desc) == ESP_OK) {
                        std::snprintf(r.mainVersion, sizeof(r.mainVersion), "%s", desc.version);
                }
                if (UpdatePackage::parseVersion(r.mainVersion, mainVersion)) {
                        r.order = r.package.version < mainVersion ? -1 : r.package.version > mainVersion ? 1 : 0;
                } else {
                        r.order = 1; // no main firmware to speak of: anything is newer
                }

                if (std::strcmp(r.package.board, CONFIG_BMDS_BOARD_DIR) != 0)
                        return refuse(job, "it is for another board");
                esp_chip_info_t chip;
                esp_chip_info(&chip);
                if (chip.revision < r.package.minChipRevision) return refuse(job, "it needs a newer chip revision");
                return makeResult(true);
        }

        // The checks of devplan/update.md §2. True for a good package.
        Result<bool> checkPackage(Job &job) {
                Update::Report &r = *job.report;
                r = Update::Report{};
                Vfs::Context context;
                auto [file, oerr] = Vfs::open(job.path, Vfs::Read, context);
                if (oerr.isError()) return makeError<bool>(oerr);
                job.file = file;
                auto [fileBytes, serr] = job.file->stream().seek(0, Stream::SeekFrom::End);
                if (serr.isError()) return makeError<bool>(serr);

                const size_t headRead = fileBytes < sizeof(job.head) ? size_t(fileBytes) : sizeof(job.head);
                Error        err = readAt(job, 0, job.head, headRead);
                if (err.isError()) return makeError<bool>(err);
                auto [headGood, herr] = checkHead(job, Span<const uint8_t>(job.head, headRead), uint64_t(fileBytes));
                if (herr.isError() || !headGood) return Result<bool>(headGood, herr);

                uint64_t total = 0;
                for (size_t i = 0; i < r.package.sectionCount; ++i) {
                        const UpdatePackage::Section &s = r.package.sections[i];
                        total += s.size;
                        if (partitionFor(s.type) != nullptr) total += s.size;
                }
                uint64_t done = 0;
                for (size_t i = 0; i < r.package.sectionCount; ++i) {
                        const UpdatePackage::Section &s = r.package.sections[i];
                        const esp_partition_t        *partition = partitionFor(s.type);
                        if (partition != nullptr && s.size > partition->size) {
                                return refuse(job, "an image is bigger than its partition");
                        }
                        Blake2s h;
                        for (uint32_t at = 0; at < s.size;) {
                                const uint32_t n = s.size - at < ChunkBytes ? s.size - at : uint32_t(ChunkBytes);
                                err = readAt(job, uint64_t(s.offset) + at, job.chunk, n);
                                if (err.isError()) return makeError<bool>(err);
                                if (at == 0 && (s.type == uint32_t(SectionType::Firmware) ||
                                                s.type == uint32_t(SectionType::Recovery) ||
                                                s.type == uint32_t(SectionType::Bootloader))) {
                                        UpdatePackage::ChipRevisions revisions;
                                        if (!UpdatePackage::appRevisions(Span<const uint8_t>(job.chunk, n),
                                                                         CONFIG_IDF_FIRMWARE_CHIP_ID, revisions)) {
                                                return refuse(job, "an app image isn't one for this chip");
                                        }
                                        // As the bootloader decides: an image it wouldn't start is never written.
                                        if (!UpdatePackage::runsOn(revisions, uint16_t(efuse_hal_chip_revision()),
                                                                   efuse_hal_get_disable_wafer_version_major())) {
                                                return refuse(job, "an app image isn't for this chip's revision");
                                        }
                                }
                                if (at == 0 && s.type == uint32_t(SectionType::Rom) &&
                                    (n < 4 || std::memcmp(job.chunk, "BMRO", 4) != 0)) {
                                        return refuse(job, "the ROM's image has no BMRO header");
                                }
                                h.update(job.chunk, n);
                                at += n;
                                done += n;
                                report(job, Update::Stage::Checking, done, total);
                        }
                        uint8_t digest[32];
                        h.final(digest);
                        if (std::memcmp(digest, s.hash, sizeof(digest)) != 0) {
                                return refuse(job, UpdatePackage::verdictText(UpdatePackage::Verdict::BadHash));
                        }
                        if (partition == nullptr) continue;
                        if (s.type == uint32_t(SectionType::Rom)) {
                                // Its tree against the partition's (§12g), not its bytes.
                                auto [bytes, rerr] = readRomSection(job, s, Update::Stage::Checking, done, total);
                                if (rerr == Error::CorruptData)
                                        return refuse(job, UpdatePackage::verdictText(UpdatePackage::Verdict::BadHash));
                                if (rerr.isError()) return makeError<bool>(rerr);
                                auto [good, cerr] = checkRom(job, i, bytes);
                                Memory::free(bytes);
                                if (cerr.isError() || !good) return Result<bool>(good, cerr);
                                done += s.size;
                                report(job, Update::Stage::Checking, done, total);
                                continue;
                        }
                        err = hashPartition(job, partition, s.size, digest, Update::Stage::Checking, done, total);
                        if (err.isError()) return makeError<bool>(err);
                        r.flashed[i] = std::memcmp(digest, s.hash, sizeof(digest)) == 0 ? Update::Flashed::Same
                                                                                        : Update::Flashed::Differs;
                }
                return makeResult(true);
        }

        // Writes section @p index into its partition, then reads it back against its hash.
        Error writeSection(Job &job, size_t index, Update::Stage stage) {
                const UpdatePackage::Section &s = job.report->package.sections[index];
                const esp_partition_t        *partition = partitionFor(s.type);
                if (partition == nullptr) return Error::NotExist;
                bmdsInfo("Update: writing %s (%u bytes) to %s", sectionName(s.type), static_cast<unsigned>(s.size),
                         partition->label);
                // Erasing, writing and reading back, a third each: the erase alone is most of the time
                // (a 3.5 MB ROM: 17 s), and a bar that doesn't move then looks stuck.
                const uint64_t total = uint64_t(s.size) * 3;
                uint64_t       done = 0;
                report(job, stage, 0, total);
                const uint32_t erase = (s.size + SectorBytes - 1) / SectorBytes * SectorBytes;
                for (uint32_t at = 0; at < erase; at += EraseStepBytes) {
                        const uint32_t n = erase - at < EraseStepBytes ? erase - at : EraseStepBytes;
                        if (esp_partition_erase_range(partition, at, n) != ESP_OK) return Error::IoError;
                        done = at + n < s.size ? at + n : s.size;
                        report(job, stage, done, total);
                }
                for (uint32_t at = 0; at < s.size;) {
                        const uint32_t n = s.size - at < ChunkBytes ? s.size - at : uint32_t(ChunkBytes);
                        const Error    err = readAt(job, uint64_t(s.offset) + at, job.chunk, n);
                        if (err.isError()) return err;
                        if (esp_partition_write(partition, at, job.chunk, n) != ESP_OK) return Error::IoError;
                        at += n;
                        done += n;
                        report(job, stage, done, total);
                }
                uint8_t     digest[32];
                const Error err = hashPartition(job, partition, s.size, digest, stage, done, total);
                if (err.isError()) return err;
                if (std::memcmp(digest, s.hash, sizeof(digest)) != 0) {
                        bmdsErr("Update: %s reads back wrong from %s", sectionName(s.type), partition->label);
                        return Error::CorruptData;
                }
                job.report->flashed[index] = Update::Flashed::Same;
                return Error::Ok;
        }

        // Recovery writes a section again when it reads back wrong: a bad read from the card (one
        // seen, devplan/update.md §11) shouldn't stop an update that a second pass completes.
        Error writeChecked(Job &job, size_t index, Update::Stage stage) {
                Error err = writeSection(job, index, stage);
                for (int attempt = 1; attempt < 3 && err == Error::CorruptData; ++attempt) {
                        bmdsWarn("Update: writing it again");
                        err = writeSection(job, index, stage);
                }
                return err;
        }

        // The flash's guard against writes to the bootloader's region (CONFIG_SPI_FLASH_DANGEROUS_
        // WRITE_ABORTS), lifted only around them, as IDF's own bootloader updates do.
        Error writeBootloaderImage(const esp_partition_t *partition, const uint8_t *image, uint32_t bytes) {
                (void)esp_flash_set_dangerous_write_protection(esp_flash_default_chip, false);
                const uint32_t erase = (bytes + SectorBytes - 1) / SectorBytes * SectorBytes;
                esp_err_t      err = esp_partition_erase_range(partition, 0, erase);
                if (err == ESP_OK) err = esp_partition_write(partition, 0, image, bytes);
                (void)esp_flash_set_dangerous_write_protection(esp_flash_default_chip, true);
                return err == ESP_OK ? Error::Ok : Error::IoError;
        }

        // The bootloader (devplan/update.md §20): unlike the other sections, the whole image is read
        // and checked in memory before anything is erased, so a bad read from the card can't cut a
        // write short; the old one is kept in memory and written back if the new one never reads
        // back right. A cut-off write still leaves a board that only a cable starts again: the
        // window is the erase and the write, well under a second.
        Error writeBootloader(Job &job, size_t index) {
                const UpdatePackage::Section &s = job.report->package.sections[index];
                const esp_partition_t        *partition = partitionFor(s.type);
                if (partition == nullptr) return Error::NotExist;
                if (s.size > partition->size) return Error::OutOfRange;
                const Update::Stage stage = Update::Stage::WritingBootloader;
                const uint64_t      total = uint64_t(s.size) * 3;
                report(job, stage, 0, total);
                auto *image = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, partition->size));
                auto *old = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, partition->size));
                Error err = image != nullptr && old != nullptr ? Error(Error::Ok) : Error(Error::NoMem);
                if (err.isOk()) err = readAt(job, s.offset, image, s.size);
                if (err.isOk()) {
                        uint8_t digest[32];
                        Blake2s h;
                        h.update(image, s.size);
                        h.final(digest);
                        if (std::memcmp(digest, s.hash, sizeof(digest)) != 0) err = Error::CorruptData;
                }
                if (err.isOk() && esp_partition_read(partition, 0, old, partition->size) != ESP_OK)
                        err = Error::IoError;
                if (err.isError()) { // nothing written
                        Memory::free(image);
                        Memory::free(old);
                        return err;
                }
                report(job, stage, s.size, total);
                bmdsWarn("Update: writing the bootloader (%u bytes): keep the power on", static_cast<unsigned>(s.size));
                for (int attempt = 0; attempt < 3; ++attempt) {
                        if (attempt > 0) bmdsWarn("Update: writing the bootloader again");
                        err = writeBootloaderImage(partition, image, s.size);
                        uint64_t done = uint64_t(s.size) * 2;
                        uint8_t  digest[32];
                        if (err.isOk()) err = hashPartition(job, partition, s.size, digest, stage, done, total);
                        if (err.isOk() && std::memcmp(digest, s.hash, sizeof(digest)) != 0) err = Error::CorruptData;
                        if (err.isOk()) break;
                }
                if (err.isError()) {
                        bmdsErr("Update: the bootloader didn't write right: writing the old one back");
                        if (writeBootloaderImage(partition, old, partition->size).isError())
                                bmdsErr("Update: the old bootloader couldn't be written back either");
                } else {
                        job.report->flashed[index] = Update::Flashed::Same;
                        bmdsInfo("Update: the bootloader is written");
                }
                Memory::free(image);
                Memory::free(old);
                return err;
        }

        const UpdatePackage::Section *differing(const Update::Report &r, SectionType type, size_t &index) {
                for (size_t i = 0; i < r.package.sectionCount; ++i) {
                        if (r.package.sections[i].type == uint32_t(type) && r.flashed[i] == Update::Flashed::Differs) {
                                index = i;
                                return &r.package.sections[i];
                        }
                }
                return nullptr;
        }

        void hex(const uint8_t *bytes, size_t n, char *out) {
                for (size_t i = 0; i < n; ++i) {
                        out[2 * i] = "0123456789abcdef"[bytes[i] >> 4];
                        out[2 * i + 1] = "0123456789abcdef"[bytes[i] & 0xf];
                }
                out[2 * n] = '\0';
                return;
        }

        Error writeFile(StringView path, StringView text) {
                Vfs::Context context;
                const Error  made = Vfs::mkdir("/flash/update", context);
                if (made.isError() && made != Error::Exists) return made;
                auto [file, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                if (err.isError()) return err;
                auto [n, werr] = file->write(text, WaitForever);
                if (werr.isOk() && n != text.size()) werr = Error::IoError;
                return werr;
        }

        Result<String> readFile(StringView path) {
                Vfs::Context context;
                auto [file, err] = Vfs::open(path, Vfs::Read, context);
                if (err.isError()) return makeError<String>(err);
                String text;
                char   buf[256];
                for (;;) {
                        auto [n, rerr] = file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                    WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                        if (rerr.isError()) return makeError<String>(rerr);
                        const Error aerr = text.append(StringView(buf, n));
                        if (aerr.isError()) return makeError<String>(aerr);
                }
                return makeResult(std::move(text));
        }

        // Left by recovery with a result, taken by the main firmware once it has logged it.
        constexpr const char *UnseenPath = "/flash/update/unseen";

        // The intent: the package's path, its head's hash, then "bootloader" when the user confirmed
        // writing one (a recovery from before §20 reads only the first two lines).
        Error writeIntent(const Job &job) {
                char headHex[65];
                hex(job.report->headHash, 32, headHex);
                String text;
                Error  err = text.appendf("%.*s\n%s\n%s", int(job.path.size()), job.path.data(), headHex,
                                         job.bootloader ? "bootloader\n" : "");
                if (err.isOk()) err = writeFile(Update::IntentPath, text.view());
                if (err.isOk()) err = Vfs::sync();
                return err;
        }

        // A result's "when: " line, now: none while the clock isn't set. Recovery's never is: the main
        // firmware's clock record is in RTC memory where each image's linker put it, so recovery
        // can't read it, and an unset clock counts from 1955. The main firmware then adds the line
        // when it takes the result (takeUnseen()), seconds later.
        Error appendWhen(String &text) {
                if (!Clock::isSet()) return Error::Ok;
                const DateTime t = DateTime::fromSeconds(Clock::now().seconds());
                return text.appendf("when: %04ld-%02u-%02u %02u:%02u:%02u UTC\n", static_cast<long>(t.year), t.month,
                                    t.day, t.hour, t.minute, t.second);
        }

        void writeResultOf(StringView path, const char *version, const char *message, StringView note = {}) {
                String text;
                (void)text.appendf("status: %s\npackage: %.*s\nversion: %s\n",
                                   message == nullptr ? "applied" : "failed", int(path.size()), path.data(), version);
                (void)appendWhen(text);
                if (message != nullptr) (void)text.appendf("message: %s\n", message);
                if (!note.isEmpty()) (void)text.append(note);
                if (writeFile(Update::ResultPath, text.view()).isError() || writeFile(UnseenPath, "").isError()) {
                        bmdsErr("Update: can't write the result");
                }
                return;
        }

        void writeResult(const Job &job, const char *message) {
                writeResultOf(job.path, job.report->package.versionText, message, job.resultNote.view());
                return;
        }

        // The main firmware's part: the recovery firmware if it differs, then the intent.
        Error applyMain(Job &job) {
                Update::Report &r = *job.report;
                size_t          index = 0;
                const bool newBootloader = job.bootloader && differing(r, SectionType::Bootloader, index) != nullptr;
                // The same version is fine when the bootloader is what it brings: the second step for a
                // board that took this package before it could confirm one (devplan/update.md §20).
                const bool sameButBootloader = r.order == 0 && newBootloader;
                if (r.order <= 0 && !job.force && !sameButBootloader) {
                        r.problem = r.order < 0 ? "it is older than the firmware installed (--force applies it)"
                                    : differing(r, SectionType::Bootloader, index) != nullptr
                                            ? "it is the version installed, but for its bootloader (--bootloader "
                                              "writes that)"
                                            : "it is the version installed (--force applies it)";
                        return Error::InvalidState;
                }
                if (differing(r, SectionType::Recovery, index) != nullptr) {
                        const Error err = writeSection(job, index, Update::Stage::WritingRecovery);
                        if (err.isError()) {
                                r.problem = "writing the recovery firmware failed";
                                return err;
                        }
                }
                if (differing(r, SectionType::Firmware, index) == nullptr &&
                    differing(r, SectionType::Rom, index) == nullptr && !newBootloader) {
                        return Error::Exists;
                }
                Error err = writeIntent(job);
                if (err.isOk()) err = System::setNextFirmware(System::Firmware::Recovery);
                if (err.isError()) {
                        Vfs::Context context;
                        (void)Vfs::remove(Update::IntentPath, context);
                        r.problem = "the recovery firmware can't be started";
                }
                return err;
        }

        // Recovery's part: the firmware and the ROM, the result, and back to the main firmware if
        // it checks good (otherwise recovery stays, the intent kept, to try again).
        Error applyRecovery(Job &job, bool good) {
                Update::Report &r = *job.report;
                const char     *failure = good ? nullptr : r.problem;
                if (failure == nullptr && job.expectHead != nullptr &&
                    std::memcmp(job.expectHead, r.headHash, 32) != 0) {
                        failure = "the package changed since it was chosen";
                }
                const Update::Observer *o = observer.load(std::memory_order_acquire);
                if (failure == nullptr && o != nullptr && o->started != nullptr) o->started(o->arg, r);
                size_t index = 0;
                // The ROM first: its tree is for the new firmware, so if the firmware doesn't make it
                // the old one passes it over for the tree before (rom.md §12f).
                if (failure == nullptr && differing(r, SectionType::Rom, index) != nullptr) {
                        const Error err = writeRom(job, index);
                        if (err == Error::NoSpace) {
                                failure = "the ROM's new files don't fit beside the user's";
                        } else if (err == Error::Cancelled) {
                                failure = "the user's ROM files couldn't be backed up to the card: nothing changed";
                        } else if (err.isError()) {
                                failure = "writing the ROM failed";
                        }
                }
                if (failure == nullptr && differing(r, SectionType::Firmware, index) != nullptr &&
                    writeChecked(job, index, Update::Stage::WritingFirmware).isError()) {
                        failure = "writing the firmware failed";
                }
                // Both written: the ROM's holes (its old system files' places) filled.
                if (failure == nullptr && job.romMerged) compactRom(job);
                // Last, and only when confirmed. If it didn't write right (the old one written back,
                // or not even that), recovery stays: a restart could find no bootloader to start.
                bool bootloaderFailed = false;
                if (failure == nullptr && job.bootloader && differing(r, SectionType::Bootloader, index) != nullptr &&
                    writeBootloader(job, index).isError()) {
                        failure = "writing the bootloader failed (recovery stays: don't turn the board off)";
                        bootloaderFailed = true;
                }
                writeResult(job, failure);
                if (bootloaderFailed) {
                        r.problem = failure;
                        bmdsErr("Update: %s", failure);
                        Vfs::Context context;
                        (void)Vfs::remove(Update::IntentPath, context); // not again at a restart
                        (void)Vfs::sync();
                        if (o != nullptr && o->finished != nullptr) o->finished(o->arg, r, failure, false);
                        return Error::InvalidState;
                }
                const Error  next = System::setNextFirmware(System::Firmware::Main);
                Vfs::Context context;
                if (next.isOk()) (void)Vfs::remove(Update::IntentPath, context);
                (void)Vfs::sync();
                if (failure != nullptr) {
                        r.problem = failure;
                        bmdsErr("Update: %s%s", failure,
                                next.isOk() ? "" : "; the main firmware doesn't check, so recovery stays");
                } else {
                        bmdsInfo("Update: %s applied", r.package.versionText);
                }
                if (o != nullptr && o->finished != nullptr) o->finished(o->arg, r, failure, next.isOk());
                // Staying (InvalidState) must not read as a failure to restart from (CorruptData).
                if (next.isError()) return Error::InvalidState;
                return failure != nullptr ? Error::CorruptData : Error::Ok;
        }

        void runJob(void *arg) {
                Job &job = *static_cast<Job *>(arg);
                if (job.mode == Mode::CheckHead) {
                        *job.report = Update::Report{};
                        auto [good, err] = checkHead(job, Span<const uint8_t>(job.head, job.headBytes), job.fileBytes);
                        job.result = err.isError() ? err : good ? Error(Error::Ok) : Error(Error::CorruptData);
                        return;
                }
                auto [good, err] = checkPackage(job);
                if (job.mode == Mode::ApplyRecovery && job.expectHead == nullptr && err.isOk() && good) {
                        // `update apply` in recovery: the intent first, so a reset resumes it.
                        job.result = writeIntent(job);
                        if (job.result.isOk()) job.result = applyRecovery(job, true);
                } else if (job.mode == Mode::ApplyRecovery && job.expectHead != nullptr) {
                        if (err.isError() && job.report->problem == nullptr) {
                                job.report->problem = err == Error::NotExist || err == Error::NoMedia
                                                              ? "the package isn't there (is the card in?)"
                                                              : "the package can't be read";
                        }
                        job.result = applyRecovery(job, err.isOk() && good);
                } else if (err.isError()) {
                        job.result = err;
                } else if (!good) {
                        job.result = Error::CorruptData;
                } else if (job.mode == Mode::ApplyMain) {
                        job.result = applyMain(job);
                } else {
                        job.result = Error::Ok;
                }
                job.file.reset();
                return;
        }

        Error run(Job &job) {
                if (busy.exchange(true)) return Error::Busy;
                job.chunk = static_cast<uint8_t *>(Memory::alloc(MemSpace::Internal, ChunkBytes));
                Error err = job.chunk != nullptr ? Thread::runOnInternalStack("update", JobStackBytes, runJob, &job)
                                                 : Error(Error::NoMem);
                if (err.isOk()) err = job.result;
                Memory::free(job.chunk);
                busy.store(false);
                return err;
        }

        Job *newJob() {
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(Job));
                return memory != nullptr ? new (memory) Job : nullptr;
        }

        void deleteJob(Job *job) {
                job->~Job();
                Memory::free(job);
                return;
        }

        // Recovery's boot: the intent applied once the card is there, then the main firmware.
        constinit Thread intentThread;

        // `rom repack`'s request (rom.md §12s): the ROM packed tight, here where nothing is mapped.
        bool repackRequested() {
                Vfs::Context context;
                return Vfs::stat(RomDisk::RepackPath, context).isOk();
        }

        void repackProgress(void *, uint64_t done, uint64_t total) {
                static uint64_t last = 0;
                if (total < 256 * 1024 || done == last) return;
                if (done * 4 / total != last * 4 / total)
                        bmdsInfo("ROM: repacking: %u KB of a round's %u KB", unsigned(done / 1024),
                                 unsigned(total / 1024));
                last = done;
                return;
        }

        // Runs the repack on an internal stack (flash writes), then removes the request: done or
        // not, it isn't tried again by itself (a reset in the middle tries again: the request stays).
        void repackRom() {
                struct RepackJob {
                                RomEditor::Report report;
                                Error             err;
                } job;
                const Error ran = Thread::runOnInternalStack(
                        "repack", JobStackBytes,
                        [](void *arg) {
                                auto        &j = *static_cast<RepackJob *>(arg);
                                PartitionRom rom(partitionFor(uint32_t(SectionType::Rom)));
                                RomEditor    editor(rom, nullptr, {});
                                editor.setTime(Clock::now());
                                editor.setProgress(repackProgress, nullptr);
                                j.err = rom.isMapped() ? editor.load() : Error(Error::NoMedia);
                                if (j.err.isOk()) j.err = editor.repack(j.report);
                                return;
                        },
                        &job);
                const Error err = ran.isError() ? ran : job.err;
                if (err.isError()) {
                        bmdsErr("ROM: repacking failed: %s (the ROM is whole: the tree before, or a round of it)",
                                err.desc());
                } else {
                        bmdsInfo("ROM: repacked: %u files moved (%u KB), tree version %u",
                                 static_cast<unsigned>(job.report.moved),
                                 static_cast<unsigned>(job.report.bytesWritten / 1024),
                                 static_cast<unsigned>(job.report.sequence));
                }
                Vfs::Context context;
                (void)Vfs::remove(RomDisk::RepackPath, context);
                (void)Vfs::sync();
                return;
        }

        // Recovery started for a repack alone: do it, then the main firmware.
        void repackMain(void *) {
                repackRom();
                if (System::setNextFirmware(System::Firmware::Main).isOk()) {
                        bmdsInfo("ROM: restarting");
                        Thread::sleep(RestartPauseMs);
                        System::restart();
                }
                return;
        }

        void intentMain(void *) {
                auto [text, err] = readFile(Update::IntentPath);
                const StringView t = text.view();
                const size_t     nl = t.indexOf('\n');
                uint8_t          expect[32];
                bool             parsed = err.isOk() && nl != StringView::NPos && t.size() >= nl + 1 + 64;
                for (size_t i = 0; parsed && i < 32; ++i) {
                        unsigned byte = 0;
                        parsed = std::sscanf(t.data() + nl + 1 + 2 * i, "%2x", &byte) == 1;
                        expect[i] = uint8_t(byte);
                }
                // The confirmation for a bootloader (§20), on the line after the hash.
                const bool bootloader = parsed && t.mid(nl + 1 + 64).startsWith("\nbootloader");
                if (!parsed) {
                        bmdsErr("Update: the intent on /flash can't be read (%s, %u bytes): removed",
                                err.isError() ? err.desc() : "not a path and a hash", static_cast<unsigned>(t.size()));
                        Vfs::Context context;
                        (void)Vfs::remove(Update::IntentPath, context);
                        return;
                }
                (void)Storage::waitForFirstMount(WaitForever);
                auto [path, perr] = String::fromView(t.left(nl));
                Job  *job = newJob();
                auto *r = static_cast<Update::Report *>(Memory::alloc(MemSpace::Psram, sizeof(Update::Report)));
                if (job == nullptr || r == nullptr || perr.isError()) {
                        bmdsErr("Update: out of memory");
                        return;
                }
                new (r) Update::Report;
                bmdsInfo("Update: applying %.*s", int(path.view().size()), path.view().data());
                job->mode = Mode::ApplyRecovery;
                job->path = path.view();
                job->expectHead = expect;
                job->bootloader = bootloader;
                job->report = r;
                job->progress = [](void *, Update::Stage stage, uint64_t done, uint64_t total) {
                        static Update::Stage last = Update::Stage::Checking;
                        static unsigned      step = 0;
                        const unsigned       now = total != 0 ? unsigned(done * 4 / total) : 0;
                        if (stage != last || now != step) {
                                if (now != 4) bmdsInfo("Update: %s, %u%%", Update::stageText(stage), now * 25);
                                last = stage;
                                step = now;
                        }
                        return;
                };
                const Error done = run(*job);
                deleteJob(job);
                Memory::free(r);
                // A repack asked for too: after the update, before the restart.
                if ((done.isOk() || done == Error::CorruptData) && repackRequested()) repackRom();
                // Back to the main firmware when that was set (Ok, or CorruptData: failed, but the
                // main firmware checks); otherwise (InvalidState) recovery stays, its shell open.
                if (done.isOk() || done == Error::CorruptData) {
                        bmdsInfo("Update: restarting");
                        Thread::sleep(RestartPauseMs); // the log to the console, the result on the screen
                        System::restart();
                }
                return;
        }

        // The notification of an update just applied (or not), once at boot: the Updater shows the rest.
        void notifyResult(bool applied, StringView version, StringView message) {
                Notification n;
                n.urgency = applied ? Notification::Urgency::Normal : Notification::Urgency::Critical;
                n.target = Notification::Target::Launch;
                std::snprintf(n.targetText, sizeof(n.targetText), "updater.desktop");
                std::snprintf(n.app, sizeof(n.app), "Updater");
                std::snprintf(n.icon, sizeof(n.icon), "system-software-update");
                n.timeoutMs = ResultBalloonMs; // longer than most: the screen may only just have come on
                if (applied) {
                        std::snprintf(n.title, sizeof(n.title), "Update installed");
                        std::snprintf(n.body, sizeof(n.body), "BMDS %.*s is installed.", int(version.size()),
                                      version.data());
                } else {
                        std::snprintf(n.title, sizeof(n.title), "The update wasn't installed");
                        std::snprintf(n.body, sizeof(n.body), "%.*s. The system is as it was.", int(message.size()),
                                      message.data());
                }
                if (Notifications::post(n, 0, nullptr).isError()) bmdsWarn("Update: can't post the notification");
                return;
        }

} // namespace

void Update::setObserver(const Observer *o) {
        observer.store(o, std::memory_order_release);
        return;
}

const char *Update::stageText(Stage stage) {
        switch (stage) {
                case Stage::Checking: return "Checking";
                case Stage::WritingRecovery: return "Writing the recovery system";
                case Stage::WritingFirmware: return "Writing the system";
                case Stage::WritingRom: return "Writing the ROM";
                case Stage::WritingBootloader: return "Writing the bootloader (keep the power on)";
        }
        return "?";
}

Error Update::check(StringView path, Report &out, Progress progress, void *arg) {
        Job *job = newJob();
        if (job == nullptr) return Error::NoMem;
        job->mode = Mode::Check;
        job->path = path;
        job->report = &out;
        job->progress = progress;
        job->arg = arg;
        const Error err = run(*job);
        deleteJob(job);
        return err;
}

Error Update::checkHead(Span<const uint8_t> head, uint64_t fileBytes, Report &out) {
        if (head.size() > UpdatePackage::MaxHeadBytes) head = head.first(UpdatePackage::MaxHeadBytes);
        Job *job = newJob();
        if (job == nullptr) return Error::NoMem;
        job->mode = Mode::CheckHead;
        std::memcpy(job->head, head.data(), head.size());
        job->headBytes = head.size();
        job->fileBytes = fileBytes;
        job->report = &out;
        const Error err = run(*job);
        deleteJob(job);
        return err;
}

bool Update::Report::hasNewBootloader() const {
        for (size_t i = 0; i < package.sectionCount; ++i) {
                if (package.sections[i].type == uint32_t(SectionType::Bootloader) && flashed[i] == Flashed::Differs)
                        return true;
        }
        return false;
}

Error Update::apply(StringView path, bool force, bool bootloader, Report &out, Progress progress, void *arg) {
        Job *job = newJob();
        if (job == nullptr) return Error::NoMem;
        const bool recovery = System::running() == System::Firmware::Recovery;
        job->mode = recovery ? Mode::ApplyRecovery : Mode::ApplyMain;
        job->path = path;
        job->force = force;
        job->bootloader = bootloader;
        job->report = &out;
        job->progress = progress;
        job->arg = arg;
        Error err = run(*job);
        deleteJob(job);
        return err;
}

Error Update::status(Status &out) {
        esp_app_desc_t         desc;
        const esp_partition_t *main = partitionFor(uint32_t(SectionType::Firmware));
        const esp_partition_t *recovery = partitionFor(uint32_t(SectionType::Recovery));
        // The descriptions are read with the caches off: not on a PSRAM stack.
        struct Read {
                        const esp_partition_t *partition;
                        esp_app_desc_t        *desc;
                        char                  *out;
        };
        for (Read r : {Read{main, &desc, out.mainVersion}, Read{recovery, &desc, out.recoveryVersion}}) {
                if (r.partition == nullptr) continue;
                (void)Thread::runOnInternalStack(
                        "otadesc", 4096,
                        [](void *arg) {
                                Read *x = static_cast<Read *>(arg);
                                if (esp_ota_get_partition_description(x->partition, x->desc) == ESP_OK)
                                        std::snprintf(x->out, 33, "%s", x->desc->version);
                                return;
                        },
                        &r);
        }
        auto [intent, ierr] = readFile(IntentPath);
        if (ierr.isOk()) {
                const size_t nl = intent.view().indexOf('\n');
                const Error  err = out.pending.assign(intent.view().left(nl == StringView::NPos ? intent.size() : nl));
                if (err.isError()) return err;
        } else if (ierr != Error::NotExist) {
                return ierr;
        }
        auto [result, rerr] = readFile(ResultPath);
        if (rerr.isOk()) out.result = std::move(result);
        return rerr.isOk() || rerr == Error::NotExist ? Error::Ok : rerr;
}

namespace {

        // The crashes in a row (devplan/update.md §17), in RTC memory: kept over resets, lost at power
        // off. Recovery's own RTC memory may lie over it: a count that doesn't check starts at 0.
        struct RestartCount {
                        uint32_t magic;
                        uint32_t count;
                        uint32_t check; // ~count
        };

        constexpr uint32_t RestartMagic = 0x424d5243; // "BMRC"
        // How long the main firmware runs after its boot before its crashes stop counting as a loop.
        constexpr uint32_t SettledMs = 60000;
        // Esc held as a keyboard is attached this soon after reset starts recovery.
        constexpr int64_t RecoveryKeyUs = 10'000'000;
        // Left by the main firmware for recovery when it kept restarting.
        constexpr const char *RestartsPath = "/flash/update/restarts";

        RTC_NOINIT_ATTR RestartCount restartCount;

        constinit StaticTimer_t settledBuffer;
        constinit TimerHandle_t settledTimer = nullptr;

        void setRestartCount(uint32_t count) {
                restartCount.magic = RestartMagic;
                restartCount.count = count;
                restartCount.check = ~count;
                return;
        }

        bool crashed(esp_reset_reason_t reason) {
                switch (reason) {
                        case ESP_RST_PANIC:
                        case ESP_RST_INT_WDT:
                        case ESP_RST_TASK_WDT:
                        case ESP_RST_WDT:
                        case ESP_RST_CPU_LOCKUP: return true;
                        default: return false;
                }
        }

        void settled(TimerHandle_t) {
                setRestartCount(0);
                return;
        }

        // Recovery next, and the restart; returns only when recovery can't be started.
        Error startRecovery() {
                const Error err = System::setNextFirmware(System::Firmware::Recovery);
                if (err.isError()) return err;
                setRestartCount(0);
                (void)Vfs::sync();
                bmdsInfo("Update: restarting into recovery");
                System::restart();
        }

        void attachKey(uint8_t usage) {
                if (usage != HidKey::Escape || System::uptimeUs() >= RecoveryKeyUs) return;
                bmdsInfo("Update: Esc held at power on: starting recovery");
                const Error err = startRecovery();
                bmdsErr("Update: can't start recovery: %s", err.desc());
                return;
        }

} // namespace

Error Update::watch() {
        if (System::running() != System::Firmware::Main) return Error::Ok;
        const esp_reset_reason_t reason = esp_reset_reason();
        const bool     valid = restartCount.magic == RestartMagic && restartCount.check == ~restartCount.count;
        const uint32_t count = !crashed(reason) ? 0 : valid ? restartCount.count + 1 : 1;
        setRestartCount(count);
        Input::setAttachKeyHandler(attachKey);
        if (count == 0) return Error::Ok;
        bmdsWarn("Update: crash %u of %u in a row", unsigned(count), unsigned(RestartLimit));
        if (count < RestartLimit) return Error::Ok;
        bmdsErr("Update: the system kept restarting: starting recovery");
        Error err = writeFile(RestartsPath, "");
        if (err.isOk()) err = startRecovery();
        bmdsErr("Update: can't start recovery: %s", err.desc());
        return err;
}

Error Update::confirm() {
        // Crashes from a minute on aren't a loop: the count is cleared then.
        if (System::running() == System::Firmware::Main && settledTimer == nullptr) {
                settledTimer = xTimerCreateStatic("settled", pdMS_TO_TICKS(SettledMs), pdFALSE, nullptr, settled,
                                                  &settledBuffer);
                if (settledTimer != nullptr) (void)xTimerStart(settledTimer, 0);
        }
        // A no-op unless this firmware was just switched to (on probation).
        return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK ? Error::Ok : Error::IoError;
}

Error Update::init() {
        Vfs::Context context;
        const bool   pending = Vfs::stat(IntentPath, context).isOk();
        if (System::running() == System::Firmware::Recovery) {
                // Chosen (switched to: on probation), or run because the main firmware didn't start
                // (the bootloader fell back: its own entry was already valid, or there is none).
                esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
                const bool chosen = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                                    state == ESP_OTA_IMG_PENDING_VERIFY;
                (void)confirm();
                // Started by the main firmware because it kept restarting (watch()).
                const bool restarting = Vfs::stat(RestartsPath, context).isOk();
                if (restarting) {
                        (void)Vfs::remove(RestartsPath, context);
                        (void)Vfs::sync();
                }
                if ((!chosen || restarting) && !pending) {
                        const char *why = restarting ? Update::SystemKeptRestarting : Update::SystemDidntStart;
                        bmdsErr("Update: recovery runs because %s", why);
                        const Update::Observer *o = observer.load(std::memory_order_acquire);
                        if (o != nullptr && o->finished != nullptr) {
                                static constinit const Update::Report none{}; // no constructor runs
                                o->finished(o->arg, none, why, false);
                        }
                }
                if (!pending && repackRequested()) {
                        Thread::Config config;
                        config.name = "repack";
                        config.stackBytes = 6144;
                        config.kernel = true;
                        return intentThread.start(config, repackMain, nullptr);
                }
                if (!pending) return Error::Ok;
                Thread::Config config;
                config.name = "update";
                config.stackBytes = 6144;
                config.kernel = true;
                return intentThread.start(config, intentMain, nullptr);
        }
        // The main firmware with an intent still there: recovery never ran it (the new recovery firmware
        // didn't start, and the bootloader came back here). Reported as a failed update, and dropped.
        if (pending) {
                auto [intent, ierr] = readFile(IntentPath);
                const size_t nl = ierr.isOk() ? intent.view().indexOf('\n') : StringView::NPos;
                bmdsErr("Update: recovery didn't run the update: its firmware didn't start");
                writeResultOf(nl != StringView::NPos ? intent.view().left(nl) : StringView(), "",
                              "the recovery firmware it brought didn't start");
                (void)Vfs::remove(IntentPath, context);
                (void)Vfs::sync();
        }
        if (Vfs::stat(UnseenPath, context).isOk()) {
                auto [result, err] = readFile(ResultPath);
                StringView rest = result.view();
                StringView status, version, message;
                while (err.isOk() && !rest.isEmpty()) {
                        const size_t     nl = rest.indexOf('\n');
                        const StringView line = rest.left(nl);
                        bmdsInfo("Update: %.*s", int(line.size()), line.data());
                        if (line.startsWith("status: ")) status = line.mid(8);
                        if (line.startsWith("version: ")) version = line.mid(9);
                        if (line.startsWith("message: ")) message = line.mid(9);
                        rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                }
                if (err.isOk()) notifyResult(status == "applied", version, message);
                // Left by recovery without the time (appendWhen()): this firmware's clock says it.
                const bool timed =
                        result.view().startsWith("when: ") || result.view().indexOf("\nwhen: ") != StringView::NPos;
                if (err.isOk() && !timed && Clock::isSet() && appendWhen(result).isOk())
                        (void)writeFile(ResultPath, result.view());
                (void)Vfs::remove(UnseenPath, context);
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
