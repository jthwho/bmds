/**
 * @file      proccommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `ps` and `kill`: the processes, seen and signalled (devplan/processes.md §3.9).
 */

#include <cstdlib>
#include <cstring>
#include <bmds/command.h>
#include <bmds/executor.h>
#include <bmds/loader.h>
#include <bmds/memory.h>
#include <bmds/processtable.h>
#include <bmds/shell.h>
#include <bmds/sys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // Linux's names, by number (BMDS's numbers are Linux's: bmds/sys.h).
        constexpr const char *SignalNames[] = {
                nullptr, "HUP",  "INT",  "QUIT", "ILL",    "TRAP",   "ABRT",  "BUS",  "FPE",  "KILL", "USR1",
                "SEGV",  "USR2", "PIPE", "ALRM", "TERM",   "STKFLT", "CHLD",  "CONT", "STOP", "TSTP", "TTIN",
                "TTOU",  "URG",  "XCPU", "XFSZ", "VTALRM", "PROF",   "WINCH", "IO",   "PWR",  "SYS",
        };
        constexpr int SignalCount = static_cast<int>(sizeof(SignalNames) / sizeof(SignalNames[0]));

        // @p text as a whole decimal number in [@p min, @p max].
        bool parseNumber(const char *text, long min, long max, long &out) {
                char      *end = nullptr;
                const long v = std::strtol(text, &end, 10);
                if (end == text || *end != '\0' || v < min || v > max) return false;
                out = v;
                return true;
        }

        // A signal by number, or by name with or without "SIG" (any case); -1 if none.
        int parseSignal(const char *text) {
                long n = 0;
                if (parseNumber(text, 0, SignalCount - 1, n)) return static_cast<int>(n);
                if (strncasecmp(text, "SIG", 3) == 0) text += 3;
                for (int i = 1; i < SignalCount; ++i) {
                        if (strcasecmp(text, SignalNames[i]) == 0) return i;
                }
                return -1;
        }

        char stateLetter(Loader::ProcessInfo::State state) {
                switch (state) {
                        case Loader::ProcessInfo::State::Running: return 'R';
                        case Loader::ProcessInfo::State::Vfork: return 'V';
                        case Loader::ProcessInfo::State::Ending: return 'E';
                        default: return 'Z';
                }
        }

        int listProcesses(CommandContext &ctx) {
                constexpr size_t Max = ProcessTable::Capacity;
                auto            *list = static_cast<Loader::ProcessInfo *>(
                        Memory::alloc(MemSpace::Psram, sizeof(Loader::ProcessInfo) * Max));
                if (list == nullptr) {
                        ctx.eprintf("ps: out of memory\n");
                        return Shell::ExitFailure;
                }
                const size_t n = Loader::listProcesses(Span<Loader::ProcessInfo>(list, Max));
                ctx.printf("  PID  PPID  PGID   SID S THR    MEMORY COMMAND\n");
                for (size_t i = 0; i < n && i < Max; ++i) {
                        const Loader::ProcessInfo &p = list[i];
                        ctx.printf("%5ld %5ld %5ld %5ld %c %3u %9lu %s\n", static_cast<long>(p.pid),
                                   static_cast<long>(p.parent > 0 ? p.parent : 0), static_cast<long>(p.group),
                                   static_cast<long>(p.session), stateLetter(p.state), static_cast<unsigned>(p.threads),
                                   static_cast<unsigned long>(p.memory),
                                   p.state == Loader::ProcessInfo::State::Zombie ? "<ended>" : p.command);
                }
                Memory::free(list);
                return Shell::ExitSuccess;
        }

        int listThreads(CommandContext &ctx) {
                // Sized by a first count; tasks started between the two calls are left out.
                const size_t count = Loader::listThreads(Span<Loader::ThreadInfo>());
                const size_t max = count > 0 ? count : 1;
                auto        *list = static_cast<Loader::ThreadInfo *>(
                        Memory::alloc(MemSpace::Psram, sizeof(Loader::ThreadInfo) * max));
                if (list == nullptr) {
                        ctx.eprintf("ps: out of memory\n");
                        return Shell::ExitFailure;
                }
                const size_t n = Loader::listThreads(Span<Loader::ThreadInfo>(list, max));
                ctx.printf("  PID TASK             S STACK_FREE\n");
                for (size_t i = 0; i < n && i < max; ++i) {
                        const Loader::ThreadInfo &t = list[i];
                        ctx.printf("%5ld %c%-15s %c %10lu\n", static_cast<long>(t.pid), t.main ? ' ' : '+', t.name,
                                   t.state, static_cast<unsigned long>(t.stackFree));
                }
                Memory::free(list);
                return Shell::ExitSuccess;
        }

        // ps [-T]
        int runPs(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) return listProcesses(ctx);
                if (args.size() == 2 && std::strcmp(args[1], "-T") == 0) return listThreads(ctx);
                return ctx.usage();
        }

        // kill [-s SIG | -SIG] [--] PID|-PGID|%JOB...   kill -l
        int runKill(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 2 && std::strcmp(args[1], "-l") == 0) {
                        for (int i = 1; i < SignalCount; ++i)
                                ctx.printf("%2d) SIG%-8s%s", i, SignalNames[i], i % 4 == 0 ? "\n" : " ");
                        ctx.printf("\n");
                        return Shell::ExitSuccess;
                }
                int    sig = BMDS_SIGTERM;
                size_t i = 1;
                if (i < args.size() && std::strcmp(args[i], "-s") == 0) {
                        if (i + 1 >= args.size() || (sig = parseSignal(args[i + 1])) < 0) return ctx.usage();
                        i += 2;
                } else if (i < args.size() && args[i][0] == '-' && std::strcmp(args[i], "--") != 0) {
                        if ((sig = parseSignal(args[i] + 1)) < 0) return ctx.usage();
                        ++i;
                }
                if (i < args.size() && std::strcmp(args[i], "--") == 0) ++i;
                if (i >= args.size()) return ctx.usage();
                int status = Shell::ExitSuccess;
                for (; i < args.size(); ++i) {
                        long pid = 0;
                        if (args[i][0] == '%') {
                                // A job of this shell: its process group.
                                Job                  *job = nullptr;
                                const JobTable::Match m = ctx.executor().state().jobs.find(StringView(args[i]), job);
                                if (m != JobTable::Match::Found) {
                                        ctx.eprintf("kill: %s: %s\n", args[i],
                                                    m == JobTable::Match::Ambiguous ? "ambiguous job spec"
                                                                                    : "no such job");
                                        status = Shell::ExitFailure;
                                        continue;
                                }
                                pid = -static_cast<long>(job->pid);
                        } else if (!parseNumber(args[i], INT32_MIN, INT32_MAX, pid) || pid == 0 || pid == -1) {
                                ctx.eprintf("kill: %s: not a process or group id\n", args[i]);
                                status = Shell::ExitFailure;
                                continue;
                        }
                        const Error err = Loader::signal(static_cast<int32_t>(pid), sig);
                        if (err.isOk()) continue;
                        status = Shell::ExitFailure;
                        if (err == Error::NotExist)
                                ctx.eprintf("kill: %ld: no such process\n", pid);
                        else if (err == Error::PermissionDenied)
                                ctx.eprintf("kill: %ld: not permitted\n", pid);
                        else
                                ctx.eprintf("kill: %ld: %s\n", pid, err.desc());
                }
                return status;
        }

} // namespace

BMDS_SHELL_COMMAND(kill, runKill, "[-s SIG | -SIG] [--] PID|-PGID|%JOB... | -l",
                   "Send a signal (TERM by default) to processes or process groups; -l lists the signals")
BMDS_SHELL_COMMAND(ps, runPs, "[-T]", "List processes (-T: their tasks)")

BMDS_NAMESPACE_END
