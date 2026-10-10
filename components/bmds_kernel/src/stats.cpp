/**
 * @file      stats.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The registry of the system's counters (devplan/sysmon.md §3.2-§3.4).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/detail/platform.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

namespace Stats {

        namespace {

                constexpr uint32_t MinIntervalMs = 100;
                constexpr uint32_t MaxIntervalMs = 60000;

                int32_t clamp32(int64_t v) {
                        if (v == Stat::Unavailable) return 0;
                        return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : int32_t(v);
                }

                // A counter's full name: the group's, a dot, its own.
                size_t fullName(StringView groupName, const Stat::Desc &d, char (&out)[Stat::MaxName + 1]) {
                        const int n = std::snprintf(out, sizeof(out), "%.*s.%s", int(groupName.size()),
                                                    groupName.data(), d.name);
                        return n < 0 ? 0 : size_t(n) < sizeof(out) ? size_t(n) : sizeof(out) - 1;
                }

        } // namespace

        // What the registry keeps for a group while it is added (PSRAM).
        struct State {
                        List<Stat::Judge> judges{MemSpace::Psram};       // one a counter
                        Stat::History     history{MemSpace::Psram};      // while watched
                        List<uint8_t>     historyIndex{MemSpace::Psram}; // the counters kept, in order
        };

        struct Registry {
                        Mutex            lock;
                        Semaphore        wake{1, 0}; // the sampler: sample now, or stop
                        Thread           sampler;
                        Group           *groups = nullptr; // in registration order
                        uint16_t         nextSerial = 1;
                        uint32_t         generation = 1;
                        size_t           watchers = 0;
                        bool             sampling = false; // the sampler thread runs
                        uint32_t         intervalMs = DefaultIntervalMs;
                        int64_t          lastSampleMs = 0;
                        Stat::ProblemLog problems{MemSpace::Psram};
                        Stat::Overrides  overrides{MemSpace::Psram};

                        static void samplerMain(void *);

                        static void freeState(Group &group) {
                                if (group._state == nullptr) return;
                                group._state->~State();
                                Memory::free(group._state);
                                group._state = nullptr;
                                return;
                        }

                        // Under the lock.

                        Stat::Limits limitsOf(const Group &g, size_t index, StringView name) const {
                                const Stat::Desc &d = g._descs[index];
                                Stat::Limits      l = d.limits;
                                if ((d.flags & Stat::Adjusted) != 0 && g._adjust != nullptr)
                                        g._adjust(g._arg, index, l);
                                (void)overrides.apply(name, l);
                                return l;
                        }

                        Group *groupOf(uint32_t id) const {
                                const uint16_t serial = uint16_t(id >> 8);
                                for (Group *g = groups; g != nullptr; g = g->_next)
                                        if (g->_serial == serial) return (id & 0xff) < g->_count ? g : nullptr;
                                return nullptr;
                        }

                        bool serialInUse(uint16_t serial) const {
                                for (Group *g = groups; g != nullptr; g = g->_next)
                                        if (g->_serial == serial) return true;
                                return false;
                        }

                        bool isJudged(const Group &g) const { return sampling && watchers > 0 && g._state != nullptr; }

                        // The counter at @p index, described (no value).
                        Group *describe(size_t index, Info &out) const {
                                for (Group *g = groups; g != nullptr; g = g->_next) {
                                        if (index >= g->_count) {
                                                index -= g->_count;
                                                continue;
                                        }
                                        const Stat::Desc &d = g->_descs[index];
                                        out = Info{};
                                        out.id = g->idOf(index);
                                        const size_t len = fullName(StringView(g->_name), d, out.name);
                                        out.desc = d;
                                        out.limits = limitsOf(*g, index, StringView(out.name, len));
                                        out.judged = isJudged(*g);
                                        out.health =
                                                out.judged ? g->_state->judges[index].health() : Stat::Health::Normal;
                                        out.rate = out.judged ? g->_state->judges[index].rate() : Stat::Unavailable;
                                        out.generation = generation;
                                        return g;
                                }
                                return nullptr;
                        }

                        void readGroup(Group &g, int64_t (&values)[MaxPerGroup]) const {
                                for (size_t i = 0; i < g._count; ++i) values[i] = Stat::Unavailable;
                                g._read(g._arg, Span<int64_t>(values, g._count));
                                return;
                        }

                        // The judges, made when the group is first sampled (nothing until anyone watches).
                        Error ensureState(Group &g) {
                                if (g._state != nullptr) return Error::Ok;
                                void *block = Memory::alloc(MemSpace::Psram, sizeof(State));
                                if (block == nullptr) return Error::NoMem;
                                g._state = new (block) State;
                                if (g._state->judges.reserve(g._count).isError()) {
                                        freeState(g);
                                        return Error::NoMem;
                                }
                                for (size_t i = 0; i < g._count; ++i) (void)g._state->judges.append(Stat::Judge{});
                                return Error::Ok;
                        }

                        Error startHistory(Group &g) {
                                if (ensureState(g).isError()) return Error::NoMem;
                                size_t kept = 0;
                                for (size_t i = 0; i < g._count; ++i)
                                        kept += (g._descs[i].flags & Stat::KeepHistory) != 0;
                                if (kept == 0) return Error::Ok;
                                g._state->historyIndex.clear();
                                if (g._state->historyIndex.reserve(kept).isError()) return Error::NoMem;
                                for (size_t i = 0; i < g._count; ++i)
                                        if ((g._descs[i].flags & Stat::KeepHistory) != 0)
                                                (void)g._state->historyIndex.append(uint8_t(i));
                                return g._state->history.setup(kept, HistoryDepth);
                        }

                        void stopHistory(Group &g) {
                                if (g._state == nullptr) return;
                                g._state->history.release();
                                g._state->historyIndex.clear();
                                return;
                        }

                        void sampleAll(int64_t nowMs) {
                                for (Group *g = groups; g != nullptr; g = g->_next) sampleGroup(*g, nowMs);
                                lastSampleMs = nowMs;
                                return;
                        }

                        void sampleGroup(Group &g, int64_t nowMs) {
                                if (ensureState(g).isError()) return; // tried again at the next sample
                                int64_t values[MaxPerGroup];
                                readGroup(g, values);
                                const StringView groupName(g._name);
                                int32_t          kept[MaxPerGroup];
                                size_t           keptCount = 0;
                                bool             untimed = false; // a total's increase over no known interval
                                for (size_t i = 0; i < g._count; ++i) {
                                        const Stat::Desc  &d = g._descs[i];
                                        char               name[Stat::MaxName + 1];
                                        const size_t       len = fullName(groupName, d, name);
                                        const StringView   nameView(name, len);
                                        const Stat::Limits l = limitsOf(g, i, nameView);
                                        Stat::Judge       &judge = g._state->judges[i];
                                        const bool         first = judge.isFirst();
                                        const Stat::Health before = judge.health();
                                        const Stat::Health after = judge.update(l, d.kind, values[i], nowMs);
                                        const bool         event = l.rule == Stat::Rule::Zero ||
                                                           (l.rule == Stat::Rule::Above && d.kind == Stat::Kind::Peak);
                                        if (first && values[i] != Stat::Unavailable) {
                                                const Stat::Health earlier = Stat::eventHealth(l, d.kind, values[i]);
                                                if (earlier != Stat::Health::Normal)
                                                        (void)problems.noteEarlier(
                                                                g.idOf(i), nameView, earlier, values[i],
                                                                d.kind == Stat::Kind::Total ? uint64_t(values[i]) : 1,
                                                                nowMs);
                                        }
                                        const uint64_t n = event ? uint64_t(judge.delta() > 0 ? judge.delta() : 0) : 1;
                                        (void)problems.note(g.idOf(i), nameView, before, after, values[i], n, nowMs,
                                                            judge.since());
                                        if (d.kind == Stat::Kind::Total && values[i] != Stat::Unavailable &&
                                            judge.rate() == Stat::Unavailable)
                                                untimed = true;
                                        if ((d.flags & Stat::KeepHistory) != 0 && keptCount < MaxPerGroup)
                                                kept[keptCount++] = clamp32(d.kind == Stat::Kind::Total ? judge.delta()
                                                                                                        : values[i]);
                                }
                                // The first sample after a pause (or ever) counts the whole gap as one
                                // interval: a spike in a graph. The history starts at the next one.
                                if (watchers > 0 && g._state->history.series() > 0 && !untimed)
                                        g._state->history.push(Span<const int32_t>(kept, keptCount));
                                return;
                        }
        };

        namespace {
                constinit StaticObject<Registry> registry;
        } // namespace

        void Registry::samplerMain(void *) {
                Registry &r = *registry;
                for (;;) {
                        uint32_t interval;
                        {
                                Mutex::Locker locker(r.lock, WaitForever);
                                interval = r.intervalMs;
                        }
                        (void)r.wake.acquire(interval);
                        Mutex::Locker locker(r.lock, WaitForever);
                        if (r.watchers == 0) {
                                // Nobody judges from here: the open problems close, the rules
                                // pause (the values kept, so what comes meanwhile is counted).
                                r.problems.closeAll(Detail::monotonicUs() / 1000);
                                for (Group *g = r.groups; g != nullptr; g = g->_next) {
                                        r.stopHistory(*g);
                                        if (g->_state == nullptr) continue;
                                        for (Stat::Judge &j : g->_state->judges) j.pause();
                                }
                                r.sampling = false;
                                r.sampler.detach(); // ends on its own; a later watch() starts another
                                return;
                        }
                        const int64_t nowMs = Detail::monotonicUs() / 1000;
                        r.sampleAll(nowMs);
                }
        }

        // ---- Group -------------------------------------------------------------------------------

        Group::~Group() {
                remove(*this);
        }

        void Group::setName(StringView base, int number) {
                if (number < 0)
                        std::snprintf(_name, sizeof(_name), "%.*s", int(base.size()), base.data());
                else
                        std::snprintf(_name, sizeof(_name), "%.*s%d", int(base.size()), base.data(), number);
                return;
        }

        StringView Group::name() const {
                return StringView(_name);
        }

        // ---- The registry ------------------------------------------------------------------------


        Error init() {
                if (!registry.isCreated()) registry.create();
                return Error::Ok;
        }

        Error add(Group &group) {
                if (!registry.isCreated()) return Error::InvalidState;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (group._serial != 0) return Error::Exists;
                if (group._count == 0 || group._descs == nullptr || group._read == nullptr || group._name[0] == '\0')
                        return Error::Invalid;
                if (r.watchers > 0) (void)r.startHistory(group); // else made when first sampled
                uint16_t serial = r.nextSerial;
                while (serial == 0 || r.serialInUse(serial)) ++serial; // wrapped: skip those in use
                r.nextSerial = uint16_t(serial + 1);
                group._serial = serial;
                group._next = nullptr;
                Group **tail = &r.groups;
                while (*tail != nullptr) tail = &(*tail)->_next;
                *tail = &group;
                ++r.generation;
                return Error::Ok;
        }

        void remove(Group &group) {
                if (!registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (group._serial == 0) return;
                for (Group **link = &r.groups; *link != nullptr; link = &(*link)->_next) {
                        if (*link != &group) continue;
                        *link = group._next;
                        break;
                }
                const int64_t nowMs = Detail::monotonicUs() / 1000;
                for (size_t i = 0; i < group._count; ++i) r.problems.close(group.idOf(i), nowMs);
                group._serial = 0;
                group._next = nullptr;
                Registry::freeState(group);
                ++r.generation;
                return;
        }

        uint32_t generation() {
                if (!registry.isCreated()) return 0;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->generation;
        }

        size_t count() {
                if (!registry.isCreated()) return 0;
                Mutex::Locker locker(registry->lock, WaitForever);
                size_t        n = 0;
                for (Group *g = registry->groups; g != nullptr; g = g->_next) n += g->_count;
                return n;
        }

        Error info(size_t index, Info &out) {
                if (!registry.isCreated()) return Error::NotFound;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                Group        *g = r.describe(index, out);
                if (g == nullptr) return Error::NotFound;
                int64_t values[MaxPerGroup];
                r.readGroup(*g, values);
                out.value = values[out.id & 0xff];
                return Error::Ok;
        }

        Error describe(size_t index, Info &out) {
                if (!registry.isCreated()) return Error::NotFound;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->describe(index, out) != nullptr ? Error::Ok : Error::NotFound;
        }

        uint32_t find(StringView name) {
                if (!registry.isCreated()) return NoId;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                for (Group *g = r.groups; g != nullptr; g = g->_next) {
                        const StringView groupName(g->_name);
                        if (!name.startsWith(groupName) || name.size() <= groupName.size() ||
                            name[groupName.size()] != '.')
                                continue;
                        const StringView rest = name.mid(groupName.size() + 1);
                        for (size_t i = 0; i < g->_count; ++i)
                                if (rest == StringView(g->_descs[i].name)) return g->idOf(i);
                }
                return NoId;
        }

        size_t read(Span<const uint32_t> ids, Span<int64_t> values) {
                return read(ids, values, Span<uint8_t>());
        }

        size_t read(Span<const uint32_t> ids, Span<int64_t> values, Span<uint8_t> health) {
                const size_t n = ids.size() < values.size() ? ids.size() : values.size();
                if (!registry.isCreated()) {
                        for (size_t i = 0; i < n; ++i) values[i] = Stat::Unavailable;
                        for (size_t i = 0; i < n && i < health.size(); ++i) health[i] = Unjudged;
                        return n;
                }
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                Group        *last = nullptr;
                int64_t       buffer[MaxPerGroup];
                for (size_t i = 0; i < n; ++i) {
                        Group *g = r.groupOf(ids[i]);
                        if (i < health.size())
                                health[i] = g != nullptr && r.isJudged(*g)
                                                    ? uint8_t(g->_state->judges[ids[i] & 0xff].health())
                                                    : Unjudged;
                        if (g == nullptr) {
                                values[i] = Stat::Unavailable;
                                continue;
                        }
                        if (g != last) { // a group read once for its run of ids
                                r.readGroup(*g, buffer);
                                last = g;
                        }
                        values[i] = buffer[ids[i] & 0xff];
                }
                return n;
        }

        Error watch() {
                if (!registry.isCreated()) return Error::InvalidState;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (r.watchers++ > 0) return Error::Ok;
                for (Group *g = r.groups; g != nullptr; g = g->_next) (void)r.startHistory(*g);
                if (r.sampling) return Error::Ok; // still stopping: it sees the watcher and goes on
                Thread::Config config;
                config.name = "stats";
                config.stackBytes = 4096;
                config.priority = 3;
                config.stackSpace = MemSpace::Psram;
                config.kernel = true; // not the watching process's, if one asked
                const Error err = r.sampler.start(config, Registry::samplerMain, nullptr);
                if (err.isError()) {
                        --r.watchers;
                        for (Group *g = r.groups; g != nullptr; g = g->_next) r.stopHistory(*g);
                        return err;
                }
                r.sampling = true;
                return Error::Ok;
        }

        void unwatch() {
                if (!registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                if (r.watchers == 0) return;
                if (--r.watchers == 0) r.wake.release(); // the sampler stops at once
                return;
        }

        size_t watchers() {
                if (!registry.isCreated()) return 0;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->watchers;
        }

        void setInterval(uint32_t ms) {
                if (!registry.isCreated()) return;
                Mutex::Locker locker(registry->lock, WaitForever);
                registry->intervalMs = ms < MinIntervalMs ? MinIntervalMs : ms > MaxIntervalMs ? MaxIntervalMs : ms;
                return;
        }

        uint32_t interval() {
                if (!registry.isCreated()) return DefaultIntervalMs;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->intervalMs;
        }

        void sample(int64_t nowMs) {
                if (!registry.isCreated()) return;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                r.sampleAll(nowMs);
                return;
        }

        size_t history(uint32_t id, Span<int32_t> out) {
                if (!registry.isCreated()) return 0;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                Group        *g = r.groupOf(id);
                if (g == nullptr) return 0;
                const uint8_t index = uint8_t(id & 0xff);
                if (g->_state == nullptr) return 0;
                for (size_t s = 0; s < g->_state->historyIndex.size(); ++s)
                        if (g->_state->historyIndex[s] == index) return g->_state->history.copy(s, out);
                return 0;
        }

        size_t problems(uint64_t after, Span<Stat::ProblemLog::Episode> out) {
                if (!registry.isCreated()) return 0;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->problems.changedAfter(after, out);
        }

        Status status() {
                Status s;
                if (!registry.isCreated()) return s;
                Registry     &r = *registry;
                Mutex::Locker locker(r.lock, WaitForever);
                s.generation = r.generation;
                for (Group *g = r.groups; g != nullptr; g = g->_next) s.count += g->_count;
                s.watchers = r.watchers;
                s.intervalMs = r.intervalMs;
                s.lastSampleMs = r.lastSampleMs;
                s.lastProblemChange = r.problems.lastChange();
                s.unseen = r.problems.unseen();
                return s;
        }

        uint64_t lastProblemChange() {
                if (!registry.isCreated()) return 0;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->problems.lastChange();
        }

        void markProblemsSeen(uint64_t change) {
                if (!registry.isCreated()) return;
                Mutex::Locker locker(registry->lock, WaitForever);
                registry->problems.markSeen(change);
                return;
        }

        Stat::Health unseenProblems() {
                if (!registry.isCreated()) return Stat::Health::Normal;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->problems.unseen();
        }

        void clearProblems() {
                if (!registry.isCreated()) return;
                Mutex::Locker locker(registry->lock, WaitForever);
                registry->problems.clearClosed();
                return;
        }

        Error setOverrides(StringView text, size_t *bad) {
                if (!registry.isCreated()) return Error::InvalidState;
                Mutex::Locker locker(registry->lock, WaitForever);
                return registry->overrides.parse(text, bad);
        }

} // namespace Stats

BMDS_NAMESPACE_END
