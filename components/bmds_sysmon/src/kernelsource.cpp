/**
 * @file      kernelsource.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The System Monitor's reads from the kernel's public classes (devplan/sysmon.md §3.6).
 */

#include <cstdio>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/cputime.h>
#include <bmds/loader.h>
#include <bmds/memory.h>
#include <bmds/stats.h>
#include <bmds/tasklist.h>

#include "kernelsource.h"

BMDS_NAMESPACE_BEGIN

namespace {

        void copyCut(char *out, size_t size, const char *text) {
                std::snprintf(out, size, "%s", text != nullptr ? text : "");
                // Cut at a character's end.
                size_t n = std::strlen(out);
                if (n + 1 == size)
                        while (n > 0 && (static_cast<uint8_t>(out[n]) & 0xc0u) == 0x80u) out[--n] = '\0';
                return;
        }

} // namespace

KernelSource::~KernelSource() {
        (void)watch(false);
}

Error KernelSource::counter(size_t index, Counter &out) {
        Stats::Info k;
        if (Stats::describe(index, k).isError()) return Error::NotFound;
        out = Counter{};
        out.id = k.id;
        out.generation = k.generation;
        out.unit = k.desc.unit;
        out.kind = k.desc.kind;
        out.flags = k.desc.flags;
        out.limits = k.limits;
        const bool event = k.limits.rule == Stat::Rule::Zero ||
                           (k.limits.rule == Stat::Rule::Above && k.desc.kind == Stat::Kind::Peak);
        out.hold = k.limits.hold != 0 || !event ? k.limits.hold : Stat::DefaultEventHold;
        copyCut(out.name, sizeof(out.name), k.name);
        copyCut(out.labels, sizeof(out.labels), k.desc.labels);
        copyCut(out.help, sizeof(out.help), k.desc.help);
        return Error::Ok;
}

size_t KernelSource::read(Span<const uint32_t> ids, Span<int64_t> values, Span<uint8_t> health, int64_t &timeNs) {
        const size_t n = Stats::read(ids, values, health);
        timeNs = int64_t(Clock::monotonicNs());
        return n;
}

size_t KernelSource::history(uint32_t id, Span<int32_t> out) {
        return Stats::history(id, out);
}

size_t KernelSource::problems(uint64_t after, Span<Stat::ProblemLog::Episode> out) {
        return Stats::problems(after, out);
}

Error KernelSource::watch(bool on) {
        if (on == _watching) return Error::Ok;
        if (on) {
                const Error err = Stats::watch();
                if (err.isError()) return err;
        } else {
                Stats::unwatch();
        }
        _watching = on;
        return Error::Ok;
}

void KernelSource::markSeen(uint64_t change) {
        Stats::markProblemsSeen(change);
        return;
}

void KernelSource::clearProblems() {
        Stats::clearProblems();
        return;
}

Source::Status KernelSource::status() {
        const Stats::Status k = Stats::status();
        Status              s;
        s.generation = k.generation;
        s.count = uint32_t(k.count);
        s.watchers = uint32_t(k.watchers);
        s.intervalMs = k.intervalMs;
        s.cores = uint32_t(CpuTime::cores());
        s.cpuCounted = CpuTime::isEnabled();
        s.lastChange = k.lastProblemChange;
        s.unseen = k.unseen;
        s.lastSampleNs = k.lastSampleMs * 1000000;
        return s;
}

size_t KernelSource::tasks(Span<Task> out) {
        if (out.size() == 0) return TaskList::list(Span<TaskList::Info>());
        auto *infos =
                static_cast<TaskList::Info *>(Memory::alloc(MemSpace::Psram, out.size() * sizeof(TaskList::Info)));
        auto *owners = static_cast<Loader::TaskOwnership *>(
                Memory::alloc(MemSpace::Psram, out.size() * sizeof(Loader::TaskOwnership)));
        size_t total = 0;
        if (infos != nullptr && owners != nullptr) {
                total = TaskList::list(Span<TaskList::Info>(infos, out.size()));
                const size_t n = total < out.size() ? total : out.size();
                for (size_t i = 0; i < n; ++i) owners[i].task = infos[i].task;
                Loader::findTaskOwners(Span<Loader::TaskOwnership>(owners, n));
                for (size_t i = 0; i < n; ++i) {
                        const TaskList::Info &t = infos[i];
                        Task                 &e = out[i];
                        e = Task{};
                        std::memcpy(e.name, t.name, sizeof(e.name));
                        e.name[sizeof(e.name) - 1] = '\0';
                        e.pid = owners[i].pid;
                        e.tid = owners[i].tid;
                        e.main = owners[i].main;
                        e.state = uint8_t(t.state);
                        e.priority = t.priority;
                        e.basePriority = t.basePriority;
                        e.core = t.core;
                        e.stackFree = t.stackFree;
                        e.number = t.number;
                        e.cpuUs = t.cpuUs;
                }
        }
        Memory::free(infos);
        Memory::free(owners);
        return total;
}

Error KernelSource::process(int32_t pid, bool next, Process &out) {
        Loader::ProcessInfo k;
        if (Loader::processInfo(pid, next, k).isError()) return Error::NotExist;
        out = Process{};
        out.pid = k.pid;
        out.parent = k.parent;
        using KState = Loader::ProcessInfo::State;
        switch (k.state) {
                case KState::Running: out.state = Process::State::Running; break;
                case KState::Vfork: out.state = Process::State::Vfork; break;
                case KState::Ending: out.state = Process::State::Ending; break;
                case KState::Zombie: out.state = Process::State::Zombie; break;
                case KState::Stopped: out.state = Process::State::Stopped; break;
        }
        out.threads = k.threads;
        out.status = k.status;
        out.memory = k.memory;
        out.cpuUs = k.cpuUs;
        out.startNs = k.startedUs * 1000;
        std::memcpy(out.command, k.command, sizeof(out.command));
        out.command[sizeof(out.command) - 1] = '\0';
        return Error::Ok;
}

Error KernelSource::signal(int32_t pid, int sig) {
        return Loader::signal(pid, sig);
}

BMDS_NAMESPACE_END
