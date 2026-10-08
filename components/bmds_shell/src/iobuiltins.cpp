/**
 * @file      iobuiltins.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `printf` and `read`, as bash's. Their behavior is a userspace interface.
 */

#include <cstdint>
#include <cstdio>
#include <new>
#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/langbuiltins.h>
#include <bmds/lineeditor.h>
#include <bmds/memory.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        bool isName(StringView s) {
                if (s.isEmpty()) return false;
                for (size_t i = 0; i < s.size(); ++i) {
                        const char c = s[i];
                        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
                        if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
                }
                return true;
        }

        int hexDigit(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

        // One backslash escape at text[i] ('\\'), as printf's format and %b take
        // them. Advances @p i past it. Returns false at \c (stop all output).
        bool escape(StringView text, size_t &i, String &out, bool inB) {
                if (i + 1 >= text.size()) {
                        (void)out.append('\\');
                        ++i;
                        return true;
                }
                const char c = text[++i];
                ++i;
                switch (c) {
                        case 'a': (void)out.append('\a'); return true;
                        case 'b': (void)out.append('\b'); return true;
                        case 'c': return false;
                        case 'e':
                        case 'E': (void)out.append('\x1b'); return true;
                        case 'f': (void)out.append('\f'); return true;
                        case 'n': (void)out.append('\n'); return true;
                        case 'r': (void)out.append('\r'); return true;
                        case 't': (void)out.append('\t'); return true;
                        case 'v': (void)out.append('\v'); return true;
                        case '\\': (void)out.append('\\'); return true;
                        case '"': (void)out.append('"'); return true;
                        case '\'': (void)out.append('\''); return true;
                        case '?': (void)out.append('?'); return true;
                        case 'x': {
                                int v = 0, n = 0;
                                while (n < 2 && i < text.size() && hexDigit(text[i]) >= 0) {
                                        v = v * 16 + hexDigit(text[i++]);
                                        ++n;
                                }
                                if (n == 0) {
                                        (void)out.append("\\x");
                                } else {
                                        (void)out.append(static_cast<char>(v));
                                }
                                return true;
                        }
                        default:
                                if (c >= '0' && c <= '7') {
                                        // \NNN in the format; \0NNN in %b (up to three more digits).
                                        int    v = 0, n = 0;
                                        size_t j = i - 1;
                                        if (inB && c == '0') j = i;
                                        const int maxDigits = 3;
                                        while (n < maxDigits && j < text.size() && text[j] >= '0' && text[j] <= '7') {
                                                v = v * 8 + (text[j++] - '0');
                                                ++n;
                                        }
                                        i = j;
                                        (void)out.append(static_cast<char>(v & 0xff));
                                        return true;
                                }
                                (void)out.append('\\');
                                (void)out.append(c);
                                return true;
                }
        }

        // A numeric printf argument: decimal, 0x hex, 0 octal, or 'c / "c for a
        // character code. Reports invalid numbers (bash keeps going).
        bool number(CommandContext &ctx, const char *arg, long long &out) {
                StringView s(arg);
                out = 0;
                if (s.isEmpty()) return true;
                if (s[0] == '\'' || s[0] == '"') {
                        out = s.size() > 1 ? static_cast<unsigned char>(s[1]) : 0;
                        return true;
                }
                size_t i = 0;
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
                bool negative = false;
                if (i < s.size() && (s[i] == '-' || s[i] == '+')) negative = s[i++] == '-';
                unsigned base = 10;
                if (i + 1 < s.size() && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                        base = 16;
                        i += 2;
                } else if (i < s.size() && s[i] == '0') {
                        base = 8;
                }
                const size_t       start = i;
                unsigned long long v = 0;
                for (; i < s.size(); ++i) {
                        const int d = hexDigit(s[i]);
                        if (d < 0 || static_cast<unsigned>(d) >= base) break;
                        v = v * base + static_cast<unsigned>(d);
                }
                out = negative ? -static_cast<long long>(v) : static_cast<long long>(v);
                if (i == s.size() && i > start) return true;
                ctx.eprintf("printf: %s: invalid number\n", arg);
                return false;
        }

        // Splits @p line into @p names on IFS (backslash-protected characters
        // never split); the last name takes the rest.
        Error assignFields(Executor &ex, const String &line, const List<bool> &protectedChars, Args names) {
                // A copy: assigning the fields may move the variable table.
                const String *ifsVar = ex.state().vars.value("IFS");
                auto [ifsCopy, ifsErr] = String::fromView(ifsVar != nullptr ? ifsVar->view() : StringView(" \t\n"));
                if (ifsErr.isError()) return ifsErr;
                const StringView ifs = ifsCopy;
                auto             isIfs = [&](size_t k) {
                        return !protectedChars[k] && ifs.contains(line[k]);
                };
                auto isWhite = [&](size_t k) {
                        return isIfs(k) && (line[k] == ' ' || line[k] == '\t' || line[k] == '\n');
                };
                size_t i = 0;
                while (i < line.size() && isWhite(i)) ++i;
                for (size_t n = 0; n < names.size(); ++n) {
                        size_t end = i;
                        if (n + 1 == names.size()) {
                                end = line.size();
                                while (end > i && isWhite(end - 1)) --end;
                        } else {
                                while (end < line.size() && !isIfs(end)) ++end;
                        }
                        String value;
                        for (size_t k = i; k < end; ++k) {
                                if (value.append(line[k]).isError()) return Error::NoMem;
                        }
                        Error err = ex.state().vars.set(names[n], value);
                        if (err.isError()) return err;
                        i = end;
                        while (i < line.size() && isWhite(i)) ++i;
                        if (i < line.size() && isIfs(i)) ++i; // one non-white delimiter
                        while (i < line.size() && isWhite(i)) ++i;
                }
                return Error::Ok;
        }

        // Reads one line from a terminal with a line editor (echo and editing). @p eof at end of
        // input (Ctrl-D on an empty line); BufferTooSmall for a line past LineEditor::LineMax.
        Error readTerminal(Console &console, StringView prompt, String &line, bool &eof) {
                // The editor in PSRAM: about 5 KB, too much for the shell's stack.
                void *memory = Memory::alloc(MemSpace::Psram, sizeof(LineEditor));
                if (memory == nullptr) return Error::NoMem;
                LineEditor &editor = *::new (memory) LineEditor(console);
                struct Freed {
                                LineEditor &e;
                                ~Freed() {
                                        e.~LineEditor();
                                        Memory::free(&e);
                                }
                } freed{editor};
                editor.start(prompt);
                for (;;) {
                        uint8_t buf[16];
                        auto [n, err] = console.read(WritableByteSpan(buf), WaitForever);
                        if (err == Error::EndOfFile) {
                                eof = true;
                                return Error::Ok;
                        }
                        if (err.isError()) return err;
                        for (size_t k = 0; k < n; ++k) {
                                switch (editor.feed(static_cast<char>(buf[k]))) {
                                        case LineEditor::Event::Line: return line.append(editor.line());
                                        case LineEditor::Event::Eof: eof = true; return Error::Ok;
                                        case LineEditor::Event::Cancel: return Error::Interrupted;
                                        case LineEditor::Event::TooLong: return Error::BufferTooSmall;
                                        case LineEditor::Event::None: break;
                                }
                        }
                }
        }

} // namespace

int LangBuiltins::printfCmd(CommandContext &ctx) {
        const Args  args = ctx.args();
        size_t      i = 1;
        const char *variable = nullptr;
        if (i + 1 < args.size() && StringView(args[i]) == "-v") {
                variable = args[i + 1];
                if (!isName(variable)) {
                        ctx.eprintf("printf: `%s': not a valid identifier\n", variable);
                        return Shell::ExitUsage;
                }
                i += 2;
        }
        if (i < args.size() && StringView(args[i]) == "--") ++i;
        if (i >= args.size()) return ctx.usage();
        const StringView format = args[i++];
        const Args       params = args.mid(i);
        size_t           next = 0;
        String           out;
        int              status = Shell::ExitSuccess;
        auto             arg = [&]() -> const char             *{
                return next < params.size() ? params[next++] : nullptr;
        };

        // The format is reused while arguments remain, as long as it uses some.
        bool stop = false;
        do {
                const size_t before = next;
                for (size_t k = 0; k < format.size() && !stop;) {
                        const char c = format[k];
                        if (c == '\\') {
                                stop = !escape(format, k, out, false);
                                continue;
                        }
                        if (c != '%') {
                                (void)out.append(c);
                                ++k;
                                continue;
                        }
                        if (k + 1 < format.size() && format[k + 1] == '%') {
                                (void)out.append('%');
                                k += 2;
                                continue;
                        }
                        // %[flags][width][.precision]conversion
                        String spec;
                        (void)spec.append('%');
                        ++k;
                        while (k < format.size() && StringView("-+ #0").contains(format[k]))
                                (void)spec.append(format[k++]);
                        for (int part = 0; part < 2; ++part) {
                                if (part == 1) {
                                        if (k >= format.size() || format[k] != '.') break;
                                        (void)spec.append(format[k++]);
                                }
                                if (k < format.size() && format[k] == '*') {
                                        long long   v = 0;
                                        const char *a = arg();
                                        if (a != nullptr && !number(ctx, a, v)) status = Shell::ExitFailure;
                                        (void)spec.appendf("%lld", v);
                                        ++k;
                                } else {
                                        while (k < format.size() && format[k] >= '0' && format[k] <= '9')
                                                (void)spec.append(format[k++]);
                                }
                        }
                        if (k >= format.size()) {
                                ctx.eprint("printf: missing format character\n");
                                return Shell::ExitFailure;
                        }
                        const char  conv = format[k++];
                        const char *a = arg();
                        switch (conv) {
                                case 's':
                                case 'b': {
                                        String text;
                                        if (conv == 'b' && a != nullptr) {
                                                const StringView av(a);
                                                for (size_t j = 0; j < av.size() && !stop;) {
                                                        if (av[j] == '\\') {
                                                                stop = !escape(av, j, text, true);
                                                        } else {
                                                                (void)text.append(av[j++]);
                                                        }
                                                }
                                        } else {
                                                (void)text.append(a != nullptr ? a : "");
                                        }
                                        (void)spec.append('s');
                                        (void)out.appendf(spec.cStr(), text.cStr());
                                        break;
                                }
                                case 'c':
                                        (void)spec.append('c');
                                        (void)out.appendf(spec.cStr(), a != nullptr && a[0] != '\0' ? a[0] : '\0');
                                        break;
                                case 'd':
                                case 'i':
                                case 'u':
                                case 'o':
                                case 'x':
                                case 'X': {
                                        long long v = 0;
                                        if (a != nullptr && !number(ctx, a, v)) status = Shell::ExitFailure;
                                        (void)spec.append("ll");
                                        (void)spec.append(conv == 'i' ? 'd' : conv);
                                        if (conv == 'd' || conv == 'i') {
                                                (void)out.appendf(spec.cStr(), v);
                                        } else {
                                                (void)out.appendf(spec.cStr(), static_cast<unsigned long long>(v));
                                        }
                                        break;
                                }
                                default:
                                        ctx.eprintf("printf: %c: invalid format character\n", conv);
                                        return Shell::ExitFailure;
                        }
                }
                if (next == before) break; // the format took no arguments
        } while (next < params.size() && !stop);

        if (variable != nullptr) {
                Error err = ctx.executor().state().vars.set(variable, out);
                if (err.isError()) {
                        ctx.eprintf("printf: %s: %s\n", variable,
                                    err == Error::ReadOnly ? "readonly variable" : "out of memory");
                        return Shell::ExitFailure;
                }
        } else {
                ctx.print(out);
        }
        return status;
}

int LangBuiltins::read(CommandContext &ctx) {
        const Args  args = ctx.args();
        bool        raw = false;
        const char *prompt = "";
        size_t      i = 1;
        for (; i < args.size() && args[i][0] == '-' && args[i][1] != '\0'; ++i) {
                const StringView o = args[i];
                if (o == "--") {
                        ++i;
                        break;
                }
                if (o == "-r") {
                        raw = true;
                } else if (o == "-p" && i + 1 < args.size()) {
                        prompt = args[++i];
                } else {
                        return ctx.usage();
                }
        }
        static char *const replyName[] = {const_cast<char *>("REPLY")};
        const Args         names = i < args.size() ? args.mid(i) : Args(replyName);
        for (const char *n : names) {
                if (!isName(n)) {
                        ctx.eprintf("read: `%s': not a valid identifier\n", n);
                        return Shell::ExitFailure;
                }
        }
        const bool reply = i >= args.size();

        FileHandle *in = ctx.fds().handle(FdTable::Stdin);
        if (in == nullptr || !in->canRead()) {
                ctx.eprint("read: read error: Bad file descriptor\n");
                return Shell::ExitFailure;
        }
        // Characters and whether a backslash protected them from splitting.
        String     line;
        List<bool> protectedChars;
        bool       eof = false;
        Error      err;
        if (in->stream().isTerminal()) {
                // Only consoles are terminals, so this cast is safe.
                Console   &console = static_cast<Console &>(in->stream());
                String     text;
                StringView linePrompt = prompt;
                for (;;) {
                        err = readTerminal(console, linePrompt, text, eof);
                        if (err.isError() || eof || raw || !text.view().endsWith('\\')) break;
                        // A trailing backslash continues the line.
                        text.truncate(text.size() - 1);
                        linePrompt = StringView();
                }
                for (size_t k = 0; err.isOk() && k < text.size(); ++k) {
                        bool prot = false;
                        char c = text[k];
                        if (!raw && c == '\\' && k + 1 < text.size()) {
                                c = text[++k];
                                prot = true;
                        }
                        if (line.append(c).isError() || protectedChars.append(prot).isError()) err = Error::NoMem;
                }
        } else {
                // One byte at a time, so nothing past the line is consumed.
                bool escaped = false;
                for (;;) {
                        uint8_t b = 0;
                        auto [n, rerr] = in->read(WritableByteSpan(&b, 1), WaitForever);
                        if (rerr == Error::EndOfFile) {
                                eof = true;
                                break;
                        }
                        if (rerr.isError()) {
                                err = rerr;
                                break;
                        }
                        if (n == 0) continue;
                        const char c = static_cast<char>(b);
                        if (escaped) {
                                escaped = false;
                                if (c == '\n') continue; // line continuation
                                if (line.append(c).isError() || protectedChars.append(true).isError()) {
                                        err = Error::NoMem;
                                        break;
                                }
                                continue;
                        }
                        if (c == '\n') break;
                        if (!raw && c == '\\') {
                                escaped = true;
                                continue;
                        }
                        if (line.append(c).isError() || protectedChars.append(false).isError()) {
                                err = Error::NoMem;
                                break;
                        }
                }
        }
        if (err == Error::Interrupted || ctx.isInterrupted()) return Shell::ExitInterrupted;
        if (err == Error::BufferTooSmall) {
                ctx.eprintf("read: line too long (%u bytes at most)\n", unsigned(LineEditor::LineMax));
                return Shell::ExitFailure;
        }
        if (err.isError()) {
                ctx.eprintf("read: %s\n", err.desc());
                return Shell::ExitFailure;
        }
        if (reply) {
                err = ctx.executor().state().vars.set("REPLY", line);
        } else {
                err = assignFields(ctx.executor(), line, protectedChars, names);
        }
        if (err.isError()) {
                ctx.eprintf("read: %s\n", err == Error::ReadOnly ? "readonly variable" : "out of memory");
                return Shell::ExitFailure;
        }
        return eof ? Shell::ExitFailure : Shell::ExitSuccess;
}

BMDS_SHELL_COMMAND_AS(printfCmd, "printf", LangBuiltins::printfCmd, "[-v var] format [arg]...",
                      "Print the arguments under the control of the format")
BMDS_SHELL_BUILTIN(read, LangBuiltins::read, "[-r] [-p prompt] [name]...",
                   "Read a line from standard input into variables")

BMDS_NAMESPACE_END
