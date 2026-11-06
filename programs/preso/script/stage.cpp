/**
 * @file      stage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Angel's stage (devplan/preso.md §8c). Until a scene call, angel runs the script as any other;
 * that call aborts angel's run (needRegion()), and the stage runs the module again from its
 * start as a region of its own: counted to its end in fast-forward, then played in a window
 * from its first beat. What the script printed before it asked is not printed again.
 */

#include "stage.h"
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace preso {

        Stage::Stage(Platform &platform, const char *path, Reporter reporter)
            : _platform(platform), _fonts(platform), _images(platform, path), _report(reporter) {
                snprintf(_path, sizeof _path, "%s", path);
                folderOf(path, _folder, sizeof _folder);
        }

        Stage::~Stage() {
                finish();
                if (_pixels != nullptr) _platform.freePixels(_pixels);
        }

        void Stage::finish() {
                if (_region == nullptr) return;
                _region->~AngelRegion();
                free(_region);
                _region = nullptr;
                return;
        }

        void Stage::attach(asIScriptEngine *engine) {
                _engine = engine;
                engine->SetUserData(static_cast<SceneHost *>(this), UserData);
                return;
        }

        bool Stage::hasDraw(asIScriptModule *module) {
                return module->GetFunctionByDecl("void draw(Scene@, float)") != nullptr;
        }

        void Stage::size(int32_t *width, int32_t *height) const {
                *width = Width;
                *height = Height;
                return;
        }

        bool Stage::resolve(StringView path, char *out, size_t size, const char **why) const {
                // From the script's folder, or as it is when absolute: angel's scripts read anything.
                int n = path.length() > 0 && path[0] == '/'
                                ? snprintf(out, size, "%.*s", int(path.length()), path.data())
                        : strcmp(_folder, ".") == 0
                                ? snprintf(out, size, "%.*s", int(path.length()), path.data())
                                : snprintf(out, size, "%s/%.*s", _folder, int(path.length()), path.data());
                if (n < 0 || size_t(n) >= size) {
                        *why = "has too long a name";
                        return false;
                }
                return true;
        }

        void Stage::report(const ScriptPlace &place, uint32_t line, const char *message) {
                // The region's message begins with its place (`file:line: `): angel says that itself.
                char prefix[160];
                snprintf(prefix, sizeof prefix, "%.*s:", int(place.file.length()), place.file.data());
                const char *text = message;
                if (strncmp(text, prefix, strlen(prefix)) == 0) {
                        text += strlen(prefix);
                        while (*text >= '0' && *text <= '9') text++;
                        if (text[0] == ':' && text[1] == ' ') text += 2;
                }
                if (_report != nullptr) _report(_path, int(line), text);
                return;
        }

        bool Stage::takeInitException(char *out, size_t size, int *line) {
                // Angel says a global's initializer's exception itself, as it builds.
                (void)out;
                (void)size;
                (void)line;
                return false;
        }

        AngelRegion *Stage::needRegion() {
                // The script's first scene call: angel's run stops here, and the stage runs it again.
                if (!_wanted) {
                        _wanted = true;
                        asIScriptContext *ctx = asGetActiveContext();
                        if (ctx != nullptr) ctx->Abort();
                }
                return nullptr;
        }

        ScriptFont *Stage::scriptFont(void *font) {
                Arena   arena;
                FontKey key = Theme::paper().body.font;
                if (!parseFont(StringView(angel::fontName(font)), arena, &key)) key = Theme::paper().body.font;
                ScriptFont *f = ScriptFont::make(key);
                if (f == nullptr) angel::raise("Out of memory for a font");
                return f;
        }

        void *Stage::makeFont(const FontKey &key) {
                char name[96];
                snprintf(name, sizeof name, "%s %u%s%s", key.family, unsigned(key.size),
                         (key.style & FontKey::Bold) != 0 ? " bold" : "",
                         (key.style & FontKey::Italic) != 0 ? " italic" : "");
                return angel::fontNamed(name);
        }

        void Stage::releaseFont(void *font) {
                angel::releaseFont(font);
                return;
        }

        void *Stage::makeEvent(const ScriptInput &in) {
                // Angel's EventType numbers are a deck's for these six.
                int type = 0;
                switch (in.type) {
                        case ScriptInput::Type::KeyDown: type = 0; break;
                        case ScriptInput::Type::KeyUp: type = 1; break;
                        case ScriptInput::Type::PointerMove: type = 2; break;
                        case ScriptInput::Type::PointerDown: type = 3; break;
                        case ScriptInput::Type::PointerUp: type = 4; break;
                        case ScriptInput::Type::Wheel: type = 5; break;
                }
                char key[24];
                angel::keyName(in.key, key, sizeof key);
                char     text[8] = {};
                uint32_t c = in.character;
                if (c >= 0x20 && c != 0x7F) {
                        // As UTF-8.
                        if (c < 0x80) {
                                text[0] = char(c);
                        } else if (c < 0x800) {
                                text[0] = char(0xC0 | (c >> 6));
                                text[1] = char(0x80 | (c & 0x3F));
                        } else if (c < 0x10000) {
                                text[0] = char(0xE0 | (c >> 12));
                                text[1] = char(0x80 | ((c >> 6) & 0x3F));
                                text[2] = char(0x80 | (c & 0x3F));
                        } else {
                                text[0] = char(0xF0 | (c >> 18));
                                text[1] = char(0x80 | ((c >> 12) & 0x3F));
                                text[2] = char(0x80 | ((c >> 6) & 0x3F));
                                text[3] = char(0x80 | (c & 0x3F));
                        }
                }
                return angel::makeEvent(type, key, text, in.x, in.y, in.button, in.wheel, in.repeat);
        }

        void Stage::releaseEvent(void *event) {
                angel::releaseEvent(event);
                return;
        }

        int Stage::play(asIScriptModule *module) {
                const char *name = strrchr(_path, '/') != nullptr ? strrchr(_path, '/') + 1 : _path;
                ScriptPlace place;
                place.theme = &Theme::paper();
                place.width = Width;
                place.height = Height;
                place.file = StringView(_path);
                place.firstLine = 1;
                // Its random numbers from its file's name, as a deck's from the deck's.
                uint32_t seed = 2166136261u;
                for (const char *p = name; *p != '\0'; p++) seed = (seed ^ uint8_t(*p)) * 16777619u;
                void *p = malloc(sizeof(AngelRegion));
                if (p == nullptr) {
                        fprintf(stderr, "angel: out of memory for the stage\n");
                        return 1;
                }
                _region = new (p) AngelRegion(*this, place, seed);
                _region->adopt(module);
                if (_region->hasFailed()) return 1;
                char title[600];
                snprintf(title, sizeof title, "%s - angel", name);
                _platform.setWindowed(true);
                if (_platform.openScreen(Width, Height, title, false) != Error::Ok) {
                        // No screen (the host's tests, a shell with no desktop): played to its end.
                        playHeadless();
                        return _region->hasFailed() ? 1 : 0;
                }
                const uint32_t bytes = uint32_t(Width) * uint32_t(Height) * 2;
                _pixels = _platform.allocatePixels(bytes);
                if (_pixels == nullptr) {
                        fprintf(stderr, "angel: out of memory for the stage's picture\n");
                        _platform.closeScreen();
                        return 1;
                }
                _canvas.attach(_pixels, uint32_t(Width) * 2, Width, Height, _platform.pixelFormat());
                _region->seek(0, false);
                _region->setAudible(true);
                if (_region->isInteractive()) _region->setEntered(true);
                Rect whole{0, 0, Width, Height};
                redraw(&whole, 1);
                _last = _platform.now();
                settle();
                (void)_platform.run(*this);
                _platform.closeScreen();
                return _region->hasFailed() ? 1 : 0;
        }

        void Stage::playHeadless() {
                // Each beat played to its end at once, as if Next were pressed at every pause; a
                // script that never ends is let go after as long as a deck's fast-forward allows.
                _region->seek(0, false);
                for (uint32_t i = 0; i < AngelRegion::EndlessYields && !_region->hasFailed(); i++) {
                        Rect areas[ScriptRegion::MaxAreas];
                        if (_region->isMoving()) {
                                (void)_region->advance(100000000, areas);
                        } else if (_region->hasFinished()) {
                                break;
                        } else {
                                _region->next();
                        }
                }
                return;
        }

        void Stage::redraw(const Rect *areas, uint32_t count) {
                for (uint32_t i = 0; i < count; i++) {
                        _canvas.setClip(areas[i]);
                        _canvas.fill(areas[i], Theme::paper().background);
                        _region->draw(_canvas, 0, 0);
                }
                _canvas.clearClip();
                if (count > 0) _platform.presentAreas(_canvas, areas, count);
                return;
        }

        void Stage::settle() {
                // Frames while it moves; the keys a script's while it takes them; the title its beat.
                _platform.animate(_region->isMoving());
                _platform.setInputMode(_region->isInteractive() ? InputMode::Script : InputMode::Deck);
                int32_t beat = int32_t(_region->beat());
                if (beat != _titleBeat && _region->beats() > 0) {
                        _titleBeat = beat;
                        const char *name = strrchr(_path, '/') != nullptr ? strrchr(_path, '/') + 1 : _path;
                        char        title[600];
                        snprintf(title, sizeof title, "%s - beat %d of %u - angel", name, beat + 1,
                                 unsigned(_region->beats() + 1));
                        _platform.setTitle(title);
                }
                return;
        }

        void Stage::next() {
                // The rest of this beat at once, then the next played.
                _region->next();
                Rect whole{0, 0, Width, Height};
                redraw(&whole, 1);
                _last = _platform.now();
                return;
        }

        void Stage::back() {
                // Previous: the beat before, as it ends (the script run again in fast-forward).
                uint32_t beat = _region->beat();
                if (beat == 0) return;
                _region->seek(beat - 1, true);
                Rect whole{0, 0, Width, Height};
                redraw(&whole, 1);
                return;
        }

        void Stage::input(const ScriptInput &in) {
                Rect     areas[ScriptRegion::MaxAreas];
                uint32_t n = _region->input(in, areas);
                redraw(areas, n);
                return;
        }

        void Stage::event(const Event &event) {
                Rect areas[ScriptRegion::MaxAreas];
                switch (event.type) {
                        case Event::Type::Close: _platform.quit(); break;
                        case Event::Type::Action:
                                switch (event.action) {
                                        case Action::Next: next(); break;
                                        case Action::Previous: back(); break;
                                        case Action::First:
                                                _region->seek(0, false);
                                                areas[0] = Rect{0, 0, Width, Height};
                                                redraw(areas, 1);
                                                break;
                                        case Action::Quit:
                                        case Action::Leave: _platform.quit(); break;
                                        default: break;
                                }
                                break;
                        case Event::Type::Key:
                        case Event::Type::KeyUp: {
                                if (!_region->isInteractive()) break;
                                ScriptInput in;
                                in.type = event.type == Event::Type::KeyUp ? ScriptInput::Type::KeyUp
                                                                           : ScriptInput::Type::KeyDown;
                                in.key = event.key;
                                in.character = event.character;
                                in.repeat = event.repeat;
                                input(in);
                                break;
                        }
                        case Event::Type::Pointer:
                        case Event::Type::Wheel: {
                                uint32_t pressed = event.buttons & ~_buttons, released = _buttons & ~event.buttons;
                                _buttons = event.buttons;
                                if (!_region->isInteractive()) {
                                        // A click is Next, as a clicker's.
                                        if ((pressed & 1u) != 0) next();
                                        break;
                                }
                                ScriptInput in;
                                in.x = event.x;
                                in.y = event.y;
                                if (event.type == Event::Type::Wheel) {
                                        in.type = ScriptInput::Type::Wheel;
                                        in.wheel = event.wheel;
                                        input(in);
                                        break;
                                }
                                if (pressed == 0 && released == 0) {
                                        in.type = ScriptInput::Type::PointerMove;
                                        input(in);
                                        break;
                                }
                                for (int32_t b = 0; b < 3; b++) {
                                        if (((pressed | released) & (1u << b)) == 0) continue;
                                        in.type = (pressed & (1u << b)) != 0 ? ScriptInput::Type::PointerDown
                                                                             : ScriptInput::Type::PointerUp;
                                        in.button = b + 1;
                                        input(in);
                                }
                                break;
                        }
                        default: break;
                }
                settle();
                return;
        }

        void Stage::frame(int64_t ns) {
                int64_t dt = ns - _last;
                _last = ns;
                // A frame late (the window moved, a busy system) doesn't jump the scene ahead.
                if (dt > 100000000) dt = 100000000;
                if (dt < 0) dt = 0;
                Rect     areas[ScriptRegion::MaxAreas];
                uint32_t n = _region->advance(dt, areas);
                redraw(areas, n);
                settle();
                return;
        }

} // namespace preso
