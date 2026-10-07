/**
 * @file      clockinit.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The target's clock boot stage: the kernel time zone, and the time a reset
 * carried over in RTC memory.
 */

#include <esp_system.h>
#include <bmds/clock.h>
#include <bmds/detail/platform.h>
#include <bmds/log.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

namespace {

        void saveAtShutdown() {
                Clock::saveResetRecord();
                return;
        }

} // namespace

Error Clock::init() {
        // A name is pending until the VFS's resolver and the card's database are there.
        if (setTimeZone(CONFIG_BMDS_TIMEZONE).isError())
                bmdsWarn("Clock: CONFIG_BMDS_TIMEZONE \"%s\" is not a TZ string or zone name; using UTC",
                         CONFIG_BMDS_TIMEZONE);

        Detail::ClockRecord record;
        if (Detail::loadClockRecord(record)) {
                const Timestamp wall{Detail::persistentTimeUs() * 1000 + record.wallMinusPersistentNs};
                Error           err = set(wall, ClockSource::Rtc);
                if (err.isOk()) err = setFrequency(record.frequencyPpb);
                String text;
                (void)formatTime(text, "%Y-%m-%d %H:%M:%S %Z", localTime(wall));
                bmdsInfo("Clock: %s (carried over the reset)", text.cStr());
        }
        // A deliberate restart refreshes the record, so the RTC timer's own
        // drift only counts from there.
        const esp_err_t err = esp_register_shutdown_handler(saveAtShutdown);
        if (err != ESP_OK) bmdsWarn("Clock: no shutdown handler: %s", Error::fromEspErr(err).desc());
        return Error();
}

BMDS_NAMESPACE_END
