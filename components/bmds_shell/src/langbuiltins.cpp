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

        // Quotes @p value for re-input to the shell, as bash's `set` and
        // `declare -p` do ('...' when needed).
        void appendQuoted(String &out, StringView value) {
                bool plain = !value.isEmpty();
                for (char c : value) {
                        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                        StringView("_./:@%+,=-").contains(c);
                        plain = plain && ok;
                }
                if (plain) {
                        (void)out.append(value);
                        return;
                }
                (void)out.append('\'');
                for (char c : value) {
                        if (c == '\'') {
                                (void)out.append("'\\''");
                        } else {
                                (void)out.append(c);
                        }
                }
                (void)out.append('\'');
                return;
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

                        bool unary(StringView op, StringView a, bool &out) {
                                const bool devNull = a == "/dev/null"; // the only file until Phase 2
                                switch (op[1]) {
                                        case 'z': out = a.isEmpty(); return true;
                                        case 'n': out = !a.isEmpty(); return true;
                                        case 'e':
                                        case 'c':
                                        case 'r':
                                        case 'w': out = devNull; return true;
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
                                } else if (op == "-nt" || op == "-ot" || op == "-ef") {
                                        out = false;
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
        if (!ex.inFunction()) {
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
                        appendQuoted(line, e.value);
                        (void)line.append('\n');
                        ctx.print(line);
                }
                return Shell::ExitSuccess;
        }
        struct Option {
                        char        letter;
                        const char *name;
                        bool ShellOptions::*field;
        };
        static const Option options[] = {
                {'e', "errexit", &ShellOptions::errexit},
                {'u', "nounset", &ShellOptions::nounset},
                {'x', "xtrace", &ShellOptions::xtrace},
        };
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
                                for (const Option &o : options) {
                                        ctx.printf("%-15s\t%s\n", o.name, st.options.*o.field ? "on" : "off");
                                }
                                return Shell::ExitSuccess;
                        }
                        const StringView name = args[++i];
                        bool             found = false;
                        for (const Option &o : options) {
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
                for (size_t k = 1; k < a.size(); ++k) {
                        bool found = false;
                        for (const Option &o : options) {
                                if (a[k] == o.letter) {
                                        st.options.*o.field = on;
                                        found = true;
                                }
                        }
                        if (!found) {
                                ctx.eprintf("set: %c%c: invalid option\n", a[0], a[k]);
                                return Shell::ExitUsage;
                        }
                }
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

BMDS_SHELL_COMMAND_AS(colon, ":", LangBuiltins::colon, "[arg]...", "Do nothing, successfully")
BMDS_SHELL_COMMAND_AS(bracket, "[", LangBuiltins::test, "expr ]", "Evaluate a conditional expression (as test)")
BMDS_SHELL_COMMAND_AS(breakCmd, "break", LangBuiltins::breakCmd, "[n]", "Leave n enclosing loops")
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
BMDS_SHELL_COMMAND(set, LangBuiltins::set, "[-+eux] [-+o name] [--] [arg]...",
                   "Set shell options and positional parameters, or list variables")
BMDS_SHELL_COMMAND(shift, LangBuiltins::shift, "[n]", "Drop the first n positional parameters")
BMDS_SHELL_COMMAND(test, LangBuiltins::test, "expr", "Evaluate a conditional expression")
BMDS_SHELL_COMMAND_AS(trueCmd, "true", LangBuiltins::colon, "", "Do nothing, successfully")
BMDS_SHELL_COMMAND(unset, LangBuiltins::unset, "[-fv] name...", "Remove variables or functions")

BMDS_NAMESPACE_END
