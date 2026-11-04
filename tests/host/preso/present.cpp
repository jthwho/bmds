/**
 * @file      present.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Presenting (devplan/preso.md §6, step 5): transitions composed, and the presenter's
 * transitions, black and white screens, going to a slide by number, and the overview, on a
 * platform with a screen buffer, its clock the test's.
 */

#include <doctest.h>

#include <stdlib.h>
#include <string.h>
#include "core/presenter.h"
#include "core/transitions.h"
#include "headless/headless.h"

using preso::Action;
using preso::Canvas;
using preso::Color;
using preso::Direction;
using preso::Error;
using preso::Event;
using preso::InputMode;
using preso::PixelFormat;
using preso::Presenter;
using preso::Rect;
using preso::StringView;
using preso::Transition;
using preso::TransitionKind;

namespace {

        // A screen of its own, frames counted, animation and input mode recorded, the clock set.
        class FramePlatform : public preso::HeadlessPlatform {
                public:
                        const char *text = "";
                        int64_t     clock = 0;
                        bool        animating = false;
                        InputMode   mode = InputMode::Deck;
                        uint32_t    frames = 0;
                        uint32_t    presents = 0;
                        int32_t     width = 0, height = 0;
                        uint32_t   *screen = nullptr;
                        Canvas      last; ///< The screen as last shown.

                        ~FramePlatform() override { free(screen); }
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
                                screen = static_cast<uint32_t *>(calloc(size_t(w) * size_t(h), 4));
                                last.attach(screen, uint32_t(w) * 4, w, h, PixelFormat::Xrgb8888);
                                return Error::Ok;
                        }
                        void present(const Canvas &c) override {
                                last.copyFrom(c);
                                presents++;
                        }
                        bool beginFrame(Canvas &c) override {
                                c.attach(screen, uint32_t(width) * 4, width, height, PixelFormat::Xrgb8888);
                                return true;
                        }
                        void    endFrame() override { frames++; }
                        void    animate(bool on) override { animating = on; }
                        void    setInputMode(InputMode m) override { mode = m; }
                        int64_t now() override { return clock; }
                        int64_t alarmAt = 0; ///< setAlarm()'s, 0: none.
                        void    setAlarm(int64_t at) override { alarmAt = at; }
                        Error   watch(const char *) override { return Error::Ok; }
                        Error   run(preso::App &) override { return Error::Ok; }
        };

        constexpr int64_t Ms = 1000000;

        Event keyEvent(char c, uint32_t usage = 0) {
                Event e;
                e.type = Event::Type::Key;
                e.character = uint32_t(c);
                e.key = 0x70000 | usage;
                return e;
        }

        // Two pictures to go between: all red, all blue.
        struct Pair {
                        uint32_t a[64 * 32], b[64 * 32], o[64 * 32];
                        Canvas   from, to, out;
                        Pair() {
                                from.attach(a, 256, 64, 32, PixelFormat::Xrgb8888);
                                to.attach(b, 256, 64, 32, PixelFormat::Xrgb8888);
                                out.attach(o, 256, 64, 32, PixelFormat::Xrgb8888);
                                from.fill(Rect{0, 0, 64, 32}, 0xFF0000);
                                to.fill(Rect{0, 0, 64, 32}, 0x0000FF);
                        }
                        uint32_t blue() const {
                                uint32_t n = 0;
                                for (uint32_t p : o) n += (p & 0xFFFFFF) == 0x0000FF;
                                return n;
                        }
                        uint32_t red() const {
                                uint32_t n = 0;
                                for (uint32_t p : o) n += (p & 0xFFFFFF) == 0xFF0000;
                                return n;
                        }
        };

} // namespace

TEST_CASE("Transitions: read as decks write them") {
        Transition  t;
        const char *why = "";
        CHECK(preso::parseTransition("wipe", &t, &why));
        CHECK(t.kind == TransitionKind::Wipe);
        CHECK(t.direction == Direction::Right);
        CHECK(t.durationMs == 400);
        CHECK(preso::parseTransition("wipe left 0.6s", &t, &why));
        CHECK(t.direction == Direction::Left);
        CHECK(t.durationMs == 600);
        CHECK(preso::parseTransition("dissolve 1s", &t, &why));
        CHECK(t.durationMs == 1000);
        CHECK(preso::parseTransition("crt", &t, &why));
        CHECK(t.durationMs == 700);
        CHECK(preso::parseTransition("none", &t, &why));
        CHECK(t.kind == TransitionKind::Cut);
        CHECK(!preso::parseTransition("fade", &t, &why));
        CHECK(!preso::parseTransition("wipe sideways", &t, &why));
        CHECK(!preso::parseTransition("wipe 1s 2s", &t, &why));
        CHECK(!preso::parseTransition("", &t, &why));
}

TEST_CASE("Transitions: each starts as the slide and ends as the next") {
        const TransitionKind kinds[] = {TransitionKind::Wipe, TransitionKind::Dissolve, TransitionKind::Pixelate,
                                        TransitionKind::Crt, TransitionKind::Cut};
        for (TransitionKind k : kinds) {
                Pair       p;
                Transition t;
                t.kind = k;
                preso::composeTransition(t, 65536, p.from, p.to, p.out);
                INFO(int(k));
                CHECK(p.blue() == 64 * 32);
                if (k == TransitionKind::Cut) continue;
                preso::composeTransition(t, 0, p.from, p.to, p.out);
                CHECK(p.red() == 64 * 32);
        }
}

TEST_CASE("Transitions: halfway") {
        Pair       p;
        Transition t;
        t.kind = TransitionKind::Wipe;
        preso::composeTransition(t, 32768, p.from, p.to, p.out);
        CHECK(p.blue() == 32 * 32); // the left half, its edge moving right
        CHECK((p.o[0] & 0xFFFFFF) == 0x0000FF);
        CHECK((p.o[63] & 0xFFFFFF) == 0xFF0000);
        t.direction = Direction::Up;
        preso::composeTransition(t, 32768, p.from, p.to, p.out);
        CHECK((p.o[0] & 0xFFFFFF) == 0xFF0000); // from the bottom up
        CHECK((p.o[64 * 31] & 0xFFFFFF) == 0x0000FF);
        t.kind = TransitionKind::Dissolve;
        preso::composeTransition(t, 32768, p.from, p.to, p.out);
        CHECK(p.blue() == 64 * 32 / 2); // the threshold map's half
        t.kind = TransitionKind::Crt;
        preso::composeTransition(t, 32768, p.from, p.to, p.out);
        CHECK((p.o[0] & 0xFFFFFF) == 0x000000); // off: black, a dot at most
}

TEST_CASE("Presenter: a slide's transition plays on the way to it, by the display's frames") {
        FramePlatform platform;
        platform.text = "# One\n\n---\n\n<!-- _transition: wipe 0.4s -->\n# Two\n\n---\n\n# Three\n"; // slide 2 only
        Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        presenter.act(Action::Next);
        CHECK(presenter.inTransition());
        CHECK(platform.animating);
        CHECK(presenter.slideNumber() == 2);
        uint32_t frames = platform.frames;
        platform.clock = 200 * Ms;
        presenter.frame(platform.clock);
        CHECK(platform.frames == frames + 1);
        CHECK(presenter.inTransition());
        platform.clock = 450 * Ms;
        presenter.frame(platform.clock);
        CHECK(!presenter.inTransition());
        CHECK(!platform.animating);
        // Onward, slide 3 has none: a cut.
        presenter.act(Action::Next);
        CHECK(!presenter.inTransition());
        CHECK(presenter.slideNumber() == 3);
        // Back onto slide 2: no transition going back.
        presenter.act(Action::Previous);
        CHECK(!presenter.inTransition());
        CHECK(presenter.slideNumber() == 2);
        // A step during a transition finishes it first, then moves.
        presenter.act(Action::Previous);
        presenter.act(Action::Next);
        CHECK(presenter.inTransition());
        presenter.act(Action::Next);
        CHECK(!presenter.inTransition());
        CHECK(presenter.slideNumber() == 3);
}

TEST_CASE("Presenter: `type` types the slide's text over its frames") {
        FramePlatform platform;
        platform.text = "# One\n\n---\n\n<!-- transition: type -->\n# Typed out here\n\nand some more words to type\n";
        Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        presenter.act(Action::Next);
        REQUIRE(presenter.inTransition());
        // The ink grows from frame to frame.
        auto ink = [&]() {
                uint32_t n = 0;
                for (int32_t i = 0; i < platform.width * platform.height; i++)
                        n += (platform.screen[i] & 0xFFFFFF) != 0xFFFFFF;
                return n;
        };
        platform.clock = 50 * Ms;
        presenter.frame(platform.clock);
        uint32_t early = ink();
        platform.clock = 300 * Ms;
        presenter.frame(platform.clock);
        uint32_t later = ink();
        CHECK(later > early);
        platform.clock = 5000 * Ms;
        presenter.frame(platform.clock);
        CHECK(!presenter.inTransition());
}

TEST_CASE("Presenter: black and white screens, and back") {
        FramePlatform platform;
        platform.text = "# One\n\n---\n\n# Two\n";
        Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        presenter.act(Action::Black);
        CHECK(presenter.blank() == 0x000000);
        CHECK((platform.screen[100] & 0xFFFFFF) == 0x000000);
        presenter.act(Action::Black); // again: back
        CHECK(presenter.blank() == preso::NoColor);
        presenter.act(Action::White);
        CHECK(presenter.blank() == 0xFFFFFF);
        // A step while blank only brings the slide back.
        presenter.act(Action::Next);
        CHECK(presenter.blank() == preso::NoColor);
        CHECK(presenter.slideNumber() == 1);
        presenter.act(Action::Next);
        CHECK(presenter.slideNumber() == 2);
}

TEST_CASE("Presenter: a number typed, then Enter, goes to that slide") {
        FramePlatform platform;
        platform.text = "# 1\n\n---\n\n# 2\n\n---\n\n# 3\n\n---\n\n# 4\n\n* a\n* b\n";
        Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        presenter.event(keyEvent('3'));
        CHECK(presenter.typedNumber().equals("3"));
        presenter.event(keyEvent('\r', 0x28));
        CHECK(presenter.slideNumber() == 3);
        CHECK(presenter.typedNumber().isEmpty());
        // Out of range: nowhere.
        presenter.event(keyEvent('9'));
        presenter.event(keyEvent('\r', 0x28));
        CHECK(presenter.slideNumber() == 3);
        // Backspace (Previous) takes a digit back; Escape (Quit) gives up, and doesn't quit.
        presenter.event(keyEvent('1'));
        presenter.event(keyEvent('2'));
        presenter.act(Action::Previous);
        CHECK(presenter.typedNumber().equals("1"));
        presenter.act(Action::Quit);
        CHECK(presenter.typedNumber().isEmpty());
        CHECK(presenter.slideNumber() == 3);
        // A digit's key without its character (the top row's 2, the keypad's 1): still a digit.
        presenter.event(keyEvent(0, 0x1F));
        presenter.event(keyEvent(0, 0x59));
        CHECK(presenter.typedNumber().equals("21"));
        presenter.act(Action::Quit);
        // Enter alone is Next; a slide gone to starts at its first step.
        presenter.event(keyEvent('\r', 0x28));
        CHECK(presenter.slideNumber() == 4);
        CHECK(presenter.step() == 0);
}

TEST_CASE("Presenter: the overview: a grid of every slide, moved round, chosen from") {
        FramePlatform platform;
        platform.text = "# 1\n\n---\n\n# 2\n\n---\n\n# 3\n\n---\n\n# 4\n\n---\n\n# 5\n\n---\n\n# 6\n";
        Presenter presenter(platform, "talk.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(2) == Error::Ok);
        presenter.act(Action::Overview);
        CHECK(presenter.inOverview());
        CHECK(platform.mode == InputMode::Overview);
        CHECK(platform.animating);
        CHECK(presenter.selected() == 2);
        // Its thumbnails, one a frame: six frames, then it stops asking.
        for (int i = 0; i < 6 && platform.animating; i++) presenter.frame(0);
        presenter.frame(0);
        CHECK(!platform.animating);
        presenter.act(Action::Right);
        CHECK(presenter.selected() == 3);
        presenter.act(Action::Down); // four across
        CHECK(presenter.selected() == 6);
        presenter.act(Action::Down);
        CHECK(presenter.selected() == 6);
        presenter.act(Action::Left);
        presenter.act(Action::Choose);
        CHECK(!presenter.inOverview());
        CHECK(platform.mode == InputMode::Deck);
        CHECK(presenter.slideNumber() == 5);
        // Closed without choosing: the slide as it was.
        presenter.act(Action::Overview);
        presenter.act(Action::Up);
        presenter.act(Action::Close);
        CHECK(presenter.slideNumber() == 5);
        // Stepping still works after it.
        presenter.act(Action::Next);
        CHECK(presenter.slideNumber() == 6);
}

TEST_CASE("Transitions: a wipe's or a dissolve's difference since a frame, laid over it, is the frame") {
        const TransitionKind kinds[] = {TransitionKind::Wipe, TransitionKind::Dissolve};
        const Direction      directions[] = {Direction::Right, Direction::Left, Direction::Up, Direction::Down};
        for (TransitionKind k : kinds) {
                for (Direction d : directions) {
                        Transition t;
                        t.kind = k;
                        t.direction = d;
                        Pair     p;
                        uint32_t whole[64 * 32];
                        Canvas   reference;
                        reference.attach(whole, 256, 64, 32, PixelFormat::Xrgb8888);
                        static preso::Piece pieces[preso::MaxPieces];
                        // Into one buffer: 10%, then 40% as a difference, then 100%.
                        uint32_t steps[] = {6553, 26214, 65536};
                        uint32_t since = UINT32_MAX;
                        for (uint32_t s : steps) {
                                uint32_t n = preso::transitionPieces(t, s, 64, 32, pieces, since);
                                preso::applyPieces(p.out, p.from, p.to, pieces, n);
                                preso::composeTransition(t, s, p.from, p.to, reference);
                                INFO(int(k), " ", int(d), " ", s);
                                CHECK(memcmp(p.o, whole, sizeof whole) == 0);
                                since = s;
                        }
                }
        }
}

namespace {

        // Two screen buffers taking turns, as BMDS's surfaces.
        class TwoBuffers : public FramePlatform {
                public:
                        uint32_t buffer[2][1280 * 720] = {};
                        uint32_t next = 0;
                        uint32_t frameBuffers() const override { return 2; }
                        bool     beginFrame(Canvas &c) override {
                                c.attach(buffer[next], uint32_t(width) * 4, width, height, PixelFormat::Xrgb8888);
                                return true;
                        }
                        void endFrame() override {
                                memcpy(screen, buffer[next], size_t(width) * size_t(height) * 4);
                                next ^= 1;
                                frames++;
                        }
                        void present(const Canvas &c) override {
                                // A whole picture into the next buffer, as a frame.
                                Canvas b;
                                beginFrame(b);
                                b.copyFrom(c);
                                endFrame();
                                FramePlatform::present(c);
                        }
        };

} // namespace

TEST_CASE("Presenter: frames into buffers taking turns change only the difference, and come out whole") {
        const char *decks[] = {
                "# One\n\nfirst\n\n---\n\n<!-- _transition: wipe 0.5s -->\n# Two\n\nsecond slide's words\n",
                "# One\n\nfirst\n\n---\n\n<!-- _transition: dissolve 0.5s -->\n# Two\n\nsecond\n",
                "# One\n\nfirst\n\n---\n\n<!-- _transition: type -->\n# Two typed\n\nand more words here\n"};
        for (const char *text : decks) {
                auto *platform = new TwoBuffers;
                platform->text = text;
                auto *presenter = new Presenter(*platform, "talk.md", nullptr);
                REQUIRE(presenter->load() == Error::Ok);
                REQUIRE(presenter->run(1) == Error::Ok);
                presenter->act(Action::Next);
                REQUIRE(presenter->inTransition());
                bool        typed = strstr(text, "type") != nullptr;
                Transition  how;
                const char *why = "";
                REQUIRE(preso::parseTransition(typed                             ? "type"
                                               : strstr(text, "wipe") != nullptr ? "wipe 0.5s"
                                                                                 : "dissolve 0.5s",
                                               &how, &why));
                static uint32_t whole[1280 * 720];
                Canvas          reference;
                reference.attach(whole, 1280 * 4, 1280, 720, PixelFormat::Xrgb8888);
                for (int64_t t = 30; presenter->inTransition() && t < 3000; t += 40) {
                        platform->clock = t * Ms;
                        presenter->frame(platform->clock);
                        if (typed || !presenter->inTransition()) continue;
                        // Each frame as a whole one would be, though only its difference was copied.
                        uint32_t progress = uint32_t(t * 65536 / 500);
                        preso::composeTransition(how, progress, presenter->shown(), presenter->ahead(), reference);
                        INFO(text, " at ", t, " ms");
                        CHECK(memcmp(platform->screen, whole, sizeof whole) == 0);
                }
                CHECK(!presenter->inTransition());
                // The end: the slide, whole, in what is shown.
                CHECK(memcmp(platform->screen, presenter->shown().pixels(), size_t(1280) * 720 * 4) == 0);
                delete presenter;
                delete platform;
        }
}

TEST_CASE("Kiosk: `auto` steps on by itself, a key gives the full time again, --loop goes round") {
        FramePlatform platform;
        Presenter     presenter(platform, "deck.md", nullptr);
        platform.text =
                "---\nauto: 10s\n---\n\n# One\n\n* a\n* b\n\n---\n\n<!-- _auto: 2s -->\n# Two\n\n---\n\n# Three\n";
        // The platform's alarm goes off once: cleared as it does.
        auto fire = [&](int64_t at) {
                platform.clock = at;
                platform.alarmAt = 0;
                presenter.alarm();
        };
        REQUIRE(presenter.load() == Error::Ok);
        REQUIRE(presenter.run(1) == Error::Ok);
        // Armed for the first step's 10 seconds; its time comes: the next step.
        CHECK(platform.alarmAt == 10000 * Ms);
        fire(10000 * Ms);
        CHECK(presenter.slideNumber() == 1);
        CHECK(presenter.step() == 1);
        CHECK(platform.alarmAt == 20000 * Ms);
        // A key meanwhile: the step after it gets the whole time from then.
        platform.clock = 15000 * Ms;
        presenter.act(Action::Next);
        CHECK(presenter.step() == 2);
        CHECK(platform.alarmAt == 25000 * Ms);
        // Black: held, no alarm; back: the full time again.
        presenter.act(Action::Black);
        CHECK(platform.alarmAt == 0);
        platform.clock = 30000 * Ms;
        presenter.act(Action::Black);
        CHECK(platform.alarmAt == 40000 * Ms);
        // The second slide's own 2 s; the third the deck's 10 s, but it is the end: without --loop,
        // nothing more.
        fire(40000 * Ms);
        CHECK(presenter.slideNumber() == 2);
        CHECK(platform.alarmAt == 42000 * Ms);
        fire(42000 * Ms);
        CHECK(presenter.slideNumber() == 3);
        CHECK(platform.alarmAt == 0);
        // --loop: round to the first slide, by itself.
        presenter.setLoop(true);
        presenter.act(Action::Leave); // any action: the end now has a step after it
        CHECK(platform.alarmAt == 52000 * Ms);
        fire(52000 * Ms);
        CHECK(presenter.slideNumber() == 1);
        CHECK(presenter.step() == 0);
        // The overview holds it.
        presenter.act(Action::Overview);
        CHECK(platform.alarmAt == 0);
}

TEST_CASE("Kiosk: without `auto`, no alarm; --loop alone wraps Next") {
        FramePlatform platform;
        platform.text = "# One\n\n---\n\n# Two\n";
        Presenter presenter(platform, "deck.md", nullptr);
        REQUIRE(presenter.load() == Error::Ok);
        presenter.setLoop(true);
        REQUIRE(presenter.run(2) == Error::Ok);
        CHECK(platform.alarmAt == 0);
        presenter.act(Action::Next);
        CHECK(presenter.slideNumber() == 1);
        // Previous doesn't wrap.
        presenter.act(Action::Previous);
        CHECK(presenter.slideNumber() == 1);
}
