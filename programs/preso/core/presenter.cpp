/**
 * @file      presenter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "presenter.h"
#include <new>
#include <stdlib.h>

namespace preso {

        /** @brief A deck read, its theme, and its slides' layouts (made as they are first shown). */
        struct Presenter::Loaded {
                        Deck          deck;
                        Arena         arena;
                        ThemeSet      themes;
                        Layouter     *layouter = nullptr;
                        SlideLayout **layouts = nullptr;
                        uint32_t      printed = 0; ///< Diagnostics written to the messages so far.
        };

        namespace {

                template <typename T, typename... Args> T *create(Args &&...args) {
                        void *p = malloc(sizeof(T));
                        return p != nullptr ? new (p) T(static_cast<Args &&>(args)...) : nullptr;
                }

                template <typename T> void destroy(T *p) {
                        if (p == nullptr) return;
                        p->~T();
                        free(p);
                        return;
                }

                void printDiagnostic(FILE *out, const char *path, const Diagnostic &d) {
                        const char *kind = d.severity == Severity::Error ? "error" : "warning";
                        if (d.line > 0) {
                                fprintf(out, "%s:%u: %s: %.*s\n", path, unsigned(d.line), kind, int(d.message.length()),
                                        d.message.data());
                        } else {
                                fprintf(out, "%s: %s: %.*s\n", path, kind, int(d.message.length()), d.message.data());
                        }
                        return;
                }

        } // namespace

        Presenter::Presenter(Platform &platform, const char *path, FILE *messages)
            : _platform(platform), _path(path), _messages(messages), _fonts(platform), _images(platform, path) {}

        Presenter::~Presenter() {
                free(_pieces);
                dropThumbnails();
                drop(_loaded);
                _platform.freePixels(_pixels[0]);
                _platform.freePixels(_pixels[1]);
        }

        void Presenter::drop(Loaded *loaded) {
                if (loaded == nullptr) return;
                destroy(loaded->layouter);
                destroy(loaded);
                return;
        }

        Error Presenter::read(Loaded *&loaded) {
                loaded = nullptr;
                char    *source = nullptr;
                uint32_t length = 0;
                Error    error = _platform.readFile(_path, Deck::MaxSource, &source, &length);
                if (error != Error::Ok) return error;
                Loaded *l = create<Loaded>();
                if (l == nullptr) {
                        free(source);
                        return Error::NoMemory;
                }
                error = l->deck.parse(source, length);
                if (error == Error::Ok) {
                        error = l->themes.load(_platform, l->deck.settings().theme, _path, _platform.dataFolder(),
                                               l->deck);
                }
                if (error == Error::Ok) {
                        l->layouter = create<Layouter>(l->deck, l->themes, _fonts, _platform, &_images);
                        uint32_t n = l->deck.slideCount();
                        l->layouts = static_cast<SlideLayout **>(
                                l->arena.allocate(n * sizeof(SlideLayout *), alignof(void *)));
                        if (l->layouter == nullptr || l->layouts == nullptr) {
                                error = Error::NoMemory;
                        } else {
                                for (uint32_t i = 0; i < n; i++) l->layouts[i] = nullptr;
                        }
                }
                if (error != Error::Ok) {
                        drop(l);
                        return error;
                }
                loaded = l;
                return Error::Ok;
        }

        Error Presenter::load() {
                Loaded *l = nullptr;
                Error   error = read(l);
                if (error != Error::Ok) {
                        if (_messages != nullptr) fprintf(_messages, "preso: %s: %s\n", _path, errorName(error));
                        return error;
                }
                drop(_loaded);
                _loaded = l;
                _deck = &l->deck;
                printNewDiagnostics();
                return Error::Ok;
        }

        void Presenter::printNewDiagnostics() {
                if (_loaded == nullptr || _messages == nullptr) return;
                uint32_t i = 0;
                for (const Diagnostic *d = _deck->diagnostics(); d != nullptr; d = d->next, i++) {
                        if (i >= _loaded->printed) printDiagnostic(_messages, _path, *d);
                }
                _loaded->printed = i;
                fflush(_messages);
                return;
        }

        uint32_t Presenter::fragments(uint32_t slide) const {
                const Slide *s = _deck != nullptr ? _deck->slide(slide) : nullptr;
                return s != nullptr ? s->fragments : 0;
        }

        void Presenter::target(Action action, uint32_t &slide, uint32_t &step) const {
                slide = _slide;
                step = _step;
                uint32_t count = _deck != nullptr ? _deck->slideCount() : 1;
                switch (action) {
                        case Action::Next:
                                if (step < fragments(slide)) {
                                        step++;
                                } else if (slide < count) {
                                        slide++;
                                        step = 0;
                                }
                                break;
                        case Action::Previous:
                                // Back onto a slide shows it as it was left: whole.
                                if (step > 0) {
                                        step--;
                                } else if (slide > 1) {
                                        slide--;
                                        step = fragments(slide);
                                }
                                break;
                        case Action::First:
                                slide = 1;
                                step = 0;
                                break;
                        case Action::Last:
                                slide = count;
                                step = fragments(count);
                                break;
                        default: break; // not a move
                }
                return;
        }

        void Presenter::draw(Canvas &canvas, uint32_t slide, uint32_t step) {
                const Slide *s = _deck->slide(slide);
                if (s == nullptr || canvas.isNull()) return;
                auto failures = [&]() {
                        char  path[256];
                        Error why;
                        while ((why = _images.takeFailure(path, sizeof path)) != Error::Ok) {
                                if (_messages != nullptr)
                                        fprintf(_messages, "preso: %s: can't be drawn: %s\n", path, errorName(why));
                        }
                };
                SlideLayout *&layout = _loaded->layouts[slide - 1];
                if (layout == nullptr) {
                        layout = _loaded->layouter->layout(*s, _loaded->arena, true);
                        printNewDiagnostics();
                }
                if (layout != nullptr) {
                        drawSlide(*layout, step, canvas, _fonts, &_images);
                        failures();
                } else {
                        canvas.clearClip();
                        canvas.fill(Rect{0, 0, canvas.width(), canvas.height()}, _loaded->themes.base().background);
                }
                if (_status[0] != '\0') {
                        // Why the deck on disk isn't the one shown: a strip along the bottom.
                        FontKey     font = _loaded->themes.base().header.font;
                        FontMetrics m = _fonts.metrics(font);
                        int32_t     h = m.lineHeight + 8;
                        canvas.fill(Rect{0, canvas.height() - h, canvas.width(), h}, 0xB00020);
                        _fonts.draw(canvas, font, 12, canvas.height() - h + 4, StringView(_status), 0xFFFFFF);
                }
                return;
        }

        void Presenter::show(bool forward) {
                // The new picture into _ahead (it may be there already), the old one kept in _shown.
                if (!(_aheadValid && _aheadSlide == _slide && _aheadStep == _step)) draw(_ahead, _slide, _step);
                _aheadValid = false;
                // Onto another slide going forward: its transition, if the screen can play one.
                const Slide *s = _deck->slide(_slide);
                Transition   t;
                const char  *why = "";
                bool         play =
                        _screen && forward && _shownSlide != 0 && _shownSlide != _slide && s != nullptr &&
                        parseTransition(s->settings.transition.isEmpty() ? StringView("cut") : s->settings.transition,
                                        &t, &why) &&
                        t.kind != TransitionKind::Cut && t.durationMs > 0;
                if (play && _pieces == nullptr) _pieces = static_cast<Piece *>(malloc(MaxPieces * sizeof(Piece)));
                SlideLayout *layout = _loaded->layouts[_slide - 1];
                if (play && (_pieces == nullptr || (t.kind == TransitionKind::Type && layout == nullptr))) play = false;
                if (play) {
                        _mode = Mode::Transition;
                        _transition = t;
                        _transitionStart = _platform.now();
                        _typeTotal = 0;
                        if (t.kind == TransitionKind::Type) {
                                // Typed over the slide without its text (drawn where the old one was:
                                // it isn't wanted again), its text copied in a run at a time.
                                _typeTotal = countCharacters(*layout, _step);
                                drawSlide(*layout, _step, _shown, _fonts, &_images, 0);
                                // Typed at a typist's pace, 60 characters a second, the time at most.
                                uint32_t ms = _typeTotal * 1000 / 60;
                                if (ms < _transition.durationMs) _transition.durationMs = ms > 0 ? ms : 1;
                        }
                        // Both pictures to the platform once (the DMA's); the frames are pieces of them.
                        (void)_platform.beginPieces(_shown, _ahead);
                        for (uint32_t &b : _buffered) b = UINT32_MAX;
                        _frameIndex = 0;
                        _lastStage = UINT32_MAX;
                        _platform.animate(true);
                        frame(_transitionStart);
                        return;
                }
                finishTransition();
                return;
        }

        void Presenter::finishTransition() {
                // The new picture is _shown's from now; the one after it drawn ahead.
                Canvas c = _shown;
                _shown = _ahead;
                _ahead = c;
                void *p = _pixels[0];
                _pixels[0] = _pixels[1];
                _pixels[1] = p;
                _shownSlide = _slide;
                _shownStep = _step;
                if (_mode == Mode::Transition) {
                        _mode = Mode::Slides;
                        _platform.endPieces();
                        _platform.animate(false);
                }
                if (_screen) _platform.present(_shown);
                prepareAhead();
                return;
        }

        void Presenter::frame(int64_t ns) {
                if (_mode == Mode::Transition) {
                        int64_t  elapsed = ns - _transitionStart;
                        uint32_t progress =
                                elapsed <= 0 ? 0
                                : elapsed >= int64_t(_transition.durationMs) * 1000000
                                        ? 65536
                                        : uint32_t(elapsed * 65536 / (int64_t(_transition.durationMs) * 1000000));
                        if (progress >= 65536) {
                                finishTransition();
                                return;
                        }
                        // The buffer this frame goes into holds the frame `buffers` before: only the
                        // difference is copied (a wipe's strip, not the screen).
                        uint32_t  buffers = _platform.frameBuffers();
                        uint32_t *had = buffers >= 1 && buffers <= 4 ? &_buffered[_frameIndex % buffers] : nullptr;
                        uint32_t  since = had != nullptr ? *had : UINT32_MAX;
                        uint32_t  now = progress;
                        bool      shown = false;
                        if (_transition.kind == TransitionKind::Type) {
                                now = uint32_t((uint64_t(_typeTotal) * progress) >> 16);
                                shown = _platform.presentPieces(_pieces, typePieces(now, since));
                        } else if (_transition.kind == TransitionKind::Pixelate) {
                                // Blocks of a colour each: the CPU's (the DMA doesn't stretch), and only
                                // when the blocks change; between, the display's next frame is waited for.
                                uint32_t stage = pixelateStage(progress, _shown.width());
                                if (stage == _lastStage) {
                                        _platform.waitFrame();
                                        return;
                                }
                                Canvas screen;
                                if (_platform.beginFrame(screen)) {
                                        composeTransition(_transition, progress, _shown, _ahead, screen);
                                        _platform.endFrame();
                                        shown = true;
                                        _lastStage = stage;
                                }
                        } else {
                                uint32_t n = transitionPieces(_transition, progress, _shown.width(), _shown.height(),
                                                              _pieces, since);
                                shown = _platform.presentPieces(_pieces, n);
                        }
                        if (!shown) {
                                finishTransition();
                                return;
                        }
                        // Pixelate is whole every frame: nothing to remember.
                        if (had != nullptr) *had = _transition.kind == TransitionKind::Pixelate ? UINT32_MAX : now;
                        _frameIndex++;
                        return;
                }
                if (_mode == Mode::Overview) {
                        // One thumbnail made a frame, the grid shown as it fills.
                        bool   more = makeThumbnail();
                        Canvas screen;
                        if (_platform.beginFrame(screen)) {
                                drawOverview(screen);
                                _platform.endFrame();
                        }
                        if (!more) _platform.animate(false);
                        return;
                }
                _platform.animate(false);
                return;
        }

        uint32_t Presenter::typePieces(uint32_t budget, uint32_t since) {
                // The slide without its text (in _shown), then each run of text typed so far copied
                // from the whole slide (in _ahead): runs whole, the last one to the character. Since
                // an earlier frame (`since` characters already there), only the runs typed since.
                uint32_t     n = 0;
                SlideLayout *layout = _loaded->layouts[_slide - 1];
                auto         add = [&](Piece::Source source, int32_t x, int32_t y, int32_t w, int32_t h) {
                        if (n >= MaxPieces || w <= 0 || h <= 0) return;
                        // On four-pixel columns, a little over (italics lean past their advance).
                        int32_t left = (x - 2) & ~3, right = (x + w + 2 + 3) & ~3;
                        if (left < 0) left = 0;
                        if (right > _shown.width()) right = _shown.width();
                        Piece &p = _pieces[n++];
                        p.source = source;
                        p.x = p.fromX = left;
                        p.y = p.fromY = y;
                        p.width = right - left;
                        p.height = y + h <= _shown.height() ? h : _shown.height() - y;
                };
                if (since == UINT32_MAX || since > budget) {
                        since = 0;
                        add(Piece::From, 0, 0, _shown.width(), _shown.height());
                }
                uint32_t before = 0; // characters in the runs before this one
                for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr && budget > 0;
                     o = o->next) {
                        if (o->type != OpType::Text || o->fragment > _step) continue;
                        uint32_t chars = 0;
                        for (uint32_t i = 0; i < o->text.length(); i++) chars += (uint8_t(o->text[i]) & 0xC0) != 0x80;
                        bool there = before + chars <= since; // all of it in the buffer already
                        before += chars;
                        if (chars <= budget) {
                                if (!there) add(Piece::To, o->x, o->y, o->width, o->height);
                                budget -= chars;
                                continue;
                        }
                        // Part of this run: as wide as its first `budget` characters.
                        uint32_t bytes = 0;
                        for (uint32_t c = 0; bytes < o->text.length(); bytes++) {
                                if ((uint8_t(o->text[bytes]) & 0xC0) != 0x80 && c++ == budget) break;
                        }
                        add(Piece::To, o->x, o->y, _fonts.width(o->font, o->text.left(bytes)), o->height);
                        budget = 0;
                }
                return n;
        }

        void Presenter::prepareAhead() {
                uint32_t slide = 0, step = 0;
                target(Action::Next, slide, step);
                if (slide == _slide && step == _step) return;
                draw(_ahead, slide, step);
                _aheadSlide = slide;
                _aheadStep = step;
                _aheadValid = true;
                return;
        }

        void Presenter::showOverlay() {
                // The slide, with the number being typed in a box over its corner (or blanked).
                Canvas screen;
                if (!_screen || !_platform.beginFrame(screen)) return;
                if (_blank != NoColor) {
                        screen.clearClip();
                        screen.fill(Rect{0, 0, screen.width(), screen.height()}, _blank);
                } else {
                        screen.copyFrom(_shown);
                }
                if (_typedLength > 0) {
                        char text[32];
                        snprintf(text, sizeof text, "Go to %.*s", int(_typedLength), _typed);
                        FontKey font = _loaded->themes.base().header.font;
                        font.size = uint16_t(screen.height() / 24 > 12 ? screen.height() / 24 : 12);
                        FontMetrics m = _fonts.metrics(font);
                        int32_t     w = _fonts.width(font, StringView(text)) + m.lineHeight;
                        int32_t     h = m.lineHeight + m.lineHeight / 2;
                        int32_t     x = m.lineHeight / 2, y = screen.height() - h - m.lineHeight / 2;
                        screen.fill(Rect{x, y, w, h}, 0x202020);
                        screen.fill(Rect{x, y, w, 2}, 0xFFCC00);
                        _fonts.draw(screen, font, x + m.lineHeight / 2, y + m.lineHeight / 4, StringView(text),
                                    0xFFFFFF);
                }
                _platform.endFrame();
                return;
        }

        void Presenter::goTo(uint32_t slide) {
                if (slide < 1 || slide > _deck->slideCount()) return;
                bool forward = slide > _slide;
                _slide = slide;
                _step = 0;
                if (_slide == _shownSlide && _step == _shownStep) {
                        if (_screen) _platform.present(_shown);
                        return;
                }
                show(forward && slide == _shownSlide + 1);
                return;
        }

        void Presenter::act(Action action) {
                // A transition playing is finished at once: the presenter is ahead of it.
                if (_mode == Mode::Transition) finishTransition();
                if (_mode == Mode::Overview) {
                        switch (action) {
                                case Action::Left: moveSelection(-1); return;
                                case Action::Right: moveSelection(1); return;
                                case Action::Up: moveSelection(-int32_t(_columns)); return;
                                case Action::Down: moveSelection(int32_t(_columns)); return;
                                case Action::Choose: closeOverview(true); return;
                                case Action::Close:
                                case Action::Overview: closeOverview(false); return;
                                case Action::Quit: _platform.quit(); return;
                                default: return;
                        }
                }
                // A number being typed: Backspace (Previous) takes a digit back, Escape (Quit) the
                // whole of it; anything else ends it.
                if (_typedLength > 0) {
                        if (action == Action::Previous) {
                                _typedLength--;
                                if (_typedLength == 0 && _screen) {
                                        _platform.present(_shown);
                                } else {
                                        showOverlay();
                                }
                                return;
                        }
                        _typedLength = 0;
                        if (action == Action::Quit) {
                                showOverlay();
                                if (_screen) _platform.present(_shown);
                                return;
                        }
                }
                switch (action) {
                        case Action::Quit: _platform.quit(); return;
                        case Action::Black:
                        case Action::White: {
                                Color want = action == Action::Black ? Color(0x000000) : Color(0xFFFFFF);
                                _blank = _blank == want ? NoColor : want;
                                if (_blank == NoColor) {
                                        if (_screen) _platform.present(_shown);
                                } else {
                                        showOverlay();
                                }
                                return;
                        }
                        case Action::Overview: openOverview(); return;
                        default: break;
                }
                // Blank, a step only brings the slide back.
                if (_blank != NoColor && (action == Action::Next || action == Action::Previous)) {
                        _blank = NoColor;
                        if (_screen) _platform.present(_shown);
                        return;
                }
                _blank = NoColor;
                uint32_t slide = 0, step = 0;
                target(action, slide, step);
                if (slide == _slide && step == _step) return;
                _slide = slide;
                _step = step;
                show(action == Action::Next);
                return;
        }

        void Presenter::key(const Event &event) {
                uint32_t usage = event.key & 0xFFFF;
                bool     enter = usage == 0x28 || usage == 0x58;
                if (_mode == Mode::Overview) {
                        if (enter) closeOverview(true);
                        return;
                }
                if (_mode == Mode::Transition) finishTransition();
                // A digit: its character, else its key (the top row's 1-9 0, the keypad's).
                char digit = event.character >= '0' && event.character <= '9' ? char(event.character) : 0;
                if (digit == 0 && usage >= 0x1E && usage <= 0x27)
                        digit = usage == 0x27 ? '0' : char('1' + (usage - 0x1E));
                if (digit == 0 && usage >= 0x59 && usage <= 0x62)
                        digit = usage == 0x62 ? '0' : char('1' + (usage - 0x59));
                if (digit != 0) {
                        if (_typedLength < 4) _typed[_typedLength++] = digit;
                        showOverlay();
                        return;
                }
                if (enter) {
                        if (_typedLength == 0) {
                                act(Action::Next);
                                return;
                        }
                        uint32_t n = uint32_t(StringView(_typed, _typedLength).toInt(0));
                        _typedLength = 0;
                        if (n >= 1 && n <= _deck->slideCount()) {
                                goTo(n);
                        } else if (_screen) {
                                _platform.present(_shown);
                        }
                }
                return;
        }

        // ---- The overview

        void Presenter::dropThumbnails() {
                for (uint32_t i = 0; i < _thumbCount; i++) freeScaled(_thumbs[i]);
                free(_thumbs);
                _thumbs = nullptr;
                _thumbCount = 0;
                return;
        }

        void Presenter::openOverview() {
                if (!_screen) return;
                uint32_t n = _deck->slideCount();
                if (_thumbCount != n) {
                        dropThumbnails();
                        _thumbs = static_cast<ScaledImage *>(calloc(n, sizeof(ScaledImage)));
                        if (_thumbs == nullptr) return;
                        _thumbCount = n;
                }
                // Four across, as many rows as fit at the slide's shape.
                int32_t w = _shown.width(), h = _shown.height();
                int32_t margin = w / 32, gap = margin / 2;
                _columns = 4;
                _thumbWidth = (w - 2 * margin - int32_t(_columns - 1) * gap) / int32_t(_columns);
                _thumbHeight = _thumbWidth * h / w;
                // A row: the thumbnail, its number under it, the gap.
                int32_t label = h / 40 > 10 ? h / 40 + h / 80 : 15;
                int32_t rows = (h - 2 * margin - h / 20 + gap) / (_thumbHeight + label + gap);
                _rows = rows > 0 ? uint32_t(rows) : 1;
                _selected = _slide;
                _mode = Mode::Overview;
                _platform.setInputMode(InputMode::Overview);
                _platform.animate(true);
                Canvas screen;
                if (_platform.beginFrame(screen)) {
                        drawOverview(screen);
                        _platform.endFrame();
                }
                return;
        }

        void Presenter::closeOverview(bool choose) {
                _mode = Mode::Slides;
                _platform.animate(false);
                _platform.setInputMode(InputMode::Deck);
                // Its thumbnails were drawn in _ahead: the next step is drawn again.
                _aheadValid = false;
                if (choose && _selected != _slide) {
                        _slide = _selected;
                        _step = 0;
                        show(false);
                        return;
                }
                if (_screen) _platform.present(_shown);
                prepareAhead();
                return;
        }

        void Presenter::moveSelection(int32_t by) {
                int64_t next = int64_t(_selected) + by;
                if (next < 1) next = 1;
                if (next > int64_t(_deck->slideCount())) next = _deck->slideCount();
                _selected = uint32_t(next);
                Canvas screen;
                if (_platform.beginFrame(screen)) {
                        drawOverview(screen);
                        _platform.endFrame();
                }
                _platform.animate(true); // a new page may want its thumbnails
                return;
        }

        bool Presenter::makeThumbnail() {
                // The page's first slide not yet made, drawn whole in _ahead and scaled down.
                uint32_t first = (_selected - 1) / perPage() * perPage() + 1;
                uint32_t last = first + perPage() - 1;
                if (last > _deck->slideCount()) last = _deck->slideCount();
                for (uint32_t n = first; n <= last; n++) {
                        if (_thumbs[n - 1].pixels != nullptr) continue;
                        // Thumbnails far from this page go: they add up (90 KB each at 1280).
                        for (uint32_t i = 0; i < _thumbCount; i++) {
                                if (_thumbs[i].pixels != nullptr &&
                                    (i + 1 + 2 * perPage() < first || i + 1 > last + 2 * perPage())) {
                                        freeScaled(_thumbs[i]);
                                }
                        }
                        _aheadValid = false;
                        draw(_ahead, n, fragments(n));
                        Picture p;
                        p.pixels = reinterpret_cast<uint32_t *>(_ahead.pixels());
                        p.width = _ahead.width();
                        p.height = _ahead.height();
                        p.stride = _ahead.stride();
                        p.layout = _ahead.format() == PixelFormat::Rgb565 ? Picture::Layout::Rgb565
                                                                          : Picture::Layout::Bgra32;
                        (void)scalePicture(p, _thumbWidth, _thumbHeight, nullptr, 0, nullptr, _ahead.format(),
                                           &_thumbs[n - 1]);
                        return true;
                }
                return false;
        }

        void Presenter::drawOverview(Canvas &screen) {
                int32_t w = screen.width(), h = screen.height();
                int32_t margin = w / 32, gap = margin / 2;
                screen.clearClip();
                screen.fill(Rect{0, 0, w, h}, 0x202020);
                FontKey font = _loaded->themes.base().header.font;
                font.family = "IBM Plex Sans";
                font.style = 0;
                font.size = uint16_t(h / 40 > 10 ? h / 40 : 10);
                FontMetrics m = _fonts.metrics(font);
                int32_t     label = h / 40 > 10 ? h / 40 + h / 80 : 15;
                uint32_t    first = (_selected - 1) / perPage() * perPage() + 1;
                for (uint32_t i = 0; i < perPage(); i++) {
                        uint32_t n = first + i;
                        if (n > _deck->slideCount()) break;
                        int32_t x = margin + int32_t(i % _columns) * (_thumbWidth + gap);
                        int32_t y = margin + int32_t(i / _columns) * (_thumbHeight + label + gap);
                        if (_thumbs != nullptr && _thumbs[n - 1].pixels != nullptr) {
                                blitImage(screen, _thumbs[n - 1], x, y);
                        } else {
                                screen.fill(Rect{x, y, _thumbWidth, _thumbHeight}, 0x3A3A3A);
                        }
                        // Its number and title under it (the slide shown now's number in yellow).
                        char         number[96];
                        const Slide *sl = _deck->slide(n);
                        int          len = snprintf(number, sizeof number, "%u  %.*s", unsigned(n),
                                           sl != nullptr ? int(sl->title.length()) : 0,
                                           sl != nullptr ? sl->title.data() : "");
                        StringView   text(number, uint32_t(len > 0 ? len : 0));
                        text = text.left(_fonts.fit(font, text, _thumbWidth));
                        _fonts.draw(screen, font, x, y + _thumbHeight + (label - m.lineHeight) / 2 + 2, text,
                                    n == _slide ? 0xFFCC00 : 0xC8C8C8);
                        if (n == _selected) {
                                int32_t lw = w >= 1000 ? 4 : 2;
                                Color   c = 0xFFCC00;
                                screen.fill(Rect{x - lw, y - lw, _thumbWidth + 2 * lw, lw}, c);
                                screen.fill(Rect{x - lw, y + _thumbHeight, _thumbWidth + 2 * lw, lw}, c);
                                screen.fill(Rect{x - lw, y, lw, _thumbHeight}, c);
                                screen.fill(Rect{x + _thumbWidth, y, lw, _thumbHeight}, c);
                        }
                }
                // A line of help along the bottom, and where the page is.
                char     help[96];
                uint32_t pages = (_deck->slideCount() + perPage() - 1) / perPage();
                snprintf(help, sizeof help, "Slide %u of %u   arrows move, Enter goes, Tab returns%s",
                         unsigned(_selected), unsigned(_deck->slideCount()), pages > 1 ? "   (more pages)" : "");
                _fonts.draw(screen, font, margin, h - m.lineHeight - margin / 2, StringView(help), 0xA0A0A0);
                return;
        }

        bool Presenter::reload() {
                // A picture may have changed too: they are read again as they are drawn.
                _images.clear();
                Loaded *l = nullptr;
                Error   error = read(l);
                if (error != Error::Ok) {
                        snprintf(_status, sizeof _status, "%s: %s: the deck as it was is shown", _path,
                                 errorName(error));
                        if (_messages != nullptr) fprintf(_messages, "preso: %s\n", _status);
                        _aheadValid = false;
                        _shownSlide = 0;
                        show(false);
                        return false;
                }
                // The same slide: by its title if the slides moved, else by its number.
                const Slide *old = _deck != nullptr ? _deck->slide(_slide) : nullptr;
                uint32_t     number = _slide;
                if (old != nullptr && !old->title.isEmpty()) {
                        const Slide *same = l->deck.slide(number);
                        if (same == nullptr || !same->title.equals(old->title)) {
                                uint32_t best = 0, distance = UINT32_MAX;
                                for (const Slide *s = l->deck.slides(); s != nullptr; s = s->next) {
                                        uint32_t d = s->number > number ? s->number - number : number - s->number;
                                        if (s->title.equals(old->title) && d < distance) {
                                                best = s->number;
                                                distance = d;
                                        }
                                }
                                if (best != 0) number = best;
                        }
                }
                if (number > l->deck.slideCount()) number = l->deck.slideCount();
                if (number == 0) number = 1;
                drop(_loaded);
                _loaded = l;
                _deck = &l->deck;
                _slide = number;
                if (_step > fragments(_slide)) _step = fragments(_slide);
                _status[0] = '\0';
                printNewDiagnostics();
                _aheadValid = false;
                // The slide shown again as it now is, without a transition; the overview's
                // thumbnails were of the deck as it was.
                if (_mode == Mode::Overview) closeOverview(false);
                if (_mode == Mode::Transition) finishTransition();
                dropThumbnails();
                _shownSlide = 0;
                show(false);
                return true;
        }

        Error Presenter::run(uint32_t number) {
                if (_deck == nullptr) return Error::Invalid;
                const Layouter &layouter = *_loaded->layouter;
                int32_t         w = layouter.width(), h = layouter.height();
                PixelFormat     format = _platform.pixelFormat();
                uint32_t        stride = uint32_t(w) * bytesPerPixel(format);
                for (int i = 0; i < 2; i++) {
                        _pixels[i] = _platform.allocatePixels(stride * uint32_t(h));
                        if (_pixels[i] == nullptr) return Error::NoMemory;
                }
                _shown.attach(_pixels[0], stride, w, h, format);
                _ahead.attach(_pixels[1], stride, w, h, format);
                _slide = number >= 1 && number <= _deck->slideCount() ? number : 1;
                _step = 0;
                const char *title = _deck->settings().title.isEmpty() ? "Preso" : nullptr;
                char        buffer[96];
                if (title == nullptr) {
                        snprintf(buffer, sizeof buffer, "%.*s", int(_deck->settings().title.length()),
                                 _deck->settings().title.data());
                        title = buffer;
                }
                Error error = _platform.openScreen(w, h, title, _loaded->themes.base().nearest);
                if (error != Error::Ok) return error;
                _screen = true;
                // The deck's folder, watched: it reloads as it is saved. Without watching, it doesn't.
                char folder[512];
                folderOf(_path, folder, sizeof folder);
                if (_platform.watch(folder) != Error::Ok && _messages != nullptr) {
                        fprintf(_messages, "preso: %s isn't watched: the deck won't reload as it changes\n", folder);
                }
                show(false);
                return _platform.run(*this);
        }

        void Presenter::event(const Event &event) {
                switch (event.type) {
                        case Event::Type::Action: act(event.action); break;
                        case Event::Type::Close: _platform.quit(); break;
                        case Event::Type::Key: key(event); break;
                }
                return;
        }

        void Presenter::filesChanged() {
                reload();
                return;
        }

        int checkDeck(Platform &platform, const char *path, bool verbose, FILE *out) {
                char    *source = nullptr;
                uint32_t length = 0;
                Error    error = platform.readFile(path, Deck::MaxSource, &source, &length);
                if (error == Error::Ok) {
                        Deck *deck = create<Deck>();
                        if (deck == nullptr) {
                                free(source);
                                error = Error::NoMemory;
                        } else {
                                error = deck->parse(source, length);
                                if (error == Error::Ok) {
                                        ThemeSet themes;
                                        error = themes.load(platform, deck->settings().theme, path,
                                                            platform.dataFolder(), *deck);
                                        FontCache  fonts(platform);
                                        ImageCache images(platform, path);
                                        Arena      arena;
                                        Layouter *layouter = error == Error::Ok ? create<Layouter>(*deck, themes, fonts,
                                                                                                   platform, &images)
                                                                                : nullptr;
                                        if (layouter == nullptr) error = Error::NoMemory;
                                        for (const Slide *s = deck->slides(); s != nullptr && error == Error::Ok;
                                             s = s->next) {
                                                if (layouter->layout(*s, arena, true) == nullptr)
                                                        error = Error::NoMemory;
                                        }
                                        destroy(layouter);
                                }
                                if (error == Error::Ok) {
                                        for (const Diagnostic *d = deck->diagnostics(); d != nullptr; d = d->next) {
                                                printDiagnostic(out, path, *d);
                                        }
                                        if (verbose) {
                                                for (const Slide *s = deck->slides(); s != nullptr; s = s->next) {
                                                        fprintf(out, "%3u  %.*s", unsigned(s->number),
                                                                int(s->title.length()), s->title.data());
                                                        if (s->fragments > 0)
                                                                fprintf(out, "  (%u steps)",
                                                                        unsigned(s->fragments) + 1);
                                                        if (!s->run.isEmpty()) {
                                                                fprintf(out, "  [run: %.*s]", int(s->run.length()),
                                                                        s->run.data());
                                                        }
                                                        fprintf(out, "\n");
                                                }
                                                fprintf(out, "%s: %u slides, %u reported\n", path,
                                                        unsigned(deck->slideCount()),
                                                        unsigned(deck->diagnosticCount()));
                                        }
                                }
                                int status = error != Error::Ok ? 2 : deck->diagnosticCount() > 0 ? 1 : 0;
                                destroy(deck);
                                if (error == Error::Ok) return status;
                        }
                }
                fprintf(stderr, "preso: %s: %s\n", path, errorName(error));
                return 2;
        }

} // namespace preso
