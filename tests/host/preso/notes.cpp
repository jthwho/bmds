/**
 * @file      notes.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Presenting from a laptop (devplan/preso.md §6b, step 8): the terminal's keys decoded and
 * mapped, the notes and the timer's pace, and the presenter's notes, terminal keys, pointer dot
 * and ink, on a platform with a screen buffer and a clock of the test's.
 */

#include <doctest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/notes.h"
#include "core/presenter.h"
#include "core/terminal.h"
#include "headless/headless.h"

using preso::Action;
using preso::Canvas;
using preso::Color;
using preso::Error;
using preso::Event;
using preso::InputMode;
using preso::PixelFormat;
using preso::Presenter;
using preso::Rect;
using preso::StringView;

namespace {

        constexpr int64_t Ms = 1000000;
        constexpr int64_t Second = 1000 * Ms;

        // The deck's text from memory, a screen, the clock and ticks the test's.
        class NotesPlatform : public preso::HeadlessPlatform {
                public:
                        const char *text = "";
                        int64_t     clock = 0;
                        bool        animating = false;
                        bool        ticking = false;
                        uint32_t   *screen = nullptr;
                        int32_t     width = 0, height = 0;
                        Canvas      last;

                        ~NotesPlatform() override { free(screen); }
                        Error readFile(const char *, uint32_t, char **data, uint32_t *length) override {
                                size_t n = strlen(text);
                                *data = static_cast<char *>(malloc(n + 1));
                                memcpy(*data, text, n + 1);
                                *length = uint32_t(n);
                                return Error::Ok;
                        }
                        PixelFormat pixelFormat() const override { return PixelFormat::Xrgb8888; }
                        Error       openScreen(int32_t w, int32_t h, const char *, bool) override {
                                width = w;
                                height = h;
                                free(screen);
                                screen = static_cast<uint32_t *>(calloc(size_t(w) * size_t(h), 4));
                                last.attach(screen, uint32_t(w) * 4, w, h, PixelFormat::Xrgb8888);
                                opens++;
                                return Error::Ok;
                        }
                        void closeScreen() override {
                                free(screen);
                                screen = nullptr;
                                last = Canvas();
                        }
                        uint32_t opens = 0;
                        void    present(const Canvas &c) override { last.copyFrom(c); }
                        void    presentAreas(const Canvas &c, const Rect *, uint32_t) override { last.copyFrom(c); }
                        void    animate(bool on) override { animating = on; }
                        void    setTicking(bool on) override { ticking = on; }
                        // Demos: the command and folder asked for, started or refused as the test says.
                        char  demo[256] = {};
                        char  demoFolder[256] = {};
                        bool  demoStarts = true;
                        Error startDemo(const char *command, const char *folder) override {
                                snprintf(demo, sizeof demo, "%s", command);
                                snprintf(demoFolder, sizeof demoFolder, "%s", folder);
                                return demoStarts ? Error::Ok : Error::NotFound;
                        }
                        int64_t now() override { return clock; }
                        Error   watch(const char *) override { return Error::Ok; }
                        Error   run(preso::App &) override { return Error::Ok; }
        };

        // Every key a terminal's bytes make.
        uint32_t decode(const char *bytes, Event *keys, uint32_t max) {
                preso::TerminalKeys t;
                uint32_t            n = 0;
                for (const char *p = bytes; *p != '\0' && n < max; p++) {
                        if (t.push(*p, &keys[n])) n++;
                }
                return n;
        }

        uint32_t count(const Canvas &c, Color color) {
                uint32_t n = 0;
                for (int32_t y = 0; y < c.height(); y++) {
                        for (int32_t x = 0; x < c.width(); x++) n += c.colorAt(x, y) == color;
                }
                return n;
        }

        Event pointerAt(int32_t x, int32_t y, uint32_t buttons) {
                Event e;
                e.type = Event::Type::Pointer;
                e.x = x;
                e.y = y;
                e.buttons = buttons;
                return e;
        }

} // namespace

TEST_CASE("Terminal: bytes become the keys they are") {
        Event    keys[16];
        uint32_t n = decode("\033[C\033[D\033OA\033[B\033[H\033[4~\033[5~\033[6~", keys, 16);
        REQUIRE(n == 8);
        const uint32_t usages[] = {0x4F, 0x50, 0x52, 0x51, 0x4A, 0x4D, 0x4B, 0x4E};
        for (uint32_t i = 0; i < n; i++) CHECK(keys[i].key == (0x70000 | usages[i]));
        n = decode("nG3\r\x7f ", keys, 16);
        REQUIRE(n == 6);
        CHECK(keys[0].key == 0x70011);
        CHECK(keys[0].character == 'n');
        CHECK(keys[1].character == 'G');
        CHECK(keys[2].key == 0x70020); // 3
        CHECK(keys[3].key == 0x70028); // Enter
        CHECK(keys[4].key == 0x7002A); // Backspace
        CHECK(keys[5].character == ' ');
        // A character of several bytes; Escape twice; a sequence of no key; one split in two.
        n = decode("\xC3\xA9\033\033\033[99x", keys, 16);
        REQUIRE(n == 2);
        CHECK(keys[0].character == 0xE9);
        CHECK(keys[1].key == 0x70029);
        preso::TerminalKeys t;
        Event               k;
        CHECK(!t.push('\033', &k));
        CHECK(!t.push('[', &k));
        CHECK(t.push('C', &k));
        CHECK(k.key == 0x7004F);
}

TEST_CASE("Terminal: keys do what the screen's do; Escape alone doesn't quit") {
        Event k[8];
        decode("nqx\033\033\003", k, 8);
        CHECK(preso::terminalAction(k[0], InputMode::Deck) == Action::Next);
        CHECK(preso::terminalAction(k[1], InputMode::Deck) == Action::Quit);
        CHECK(preso::terminalAction(k[2], InputMode::Deck) == Action::None);
        CHECK(preso::terminalAction(k[3], InputMode::Deck) == Action::None);
        CHECK(preso::terminalAction(k[3], InputMode::Script) == Action::Leave);
        CHECK(preso::terminalAction(k[4], InputMode::Deck) == Action::Quit); // Ctrl-C
        decode("\033[C\r\t", k, 8);
        CHECK(preso::terminalAction(k[0], InputMode::Deck) == Action::Next);
        CHECK(preso::terminalAction(k[0], InputMode::Overview) == Action::Right);
        CHECK(preso::terminalAction(k[1], InputMode::Overview) == Action::Choose);
        CHECK(preso::terminalAction(k[1], InputMode::Deck) == Action::None); // Enter: Next, or a number's end
        CHECK(preso::terminalAction(k[2], InputMode::Deck) == Action::Overview);
}

TEST_CASE("Notes: the slide, its notes, what comes next, and the pace") {
        static const char deck[] =
                "---\nduration: 10m\n---\n\n# One\n\n<!-- Say hello. -->\n\n---\n\n# Two\n\n"
                "First point of two.\n\n* a\n* b\n\n---\n\n# Three\n\n---\n\n# Four\n\n---\n\n# Five\n";
        preso::Deck d;
        char       *source = static_cast<char *>(malloc(sizeof deck));
        memcpy(source, deck, sizeof deck);
        REQUIRE(d.parse(source, uint32_t(sizeof deck - 1)) == Error::Ok);
        preso::Buffer    out;
        preso::NotesTime time;
        time.talk = 65 * Second;
        time.slide = 5 * Second;
        preso::writeNotes(out, d, 1, 0, 0, time, 78);
        StringView text = out.view();
        CHECK(text.indexOf("1/5  One") >= 0);
        CHECK(text.indexOf("1:05  slide 0:05") >= 0);
        CHECK(text.indexOf("Say hello.") >= 0);
        CHECK(text.indexOf("Next: 2  Two") >= 0);
        CHECK(text.indexOf("    First point of two.") >= 0);
        // Ten minutes over five slides: slide 1 is due at 0:00, so 65 s in is behind.
        CHECK(text.indexOf("1:05 behind") >= 0);
        CHECK(preso::paceSeconds(d, 3, 4 * 60 * Second) == 0);   // due at 4:00
        CHECK(preso::paceSeconds(d, 3, 3 * 60 * Second) == -60); // a minute ahead
        out.clear();
        time.talk = 4 * 60 * Second;
        preso::writeRule(out, d, 3, time, 78);
        CHECK(out.view().indexOf("on time") >= 0);
        out.clear();
        preso::writeNotes(out, d, 5, 0, 0, time, 78);
        CHECK(out.view().indexOf("Next: the end") >= 0);
        CHECK(out.view().indexOf("(no notes)") >= 0);
}

TEST_CASE("Presenting: notes for each step shown, keys from the terminal") {
        NotesPlatform platform;
        platform.text = "# One\n\n<!-- First. -->\n\n* a\n* b\n\n---\n\n# Two\n\n---\n\n# Three\n";
        char      text[8192] = {};
        FILE     *out = fmemopen(text, sizeof text - 1, "w");
        Presenter presenter(platform, "deck.md", out);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        fflush(out);
        CHECK(strstr(text, "1/3  One") != nullptr);
        CHECK(strstr(text, "step 1 of 3") != nullptr);
        CHECK(strstr(text, "First.") != nullptr);
        // `n` on the terminal: the next step, its notes once.
        platform.clock += 3 * Second;
        presenter.terminalInput("n", 1);
        CHECK(presenter.step() == 1);
        fflush(out);
        CHECK(strstr(text, "step 2 of 3") != nullptr);
        // The right arrow, then "3" and Enter: slide 3.
        presenter.terminalInput("\033[C", 3);
        CHECK(presenter.step() == 2);
        presenter.terminalInput("3\r", 2);
        CHECK(presenter.slideNumber() == 3);
        fflush(out);
        CHECK(strstr(text, "3/3  Three") != nullptr);
        CHECK(strstr(text, "Next: the end") != nullptr);
        // Escape alone does nothing; `q` quits (the platform told).
        presenter.terminalInput("\033\033", 2);
        CHECK(presenter.slideNumber() == 3);
        fclose(out);
}

TEST_CASE("Presenting: the pointer's dot comes and goes; ink is drawn and wiped") {
        NotesPlatform platform;
        platform.text = "# Ink\n\n---\n\n# Clean\n";
        Presenter presenter(platform, "deck.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        Color accent = preso::Theme::paper().accent;
        CHECK(count(platform.last, accent) == 0);
        // A move: the dot, and the tick that will hide it.
        presenter.event(pointerAt(600, 400, 0));
        CHECK(presenter.dotShown());
        CHECK(platform.ticking);
        Color paper = preso::Theme::paper().background;
        CHECK(platform.last.colorAt(600, 400) != paper); // the dot, mostly opaque
        // Two seconds still: gone, and no more ticks.
        platform.clock += 2500 * Ms;
        presenter.second();
        CHECK(!presenter.dotShown());
        CHECK(!platform.ticking);
        CHECK(platform.last.colorAt(600, 400) == paper);
        // Held down and dragged: a stroke of ink.
        presenter.event(pointerAt(200, 300, 1));
        presenter.event(pointerAt(400, 300, 1));
        presenter.event(pointerAt(400, 300, 0));
        CHECK(presenter.inkStrokes() == 1);
        platform.clock += 3 * Second;
        presenter.second(); // the dot gone: the ink stays
        uint32_t inked = count(platform.last, accent);
        CHECK(inked > 600);
        // Wiped by Clear ink; and left behind on the slide before.
        presenter.act(Action::ClearInk);
        CHECK(presenter.inkStrokes() == 0);
        CHECK(count(platform.last, accent) == 0);
        presenter.event(pointerAt(200, 300, 1));
        presenter.event(pointerAt(300, 300, 0));
        presenter.act(Action::Next);
        CHECK(presenter.inkStrokes() == 0);
        // A pad's stick: the dot, carried by frames while it leans.
        Event stick;
        stick.type = Event::Type::Stick;
        stick.x = 32767;
        presenter.event(stick);
        CHECK(presenter.dotShown());
        CHECK(platform.animating);
        stick.x = 0;
        presenter.event(stick);
        CHECK(!platform.animating);
}


TEST_CASE("Presenting: a slide's demo runs as the deck reaches it, and the deck goes on after") {
        NotesPlatform platform;
        platform.text = "# Intro\n\n---\n\n<!-- run: luckyduck --deal -->\n\n# Cards\n\n---\n\n# After\n";
        char  text[8192] = {};
        FILE *out = fmemopen(text, sizeof text - 1, "w");
        {
                // Trusted: no question; Next onto the demo's slide starts it from the deck's folder.
                Presenter presenter(platform, "talks/deck.md", out, nullptr, true);
                REQUIRE(presenter.load() == Error::Ok);
                REQUIRE(presenter.run(1) == Error::Ok);
                presenter.act(Action::Next);
                CHECK(strcmp(platform.demo, "luckyduck --deal") == 0);
                CHECK(strcmp(platform.demoFolder, "talks") == 0);
                CHECK(presenter.demoRunning());
                fflush(out);
                CHECK(strstr(text, "Demo: luckyduck --deal (running)") != nullptr);
                // The screen and the pictures given back for it as it starts.
                CHECK(presenter.screenReleased());
                CHECK(platform.screen == nullptr);
                // While it runs, the deck waits.
                presenter.act(Action::Next);
                CHECK(presenter.slideNumber() == 2);
                // Its end: the screen again, at the slide after.
                presenter.demoEnded(0);
                CHECK(!presenter.demoRunning());
                CHECK(!presenter.screenReleased());
                CHECK(platform.opens == 2);
                CHECK(platform.screen != nullptr);
                CHECK(presenter.slideNumber() == 3);
                // Back onto it: the demo isn't run again.
                platform.demo[0] = '\0';
                presenter.act(Action::Previous);
                CHECK(presenter.slideNumber() == 2);
                CHECK(platform.demo[0] == '\0');
                fflush(out);
                CHECK(strstr(text, "Demo: luckyduck --deal (runs stepping onto the slide)") != nullptr);
        }
        {
                // Asked and declined: not run, and the notes say so.
                Presenter presenter(platform, "deck.md", out);
                REQUIRE(presenter.load() == Error::Ok);
                REQUIRE(presenter.run(1) == Error::Ok);
                presenter.event([] {
                        Event e;
                        e.type = Event::Type::Key;
                        e.key = 0x70011; // n
                        e.character = 'n';
                        return e;
                }());
                CHECK(!presenter.asking());
                presenter.act(Action::Next);
                CHECK(presenter.slideNumber() == 2);
                CHECK(!presenter.demoRunning());
                CHECK(platform.demo[0] == '\0');
                fflush(out);
                CHECK(strstr(text, "(not run: the deck's demos were declined)") != nullptr);
        }
        fclose(out);
}

TEST_CASE("Presenting: a demo on the last slide brings that slide back as it ends") {
        NotesPlatform platform;
        platform.text = "# Intro\n\n---\n\n<!-- run: luckyduck -->\n\n# Last\n";
        Presenter presenter(platform, "deck.md", nullptr, nullptr, true);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        presenter.act(Action::Next);
        REQUIRE(presenter.demoRunning());
        REQUIRE(platform.screen == nullptr);
        presenter.demoEnded(0);
        CHECK(presenter.slideNumber() == 2);
        REQUIRE(platform.screen != nullptr);
        // Drawn again: the slide's background, not the cleared screen's zeros.
        CHECK(platform.screen[0] != 0);
}

TEST_CASE("Presenting: a demo that can't be started leaves its slide shown") {
        NotesPlatform platform;
        platform.text = "# Intro\n\n---\n\n<!-- run: nosuchprogram -->\n\n# Missing\n";
        platform.demoStarts = false;
        char  text[8192] = {};
        FILE *out = fmemopen(text, sizeof text - 1, "w");
        {
                Presenter presenter(platform, "deck.md", out, nullptr, true);
                REQUIRE(presenter.load() == Error::Ok);
                REQUIRE(presenter.run(1) == Error::Ok);
                presenter.act(Action::Next);
                CHECK(presenter.slideNumber() == 2);
                CHECK(!presenter.demoRunning());
                CHECK(!presenter.screenReleased());
                REQUIRE(platform.screen != nullptr);
                CHECK(platform.screen[0] != 0);
                // The deck goes on as ever.
                presenter.act(Action::Previous);
                CHECK(presenter.slideNumber() == 1);
                fflush(out);
                CHECK(strstr(text, "Demo: nosuchprogram (couldn't be started)") != nullptr);
        }
        fclose(out);
}
