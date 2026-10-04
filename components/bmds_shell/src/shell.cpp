/**
 * @file      shell.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <bmds/assert.h>
#include <bmds/console.h>
#include <bmds/memorystream.h>
#include <bmds/pipe.h>
#include <bmds/pipeline.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Prompt = "bmds> ";

        constexpr char CtrlC = 0x03;

        WorkerPool::Config workerConfig() {
                WorkerPool::Config config;
                config.thread.name = "pipe";
                config.thread.stackBytes = Shell::WorkerStackBytes;
                config.thread.priority = Shell::WorkerPriority;
                config.thread.stackSpace = MemSpace::Psram;
                config.maxWorkers = Shell::MaxStages - 1;
                config.maxIdle = 2;
                return config;
        }

        // One pipeline stage being run.
        struct StageRun {
                        Shell              *shell = nullptr;
                        const Command      *command = nullptr; // null: nothing to run
                        Span<char *const>   args;
                        FdTable             fds;
                        int                 status = Shell::ExitSuccess;
                        WorkerPool::Worker *worker = nullptr;
        };

        void runStage(void *arg) {
                StageRun      *run = static_cast<StageRun *>(arg);
                CommandContext ctx(*run->shell, *run->command, run->args, run->fds);
                run->status = run->command->run(ctx);
                // Close this stage's ends now: that is what gives the next stage
                // its EOF and the previous one its broken pipe.
                run->fds.closeAll();
                return;
        }

        // Opens the file a redirection names. Without a filesystem (Phase 2),
        // that is only /dev/null.
        Result<FileHandle::Ptr> openPath(const char *path, uint8_t mode) {
                if (StringView(path) == "/dev/null") return FileHandle::open(NullStream::instance(), mode);
                return makeError<FileHandle::Ptr>(Error::NotExist);
        }

} // namespace

Shell::Shell(Console &console, Span<const Command> commands)
    : _console(console), _commands(commands), _workers(workerConfig()), _editor(console) {
        // findCommand() binary-searches, so the table must be sorted and unique.
        for (size_t i = 1; i < _commands.size(); ++i) {
                BMDS_ASSERT(StringView(_commands[i - 1].name) < StringView(_commands[i].name));
        }
}

Error Shell::init() {
        // One open of the console for all three, as a terminal's are.
        auto [handle, err] = FileHandle::open(_console, FileHandle::ReadWrite);
        if (err.isError()) return err;
        (void)_fds.set(FdTable::Stdin, handle);
        (void)_fds.set(FdTable::Stdout, handle);
        (void)_fds.set(FdTable::Stderr, handle);
        return Error::Ok;
}

const Command *Shell::findCommand(StringView name) const {
        size_t lo = 0;
        size_t hi = _commands.size();
        while (lo < hi) {
                const size_t mid = lo + (hi - lo) / 2;
                const auto   cmp = name <=> StringView(_commands[mid].name);
                if (cmp == 0) return &_commands[mid];
                if (cmp < 0) {
                        hi = mid;
                } else {
                        lo = mid + 1;
                }
        }
        return nullptr;
}

void Shell::start() {
        _editor.start(Prompt);
        return;
}

void Shell::feed(char c) {
        switch (_editor.feed(c)) {
                case LineEditor::Event::Line:
                        (void)execute(_editor.lineData());
                        start();
                        break;
                case LineEditor::Event::Cancel: start(); break;
                case LineEditor::Event::Eof: // nothing to end: the console stays
                case LineEditor::Event::None: break;
        }
        return;
}

int Shell::execute(char *line) {
        Pipeline   pipeline;
        ParseError parseError;
        Error      err = parsePipeline(line, pipeline, parseError);
        if (err == Error::NoMem) {
                report("out of memory\n");
                _lastExitCode = ExitFailure;
                return _lastExitCode;
        }
        if (err.isError()) {
                if (parseError.token != nullptr) {
                        report("%s `%s'\n", parseError.message, parseError.token);
                } else {
                        report("%s\n", parseError.message);
                }
                _lastExitCode = ExitUsage;
                return _lastExitCode;
        }
        if (pipeline.stages.isEmpty()) return _lastExitCode;

        // A Ctrl-C typed after the line but before the command started is for
        // the command (as with a tty); one typed earlier was taken by read().
        _lastExitCode = runPipeline(pipeline);
        Interrupt &interrupt = _console.interrupt();
        if (interrupt.isRaised()) {
                // The console swallowed the ^C, so echo it here, as a terminal would.
                (void)_console.write(StringView("^C\n"), WaitForever);
                _lastExitCode = ExitInterrupted;
                interrupt.clear();
        }
        return _lastExitCode;
}

int Shell::runPipeline(Pipeline &pipeline) {
        const size_t count = pipeline.stages.size();
        if (count > MaxStages) {
                report("too many pipeline stages (at most %u)\n", static_cast<unsigned>(MaxStages));
                return ExitFailure;
        }
        List<StageRun> runs;
        // Reserved up front: workers hold pointers to their StageRun.
        if (runs.reserve(count).isError()) {
                report("out of memory\n");
                return ExitFailure;
        }
        for (size_t i = 0; i < count; ++i) {
                StageRun run;
                run.shell = this;
                run.args = pipeline.stages[i].args.span();
                run.fds = _fds;
                (void)runs.append(std::move(run)); // reserved: can't fail
        }

        // Connect neighbors. Only the stages hold the ends, so each end closes
        // when its stage finishes.
        for (size_t i = 0; i + 1 < count; ++i) {
                auto [ends, err] = Pipe::create(&_console.interrupt());
                if (err.isError()) {
                        report("cannot create pipe: %s\n", err.desc());
                        return ExitFailure;
                }
                (void)runs[i].fds.set(FdTable::Stdout, std::move(ends.write));
                (void)runs[i + 1].fds.set(FdTable::Stdin, std::move(ends.read));
        }

        // Redirections and command lookup. A stage that fails here doesn't
        // run; closing its descriptors lets its neighbors finish.
        for (size_t i = 0; i < count; ++i) {
                StageRun &run = runs[i];
                Error     err;
                for (const Redirect &r : pipeline.stages[i].redirects) {
                        switch (r.kind) {
                                case Redirect::Kind::Input:
                                case Redirect::Kind::Output:
                                case Redirect::Kind::Append: {
                                        const uint8_t mode =
                                                r.kind == Redirect::Kind::Input ? FileHandle::Read : FileHandle::Write;
                                        auto [handle, openErr] = openPath(r.path, mode);
                                        err = openErr;
                                        if (err.isOk()) err = run.fds.set(r.fd, std::move(handle));
                                        if (err.isError()) {
                                                report("%s: %s\n", err == Error::NotExist ? r.path : "redirect",
                                                       err == Error::NotExist ? "No such file or directory"
                                                                              : err.desc());
                                        }
                                        break;
                                }
                                case Redirect::Kind::Duplicate:
                                        err = run.fds.dup2(r.sourceFd, r.fd);
                                        if (err.isError()) {
                                                const bool badTarget = r.fd < 0 || r.fd >= FdTable::MaxFds;
                                                report("%d: Bad file descriptor\n", badTarget ? r.fd : r.sourceFd);
                                        }
                                        break;
                                case Redirect::Kind::Close:
                                        // Closing a closed descriptor is fine; one out of range isn't.
                                        if (r.fd < 0 || r.fd >= FdTable::MaxFds) {
                                                err = Error::BadDescriptor;
                                                report("%d: Bad file descriptor\n", r.fd);
                                        } else {
                                                (void)run.fds.close(r.fd);
                                        }
                                        break;
                        }
                        if (err.isError()) break;
                }
                if (err.isError()) {
                        run.status = ExitFailure;
                } else if (!run.args.isEmpty()) {
                        run.command = findCommand(run.args[0]);
                        if (run.command == nullptr) {
                                report("%s: command not found\n", run.args[0]);
                                run.status = ExitNotFound;
                        }
                }
                if (run.command == nullptr) run.fds.closeAll();
        }

        // Start the earlier stages on workers, run the last one here.
        for (size_t i = 0; i + 1 < count; ++i) {
                StageRun &run = runs[i];
                if (run.command == nullptr) continue;
                auto [worker, err] = _workers.start(runStage, &run);
                if (err.isError()) {
                        report("%s: cannot start: %s\n", run.args[0], err.desc());
                        run.status = ExitFailure;
                        run.fds.closeAll();
                        continue;
                }
                run.worker = worker;
        }
        StageRun &last = runs.back();
        if (last.command != nullptr) runStage(&last);
        for (StageRun &run : runs) {
                if (run.worker != nullptr) _workers.wait(run.worker);
        }
        return last.status;
}

void Shell::run() {
        start();
        for (;;) {
                uint8_t buf[32];
                // An idle shell waits for input indefinitely; it uses no CPU
                // while it waits.
                auto [n, err] = _console.read(WritableByteSpan(buf), WaitForever);
                if (err == Error::Interrupted) {
                        // Ctrl-C at the prompt: the console turned it into an
                        // interrupt; the editor cancels the line as for a raw ^C.
                        _console.interrupt().clear();
                        feed(CtrlC);
                        continue;
                }
                if (err.isError()) continue;
                for (size_t i = 0; i < n; ++i) feed(static_cast<char>(buf[i]));
        }
}

void Shell::report(const char *fmt, ...) {
        char    buf[CommandContext::PrintBufferBytes];
        va_list args;
        va_start(args, fmt);
        const int len = std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (len <= 0) return;
        FileHandle *err = _fds.handle(FdTable::Stderr);
        if (err == nullptr) return;
        const size_t n = static_cast<size_t>(len) < sizeof(buf) ? static_cast<size_t>(len) : sizeof(buf) - 1;
        (void)err->write(StringView(buf, n), WaitForever);
        return;
}

int Shell::runHelp(CommandContext &ctx) {
        Span<char *const> args = ctx.args();
        if (args.size() > 2) return ctx.usage();
        Shell &shell = ctx.shell();
        if (args.size() == 2) {
                const Command *cmd = shell.findCommand(args[1]);
                if (cmd == nullptr) {
                        ctx.eprintf("help: no such command: %s\n", args[1]);
                        return ExitFailure;
                }
                if (cmd->usage[0] == '\0') {
                        ctx.printf("usage: %s\n%s\n", cmd->name, cmd->help);
                } else {
                        ctx.printf("usage: %s %s\n%s\n", cmd->name, cmd->usage, cmd->help);
                }
                return ExitSuccess;
        }
        int width = 0;
        for (const Command &cmd : shell.commands()) {
                const int len = static_cast<int>(StringView(cmd.name).size());
                if (len > width) width = len;
        }
        for (const Command &cmd : shell.commands()) {
                if (ctx.isInterrupted()) return ExitInterrupted;
                ctx.printf("%-*s  %s\n", width, cmd.name, cmd.help);
        }
        return ExitSuccess;
}

BMDS_NAMESPACE_END
