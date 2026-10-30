/**
 * @file      keptcounters.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system monitor's counters kept across boots (devplan/keptstate.md §5).
 */

#include <esp_attr.h>
#include <bmds/detail/platform.h>
#include <bmds/kept.h>
#include <bmds/keptrecord.h>
#include <bmds/mutex.h>
#include <bmds/stat.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>
#include "keptcounters.h"

BMDS_NAMESPACE_BEGIN

namespace KeptCounters {

        namespace {

                struct Counter {
                                const char *name;
                                Mode        mode;
                };

                // The counters kept. Changing the list starts the record again (its hash).
                constexpr Counter List[] = {
                        {"mem.internal.min_free", Mode::Low},
                        {"mem.internal.largest", Mode::Low},
                        {"mem.internal_dma.min_free", Mode::Low},
                        {"mem.psram.min_free", Mode::Low},
                        {"mem.psram.largest", Mode::Low},
                        {"audio.dsp0.fill_us_max", Mode::High},
                        {"display0.scanout.isr_cycles_max", Mode::High},
                        {"display0.surface.wait_us_max", Mode::High},
                        {"net.ip.heap_max", Mode::High},
                        {"display0.scanout.underruns", Mode::Total},
                        {"display0.scanout.resyncs", Mode::Total},
                        {"display0.scanout.failures", Mode::Total},
                        {"audio.dsp0.late", Mode::Total},
                        {"audio.dsp0.underruns", Mode::Total},
                        {"audio.dsp0.client_late", Mode::Total},
                        {"audio.dsp0.sounds_dropped", Mode::Total},
                        {"net.eth0.rx_dropped", Mode::Total},
                        {"net.eth0.rx_errors", Mode::Total},
                        {"net.eth0.tx_errors", Mode::Total},
                        {"net.eth0.rx_missed", Mode::Total},
                        {"net.eth0.rx_overflow", Mode::Total},
                        {"net.ip.heap_failed", Mode::Total},
                        {"net.ip.pools_failed", Mode::Total},
                        {"console.usbserial.rx_dropped", Mode::Total},
                        {"usb0.unhandled_halts", Mode::Total},
                        {"usb0.hid_rearm_failures", Mode::Total},
                        {"midi.port0.dropped", Mode::Total},
                };
                constexpr size_t Count = sizeof(List) / sizeof(List[0]);

                // Low, high: `kept` the extreme (`Stat::Unavailable`: never seen). Total: `kept` the
                // earlier boots' sum, `last` this boot's reading.
                struct Value {
                                int64_t kept;
                                int64_t last;
                };

                struct Record {
                                uint32_t listHash;
                                uint32_t boots;
                                int64_t  startUs;
                                Value    values[Count];
                };

                constexpr uint32_t Magic = 0x5450454b; // "KEPT"

                RTC_NOINIT_ATTR uint32_t store[KeptRecord::storageWords(sizeof(Record))];
                constinit KeptRecord     kept(store, sizeof(Record), Magic);

                struct State {
                                Mutex    lock;
                                Record   record{};
                                int64_t  now[Count] = {};
                                uint32_t ids[Count] = {};
                                uint32_t generation = 0;
                                bool     found = false; // ids looked up at least once
                };

                constinit StaticObject<State> state;

                uint32_t listHash() {
                        uint32_t h = 2166136261u;
                        auto     mix = [&h](uint8_t b) {
                                h ^= b;
                                h *= 16777619u;
                        };
                        for (const Counter &k : List) {
                                for (const char *c = k.name; *c != '\0'; ++c) mix(uint8_t(*c));
                                mix(uint8_t(k.mode));
                        }
                        return h;
                }

                void start(Record &r) {
                        r.listHash = listHash();
                        r.boots = 1;
                        r.startUs = Detail::persistentTimeUs() - Detail::monotonicUs();
                        for (size_t i = 0; i < Count; ++i)
                                r.values[i] = {List[i].mode == Mode::Total ? 0 : Stat::Unavailable, 0};
                        return;
                }

                // Each pass (Kept): the counters read, the record saved if anything changed.
                void keep(void *) {
                        State         &s = *state;
                        Mutex::Locker  locker(s.lock, WaitForever);
                        const uint32_t generation = Stats::generation();
                        if (!s.found || generation != s.generation) {
                                for (size_t i = 0; i < Count; ++i) s.ids[i] = Stats::find(List[i].name);
                                s.generation = generation;
                                s.found = true;
                        }
                        (void)Stats::read(Span<const uint32_t>(s.ids, Count), Span<int64_t>(s.now, Count));
                        bool changed = false;
                        for (size_t i = 0; i < Count; ++i) {
                                const int64_t v = s.now[i];
                                Value        &k = s.record.values[i];
                                if (v == Stat::Unavailable) continue;
                                switch (List[i].mode) {
                                        case Mode::Low:
                                                if (k.kept == Stat::Unavailable || v < k.kept) {
                                                        k.kept = v;
                                                        changed = true;
                                                }
                                                break;
                                        case Mode::High:
                                                if (k.kept == Stat::Unavailable || v > k.kept) {
                                                        k.kept = v;
                                                        changed = true;
                                                }
                                                break;
                                        case Mode::Total:
                                                if (v != k.last) {
                                                        k.last = v;
                                                        changed = true;
                                                }
                                                break;
                                }
                        }
                        if (changed) kept.save(&s.record);
                        return;
                }

        } // namespace

        Error init() {
                State  &s = state.create();
                Record &r = s.record;
                if (kept.load(&r) && r.listHash == listHash() && r.startUs <= Detail::persistentTimeUs()) {
                        r.boots += 1;
                        for (size_t i = 0; i < Count; ++i) {
                                if (List[i].mode != Mode::Total) continue;
                                r.values[i].kept += r.values[i].last; // the last boot's count joins the sum
                                r.values[i].last = 0;
                        }
                } else {
                        start(r);
                }
                for (int64_t &v : s.now) v = Stat::Unavailable;
                kept.save(&r);
                return Kept::add(keep, nullptr);
        }

        size_t count() {
                return Count;
        }

        size_t rows(Span<Row> out) {
                if (!state.isCreated()) return 0;
                State        &s = *state;
                Mutex::Locker locker(s.lock, WaitForever);
                size_t        n = 0;
                for (; n < Count && n < out.size(); ++n) {
                        const Value &v = s.record.values[n];
                        out[n] = {List[n].name, List[n].mode, s.now[n],
                                  List[n].mode == Mode::Total ? v.kept + v.last : v.kept};
                }
                return n;
        }

        Summary summary() {
                Summary out;
                if (!state.isCreated()) return out;
                State        &s = *state;
                Mutex::Locker locker(s.lock, WaitForever);
                out.boots = s.record.boots;
                out.startUs = s.record.startUs;
                return out;
        }

        void clear() {
                if (!state.isCreated()) return;
                State        &s = *state;
                Mutex::Locker locker(s.lock, WaitForever);
                start(s.record);
                // Totals from now: this boot's count so far left out.
                for (size_t i = 0; i < Count; ++i) {
                        if (List[i].mode != Mode::Total || s.now[i] == Stat::Unavailable) continue;
                        s.record.values[i] = {-s.now[i], s.now[i]};
                }
                kept.save(&s.record);
                return;
        }

} // namespace KeptCounters

BMDS_NAMESPACE_END
