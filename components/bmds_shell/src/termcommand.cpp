/**
 * @file      termcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `term`: the controlling terminal's color depth and size. Its output and
 * arguments are a userspace interface.
 */

#include <cstdio>
#include <bmds/console.h>
#include <bmds/executor.h>
#include <bmds/langbuiltins.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        // How long `term size` waits for the terminal's answer.
        constexpr unsigned int ProbeTimeoutMs = 500;

        bool parseDepth(StringView s, ColorDepth &out) {
                if (s == "none") {
                        out = ColorDepth::None;
                } else if (s == "16") {
                        out = ColorDepth::Ansi16;
                } else if (s == "256") {
                        out = ColorDepth::Ansi256;
                } else if (s == "truecolor" || s == "24bit") {
                        out = ColorDepth::TrueColor;
                } else {
                        return false;
                }
                return true;
        }

        bool parseNumber(StringView s, uint16_t &out) {
                if (s.isEmpty() || s.size() > 4) return false;
                unsigned v = 0;
                for (char c : s) {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + static_cast<unsigned>(c - '0');
                }
                if (v == 0) return false;
                out = static_cast<uint16_t>(v);
                return true;
        }

        // The variables programs look at: TERM, COLORTERM, COLUMNS, LINES.
        void exportTerminal(Executor &ex, const Console &console) {
                Variables &vars = ex.state().vars;
                if (console.colorDepth() == ColorDepth::TrueColor) {
                        (void)vars.set("COLORTERM", "truecolor");
                        (void)vars.addFlags("COLORTERM", Variables::Exported);
                } else {
                        (void)vars.unset("COLORTERM");
                }
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%u", console.size().columns);
                (void)vars.set("COLUMNS", buf);
                std::snprintf(buf, sizeof(buf), "%u", console.size().rows);
                (void)vars.set("LINES", buf);
                return;
        }

        // Asks the terminal for its size (ESC [ 18 t; the answer is ESC [ 8 ; rows ; cols t).
        Error probeSize(Console &console, Console::Size &size) {
                (void)console.write(StringView("\x1b[18t"), ProbeTimeoutMs);
                char   reply[24];
                size_t n = 0;
                for (;;) {
                        uint8_t b = 0;
                        auto [got, err] = console.read(WritableByteSpan(&b, 1), ProbeTimeoutMs);
                        if (err.isError()) return err;
                        if (got == 0) continue;
                        if (n == 0 && b != 0x1B) continue; // typed-ahead input before the reply
                        if (n < sizeof(reply)) reply[n++] = static_cast<char>(b);
                        if (b == 't') break;
                }
                const StringView r(reply, n);
                if (!r.startsWith("\x1b[8;")) return Error::Invalid;
                const StringView body = r.mid(4, r.size() - 5);
                const size_t     semi = body.indexOf(';');
                if (semi == StringView::NPos) return Error::Invalid;
                if (!parseNumber(body.left(semi), size.rows) || !parseNumber(body.mid(semi + 1), size.columns))
                        return Error::Invalid;
                return Error::Ok;
        }

} // namespace

int LangBuiltins::term(CommandContext &ctx) {
        const Args args = ctx.args();
        Console   &console = ctx.shell().console();
        Executor  &ex = ctx.executor();
        if (args.size() == 1) {
                const Console::Size size = console.size();
                ctx.printf("terminal: %s\ncolor:    %s\nsize:     %ux%u\n", console.name(),
                           colorDepthName(console.colorDepth()), size.columns, size.rows);
                return Shell::ExitSuccess;
        }
        const StringView what = args[1];
        if (what == "color" && args.size() <= 3) {
                if (args.size() == 2) {
                        ctx.printf("%s\n", colorDepthName(console.colorDepth()));
                        return Shell::ExitSuccess;
                }
                ColorDepth depth;
                if (!parseDepth(args[2], depth)) return ctx.usage();
                console.setColorDepth(depth);
                exportTerminal(ex, console);
                return Shell::ExitSuccess;
        }
        if (what == "size" && args.size() <= 3) {
                Console::Size size = console.size();
                if (args.size() == 3) {
                        const StringView v = args[2];
                        const size_t     x = v.indexOf('x');
                        if (x == StringView::NPos || !parseNumber(v.left(x), size.columns) ||
                            !parseNumber(v.mid(x + 1), size.rows))
                                return ctx.usage();
                } else {
                        Error err = probeSize(console, size);
                        if (err == Error::Interrupted || ctx.isInterrupted()) return Shell::ExitInterrupted;
                        if (err.isError()) {
                                ctx.eprint("term: the terminal didn't report its size\n");
                                return Shell::ExitFailure;
                        }
                }
                console.setSize(size);
                exportTerminal(ex, console);
                ctx.printf("%ux%u\n", size.columns, size.rows);
                return Shell::ExitSuccess;
        }
        return ctx.usage();
}

BMDS_SHELL_COMMAND(term, LangBuiltins::term, "[color [none|16|256|truecolor] | size [COLSxROWS]]",
                   "Show or set the terminal's color depth and size (size alone asks the terminal)")

BMDS_NAMESPACE_END
