/**
 * @file      presenter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "presenter.h"
#include "effects.h"
#include "notes.h"

#include <new>
#include <stdlib.h>

namespace preso {

        /** @brief A deck read, its theme, and its slides' layouts (made as they are first shown). */
        struct Presenter::Loaded {
                        Deck          deck;
                        Arena         arena;
                        ThemeSet      themes;
                        Layouter     *layouter = nullptr;
                        Scripts      *scripts = nullptr; ///< The deck's script regions (nullptr: boxes).
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

                void destroyScripts(Scripts *scripts) {
                        if (scripts == nullptr) return;
                        scripts->~Scripts();
                        free(scripts);
                        return;
                }

                // A step only a region's beat takes: nothing else appears at it.
                bool beatOnly(const SlideLayout &layout, uint32_t step) {
                        for (const DrawOp *o = layout.ops; o != nullptr; o = o->next) {
                                if (o->fragment == step) return false;
                        }
                        return true;
                }

                // The beat a region shows at @p step.
                uint32_t beatAt(const DrawOp &o, uint32_t step) {
                        uint32_t beats = o.script->beats();
                        uint32_t beat = step >= o.beat ? step - o.beat + 1 : 0;
                        return beat > beats ? beats : beat;
                }

                constexpr uint32_t MaxDamage = 16;

                // @p r added to @p areas (@p n of them, MaxDamage at most): joined to one it overlaps,
                // or the last joined when there is no room.
                void addDamage(Rect *areas, uint32_t &n, const Rect &r) {
                        if (r.isEmpty()) return;
                        for (uint32_t i = 0; i < n; i++) {
                                if (!areas[i].intersected(r).isEmpty()) {
                                        areas[i] = uniteRects(areas[i], r);
                                        return;
                                }
                        }
                        if (n < MaxDamage) {
                                areas[n++] = r;
                        } else {
                                areas[n - 1] = uniteRects(areas[n - 1], r);
                        }
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

        Presenter::Presenter(Platform &platform, const char *path, FILE *messages, ScriptsMaker scripts, bool trusted)
            : _platform(platform), _path(path), _messages(messages), _makeScripts(scripts), _fonts(platform),
              _images(platform, path), _trusted(trusted), _demosAllowed(trusted) {}

        Presenter::~Presenter() {
                if (_slideSound >= 0) _platform.closeSound(_slideSound);
                free(_ink);
                free(_strokes);
                free(_pieces);
                dropThumbnails();
                drop(_loaded);
                _platform.freePixels(_pixels[0]);
                _platform.freePixels(_pixels[1]);
        }

        void Presenter::drop(Loaded *loaded) {
                if (loaded == nullptr) return;
                if (loaded == _loaded) {
                        // Its regions go with it: the one with the keys, the one a button is held in.
                        if (_entered != nullptr) {
                                _entered->script->setEntered(false);
                                _entered = nullptr;
                                _platform.setInputMode(InputMode::Deck);
                        }
                        _pointerRegion = nullptr;
                }
                destroy(loaded->layouter);
                destroyScripts(loaded->scripts);
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
                                if (_makeScripts != nullptr) {
                                        ScriptSetup setup;
                                        setup.platform = &_platform;
                                        setup.deck = &l->deck;
                                        setup.themes = &l->themes;
                                        setup.fonts = &_fonts;
                                        setup.images = &_images;
                                        setup.deckPath = _path;
                                        setup.trusted = _trusted;
                                        l->scripts = _makeScripts(setup);
                                        l->layouter->setScripts(l->scripts);
                                }
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

        SlideLayout *Presenter::layoutOf(uint32_t slide) {
                const Slide *s = _deck != nullptr ? _deck->slide(slide) : nullptr;
                if (s == nullptr) return nullptr;
                SlideLayout *&layout = _loaded->layouts[slide - 1];
                if (layout == nullptr) {
                        layout = _loaded->layouter->layout(*s, _loaded->arena, true);
                        printNewDiagnostics();
                }
                return layout;
        }

        uint32_t Presenter::steps(uint32_t slide) {
                // Its fragments and its regions' beats, which only its layout knows.
                SlideLayout *layout = layoutOf(slide);
                if (layout != nullptr) return layout->steps;
                const Slide *s = _deck != nullptr ? _deck->slide(slide) : nullptr;
                return s != nullptr ? s->fragments : 0;
        }

        void Presenter::target(Action action, uint32_t &slide, uint32_t &step) {
                slide = _slide;
                step = _step;
                uint32_t count = _deck != nullptr ? _deck->slideCount() : 1;
                switch (action) {
                        case Action::Next:
                                if (step < steps(slide)) {
                                        step++;
                                } else if (slide < count) {
                                        slide++;
                                        step = 0;
                                } else if (_loop) {
                                        slide = 1; // kiosk mode: round again
                                        step = 0;
                                }
                                break;
                        case Action::Previous:
                                // Back onto a slide shows it as it was left: whole.
                                if (step > 0) {
                                        step--;
                                } else if (slide > 1) {
                                        slide--;
                                        step = steps(slide);
                                }
                                break;
                        case Action::First:
                                slide = 1;
                                step = 0;
                                break;
                        case Action::Last:
                                slide = count;
                                step = steps(count);
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
                SlideLayout *layout = layoutOf(slide);
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

        void Presenter::show(bool forward, bool play) {
                // No pictures while a demo has the screen (the deck saved meanwhile): drawn as it opens again.
                if (_released) return;
                _arriving = play;
                // Script regions first: what they show at the new step.
                bool same = _shownSlide == _slide;
                if (same && forward && _step == _shownStep + 1) {
                        if (stepRegions()) {
                                // Only a region's beat: its frames change, nothing else.
                                _shownStep = _step;
                                _aheadValid = false;
                                redrawRegions(true);
                                prepareAhead();
                                updateAnimation();
                                printNotes();
                                return;
                        }
                } else if (!(same && _step == _shownStep)) {
                        // Onto another slide's start going on: its regions play their first beat;
                        // anything else (back, reading the deck again) shows its beats played.
                        seekRegions(_slide, _step, !(play && _step == 0 && !same));
                }
                // The new picture into _ahead (it may be there already), the old one kept in _shown.
                bool prepared = _aheadValid && _aheadSlide == _slide && _aheadStep == _step;
                if (!prepared) draw(_ahead, _slide, _step);
                // Drawn ahead on this slide, its regions may have moved since.
                _regionsStale = prepared && same;
                _aheadValid = false;
                // Onto another slide going forward: its transition, if the screen can play one.
                const Slide *s = _deck->slide(_slide);
                Transition   t;
                const char  *why = "";
                bool         transition =
                        _screen && forward && _shownSlide != 0 && _shownSlide != _slide && s != nullptr &&
                        parseTransition(s->settings.transition.isEmpty() ? StringView("cut") : s->settings.transition,
                                        &t, &why) &&
                        t.kind != TransitionKind::Cut && t.durationMs > 0;
                if (transition && _pieces == nullptr) _pieces = static_cast<Piece *>(malloc(MaxPieces * sizeof(Piece)));
                SlideLayout *layout = _loaded->layouts[_slide - 1];
                if (transition && (_pieces == nullptr || (t.kind == TransitionKind::Type && layout == nullptr)))
                        transition = false;
                if (transition) {
                        // The transition's frames from now; the regions wait for its end.
                        if (_animating) _platform.animate(false);
                        _animating = false;
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
                uint32_t from = _shownSlide;
                _shownSlide = _slide;
                _shownStep = _step;
                if (_regionsStale) redrawRegions(false);
                _regionsStale = false;
                if (_mode == Mode::Transition) {
                        _mode = Mode::Slides;
                        _platform.endPieces();
                        _platform.animate(false);
                }
                if (from != _shownSlide) arrived(from);
                if (_demo == Demo::Starting && startDemo()) {
                        // Its demo running (§6a): the screen is its memory until it ends.
                        printNotes();
                        return;
                }
                if (_inkCount > 0 || _dotShown) decorate(_shown, Rect{0, 0, _shown.width(), _shown.height()});
                if (_screen) _platform.present(_shown);
                prepareAhead();
                updatePointer();
                updateAnimation();
                printNotes();
                updateAuto();
                return;
        }

        // ---- Interactive pieces

        void Presenter::arrived(uint32_t from) {
                // Another slide shown: the one before's regions go quiet and give back the keys; this
                // one's are heard, its sound plays (going on), and an interactive slide enters its
                // region.
                leave();
                _pointerRegion = nullptr;
                clearInk(false);
                _slideStart = _platform.now();
                SlideLayout *before = from != 0 ? layoutOf(from) : nullptr;
                for (const DrawOp *o = before != nullptr ? before->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type == OpType::Script && o->script != nullptr) o->script->setAudible(false);
                }
                SlideLayout *now = layoutOf(_shownSlide);
                for (const DrawOp *o = now != nullptr ? now->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type == OpType::Script && o->script != nullptr) o->script->setAudible(true);
                }
                const Slide *s = _deck->slide(_shownSlide);
                if (s != nullptr && _arriving && !s->sound.isEmpty() && _screen) {
                        if (_slideSound >= 0) _platform.closeSound(_slideSound);
                        _slideSound = -1;
                        char folder[512], path[700];
                        folderOf(_path, folder, sizeof folder);
                        snprintf(path, sizeof path, "%s/%.*s", folder, int(s->sound.length()), s->sound.data());
                        _slideSound = _platform.openSound(path);
                        if (_slideSound >= 0) {
                                _platform.playSound(_slideSound, 1);
                        } else if (_messages != nullptr) {
                                fprintf(_messages, "preso: slide %u: sound %s can't be played\n", unsigned(_shownSlide),
                                        path);
                        }
                }
                if (s != nullptr && s->interactive) {
                        const DrawOp *o = firstInteractive();
                        if (o != nullptr) enter(o);
                }
                // Its demo (§6a), stepping onto it: started by finishTransition(), the screen given back.
                _demo = Demo::None;
                if (s != nullptr && !s->run.isEmpty() && _arriving && _screen)
                        _demo = _demosAllowed ? Demo::Starting : Demo::Declined;
                return;
        }

        bool Presenter::startDemo() {
                // The screen and the pictures given back first: a program may need the memory before it
                // opens its window (Lucky Duck its sprites). Not started: the screen again.
                const Slide *s = _deck->slide(_shownSlide);
                char         command[512], folder[512];
                snprintf(command, sizeof command, "%.*s", int(s->run.length()), s->run.data());
                folderOf(_path, folder, sizeof folder);
                releaseScreen();
                if (_platform.startDemo(command, folder) == Error::Ok) {
                        _demo = Demo::Running;
                        return true;
                }
                _demo = Demo::Failed;
                _released = false;
                if (openScreen() != Error::Ok) {
                        _platform.quit();
                        return true;
                }
                draw(_shown, _shownSlide, _shownStep);
                return false;
        }

        const DrawOp *Presenter::firstInteractive() {
                SlideLayout *layout = _shownSlide != 0 ? layoutOf(_shownSlide) : nullptr;
                for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type == OpType::Script && o->script != nullptr && o->fragment <= _shownStep &&
                            o->script->isInteractive()) {
                                return o;
                        }
                }
                return nullptr;
        }

        const DrawOp *Presenter::regionAt(int32_t x, int32_t y) {
                // The last laid out over the point: the one on top.
                SlideLayout  *layout = _shownSlide != 0 ? layoutOf(_shownSlide) : nullptr;
                const DrawOp *found = nullptr;
                for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type != OpType::Script || o->script == nullptr || o->fragment > _shownStep) continue;
                        const Rect &r = o->clip;
                        if (x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height) found = o;
                }
                return found;
        }

        void Presenter::enter(const DrawOp *op) {
                if (op == _entered) return;
                leave();
                if (op == nullptr || !op->script->isInteractive()) return;
                _entered = op;
                op->script->setEntered(true);
                _platform.setInputMode(InputMode::Script);
                if (_screen && _mode == Mode::Slides) redrawRegions(true);
                updateAnimation();
                return;
        }

        void Presenter::leave() {
                if (_entered == nullptr) return;
                _entered->script->setEntered(false);
                _entered = nullptr;
                _platform.setInputMode(InputMode::Deck);
                if (_screen && _mode == Mode::Slides) redrawRegions(true);
                updateAnimation();
                return;
        }

        void Presenter::toRegion(const DrawOp *op, const ScriptInput &input) {
                // Input for a region: what it changed drawn again and shown.
                ScriptInput in = input;
                in.x -= op->x;
                in.y -= op->y;
                Rect         changed[ScriptRegion::MaxAreas];
                uint32_t     count = op->script->input(in, changed);
                SlideLayout *layout = layoutOf(_shownSlide);
                if (layout != nullptr && count > 0 && _screen && _mode == Mode::Slides && _blank == NoColor) {
                        Rect     damage[MaxDamage];
                        uint32_t n = 0;
                        for (uint32_t i = 0; i < count; i++) {
                                Rect area = Rect{op->x + changed[i].x, op->y + changed[i].y, changed[i].width,
                                                 changed[i].height}
                                                    .intersected(op->clip);
                                if (area.isEmpty()) continue;
                                redrawArea(area);
                                addDamage(damage, n, area);
                        }
                        if (n > 0) _platform.presentAreas(_shown, damage, n);
                }
                updateAnimation();
                return;
        }

        void Presenter::pointer(const Event &event) {
                // Over a region (or held since a button went down in one): its. A press enters it.
                if (_mode != Mode::Slides || _blank != NoColor) return;
                uint32_t      pressed = event.buttons & ~_buttons, released = _buttons & ~event.buttons;
                const DrawOp *op = _pointerRegion != nullptr ? _pointerRegion : regionAt(event.x, event.y);
                _buttons = event.buttons;
                if (op == nullptr || !op->script->isInteractive()) {
                        if (_buttons == 0) _pointerRegion = nullptr;
                        // The presenter's own: the dot follows it, the first button draws ink.
                        if (event.type == Event::Type::Wheel) return;
                        if ((pressed & 1u) != 0) {
                                ink(event.x, event.y, true);
                        } else if ((_buttons & 1u) != 0) {
                                ink(event.x, event.y, false);
                        }
                        moveDot(event.x, event.y);
                        return;
                }
                ScriptInput in;
                in.x = event.x;
                in.y = event.y;
                if (event.type == Event::Type::Wheel) {
                        in.type = ScriptInput::Type::Wheel;
                        in.wheel = event.wheel;
                        toRegion(op, in);
                        return;
                }
                if (pressed != 0) {
                        if (op != _entered) enter(op);
                        _pointerRegion = op;
                        for (int32_t b = 0; b < 3; b++) {
                                if ((pressed & (1u << b)) == 0) continue;
                                in.type = ScriptInput::Type::PointerDown;
                                in.button = b + 1;
                                toRegion(op, in);
                        }
                } else if (released != 0) {
                        for (int32_t b = 0; b < 3; b++) {
                                if ((released & (1u << b)) == 0) continue;
                                in.type = ScriptInput::Type::PointerUp;
                                in.button = b + 1;
                                toRegion(op, in);
                        }
                        if (_buttons == 0) _pointerRegion = nullptr;
                } else {
                        in.type = ScriptInput::Type::PointerMove;
                        toRegion(op, in);
                }
                return;
        }

        void Presenter::updatePointer() {
                // The system's pointer where there is something to point at.
                bool want = _screen && _mode == Mode::Slides && firstInteractive() != nullptr;
                if (want != _pointerShown) _platform.setPointerVisible(want);
                _pointerShown = want;
                return;
        }

        void Presenter::watchFolders(const char *deckFolder) {
                // The folders the deck's scripts and pictures are in (`anim/`), each once: an edit
                // there reloads the deck as an edit of the deck does.
                constexpr uint32_t Max = 7;
                StringView         seen[Max];
                uint32_t           n = 0;
                auto               watchOf = [&](StringView file) {
                        int32_t slash = -1;
                        for (uint32_t i = 0; i < file.length(); i++) {
                                if (file[i] == '/') slash = int32_t(i);
                        }
                        if (slash <= 0 || n == Max || file.startsWith("/") || file.indexOf(':') >= 0) return;
                        StringView sub = file.left(uint32_t(slash));
                        for (uint32_t i = 0; i < n; i++) {
                                if (seen[i].equals(sub)) return;
                        }
                        seen[n++] = sub;
                        char path[600];
                        snprintf(path, sizeof path, "%s/%.*s", deckFolder, int(sub.length()), sub.data());
                        (void)_platform.watch(path);
                };
                // Every image of the slides: their backgrounds, and those in their text.
                auto inBlocks = [&](auto &self, const Block *b) -> void {
                        for (; b != nullptr; b = b->next) {
                                for (const Inline *i = b->inlines; i != nullptr; i = i->next) {
                                        if (i->image != nullptr)
                                                watchOf(i->image->isScript() ? i->image->script : i->image->path);
                                }
                                self(self, b->children);
                        }
                };
                for (const Slide *s = _deck->slides(); s != nullptr; s = s->next) {
                        for (const Image *i = s->backgrounds; i != nullptr; i = i->next) {
                                watchOf(i->isScript() ? i->script : i->path);
                        }
                        inBlocks(inBlocks, s->blocks);
                }
                return;
        }

        uint32_t Presenter::demoNames(char *out, size_t size) {
                // The programs the deck's `run:` slides start, each once: "luckyduck, sysmon".
                uint32_t n = 0;
                size_t   used = 0;
                out[0] = '\0';
                for (const Slide *s = _deck->slides(); s != nullptr; s = s->next) {
                        if (s->run.isEmpty()) continue;
                        StringView name = s->run.trimmed();
                        int32_t    space = name.indexOf(' ');
                        if (space > 0) name = name.left(uint32_t(space));
                        int32_t slash = -1;
                        for (uint32_t i = 0; i < name.length(); i++) {
                                if (name[i] == '/') slash = int32_t(i);
                        }
                        if (slash >= 0) name = name.mid(uint32_t(slash) + 1);
                        bool seen = false;
                        for (const Slide *t = _deck->slides(); t != s && !seen; t = t->next) {
                                if (t->run.isEmpty()) continue;
                                StringView other = t->run.trimmed();
                                int32_t    sp = other.indexOf(' ');
                                if (sp > 0) other = other.left(uint32_t(sp));
                                int32_t sl = -1;
                                for (uint32_t i = 0; i < other.length(); i++) {
                                        if (other[i] == '/') sl = int32_t(i);
                                }
                                if (sl >= 0) other = other.mid(uint32_t(sl) + 1);
                                seen = other.equals(name);
                        }
                        if (seen) continue;
                        int k = snprintf(out + used, size - used, "%s%.*s", n > 0 ? ", " : "", int(name.length()),
                                         name.data());
                        if (k < 0 || used + size_t(k) >= size) break;
                        used += size_t(k);
                        n++;
                }
                return n;
        }

        void Presenter::drawQuestion() {
                // A box in the middle of the screen: what the deck runs, and the keys.
                Canvas screen;
                if (!_screen || !_platform.beginFrame(screen)) return;
                int32_t w = screen.width(), h = screen.height();
                screen.clearClip();
                screen.fill(Rect{0, 0, w, h}, 0x202020);
                FontKey font = _loaded->themes.base().header.font;
                font.family = "IBM Plex Sans";
                font.style = 0;
                font.size = uint16_t(h / 24 > 12 ? h / 24 : 12);
                FontMetrics m = _fonts.metrics(font);
                char        names[200], line[260];
                demoNames(names, sizeof names);
                snprintf(line, sizeof line, "This deck runs programs: %s", names);
                const char *lines[3] = {line, "Y: run them as the deck reaches them",
                                        "N or Escape: present it without them"};
                int32_t     y = h / 2 - 2 * m.lineHeight;
                for (int i = 0; i < 3; i++) {
                        StringView t(lines[i]);
                        t = t.left(_fonts.fit(font, t, w - 2 * m.lineHeight));
                        int32_t tw = _fonts.width(font, t);
                        _fonts.draw(screen, font, (w - tw) / 2, y, t, i == 0 ? 0xFFFFFF : 0xC8C8C8);
                        y += m.lineHeight * (i == 0 ? 2 : 1);
                }
                _platform.endFrame();
                return;
        }

        void Presenter::answer(bool yes) {
                _demosAllowed = yes;
                _mode = Mode::Slides;
                if (_messages != nullptr) {
                        fprintf(_messages, "preso: the deck's demos %s\n", yes ? "will run" : "won't run");
                        fflush(_messages);
                }
                show(false, true);
                return;
        }

        // ---- Notes, the timer (§6b)

        void Presenter::printNotes() {
                // The slide and step shown, once each: the notes for them.
                if (_messages == nullptr || _shownSlide == 0) return;
                if (_shownSlide == _notedSlide && _shownStep == _notedStep) return;
                int64_t now = _platform.now();
                if (_talkStart < 0) {
                        _talkStart = now;
                        _slideStart = now;
                }
                _notedSlide = _shownSlide;
                _notedStep = _shownStep;
                NotesTime time;
                time.talk = now - _talkStart;
                time.slide = now - _slideStart;
                Buffer out;
                if (_notesTerminal) out.append("\033[H\033[2J"); // the screen cleared: these notes on it
                writeNotes(out, *_deck, _shownSlide, _shownStep, steps(_shownSlide), time, 78);
                const Slide *shown = _deck->slide(_shownSlide);
                if (shown != nullptr && !shown->run.isEmpty()) {
                        static constexpr const char *States[] = {
                                "runs stepping onto the slide", "running", "not run: the deck's demos were declined",
                                "couldn't be started",          "ended",   "starting"};
                        out.appendf("\nDemo: %.*s (%s)\n", int(shown->run.length()), shown->run.data(),
                                    States[int(_demo)]);
                }
                if (_deck->diagnosticCount() > 0) {
                        out.appendf("\n(%u problems in the deck: preso check says what)\n",
                                    unsigned(_deck->diagnosticCount()));
                }
                fwrite(out.view().data(), 1, out.length(), _messages);
                fflush(_messages);
                return;
        }

        void Presenter::writeTimer() {
                // On a terminal, the notes' first line written again in place.
                if (!_notesTerminal || _talkStart < 0 || _notedSlide == 0) return;
                int64_t   now = _platform.now();
                NotesTime time;
                time.talk = now - _talkStart;
                time.slide = now - _slideStart;
                Buffer out;
                out.append("\0337\033[1;1H\033[2K"); // the cursor kept, the first line cleared
                writeRule(out, *_deck, _notedSlide, time, 78);
                out.append("\0338");
                fwrite(out.view().data(), 1, out.length(), _messages);
                fflush(_messages);
                return;
        }

        void Presenter::updateTicking() {
                // A second's tick while something shows the time or will hide: nothing otherwise.
                bool want = _notesTerminal || _dotShown;
                if (want != _ticking) _platform.setTicking(want);
                _ticking = want;
                return;
        }

        void Presenter::second() {
                writeTimer();
                if (_dotShown && _platform.now() - _dotMoved > 2000000000 && !stickMoving()) {
                        // Still two seconds: the dot goes.
                        Rect was = dotBox();
                        _dotShown = false;
                        if (_screen && _mode == Mode::Slides && _blank == NoColor) {
                                redrawArea(was);
                                _platform.presentAreas(_shown, &was, 1);
                        }
                        updateTicking();
                }
                return;
        }

        void Presenter::demoEnded(int32_t status) {
                // The demo is over and the screen back: the slide after it.
                if (_demo != Demo::Running) return;
                _demo = Demo::Ended;
                _demoStatus = status;
                if (_messages != nullptr && !_notesTerminal) {
                        fprintf(_messages, "preso: slide %u: its demo ended (status %d)\n", unsigned(_shownSlide),
                                int(status));
                        fflush(_messages);
                }
                if (_released) {
                        // The screen again, the pictures drawn afresh: no transition from what was.
                        _released = false;
                        _shownSlide = 0;
                        Error error = openScreen();
                        if (error != Error::Ok) {
                                if (_messages != nullptr)
                                        fprintf(_messages, "preso: the screen didn't open again after the demo: %s\n",
                                                errorName(error));
                                _platform.quit();
                                return;
                        }
                }
                act(Action::Next);
                if (_shownSlide == 0) show(false, false); // the last slide's demo: that slide again
                return;
        }

        void Presenter::terminalInput(const char *data, uint32_t length) {
                // The laptop's keys over SSH: mapped as the screen's are (terminal.h).
                for (uint32_t i = 0; i < length; i++) {
                        Event key;
                        if (!_terminalKeys.push(data[i], &key)) continue;
                        if (_mode == Mode::Question) {
                                if ((key.key & 0xFFFF) == 0x29) {
                                        act(Action::Quit);
                                } else {
                                        this->key(key);
                                }
                                continue;
                        }
                        InputMode mode = _entered != nullptr       ? InputMode::Script
                                         : _mode == Mode::Overview ? InputMode::Overview
                                                                   : InputMode::Deck;
                        Action    a = terminalAction(key, mode);
                        if (a != Action::None) {
                                act(a);
                        } else {
                                event(key);
                        }
                }
                return;
        }

        // ---- The pointer's dot and the ink (§6b)

        Rect Presenter::dotBox() const {
                int32_t r = _shown.width() >= 1000 ? 12 : 7;
                return Rect{int32_t(_dotX) - r - 1, int32_t(_dotY) - r - 1, 2 * r + 3, 2 * r + 3};
        }

        bool Presenter::stickMoving() const {
                constexpr int32_t Dead = 6000; // a stick at rest isn't quite centred
                return _stickX * _stickX + _stickY * _stickY > Dead * Dead;
        }

        void Presenter::redrawArea(const Rect &area) {
                // The shown slide drawn again there, the dot and the ink over it.
                SlideLayout *layout = _shownSlide != 0 ? layoutOf(_shownSlide) : nullptr;
                if (layout == nullptr) return;
                drawSlide(*layout, _shownStep, _shown, _fonts, &_images, UINT32_MAX, &area);
                decorate(_shown, area);
                return;
        }

        void Presenter::decorate(Canvas &canvas, const Rect &area) {
                // The ink, then the dot (where the system's pointer isn't), in the accent colour.
                const Theme &theme = _loaded->themes.base();
                canvas.setClip(area);
                float width = canvas.width() >= 1000 ? 4.0f : 2.5f;
                for (uint32_t i = 0; i < _strokeCount; i++) {
                        uint32_t first = _strokes[i];
                        uint32_t end = i + 1 < _strokeCount ? _strokes[i + 1] : _inkCount;
                        Path     path;
                        path.addStroke(_ink + first, end - first, false, width);
                        (void)fillPath(canvas, path, theme.accent, 255);
                }
                if (_dotShown && !_pointerShown) {
                        Path  path;
                        float r = canvas.width() >= 1000 ? 11.0f : 6.0f;
                        path.addEllipse(_dotX, _dotY, r, r);
                        (void)fillPath(canvas, path, theme.accent, 200);
                }
                canvas.clearClip();
                return;
        }

        void Presenter::moveDot(int32_t x, int32_t y) {
                // Where the system's pointer shows (an interactive slide), no dot.
                x = x < 0 ? 0 : x >= _shown.width() ? _shown.width() - 1 : x;
                y = y < 0 ? 0 : y >= _shown.height() ? _shown.height() - 1 : y;
                _dotMoved = _platform.now();
                if (_pointerShown) return;
                Rect was = _dotShown ? dotBox() : Rect{};
                _dotX = float(x);
                _dotY = float(y);
                _dotShown = true;
                Rect now = dotBox();
                if (_screen && _mode == Mode::Slides && _blank == NoColor && _typedLength == 0) {
                        Rect     areas[2] = {was, now};
                        uint32_t n = 0;
                        for (const Rect &a : areas) {
                                if (a.isEmpty()) continue;
                                redrawArea(a);
                                areas[n++] = a;
                        }
                        _platform.presentAreas(_shown, areas, n);
                }
                updateTicking();
                return;
        }

        void Presenter::ink(int32_t x, int32_t y, bool start) {
                // A point of the stroke being drawn (@p start: a new stroke), drawn as it goes.
                if (_inkCount + 1 > _inkCapacity) {
                        uint32_t capacity = _inkCapacity != 0 ? _inkCapacity * 2 : 1024;
                        auto    *grown = static_cast<Point *>(realloc(_ink, capacity * sizeof(Point)));
                        if (grown == nullptr) return;
                        _ink = grown;
                        _inkCapacity = capacity;
                }
                if (start || _strokeCount == 0) {
                        if (_strokeCount + 1 > _strokeCapacity) {
                                uint32_t capacity = _strokeCapacity != 0 ? _strokeCapacity * 2 : 64;
                                auto *grown = static_cast<uint32_t *>(realloc(_strokes, capacity * sizeof(uint32_t)));
                                if (grown == nullptr) return;
                                _strokes = grown;
                                _strokeCapacity = capacity;
                        }
                        _strokes[_strokeCount++] = _inkCount;
                }
                Point p{float(x), float(y)};
                Point before = _inkCount > _strokes[_strokeCount - 1] ? _ink[_inkCount - 1] : p;
                _ink[_inkCount++] = p;
                if (!_screen || _mode != Mode::Slides || _blank != NoColor) return;
                int32_t pad = 6;
                int32_t x0 = int32_t(before.x < p.x ? before.x : p.x) - pad,
                        y0 = int32_t(before.y < p.y ? before.y : p.y) - pad;
                int32_t x1 = int32_t(before.x > p.x ? before.x : p.x) + pad,
                        y1 = int32_t(before.y > p.y ? before.y : p.y) + pad;
                Rect area = Rect{x0, y0, x1 - x0, y1 - y0}.intersected(Rect{0, 0, _shown.width(), _shown.height()});
                redrawArea(area);
                _platform.presentAreas(_shown, &area, 1);
                return;
        }

        void Presenter::clearInk(bool redraw) {
                if (_inkCount == 0) return;
                // Where it was, drawn again without it.
                float x0 = _ink[0].x, y0 = _ink[0].y, x1 = x0, y1 = y0;
                for (uint32_t i = 1; i < _inkCount; i++) {
                        x0 = _ink[i].x < x0 ? _ink[i].x : x0;
                        y0 = _ink[i].y < y0 ? _ink[i].y : y0;
                        x1 = _ink[i].x > x1 ? _ink[i].x : x1;
                        y1 = _ink[i].y > y1 ? _ink[i].y : y1;
                }
                _inkCount = 0;
                _strokeCount = 0;
                if (!redraw || !_screen || _mode != Mode::Slides) return;
                Rect area = Rect{int32_t(x0) - 6, int32_t(y0) - 6, int32_t(x1 - x0) + 13, int32_t(y1 - y0) + 13}
                                    .intersected(Rect{0, 0, _shown.width(), _shown.height()});
                redrawArea(area);
                if (_blank == NoColor) _platform.presentAreas(_shown, &area, 1);
                return;
        }

        // ---- Script regions

        void Presenter::seekRegions(uint32_t slide, uint32_t step, bool played) {
                SlideLayout *layout = layoutOf(slide);
                for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type != OpType::Script || o->script == nullptr) continue;
                        // Not there yet: it plays from its start when it appears.
                        if (o->fragment > step) {
                                o->script->seek(0, false);
                                continue;
                        }
                        o->script->seek(beatAt(*o, step), played);
                }
                return;
        }

        bool Presenter::stepRegions() {
                // One step on: a region appearing plays its first beat, a region whose beat this is
                // goes on to it. True if nothing but beats changed.
                SlideLayout *layout = layoutOf(_slide);
                if (layout == nullptr) return false;
                for (const DrawOp *o = layout->ops; o != nullptr; o = o->next) {
                        if (o->type != OpType::Script || o->script == nullptr || o->fragment > _step) continue;
                        if (o->fragment == _step) {
                                o->script->seek(0, false);
                        } else if (beatAt(*o, _step) > beatAt(*o, _step - 1)) {
                                o->script->next();
                        }
                }
                return beatOnly(*layout, _step);
        }

        void Presenter::redrawRegions(bool present) {
                // Each region's part of the slide drawn again into _shown, as it now is.
                SlideLayout *layout = _shownSlide != 0 ? layoutOf(_shownSlide) : nullptr;
                Rect         damage[MaxDamage];
                uint32_t     n = 0;
                for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type != OpType::Script || o->script == nullptr || o->fragment > _shownStep) continue;
                        Rect area = o->clip;
                        redrawArea(area);
                        addDamage(damage, n, area);
                }
                if (present && _screen && n > 0) _platform.presentAreas(_shown, damage, n);
                return;
        }

        bool Presenter::regionsMoving() {
                SlideLayout *layout = _shownSlide != 0 ? layoutOf(_shownSlide) : nullptr;
                for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type == OpType::Script && o->script != nullptr && o->fragment <= _shownStep &&
                            o->script->isMoving()) {
                                return true;
                        }
                }
                return false;
        }

        void Presenter::updateAnimation() {
                // Regions' clocks run while their slide is shown as it is: not blanked, not under an
                // overlay, not in a transition or the overview.
                bool want = _screen && _mode == Mode::Slides && _blank == NoColor && _typedLength == 0 &&
                            (regionsMoving() || stickMoving());
                if (want && !_animating) {
                        _lastFrame = _platform.now();
                        _platform.animate(true);
                } else if (!want && _animating) {
                        _platform.animate(false);
                }
                _animating = want;
                return;
        }

        void Presenter::frameRegions(int64_t ns) {
                int64_t elapsed = ns - _lastFrame;
                _lastFrame = ns;
                if (elapsed < 0) elapsed = 0;
                if (stickMoving()) {
                        // Across the slide in about a second and a half at full tilt.
                        float speed = float(_shown.width()) / 1.5f * float(elapsed) / 1e9f / 32767.0f;
                        moveDot(int32_t(_dotX + float(_stickX) * speed), int32_t(_dotY + float(_stickY) * speed));
                }
                SlideLayout *layout = layoutOf(_shownSlide);
                Rect         damage[MaxDamage];
                uint32_t     n = 0;
                for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr; o = o->next) {
                        if (o->type != OpType::Script || o->script == nullptr || o->fragment > _shownStep ||
                            !o->script->isMoving()) {
                                continue;
                        }
                        Rect     changed[ScriptRegion::MaxAreas];
                        uint32_t count = o->script->advance(elapsed, changed);
                        // What changed, drawn again: the slide under it, the region, what is over it.
                        for (uint32_t i = 0; i < count; i++) {
                                Rect area = Rect{o->x + changed[i].x, o->y + changed[i].y, changed[i].width,
                                                 changed[i].height}
                                                    .intersected(o->clip);
                                if (area.isEmpty()) continue;
                                drawSlide(*layout, _shownStep, _shown, _fonts, &_images, UINT32_MAX, &area);
                                decorate(_shown, area);
                                addDamage(damage, n, area);
                        }
                }
                if (n > 0) {
                        _platform.presentAreas(_shown, damage, n);
                } else {
                        _platform.waitFrame();
                }
                if (!regionsMoving() && !stickMoving()) updateAnimation();
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
                if (_animating) {
                        frameRegions(ns);
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
                if (slide == _slide) {
                        // A beat changes only its region: nothing to draw ahead.
                        SlideLayout *layout = layoutOf(slide);
                        if (layout != nullptr && beatOnly(*layout, step)) return;
                } else {
                        // The next slide as it appears: its regions at their first beat's start.
                        seekRegions(slide, 0, false);
                }
                draw(_ahead, slide, step);
                _aheadSlide = slide;
                _aheadStep = step;
                _aheadValid = true;
                return;
        }

        void Presenter::showOverlay() {
                // The slide, with the number being typed in a box over its corner (or blanked); its
                // regions wait meanwhile.
                updateAnimation();
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
                        updateAnimation();
                        return;
                }
                show(forward && slide == _shownSlide + 1, true);
                return;
        }

        void Presenter::act(Action action) {
                // Someone's action gives the step `auto`'s whole time again (§8a).
                actNow(action);
                updateAuto(true);
                return;
        }

        bool Presenter::autoFree() const {
                // Nothing holding the deck: the overview, a number, a blank screen, the question, a
                // region with the keys, a demo.
                return _screen && _mode == Mode::Slides && _blank == NoColor && _typedLength == 0 &&
                       _entered == nullptr && _demo != Demo::Running;
        }

        void Presenter::updateAuto(bool restart) {
                // The shown step's `auto`, timed from when it was shown (@p restart: from now); off
                // where there is no step after it (the end, without --loop).
                const Slide *s = _deck != nullptr ? _deck->slide(_shownSlide) : nullptr;
                uint32_t     ms = s != nullptr ? s->settings.autoMs : 0;
                uint32_t     slide = 0, step = 0;
                if (ms > 0) target(Action::Next, slide, step);
                bool want = ms > 0 && autoFree() && _slide == _shownSlide && _step == _shownStep &&
                            !(slide == _slide && step == _step);
                if (!want) {
                        if (_autoArmed) _platform.setAlarm(0);
                        _autoArmed = false;
                        return;
                }
                if (_autoArmed && !restart && _autoSlide == _shownSlide && _autoStep == _shownStep) return;
                _autoArmed = true;
                _autoSlide = _shownSlide;
                _autoStep = _shownStep;
                _platform.setAlarm(_platform.now() + int64_t(ms) * 1000000);
                return;
        }

        void Presenter::alarm() {
                // Its time: the next step, unless something holds the deck now (then later).
                _autoArmed = false;
                if (autoFree()) {
                        act(Action::Next);
                } else {
                        updateAuto();
                }
                return;
        }

        void Presenter::actNow(Action action) {
                // A demo running has the screen: the deck waits for it (quitting aside).
                if (_demo == Demo::Running && action != Action::Quit) return;
                if (_mode == Mode::Question) {
                        if (action == Action::Quit || action == Action::Close || action == Action::Leave) answer(false);
                        return;
                }
                if (action == Action::Into) {
                        enter(firstInteractive());
                        return;
                }
                if (action == Action::Leave) {
                        leave();
                        return;
                }
                if (action == Action::ClearInk) {
                        clearInk(true);
                        return;
                }
                // Stepping (a clicker's keys work while a region has the others): the keys back first.
                if (_entered != nullptr && action != Action::Next && action != Action::Previous) leave();
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
                                        updateAnimation();
                                } else {
                                        showOverlay();
                                }
                                return;
                        }
                        _typedLength = 0;
                        if (action == Action::Quit) {
                                if (_screen) _platform.present(_shown);
                                updateAnimation();
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
                                        updateAnimation();
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
                        updateAnimation();
                        return;
                }
                _blank = NoColor;
                uint32_t slide = 0, step = 0;
                target(action, slide, step);
                if (slide == _slide && step == _step) return;
                _slide = slide;
                _step = step;
                show(action == Action::Next, action == Action::Next);
                return;
        }

        void Presenter::key(const Event &event) {
                uint32_t usage = event.key & 0xFFFF;
                bool     enter = usage == 0x28 || usage == 0x58;
                if (_mode == Mode::Question) {
                        // Y yes; N no (Escape comes as Quit).
                        if (usage == 0x1C || event.character == 'y' || event.character == 'Y') answer(true);
                        if (usage == 0x11 || event.character == 'n' || event.character == 'N') answer(false);
                        return;
                }
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
                        if (n >= 1 && n <= _deck->slideCount() && n != _slide) {
                                goTo(n);
                        } else if (_screen) {
                                _platform.present(_shown);
                        }
                        updateAnimation();
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
                if (_animating) _platform.animate(false);
                _animating = false;
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
                        show(false, true);
                        return;
                }
                if (_screen) _platform.present(_shown);
                prepareAhead();
                updateAnimation();
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
                        // As it ends; the slide shown keeps its regions as they are.
                        if (n != _slide) seekRegions(n, steps(n), true);
                        draw(_ahead, n, steps(n));
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
                        show(false, false);
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
                if (_step > steps(_slide)) _step = steps(_slide);
                _status[0] = '\0';
                printNewDiagnostics();
                _aheadValid = false;
                // The slide shown again as it now is, without a transition; the overview's
                // thumbnails were of the deck as it was.
                if (_mode == Mode::Overview) closeOverview(false);
                if (_mode == Mode::Transition) finishTransition();
                dropThumbnails();
                _shownSlide = 0;
                show(false, false);
                return true;
        }

        Error Presenter::openScreen() {
                // The two pictures, then the screen: at the start, and again after a demo had it.
                const Layouter &layouter = *_loaded->layouter;
                int32_t         w = layouter.width(), h = layouter.height();
                PixelFormat     format = _platform.pixelFormat();
                uint32_t        stride = uint32_t(w) * bytesPerPixel(format);
                for (int i = 0; i < 2; i++) {
                        if (_pixels[i] == nullptr) _pixels[i] = _platform.allocatePixels(stride * uint32_t(h));
                        if (_pixels[i] == nullptr) return Error::NoMemory;
                }
                _shown.attach(_pixels[0], stride, w, h, format);
                _ahead.attach(_pixels[1], stride, w, h, format);
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
                return Error::Ok;
        }

        void Presenter::releaseScreen() {
                // For a demo: what is drawn again from the deck is given back (the pictures, the
                // screen's buffers, scaled images, thumbnails), the deck itself kept.
                if (_mode == Mode::Overview) closeOverview(false);
                if (_mode == Mode::Transition) finishTransition();
                _screen = false;
                updateAnimation();
                dropThumbnails();
                _images.clear();
                _platform.closeScreen();
                for (void *&pixels : _pixels) {
                        _platform.freePixels(pixels);
                        pixels = nullptr;
                }
                _shown = Canvas();
                _ahead = Canvas();
                _aheadValid = false;
                _released = true;
                return;
        }

        Error Presenter::exportSlides(const char *folder, bool posters, uint32_t *written) {
                *written = 0;
                if (_deck == nullptr) return Error::Invalid;
                Error error = _platform.makeFolder(folder);
                if (error != Error::Ok && error != Error::NotSupported) return error; // none: it must be there
                // One picture, drawn step by step as the screen would show each once it has played.
                const Layouter &layouter = *_loaded->layouter;
                int32_t         w = layouter.width(), h = layouter.height();
                PixelFormat     format = _platform.pixelFormat();
                uint32_t        stride = uint32_t(w) * bytesPerPixel(format);
                if (_pixels[1] == nullptr) _pixels[1] = _platform.allocatePixels(stride * uint32_t(h));
                if (_pixels[1] == nullptr) return Error::NoMemory;
                _ahead.attach(_pixels[1], stride, w, h, format);
                _aheadValid = false;
                char deckFolder[512];
                folderOf(_path, deckFolder, sizeof deckFolder);
                char path[1024];
                auto write = [&](const Rect &area) {
                        error = _platform.writePicture(path, _ahead, area);
                        if (error != Error::Ok) {
                                if (_messages != nullptr) fprintf(_messages, "preso: %s: %s\n", path, errorName(error));
                                return false;
                        }
                        ++*written;
                        return true;
                };
                for (uint32_t slide = 1; slide <= _deck->slideCount(); slide++) {
                        uint32_t n = steps(slide);
                        for (uint32_t step = 0; step <= n; step++) {
                                seekRegions(slide, step, true);
                                draw(_ahead, slide, step);
                                if (n == 0) {
                                        snprintf(path, sizeof path, "%s/%03u.png", folder, unsigned(slide));
                                } else {
                                        snprintf(path, sizeof path, "%s/%03u-%u.png", folder, unsigned(slide),
                                                 unsigned(step + 1));
                                }
                                if (!write(Rect{0, 0, w, h})) return error;
                        }
                        if (!posters) continue;
                        // Its regions as its last step left them, already drawn: each cut out (§7.2).
                        SlideLayout *layout = layoutOf(slide);
                        for (const DrawOp *o = layout != nullptr ? layout->ops : nullptr; o != nullptr; o = o->next) {
                                if (o->type != OpType::Script || o->script == nullptr || o->image == nullptr) continue;
                                StringView script = o->image->script;
                                if (script.startsWith("/")) {
                                        snprintf(path, sizeof path, "%.*s.png", int(script.length()), script.data());
                                } else {
                                        snprintf(path, sizeof path, "%s/%.*s.png", deckFolder, int(script.length()),
                                                 script.data());
                                }
                                if (!write(Rect{o->x, o->y, o->width, o->height})) return error;
                        }
                }
                return Error::Ok;
        }

        Error Presenter::run(uint32_t number) {
                if (_deck == nullptr) return Error::Invalid;
                _slide = number >= 1 && number <= _deck->slideCount() ? number : 1;
                _step = 0;
                Error error = openScreen();
                if (error != Error::Ok) return error;
                // The deck's folder, watched: it reloads as it is saved. Without watching, it doesn't.
                char folder[512];
                folderOf(_path, folder, sizeof folder);
                if (_platform.watch(folder) != Error::Ok && _messages != nullptr) {
                        fprintf(_messages, "preso: %s isn't watched: the deck won't reload as it changes\n", folder);
                } else {
                        watchFolders(folder);
                }
                // The terminal it was started from: its keys move the deck, its screen shows the notes.
                _notesTerminal = _messages != nullptr && _platform.outputIsTerminal();
                (void)_platform.readTerminal();
                updateTicking();
                // A deck that starts programs says so first, unless trusted (§7.7).
                char names[200];
                if (!_trusted && demoNames(names, sizeof names) > 0) {
                        _mode = Mode::Question;
                        drawQuestion();
                        return _platform.run(*this);
                }
                show(false, true);
                return _platform.run(*this);
        }

        void Presenter::event(const Event &event) {
                switch (event.type) {
                        case Event::Type::Action: act(event.action); break;
                        case Event::Type::Close: _platform.quit(); break;
                        case Event::Type::Key:
                        case Event::Type::KeyUp: {
                                if (_entered != nullptr && _mode == Mode::Slides) {
                                        // An entered region's keys.
                                        ScriptInput in;
                                        in.type = event.type == Event::Type::KeyUp ? ScriptInput::Type::KeyUp
                                                                                   : ScriptInput::Type::KeyDown;
                                        in.key = event.key;
                                        in.character = event.character;
                                        in.repeat = event.repeat;
                                        in.x = _entered->x;
                                        in.y = _entered->y;
                                        toRegion(_entered, in);
                                        break;
                                }
                                if (event.type == Event::Type::Key) key(event);
                                break;
                        }
                        case Event::Type::Pointer:
                        case Event::Type::Wheel: pointer(event); break;
                        case Event::Type::Stick:
                                // A pad's stick moves the dot (frames carry it while it is pushed).
                                _stickX = event.x;
                                _stickY = event.y;
                                if (stickMoving() && !_dotShown) moveDot(_shown.width() / 2, _shown.height() / 2);
                                updateAnimation();
                                break;
                }
                updateAuto(); // a region entered or left, a number typed: `auto` held or going again
                return;
        }

        void Presenter::filesChanged() {
                reload();
                return;
        }

        int checkDeck(Platform &platform, const char *path, bool verbose, FILE *out, ScriptsMaker scripts) {
                char    *source = nullptr;
                uint32_t length = 0;
                Error    error = platform.readFile(path, Deck::MaxSource, &source, &length);
                if (error == Error::Ok) {
                        Deck     *deck = create<Deck>();
                        uint16_t *steps = nullptr; // each slide's, by its number
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
                                        // Every script runs to its end as its slide is laid out.
                                        Scripts *regions = nullptr;
                                        if (error == Error::Ok && scripts != nullptr) {
                                                ScriptSetup setup;
                                                setup.platform = &platform;
                                                setup.deck = deck;
                                                setup.themes = &themes;
                                                setup.fonts = &fonts;
                                                setup.images = &images;
                                                setup.deckPath = path;
                                                regions = scripts(setup);
                                                layouter->setScripts(regions);
                                        }
                                        steps = static_cast<uint16_t *>(
                                                calloc(deck->slideCount() + 1, sizeof(uint16_t)));
                                        for (const Slide *s = deck->slides(); s != nullptr && error == Error::Ok;
                                             s = s->next) {
                                                SlideLayout *layout = layouter->layout(*s, arena, true);
                                                if (layout == nullptr) {
                                                        error = Error::NoMemory;
                                                } else if (steps != nullptr) {
                                                        steps[s->number] = layout->steps;
                                                }
                                        }
                                        destroy(layouter);
                                        destroyScripts(regions);
                                }
                                if (error == Error::Ok) {
                                        for (const Diagnostic *d = deck->diagnostics(); d != nullptr; d = d->next) {
                                                printDiagnostic(out, path, *d);
                                        }
                                        if (verbose) {
                                                for (const Slide *s = deck->slides(); s != nullptr; s = s->next) {
                                                        fprintf(out, "%3u  %.*s", unsigned(s->number),
                                                                int(s->title.length()), s->title.data());
                                                        uint32_t n = steps != nullptr ? steps[s->number] : s->fragments;
                                                        if (n > 0) fprintf(out, "  (%u steps)", unsigned(n) + 1);
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
                                free(steps);
                                destroy(deck);
                                if (error == Error::Ok) return status;
                        }
                }
                fprintf(stderr, "preso: %s: %s\n", path, errorName(error));
                return 2;
        }

} // namespace preso
