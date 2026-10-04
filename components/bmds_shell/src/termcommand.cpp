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
#include <bmds/deadline.h>
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

        // Asks the terminal for its size and waits for the answer, which the
        // console takes out of the input as it is read. Ok if it answered.
        Error probeSize(Console &console) {
                console.resetSizeRequests();
                console.requestSize();
                if (!console.isSizeRequestPending()) return Error::NotSupported;
                Deadline deadline(ProbeTimeoutMs);
                while (console.isSizeRequestPending() && !deadline.isExpired()) {
                        // Keys typed meanwhile are dropped, as at a prompt that isn't there yet.
                        uint8_t b = 0;
                        auto [n, err] = console.read(WritableByteSpan(&b, 1), ProbeTimeoutMs);
                        (void)n;
                        if (err == Error::Interrupted) return err;
                        if (err.isError()) break;
                }
                return console.isSizeRequestPending() || console.unansweredSizeRequests() > 0 ? Error::Timeout : Error::Ok;
        }

} // namespace

int LangBuiltins::term(CommandContext &ctx) {
        const Args args = ctx.args();
        Console   &console = ctx.shell().console();
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
                ctx.shell().exportTerminal();
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
                        console.setSize(size);
                } else {
                        const Error err = probeSize(console);
                        if (err == Error::Interrupted || ctx.isInterrupted()) return Shell::ExitInterrupted;
                        if (err.isError()) {
                                ctx.eprint("term: the terminal didn't report its size\n");
                                return Shell::ExitFailure;
                        }
                        size = console.size();
                }
                ctx.shell().exportTerminal();
                ctx.printf("%ux%u\n", size.columns, size.rows);
                return Shell::ExitSuccess;
        }
        return ctx.usage();
}

int LangBuiltins::clear(CommandContext &ctx) {
        if (ctx.args().size() != 1) return ctx.usage();
        // Home, erase the screen, erase the scrollback (xterm's E3).
        ctx.print("\x1b[H\x1b[2J\x1b[3J");
        return Shell::ExitSuccess;
}

int LangBuiltins::reset(CommandContext &ctx) {
        if (ctx.args().size() != 1) return ctx.usage();
        Console &console = ctx.shell().console();
        // The terminal's full reset (RIS: modes, character sets, colors, the
        // screen), then the console's own modes as a session starts them, and
        // the size asked for again (also after a terminal that didn't answer).
        (void)console.write(StringView("\x1b" "c"), ProbeTimeoutMs);
        console.setMode(Console::TermMode());
        console.resetSizeRequests();
        console.requestSize();
        return Shell::ExitSuccess;
}

BMDS_SHELL_COMMAND(clear, LangBuiltins::clear, "", "Clear the screen and its scrollback")
BMDS_SHELL_COMMAND(reset, LangBuiltins::reset, "", "Reset the terminal and the console's modes")
BMDS_SHELL_COMMAND(term, LangBuiltins::term, "[color [none|16|256|truecolor] | size [COLSxROWS]]",
                   "Show or set the terminal's color depth and size (size alone asks the terminal)")

BMDS_NAMESPACE_END
