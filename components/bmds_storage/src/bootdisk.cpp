/**
 * @file      bootdisk.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * /boot: FAT on the flash partition "boot" through ESP-IDF's wear levelling
 * (devplan/storage.md §13).
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <sdkconfig.h>
#include <esp_memory_utils.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <wear_levelling.h>
#include <bmds/blockdevice.h>
#include <bmds/blockdevices.h>
#include <bmds/bootdisk.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/random.h>
#include <bmds/randompool.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/string.h>
#include <bmds/system.h>
#include <bmds/task.h>
#include <bmds/vfs.h>

#include "bootdefaults.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *PartitionLabel = "boot";

        // A flash partition through wear levelling, as whole sectors (512 bytes: FatFs's).
        //
        // Writes go into a write-back cache of CacheBlocks 4 KB flash blocks (devplan/storage.md
        // §13a): wear levelling rewrites a whole flash sector (twice, in its safe mode) for each
        // 512-byte write, so the sectors of a block written together (a file's data; the FAT and
        // the directory, rewritten again and again) cost one erase and one write when the block is
        // written out. The worker writes the dirty blocks in the background: SyncDelayMs after a
        // sync() (FatFs's, at each close, which doesn't wait for it), IdleFlushMs after a write
        // without one, or at once when a block is needed and all are dirty. A restart writes them
        // all first (a shutdown handler).
        //
        // A flash operation turns off the cache PSRAM is reached through, so it must run on a stack
        // in internal RAM (IDF asserts it): writes and syncs always run on the device's worker task
        // (whose stack is internal: it keeps the deadline too); reads run there when the caller's
        // stack is in PSRAM (the shells, programs).
        class FlashBlockDevice final : public BlockDevice {
                public:
                        static constexpr uint32_t     BlockBytes = 4096; // a flash sector
                        static constexpr size_t       CacheBlocks = 8;
                        static constexpr unsigned int SyncDelayMs = 100;
                        static constexpr unsigned int IdleFlushMs = 1000;
                        static constexpr unsigned int BusyRetryMs = 10; // the lock taken: try again

                        FlashBlockDevice() : _request(1, 0), _done(1, 0) {}

                        // Mounts wear levelling on the partition labelled @p label; starts the worker.
                        Error open(const char *label) {
                                const esp_partition_t *partition = esp_partition_find_first(
                                        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, label);
                                if (partition == nullptr) return Error::NotExist;
                                if (wl_mount(partition, &_handle) != ESP_OK) return Error::IoError;
                                _sectorSize = static_cast<uint32_t>(wl_sector_size(_handle));
                                _sectors = wl_size(_handle) / _sectorSize;
                                _blocks = static_cast<uint8_t *>(
                                        Memory::alloc(MemSpace::Psram, size_t(BlockBytes) * CacheBlocks));
                                if (_blocks == nullptr) return Error::NoMem;
                                Task::Config config;
                                config.name = "flash0";
                                config.stackBytes = 3072;
                                config.stackSpace = MemSpace::Internal;
                                config.priority = CONFIG_BMDS_STORAGE_PRIORITY;
                                return _worker.start(config, workerMain, this);
                        }

                        uint32_t sectorSize() const override { return _sectorSize; }
                        uint32_t eraseBlockSectors() const override { return BlockBytes / _sectorSize; }
                        uint64_t sectorCount() const override { return _sectors; }

                        // The dirty blocks written soon, not waited for.
                        Error sync() override { return run(Op::Sync, 0, nullptr, 0); }

                        // Every dirty block written now (a restart's).
                        Error flushAll() { return run(Op::Flush, 0, nullptr, 0); }

                protected:
                        Error readSectors(uint64_t sector, WritableByteSpan buf) override {
                                return run(Op::Read, size_t(sector) * _sectorSize, buf.data(), buf.size());
                        }

                        Error writeSectors(uint64_t sector, ByteSpan data) override {
                                return run(Op::Write, size_t(sector) * _sectorSize, const_cast<uint8_t *>(data.data()),
                                           data.size());
                        }

                private:
                        enum class Op : uint8_t {
                                Read,
                                Write,
                                Sync,
                                Flush
                        };

                        static constexpr size_t   NoBlock = SIZE_MAX;
                        static constexpr uint64_t NoDeadline = UINT64_MAX;

                        struct Slot {
                                        size_t   block = NoBlock;
                                        uint32_t used = 0; // the clock when last used
                                        bool     dirty = false;
                        };

                        // One operation: a read on this task if its stack is internal, the rest on the worker.
                        Error run(Op op, size_t at, uint8_t *data, size_t bytes) {
                                Mutex::Locker lock(_lock, WaitForever); // also: wear levelling isn't thread-safe
                                if (op == Op::Read && esp_ptr_internal(__builtin_frame_address(0)))
                                        return perform(op, at, data, bytes);
                                _op = op;
                                _at = at;
                                _data = data;
                                _bytes = bytes;
                                _request.release();
                                (void)_done.acquire(WaitForever);
                                return _result;
                        }

                        Error perform(Op op, size_t at, uint8_t *data, size_t bytes) {
                                switch (op) {
                                        case Op::Read: return read(at, data, bytes);
                                        case Op::Write: {
                                                const Error err = write(at, data, bytes);
                                                if (_deadline == NoDeadline) _deadline = later(IdleFlushMs);
                                                return err;
                                        }
                                        case Op::Sync:
                                                if (hasDirty() && _deadline > later(SyncDelayMs))
                                                        _deadline = later(SyncDelayMs);
                                                return Error::Ok;
                                        case Op::Flush: {
                                                Error err;
                                                for (Slot &s : _slots)
                                                        if (s.dirty && err.isOk()) err = writeOut(s);
                                                if (!hasDirty()) _deadline = NoDeadline;
                                                return err;
                                        }
                                }
                                return Error::Invalid;
                        }

                        static uint64_t later(unsigned int ms) { return System::uptimeUs() + uint64_t(ms) * 1000; }

                        bool hasDirty() const {
                                for (const Slot &s : _slots)
                                        if (s.dirty) return true;
                                return false;
                        }

                        uint8_t *bytesOf(const Slot &s) { return _blocks + size_t(&s - _slots) * BlockBytes; }

                        Slot *find(size_t block) {
                                for (Slot &s : _slots)
                                        if (s.block == block) return &s;
                                return nullptr;
                        }

                        // Cached blocks' sectors from the cache, the rest from the flash.
                        Error read(size_t at, uint8_t *data, size_t bytes) {
                                while (bytes > 0) {
                                        const size_t block = at / BlockBytes, offset = at % BlockBytes;
                                        const size_t n = bytes < BlockBytes - offset ? bytes : BlockBytes - offset;
                                        if (const Slot *s = find(block); s != nullptr) {
                                                std::memcpy(data, bytesOf(*s) + offset, n);
                                        } else if (wl_read(_handle, at, data, n) != ESP_OK) {
                                                return Error::IoError;
                                        }
                                        at += n;
                                        data += n;
                                        bytes -= n;
                                }
                                return Error::Ok;
                        }

                        // Into the cache: a block not there is read in first (unless written whole).
                        Error write(size_t at, const uint8_t *data, size_t bytes) {
                                while (bytes > 0) {
                                        const size_t block = at / BlockBytes, offset = at % BlockBytes;
                                        const size_t n = bytes < BlockBytes - offset ? bytes : BlockBytes - offset;
                                        Slot        *s = find(block);
                                        if (s == nullptr) {
                                                auto [free, err] = take();
                                                if (err.isError()) return err;
                                                s = free;
                                                if (n != BlockBytes && wl_read(_handle, block * BlockBytes, bytesOf(*s),
                                                                               BlockBytes) != ESP_OK)
                                                        return Error::IoError;
                                                s->block = block;
                                        }
                                        std::memcpy(bytesOf(*s) + offset, data, n);
                                        s->dirty = true;
                                        s->used = ++_clock;
                                        at += n;
                                        data += n;
                                        bytes -= n;
                                }
                                return Error::Ok;
                        }

                        // A slot for another block: a free one, else the least recently used clean one,
                        // else the least recently used, written out first.
                        Result<Slot *> take() {
                                Slot *best = nullptr;
                                for (Slot &s : _slots) {
                                        if (s.block == NoBlock) return makeResult(&s);
                                        if (best == nullptr || (best->dirty && !s.dirty) ||
                                            (best->dirty == s.dirty && s.used < best->used))
                                                best = &s;
                                }
                                if (best->dirty) {
                                        const Error err = writeOut(*best);
                                        if (err.isError()) return makeError<Slot *>(err);
                                }
                                best->block = NoBlock;
                                return makeResult(best);
                        }

                        // A dirty block to the flash: erased (one flash sector, so no read-modify-write),
                        // then written.
                        Error writeOut(Slot &s) {
                                const size_t at = s.block * BlockBytes;
                                esp_err_t    err = wl_erase_range(_handle, at, BlockBytes);
                                if (err == ESP_OK) err = wl_write(_handle, at, bytesOf(s), BlockBytes);
                                if (err != ESP_OK) return Error::IoError;
                                s.dirty = false;
                                return Error::Ok;
                        }

                        // The oldest dirty block written out (the lock held); false if none was.
                        bool writeOne() {
                                Slot *oldest = nullptr;
                                for (Slot &s : _slots)
                                        if (s.dirty && (oldest == nullptr || s.used < oldest->used)) oldest = &s;
                                if (oldest == nullptr) return false;
                                if (writeOut(*oldest).isError()) {
                                        bmdsErr("flash0: a block couldn't be written: kept, tried again");
                                        _deadline = later(IdleFlushMs);
                                        return false;
                                }
                                return true;
                        }

                        // Requests, and the dirty blocks written when due, a block at a time.
                        static void workerMain(void *arg) {
                                auto *self = static_cast<FlashBlockDevice *>(arg);
                                for (;;) {
                                        unsigned int wait = WaitForever;
                                        if (self->_deadline != NoDeadline) {
                                                const uint64_t now = System::uptimeUs();
                                                const uint64_t left = self->_deadline > now ? self->_deadline - now : 0;
                                                wait = static_cast<unsigned int>(left / 1000) + 1; // never 0 (forever)
                                        }
                                        // The requester holds the lock, so a request is performed without it.
                                        if (self->_request.acquire(wait).isOk()) {
                                                self->_result =
                                                        self->perform(self->_op, self->_at, self->_data, self->_bytes);
                                                self->_done.release();
                                                continue;
                                        }
                                        if (self->_deadline == NoDeadline || System::uptimeUs() < self->_deadline)
                                                continue;
                                        if (self->_lock.tryLock().isError()) {
                                                // A reader on its own task: the next request, or a little later.
                                                if (self->_request.acquire(BusyRetryMs).isOk()) {
                                                        self->_result = self->perform(self->_op, self->_at, self->_data,
                                                                                      self->_bytes);
                                                        self->_done.release();
                                                }
                                                continue;
                                        }
                                        if (!self->writeOne() && !self->hasDirty()) self->_deadline = NoDeadline;
                                        self->_lock.unlock();
                                }
                        }

                        Mutex       _lock; // one operation at a time, whoever runs it
                        Semaphore   _request;
                        Semaphore   _done;
                        Task        _worker;
                        wl_handle_t _handle = WL_INVALID_HANDLE;
                        uint32_t    _sectorSize = 512;
                        uint64_t    _sectors = 0;
                        uint8_t    *_blocks = nullptr; // CacheBlocks blocks (Psram)
                        Slot        _slots[CacheBlocks];
                        uint32_t    _clock = 0;
                        uint64_t    _deadline = NoDeadline; // when the dirty blocks are written (uptime, us)
                        Op          _op = Op::Read;         // the worker's request, under _lock
                        size_t      _at = 0;
                        uint8_t    *_data = nullptr;
                        size_t      _bytes = 0;
                        Error       _result;
        };

        constinit StaticObject<FlashBlockDevice> flash;

        // A restart (reboot, esp_restart()) writes the cached blocks first (devplan/storage.md §13a).
        void flushOnRestart() {
                if (flash.isCreated()) (void)flash->flushAll();
                return;
        }

        // The defaults installed (devplan/gui.md G14): each file of the repository's boot/ not
        // already on /boot and not listed in Manifest is written, and listed; so a new release's
        // defaults reach a /boot made before it, and a default deleted stays deleted.
        constexpr const char Manifest[] = "/boot/.defaults";
        constexpr size_t     MaxManifest = 4096;
        constexpr size_t     MaxPath = 128;

        Error writeFile(const char *path, ByteSpan bytes) {
                Vfs::Context context;
                auto [file, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Exclusive, context);
                if (err.isError()) return err;
                auto [n, werr] = file->write(bytes, WaitForever);
                if (werr.isOk() && n != bytes.size()) werr = Error::IoError;
                return werr;
        }

        // Every directory above @p path made (those there already are fine).
        void makeParents(const char *path) {
                char         dir[MaxPath];
                Vfs::Context context;
                for (const char *slash = std::strchr(path + 1, '/'); slash != nullptr;
                     slash = std::strchr(slash + 1, '/')) {
                        const size_t n = static_cast<size_t>(slash - path);
                        if (n >= sizeof(dir)) return;
                        std::memcpy(dir, path, n);
                        dir[n] = '\0';
                        (void)Vfs::mkdir(StringView(dir), context);
                }
                return;
        }

        bool listed(StringView manifest, StringView path) {
                while (!manifest.isEmpty()) {
                        const size_t     end = manifest.indexOf('\n');
                        const StringView line = end == StringView::NPos ? manifest : manifest.left(end);
                        if (line.trimmed() == path) return true;
                        manifest = end == StringView::NPos ? StringView() : manifest.mid(end + 1);
                }
                return false;
        }

        void installDefaults() {
                char *manifest = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxManifest));
                if (manifest == nullptr) return;
                size_t       got = 0;
                Vfs::Context context;
                {
                        auto [file, err] = Vfs::open(Manifest, Vfs::Read, context);
                        while (err.isOk() && got < MaxManifest) {
                                auto [n, rerr] =
                                        file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(manifest) + got,
                                                                    MaxManifest - got),
                                                   WaitForever);
                                if (rerr.isError() || n == 0) break;
                                got += n;
                        }
                }
                const StringView had(manifest, got);
                String           added;
                for (size_t i = 0; i < BootDefaults::count; ++i) {
                        const BootDefaults::File &f = BootDefaults::files[i];
                        if (listed(had, StringView(f.path))) continue;
                        char path[MaxPath];
                        std::snprintf(path, sizeof(path), "/boot/%s", f.path);
                        makeParents(path);
                        // Not there yet: written (a file the user made is kept, and listed).
                        const Error err = writeFile(path, ByteSpan(f.data, f.size));
                        if (err.isOk()) {
                                bmdsInfo("BootDisk: installed %s", path);
                        } else if (err != Error::Exists) {
                                bmdsWarn("BootDisk: %s: %s", path, err.desc());
                                continue;
                        }
                        if (added.append(StringView(f.path)).isError() || added.append('\n').isError()) break;
                }
                Memory::free(manifest);
                if (added.isEmpty()) return;
                auto [file, err] = Vfs::open(Manifest, Vfs::Write | Vfs::Create | Vfs::Append, context);
                if (err.isOk()) {
                        auto [n, werr] = file->write(added.view(), WaitForever);
                        err = werr.isOk() && n != added.size() ? Error(Error::IoError) : werr;
                }
                if (err.isError()) bmdsWarn("BootDisk: %s: %s", Manifest, err.desc());
                return;
        }

        // The random seed kept across boots (devplan/random.md).
        constexpr const char SeedPath[] = "/boot/random-seed";
        constexpr size_t     SeedBytes = 64;

        // A new seed from the pool over the old one.
        Error writeSeed() {
                uint8_t seed[SeedBytes];
                Random::fill(seed, sizeof(seed));
                Vfs::Context context;
                auto [file, err] = Vfs::open(SeedPath, Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                if (err.isOk()) {
                        auto [n, werr] = file->write(ByteSpan(seed, sizeof(seed)), WaitForever);
                        err = werr.isOk() && n != sizeof(seed) ? Error(Error::IoError) : werr;
                }
                RandomPool::wipe(seed, sizeof(seed));
                return err; // the file closes (and /boot syncs) with its handle
        }

        constinit bool seedable = false; // /boot is mounted (set at boot)

        // Mixes the last boot's seed in. A new one is written a few seconds
        // later (refreshRandomSeed()), so a crash soon after boot can reuse
        // it; the boot's own noise (Random::init()) is mixed in regardless.
        void seedRandom() {
                uint8_t      seed[SeedBytes];
                size_t       got = 0;
                Vfs::Context context;
                auto [file, err] = Vfs::open(SeedPath, Vfs::Read, context);
                while (err.isOk() && got < sizeof(seed)) {
                        auto [n, rerr] = file->read(WritableByteSpan(seed + got, sizeof(seed) - got), WaitForever);
                        if (rerr.isError()) break;
                        got += n;
                }
                file.reset();
                if (got != 0) Random::mix(seed, got);
                RandomPool::wipe(seed, sizeof(seed));
                seedable = true;
                bmdsInfo("Random: %s %s", got != 0 ? "seeded from" : "no seed yet in", SeedPath);
                return;
        }

        void saveSeedAtShutdown() {
                BootDisk::refreshRandomSeed();
                return;
        }

} // namespace

void BootDisk::refreshRandomSeed() {
        if (!seedable) return;
        const Error err = writeSeed();
        if (err.isError()) bmdsWarn("BootDisk: %s: %s", SeedPath, err.desc());
        return;
}

Error BootDisk::init() {
        Error err = Vfs::init();
        if (err.isError()) return err;
        FlashBlockDevice &device = flash.create();
        err = device.open(PartitionLabel);
        char name[BlockDevices::MaxName + 1];
        if (err.isOk()) err = BlockDevices::addNumbered(device, "flash", name);
        if (err.isOk() && esp_register_shutdown_handler(flushOnRestart) != ESP_OK)
                bmdsWarn("BootDisk: no shutdown handler: a restart may lose the last writes");
        if (err.isError()) {
                bmdsErr("BootDisk: the flash partition '%s': %s", PartitionLabel, err.desc());
                return err;
        }
        err = Vfs::mount(Path, device);
        bool formatted = false;
        if (err == Error::NoFilesystem) {
                // The first boot: a new filesystem with the default scripts.
                bmdsInfo("BootDisk: /dev/%s has no filesystem: formatting it", name);
                err = Vfs::format(device, FsFormat::Auto);
                if (err.isOk()) err = Vfs::mount(Path, device);
                formatted = err.isOk();
        }
        if (err.isOk()) err = Vfs::pin(Path);
        if (err.isError()) {
                bmdsErr("BootDisk: /dev/%s on %s: %s", name, Path, err.desc());
                return err;
        }
        installDefaults();
        bmdsInfo("BootDisk: /dev/%s mounted on %s, %u KB%s", name, Path,
                 static_cast<unsigned>(device.sectorCount() * device.sectorSize() / 1024),
                 formatted ? " (formatted)" : "");
        seedRandom();
        const esp_err_t herr = esp_register_shutdown_handler(saveSeedAtShutdown);
        if (herr != ESP_OK)
                bmdsWarn("BootDisk: no shutdown handler for %s: %s", SeedPath, Error::fromEspErr(herr).desc());
        return Error::Ok;
}

BMDS_NAMESPACE_END
