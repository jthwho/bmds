/**
 * @file      terminal.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The terminal model: UTF-8, colors, SGR, the VT parser, the color filter,
 * the line editor's UTF-8 handling, and `term`.
 */

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>
#include <bmds/colorfilter.h>
#include <bmds/langbuiltins.h>
#include <bmds/lineeditor.h>
#include <bmds/shell.h>
#include <bmds/textcommands.h>
#include <bmds/utf8.h>
#include <bmds/vtparser.h>
#include "fakeconsole.h"

using bmds::Color;
using bmds::ColorDepth;
using bmds::StringView;
using bmds::TextStyle;
namespace Utf8 = bmds::Utf8;

TEST_CASE("UTF-8: decode and encode") {
        const std::string s = "a\xC3\xA9\xE6\x97\xA5\xF0\x9F\x98\x80"; // a é 日 😀
        size_t            i = 0;
        StringView        v(s.data(), s.size());
        CHECK(Utf8::decode(v, i) == 'a');
        CHECK(Utf8::decode(v, i) == 0xE9);
        CHECK(Utf8::decode(v, i) == 0x65E5);
        CHECK(Utf8::decode(v, i) == 0x1F600);
        CHECK(i == s.size());
        char out[4];
        CHECK(Utf8::encode(0x1F600, out) == 4);
        CHECK(std::string(out, 4) == "\xF0\x9F\x98\x80");
        CHECK(Utf8::encode(0xD800, out) == 3); // a surrogate becomes U+FFFD
        CHECK(std::string(out, 3) == "\xEF\xBF\xBD");

        // Malformed: each bad byte is one replacement.
        for (const char *bad : {"\x80", "\xC0\xAF", "\xED\xA0\x80", "\xF5\x80\x80\x80", "\xE6\x97"}) {
                const StringView b(bad);
                size_t           k = 0;
                CHECK(Utf8::decode(b, k) == Utf8::Replacement);
                CHECK(k == 1);
        }
}

TEST_CASE("UTF-8: byte-at-a-time decoder") {
        bmds::Utf8Decoder     d;
        std::vector<uint32_t> got;
        for (unsigned char c : std::string("\xC3\xA9x\xE6\x97"
                                           "y\xF0\x9F\x98\x80")) {
                uint32_t     out[2];
                const size_t n = d.feed(c, out);
                for (size_t k = 0; k < n; ++k) got.push_back(out[k]);
        }
        CHECK(got == std::vector<uint32_t>{0xE9, 'x', Utf8::Replacement, 'y', 0x1F600});
}

TEST_CASE("UTF-8: display width") {
        CHECK(Utf8::displayWidth('a') == 1);
        CHECK(Utf8::displayWidth(0xE9) == 1);
        CHECK(Utf8::displayWidth(0x0301) == 0); // combining acute
        CHECK(Utf8::displayWidth(0x200B) == 0); // zero width space
        CHECK(Utf8::displayWidth(0x65E5) == 2); // CJK
        CHECK(Utf8::displayWidth(0xAC00) == 2); // Hangul
        CHECK(Utf8::displayWidth(0x1F600) == 2);
        CHECK(Utf8::displayWidth('\t') == 0);
        CHECK(Utf8::displayWidth(StringView("e\xCC\x81\xE6\x97\xA5")) == 3);
}

TEST_CASE("Color: downgrade") {
        CHECK(Color::rgb(255, 0, 0).downgrade(ColorDepth::Ansi16) == Color::indexed(9));
        CHECK(Color::rgb(205, 0, 0).downgrade(ColorDepth::Ansi16) == Color::indexed(1));
        CHECK(Color::rgb(255, 0, 0).downgrade(ColorDepth::Ansi256) == Color::indexed(196));
        CHECK(Color::rgb(128, 128, 128).downgrade(ColorDepth::Ansi256) == Color::indexed(244));
        CHECK(Color::indexed(196).downgrade(ColorDepth::Ansi16) == Color::indexed(9));
        CHECK(Color::indexed(5).downgrade(ColorDepth::Ansi16) == Color::indexed(5));
        CHECK(Color::rgb(1, 2, 3).downgrade(ColorDepth::None).isDefault());
        CHECK(Color::rgb(1, 2, 3).downgrade(ColorDepth::TrueColor) == Color::rgb(1, 2, 3));
        CHECK(Color::indexed(21).toRgb() == Color::rgb(0, 0, 255));
        CHECK(Color::indexed(232).toRgb() == Color::rgb(8, 8, 8));
}

namespace {

        // Records parser events as text.
        class Recorder final : public bmds::VtHandler {
                public:
                        void print(uint32_t c) override {
                                char         buf[4];
                                const size_t n = Utf8::encode(c, buf);
                                text.append(buf, n);
                        }
                        void execute(uint8_t c) override { text += "<C" + std::to_string(c) + ">"; }
                        void csiDispatch(const bmds::VtParams &p, StringView inter, uint8_t final) override {
                                text += "<CSI";
                                if (p.privateMarker) text += static_cast<char>(p.privateMarker);
                                for (size_t i = 0; i < p.count; ++i)
                                        text += (i == 0 ? "" : (p.isSub(i) ? ":" : ";")) + std::to_string(p.values[i]);
                                text += std::string(inter.data(), inter.size());
                                text += static_cast<char>(final);
                                text += ">";
                        }
                        void escDispatch(StringView inter, uint8_t final) override {
                                text += "<ESC" + std::string(inter.data(), inter.size()) + static_cast<char>(final) +
                                        ">";
                        }
                        void oscDispatch(StringView data) override {
                                text += "<OSC" + std::string(data.data(), data.size()) + ">";
                        }
                        std::string text;
        };

        std::string parse(const std::string &in, bool byteByByte = false) {
                Recorder       r;
                bmds::VtParser p(r);
                if (byteByByte) {
                        for (unsigned char c : in) p.feed(c);
                } else {
                        p.feed(StringView(in.data(), in.size()));
                }
                return r.text;
        }

} // namespace

TEST_CASE("VtParser") {
        const struct {
                        const char *in;
                        const char *out;
        } cases[] = {
                {"plain", "plain"},
                {"a\r\nb\tc\a", "a<C13><C10>b<C9>c<C7>"},
                {"\x1b[m\x1b[1;31m", "<CSIm><CSI1;31m>"},
                {"\x1b[;5H", "<CSI0;5H>"},
                {"\x1b[?25l\x1b[>c", "<CSI?25l><CSI>c>"},
                {"\x1b[38:2::10:20:30m", "<CSI38:2:0:10:20:30m>"},
                {"\x1b[1 q", "<CSI1 q>"},
                {"\x1b"
                 "7\x1b(B",
                 "<ESC7><ESC(B>"},
                {"\x1b]0;title\a\x1b]2;x\x1b\\", "<OSC0;title><OSC2;x><ESC\\>"},
                {"\x1bPqignored\x1b\\after", "<ESC\\>after"},
                {"\x1b[12\x18x", "<C24>x"},        // CAN aborts
                {"\x1b[1;2\nm", "<C10><CSI1;2m>"}, // a control inside a sequence runs
                {"\x1b[1<5m", ""},                 // a private marker in the middle: ignored
                {"\x1b[99999m", "<CSI65535m>"},    // clamped
                {"\xC3\xA9\xE6\x97\xA5", "\xC3\xA9\xE6\x97\xA5"},
                {"\xC3\x1b[m", "\xEF\xBF\xBD<CSIm>"}, // ESC cuts a character short
        };
        for (const auto &c : cases) {
                CAPTURE(c.in);
                CHECK(parse(c.in) == c.out);
                CHECK(parse(c.in, true) == c.out);
        }
        std::string many = "\x1b[";
        for (int i = 0; i < 40; ++i) many += "1;";
        many += "m";
        CHECK(parse(many).rfind("<CSI1;1", 0) == 0); // extra parameters are dropped, not fatal
}

namespace {

        std::string sgr(const TextStyle &from, const TextStyle &to, ColorDepth depth) {
                char         out[bmds::Sgr::MaxSequence];
                const size_t n = bmds::Sgr::transition(from, to, depth, out);
                return std::string(out, n);
        }

        TextStyle apply(const char *seq, TextStyle style = TextStyle()) {
                struct Apply final : bmds::VtHandler {
                                TextStyle *s;
                                void       print(uint32_t) override {}
                                void       execute(uint8_t) override {}
                                void       csiDispatch(const bmds::VtParams &p, StringView, uint8_t f) override {
                                        if (f == 'm') bmds::Sgr::apply(p, *s);
                                }
                                void escDispatch(StringView, uint8_t) override {}
                                void oscDispatch(StringView) override {}
                } h;
                h.s = &style;
                bmds::VtParser p(h);
                p.feed(seq);
                return style;
        }

} // namespace

TEST_CASE("SGR: apply") {
        TextStyle s = apply("\x1b[1;4;31;48;5;200m");
        CHECK(s.attributes == (TextStyle::Bold | TextStyle::Underline));
        CHECK(s.foreground == Color::indexed(1));
        CHECK(s.background == Color::indexed(200));
        s = apply("\x1b[22;24;39;49;91m", s);
        CHECK(s.attributes == 0);
        CHECK(s.foreground == Color::indexed(9));
        CHECK(s.background.isDefault());
        CHECK(apply("\x1b[38;2;1;2;3m").foreground == Color::rgb(1, 2, 3));
        CHECK(apply("\x1b[38:2::1:2:3m").foreground == Color::rgb(1, 2, 3));
        CHECK(apply("\x1b[38:2:1:2:3m").foreground == Color::rgb(1, 2, 3));
        CHECK(apply("\x1b[4:3m").attributes == TextStyle::Underline);
        CHECK(apply("\x1b[1m\x1b[m").attributes == 0);
        CHECK(apply("\x1b[38;5m").foreground.isDefault()); // truncated: ignored
}

TEST_CASE("SGR: transitions are minimal and round-trip") {
        TextStyle red;
        red.foreground = Color::rgb(255, 0, 0);
        CHECK(sgr(TextStyle(), red, ColorDepth::TrueColor) == "\x1b[38;2;255;0;0m");
        CHECK(sgr(TextStyle(), red, ColorDepth::Ansi256) == "\x1b[38;5;196m");
        CHECK(sgr(TextStyle(), red, ColorDepth::Ansi16) == "\x1b[91m");
        CHECK(sgr(TextStyle(), red, ColorDepth::None) == "");
        CHECK(sgr(red, TextStyle(), ColorDepth::Ansi16) == "\x1b[0m"); // shorter than 39
        TextStyle bold;
        bold.attributes = TextStyle::Bold;
        TextStyle boldDim = bold;
        boldDim.attributes |= TextStyle::Dim;
        CHECK(sgr(boldDim, bold, ColorDepth::TrueColor) == "\x1b[0;1m"); // shorter than 22;1 (22 clears both)
        TextStyle boldRed = bold;
        boldRed.foreground = Color::indexed(1);
        TextStyle boldDimRed = boldRed;
        boldDimRed.attributes |= TextStyle::Dim;
        CHECK(sgr(boldDimRed, boldRed, ColorDepth::TrueColor) == "\x1b[22;1m"); // here the increment is shorter
        CHECK(sgr(bold, bold, ColorDepth::TrueColor) == "");

        // Round trip: apply(transition(a, b)) to a gives b, for many pairs.
        const Color colors[] = {Color(), Color::indexed(3), Color::indexed(12), Color::indexed(100),
                                Color::rgb(10, 200, 30)};
        for (uint8_t attrsA : {0, 1, 3, 0x85, 0xFF}) {
                for (uint8_t attrsB : {0, 2, 0x40, 0x7E}) {
                        for (const Color &fa : colors) {
                                for (const Color &fb : colors) {
                                        TextStyle a, b;
                                        a.attributes = attrsA;
                                        a.foreground = fa;
                                        a.background = fb;
                                        b.attributes = attrsB;
                                        b.foreground = fb;
                                        const std::string seq = sgr(a, b, ColorDepth::TrueColor);
                                        CHECK(apply(seq.c_str(), a) == b);
                                }
                        }
                }
        }
}

namespace {

        std::string filter(const std::string &in, ColorDepth depth, bool byteByByte = false) {
                bmds::ColorFilter f(depth);
                std::string       out;
                auto              sink = [](void *ctx, bmds::ByteSpan d) {
                        static_cast<std::string *>(ctx)->append(reinterpret_cast<const char *>(d.data()), d.size());
                };
                const auto *bytes = reinterpret_cast<const uint8_t *>(in.data());
                if (byteByByte) {
                        for (size_t i = 0; i < in.size(); ++i) f.process(bmds::ByteSpan(bytes + i, 1), sink, &out);
                } else {
                        f.process(bmds::ByteSpan(bytes, in.size()), sink, &out);
                }
                return out;
        }

} // namespace

TEST_CASE("ColorFilter") {
        const std::string in =
                "\x1b[38;2;255;0;0mred\x1b[0m plain \xE6\x97\xA5 \x1b[2J\x1b]0;title\a\x1b[1;38;5;21mb\x1b[m";
        for (bool split : {false, true}) {
                CAPTURE(split);
                CHECK(filter(in, ColorDepth::Ansi16, split) ==
                      "\x1b[91mred\x1b[0m plain \xE6\x97\xA5 \x1b[2J\x1b]0;title\a\x1b[1;34mb\x1b[0m");
                CHECK(filter(in, ColorDepth::Ansi256, split) ==
                      "\x1b[38;5;196mred\x1b[0m plain \xE6\x97\xA5 \x1b[2J\x1b]0;title\a\x1b[1;38;5;21mb\x1b[0m");
                CHECK(filter(in, ColorDepth::None, split) ==
                      "red plain \xE6\x97\xA5 \x1b[2J\x1b]0;title\a\x1b[1mb\x1b[0m");
        }
        // A sequence too long to hold passes through untouched.
        const std::string longTitle = "\x1b]0;" + std::string(200, 't') + "\a" + "after";
        CHECK(filter(longTitle, ColorDepth::Ansi16) == longTitle);
        CHECK(filter(longTitle, ColorDepth::Ansi16, true) == longTitle);
}

TEST_CASE("LineEditor: UTF-8") {
        FakeConsole      con;
        bmds::LineEditor ed(con);
        ed.start("");
        for (char c : std::string("a\xC3\xA9\xE6\x97\xA5")) (void)ed.feed(c); // a é 日
        CHECK(std::string(ed.line().data(), ed.line().size()) == "a\xC3\xA9\xE6\x97\xA5");
        CHECK(con.take() == "a\xC3\xA9\xE6\x97\xA5");
        (void)ed.feed('\x02'); // Ctrl-B over the wide character: two columns
        CHECK(ed.cursor() == 3);
        CHECK(con.take() == "\x1b[2D");
        (void)ed.feed('\x02'); // over é: one column
        CHECK(ed.cursor() == 1);
        CHECK(con.take() == "\x1b[1D");
        (void)ed.feed('\x04'); // Ctrl-D deletes é whole
        CHECK(std::string(ed.line().data(), ed.line().size()) == "a\xE6\x97\xA5");
        CHECK(con.take() == "\xE6\x97\xA5\x1b[K\x1b[2D");
        (void)ed.feed('\x05'); // to the end
        (void)ed.feed('\x7f'); // backspace removes 日 whole
        CHECK(std::string(ed.line().data(), ed.line().size()) == "a");
        // A stray continuation byte and a broken sequence are dropped.
        for (char c : std::string("\x80\xC3z")) (void)ed.feed(c);
        CHECK(std::string(ed.line().data(), ed.line().size()) == "az");
}

namespace {

        namespace L = bmds::LangBuiltins;
        namespace T = bmds::TextCommands;

        constexpr bmds::Command TermCommands[] = {
                {"clear", "", "", L::clear},
                {"echo", "", "", T::echo},
                {"reset", "", "", L::reset},
                {"term", "[color [none|16|256|truecolor] | size [COLSxROWS]]", "", L::term},
        };

} // namespace

TEST_CASE("Console: a size change raises resized()") {
        FakeConsole    console;
        const uint32_t generation = console.sizeGeneration();
        console.setSize(bmds::Console::Size{100, 40});
        CHECK(console.resized().raises() == 1);
        CHECK(console.sizeGeneration() != generation);
        console.setSize(bmds::Console::Size{100, 40}); // the same: nothing
        CHECK(console.resized().raises() == 1);
}

TEST_CASE("term") {
        FakeConsole con;
        auto        shell = std::make_unique<bmds::Shell>(con, bmds::Span<const bmds::Command>(TermCommands), 1 << 20);
        REQUIRE(shell->init().isOk());
        auto run = [&](const char *line) {
                (void)shell->execute(line);
                return con.take();
        };
        CHECK(run("term") == "terminal: fake\ncolor:    truecolor\nsize:     80x24\n");
        CHECK(run("echo $TERM $COLORTERM") == "xterm-256color truecolor\n");
        CHECK(run("term color 16; term color") == "16\n");
        CHECK(con.colorDepth() == ColorDepth::Ansi16);
        CHECK(run("echo [$COLORTERM]") == "[]\n");
        CHECK(run("term color 24bit; echo $COLORTERM") == "truecolor\n");
        CHECK(run("term color purple") == "usage: term [color [none|16|256|truecolor] | size [COLSxROWS]]\n");
        CHECK(run("term size 132x43; echo $COLUMNS $LINES") == "132x43\n132 43\n");
        CHECK(con.size().columns == 132);
        // Not a terminal that can be asked.
        CHECK(run("term size") == "term: the terminal didn't report its size\n");
        con.caps |= bmds::Console::SizeQuery;
        con.input = "\x1b[8;50;100t";
        CHECK(run("term size") == "\x1b[18t100x50\n");
        CHECK(con.size().rows == 50);
        CHECK(run("term size") == "\x1b[18tterm: the terminal didn't report its size\n");
        CHECK(run("clear") == "\x1b[H\x1b[2J\x1b[3J");
        CHECK(run("reset") == "\x1b" "c\x1b[18t");
}

namespace {

        // Reads everything @p con has for its reader, @p chunk bytes at a time.
        std::string drain(FakeConsole &con, size_t chunk) {
                std::string out;
                uint8_t     buf[64];
                for (;;) {
                        auto [n, err] = con.read(bmds::WritableByteSpan(buf, chunk), 50);
                        if (err.isError()) break;
                        out.append(reinterpret_cast<char *>(buf), n);
                }
                return out;
        }

} // namespace

TEST_CASE("Console: the size answer is taken out of the input") {
        for (size_t chunk : {size_t(64), size_t(1), size_t(3)}) {
                CAPTURE(chunk);
                FakeConsole con;
                con.caps |= bmds::Console::SizeQuery;
                // No request: an answer-like sequence is input like any other.
                con.input = "a\x1b[8;5;6tb";
                CHECK(drain(con, chunk) == "a\x1b[8;5;6tb");
                CHECK(con.size().columns == 80);
                // A request: the answer goes, typed keys around it stay, in order.
                const uint32_t gen = con.sizeGeneration();
                con.requestSize();
                CHECK(con.take() == "\x1b[18t");
                CHECK(con.isSizeRequestPending());
                con.input += "ls\x1b[A\x1b[8;40;120tq";
                CHECK(drain(con, chunk) == "ls\x1b[Aq");
                CHECK(con.size().columns == 120);
                CHECK(con.size().rows == 40);
                CHECK(con.sizeGeneration() != gen);
                CHECK_FALSE(con.isSizeRequestPending());
                // A lone ESC (vi's) while waiting comes through, as do broken answers.
                con.requestSize();
                con.input += "\x1b\x1b[8;x\x1b[8;1;2t";
                CHECK(drain(con, chunk) == "\x1b\x1b[8;x");
                CHECK(con.size().columns == 2);
                // An answer that never comes: the ESC held meanwhile comes out at the end.
                con.requestSize();
                con.input += "\x1b[8;12";
                CHECK(drain(con, chunk) == "\x1b[8;12");
        }
}

TEST_CASE("Shell: COLUMNS and LINES follow the terminal") {
        FakeConsole con;
        con.caps |= bmds::Console::SizeQuery;
        auto shell = std::make_unique<bmds::Shell>(con, bmds::Span<const bmds::Command>(TermCommands), 1 << 20);
        REQUIRE(shell->init().isOk());
        shell->start();
        CHECK(con.take().find("\x1b[18t") != std::string::npos); // asked at the prompt
        // The answer arrives ahead of the keys of the next line, read a byte at a time as the shell does.
        con.input += "\x1b[8;33;99techo $COLUMNS $LINES\r";
        for (;;) {
                uint8_t c = 0;
                auto [n, err] = con.read(bmds::WritableByteSpan(&c, 1), 50);
                if (err.isError()) break;
                shell->feed(static_cast<char>(c));
        }
        const std::string out = con.take();
        CHECK_MESSAGE(out.find("99 33\n") != std::string::npos, out);
}
