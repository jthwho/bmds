/**
 * @file      jobs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Background jobs: the job table and `jobs`, `wait`, `disown`
 * (devplan/processes.md §3.8.1).
 */

#include <cstdlib>
#include <cstring>
#include <bmds/executor.h>
#include <bmds/langbuiltins.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // What bash says of a job a signal ended (strsignal's words), by Linux's numbers.
        constexpr const char *SignalWords[] = {
                nullptr,
                "Hangup",
                "Interrupt",
                "Quit",
                "Illegal instruction",
                "Trace/breakpoint trap",
                "Aborted",
                "Bus error",
                "Floating point exception",
                "Killed",
                "User defined signal 1",
                "Segmentation fault",
                "User defined signal 2",
                "Broken pipe",
                "Alarm clock",
                "Terminated",
        };
        constexpr int SignalWordCount = static_cast<int>(sizeof(SignalWords) / sizeof(SignalWords[0]));

        // A wait status (Linux's layout) as the job's status and signal.
        void setEnded(Job &job, int32_t waitStatus) {
                job.ended = true;
                job.signal = waitStatus & 0x7f;
                job.status = job.signal != 0 ? 128 + job.signal : (waitStatus >> 8) & 0xff;
                return;
        }

        bool parseNumber(StringView text, long &out) {
                if (text.isEmpty() || text.size() > 10) return false;
                long n = 0;
                for (char c : text) {
                        if (c < '0' || c > '9') return false;
                        n = n * 10 + (c - '0');
                }
                out = n;
                return true;
        }

} // namespace

// --- JobTable ------------------------------------------------------------------

JobTable::~JobTable() {
        disownAll();
}

JobTable::JobTable(JobTable &&other) : _process(other._process), _jobs(std::move(other._jobs)) {
        other._process = nullptr;
        other._jobs = List<Job>();
}

JobTable &JobTable::operator=(JobTable &&other) {
        if (this == &other) return *this;
        disownAll();
        _process = other._process;
        _jobs = std::move(other._jobs);
        other._process = nullptr;
        other._jobs = List<Job>();
        return *this;
}

void JobTable::disownAll() {
        for (const Job &job : _jobs) {
                if (!job.ended && _process != nullptr) _process->disownChild(job.pid);
        }
        _jobs.clear();
        return;
}

Job *JobTable::add(ProcessControl &process, int32_t pid, StringView command) {
        _process = &process;
        // As bash numbers them: one past the highest.
        int number = 1;
        for (const Job &j : _jobs) {
                if (j.number >= number) number = j.number + 1;
        }
        Job job;
        job.number = number;
        job.pid = pid;
        Error err = job.command.assign(command);
        if (err.isOk()) err = _jobs.append(std::move(job));
        if (err.isError()) {
                process.disownChild(pid);
                return nullptr;
        }
        return &_jobs.back();
}

void JobTable::poll() {
        if (_process == nullptr) return;
        for (Job &job : _jobs) {
                if (job.ended) continue;
                int32_t               status = 0;
                const Result<int32_t> r = _process->waitChild(job.pid, false, status);
                if (r.isOk() && r.value() == job.pid) {
                        setEnded(job, status);
                } else if (r.isError()) {
                        setEnded(job, 127 << 8); // gone: not ours to take any more
                }
        }
        return;
}

Error JobTable::wait(Job &job) {
        if (job.ended || _process == nullptr) return Error::Ok;
        int32_t               status = 0;
        const Result<int32_t> r = _process->waitChild(job.pid, true, status);
        if (r.isError() && r.error() == Error::Interrupted) return Error::Interrupted;
        setEnded(job, r.isOk() ? status : 127 << 8);
        return Error::Ok;
}

void JobTable::remove(Job &job) {
        for (size_t i = 0; i < _jobs.size(); ++i) {
                if (&_jobs[i] != &job) continue;
                if (!job.ended && _process != nullptr) _process->disownChild(job.pid);
                _jobs.removeAt(i);
                return;
        }
        return;
}

Job *JobTable::current() {
        return _jobs.isEmpty() ? nullptr : &_jobs.back();
}

Job *JobTable::previous() {
        return _jobs.size() < 2 ? nullptr : &_jobs[_jobs.size() - 2];
}

char JobTable::mark(const Job &job) {
        if (&job == current()) return '+';
        if (&job == previous()) return '-';
        return ' ';
}

JobTable::Match JobTable::find(StringView spec, Job *&out) {
        out = nullptr;
        long n = 0;
        if (!spec.startsWith('%')) {
                // A pid.
                if (!parseNumber(spec, n)) return Match::None;
                for (Job &job : _jobs) {
                        if (job.pid == n) out = &job;
                }
                return out != nullptr ? Match::Found : Match::None;
        }
        const StringView rest = spec.mid(1);
        if (rest.isEmpty() || rest == "%" || rest == "+") {
                out = current();
        } else if (rest == "-") {
                out = previous();
        } else if (parseNumber(rest, n)) {
                for (Job &job : _jobs) {
                        if (job.number == n) out = &job;
                }
        } else {
                // %name: the command starts with it; %?text: contains it.
                const bool       contains = rest.startsWith('?');
                const StringView text = contains ? rest.mid(1) : rest;
                for (Job &job : _jobs) {
                        const bool match =
                                contains ? job.command.view().contains(text) : job.command.view().startsWith(text);
                        if (!match) continue;
                        if (out != nullptr) return Match::Ambiguous;
                        out = &job;
                }
        }
        return out != nullptr ? Match::Found : Match::None;
}

void JobTable::appendState(const Job &job, String &out) {
        if (!job.ended) {
                (void)out.append("Running");
        } else if (job.signal > 0 && job.signal < SignalWordCount) {
                (void)out.append(SignalWords[job.signal]);
        } else if (job.signal != 0) {
                (void)out.appendf("Signal %d", job.signal);
        } else if (job.status == 0) {
                (void)out.append("Done");
        } else {
                (void)out.appendf("Exit %d", job.status);
        }
        return;
}

void Shell::reportJobs() {
        JobTable &jobs = _state.jobs;
        if (jobs.isEmpty()) return;
        jobs.poll();
        String line;
        for (size_t i = 0; i < jobs.jobs().size();) {
                Job &job = jobs.jobs()[i];
                if (!job.ended) {
                        ++i;
                        continue;
                }
                line.clear();
                jobs.appendLine(job, false, line);
                (void)line.append('\n');
                FileHandle *err = _fds.handle(FdTable::Stderr);
                if (err != nullptr) (void)err->write(line, WaitForever);
                jobs.remove(job);
        }
        return;
}

void JobTable::appendLine(const Job &job, bool withPid, String &out) {
        // As bash: "[1]+  Running                 sleep 10 &", "[1]+ 123 Running ..." with the pid.
        (void)out.appendf("[%d]%c ", job.number, mark(job));
        if (withPid) {
                (void)out.appendf("%ld ", static_cast<long>(job.pid));
        } else {
                (void)out.append(' ');
        }
        const size_t at = out.size();
        appendState(job, out);
        while (out.size() < at + 24) (void)out.append(' ');
        (void)out.append(job.command);
        if (!job.ended) (void)out.append(" &");
        return;
}

// --- The commands ----------------------------------------------------------------

namespace LangBuiltins {

        // jobs [-l | -p]
        int jobs(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                bool                    pids = false, withPid = false;
                if (args.size() > 2) return ctx.usage();
                if (args.size() == 2) {
                        if (std::strcmp(args[1], "-p") == 0) {
                                pids = true;
                        } else if (std::strcmp(args[1], "-l") == 0) {
                                withPid = true;
                        } else {
                                return ctx.usage();
                        }
                }
                JobTable &table = ctx.executor().state().jobs;
                table.poll();
                String line;
                for (Job &job : table.jobs()) {
                        if (pids) {
                                ctx.printf("%ld\n", static_cast<long>(job.pid));
                                continue;
                        }
                        line.clear();
                        table.appendLine(job, withPid, line);
                        (void)line.append('\n');
                        ctx.print(line);
                }
                // Ended jobs are reported once.
                for (size_t i = 0; i < table.jobs().size();) {
                        if (table.jobs()[i].ended) {
                                table.remove(table.jobs()[i]);
                        } else {
                                ++i;
                        }
                }
                return Shell::ExitSuccess;
        }

        // wait [%N | pid]...
        int wait(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                JobTable               &table = ctx.executor().state().jobs;
                if (args.size() == 1) {
                        // All of them; the status is 0.
                        for (Job &job : table.jobs()) {
                                if (table.wait(job) == Error::Interrupted) return Shell::ExitInterrupted;
                        }
                        while (!table.isEmpty()) table.remove(table.jobs()[0]);
                        return Shell::ExitSuccess;
                }
                int status = Shell::ExitSuccess;
                for (size_t i = 1; i < args.size(); ++i) {
                        Job                  *job = nullptr;
                        const StringView      spec(args[i]);
                        const JobTable::Match m = table.find(spec, job);
                        if (m != JobTable::Match::Found) {
                                if (m == JobTable::Match::Ambiguous)
                                        ctx.eprintf("wait: %s: ambiguous job spec\n", args[i]);
                                else if (spec.startsWith('%'))
                                        ctx.eprintf("wait: %s: no such job\n", args[i]);
                                else
                                        ctx.eprintf("wait: pid %s is not a child of this shell\n", args[i]);
                                status = Shell::ExitNotFound;
                                continue;
                        }
                        if (table.wait(*job) == Error::Interrupted) return Shell::ExitInterrupted;
                        status = job->status;
                        table.remove(*job);
                }
                return status;
        }

        // disown [-a | %N...]
        int disown(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                JobTable               &table = ctx.executor().state().jobs;
                if (args.size() == 2 && std::strcmp(args[1], "-a") == 0) {
                        while (!table.isEmpty()) table.remove(table.jobs()[0]);
                        return Shell::ExitSuccess;
                }
                if (args.size() == 1) {
                        Job *job = table.current();
                        if (job == nullptr) {
                                ctx.eprintf("disown: current: no such job\n");
                                return Shell::ExitFailure;
                        }
                        table.remove(*job);
                        return Shell::ExitSuccess;
                }
                int status = Shell::ExitSuccess;
                for (size_t i = 1; i < args.size(); ++i) {
                        Job *job = nullptr;
                        if (table.find(StringView(args[i]), job) != JobTable::Match::Found) {
                                ctx.eprintf("disown: %s: no such job\n", args[i]);
                                status = Shell::ExitFailure;
                                continue;
                        }
                        table.remove(*job);
                }
                return status;
        }

} // namespace LangBuiltins

BMDS_SHELL_BUILTIN(disown, LangBuiltins::disown, "[-a | %N...]", "Forget jobs: they go on, and end on their own")
BMDS_SHELL_BUILTIN(jobs, LangBuiltins::jobs, "[-l | -p]", "List background jobs (-l: with pids, -p: only pids)")
BMDS_SHELL_BUILTIN(wait, LangBuiltins::wait, "[%N | pid]...",
                   "Wait for jobs to end (all of them without arguments); the last one's status")

BMDS_NAMESPACE_END
