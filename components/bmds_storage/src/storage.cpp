/**
 * @file      storage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <bmds/blockdevices.h>
#include <bmds/bootdisk.h>
#include <bmds/log.h>
#include <bmds/sdcard.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/storage.h>
#include <bmds/task.h>
#include <bmds/vfs.h>

BMDS_DEBUG(Storage)

BMDS_NAMESPACE_BEGIN

namespace {

        // The card-detect switch must be quiet this long before it counts.
        constexpr unsigned int DebounceMs = 50;

        // When the random seed is written after boot.
        constexpr unsigned int SeedDelayMs = 5000;

        struct Service {
                        Service() : firstCheck(1, 0) {}

                        Task      task;
                        Semaphore firstCheck;                           // Released once the first check is done.
                        char      name[BlockDevices::MaxName + 1] = {}; // The slot's device name ("sd0").
        };

        constinit StaticObject<Service> service;
        constinit std::atomic<bool>     running = false;

        const char *typeName(SdCard::Type type) {
                switch (type) {
                        case SdCard::Type::Sdsc: return "SDSC";
                        case SdCard::Type::Sdhc: return "SDHC";
                        case SdCard::Type::Sdxc: return "SDXC";
                        case SdCard::Type::Mmc: return "MMC";
                        default: return "other";
                }
        }

        void cardInserted(SdCard &card) {
                const Error err = card.attach();
                if (err.isError()) {
                        // Retried on the next insertion.
                        bmdsWarn("Storage: SD card not usable: %s", err.desc());
                        return;
                }
                const SdCard::Info info = card.info();
                const uint64_t     mb = info.sectorCount * info.sectorSize / 1000000;
                bmdsInfo("Storage: SD card %s (%s), %llu MB, %u-bit %u kHz", info.name, typeName(info.type),
                         static_cast<unsigned long long>(mb), info.busWidth, static_cast<unsigned>(info.clockKhz));
                const Result<size_t> mounted = Vfs::automount(card);
                if (mounted.error().isError()) {
                        bmdsWarn("Storage: /dev/%s: no partitions: %s", service->name, mounted.error().desc());
                } else if (mounted.value() == 0) {
                        bmdsWarn("Storage: /dev/%s holds no FAT or exFAT filesystem (mkfs formats it)", service->name);
                }
                Vfs::MountInfo mounts[Vfs::MaxMounts];
                const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts));
                for (size_t i = 0; i < n; ++i) {
                        // This card's mounts: its partitions are named after it.
                        if (StringView(mounts[i].device).startsWith(StringView(service->name)))
                                bmdsInfo("Storage: /dev/%s mounted on %s (%s)", mounts[i].device, mounts[i].path,
                                         mounts[i].type);
                }
                return;
        }

        void cardRemoved(SdCard &card) {
                // Unmount first: open files go stale (NoMedia) rather than failing with I/O errors.
                Vfs::eject(card);
                card.detach();
                bmdsInfo("Storage: SD card removed");
                return;
        }

        void serviceMain(void *) {
                SdCard &card = SdCard::instance();
                bool    first = true;
                bool    seedPending = true;
                for (;;) {
                        // Debounce: act once the switch has been quiet for a while.
                        while (card.waitForDetectChange(DebounceMs).isOk()) {}
                        const bool inserted = card.isInserted();
                        bmdsDebug("Storage: card detect settled: %s", inserted ? "inserted" : "empty");
                        if (inserted && !card.isAttached()) {
                                cardInserted(card);
                        } else if (!inserted && card.isAttached()) {
                                cardRemoved(card);
                        }
                        if (first) {
                                first = false;
                                service->firstCheck.release();
                        }
                        if (seedPending) {
                                // A new random seed in /boot once boot's own use of /boot is over
                                // (devplan/random.md); a card change first is handled first.
                                if (card.waitForDetectChange(SeedDelayMs) != Error::Timeout) continue;
                                seedPending = false;
                                BootDisk::refreshRandomSeed();
                        }
                        (void)card.waitForDetectChange(WaitForever);
                }
        }

} // namespace

Error Storage::init() {
        Error err = Vfs::init();
        // The time zone database on the card (devplan/time.md §9); it may come later.
        if (err.isOk()) err = Vfs::setZoneDatabase(CONFIG_BMDS_ZONEINFO_PATH);
        if (err.isOk()) err = SdCard::init();
        if (err.isError()) return err;
        Service &s = service.create();
        err = BlockDevices::addNumbered(SdCard::instance(), "sd", s.name);
        if (err.isError()) return err;
        Task::Config config;
        config.name = "storage";
        config.stackBytes = CONFIG_BMDS_STORAGE_STACK_BYTES;
        config.stackSpace = MemSpace::Psram;
        config.priority = CONFIG_BMDS_STORAGE_PRIORITY;
        err = s.task.start(config, serviceMain, nullptr);
        if (err.isOk()) running.store(true, std::memory_order_release);
        return err;
}

Error Storage::waitForFirstMount(unsigned int timeoutMs) {
        if (!running.load(std::memory_order_acquire)) return Error::NotReady;
        const Error err = service->firstCheck.acquire(timeoutMs);
        // Leave it released for the next waiter.
        if (err.isOk()) service->firstCheck.release();
        return err;
}

BMDS_NAMESPACE_END
