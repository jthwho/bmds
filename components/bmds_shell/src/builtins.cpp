/**
 * @file      builtins.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Built-in shell commands. Output formats are userspace interfaces: keep
 * column order and units, and add new fields only at the end.
 */

#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/console.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // How long `reboot` waits for queued output to reach the terminal.
        constexpr unsigned int RebootFlushMs = 500;

        // `log show` prints in pieces, each within the shell's write timeout.
        constexpr size_t LogShowChunkBytes = 256;

        int runVersion(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                const SystemInfo info = System::info();
                ctx.printf("BMDS %s (build %s, ESP-IDF %s)\n", info.bmdsVersion, info.buildVersion, info.idfVersion);
                ctx.printf("Board: %s\n", info.boardName);
                ctx.printf("Chip: ESP32-P4 rev v%u.%u, %u cores @ %u MHz\n", info.chipRevMajor, info.chipRevMinor,
                           info.cores, info.cpuMhz);
                return Shell::ExitSuccess;
        }

        int runUptime(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                const uint64_t ms = System::uptimeUs() / 1000;
                const uint64_t s = ms / 1000;
                ctx.printf("up %lud %02u:%02u:%02u.%03u\n", static_cast<unsigned long>(s / 86400),
                           static_cast<unsigned>(s / 3600 % 24), static_cast<unsigned>(s / 60 % 60),
                           static_cast<unsigned>(s % 60), static_cast<unsigned>(ms % 1000));
                return Shell::ExitSuccess;
        }

        int runMem(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                ctx.printf("%-11s %10s %10s %10s %10s\n", "space", "total", "free", "min_free", "largest");
                for (size_t i = 0; i < MemSpaceCount; ++i) {
                        const MemSpace space = static_cast<MemSpace>(i);
                        const MemStats s = Memory::stats(space);
                        ctx.printf("%-11s %10u %10u %10u %10u\n", memSpaceName(space),
                                   static_cast<unsigned>(s.totalBytes), static_cast<unsigned>(s.freeBytes),
                                   static_cast<unsigned>(s.minFreeBytes), static_cast<unsigned>(s.largestFreeBlock));
                }
                return Shell::ExitSuccess;
        }

        const char *taskStateName(eTaskState state) {
                switch (state) {
                        case eRunning: return "running";
                        case eReady: return "ready";
                        case eBlocked: return "blocked";
                        case eSuspended: return "suspended";
                        case eDeleted: return "deleted";
                        default: return "unknown";
                }
        }

        int runTasks(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                // Room for a few tasks created between counting and listing.
                UBaseType_t capacity = uxTaskGetNumberOfTasks() + 4;
                auto       *tasks =
                        static_cast<TaskStatus_t *>(Memory::alloc(MemSpace::Psram, capacity * sizeof(TaskStatus_t)));
                if (tasks == nullptr) {
                        ctx.eprint("tasks: out of memory\n");
                        return Shell::ExitFailure;
                }
                const UBaseType_t count = uxTaskGetSystemState(tasks, capacity, nullptr);

                // Sort by name so the listing is stable from run to run.
                for (UBaseType_t i = 1; i < count; ++i) {
                        for (UBaseType_t j = i;
                             j > 0 && StringView(tasks[j].pcTaskName) < StringView(tasks[j - 1].pcTaskName); --j) {
                                const TaskStatus_t t = tasks[j];
                                tasks[j] = tasks[j - 1];
                                tasks[j - 1] = t;
                        }
                }

                ctx.printf("%-16s %-9s %4s %4s %10s\n", "name", "state", "prio", "core", "stack_free");
                for (UBaseType_t i = 0; i < count; ++i) {
                        const TaskStatus_t &t = tasks[i];
                        const BaseType_t    core = xTaskGetCoreID(t.xHandle);
                        char                coreText[12] = "-";
                        if (core != tskNO_AFFINITY)
                                std::snprintf(coreText, sizeof(coreText), "%d", static_cast<int>(core));
                        // ESP-IDF reports the high-water mark in bytes.
                        ctx.printf("%-16s %-9s %4u %4s %10u\n", t.pcTaskName, taskStateName(t.eCurrentState),
                                   static_cast<unsigned>(t.uxCurrentPriority), coreText,
                                   static_cast<unsigned>(t.usStackHighWaterMark));
                }
                Memory::free(tasks);
                return Shell::ExitSuccess;
        }

        const char *levelName(LogLevel level) {
                switch (level) {
                        case LogLevel::Debug: return "debug";
                        case LogLevel::Info: return "info";
                        case LogLevel::Warn: return "warn";
                        case LogLevel::Err: return "err";
                }
                return "?";
        }

        void printLogStatus(CommandContext &ctx) {
                ctx.printf("level: %s\n", levelName(Log::level()));
                const Span<const DebugModule> modules = Log::debugModules();
                if (modules.isEmpty()) return;
                ctx.printf("%-20s %s\n", "module", "debug");
                // Print in name order (the registry is in link order): pick the
                // smallest name greater than the last one printed.
                StringView last;
                for (size_t n = 0; n < modules.size(); ++n) {
                        const DebugModule *next = nullptr;
                        for (const DebugModule &m : modules) {
                                if ((n == 0 || StringView(m.name) > last) &&
                                    (next == nullptr || StringView(m.name) < StringView(next->name))) {
                                        next = &m;
                                }
                        }
                        if (next == nullptr) break;
                        ctx.printf("%-20s %s\n", next->name, next->isEnabled() ? "on" : "off");
                        last = next->name;
                }
                return;
        }

        int setLogLevel(CommandContext &ctx, Args args) {
                if (args.size() != 3) return ctx.usage();
                const StringView name = args[2];
                if (name == "info") {
                        Log::setLevel(LogLevel::Info);
                } else if (name == "warn") {
                        Log::setLevel(LogLevel::Warn);
                } else if (name == "err") {
                        Log::setLevel(LogLevel::Err);
                } else {
                        return ctx.usage();
                }
                return Shell::ExitSuccess;
        }

        int setLogModules(CommandContext &ctx, Args args, bool on) {
                if (args.size() < 3) return ctx.usage();
                int result = Shell::ExitSuccess;
                for (size_t i = 2; i < args.size(); ++i) {
                        if (StringView(args[i]) == "all") {
                                for (const DebugModule &m : Log::debugModules()) m.setEnabled(on);
                                continue;
                        }
                        const DebugModule *m = Log::findDebugModule(args[i]);
                        if (m == nullptr) {
                                ctx.eprintf("log: no such debug module: %s\n", args[i]);
                                result = Shell::ExitFailure;
                                continue;
                        }
                        m->setEnabled(on);
                }
                return result;
        }

        int showLog(CommandContext &ctx, Args args) {
                if (args.size() != 2) return ctx.usage();
                const size_t capacity = Log::historyCapacity();
                if (capacity == 0) {
                        ctx.eprint("log: no history\n");
                        return Shell::ExitFailure;
                }
                char *buf = static_cast<char *>(Memory::alloc(MemSpace::Psram, capacity));
                if (buf == nullptr) {
                        ctx.eprint("log: out of memory\n");
                        return Shell::ExitFailure;
                }
                const size_t len = Log::copyHistory(Span<char>(buf, capacity));
                int          result = Shell::ExitSuccess;
                for (size_t done = 0; done < len; done += LogShowChunkBytes) {
                        if (ctx.isInterrupted()) {
                                result = Shell::ExitInterrupted;
                                break;
                        }
                        const size_t n = len - done < LogShowChunkBytes ? len - done : LogShowChunkBytes;
                        ctx.print(StringView(buf + done, n));
                }
                Memory::free(buf);
                return result;
        }

        int runLog(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() == 1) {
                        printLogStatus(ctx);
                        return Shell::ExitSuccess;
                }
                const StringView sub = args[1];
                if (sub == "level") return setLogLevel(ctx, args);
                if (sub == "on") return setLogModules(ctx, args, true);
                if (sub == "off") return setLogModules(ctx, args, false);
                if (sub == "show") return showLog(ctx, args);
                return ctx.usage();
        }

        // Parses "N" or "N.FFF" seconds into milliseconds (fraction to 1 ms).
        bool parseSeconds(StringView text, unsigned int &ms) {
                constexpr uint64_t MaxMs = 7 * 24 * 3600 * 1000ull;
                uint64_t           whole = 0;
                size_t             i = 0;
                for (; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                        whole = whole * 10 + static_cast<uint64_t>(text[i] - '0');
                        if (whole * 1000 > MaxMs) return false;
                }
                const bool hasWhole = i > 0;
                uint64_t   frac = 0;
                uint64_t   scale = 100;
                bool       hasFrac = false;
                if (i < text.size() && text[i] == '.') {
                        for (++i; i < text.size() && text[i] >= '0' && text[i] <= '9'; ++i) {
                                frac += static_cast<uint64_t>(text[i] - '0') * scale;
                                scale /= 10;
                                hasFrac = true;
                        }
                }
                if (i != text.size() || (!hasWhole && !hasFrac)) return false;
                const uint64_t total = whole * 1000 + frac;
                if (total > MaxMs) return false;
                ms = static_cast<unsigned int>(total);
                return true;
        }

        int runSleep(CommandContext &ctx) {
                const Args   args = ctx.args();
                unsigned int ms = 0;
                if (args.size() != 2 || !parseSeconds(args[1], ms)) return ctx.usage();
                if (ms == 0) return Shell::ExitSuccess; // 0 would mean WaitForever
                return ctx.shell().interrupt().sleep(ms).isOk() ? Shell::ExitSuccess : Shell::ExitInterrupted;
        }

        int runReboot(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() != 1) return ctx.usage();
                ctx.print("Rebooting...\n");
                (void)ctx.shell().console().flush(RebootFlushMs);
                System::restart();
        }

} // namespace

BMDS_SHELL_COMMAND(help, Shell::runHelp, "[command]", "List commands, or show one command's usage")
BMDS_SHELL_COMMAND(log, runLog, "[level info|warn|err | on <module>...|all | off <module>...|all | show]",
                   "Show or set the log level and debug modules; show the log history")
BMDS_SHELL_COMMAND(mem, runMem, "", "Show memory usage per memory space (bytes)")
BMDS_SHELL_COMMAND(reboot, runReboot, "", "Restart the system")
BMDS_SHELL_COMMAND(sleep, runSleep, "<seconds>", "Wait for a number of seconds (fractions allowed, e.g. 0.5)")
BMDS_SHELL_COMMAND(tasks, runTasks, "", "List tasks: state, priority, core, free stack (bytes)")
BMDS_SHELL_COMMAND(uptime, runUptime, "", "Show time since boot")
BMDS_SHELL_COMMAND(version, runVersion, "", "Show BMDS, build, board and chip versions")

BMDS_NAMESPACE_END
