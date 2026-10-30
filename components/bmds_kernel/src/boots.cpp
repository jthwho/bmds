/**
 * @file      boots.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The record of the last boots (devplan/keptstate.md §4).
 */

#include <cstring>
#include <esp_app_desc.h>
#include <esp_attr.h>
#include <bmds/boots.h>
#include <bmds/clock.h>
#include <bmds/detail/platform.h>
#include <bmds/kept.h>
#include <bmds/keptrecord.h>
#include <bmds/log.h>
#include <bmds/mutex.h>
#include <bmds/staticobject.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace Boots {

        namespace {

                // A ring: `newest` is this boot's entry, the ones before it older.
                struct Record {
                                uint32_t count;
                                uint32_t newest;
                                Entry    entries[MaxEntries];
                };

                constexpr uint32_t Magic = 0x544f4f42; // "BOOT"

                RTC_NOINIT_ATTR uint32_t store[KeptRecord::storageWords(sizeof(Record))];
                constinit KeptRecord     kept(store, sizeof(Record), Magic);

                struct State {
                                Mutex  lock;
                                Record record{};
                };

                constinit StaticObject<State> state;

                // The RTC timer when the application started: the timer's reading less the uptime.
                int64_t startedUs() {
                        return Detail::persistentTimeUs() - Detail::monotonicUs();
                }

                void begin(Record &r) {
                        r.newest = (r.newest + 1) % MaxEntries;
                        if (r.count < MaxEntries) ++r.count;
                        Entry &e = r.entries[r.newest];
                        std::memset(&e, 0, sizeof(e));
                        e.startUs = startedUs();
                        e.aliveUs = Detail::persistentTimeUs();
                        e.endReason = Running;
                        std::strncpy(e.firmware, esp_app_get_description()->version, sizeof(e.firmware) - 1);
                        return;
                }

                // The time last alive, each pass (Kept).
                void keep(void *) {
                        State        &s = *state;
                        Mutex::Locker locker(s.lock, WaitForever);
                        s.record.entries[s.record.newest].aliveUs = Detail::persistentTimeUs();
                        kept.save(&s.record);
                        return;
                }

                bool passes(const Record &r) {
                        if (r.count == 0 || r.count > MaxEntries || r.newest >= MaxEntries) return false;
                        // Times run forward from the oldest to the newest.
                        for (uint32_t i = 1; i < r.count; ++i) {
                                const Entry &newer = r.entries[(r.newest + MaxEntries - i + 1) % MaxEntries];
                                const Entry &older = r.entries[(r.newest + MaxEntries - i) % MaxEntries];
                                if (older.startUs > newer.startUs) return false;
                        }
                        return r.entries[r.newest].startUs <= startedUs();
                }

        } // namespace

        Error init() {
                State  &s = state.create();
                Record &r = s.record;
                if (kept.load(&r) && passes(r)) {
                        Entry        &last = r.entries[r.newest];
                        const int64_t now = startedUs();
                        last.endReason = System::resetReasonCode();
                        const int64_t ranS = (now - last.startUs) / 1000000;
                        const int64_t quietS = (now - last.aliveUs) / 1000000;
                        bmdsInfo("Boots: the last ran %lld:%02d:%02d, ended by %s, last alive %lld s before",
                                 static_cast<long long>(ranS / 3600), int(ranS / 60 % 60), int(ranS % 60),
                                 System::resetReason(), static_cast<long long>(quietS));
                } else {
                        std::memset(&r, 0, sizeof(r));
                        r.newest = MaxEntries - 1;
                        bmdsInfo("Boots: the record started");
                }
                begin(r);
                kept.save(&r);
                return Kept::add(keep, nullptr);
        }

        size_t list(Span<Entry> out) {
                if (!state.isCreated()) return 0;
                State        &s = *state;
                Mutex::Locker locker(s.lock, WaitForever);
                const Record &r = s.record;
                size_t        n = 0;
                for (; n < r.count && n < out.size(); ++n) out[n] = r.entries[(r.newest + MaxEntries - n) % MaxEntries];
                return n;
        }

        bool wallTime(int64_t rtcUs, Timestamp &out) {
                if (!Clock::isSet()) return false;
                out = Timestamp{Clock::now().ns - (Detail::persistentTimeUs() - rtcUs) * 1000};
                return true;
        }

        void clear() {
                if (!state.isCreated()) return;
                State        &s = *state;
                Mutex::Locker locker(s.lock, WaitForever);
                const Entry   now = s.record.entries[s.record.newest];
                std::memset(&s.record, 0, sizeof(s.record));
                s.record.count = 1;
                s.record.entries[0] = now;
                kept.save(&s.record);
                return;
        }

} // namespace Boots

BMDS_NAMESPACE_END
