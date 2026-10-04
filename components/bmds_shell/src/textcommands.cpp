/**
 * @file      textcommands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Text built-ins. Output formats follow the GNU tools these are named after
 * and are userspace interfaces: they only ever grow. Reading files arrives
 * with storage (Phase 2); until then they filter stdin.
 */

#include <cstdint>
#include <bmds/regex.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/textcommands.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        constexpr size_t ReadChunkBytes = 256;

        bool isSpace(char c) {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
        }

        // Reads stdin a line at a time.
        class LineReader {
                public:
                        explicit LineReader(CommandContext &ctx) : _ctx(ctx) {}

                        /**
                         * Reads the next line into @p line, without its newline.
                         * Returns Ok, EndOfFile (nothing left), Interrupted or NoMem.
                         */
                        Error next(String &line) {
                                line.truncate(0);
                                bool any = false;
                                for (;;) {
                                        if (_pos == _len) {
                                                if (_ctx.isInterrupted()) return Error::Interrupted;
                                                auto [n, err] = _ctx.read(WritableByteSpan(_buf));
                                                if (err == Error::EndOfFile || err == Error::BadDescriptor)
                                                        return any ? Error(Error::Ok) : Error(Error::EndOfFile);
                                                if (err.isError()) return err;
                                                _pos = 0;
                                                _len = n;
                                        }
                                        any = true;
                                        const char  *start = reinterpret_cast<const char *>(_buf) + _pos;
                                        const size_t avail = _len - _pos;
                                        size_t       n = 0;
                                        while (n < avail && start[n] != '\n') ++n;
                                        Error err = line.append(StringView(start, n));
                                        if (err.isError()) return err;
                                        _pos += n;
                                        if (_pos < _len) {
                                                ++_pos; // the newline
                                                return Error::Ok;
                                        }
                                }
                        }

                private:
                        CommandContext &_ctx;
                        uint8_t         _buf[ReadChunkBytes];
                        size_t          _pos = 0;
                        size_t          _len = 0;
        };

        // Reports a read error (other than Ctrl-C) and picks the exit code.
        int readFailed(CommandContext &ctx, Error err) {
                if (err == Error::Interrupted || ctx.isInterrupted()) return Shell::ExitInterrupted;
                ctx.eprintf("%s: %s\n", ctx.command().name, err.desc());
                return Shell::ExitFailure;
        }

        bool parseCount(const char *text, unsigned long &value) {
                if (*text == '\0') return false;
                unsigned long v = 0;
                for (; *text != '\0'; ++text) {
                        if (*text < '0' || *text > '9') return false;
                        v = v * 10 + static_cast<unsigned long>(*text - '0');
                        if (v > 1000000000ul) return false;
                }
                value = v;
                return true;
        }

        // Files are Phase 2; `-` means stdin.
        bool onlyStdin(CommandContext &ctx, Args files) {
                for (const char *f : files) {
                        if (StringView(f) == "-") continue;
                        ctx.eprintf("%s: %s: No such file or directory\n", ctx.command().name, f);
                        return false;
                }
                return true;
        }

        // `\x` escapes of echo -e. Returns false at `\c` (stop all output).
        bool echoEscaped(StringView text, String &out) {
                for (size_t i = 0; i < text.size(); ++i) {
                        char c = text[i];
                        if (c != '\\' || i + 1 == text.size()) {
                                (void)out.append(c);
                                continue;
                        }
                        c = text[++i];
                        switch (c) {
                                case 'a': (void)out.append('\a'); break;
                                case 'b': (void)out.append('\b'); break;
                                case 'c': return false;
                                case 'e':
                                case 'E': (void)out.append('\x1b'); break;
                                case 'f': (void)out.append('\f'); break;
                                case 'n': (void)out.append('\n'); break;
                                case 'r': (void)out.append('\r'); break;
                                case 't': (void)out.append('\t'); break;
                                case 'v': (void)out.append('\v'); break;
                                case '\\': (void)out.append('\\'); break;
                                case '0': {
                                        unsigned v = 0;
                                        for (int k = 0;
                                             k < 3 && i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '7';
                                             ++k)
                                                v = v * 8 + static_cast<unsigned>(text[++i] - '0');
                                        (void)out.append(static_cast<char>(v));
                                        break;
                                }
                                case 'x': {
                                        unsigned v = 0;
                                        int      digits = 0;
                                        for (; digits < 2 && i + 1 < text.size(); ++digits) {
                                                const char h = text[i + 1];
                                                unsigned   d;
                                                if (h >= '0' && h <= '9') {
                                                        d = static_cast<unsigned>(h - '0');
                                                } else if (h >= 'a' && h <= 'f') {
                                                        d = static_cast<unsigned>(h - 'a' + 10);
                                                } else if (h >= 'A' && h <= 'F') {
                                                        d = static_cast<unsigned>(h - 'A' + 10);
                                                } else {
                                                        break;
                                                }
                                                v = v * 16 + d;
                                                ++i;
                                        }
                                        if (digits == 0) {
                                                (void)out.append("\\x");
                                        } else {
                                                (void)out.append(static_cast<char>(v));
                                        }
                                        break;
                                }
                                default:
                                        (void)out.append('\\');
                                        (void)out.append(c);
                                        break;
                        }
                }
                return true;
        }

} // namespace

int TextCommands::echo(CommandContext &ctx) {
        const Args args = ctx.args();
        bool       newline = true;
        bool       escapes = false;
        size_t     first = 1;
        // As in bash: leading words made only of n, e, E after a '-' are options.
        for (; first < args.size(); ++first) {
                const StringView word = args[first];
                if (word.size() < 2 || word[0] != '-') break;
                bool valid = true;
                for (size_t i = 1; i < word.size(); ++i) valid = valid && StringView("neE").contains(word[i]);
                if (!valid) break;
                for (size_t i = 1; i < word.size(); ++i) {
                        if (word[i] == 'n') newline = false;
                        if (word[i] == 'e') escapes = true;
                        if (word[i] == 'E') escapes = false;
                }
        }
        // Escapes never lengthen the text, so one reservation covers every
        // append below.
        size_t total = 1;
        for (size_t i = first; i < args.size(); ++i) total += StringView(args[i]).size() + 1;
        String out;
        if (out.reserve(total).isError()) return readFailed(ctx, Error::NoMem);
        bool more = true;
        for (size_t i = first; i < args.size() && more; ++i) {
                if (i > first) (void)out.append(' ');
                if (escapes) {
                        more = echoEscaped(args[i], out);
                } else {
                        (void)out.append(args[i]);
                }
        }
        if (more && newline) (void)out.append('\n');
        ctx.print(out);
        return Shell::ExitSuccess;
}

int TextCommands::cat(CommandContext &ctx) {
        if (!onlyStdin(ctx, ctx.args().mid(1))) return Shell::ExitFailure;
        uint8_t buf[ReadChunkBytes];
        for (;;) {
                if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                auto [n, err] = ctx.read(WritableByteSpan(buf));
                if (err == Error::EndOfFile) return Shell::ExitSuccess;
                if (err.isError()) return readFailed(ctx, err);
                (void)ctx.write(FdTable::Stdout, ByteSpan(buf, n));
        }
}

int TextCommands::head(CommandContext &ctx) {
        const Args    args = ctx.args();
        unsigned long count = 10;
        size_t        i = 1;
        if (i < args.size() && StringView(args[i]).startsWith("-n")) {
                const char *value = args[i][2] != '\0' ? args[i] + 2 : (i + 1 < args.size() ? args[++i] : "");
                if (!parseCount(value, count)) return ctx.usage();
                ++i;
        }
        if (!onlyStdin(ctx, args.mid(i))) return Shell::ExitFailure;
        LineReader reader(ctx);
        String     line;
        for (unsigned long n = 0; n < count; ++n) {
                Error err = reader.next(line);
                if (err == Error::EndOfFile) break;
                if (err.isError()) return readFailed(ctx, err);
                if (line.append('\n').isError()) return readFailed(ctx, Error::NoMem);
                ctx.print(line);
        }
        return ctx.isInterrupted() ? Shell::ExitInterrupted : Shell::ExitSuccess;
}

int TextCommands::wc(CommandContext &ctx) {
        const Args args = ctx.args();
        bool       lines = false, words = false, bytes = false;
        size_t     i = 1;
        for (; i < args.size() && args[i][0] == '-' && args[i][1] != '\0'; ++i) {
                for (const char *o = args[i] + 1; *o != '\0'; ++o) {
                        if (*o == 'l') {
                                lines = true;
                        } else if (*o == 'w') {
                                words = true;
                        } else if (*o == 'c') {
                                bytes = true;
                        } else {
                                return ctx.usage();
                        }
                }
        }
        if (!onlyStdin(ctx, args.mid(i))) return Shell::ExitFailure;
        if (!lines && !words && !bytes) lines = words = bytes = true;

        unsigned long nl = 0, nw = 0, nc = 0;
        bool          inWord = false;
        uint8_t       buf[ReadChunkBytes];
        for (;;) {
                if (ctx.isInterrupted()) return Shell::ExitInterrupted;
                auto [n, err] = ctx.read(WritableByteSpan(buf));
                if (err == Error::EndOfFile) break;
                if (err.isError()) return readFailed(ctx, err);
                nc += n;
                for (size_t k = 0; k < n; ++k) {
                        const char c = static_cast<char>(buf[k]);
                        if (c == '\n') ++nl;
                        if (isSpace(c)) {
                                inWord = false;
                        } else if (!inWord) {
                                inWord = true;
                                ++nw;
                        }
                }
        }
        // GNU: one count alone is printed bare; several are padded to 7 columns.
        const int     selected = int(lines) + int(words) + int(bytes);
        const int     width = selected == 1 ? 0 : 7;
        String        out;
        unsigned long values[3] = {nl, nw, nc};
        bool          wanted[3] = {lines, words, bytes};
        for (int k = 0; k < 3; ++k) {
                if (!wanted[k]) continue;
                (void)out.appendf(out.isEmpty() ? "%*lu" : " %*lu", width, values[k]);
        }
        (void)out.append('\n');
        ctx.print(out);
        return Shell::ExitSuccess;
}

int TextCommands::grep(CommandContext &ctx) {
        const Args args = ctx.args();
        bool       ignoreCase = false, invert = false, countOnly = false, numbers = false, quiet = false, fixed = false;
        size_t     i = 1;
        for (; i < args.size() && args[i][0] == '-' && args[i][1] != '\0'; ++i) {
                if (StringView(args[i]) == "--") {
                        ++i;
                        break;
                }
                for (const char *o = args[i] + 1; *o != '\0'; ++o) {
                        switch (*o) {
                                case 'i': ignoreCase = true; break;
                                case 'v': invert = true; break;
                                case 'c': countOnly = true; break;
                                case 'n': numbers = true; break;
                                case 'q': quiet = true; break;
                                case 'F': fixed = true; break;
                                default: return ctx.usage();
                        }
                }
        }
        if (i >= args.size()) return ctx.usage();
        const StringView pattern = args[i++];
        if (!onlyStdin(ctx, args.mid(i))) return Shell::ExitUsage;
        if (!fixed) {
                Error err = Regex::check(pattern);
                if (err == Error::NotSupported) {
                        ctx.eprintf("grep: unsupported pattern: %s\n", pattern.data());
                        return Shell::ExitUsage;
                }
                if (err.isError()) {
                        ctx.eprintf("grep: invalid pattern: %s\n", pattern.data());
                        return Shell::ExitUsage;
                }
        }

        LineReader    reader(ctx);
        String        line;
        unsigned long lineNo = 0, selected = 0;
        for (;;) {
                Error err = reader.next(line);
                if (err == Error::EndOfFile) break;
                if (err.isError()) return err == Error::Interrupted ? Shell::ExitInterrupted : Shell::ExitUsage;
                ++lineNo;
                bool match = false;
                if (fixed) {
                        if (!ignoreCase) {
                                match = line.view().contains(pattern);
                        } else {
                                // Case-folding fixed search: try each position.
                                for (size_t s = 0; !match && s + pattern.size() <= line.size(); ++s) {
                                        size_t k = 0;
                                        while (k < pattern.size()) {
                                                char a = line[s + k], b = pattern[k];
                                                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                                                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                                                if (a != b) break;
                                                ++k;
                                        }
                                        match = k == pattern.size();
                                }
                        }
                } else {
                        match = Regex::search(pattern, line, ignoreCase);
                }
                if (match == invert) continue;
                ++selected;
                if (quiet) return Shell::ExitSuccess;
                if (countOnly) continue;
                if (numbers) {
                        String prefixed;
                        if (prefixed.appendf("%lu:", lineNo).isError() || prefixed.append(line).isError())
                                return readFailed(ctx, Error::NoMem);
                        line = std::move(prefixed);
                }
                if (line.append('\n').isError()) return readFailed(ctx, Error::NoMem);
                ctx.print(line);
                if (ctx.isInterrupted()) return Shell::ExitInterrupted;
        }
        if (countOnly) ctx.printf("%lu\n", selected);
        return selected > 0 ? Shell::ExitSuccess : Shell::ExitFailure;
}

BMDS_SHELL_COMMAND(cat, TextCommands::cat, "[-]", "Copy standard input to standard output")
BMDS_SHELL_COMMAND(echo, TextCommands::echo, "[-neE] [word]...", "Print the words, separated by spaces")
BMDS_SHELL_COMMAND(grep, TextCommands::grep, "[-civnqF] pattern",
                   "Print the lines of standard input that match a basic regular expression")
BMDS_SHELL_COMMAND(head, TextCommands::head, "[-n count]", "Print the first lines of standard input (10)")
BMDS_SHELL_COMMAND(wc, TextCommands::wc, "[-lwc]", "Count lines, words and bytes of standard input")

BMDS_NAMESPACE_END
