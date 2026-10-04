/**
 * @file      storage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <atomic>
#include <bmds/log.h>
#include <bmds/sdcard.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/storage.h>
#include <bmds/task.h>

BMDS_DEBUG(Storage)

BMDS_NAMESPACE_BEGIN

namespace {

        // The card-detect switch must be quiet this long before it counts.
        constexpr unsigned int DebounceMs = 50;

        struct Service {
                        Service() : firstCheck(1, 0) {}

                        Task      task;
                        Semaphore firstCheck; // Released once the first check is done.
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
                return;
        }

        void cardRemoved(SdCard &card) {
                card.detach();
                bmdsInfo("Storage: SD card removed");
                return;
        }

        void serviceMain(void *) {
                SdCard &card = SdCard::instance();
                bool    first = true;
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
                        (void)card.waitForDetectChange(WaitForever);
                }
        }

} // namespace

Error Storage::init() {
        Error err = SdCard::init();
        if (err.isError()) return err;
        Service     &s = service.create();
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
