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
#include <bmds/task.h>
#include <bmds/vfs.h>

#include "bootdefaults.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *PartitionLabel = "boot";

        // A flash partition through wear levelling, as whole sectors (512 bytes: FatFs's).
        //
        // Writes go through a write-back cache of one 4 KB flash block: wear levelling rewrites a
        // whole flash sector (twice, in its safe mode) for each 512-byte write, so the sectors of
        // a block written together (a file's data; the FAT and the directory, rewritten again and
        // again) cost one erase and one write when the block is flushed: on sync() (FatFs's on
        // close), when another block is needed, or after IdleFlushMs without writes. A write of
        // whole blocks goes straight to the flash.
        //
        // A flash operation turns off the cache PSRAM is reached through, so it must run on a stack
        // in internal RAM (IDF asserts it): a caller whose stack is in PSRAM (the shells, programs)
        // hands the operation to the device's worker task, whose stack is internal, and waits.
        class FlashBlockDevice final : public BlockDevice {
                public:
                        static constexpr uint32_t     BlockBytes = 4096; // a flash sector
                        static constexpr unsigned int IdleFlushMs = 1000;

                        FlashBlockDevice() : _request(1, 0), _done(1, 0) {}

                        // Mounts wear levelling on the partition labelled @p label; starts the worker.
                        Error open(const char *label) {
                                const esp_partition_t *partition = esp_partition_find_first(
                                        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, label);
                                if (partition == nullptr) return Error::NotExist;
                                if (wl_mount(partition, &_handle) != ESP_OK) return Error::IoError;
                                _sectorSize = static_cast<uint32_t>(wl_sector_size(_handle));
                                _sectors = wl_size(_handle) / _sectorSize;
                                _block = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, BlockBytes));
                                if (_block == nullptr) return Error::NoMem;
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

                        Error sync() override { return run(Op::Sync, 0, nullptr, 0); }

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
                                Sync
                        };

                        static constexpr size_t NoBlock = SIZE_MAX;

                        // One operation, on this task if its stack is internal, else on the worker's.
                        Error run(Op op, size_t at, uint8_t *data, size_t bytes) {
                                Mutex::Locker lock(_lock, WaitForever); // also: wear levelling isn't thread-safe
                                if (esp_ptr_internal(__builtin_frame_address(0))) return perform(op, at, data, bytes);
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
                                        case Op::Write: return write(at, data, bytes);
                                        default: return flush();
                                }
                        }

                        // The cached block's sectors from the cache, the rest from the flash.
                        Error read(size_t at, uint8_t *data, size_t bytes) {
                                while (bytes > 0) {
                                        const size_t block = at / BlockBytes, offset = at % BlockBytes;
                                        const size_t n = bytes < BlockBytes - offset ? bytes : BlockBytes - offset;
                                        if (block == _cached) {
                                                std::memcpy(data, _block + offset, n);
                                        } else if (wl_read(_handle, at, data, n) != ESP_OK) {
                                                return Error::IoError;
                                        }
                                        at += n;
                                        data += n;
                                        bytes -= n;
                                }
                                return Error::Ok;
                        }

                        // Whole blocks straight to the flash; parts of one into the cache.
                        Error write(size_t at, const uint8_t *data, size_t bytes) {
                                while (bytes > 0) {
                                        const size_t block = at / BlockBytes, offset = at % BlockBytes;
                                        const size_t n = bytes < BlockBytes - offset ? bytes : BlockBytes - offset;
                                        if (n == BlockBytes) {
                                                if (block == _cached) _cached = NoBlock; // replaced whole
                                                const Error err = program(at, data);
                                                if (err.isError()) return err;
                                        } else {
                                                if (block != _cached) {
                                                        const Error err = load(block);
                                                        if (err.isError()) return err;
                                                }
                                                std::memcpy(_block + offset, data, n);
                                                _dirty = true;
                                        }
                                        at += n;
                                        data += n;
                                        bytes -= n;
                                }
                                return Error::Ok;
                        }

                        // Block @p block into the cache, the one there flushed first.
                        Error load(size_t block) {
                                const Error err = flush();
                                if (err.isError()) return err;
                                _cached = NoBlock;
                                if (wl_read(_handle, block * BlockBytes, _block, BlockBytes) != ESP_OK)
                                        return Error::IoError;
                                _cached = block;
                                return Error::Ok;
                        }

                        Error flush() {
                                if (!_dirty) return Error::Ok;
                                const Error err = program(_cached * BlockBytes, _block);
                                if (err.isOk()) _dirty = false;
                                return err;
                        }

                        // A whole block: erased (one flash sector, so no read-modify-write), then written.
                        Error program(size_t at, const uint8_t *data) {
                                esp_err_t err = wl_erase_range(_handle, at, BlockBytes);
                                if (err == ESP_OK) err = wl_write(_handle, at, data, BlockBytes);
                                return err == ESP_OK ? Error::Ok : Error::IoError;
                        }

                        // Requests from tasks whose stacks are in PSRAM; a dirty block flushed when idle.
                        static void workerMain(void *arg) {
                                auto *self = static_cast<FlashBlockDevice *>(arg);
                                for (;;) {
                                        // The requester holds the lock, so a request is performed without it.
                                        if (self->_request.acquire(self->_dirty ? IdleFlushMs : WaitForever).isOk()) {
                                                self->_result =
                                                        self->perform(self->_op, self->_at, self->_data, self->_bytes);
                                                self->_done.release();
                                        } else if (self->_lock.tryLock().isOk()) { // idle: flush, unless busy
                                                (void)self->flush();
                                                self->_lock.unlock();
                                        }
                                }
                        }

                        Mutex       _lock; // one operation at a time, whoever runs it
                        Semaphore   _request;
                        Semaphore   _done;
                        Task        _worker;
                        wl_handle_t _handle = WL_INVALID_HANDLE;
                        uint32_t    _sectorSize = 512;
                        uint64_t    _sectors = 0;
                        uint8_t    *_block = nullptr; // the cached block (Psram)
                        size_t      _cached = NoBlock;
                        bool        _dirty = false;
                        Op          _op = Op::Read; // the worker's request, under _lock
                        size_t      _at = 0;
                        uint8_t    *_data = nullptr;
                        size_t      _bytes = 0;
                        Error       _result;
        };

        constinit StaticObject<FlashBlockDevice> flash;

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
