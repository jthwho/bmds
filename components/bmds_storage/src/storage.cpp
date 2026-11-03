/**
 * @file      storage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <cstdio>
#include <bmds/blockdevices.h>
#include <bmds/cfg.h>
#include <bmds/log.h>
#include <bmds/notifications.h>
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
                        std::atomic<uint32_t> question{0}; // "Format it?" while nothing on the card mounts
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

        void closeQuestion() {
                const uint32_t id = service->question.exchange(0, std::memory_order_acq_rel);
                if (id != 0) (void)Notifications::close(id, 0);
                return;
        }

        // A card nothing mounted from (devplan/format.md §3): asked about, Yes opening Format.
        void askToFormat(SdCard &card, uint64_t bytes) {
                size_t partitions = 0;
                {
                        BlockDevices::Entry entries[BlockDevices::MaxDevices];
                        const size_t        n = BlockDevices::list(Span<BlockDevices::Entry>(entries));
                        for (size_t i = 0; i < n; ++i) partitions += entries[i].parent == &card ? 1 : 0;
                }
                const unsigned tenths = unsigned(bytes / 100000000u); // GB, a decimal place
                Notification   n;
                n.flags = Notification::Question;
                n.timeoutMs = Notification::NoTimeout;
                n.target = Notification::Target::Launch;
                std::snprintf(n.targetText, sizeof(n.targetText), "format.desktop");
                std::snprintf(n.app, sizeof(n.app), "Storage");
                std::snprintf(n.icon, sizeof(n.icon), "media-flash");
                if (partitions == 0) {
                        std::snprintf(n.title, sizeof(n.title), "Blank SD card detected");
                        std::snprintf(n.body, sizeof(n.body),
                                      "The %u.%u GB card in %s is blank. Would you like to format it?", tenths / 10,
                                      tenths % 10, service->name);
                } else {
                        std::snprintf(n.title, sizeof(n.title), "SD card can't be read");
                        std::snprintf(n.body, sizeof(n.body),
                                      "The %u.%u GB card in %s has no FAT or exFAT filesystem. Would you like to "
                                      "format it? That erases everything on it.",
                                      tenths / 10, tenths % 10, service->name);
                }
                n.actionCount = 2;
                n.actionTargets = 1u << 0; // Yes: Format
                std::snprintf(n.actions[0], sizeof(n.actions[0]), "Yes");
                std::snprintf(n.actions[1], sizeof(n.actions[1]), "No");
                auto [id, err] = Notifications::post(n, 0, nullptr);
                if (err.isError()) {
                        bmdsWarn("Storage: can't ask about the card: %s", err.desc());
                        return;
                }
                closeQuestion(); // an earlier card's
                service->question.store(id, std::memory_order_release);
                return;
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
                if (mounted.error().isError() || mounted.value() == 0)
                        askToFormat(card, info.sectorCount * info.sectorSize);
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
                // A write in flight fails now rather than after ESP-IDF's timeouts: the eject
                // waits for it (the storage lock).
                card.cancelTransfer();
                // Unmount first: open files go stale (NoMedia) rather than failing with I/O errors.
                Vfs::eject(card);
                card.detach();
                closeQuestion();
                bmdsInfo("Storage: SD card removed");
                return;
        }

        void serviceMain(void *) {
                SdCard &card = SdCard::instance();
                bool    first = true;
                bool    seedPending = true;
                for (;;) {
                        // Settling (begun by init() for the first check) until a card is mounted or
                        // known not to be: waitmount doesn't wait longer for a slot that is empty.
                        // Debounce: act once the switch has been quiet for a while.
                        while (card.waitForDetectChange(DebounceMs).isOk()) {}
                        const bool inserted = card.isInserted();
                        bmdsDebug("Storage: card detect settled: %s", inserted ? "inserted" : "empty");
                        if (inserted && !card.isAttached()) {
                                cardInserted(card);
                        } else if (!inserted && card.isAttached()) {
                                cardRemoved(card);
                        }
                        Vfs::endSettling();
                        if (first) {
                                first = false;
                                service->firstCheck.release();
                        }
                        if (seedPending) {
                                // A new random seed in /cfg once boot's own use of it is over
                                // (devplan/random.md); a card change first is handled first.
                                if (card.waitForDetectChange(SeedDelayMs) != Error::Timeout) {
                                        Vfs::beginSettling();
                                        continue;
                                }
                                seedPending = false;
                                Cfg::refreshRandomSeed();
                        }
                        (void)card.waitForDetectChange(WaitForever);
                        Vfs::beginSettling();
                }
        }

} // namespace

Error Storage::init() {
        // The time zone database's path is the system configuration's clock/tzdata (sysconf.md §8).
        Error err = Vfs::init();
        if (err.isOk()) err = SdCard::init();
        if (err.isError()) return err;
        Service &s = service.create();
        err = BlockDevices::addNumbered(SdCard::instance(), "sd", s.name);
        if (err.isError()) return err;
        Vfs::beginSettling(); // the first check's: ended by the task
        Task::Config config;
        config.name = "storage";
        config.stackBytes = CONFIG_BMDS_STORAGE_STACK_BYTES;
        config.stackSpace = MemSpace::Psram;
        config.priority = CONFIG_BMDS_STORAGE_PRIORITY;
        err = s.task.start(config, serviceMain, nullptr);
        if (err.isOk()) {
                running.store(true, std::memory_order_release);
        } else {
                Vfs::endSettling();
        }
        return err;
}

void Storage::cardFormatted(BlockDevice &device) {
        if (running.load(std::memory_order_acquire) && &device == &SdCard::instance()) closeQuestion();
        return;
}

Error Storage::waitForFirstMount(unsigned int timeoutMs) {
        if (!running.load(std::memory_order_acquire)) return Error::NotReady;
        const Error err = service->firstCheck.acquire(timeoutMs);
        // Leave it released for the next waiter.
        if (err.isOk()) service->firstCheck.release();
        return err;
}

BMDS_NAMESPACE_END
