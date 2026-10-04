/**
 * @file      exec.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Running commands that aren't functions: built-ins, then files found by
 * path or on $PATH, run by what they contain (devplan/programs.md §5): a
 * BMDS program goes to the launcher, a `#!` script to its interpreter, a
 * `.sh` file to the built-in `sh`.
 */

#include <bmds/command.h>
#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/programlauncher.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        bool isBlank(char c) {
                return c == ' ' || c == '\t';
        }

        // A `#!` line, split as Linux does: the interpreter, then the rest of
        // the line (trimmed) as at most one argument.
        struct Shebang {
                        StringView interpreter;
                        StringView argument;
        };

        // @p head starts with "#!"; @p whole if it is the whole file. False if
        // the line doesn't end within the head.
        bool parseShebang(StringView head, bool whole, Shebang &out) {
                StringView   line = head.mid(2);
                const size_t newline = line.indexOf('\n');
                if (newline != StringView::NPos) {
                        line = line.left(newline);
                } else if (!whole) {
                        return false;
                }
                // Trailing CRs too: scripts written on a PC may have CR LF lines.
                line = line.trimmed();
                size_t end = 0;
                while (end < line.size() && !isBlank(line[end])) ++end;
                out.interpreter = line.left(end);
                out.argument = line.mid(end).trimmed();
                return true;
        }

        StringView baseName(StringView path) {
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        Error appendCopy(List<String> &list, StringView text) {
                auto [s, err] = String::fromView(text);
                return err.isOk() ? list.append(std::move(s)) : err;
        }

} // namespace

int Executor::invoke(List<String> &args, bool functions) {
        if (functions) {
                if (const ShellFunction *fn = _state.findFunction(args[0])) return callFunction(*fn, args);
        }
        if (const Command *cmd = _shell.findCommand(args[0])) return runBuiltin(*cmd, args);
        String path;
        Error  err = Error::Ok;
        if (args[0].view().contains('/')) {
                err = path.assign(args[0]);
        } else {
                err = findInPath(args[0], path);
                if (err == Error::NotExist) {
                        report("%s: command not found", args[0].cStr());
                        return Shell::ExitNotFound;
                }
        }
        if (err.isError()) {
                report("out of memory");
                return Shell::ExitFailure;
        }
        return runFile(path, args, 0);
}

int Executor::runBuiltin(const Command &cmd, List<String> &args) {
        List<char *> argv;
        if (argv.reserve(args.size()).isError()) {
                report("out of memory");
                return Shell::ExitFailure;
        }
        for (String &a : args) (void)argv.append(a.data());
        CommandContext ctx(_shell, cmd, argv.span(), *_fds, this);
        return cmd.run(ctx);
}

Error Executor::findInPath(StringView name, String &out) {
        static const char *const suffixes[] = {"", ".elf", ".sh"};
        const String            *path = _state.vars.value("PATH");
        if (path == nullptr || name.isEmpty()) return Error::NotExist;
        const Vfs::Context context = vfs();
        const StringView   dirs = path->view();
        size_t             at = 0;
        for (;;) {
                size_t colon = dirs.indexOf(':', at);
                if (colon == StringView::NPos) colon = dirs.size();
                // An empty entry is the current directory, as in POSIX.
                const StringView dir = colon > at ? dirs.mid(at, colon - at) : StringView(".");
                for (const char *suffix : suffixes) {
                        out.clear();
                        Error err = out.append(dir);
                        if (err.isOk() && !dir.endsWith('/')) err = out.append('/');
                        if (err.isOk()) err = out.append(name);
                        if (err.isOk()) err = out.append(suffix);
                        if (err.isError()) return err;
                        const Result<FileInfo> info = Vfs::stat(out, context);
                        if (info.isOk() && info.value().type == FileType::File) return Error::Ok;
                }
                if (colon == dirs.size()) break;
                at = colon + 1;
        }
        out.clear();
        return Error::NotExist;
}

Executor::FileKind Executor::classify(StringView path, StringView head) {
        if (head.startsWith(StringView("\x7f" "ELF"))) return FileKind::Program;
        if (head.startsWith(StringView("#!"))) return FileKind::Interpreted;
        if (path.endsWith(StringView(".sh"))) return FileKind::ShellScript;
        return FileKind::Other;
}

Error Executor::readHead(StringView path, char (&head)[HeadBytes], size_t &size, bool &whole) {
        size = 0;
        whole = false;
        const Vfs::Context     context = vfs();
        const Result<FileInfo> info = Vfs::stat(path, context);
        if (info.isError()) return info.error();
        if (info.value().isDirectory()) return Error::IsDir;
        auto [file, err] = Vfs::open(path, Vfs::Read, context);
        if (err.isError()) return err;
        while (size < HeadBytes) {
                auto [n, rerr] = file->read(
                        WritableByteSpan(reinterpret_cast<uint8_t *>(head) + size, HeadBytes - size), WaitForever);
                if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) {
                        whole = true;
                        break;
                }
                if (rerr.isError()) return rerr;
                size += n;
        }
        if (!whole) whole = info.value().size <= HeadBytes;
        return Error::Ok;
}

bool Executor::isExecutable(StringView path) {
        const Result<FileInfo> info = Vfs::stat(path, vfs());
        if (info.isError()) return false;
        if (info.value().isDirectory()) return true;
        if (info.value().type != FileType::File) return false;
        if (path.endsWith(StringView(".sh"))) return true;
        char   head[HeadBytes];
        size_t size = 0;
        bool   whole = false;
        if (readHead(path, head, size, whole).isError()) return false;
        return classify(path, StringView(head, size)) != FileKind::Other;
}

int Executor::runFile(StringView path, List<String> &args, int depth) {
        char   head[HeadBytes];
        size_t size = 0;
        bool   whole = false;
        Error  err = readHead(path, head, size, whole);
        if (err.isError()) {
                report("%.*s: %s", static_cast<int>(path.size()), path.data(), CommandContext::errorText(err));
                return err == Error::NotExist ? Shell::ExitNotFound : Shell::ExitCannotRun;
        }
        switch (classify(path, StringView(head, size))) {
                case FileKind::Program:
                        if (_shell.launcher() == nullptr) break;
                        return runProgram(path, args);
                case FileKind::Interpreted: return runInterpreter(path, StringView(head, size), whole, args, depth);
                case FileKind::ShellScript: {
                        // As if it started with `#!/bin/sh`.
                        const Command *sh = _shell.findCommand("sh");
                        if (sh == nullptr) break;
                        List<String> shArgs;
                        err = shArgs.reserve(args.size() + 1);
                        if (err.isOk()) err = appendCopy(shArgs, "sh");
                        if (err.isOk()) err = appendCopy(shArgs, path);
                        for (size_t i = 1; err.isOk() && i < args.size(); ++i) err = shArgs.append(std::move(args[i]));
                        if (err.isError()) {
                                report("out of memory");
                                return Shell::ExitFailure;
                        }
                        return runBuiltin(*sh, shArgs);
                }
                case FileKind::Other: break;
        }
        report("%.*s: cannot execute: not a program or script", static_cast<int>(path.size()), path.data());
        return Shell::ExitCannotRun;
}

// `#!interpreter [argument]`: runs `interpreter [argument] path args...`.
int Executor::runInterpreter(StringView path, StringView head, bool whole, List<String> &args, int depth) {
        const int pathLen = static_cast<int>(path.size());
        Shebang   line;
        if (!parseShebang(head, whole, line)) {
                report("%.*s: interpreter line too long (at most %u bytes)", pathLen, path.data(),
                       static_cast<unsigned>(HeadBytes - 1));
                return Shell::ExitCannotRun;
        }
        if (line.interpreter.isEmpty()) {
                report("%.*s: cannot execute: no interpreter after #!", pathLen, path.data());
                return Shell::ExitCannotRun;
        }
        if (depth >= MaxInterpreterDepth) {
                report("%.*s: too many levels of interpreters", pathLen, path.data());
                return Shell::ExitCannotRun;
        }
        // The interpreter's own path: an existing file, else what a PC
        // script means by it (/bin/sh, /usr/bin/env name).
        const Result<FileInfo> info = Vfs::stat(line.interpreter, vfs());
        StringView             name = line.interpreter;
        StringView             argument = line.argument;
        const Command         *builtin = nullptr;
        bool                   lookUp = false;
        if (info.isError()) {
                StringView base = baseName(line.interpreter);
                if (base == "env" && !argument.isEmpty()) {
                        name = argument;
                        argument = StringView();
                        lookUp = true;
                } else {
                        if (base == "bash") base = "sh";
                        builtin = _shell.findCommand(base);
                        if (builtin == nullptr) {
                                report("%.*s: %.*s: No such file or directory", pathLen, path.data(),
                                       static_cast<int>(line.interpreter.size()), line.interpreter.data());
                                return Shell::ExitNotFound;
                        }
                        name = base;
                }
        }
        List<String> newArgs;
        Error        err = newArgs.reserve(args.size() + 2);
        if (err.isOk()) err = appendCopy(newArgs, name);
        if (err.isOk() && !argument.isEmpty()) err = appendCopy(newArgs, argument);
        if (err.isOk()) err = appendCopy(newArgs, path);
        for (size_t i = 1; err.isOk() && i < args.size(); ++i) err = newArgs.append(std::move(args[i]));
        if (err.isError()) {
                report("out of memory");
                return Shell::ExitFailure;
        }
        if (builtin != nullptr) return runBuiltin(*builtin, newArgs);
        if (lookUp) return invoke(newArgs, false);
        // newArgs[0] is a copy: path and head may point into args or the caller's buffer.
        auto [interpreter, cerr] = String::fromView(line.interpreter);
        if (cerr.isError()) {
                report("out of memory");
                return Shell::ExitFailure;
        }
        return runFile(interpreter, newArgs, depth + 1);
}

// A program file: the launcher runs it with the exported variables as its
// environment. It reports its own problems; they become 127 (no such file)
// or 126 (can't run it), as in bash.
int Executor::runProgram(StringView path, List<String> &args) {
        List<StringView> argViews;
        List<String>     envStrings;
        List<StringView> envViews;
        Error            err = argViews.reserve(args.size());
        for (const String &a : args) {
                if (err.isOk()) err = argViews.append(a.view());
        }
        for (const Variables::Entry &e : _state.vars.entries()) {
                if (err.isError()) break;
                if ((e.flags & Variables::Exported) == 0 || (e.flags & Variables::IsSet) == 0) continue;
                String line;
                err = line.append(e.name);
                if (err.isOk()) err = line.append('=');
                if (err.isOk()) err = line.append(e.value);
                if (err.isOk()) err = envStrings.append(std::move(line));
        }
        if (err.isOk()) err = envViews.reserve(envStrings.size());
        for (const String &e : envStrings) {
                if (err.isOk()) err = envViews.append(e.view());
        }
        if (err.isError()) {
                report("out of memory");
                return Shell::ExitFailure;
        }
        ProgramLauncher::Request request;
        request.path = path;
        request.args = Span<const StringView>(argViews.data(), argViews.size());
        request.environment = Span<const StringView>(envViews.data(), envViews.size());
        request.fds = _fds;
        request.cwd = vfs().cwd;
        request.interrupt = &_shell.console().interrupt();
        request.console = &_shell.console();
        auto [status, lerr] = _shell.launcher()->run(request);
        if (lerr.isOk()) return status;
        if (lerr == Error::Interrupted) return Shell::ExitInterrupted;
        return lerr == Error::NotExist ? Shell::ExitNotFound : Shell::ExitCannotRun;
}

BMDS_NAMESPACE_END
