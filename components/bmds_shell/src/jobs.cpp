/**
 * @file      jobs.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Jobs: the job table and `jobs`, `fg`, `bg`, `wait`, `disown`
 * (devplan/processes.md §3.8.1, §3.7.1).
 */

#include <cstdlib>
#include <cstring>
#include <bmds/console.h>
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

        // The stop signals' names, for `Stopped(SIGTTIN)` (SIGTSTP is plain `Stopped`).
        constexpr int SigHup = 1;
        constexpr int SigCont = 18;
        constexpr int SigStop = 19;
        constexpr int SigTstp = 20;
        constexpr int SigTtin = 21;
        constexpr int SigTtou = 22;

        const char *stopSignalName(int sig) {
                switch (sig) {
                        case SigStop: return "SIGSTOP";
                        case SigTtin: return "SIGTTIN";
                        case SigTtou: return "SIGTTOU";
                        default: return nullptr;
                }
        }

        // A wait status that says the child stopped (Linux's layout), and the signal.
        bool isStopStatus(int32_t waitStatus) {
                return (waitStatus & 0xff) == 0x7f;
        }

        constexpr int32_t ContinuedStatus = 0xffff;

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
                // Every change since the last look: stops and continues, then perhaps the end.
                while (!job.ended) {
                        int32_t               status = 0;
                        const Result<int32_t> r = _process->waitChild(
                                job.pid, ProcessControl::WaitStopped | ProcessControl::WaitContinued, status);
                        if (r.isError()) {
                                setEnded(job, 127 << 8); // gone: not ours to take any more
                        } else if (r.value() != job.pid) {
                                break;
                        } else if (isStopStatus(status)) {
                                job.stopped = job.notify = true;
                                job.stopSignal = (status >> 8) & 0xff;
                        } else if (status == ContinuedStatus) {
                                job.stopped = job.notify = false;
                        } else {
                                setEnded(job, status);
                        }
                }
        }
        dropOldEnded();
        return;
}

// A script's shell never reports its ended jobs: each would stay until it exits (52 bytes a
// background job, found as a slow leak). The newest MaxEnded are kept for `wait pid`.
void JobTable::dropOldEnded() {
        size_t ended = 0;
        for (const Job &job : _jobs) ended += job.ended ? 1 : 0;
        for (size_t i = 0; i < _jobs.size() && ended > MaxEnded;) {
                if (!_jobs[i].ended) {
                        ++i;
                        continue;
                }
                _jobs.removeAt(i); // ended: its process is taken already
                --ended;
        }
        return;
}

Error JobTable::wait(Job &job) {
        if (job.ended || _process == nullptr) return Error::Ok;
        int32_t               status = 0;
        const Result<int32_t> r = _process->waitChild(job.pid, ProcessControl::WaitBlock, status);
        if (r.isError() && r.error() == Error::Interrupted) return Error::Interrupted;
        setEnded(job, r.isOk() ? status : 127 << 8);
        return Error::Ok;
}

Error JobTable::foreground(Job &job) {
        if (job.ended || _process == nullptr) return Error::Ok;
        // Its group is its pid (it leads it).
        (void)_process->setForeground(job.pid);
        if (job.stopped) (void)_process->signal(-job.pid, SigCont);
        job.stopped = job.notify = false;
        Error err = Error::Ok;
        for (;;) {
                int32_t               status = 0;
                const Result<int32_t> r =
                        _process->waitChild(job.pid, ProcessControl::WaitBlock | ProcessControl::WaitStopped, status);
                if (r.isError() && r.error() == Error::Interrupted) {
                        if (!_process->isEnding()) continue;
                        err = Error::Interrupted;
                } else if (r.isError()) {
                        setEnded(job, 127 << 8);
                } else if (isStopStatus(status)) {
                        job.stopped = true;
                        job.stopSignal = (status >> 8) & 0xff;
                } else {
                        setEnded(job, status);
                }
                break;
        }
        (void)_process->setForeground(0);
        return err;
}

bool JobTable::continueJob(Job &job) {
        if (job.ended || !job.stopped || _process == nullptr) return false;
        (void)_process->signal(-job.pid, SigCont);
        job.stopped = job.notify = false;
        return true;
}

void JobTable::hangUp() {
        if (_process == nullptr) return;
        for (const Job &job : _jobs) {
                if (job.ended || job.keepOnHangUp) continue;
                (void)_process->signal(-job.pid, SigHup);
                if (job.stopped) (void)_process->signal(-job.pid, SigCont);
        }
        return;
}

bool JobTable::hasStopped() const {
        for (const Job &job : _jobs) {
                if (job.stopped && !job.ended) return true;
        }
        return false;
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
        if (!job.ended && job.stopped) {
                const char *name = stopSignalName(job.stopSignal);
                if (name != nullptr) {
                        (void)out.appendf("Stopped(%s)", name);
                } else {
                        (void)out.append("Stopped");
                }
        } else if (!job.ended) {
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
                if (!job.ended && !job.notify) {
                        ++i;
                        continue;
                }
                job.notify = false;
                line.clear();
                jobs.appendLine(job, false, line);
                (void)line.append('\n');
                FileHandle *err = _fds.handle(FdTable::Stderr);
                if (err != nullptr) (void)err->write(line, WaitForever);
                if (job.ended) {
                        jobs.remove(job);
                } else {
                        ++i;
                }
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
        if (!job.ended && !job.stopped) (void)out.append(" &");
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
                        job.notify = false; // reported here
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

        // fg, bg and disown take a bare number as a job number, as bash does (`bg 1` is `bg %1`);
        // wait and kill take it as a pid.
        JobTable::Match findJob(JobTable &table, const char *spec, Job *&job) {
                long n = 0;
                if (!parseNumber(StringView(spec), n)) return table.find(StringView(spec), job);
                String numbered;
                (void)numbered.appendf("%%%ld", n);
                return table.find(numbered, job);
        }

        // The job @p spec names (none: the current one) for fg and bg; reported if there is none.
        Job *jobFor(CommandContext &ctx, const char *command, const char *spec) {
                JobTable &table = ctx.executor().state().jobs;
                table.poll();
                Job *job = nullptr;
                if (spec == nullptr) {
                        job = table.current();
                        if (job == nullptr) ctx.eprintf("%s: current: no such job\n", command);
                        return job;
                }
                const JobTable::Match m = findJob(table, spec, job);
                if (m == JobTable::Match::Ambiguous) {
                        ctx.eprintf("%s: %s: ambiguous job spec\n", command, spec);
                } else if (m != JobTable::Match::Found || job->ended) {
                        ctx.eprintf("%s: %s: no such job\n", command, spec);
                        job = nullptr;
                }
                return job;
        }

        // fg [%N]
        int fg(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() > 2) return ctx.usage();
                if (!ctx.shell().hasJobControl()) {
                        ctx.eprint("fg: no job control\n");
                        return Shell::ExitFailure;
                }
                JobTable &table = ctx.executor().state().jobs;
                Job      *job = jobFor(ctx, "fg", args.size() == 2 ? args[1] : nullptr);
                if (job == nullptr) return Shell::ExitFailure;
                // As bash: the command, then it runs in the foreground until it ends or stops. The
                // shell's terminal modes come back after it (the job may change them).
                ctx.printf("%s\n", job->command.cStr());
                const Console::TermMode mode = ctx.shell().console().mode();
                const Error             err = table.foreground(*job);
                ctx.shell().console().setMode(mode);
                if (err.isError()) return Shell::ExitInterrupted;
                if (job->stopped) {
                        String line;
                        (void)line.append(job->stopSignal == SigTstp ? "^Z\n" : "\n");
                        table.appendLine(*job, false, line);
                        (void)line.append('\n');
                        ctx.eprint(line);
                        return 128 + job->stopSignal;
                }
                const int status = job->status;
                table.remove(*job);
                // Ended by Ctrl-C: the shell's commands stop too, as for a program run here.
                if (status == Shell::ExitInterrupted) ctx.shell().interrupt().raise();
                return status;
        }

        // bg [%N...]
        int bg(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (!ctx.shell().hasJobControl()) {
                        ctx.eprint("bg: no job control\n");
                        return Shell::ExitFailure;
                }
                JobTable &table = ctx.executor().state().jobs;
                int       status = Shell::ExitSuccess;
                for (size_t i = 1; i == 1 || i < args.size(); ++i) {
                        Job *job = jobFor(ctx, "bg", i < args.size() ? args[i] : nullptr);
                        if (job == nullptr) {
                                status = Shell::ExitFailure;
                                continue;
                        }
                        if (!table.continueJob(*job)) {
                                ctx.eprintf("bg: job %d already in background\n", job->number);
                                continue;
                        }
                        // As bash: "[1]+ sleep 10 &".
                        ctx.printf("[%d]%c %s &\n", job->number, table.mark(*job), job->command.cStr());
                }
                return status;
        }

        // Disowns @p job; a stopped one is hung up (nobody could continue it), as bash warns.
        void disownJob(CommandContext &ctx, JobTable &table, Job &job) {
                if (job.stopped && !job.ended)
                        ctx.eprintf("disown: warning: deleting stopped job %d with process group %ld\n", job.number,
                                    static_cast<long>(job.pid));
                table.remove(job);
                return;
        }

        // disown [-h] [-a | %N...]: forgets jobs; with -h keeps them, but not to be hung up.
        int disown(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                JobTable               &table = ctx.executor().state().jobs;
                size_t                  first = 1;
                bool                    keep = false, all = false;
                for (; first < args.size() && args[first][0] == '-'; ++first) {
                        if (std::strcmp(args[first], "-h") == 0) {
                                keep = true;
                        } else if (std::strcmp(args[first], "-a") == 0) {
                                all = true;
                        } else {
                                return ctx.usage();
                        }
                }
                auto act = [&](Job &job) {
                        if (keep) {
                                job.keepOnHangUp = true;
                        } else {
                                disownJob(ctx, table, job);
                        }
                };
                if (all) {
                        if (keep) {
                                for (Job &job : table.jobs()) act(job);
                        } else {
                                while (!table.isEmpty()) act(table.jobs()[0]);
                        }
                        return Shell::ExitSuccess;
                }
                if (first == args.size()) {
                        Job *job = table.current();
                        if (job == nullptr) {
                                ctx.eprintf("disown: current: no such job\n");
                                return Shell::ExitFailure;
                        }
                        act(*job);
                        return Shell::ExitSuccess;
                }
                int status = Shell::ExitSuccess;
                for (size_t i = first; i < args.size(); ++i) {
                        Job *job = nullptr;
                        if (findJob(table, args[i], job) != JobTable::Match::Found) {
                                ctx.eprintf("disown: %s: no such job\n", args[i]);
                                status = Shell::ExitFailure;
                                continue;
                        }
                        act(*job);
                }
                return status;
        }

} // namespace LangBuiltins

BMDS_SHELL_BUILTIN(bg, LangBuiltins::bg, "[%N...]", "Continue stopped jobs in the background")
BMDS_SHELL_BUILTIN(disown, LangBuiltins::disown, "[-h] [-a | %N...]",
                   "Forget jobs: they go on, and end on their own (-h: keep them, but not hung up with the shell)")
BMDS_SHELL_BUILTIN(fg, LangBuiltins::fg, "[%N]", "Continue a job in the foreground and wait for it")
BMDS_SHELL_BUILTIN(jobs, LangBuiltins::jobs, "[-l | -p]", "List jobs (-l: with pids, -p: only pids)")
BMDS_SHELL_BUILTIN(wait, LangBuiltins::wait, "[%N | pid]...",
                   "Wait for jobs to end (all of them without arguments); the last one's status")

BMDS_NAMESPACE_END
