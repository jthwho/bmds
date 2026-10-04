/**
 * @file      textcommands.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Text built-ins. Output formats follow the GNU tools these are named after
 * and are userspace interfaces: they only ever grow. They read the files
 * named (`-`, or none, is stdin).
 */

#include <cstdint>
#include <bmds/memory.h>
#include <bmds/regex.h>
#include <bmds/shell.h>
#include <bmds/string.h>
#include <bmds/textcommands.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // Inputs are read in big pieces, so a file's whole sectors go from
        // the card straight into the buffer (one transfer each), as cp does.
        constexpr size_t ReadBufferBytes = 32 * 1024;

        // A command's read buffer, in DMA-capable PSRAM.
        class ReadBuffer {
                public:
                        ReadBuffer()
                            : _data(static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, ReadBufferBytes))) {}
                        ~ReadBuffer() { Memory::free(_data); }
                        ReadBuffer(const ReadBuffer &) = delete;
                        ReadBuffer &operator=(const ReadBuffer &) = delete;

                        /** False if there was no memory (reported on stderr here). */
                        bool check(CommandContext &ctx) const {
                                if (_data == nullptr) ctx.eprintf("%s: out of memory\n", ctx.command().name);
                                return _data != nullptr;
                        }

                        WritableByteSpan span() const { return WritableByteSpan(_data, ReadBufferBytes); }

                private:
                        uint8_t *_data;
        };

        bool isSpace(char c) {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
        }

        // Reads an input a line at a time.
        class LineReader {
                public:
                        LineReader(CommandContext &ctx, CommandContext::Input &input, WritableByteSpan buf)
                            : _ctx(ctx), _input(input), _buf(buf) {}

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
                                                auto [n, err] = _input.read(_buf);
                                                if (err == Error::EndOfFile || err == Error::BadDescriptor)
                                                        return any ? Error(Error::Ok) : Error(Error::EndOfFile);
                                                if (err.isError()) return err;
                                                _pos = 0;
                                                _len = n;
                                        }
                                        any = true;
                                        const char  *start = reinterpret_cast<const char *>(_buf.data()) + _pos;
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
                        CommandContext        &_ctx;
                        CommandContext::Input &_input;
                        WritableByteSpan       _buf;
                        size_t                 _pos = 0;
                        size_t                 _len = 0;
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

        // The inputs named by @p files, stdin when there are none. Calls
        // @p fn(name, input) for each that opens (reporting the others) and
        // stops early if it returns false. Returns false if any failed to open.
        template <typename Fn> bool forEachInput(CommandContext &ctx, Args files, Fn &&fn) {
                static char *const Stdin[] = {const_cast<char *>("-")};
                const Args         names = files.isEmpty() ? Args(Stdin) : files;
                bool               ok = true;
                for (const char *name : names) {
                        if (ctx.isInterrupted()) break;
                        CommandContext::Input input;
                        const Error           err = ctx.openInput(name, input);
                        if (err.isError()) {
                                ctx.eprintf("%s: %s: %s\n", ctx.command().name, name, CommandContext::errorText(err));
                                ok = false;
                                continue;
                        }
                        if (!fn(StringView(name), input)) break;
                }
                return ok;
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
        ReadBuffer buffer;
        if (!buffer.check(ctx)) return Shell::ExitFailure;
        Error      failed;
        const bool opened = forEachInput(ctx, ctx.args().mid(1), [&](StringView, CommandContext::Input &input) {
                for (;;) {
                        if (ctx.isInterrupted()) return false;
                        auto [n, err] = input.read(buffer.span());
                        if (err == Error::EndOfFile) return true;
                        if (err.isError()) {
                                failed = err;
                                return false;
                        }
                        (void)ctx.write(FdTable::Stdout, ByteSpan(buffer.span().data(), n));
                }
        });
        if (failed.isError()) return readFailed(ctx, failed);
        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
        return opened ? Shell::ExitSuccess : Shell::ExitFailure;
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
        const Args files = args.mid(i);
        ReadBuffer buffer;
        if (!buffer.check(ctx)) return Shell::ExitFailure;
        String     line;
        Error      failed;
        bool       first = true;
        const bool opened = forEachInput(ctx, files, [&](StringView name, CommandContext::Input &input) {
                // GNU: with several files, a header before each.
                if (files.size() > 1)
                        ctx.printf("%s==> %.*s <==\n", first ? "" : "\n", static_cast<int>(name.size()), name.data());
                first = false;
                LineReader reader(ctx, input, buffer.span());
                for (unsigned long n = 0; n < count; ++n) {
                        Error err = reader.next(line);
                        if (err == Error::EndOfFile) break;
                        if (err.isOk() && line.append('\n').isError()) err = Error::NoMem;
                        if (err.isError()) {
                                failed = err;
                                return false;
                        }
                        ctx.print(line);
                }
                return true;
        });
        if (failed.isError()) return readFailed(ctx, failed);
        if (ctx.isInterrupted()) return Shell::ExitInterrupted;
        return opened ? Shell::ExitSuccess : Shell::ExitFailure;
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
        if (!lines && !words && !bytes) lines = words = bytes = true;
        const Args files = args.mid(i);

        struct Counts {
                        unsigned long values[3] = {}; // lines, words, bytes
        };
        // Up to this many per-file rows are kept, to size the columns once.
        constexpr size_t MaxRows = 64;
        Counts           rows[MaxRows];
        const char      *names[MaxRows] = {};
        size_t           rowCount = 0;
        Counts           total;
        Error            failed;
        ReadBuffer       buffer;
        if (!buffer.check(ctx)) return Shell::ExitFailure;
        const uint8_t *buf = buffer.span().data();
        const bool     opened = forEachInput(ctx, files, [&](StringView name, CommandContext::Input &input) {
                Counts c;
                bool   inWord = false;
                for (;;) {
                        if (ctx.isInterrupted()) return false;
                        auto [n, err] = input.read(buffer.span());
                        if (err == Error::EndOfFile) break;
                        if (err.isError()) {
                                failed = err;
                                return false;
                        }
                        c.values[2] += n;
                        for (size_t k = 0; k < n; ++k) {
                                const char ch = static_cast<char>(buf[k]);
                                if (ch == '\n') ++c.values[0];
                                if (isSpace(ch)) {
                                        inWord = false;
                                } else if (!inWord) {
                                        inWord = true;
                                        ++c.values[1];
                                }
                        }
                }
                for (int k = 0; k < 3; ++k) total.values[k] += c.values[k];
                if (rowCount == MaxRows) return false;
                rows[rowCount] = c;
                names[rowCount++] = name.data();
                return true;
        });
        if (failed.isError()) return readFailed(ctx, failed);
        if (ctx.isInterrupted()) return Shell::ExitInterrupted;

        const bool wanted[3] = {lines, words, bytes};
        const int  selected = int(lines) + int(words) + int(bytes);
        // GNU: stdin alone gets 7-wide columns when several counts are shown
        // (one count is bare); named files get columns as wide as the
        // largest number printed.
        int width = selected == 1 ? 0 : 7;
        if (!files.isEmpty()) {
                width = 1;
                for (int k = 0; k < 3; ++k) {
                        int digits = 1;
                        for (unsigned long v = total.values[k]; v >= 10; v /= 10) ++digits;
                        if (wanted[k] && digits > width) width = digits;
                }
        }
        auto printRow = [&](const Counts &c, const char *name) {
                String out;
                for (int k = 0; k < 3; ++k) {
                        if (!wanted[k]) continue;
                        (void)out.appendf(out.isEmpty() ? "%*lu" : " %*lu", width, c.values[k]);
                }
                if (!files.isEmpty()) (void)out.appendf(" %s", name);
                (void)out.append('\n');
                ctx.print(out);
        };
        for (size_t r = 0; r < rowCount; ++r) printRow(rows[r], names[r]);
        if (files.size() > 1) printRow(total, "total");
        return opened ? Shell::ExitSuccess : Shell::ExitFailure;
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
        const Args       files = args.mid(i);
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

        ReadBuffer buffer;
        if (!buffer.check(ctx)) return Shell::ExitFailure;
        String        line;
        unsigned long selected = 0;
        bool          quit = false;
        Error         failed;
        const bool    named = files.size() > 1; // GNU: "file:" before each line with several files
        const bool    opened = forEachInput(ctx, files, [&](StringView name, CommandContext::Input &input) {
                LineReader    reader(ctx, input, buffer.span());
                unsigned long lineNo = 0;
                unsigned long fileSelected = 0;
                for (;;) {
                        Error err = reader.next(line);
                        if (err == Error::EndOfFile) break;
                        if (err.isError()) {
                                failed = err;
                                return false;
                        }
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
                        ++fileSelected;
                        if (quiet) {
                                quit = true;
                                return false;
                        }
                        if (countOnly) continue;
                        String out;
                        if (named) (void)out.appendf("%.*s:", static_cast<int>(name.size()), name.data());
                        if (numbers) (void)out.appendf("%lu:", lineNo);
                        if (out.append(line).isError() || out.append('\n').isError()) {
                                failed = Error::NoMem;
                                return false;
                        }
                        ctx.print(out);
                        if (ctx.isInterrupted()) return false;
                }
                if (countOnly) {
                        if (named) ctx.printf("%.*s:", static_cast<int>(name.size()), name.data());
                        ctx.printf("%lu\n", fileSelected);
                }
                return true;
        });
        if (quit) return Shell::ExitSuccess;
        if (ctx.isInterrupted() || failed == Error::Interrupted) return Shell::ExitInterrupted;
        // GNU: 2 for trouble (a file that couldn't be read), else 0 if anything matched.
        if (failed.isError()) {
                ctx.eprintf("grep: %s\n", failed.desc());
                return Shell::ExitUsage;
        }
        if (!opened) return Shell::ExitUsage;
        return selected > 0 ? Shell::ExitSuccess : Shell::ExitFailure;
}

BMDS_SHELL_COMMAND(cat, TextCommands::cat, "[file]...", "Copy files (or standard input) to standard output")
BMDS_SHELL_COMMAND(echo, TextCommands::echo, "[-neE] [word]...", "Print the words, separated by spaces")
BMDS_SHELL_COMMAND(grep, TextCommands::grep, "[-civnqF] pattern [file]...",
                   "Print the lines of files (or standard input) that match a basic regular expression")
BMDS_SHELL_COMMAND(head, TextCommands::head, "[-n count] [file]...", "Print the first lines of files (10)")
BMDS_SHELL_COMMAND(wc, TextCommands::wc, "[-lwc] [file]...", "Count lines, words and bytes of files")

BMDS_NAMESPACE_END
