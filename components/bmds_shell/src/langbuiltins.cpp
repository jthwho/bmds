/**
 * @file      langbuiltins.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Built-ins of the shell language. Behavior and messages follow bash;
 * they are userspace interfaces.
 */

#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/langbuiltins.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;
        using Flow = Executor::Flow;

        bool isName(StringView s) {
                if (s.isEmpty()) return false;
                for (size_t i = 0; i < s.size(); ++i) {
                        const char c = s[i];
                        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                        if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
                }
                return true;
        }

        // Parses a decimal integer, with optional sign and surrounding blanks (test, exit...).
        bool parseInt(StringView s, long &out) {
                s = s.trimmed();
                if (s.isEmpty()) return false;
                bool   negative = false;
                size_t i = 0;
                if (s[0] == '-' || s[0] == '+') {
                        negative = s[0] == '-';
                        i = 1;
                }
                if (i == s.size()) return false;
                long v = 0;
                for (; i < s.size(); ++i) {
                        if (s[i] < '0' || s[i] > '9') return false;
                        v = v * 10 + (s[i] - '0');
                        if (v > 0x7fffffffL) return false;
                }
                out = negative ? -v : v;
                return true;
        }

        // `break`, `continue`: the loop count.
        int loopControl(CommandContext &ctx, Flow flow) {
                Executor  &ex = ctx.executor();
                const Args args = ctx.args();
                if (args.size() > 2) return ctx.usage();
                long n = 1;
                if (args.size() == 2 && !parseInt(args[1], n)) {
                        ctx.eprintf("%s: %s: numeric argument required\n", args[0], args[1]);
                        return Shell::ExitFailure;
                }
                if (n < 1) {
                        ctx.eprintf("%s: %ld: loop count out of range\n", args[0], n);
                        return Shell::ExitFailure;
                }
                if (ex.loopDepth() == 0) {
                        ctx.eprintf("%s: only meaningful in a `for', `while', or `until' loop\n", args[0]);
                        return Shell::ExitSuccess;
                }
                ex.setFlow(flow, n < ex.loopDepth() ? static_cast<int>(n) : ex.loopDepth());
                return Shell::ExitSuccess;
        }

        // `export`, `readonly`: set attributes, or list.
        int attributes(CommandContext &ctx, uint8_t flag) {
                Executor  &ex = ctx.executor();
                const Args args = ctx.args();
                size_t     i = 1;
                if (i < args.size() && StringView(args[i]) == "-p") ++i;
                if (i == args.size()) {
                        for (const Variables::Entry &e : ex.state().vars.entries()) {
                                if ((e.flags & flag) == 0) continue;
                                String line;
                                (void)line.append("declare -");
                                if (e.flags & Variables::ReadOnly) (void)line.append('r');
                                if (e.flags & Variables::Exported) (void)line.append('x');
                                (void)line.append(' ');
                                (void)line.append(e.name);
                                if (e.flags & Variables::IsSet) {
                                        (void)line.append("=\"");
                                        for (char c : e.value) {
                                                if (StringView("\"\\$`").contains(c)) (void)line.append('\\');
                                                (void)line.append(c);
                                        }
                                        (void)line.append('"');
                                }
                                (void)line.append('\n');
                                ctx.print(line);
                        }
                        return Shell::ExitSuccess;
                }
                int status = Shell::ExitSuccess;
                for (; i < args.size(); ++i) {
                        const StringView arg = args[i];
                        const size_t     eq = arg.indexOf('=');
                        const StringView name = eq == StringView::NPos ? arg : arg.left(eq);
                        if (!isName(name)) {
                                ctx.eprintf("%s: `%s': not a valid identifier\n", args[0], args[i]);
                                status = Shell::ExitFailure;
                                continue;
                        }
                        Error err;
                        if (eq != StringView::NPos) err = ex.state().vars.set(name, arg.mid(eq + 1));
                        if (err.isOk()) err = ex.state().vars.addFlags(name, flag);
                        if (err.isError()) {
                                ctx.eprintf("%s: %.*s: %s\n", args[0], static_cast<int>(name.size()), name.data(),
                                            err == Error::ReadOnly ? "readonly variable" : "out of memory");
                                status = Shell::ExitFailure;
                        }
                }
                return status;
        }

        // test / [: POSIX rules by argument count, then a recursive grammar.
        class TestEval {
                public:
                        TestEval(CommandContext &ctx, Args args) : _ctx(ctx), _args(args) {}

                        int run() {
                                const size_t n = _args.size();
                                bool         result = false;
                                if (n == 0) return 1;
                                if (n <= 4) {
                                        if (!shortcut(0, n, result)) return 2;
                                } else {
                                        _pos = 0;
                                        if (!orExpr(result)) return 2;
                                        if (_pos != n) return syntax(_args[_pos], "too many arguments");
                                }
                                return result ? 0 : 1;
                        }

                private:
                        StringView arg(size_t i) const { return _args[i]; }

                        int syntax(StringView token, const char *message) {
                                (void)token;
                                _ctx.eprintf("%s: %s\n", _ctx.args()[0], message);
                                _failed = true;
                                return 2;
                        }

                        static bool isUnary(StringView op) {
                                return op.size() == 2 && op[0] == '-' &&
                                       StringView("zneEfdscbprwxSLhgukOGNtv").contains(op[1]);
                        }

                        static bool isBinary(StringView op) {
                                return op == "=" || op == "==" || op == "!=" || op == "<" || op == ">" || op == "-eq" ||
                                       op == "-ne" || op == "-lt" || op == "-le" || op == "-gt" || op == "-ge" ||
                                       op == "-nt" || op == "-ot" || op == "-ef";
                        }

                        // The fixed-count rules of POSIX for 0..4 arguments.
                        bool shortcut(size_t at, size_t n, bool &out) {
                                switch (n) {
                                        case 0: out = false; return true;
                                        case 1: out = !arg(at).isEmpty(); return true;
                                        case 2:
                                                if (arg(at) == "!") {
                                                        out = arg(at + 1).isEmpty();
                                                        return true;
                                                }
                                                if (isUnary(arg(at))) return unary(arg(at), arg(at + 1), out);
                                                syntax(arg(at), "unary operator expected");
                                                return false;
                                        case 3:
                                                if (isBinary(arg(at + 1)))
                                                        return binary(arg(at), arg(at + 1), arg(at + 2), out);
                                                if (arg(at) == "!") {
                                                        if (!shortcut(at + 1, 2, out)) return false;
                                                        out = !out;
                                                        return true;
                                                }
                                                if (arg(at) == "(" && arg(at + 2) == ")")
                                                        return shortcut(at + 1, 1, out);
                                                if (arg(at + 1) == "-a" || arg(at + 1) == "-o") break;
                                                syntax(arg(at + 1), "binary operator expected");
                                                return false;
                                        case 4:
                                                if (arg(at) == "!") {
                                                        if (!shortcut(at + 1, 3, out)) return false;
                                                        out = !out;
                                                        return true;
                                                }
                                                if (arg(at) == "(" && arg(at + 3) == ")")
                                                        return shortcut(at + 1, 2, out);
                                                break;
                                }
                                _pos = at;
                                if (!orExpr(out)) return false;
                                if (_pos != _args.size()) {
                                        syntax(arg(_pos), "too many arguments");
                                        return false;
                                }
                                return true;
                        }

                        bool orExpr(bool &out) {
                                if (!andExpr(out)) return false;
                                while (_pos < _args.size() && arg(_pos) == "-o") {
                                        ++_pos;
                                        bool r = false;
                                        if (!andExpr(r)) return false;
                                        out = out || r;
                                }
                                return true;
                        }

                        bool andExpr(bool &out) {
                                if (!notExpr(out)) return false;
                                while (_pos < _args.size() && arg(_pos) == "-a") {
                                        ++_pos;
                                        bool r = false;
                                        if (!notExpr(r)) return false;
                                        out = out && r;
                                }
                                return true;
                        }

                        bool notExpr(bool &out) {
                                if (_pos < _args.size() && arg(_pos) == "!") {
                                        ++_pos;
                                        if (!notExpr(out)) return false;
                                        out = !out;
                                        return true;
                                }
                                return primary(out);
                        }

                        bool primary(bool &out) {
                                const size_t left = _args.size() - _pos;
                                if (left == 0) {
                                        syntax(StringView(), "argument expected");
                                        return false;
                                }
                                if (arg(_pos) == "(") {
                                        ++_pos;
                                        if (!orExpr(out)) return false;
                                        if (_pos >= _args.size() || arg(_pos) != ")") {
                                                syntax(StringView(), "`)' expected");
                                                return false;
                                        }
                                        ++_pos;
                                        return true;
                                }
                                if (left >= 3 && isBinary(arg(_pos + 1))) {
                                        _pos += 3;
                                        return binary(arg(_pos - 3), arg(_pos - 2), arg(_pos - 1), out);
                                }
                                if (left >= 2 && isUnary(arg(_pos))) {
                                        _pos += 2;
                                        return unary(arg(_pos - 2), arg(_pos - 1), out);
                                }
                                out = !arg(_pos++).isEmpty();
                                return true;
                        }

                        // What @p path names, or false if nothing.
                        bool stat(StringView path, FileInfo &info) {
                                if (path.isEmpty()) return false;
                                Result<FileInfo> r = Vfs::stat(path, _ctx.vfs());
                                if (r.error().isError()) return false;
                                info = std::move(r).value();
                                return true;
                        }

                        bool unary(StringView op, StringView a, bool &out) {
                                FileInfo info;
                                switch (op[1]) {
                                        case 'z': out = a.isEmpty(); return true;
                                        case 'n': out = !a.isEmpty(); return true;
                                        case 'e':
                                        case 'r': out = stat(a, info); return true;
                                        case 'f': out = stat(a, info) && info.type == FileType::File; return true;
                                        case 'd': out = stat(a, info) && info.isDirectory(); return true;
                                        case 'c': out = stat(a, info) && info.type == FileType::Device; return true;
                                        // Directories count as non-empty, as on Unix (where they have a size).
                                        case 's':
                                                out = stat(a, info) && (info.size > 0 || info.isDirectory());
                                                return true;
                                        case 'w':
                                                out = stat(a, info) && (info.attributes & FileInfo::ReadOnly) == 0;
                                                return true;
                                        // No execute bits on FAT: directories, and what would run
                                        // (programs and scripts, by content or name).
                                        case 'x': out = _ctx.executor().isExecutable(a); return true;
                                        case 'v': out = _ctx.executor().state().vars.value(a) != nullptr; return true;
                                        case 't': {
                                                long fd = 0;
                                                if (!parseInt(a, fd)) {
                                                        _ctx.eprintf("%s: %.*s: integer expression expected\n",
                                                                     _ctx.args()[0], static_cast<int>(a.size()),
                                                                     a.data());
                                                        return false;
                                                }
                                                FileHandle *h = _ctx.fds().handle(static_cast<int>(fd));
                                                out = h != nullptr && h->stream().isTerminal();
                                                return true;
                                        }
                                        default: out = false; return true;
                                }
                        }

                        bool binary(StringView a, StringView op, StringView b, bool &out) {
                                if (op == "=" || op == "==") {
                                        out = a == b;
                                } else if (op == "!=") {
                                        out = !(a == b);
                                } else if (op == "<") {
                                        out = a < b;
                                } else if (op == ">") {
                                        out = a > b;
                                } else if (op == "-nt" || op == "-ot") {
                                        // As bash: a file that exists is newer than one that doesn't.
                                        FileInfo   x, y;
                                        const bool hasA = stat(a, x);
                                        const bool hasB = stat(b, y);
                                        if (op == "-ot") {
                                                out = hasB && (!hasA || x.modified < y.modified);
                                        } else {
                                                out = hasA && (!hasB || x.modified > y.modified);
                                        }
                                } else if (op == "-ef") {
                                        // FAT has no links: the same file is the same path
                                        // (names are case-insensitive).
                                        FileInfo x, y;
                                        out = false;
                                        if (stat(a, x) && stat(b, y)) {
                                                const Result<String> pa = Vfs::normalize(a, _ctx.cwd());
                                                const Result<String> pb = Vfs::normalize(b, _ctx.cwd());
                                                out = pa.error().isOk() && pb.error().isOk() &&
                                                      pa.value().view().equalsIgnoreCase(pb.value());
                                        }
                                } else {
                                        long x = 0, y = 0;
                                        for (StringView v : {a, b}) {
                                                long tmp = 0;
                                                if (!parseInt(v, tmp)) {
                                                        _ctx.eprintf("%s: %.*s: integer expression expected\n",
                                                                     _ctx.args()[0], static_cast<int>(v.size()),
                                                                     v.data());
                                                        return false;
                                                }
                                        }
                                        (void)parseInt(a, x);
                                        (void)parseInt(b, y);
                                        if (op == "-eq") out = x == y;
                                        if (op == "-ne") out = x != y;
                                        if (op == "-lt") out = x < y;
                                        if (op == "-le") out = x <= y;
                                        if (op == "-gt") out = x > y;
                                        if (op == "-ge") out = x >= y;
                                }
                                return true;
                        }

                        CommandContext &_ctx;
                        Args            _args;
                        size_t          _pos = 0;
                        bool            _failed = false;
        };

} // namespace

namespace {

        bool isKeyword(StringView w) {
                static const char *const keywords[] = {"!",    "[[",    "]]",    "case", "do",       "done", "elif",
                                                       "else", "esac",  "fi",    "for",  "function", "if",   "in",
                                                       "then", "until", "while", "{",    "}"};
                for (const char *k : keywords) {
                        if (w == k) return true;
                }
                return false;
        }

        // What a name is, the way `type` and `command -v` look it up.
        enum class Kind {
                None,
                Keyword,
                Function,
                Builtin,
                File
        };

        // For a File, @p path is where (the name itself if it has a `/`).
        Kind kindOf(CommandContext &ctx, StringView name, String &path) {
                if (isKeyword(name)) return Kind::Keyword;
                if (ctx.executor().state().findFunction(name) != nullptr) return Kind::Function;
                if (ctx.shell().findCommand(name) != nullptr) return Kind::Builtin;
                if (name.contains('/')) {
                        const bool found = ctx.executor().isExecutable(name) && path.assign(name).isOk();
                        return found ? Kind::File : Kind::None;
                }
                return ctx.executor().findInPath(name, path).isOk() ? Kind::File : Kind::None;
        }

        // `type`'s description of @p name; false if not found.
        bool describe(CommandContext &ctx, const char *name, bool word) {
                String path;
                switch (kindOf(ctx, name, path)) {
                        case Kind::Keyword:
                                ctx.printf(word ? "keyword\n" : "%s is a shell keyword\n", name);
                                return true;
                        case Kind::Function: {
                                if (word) {
                                        ctx.print("function\n");
                                        return true;
                                }
                                const ShellFunction *f = ctx.executor().state().findFunction(name);
                                ctx.printf("%s is a function\n%s () ", name, name);
                                ctx.print(f->source);
                                ctx.print("\n");
                                return true;
                        }
                        case Kind::Builtin:
                                ctx.printf(word ? "builtin\n" : "%s is a shell builtin\n", name);
                                return true;
                        case Kind::File:
                                if (word) {
                                        ctx.print("file\n");
                                } else {
                                        ctx.printf("%s is %s\n", name, path.cStr());
                                }
                                return true;
                        case Kind::None: break;
                }
                return false;
        }

        // The options `set` and `sh` take.
        struct OptionName {
                        char        letter;
                        const char *name;
                        bool ShellOptions::*field;
        };

        constexpr OptionName OptionNames[] = {
                {'e', "errexit", &ShellOptions::errexit},
                {'C', "noclobber", &ShellOptions::noclobber},
                {'u', "nounset", &ShellOptions::nounset},
                {'x', "xtrace", &ShellOptions::xtrace},
        };

        // `-eux` (on) or `+eux` (off) into @p options; false, reported, for an unknown letter.
        bool applyOptionLetters(CommandContext &ctx, StringView arg, ShellOptions &options) {
                const bool on = arg[0] == '-';
                for (size_t k = 1; k < arg.size(); ++k) {
                        bool found = false;
                        for (const OptionName &o : OptionNames) {
                                if (arg[k] == o.letter) {
                                        options.*o.field = on;
                                        found = true;
                                }
                        }
                        if (!found) {
                                ctx.eprintf("%s: %c%c: invalid option\n", ctx.args()[0], arg[0], arg[k]);
                                return false;
                        }
                }
                return true;
        }

} // namespace

int LangBuiltins::type(CommandContext &ctx) {
        const Args args = ctx.args();
        size_t     i = 1;
        bool       word = false;
        if (i < args.size() && StringView(args[i]) == "-t") {
                word = true;
                ++i;
        }
        if (i == args.size()) return ctx.usage();
        int status = Shell::ExitSuccess;
        for (; i < args.size(); ++i) {
                if (!describe(ctx, args[i], word)) {
                        if (!word) ctx.eprintf("type: %s: not found\n", args[i]);
                        status = Shell::ExitFailure;
                }
        }
        return status;
}

int LangBuiltins::command(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() < 2) return Shell::ExitSuccess;
        const StringView option = args[1];
        if (option == "-v" || option == "-V") {
                int  status = Shell::ExitSuccess;
                Kind kind = Kind::None;
                for (size_t i = 2; i < args.size(); ++i) {
                        if (option == "-V") {
                                if (!describe(ctx, args[i], false)) {
                                        ctx.eprintf("command: %s: not found\n", args[i]);
                                        status = Shell::ExitFailure;
                                }
                        } else if (String path; (kind = kindOf(ctx, args[i], path)) != Kind::None) {
                                ctx.printf("%s\n", kind == Kind::File ? path.cStr() : args[i]);
                        } else {
                                status = Shell::ExitFailure;
                        }
                }
                return status;
        }
        // Run a built-in or a file, bypassing functions of the same name.
        const Args rest = args.mid(option == "--" ? 2 : 1);
        if (rest.isEmpty()) return Shell::ExitSuccess;
        List<String> words;
        Error        err = words.reserve(rest.size());
        for (size_t i = 0; err.isOk() && i < rest.size(); ++i) {
                auto [w, werr] = String::fromView(rest[i]);
                err = werr.isOk() ? words.append(std::move(w)) : werr;
        }
        if (err.isError()) {
                ctx.eprint("command: out of memory\n");
                return Shell::ExitFailure;
        }
        return ctx.executor().invoke(words, false);
}

int LangBuiltins::colon(CommandContext &) {
        return Shell::ExitSuccess;
}

int LangBuiltins::falseCmd(CommandContext &) {
        return Shell::ExitFailure;
}

int LangBuiltins::test(CommandContext &ctx) {
        Args args = ctx.args();
        if (StringView(args[0]) == "[") {
                if (StringView(args[args.size() - 1]) != "]" || args.size() < 2) {
                        ctx.eprint("[: missing `]'\n");
                        return 2;
                }
                args = args.first(args.size() - 1);
        }
        return TestEval(ctx, args.mid(1)).run();
}

int LangBuiltins::breakCmd(CommandContext &ctx) {
        return loopControl(ctx, Flow::Break);
}

int LangBuiltins::continueCmd(CommandContext &ctx) {
        return loopControl(ctx, Flow::Continue);
}

int LangBuiltins::returnCmd(CommandContext &ctx) {
        Executor  &ex = ctx.executor();
        const Args args = ctx.args();
        if (!ex.canReturn()) {
                ctx.eprint("return: can only `return' from a function or sourced script\n");
                return Shell::ExitFailure;
        }
        long n = ex.state().lastStatus;
        if (args.size() > 2) return ctx.usage();
        if (args.size() == 2 && !parseInt(args[1], n)) {
                ctx.eprintf("return: %s: numeric argument required\n", args[1]);
                n = 2;
        }
        ex.setFlow(Flow::Return);
        return static_cast<int>(n & 0xff);
}

int LangBuiltins::exitCmd(CommandContext &ctx) {
        Executor  &ex = ctx.executor();
        const Args args = ctx.args();
        long       n = ex.state().lastStatus;
        if (args.size() > 2) {
                ctx.eprint("exit: too many arguments\n");
                return Shell::ExitFailure;
        }
        if (args.size() == 2 && !parseInt(args[1], n)) {
                ctx.eprintf("exit: %s: numeric argument required\n", args[1]);
                n = 2;
        }
        ex.setFlow(Flow::Exit);
        return static_cast<int>(n & 0xff);
}

int LangBuiltins::eval(CommandContext &ctx) {
        const Args args = ctx.args();
        String     source;
        for (size_t i = 1; i < args.size(); ++i) {
                if ((i > 1 && source.append(' ').isError()) || source.append(args[i]).isError()) {
                        ctx.eprint("eval: out of memory\n");
                        return Shell::ExitFailure;
                }
        }
        return ctx.executor().runSource(source);
}

int LangBuiltins::source(CommandContext &ctx) {
        const Args args = ctx.args();
        if (args.size() < 2) {
                ctx.eprintf("%s: filename argument required\n", args[0]);
                return Shell::ExitUsage;
        }
        String script;
        Error  err = ctx.executor().readFile(args[1], script);
        if (err.isError()) {
                ctx.eprintf("%s: %s: %s\n", args[0], args[1], CommandContext::errorText(err));
                return Shell::ExitFailure;
        }
        return ctx.executor().runSourced(script, args.mid(2));
}

int LangBuiltins::sh(CommandContext &ctx) {
        const Args   args = ctx.args();
        String       script;
        Args         params;
        ShellOptions options = ctx.executor().state().options;
        // Options first (`#!/bin/sh -e` comes here as `sh -e script`).
        size_t first = 1;
        while (first < args.size()) {
                const StringView a = args[first];
                if (a.size() < 2 || (a[0] != '-' && a[0] != '+') || a == "-c") break;
                ++first;
                if (a == "--") break;
                if (!applyOptionLetters(ctx, a, options)) return Shell::ExitUsage;
        }
        if (first < args.size() && StringView(args[first]) == "-c") {
                // `sh -c command [name [arg]...]`: name would be $0.
                if (first + 1 == args.size()) {
                        ctx.eprint("sh: -c: option requires an argument\n");
                        return Shell::ExitUsage;
                }
                if (script.assign(args[first + 1]).isError()) {
                        ctx.eprint("sh: out of memory\n");
                        return Shell::ExitFailure;
                }
                params = args.size() > first + 3 ? args.mid(first + 3) : Args();
        } else if (first < args.size()) {
                Error err = ctx.executor().readFile(args[first], script);
                if (err.isError()) {
                        ctx.eprintf("sh: %s: %s\n", args[first], CommandContext::errorText(err));
                        return Shell::ExitNotFound;
                }
                params = args.mid(first + 1);
        } else {
                // The script on stdin; an interactive subshell is not supported.
                FileHandle *in = ctx.fds().handle(FdTable::Stdin);
                if (in == nullptr || in->stream().isTerminal()) return ctx.usage();
                uint8_t buf[256];
                for (;;) {
                        auto [n, err] = ctx.read(WritableByteSpan(buf));
                        if (err == Error::EndOfFile || (err.isOk() && n == 0)) break;
                        if (err.isError()) return Shell::ExitFailure;
                        if (script.append(StringView(reinterpret_cast<const char *>(buf), n)).isError()) {
                                ctx.eprint("sh: out of memory\n");
                                return Shell::ExitFailure;
                        }
                }
        }
        return ctx.executor().runInSubshell(script, params, &options);
}

int LangBuiltins::set(CommandContext &ctx) {
        Executor   &ex = ctx.executor();
        ShellState &st = ex.state();
        const Args  args = ctx.args();
        if (args.size() == 1) {
                for (const Variables::Entry &e : st.vars.entries()) {
                        if ((e.flags & Variables::IsSet) == 0) continue;
                        String line;
                        (void)line.append(e.name);
                        (void)line.append('=');
                        appendShellQuoted(line, e.value);
                        (void)line.append('\n');
                        ctx.print(line);
                }
                return Shell::ExitSuccess;
        }
        size_t i = 1;
        bool   setPositional = false;
        for (; i < args.size(); ++i) {
                const StringView a = args[i];
                if (a == "--") {
                        ++i;
                        setPositional = true;
                        break;
                }
                if (a.size() < 2 || (a[0] != '-' && a[0] != '+')) break;
                const bool on = a[0] == '-';
                if (a[1] == 'o') {
                        if (i + 1 == args.size()) {
                                for (const OptionName &o : OptionNames) {
                                        ctx.printf("%-15s\t%s\n", o.name, st.options.*o.field ? "on" : "off");
                                }
                                return Shell::ExitSuccess;
                        }
                        const StringView name = args[++i];
                        bool             found = false;
                        for (const OptionName &o : OptionNames) {
                                if (name == o.name) {
                                        st.options.*o.field = on;
                                        found = true;
                                }
                        }
                        if (!found) {
                                ctx.eprintf("set: %s: invalid option name\n", args[i]);
                                return Shell::ExitUsage;
                        }
                        continue;
                }
                if (!applyOptionLetters(ctx, a, st.options)) return Shell::ExitUsage;
        }
        if (i < args.size()) setPositional = true;
        if (setPositional) {
                List<String> params;
                for (; i < args.size(); ++i) {
                        auto [p, err] = String::fromView(args[i]);
                        if (err.isError() || params.append(std::move(p)).isError()) {
                                ctx.eprint("set: out of memory\n");
                                return Shell::ExitFailure;
                        }
                }
                st.positional = std::move(params);
        }
        return Shell::ExitSuccess;
}

int LangBuiltins::shift(CommandContext &ctx) {
        ShellState &st = ctx.executor().state();
        const Args  args = ctx.args();
        long        n = 1;
        if (args.size() > 2) return ctx.usage();
        if (args.size() == 2 && (!parseInt(args[1], n) || n < 0)) {
                ctx.eprintf("shift: %s: numeric argument required\n", args[1]);
                return Shell::ExitFailure;
        }
        if (static_cast<size_t>(n) > st.positional.size()) return Shell::ExitFailure;
        for (long k = 0; k < n; ++k) st.positional.removeAt(0);
        return Shell::ExitSuccess;
}

int LangBuiltins::exportCmd(CommandContext &ctx) {
        return attributes(ctx, Variables::Exported);
}

int LangBuiltins::readonly(CommandContext &ctx) {
        return attributes(ctx, Variables::ReadOnly);
}

int LangBuiltins::unset(CommandContext &ctx) {
        ShellState &st = ctx.executor().state();
        const Args  args = ctx.args();
        bool        functions = false;
        size_t      i = 1;
        for (; i < args.size() && args[i][0] == '-'; ++i) {
                const StringView o = args[i];
                if (o == "-f") {
                        functions = true;
                } else if (o == "-v") {
                        functions = false;
                } else if (o == "--") {
                        ++i;
                        break;
                } else {
                        return ctx.usage();
                }
        }
        int status = Shell::ExitSuccess;
        for (; i < args.size(); ++i) {
                if (functions) {
                        st.removeFunction(args[i]);
                        continue;
                }
                if (st.vars.unset(args[i]) == Error::ReadOnly) {
                        ctx.eprintf("unset: %s: cannot unset: readonly variable\n", args[i]);
                        status = Shell::ExitFailure;
                }
        }
        return status;
}

int LangBuiltins::local(CommandContext &ctx) {
        Executor  &ex = ctx.executor();
        const Args args = ctx.args();
        if (!ex.inFunction()) {
                ctx.eprint("local: can only be used in a function\n");
                return Shell::ExitFailure;
        }
        int status = Shell::ExitSuccess;
        for (size_t i = 1; i < args.size(); ++i) {
                const StringView arg = args[i];
                const size_t     eq = arg.indexOf('=');
                const StringView name = eq == StringView::NPos ? arg : arg.left(eq);
                if (!isName(name)) {
                        ctx.eprintf("local: `%s': not a valid identifier\n", args[i]);
                        status = Shell::ExitFailure;
                        continue;
                }
                const StringView value = eq == StringView::NPos ? StringView() : arg.mid(eq + 1);
                Error            err = ex.declareLocal(name, eq == StringView::NPos ? nullptr : &value);
                if (err.isError()) {
                        ctx.eprintf("local: %.*s: %s\n", static_cast<int>(name.size()), name.data(),
                                    err == Error::ReadOnly ? "readonly variable" : "out of memory");
                        status = Shell::ExitFailure;
                }
        }
        return status;
}

BMDS_SHELL_COMMAND_AS(dot, ".", LangBuiltins::source, "file [arg]...", "Run a script in this shell (as source)")
BMDS_SHELL_COMMAND_AS(colon, ":", LangBuiltins::colon, "[arg]...", "Do nothing, successfully")
BMDS_SHELL_COMMAND_AS(bracket, "[", LangBuiltins::test, "expr ]", "Evaluate a conditional expression (as test)")
BMDS_SHELL_COMMAND_AS(breakCmd, "break", LangBuiltins::breakCmd, "[n]", "Leave n enclosing loops")
BMDS_SHELL_COMMAND(command, LangBuiltins::command, "[-vV] name [arg]...",
                   "Run a built-in, ignoring functions; or say what a name is")
BMDS_SHELL_COMMAND_AS(continueCmd, "continue", LangBuiltins::continueCmd, "[n]",
                      "Start the next iteration of the n-th enclosing loop")
BMDS_SHELL_COMMAND(eval, LangBuiltins::eval, "[word]...", "Run the words as a command")
BMDS_SHELL_COMMAND_AS(exitCmd, "exit", LangBuiltins::exitCmd, "[n]", "Leave the shell (at the prompt: the input)")
BMDS_SHELL_COMMAND_AS(exportCmd, "export", LangBuiltins::exportCmd, "[-p] [name[=value]]...",
                      "Mark variables for export to programs, or list them")
BMDS_SHELL_COMMAND_AS(falseCmd, "false", LangBuiltins::falseCmd, "", "Do nothing, unsuccessfully")
BMDS_SHELL_COMMAND(local, LangBuiltins::local, "name[=value]...", "Make variables local to the function")
BMDS_SHELL_COMMAND(readonly, LangBuiltins::readonly, "[-p] [name[=value]]...", "Make variables read-only, or list them")
BMDS_SHELL_COMMAND_AS(returnCmd, "return", LangBuiltins::returnCmd, "[n]", "Return from a function")
BMDS_SHELL_COMMAND(set, LangBuiltins::set, "[-+Ceux] [-+o name] [--] [arg]...",
                   "Set shell options and positional parameters, or list variables")
BMDS_SHELL_COMMAND(sh, LangBuiltins::sh, "[-+Ceux] [-c command [name] | file] [arg]...",
                   "Run a script in a subshell (from stdin without arguments)")
BMDS_SHELL_COMMAND(shift, LangBuiltins::shift, "[n]", "Drop the first n positional parameters")
BMDS_SHELL_COMMAND(source, LangBuiltins::source, "file [arg]...", "Run a script in this shell")
BMDS_SHELL_COMMAND(test, LangBuiltins::test, "expr", "Evaluate a conditional expression")
BMDS_SHELL_COMMAND(type, LangBuiltins::type, "[-t] name...", "Say whether names are keywords, functions or built-ins")
BMDS_SHELL_COMMAND_AS(trueCmd, "true", LangBuiltins::colon, "", "Do nothing, successfully")
BMDS_SHELL_COMMAND(unset, LangBuiltins::unset, "[-fv] name...", "Remove variables or functions")

BMDS_NAMESPACE_END
