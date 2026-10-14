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

#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_chip_info.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <sdkconfig.h>
#include <bmds/blake2s.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/log.h>
#include <bmds/memory.h>
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
        constexpr uint8_t  AppMagic = 0xE9;
        // How long recovery shows how an update ended before it restarts.
        constexpr unsigned RestartPauseMs = 2000;

        std::atomic<bool> busy{false};

        std::atomic<const Update::Observer *> observer{nullptr};

        // Where packages are kept: what tools/bmds-update copies to, and (step 4) what the
        // Updater lists.
        BMDS_SYSCONF(updateDir, "update/dir", Config::Type::Path, "/sd0/p0/update",
                     "Where update packages (.bmup) are kept: tools/bmds-update copies them here", 0, 255, nullptr,
                     nullptr)

        enum class Mode : uint8_t {
                Check,
                ApplyMain,
                ApplyRecovery
        };

        struct Job {
                        Mode             mode = Mode::Check;
                        StringView       path;
                        bool             force = false;
                        const uint8_t   *expectHead = nullptr; // the intent's head hash, or null
                        Update::Report  *report = nullptr;
                        Update::Progress progress = nullptr;
                        void            *arg = nullptr;
                        FileHandle::Ptr  file;
                        uint8_t         *chunk = nullptr;
                        uint8_t          head[UpdatePackage::MaxHeadBytes];
                        Error            result;
        };

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
                        default: return nullptr;
                }
        }

        const char *sectionName(uint32_t type) {
                switch (static_cast<SectionType>(type)) {
                        case SectionType::Firmware: return "the firmware";
                        case SectionType::Recovery: return "the recovery firmware";
                        case SectionType::Rom: return "the ROM";
                        case SectionType::Notes: return "the release notes";
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
                const Span<const uint8_t> head(job.head, headRead);
                UpdatePackage::Verdict    v = UpdatePackage::parse(head, fileBytes, r.package);
                if (v != UpdatePackage::Verdict::Ok) return refuse(job, UpdatePackage::verdictText(v));
                const Span<const UpdatePackage::Key> keys = UpdateKeys::accepted();
                v = UpdatePackage::Verdict::BadSignature;
                for (size_t i = 0; i < keys.size() && v != UpdatePackage::Verdict::Ok; ++i) {
                        v = UpdatePackage::verify(r.package, head, keys.mid(i, 1));
                        if (v == UpdatePackage::Verdict::Ok) r.devKey = UpdateKeys::isDevKey(i);
                }
                if (v != UpdatePackage::Verdict::Ok) {
                        return refuse(job, UpdateKeys::isDevMode() ? "it isn't signed by a key this firmware accepts"
                                                                   : "it isn't signed by a release key");
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
                                                s.type == uint32_t(SectionType::Recovery))) {
                                        const uint16_t chipId = uint16_t(job.chunk[12] | (job.chunk[13] << 8));
                                        if (n < 24 || job.chunk[0] != AppMagic ||
                                            chipId != CONFIG_IDF_FIRMWARE_CHIP_ID) {
                                                return refuse(job, "an app image isn't one for this chip");
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
                const Error  made = Vfs::mkdir("/boot/update", context);
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
        constexpr const char *UnseenPath = "/boot/update/unseen";

        Error writeIntent(const Job &job) {
                char headHex[65];
                hex(job.report->headHash, 32, headHex);
                String text;
                Error  err = text.appendf("%.*s\n%s\n", int(job.path.size()), job.path.data(), headHex);
                if (err.isOk()) err = writeFile(Update::IntentPath, text.view());
                if (err.isOk()) err = Vfs::sync();
                return err;
        }

        void writeResult(const Job &job, const char *message) {
                const Update::Report &r = *job.report;
                const DateTime        t = DateTime::fromSeconds(Clock::now().seconds());
                String                text;
                (void)text.appendf("status: %s\npackage: %.*s\nversion: %s\nwhen: %04ld-%02u-%02u %02u:%02u:%02u UTC\n",
                                   message == nullptr ? "applied" : "failed", int(job.path.size()), job.path.data(),
                                   r.package.versionText, static_cast<long>(t.year), t.month, t.day, t.hour, t.minute,
                                   t.second);
                if (message != nullptr) (void)text.appendf("message: %s\n", message);
                if (writeFile(Update::ResultPath, text.view()).isError() || writeFile(UnseenPath, "").isError()) {
                        bmdsErr("Update: can't write the result");
                }
                return;
        }

        // The main firmware's part: the recovery firmware if it differs, then the intent.
        Error applyMain(Job &job) {
                Update::Report &r = *job.report;
                if (r.order <= 0 && !job.force) {
                        r.problem = r.order < 0 ? "it is older than the firmware installed (--force applies it)"
                                                : "it is the version installed (--force applies it)";
                        return Error::InvalidState;
                }
                size_t index = 0;
                if (differing(r, SectionType::Recovery, index) != nullptr) {
                        const Error err = writeSection(job, index, Update::Stage::WritingRecovery);
                        if (err.isError()) {
                                r.problem = "writing the recovery firmware failed";
                                return err;
                        }
                }
                if (differing(r, SectionType::Firmware, index) == nullptr &&
                    differing(r, SectionType::Rom, index) == nullptr) {
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
                if (failure == nullptr && differing(r, SectionType::Firmware, index) != nullptr &&
                    writeChecked(job, index, Update::Stage::WritingFirmware).isError()) {
                        failure = "writing the firmware failed";
                }
                if (failure == nullptr && differing(r, SectionType::Rom, index) != nullptr &&
                    writeChecked(job, index, Update::Stage::WritingRom).isError()) {
                        failure = "writing the ROM failed";
                }
                writeResult(job, failure);
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
                if (!parsed) {
                        bmdsErr("Update: the intent on /boot can't be read (%s, %u bytes): removed",
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
                // Back to the main firmware when that was set (Ok, or CorruptData: failed, but the
                // main firmware checks); otherwise (InvalidState) recovery stays, its shell open.
                if (done.isOk() || done == Error::CorruptData) {
                        bmdsInfo("Update: restarting");
                        Thread::sleep(RestartPauseMs); // the log to the console, the result on the screen
                        System::restart();
                }
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

Error Update::apply(StringView path, bool force, Report &out, Progress progress, void *arg) {
        Job *job = newJob();
        if (job == nullptr) return Error::NoMem;
        const bool recovery = System::running() == System::Firmware::Recovery;
        job->mode = recovery ? Mode::ApplyRecovery : Mode::ApplyMain;
        job->path = path;
        job->force = force;
        job->report = &out;
        job->progress = progress;
        job->arg = arg;
        Error err = run(*job);
        deleteJob(job);
        return err;
}

Error Update::status(Status &out) {
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

Error Update::init() {
        Vfs::Context context;
        const bool   pending = Vfs::stat(IntentPath, context).isOk();
        if (System::running() == System::Firmware::Recovery) {
                if (!pending) return Error::Ok;
                Thread::Config config;
                config.name = "update";
                config.stackBytes = 6144;
                config.kernel = true;
                return intentThread.start(config, intentMain, nullptr);
        }
        if (pending) bmdsWarn("Update: an update is waiting to be applied by recovery ('update status')");
        if (Vfs::stat(UnseenPath, context).isOk()) {
                auto [result, err] = readFile(ResultPath);
                StringView rest = result.view();
                while (err.isOk() && !rest.isEmpty()) {
                        const size_t     nl = rest.indexOf('\n');
                        const StringView line = rest.left(nl);
                        bmdsInfo("Update: %.*s", int(line.size()), line.data());
                        rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                }
                (void)Vfs::remove(UnseenPath, context);
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
