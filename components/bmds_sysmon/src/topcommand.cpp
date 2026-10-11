/**
 * @file      topcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `top` command (devplan/sysmon.md §3.5, step 6): the system's load, its memory and the
 * processes by CPU, redrawn every interval; read through the System Monitor's Source, as its
 * window is. The batch form (TopView's lines) is a userspace interface: keep the columns and
 * their order, and add new ones only at the end.
 */

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/console.h>
#include <bmds/fdtable.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/topview.h>

#include "kernelsource.h"
#include "monitor.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;
        using Row = TopView::Row;

        constexpr uint32_t FirstFrameMs = 500; // the first frame's shares are over this
        constexpr uint32_t MinIntervalMs = 100;
        constexpr uint32_t MaxIntervalMs = 3600 * 1000;
        constexpr size_t   MaxCores = 2;
        constexpr int64_t  Unavailable = Stat::Unavailable;

        // What a process or task had run at the last frame: its share is the change since.
        struct Seen {
                        int64_t  key = 0; // a pid, or a task's number
                        uint64_t cpuUs = 0;
        };

        const char *processState(Source::Process::State state) {
                switch (state) {
                        case Source::Process::State::Running: return "running";
                        case Source::Process::State::Vfork: return "vfork";
                        case Source::Process::State::Ending: return "ending";
                        case Source::Process::State::Zombie: return "zombie";
                        case Source::Process::State::Stopped: return "stopped";
                }
                return "-";
        }

        const char *taskState(uint8_t state) {
                static constexpr const char *Names[] = {"running", "ready", "blocked", "suspended", "deleted"};
                return state < 5 ? Names[state] : "unknown";
        }

        bool isIdle(const Source::Task &t) {
                return StringView(t.name).startsWith("IDLE");
        }

        // A command line's program: its first word's last part.
        StringView programName(const char *command) {
                StringView c(command);
                size_t     end = 0;
                while (end < c.size() && c[end] != ' ') ++end;
                StringView word = c.left(end);
                for (size_t i = word.size(); i-- > 0;)
                        if (word[i] == '/') return word.mid(i + 1);
                return word;
        }

        // Seconds, maybe with a fraction ("0.5"), as ms. False if it isn't one.
        bool parseSeconds(const char *text, uint32_t &ms) {
                char        *end = nullptr;
                const double s = std::strtod(text, &end);
                if (end == text || *end != '\0' || !(s >= 0)) return false;
                const double v = s * 1000.0;
                ms = v < MinIntervalMs ? MinIntervalMs : v > MaxIntervalMs ? MaxIntervalMs : uint32_t(v);
                return true;
        }

        class Top {
                public:
                        Top(CommandContext &ctx, bool showTasks)
                            : _ctx(ctx), _monitor(_source), _showTasks(showTasks) {}
                        ~Top() { (void)_source.watch(false); }
                        Top(const Top &) = delete;
                        Top &operator=(const Top &) = delete;

                        // Watching while it runs: the rules run, so the open problems are current.
                        void start() {
                                (void)_source.watch(true);
                                (void)read();
                                return;
                        }

                        // A frame read: the counters, the processes and the tasks, the time since the last.
                        Error read();

                        // The frame's lines, each ended by @p eol, into @p out; @p width cuts them (0: not),
                        // @p rows at most (0: all).
                        Error write(String &out, StringView eol, size_t width, size_t rows);

                private:
                        uint64_t since(List<Seen> &seen, int64_t key, uint64_t cpuUs) const;
                        void     headline(char (&out)[TopView::LineBytes]) const;

                        CommandContext       &_ctx;
                        KernelSource          _source;
                        Monitor               _monitor;
                        bool                  _showTasks;
                        List<Source::Process> _procs{MemSpace::Psram};
                        List<Source::Task>    _tasks{MemSpace::Psram};
                        List<Seen>            _seenProcs{MemSpace::Psram}; // at the frame before
                        List<Seen>            _seenTasks{MemSpace::Psram};
                        List<Seen>            _lastProcs{MemSpace::Psram}; // at this frame
                        List<Seen>            _lastTasks{MemSpace::Psram};
                        int64_t               _lastNs = 0;
                        int64_t               _elapsedUs = 0;
        };

        Error Top::read() {
                if (Error e = _monitor.refresh(); e.isError()) return e;
                const int64_t nowNs = int64_t(Clock::monotonicNs());
                _elapsedUs = _lastNs != 0 ? (nowNs - _lastNs) / 1000 : 0;
                _lastNs = nowNs;
                // The last frame's readings become the ones to compare with.
                _seenProcs.clear();
                _seenTasks.clear();
                for (const Seen &s : _lastProcs)
                        if (_seenProcs.append(s).isError()) return Error::NoMem;
                for (const Seen &s : _lastTasks)
                        if (_seenTasks.append(s).isError()) return Error::NoMem;
                _lastProcs.clear();
                _lastTasks.clear();

                _procs.clear();
                Source::Process p;
                for (int32_t pid = 0; _source.process(pid, true, p).isOk(); pid = p.pid) {
                        if (_procs.append(p).isError() || _lastProcs.append(Seen{p.pid, p.cpuUs}).isError())
                                return Error::NoMem;
                }
                // Room for a few tasks made between counting and listing.
                const size_t capacity = _source.tasks(Span<Source::Task>()) + 4;
                auto        *listed =
                        static_cast<Source::Task *>(Memory::alloc(MemSpace::Psram, capacity * sizeof(Source::Task)));
                if (listed == nullptr) return Error::NoMem;
                const size_t total = _source.tasks(Span<Source::Task>(listed, capacity));
                Error        err;
                _tasks.clear();
                for (size_t i = 0; i < total && i < capacity && err.isOk(); ++i) {
                        err = _tasks.append(listed[i]);
                        if (err.isOk()) err = _lastTasks.append(Seen{int64_t(listed[i].number), listed[i].cpuUs});
                }
                Memory::free(listed);
                return err;
        }

        // The time run since the frame before (0 if it is new).
        uint64_t Top::since(List<Seen> &seen, int64_t key, uint64_t cpuUs) const {
                for (const Seen &s : seen)
                        if (s.key == key) return cpuUs > s.cpuUs ? cpuUs - s.cpuUs : 0;
                return 0;
        }

        // "audio dsp0 5.6% 0 late, display0 60 fps, sd0 1.2 MB/s, eth0 in 1.2 MB/s out 3.0 KB/s": the
        // subsystems there are, and the whole storage devices and network interfaces with traffic.
        void Top::headline(char (&out)[TopView::LineBytes]) const {
                size_t     n = 0;
                const auto add = [&](const char *part) {
                        const int w = std::snprintf(out + n, sizeof(out) - n, "%s%s", n == 0 ? "" : ", ", part);
                        if (w > 0) n = size_t(w) + n < sizeof(out) ? n + size_t(w) : sizeof(out) - 1;
                };
                out[0] = '\0';
                char name[48], part[64], a[TopView::TextBytes];
                for (unsigned d = 0; d < 4; ++d) {
                        std::snprintf(name, sizeof(name), "audio.dsp%u.mix_us", d);
                        const size_t i = _monitor.find(name);
                        if (i == Monitor::NotFound) continue;
                        const int64_t mix = _monitor.rate(i);
                        std::snprintf(name, sizeof(name), "audio.dsp%u.late", d);
                        const int64_t late = _monitor.valueOf(name);
                        std::snprintf(part, sizeof(part), "audio dsp%u %s%% %" PRId64 " late", d,
                                      mix == Unavailable ? "-"
                                                         : TopView::formatShare(a, uint32_t(mix > 0 ? mix / 1000 : 0)),
                                      late == Unavailable ? 0 : late);
                        add(part);
                }
                for (unsigned d = 0; d < 2; ++d) {
                        std::snprintf(name, sizeof(name), "display%u.scanout.frames", d);
                        const size_t i = _monitor.find(name);
                        if (i == Monitor::NotFound) continue;
                        const int64_t fps = _monitor.rate(i);
                        std::snprintf(part, sizeof(part), "display%u %" PRId64 " fps", d, fps == Unavailable ? 0 : fps);
                        add(part);
                }
                for (size_t i = 0; i < _monitor.count(); ++i) {
                        const StringView c(_monitor.counter(i).name);
                        if (!c.startsWith("storage.") || !c.endsWith(".read_bytes")) continue;
                        const StringView dev = c.mid(8, c.size() - 8 - 11);
                        // Whole devices only (a partition's bytes are its device's too): no 'p' after the number.
                        bool partition = false;
                        for (size_t k = 1; k < dev.size(); ++k)
                                if (dev[k] == 'p' && dev[k - 1] >= '0' && dev[k - 1] <= '9') partition = true;
                        if (partition) continue;
                        std::snprintf(name, sizeof(name), "storage.%.*s.written_bytes", int(dev.size()), dev.data());
                        const int64_t r = _monitor.rate(i), w = _monitor.rateOf(name);
                        const int64_t bytes = (r == Unavailable ? 0 : r) + (w == Unavailable ? 0 : w);
                        if (bytes <= 0) continue;
                        std::snprintf(part, sizeof(part), "%.*s %s/s", int(dev.size()), dev.data(),
                                      TopView::formatBytes(a, uint64_t(bytes)));
                        add(part);
                }
                // The network interfaces with traffic: "eth0 in 1.2 MB/s out 3.0 KB/s".
                for (size_t i = 0; i < _monitor.count(); ++i) {
                        const StringView c(_monitor.counter(i).name);
                        if (!c.startsWith("net.") || !c.endsWith(".rx_bytes")) continue;
                        const StringView dev = c.mid(4, c.size() - 4 - 9);
                        std::snprintf(name, sizeof(name), "net.%.*s.tx_bytes", int(dev.size()), dev.data());
                        int64_t r = _monitor.rate(i), w = _monitor.rateOf(name);
                        r = r == Unavailable ? 0 : r;
                        w = w == Unavailable ? 0 : w;
                        if (r <= 0 && w <= 0) continue;
                        char b[TopView::TextBytes];
                        std::snprintf(part, sizeof(part), "%.*s in %s/s out %s/s", int(dev.size()), dev.data(),
                                      TopView::formatBytes(a, uint64_t(r)), TopView::formatBytes(b, uint64_t(w)));
                        add(part);
                }
                return;
        }

        Error Top::write(String &out, StringView eol, size_t width, size_t rows) {
                size_t     lines = 0;
                const auto put = [&](StringView line) -> Error {
                        if (rows != 0 && lines >= rows) return Error::Ok;
                        size_t n = line.size();
                        if (width != 0 && n > width) {
                                n = width;
                                while (n > 0 && (static_cast<uint8_t>(line[n]) & 0xc0u) == 0x80u)
                                        --n; // a whole character
                        }
                        ++lines;
                        if (out.append(line.left(n)).isError() || out.append(eol).isError()) return Error::NoMem;
                        return Error::Ok;
                };
                char line[TopView::LineBytes];

                // The header.
                size_t open = 0;
                {
                        Stat::ProblemLog::Episode episodes[16];
                        uint64_t                  after = 0;
                        for (;;) {
                                const size_t n = _source.problems(after, Span<Stat::ProblemLog::Episode>(episodes, 16));
                                for (size_t k = 0; k < n; ++k) {
                                        after = episodes[k].changed;
                                        open += episodes[k].isOpen() ? 1 : 0;
                                }
                                if (n < 16) break;
                        }
                }
                const int64_t uptime = _monitor.valueOf("system.uptime_us");
                if (Error e = put(TopView::summaryLine(line, uint64_t(uptime > 0 ? uptime : 0), _procs.size(),
                                                       _tasks.size(), open));
                    e.isError())
                        return e;
                uint32_t shares[MaxCores] = {};
                size_t   cores = 0;
                for (; cores < MaxCores; ++cores) {
                        char name[32];
                        std::snprintf(name, sizeof(name), "cpu.core%u.busy_us", unsigned(cores));
                        const size_t i = _monitor.find(name);
                        if (i == Monitor::NotFound) break;
                        const int64_t r = _monitor.rate(i);
                        shares[cores] = r == Unavailable ? 0 : TopView::share(uint64_t(r > 0 ? r : 0), 0, 1000000);
                }
                if (Error e = put(TopView::coresLine(line, shares, cores)); e.isError()) return e;
                static constexpr const char *Spaces[] = {"internal", "psram"};
                for (const char *space : Spaces) {
                        char       name[48];
                        const auto value = [&](const char *field) {
                                std::snprintf(name, sizeof(name), "mem.%s.%s", space, field);
                                const int64_t v = _monitor.valueOf(name);
                                return uint64_t(v == Unavailable || v < 0 ? 0 : v);
                        };
                        const uint64_t total = value("total");
                        if (Error e = put(TopView::memoryLine(line, space, value("used"), total, value("largest"),
                                                              value("min_free")));
                            e.isError())
                                return e;
                }
                headline(line);
                if (Error e = put(StringView(line)); e.isError()) return e;
                if (Error e = put(StringView()); e.isError()) return e;
                if (Error e = put(TopView::headingLine(line)); e.isError()) return e;

                // The rows: the processes and the kernel, the busiest first.
                List<Row> rows_(MemSpace::Psram);
                List<Row> taskRows(MemSpace::Psram);
                Row       kernel;
                kernel.pid = 0;
                kernel.name = "kernel";
                kernel.threads = 0;
                uint64_t kernelRan = 0;
                for (const Source::Task &t : _tasks) {
                        const uint64_t ran = since(_seenTasks, int64_t(t.number), t.cpuUs);
                        if (t.pid == 0 && !isIdle(t)) {
                                kernel.cpuUs += t.cpuUs;
                                kernelRan += ran;
                                ++kernel.threads;
                        }
                        if (!_showTasks || (t.pid == 0 && isIdle(t))) continue;
                        Row r;
                        r.pid = t.pid;
                        r.task = true;
                        r.name = t.name;
                        r.share = TopView::share(ran, 0, _elapsedUs);
                        r.cpuUs = t.cpuUs;
                        r.state = taskState(t.state);
                        if (taskRows.append(r).isError()) return Error::NoMem;
                }
                kernel.share = TopView::share(kernelRan, 0, _elapsedUs);
                if (rows_.append(kernel).isError()) return Error::NoMem;
                char   names[32][32]; // the processes' names, while the rows point at them
                size_t named = 0;
                for (const Source::Process &p : _procs) {
                        if (named == 32) break;
                        Row r;
                        r.pid = p.pid;
                        const StringView program = programName(p.command);
                        std::snprintf(names[named], sizeof(names[named]), "%.*s", int(program.size()), program.data());
                        r.name = names[named++];
                        r.share = TopView::share(since(_seenProcs, p.pid, p.cpuUs), 0, _elapsedUs);
                        r.cpuUs = p.cpuUs;
                        r.memory = p.memory;
                        r.threads = int32_t(p.threads);
                        r.state = processState(p.state);
                        if (rows_.append(r).isError()) return Error::NoMem;
                }
                std::sort(rows_.begin(), rows_.end(), TopView::isBefore);
                std::sort(taskRows.begin(), taskRows.end(), TopView::isBefore);
                for (const Row &r : rows_) {
                        if (Error e = put(TopView::rowLine(line, r)); e.isError()) return e;
                        for (const Row &t : taskRows)
                                if (t.pid == r.pid)
                                        if (Error e = put(TopView::rowLine(line, t)); e.isError()) return e;
                }
                return Error::Ok;
        }

        int runTop(CommandContext &ctx) {
                const Args args = ctx.args();
                uint32_t   intervalMs = 1000;
                long       count = 0;
                bool       showTasks = false;
                for (size_t i = 1; i < args.size(); ++i) {
                        const StringView a(args[i]);
                        if (a == "-T") {
                                showTasks = true;
                        } else if (a == "-d" && i + 1 < args.size()) {
                                if (!parseSeconds(args[++i], intervalMs)) return ctx.usage();
                        } else if (a == "-n" && i + 1 < args.size()) {
                                char *end = nullptr;
                                count = std::strtol(args[++i], &end, 10);
                                if (end == args[i] || *end != '\0' || count <= 0) return ctx.usage();
                        } else {
                                return ctx.usage();
                        }
                }
                FileHandle *out = ctx.fds().handle(FdTable::Stdout);
                FileHandle *in = ctx.fds().handle(FdTable::Stdin);
                // A screen on a terminal that understands ANSI, unless a count asks for batch lines.
                Console *console =
                        out != nullptr && out->stream().isTerminal() ? static_cast<Console *>(&out->stream()) : nullptr;
                const bool screen = count == 0 && console != nullptr && (console->capabilities() & Console::Ansi) != 0;
                const bool keys = screen && in != nullptr && in->canRead() && in->stream().isTerminal();

                Top top(ctx, showTasks);
                top.start();
                Interrupt &interrupt = ctx.shell().interrupt();
                // A wait: keys read with the time left as the read's timeout on a screen (q ends it), else
                // the shell's interruptible sleep. False when it should end.
                const auto wait = [&](uint32_t ms) {
                        if (!keys) return interrupt.sleep(ms).isOk();
                        const int64_t until = int64_t(Clock::monotonicNs()) + int64_t(ms) * 1000000;
                        for (;;) {
                                const int64_t left = (until - int64_t(Clock::monotonicNs())) / 1000000;
                                if (left <= 0) return true;
                                uint8_t b = 0;
                                auto [n, err] = in->read(WritableByteSpan(&b, 1), unsigned(left));
                                if (err == Error::Timeout || err == Error::WouldBlock) return true;
                                if (err.isError()) return false; // interrupted (Ctrl-C), or the terminal went
                                if (n == 1 && (b == 'q' || b == 'Q')) return false;
                        }
                };
                if (screen) ctx.print("\x1b[?25l\x1b[2J");
                int    status = Shell::ExitSuccess;
                String frame(MemSpace::Psram);
                bool   go = wait(FirstFrameMs);
                for (long shown = 0; go && !ctx.isInterrupted(); ++shown) {
                        if (count != 0 && shown == count) break;
                        frame.clear();
                        Error err = top.read();
                        if (err.isOk() && screen) {
                                const Console::Size size = console->size();
                                (void)frame.append("\x1b[H");
                                err = top.write(frame, "\x1b[K\n", size.columns > 1 ? size.columns - 1 : 0,
                                                size.rows > 1 ? size.rows - 1 : 0);
                                if (err.isOk()) err = frame.append("\x1b[J");
                        } else if (err.isOk()) {
                                if (shown > 0) (void)frame.append("\n");
                                err = top.write(frame, "\n", 0, 0);
                        }
                        if (err.isError()) {
                                ctx.eprint("top: out of memory\n");
                                status = Shell::ExitFailure;
                                break;
                        }
                        ctx.print(frame.view());
                        if (count != 0 && shown + 1 == count) break;
                        go = wait(intervalMs);
                }
                if (screen) ctx.print("\x1b[?25h");
                if (status == Shell::ExitSuccess && ctx.isInterrupted()) status = Shell::ExitInterrupted;
                return status;
        }

} // namespace

BMDS_SHELL_COMMAND(top, runTop, "[-d SECONDS] [-n COUNT] [-T]",
                   "Show the load, memory and processes by CPU, every few seconds (q ends it); -T their tasks")

BMDS_NAMESPACE_END
