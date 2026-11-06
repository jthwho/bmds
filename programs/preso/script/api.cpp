/**
 * @file      api.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The scene (devplan/preso.md §7.4, §8c): `Scene` and the global `scene` (its size, its theme,
 * add, play, wait, pause, start, and draw()'s drawing), its objects (`SceneObject`, `SceneRect`
 * and the rest) with their animations as their methods, and `readText()`; the same in a deck
 * and in angel, whose `Font`, `Event` and `Callback` it shares. The deck profile (§7.7) is
 * the language (libangel's strings, maths, print, colours), a deck's own `Font`, `Event` and
 * `Sound`, and the scene: nothing reaches outside the deck. Userspace ABI once released:
 * append only (tests/preso/api.lock, from `preso --api`; angel's tests/scripts/api.lock).
 */

#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include "../../../thirdparty/angelscript/add_on/scriptarray/scriptarray.h"
#include "angel.h"
#include "runtime.h"

namespace preso {

        namespace {

                using angel::behaviour;
                using angel::def;
                using angel::method;
                using angel::property;
                using angel::raise;
                using angel::String;

                constexpr uint32_t MaxRead = 1u << 20;
                constexpr uint32_t MaxPoints = 1024; ///< A polygon's, drawn or made.

                // Points for a polygon or an ellipse, allocated when first wanted (not in the
                // program's .bss: angel has this code whether its script draws a scene or not);
                // nullptr (raised) if there's no memory.
                Point *pointSpace = nullptr;
                Point *points() {
                        if (pointSpace == nullptr) pointSpace = static_cast<Point *>(malloc(MaxPoints * sizeof(Point)));
                        if (pointSpace == nullptr) angel::raise("Out of memory for points");
                        return pointSpace;
                }

                // ---- Helpers

                AngelRegion *region() {
                        return runningRegion();
                }

                // The host of the script calling (nullptr: an engine that only lists the API).
                SceneHost *host() {
                        asIScriptContext *ctx = asGetActiveContext();
                        return ctx != nullptr ? SceneHost::of(ctx->GetEngine()) : nullptr;
                }

                // The region a scene's own call is for: the running one, or one its host makes for it
                // (angel's stage); nullptr: the call does nothing.
                AngelRegion *sceneRegion() {
                        if (region() != nullptr) return region();
                        SceneHost *h = host();
                        return h != nullptr ? h->needRegion() : nullptr;
                }

                const Theme &theme() {
                        AngelRegion *r = region();
                        if (r != nullptr && r->place().theme != nullptr) return *r->place().theme;
                        SceneHost *h = host();
                        return h != nullptr ? h->theme() : Theme::paper();
                }

                // A theme's font, in the scene's units: at the theme's size in a region shrunk to fit.
                FontKey sceneFont(const FontKey &key) {
                        AngelRegion *r = region();
                        return r != nullptr ? preso::sceneFont(r->context(), key) : key;
                }

                // A file a script names: the path to open, and the image cache's name for it.
                bool fileOf(StringView path, char *out, size_t size, char *cached, size_t cachedSize,
                            const char **why) {
                        if (region() != nullptr) return region()->resolve(path, out, size, cached, cachedSize, why);
                        SceneHost *h = host();
                        if (h == nullptr) {
                                *why = "can't be read here";
                                return false;
                        }
                        if (cached != nullptr) snprintf(cached, cachedSize, "%.*s", int(path.length()), path.data());
                        return h->resolve(path, out, size, why);
                }

                Color color(StringView name) {
                        Color c = 0;
                        (void)theme().scriptColor(name, &c);
                        return c;
                }

                const String &argString(asIScriptGeneric *gen, asUINT n) {
                        return *static_cast<const String *>(gen->GetArgAddress(n));
                }

                StringView view(const String &s) {
                        return StringView(s.data(), s.length());
                }

                Shape *self(asIScriptGeneric *gen) {
                        return static_cast<Shape *>(gen->GetObject());
                }

                // A handle argument: AngelScript gives the function its reference (the generic
                // calling convention as the native one), released when this goes.
                template <typename T> class Arg {
                        public:
                                Arg(asIScriptGeneric *gen, asUINT n) {
                                        // Copied out: on a 64-bit host the script's stack holds pointers on
                                        // 4-byte boundaries.
                                        memcpy(&_p, gen->GetAddressOfArg(n), sizeof _p);
                                }
                                ~Arg() {
                                        if (_p != nullptr) _p->release();
                                }
                                Arg(const Arg &) = delete;
                                Arg     &operator=(const Arg &) = delete;
                                T       *get() const { return _p; }
                                T       *operator->() const { return _p; }
                                explicit operator bool() const { return _p != nullptr; }

                        private:
                                T *_p = nullptr;
                };

                Shape *made(Shape *s) {
                        if (s == nullptr) raise("Out of memory for an object");
                        return s;
                }

                Shape *newShape(ShapeKind kind, Color c) {
                        Shape *s = Shape::make(kind);
                        if (s == nullptr) return nullptr;
                        s->color = c;
                        s->fill = color("Script.Accent");
                        return s;
                }

                void returnShape(asIScriptGeneric *gen, Shape *s) {
                        gen->SetReturnAddress(made(s));
                        return;
                }

                // ---- Fonts

                ScriptFont *fontSelf(asIScriptGeneric *gen) {
                        return static_cast<ScriptFont *>(gen->GetObject());
                }

                void fontAddRef(asIScriptGeneric *gen) {
                        fontSelf(gen)->addRef();
                }
                void fontRelease(asIScriptGeneric *gen) {
                        fontSelf(gen)->release();
                }
                void fontByName(asIScriptGeneric *gen) {
                        // As themes write fonts: what isn't said is the theme's text's.
                        FontKey key = sceneFont(theme().body.font);
                        Arena   arena;
                        if (!parseFont(view(argString(gen, 0)), arena, &key)) {
                                raise("`%s` isn't a font (a family, a size, a style: \"IBM Plex Sans 40 bold\")",
                                      argString(gen, 0).cStr());
                                return;
                        }
                        ScriptFont *f = ScriptFont::make(key);
                        if (f == nullptr) raise("Out of memory for a font");
                        gen->SetReturnAddress(f);
                }
                void fontSized(asIScriptGeneric *gen) {
                        FontKey key = fontSelf(gen)->key();
                        int     size = int(gen->GetArgDWord(0));
                        key.size = uint16_t(size < 6 ? 6 : size > 400 ? 400 : size);
                        ScriptFont *f = ScriptFont::make(key);
                        if (f == nullptr) raise("Out of memory for a font");
                        gen->SetReturnAddress(f);
                }
                void fontSize(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(fontSelf(gen)->key().size);
                }

                // A Font@ argument (the host's object) as the scene's font, a reference of its own;
                // nullptr for null. The argument's reference is let go.
                ScriptFont *fontArg(asIScriptGeneric *gen, asUINT n) {
                        void *font = nullptr;
                        memcpy(&font, gen->GetAddressOfArg(n), sizeof font);
                        if (font == nullptr) return nullptr;
                        SceneHost  *h = SceneHost::of(gen->GetEngine());
                        ScriptFont *f = h != nullptr ? h->scriptFont(font) : nullptr;
                        if (h != nullptr) h->releaseFont(font);
                        return f;
                }

                // ---- The theme and the scene

                struct Global {
                                int unused;
                };
                Global themeObject;
                Global sceneGlobal;

                void themeColor(asIScriptGeneric *gen) {
                        Color c = 0;
                        if (!theme().scriptColor(view(argString(gen, 0)), &c)) {
                                raise("The theme has no colour `%s` (Text, Accent, H1, Script.Highlight...)",
                                      argString(gen, 0).cStr());
                        }
                        gen->SetReturnDWord(c);
                }
                void themeFont(asIScriptGeneric *gen) {
                        StringView   name = view(argString(gen, 0));
                        const Theme &t = theme();
                        FontKey      key;
                        if (name.equals("Text") || name.equals("Body")) {
                                key = t.body.font;
                        } else if (name.equals("H1")) {
                                key = t.heading[0].text.font;
                        } else if (name.equals("H2")) {
                                key = t.heading[1].text.font;
                        } else if (name.equals("H3") || name.equals("H4") || name.equals("H5") || name.equals("H6")) {
                                key = t.heading[2].text.font;
                        } else if (name.equals("Code")) {
                                key = t.code.font;
                        } else if (name.equals("Small") || name.equals("Header")) {
                                key = t.header.font;
                        } else {
                                raise("The theme has no font `%s` (Text, H1, H2, H3, Code, Small)",
                                      argString(gen, 0).cStr());
                                return;
                        }
                        key = sceneFont(key);
                        SceneHost *h = SceneHost::of(gen->GetEngine());
                        void      *f = h != nullptr ? h->makeFont(key) : nullptr;
                        if (f == nullptr) raise("Out of memory for a font");
                        gen->SetReturnAddress(f);
                }
                void sceneSize(int32_t *w, int32_t *h) {
                        AngelRegion *r = region();
                        SceneHost   *sh = host();
                        *w = *h = 0;
                        if (r != nullptr) {
                                *w = r->sceneWidth();
                                *h = r->sceneHeight();
                        } else if (sh != nullptr) {
                                sh->size(w, h);
                        }
                        return;
                }
                void sceneWidth(asIScriptGeneric *gen) {
                        int32_t w, h;
                        sceneSize(&w, &h);
                        gen->SetReturnDWord(asDWORD(w));
                }
                void sceneHeight(asIScriptGeneric *gen) {
                        int32_t w, h;
                        sceneSize(&w, &h);
                        gen->SetReturnDWord(asDWORD(h));
                }
                void sceneTheme(asIScriptGeneric *gen) {
                        gen->SetReturnAddress(&themeObject);
                }

                // ---- Shapes: every kind's

                void shapeAddRef(asIScriptGeneric *gen) {
                        self(gen)->addRef();
                }
                void shapeRelease(asIScriptGeneric *gen) {
                        self(gen)->release();
                }
                template <float Shape::*M> void getFloat(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(self(gen)->*M);
                }
                template <float Shape::*M> void setFloat(asIScriptGeneric *gen) {
                        self(gen)->*M = gen->GetArgFloat(0);
                }
                template <Color Shape::*M> void getColor(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->*M);
                }
                void setColor(asIScriptGeneric *gen) {
                        self(gen)->color = gen->GetArgDWord(0) & 0xFFFFFF;
                }
                void setFill(asIScriptGeneric *gen) {
                        // A fill given is a fill seen.
                        Shape *s = self(gen);
                        s->fill = gen->GetArgDWord(0) & 0xFFFFFF;
                        if (s->fillOpacity <= 0) s->fillOpacity = 1;
                }
                void getVisible(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->visible ? 1 : 0);
                }
                void setVisible(asIScriptGeneric *gen) {
                        self(gen)->visible = gen->GetArgByte(0) != 0;
                }
                void shapeAt(asIScriptGeneric *gen) {
                        Shape *s = self(gen);
                        s->x = gen->GetArgFloat(0);
                        s->y = gen->GetArgFloat(1);
                        s->addRef();
                        gen->SetReturnAddress(s);
                }
                void implicitCast(asIScriptGeneric *gen) {
                        Shape *s = self(gen);
                        s->addRef();
                        gen->SetReturnAddress(s);
                }
                // Shape to a kind: itself if it is one, else null.
                void downCast(asIScriptGeneric *gen) {
                        Shape    *s = self(gen);
                        ShapeKind k = ShapeKind(reinterpret_cast<uintptr_t>(gen->GetAuxiliary()));
                        if (s->kind != k) {
                                gen->SetReturnAddress(nullptr);
                                return;
                        }
                        s->addRef();
                        gen->SetReturnAddress(s);
                }

                // ---- Making each kind

                void makeText(asIScriptGeneric *gen) {
                        angel::Text text;
                        angel::formatValue(gen->GetEngine(), text, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        ScriptFont *f = fontArg(gen, 1);
                        Shape      *s = newShape(ShapeKind::Text, theme().body.color);
                        if (s != nullptr) {
                                s->font = f;
                                f = nullptr;
                                if (!s->setText(text.data(), uint32_t(text.length()))) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        if (f != nullptr) f->release();
                        returnShape(gen, s);
                }
                void textGet(asIScriptGeneric *gen) {
                        Shape *s = self(gen);
                        new (gen->GetAddressOfReturnLocation())
                                String(s->text != nullptr ? s->text : "", s->textLength);
                }
                void textSet(asIScriptGeneric *gen) {
                        const String &t = argString(gen, 0);
                        if (!self(gen)->setText(t.data(), t.length())) raise("Out of memory for text");
                }
                void textWidth(asIScriptGeneric *gen) {
                        float w = 0, h = 0;
                        if (region() != nullptr) self(gen)->measureText(region()->context(), &w, &h);
                        gen->SetReturnFloat(w);
                }
                void textHeight(asIScriptGeneric *gen) {
                        float w = 0, h = 0;
                        if (region() != nullptr) self(gen)->measureText(region()->context(), &w, &h);
                        gen->SetReturnFloat(h);
                }
                void makeRect(asIScriptGeneric *gen) {
                        Shape *s = newShape(ShapeKind::Rect, color("Script.Accent"));
                        if (s != nullptr) {
                                s->width = gen->GetArgFloat(0);
                                s->height = gen->GetArgFloat(1);
                        }
                        returnShape(gen, s);
                }
                void makeCircle(asIScriptGeneric *gen) {
                        Shape *s = newShape(ShapeKind::Circle, color("Script.Accent"));
                        if (s != nullptr) s->radius = gen->GetArgFloat(0);
                        returnShape(gen, s);
                }
                void makeEmptyLine(asIScriptGeneric *gen) {
                        bool   arrow = gen->GetAuxiliary() != nullptr;
                        Shape *s = newShape(arrow ? ShapeKind::Arrow : ShapeKind::Line, color("Script.Line"));
                        if (s != nullptr && arrow) s->head = ArrowHead::Triangle;
                        returnShape(gen, s);
                }
                void makeLine(asIScriptGeneric *gen) {
                        // Placed at its middle; its ends from there.
                        bool   arrow = gen->GetAuxiliary() != nullptr;
                        Shape *s = newShape(arrow ? ShapeKind::Arrow : ShapeKind::Line, color("Script.Line"));
                        if (s != nullptr) {
                                float x0 = gen->GetArgFloat(0), y0 = gen->GetArgFloat(1), x1 = gen->GetArgFloat(2),
                                      y1 = gen->GetArgFloat(3);
                                s->x = (x0 + x1) / 2;
                                s->y = (y0 + y1) / 2;
                                s->ends[0] = Point{x0 - s->x, y0 - s->y};
                                s->ends[1] = Point{x1 - s->x, y1 - s->y};
                                if (arrow) s->head = ArrowHead::Triangle;
                        }
                        returnShape(gen, s);
                }
                // An array of x, y numbers (float or int) as points; false (raised) if it isn't one.
                bool pointsOf(const CScriptArray *array, Point *out, uint32_t max, uint32_t *count) {
                        uint32_t n = array != nullptr ? array->GetSize() : 0;
                        if (n % 2 != 0) {
                                raise("Points are x, y pairs: an even count of numbers, not %u", unsigned(n));
                                return false;
                        }
                        if (n / 2 > max) {
                                raise("%u points at most, not %u", unsigned(max), unsigned(n / 2));
                                return false;
                        }
                        bool ints = array->GetElementTypeId() == asTYPEID_INT32;
                        for (uint32_t i = 0; i < n; i += 2) {
                                out[i / 2] = ints ? Point{float(*static_cast<const int32_t *>(array->At(i))),
                                                          float(*static_cast<const int32_t *>(array->At(i + 1)))}
                                                  : Point{*static_cast<const float *>(array->At(i)),
                                                          *static_cast<const float *>(array->At(i + 1))};
                        }
                        *count = n / 2;
                        return true;
                }
                void makePolygon(asIScriptGeneric *gen) {
                        Point   *pts = points();
                        uint32_t n = 0;
                        if (pts == nullptr ||
                            !pointsOf(static_cast<const CScriptArray *>(gen->GetArgAddress(0)), pts, 256, &n))
                                return;
                        if (n < 3) {
                                raise("A polygon has 3 points at least");
                                return;
                        }
                        // Placed at the middle of its box; its points from there.
                        float x0 = pts[0].x, x1 = x0, y0 = pts[0].y, y1 = y0;
                        for (uint32_t i = 1; i < n; i++) {
                                x0 = pts[i].x < x0 ? pts[i].x : x0;
                                x1 = pts[i].x > x1 ? pts[i].x : x1;
                                y0 = pts[i].y < y0 ? pts[i].y : y0;
                                y1 = pts[i].y > y1 ? pts[i].y : y1;
                        }
                        Shape *s = newShape(ShapeKind::Polygon, color("Script.Accent"));
                        if (s != nullptr) {
                                s->x = (x0 + x1) / 2;
                                s->y = (y0 + y1) / 2;
                                for (uint32_t i = 0; i < n; i++) pts[i] = Point{pts[i].x - s->x, pts[i].y - s->y};
                                if (!s->setPoints(pts, n)) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        returnShape(gen, s);
                }
                void makeImage(asIScriptGeneric *gen) {
                        SceneHost  *sh = host();
                        StringView  given = view(argString(gen, 0));
                        char        resolved[512], cached[512];
                        const char *why = nullptr;
                        if (!fileOf(given, resolved, sizeof resolved, cached, sizeof cached, &why)) {
                                raise("Image `%s` %s", argString(gen, 0).cStr(),
                                      why != nullptr ? why : "can't be read");
                                return;
                        }
                        StringView path(cached);
                        float w = 0, h = 0;
                        if (gen->GetArgCount() >= 3) {
                                w = gen->GetArgFloat(1);
                                h = gen->GetArgFloat(2);
                        } else {
                                int32_t iw = 0, ih = 0;
                                ImageCache *images = sh != nullptr ? sh->images() : nullptr;
                                Error       e = images != nullptr ? images->size(path, &iw, &ih) : Error::NotSupported;
                                if (e == Error::NotFound) {
                                        raise("Image `%s` isn't there", argString(gen, 0).cStr());
                                        return;
                                }
                                if (e == Error::Ok) {
                                        w = float(iw);
                                        h = float(ih);
                                } else {
                                        // There, but not read here (no decoder): a box of a size.
                                        w = 320;
                                        h = 180;
                                }
                        }
                        Shape *s = newShape(ShapeKind::Image, 0);
                        if (s != nullptr) {
                                s->width = w;
                                s->height = h;
                                s->lineWidth = 0;
                                if (!s->setPath(path.data(), path.length())) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        returnShape(gen, s);
                }
                void makeCode(asIScriptGeneric *gen) {
                        const String &text = argString(gen, 0);
                        const String &language = argString(gen, 1);
                        Shape        *s = newShape(ShapeKind::Code, theme().code.color);
                        if (s != nullptr) {
                                s->fill = theme().codeBackground;
                                s->fillOpacity = 1;
                                s->lineWidth = 0;
                                SceneHost *h = host();
                                if (language.length() > 0 && h != nullptr)
                                        s->syntax = h->platform().findSyntax(view(language));
                                if (!s->setText(text.data(), text.length())) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        returnShape(gen, s);
                }
                void makeGroup(asIScriptGeneric *gen) {
                        Shape *s = newShape(ShapeKind::Group, color("Script.Line"));
                        returnShape(gen, s);
                }
                void groupAdd(asIScriptGeneric *gen) {
                        Arg<Shape> child(gen, 0);
                        if (!child) {
                                raise("A group can't hold null");
                                return;
                        }
                        if (!self(gen)->addChild(child.get()))
                                raise("A group can't hold itself (or there wasn't the memory)");
                }
                void groupRemove(asIScriptGeneric *gen) {
                        Arg<Shape> child(gen, 0);
                        if (child) self(gen)->removeChild(child.get());
                }
                void groupCount(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->childCount);
                }
                void makeBars(asIScriptGeneric *gen) {
                        const CScriptArray *values = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        uint32_t            n = values != nullptr ? values->GetSize() : 0;
                        if (n > 1024) {
                                raise("Bars shows 1024 values at most");
                                return;
                        }
                        float *v = static_cast<float *>(malloc((n > 0 ? n : 1) * sizeof(float)));
                        if (v == nullptr) {
                                raise("Out of memory for bars");
                                return;
                        }
                        bool ints = values != nullptr && values->GetElementTypeId() == asTYPEID_INT32;
                        for (uint32_t i = 0; i < n; i++) {
                                v[i] = ints ? float(*static_cast<const int32_t *>(values->At(i)))
                                            : *static_cast<const float *>(values->At(i));
                        }
                        Shape *s = newShape(ShapeKind::Bars, color("Script.Line"));
                        if (s != nullptr) {
                                s->width = gen->GetArgFloat(1);
                                s->height = gen->GetArgFloat(2);
                                s->lineWidth = 0;
                                if (!s->setBars(v, n, color("Script.Accent"))) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        free(v);
                        returnShape(gen, s);
                }
                bool barIndex(Shape *s, int i) {
                        if (i >= 0 && uint32_t(i) < s->barCount) return true;
                        raise("No bar %d (there are %u)", i, unsigned(s->barCount));
                        return false;
                }
                void barsValue(asIScriptGeneric *gen) {
                        Shape *s = self(gen);
                        int    i = int(gen->GetArgDWord(0));
                        gen->SetReturnFloat(barIndex(s, i) ? s->bars[i].value : 0);
                }
                void barsCount(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->barCount);
                }
                void makeGrid(asIScriptGeneric *gen) {
                        Shape *s = newShape(ShapeKind::Grid, color("Script.Muted"));
                        if (s != nullptr) {
                                s->width = gen->GetArgFloat(0);
                                s->height = gen->GetArgFloat(1);
                                s->step = gen->GetArgFloat(2);
                                s->lineWidth = 1;
                                if (s->step < 2) s->step = 2;
                        }
                        returnShape(gen, s);
                }

                // ---- Animations

                Animation *animSelf(asIScriptGeneric *gen) {
                        return static_cast<Animation *>(gen->GetObject());
                }
                void animAddRef(asIScriptGeneric *gen) {
                        animSelf(gen)->addRef();
                }
                void animRelease(asIScriptGeneric *gen) {
                        animSelf(gen)->release();
                }
                void returnSelf(asIScriptGeneric *gen) {
                        Animation *a = animSelf(gen);
                        a->addRef();
                        gen->SetReturnAddress(a);
                }
                void animEase(asIScriptGeneric *gen) {
                        uint32_t e = gen->GetArgDWord(0);
                        animSelf(gen)->ease = e <= uint32_t(Ease::Wiggle) ? Ease(e) : Ease::InOut;
                        returnSelf(gen);
                }
                void animDelay(asIScriptGeneric *gen) {
                        float d = gen->GetArgFloat(0);
                        animSelf(gen)->delay = d > 0 ? d : 0;
                        returnSelf(gen);
                }
                void animDuration(asIScriptGeneric *gen) {
                        float d = gen->GetArgFloat(0);
                        animSelf(gen)->duration = d > 0 ? d : 0;
                        returnSelf(gen);
                }

                // An animation of the object it is called on.
                Animation *animationOf(asIScriptGeneric *gen) {
                        Animation *a = Animation::make(self(gen));
                        if (a == nullptr) raise("Out of memory for an animation");
                        return a;
                }
                void returnAnimation(asIScriptGeneric *gen, Animation *a) {
                        gen->SetReturnAddress(a);
                        return;
                }
                void animMove(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::X, gen->GetArgFloat(0));
                                a->change(Property::Y, gen->GetArgFloat(1));
                        }
                        returnAnimation(gen, a);
                }
                void animShift(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::X, gen->GetArgFloat(0), true);
                                a->change(Property::Y, gen->GetArgFloat(1), true);
                        }
                        returnAnimation(gen, a);
                }
                void animFadeIn(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Opacity, 0, true); // to its own
                                a->from(0);
                                a->adds = true;
                        }
                        returnAnimation(gen, a);
                }
                void animFadeOut(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Opacity, 0);
                                a->then = Animation::Then::Remove;
                        }
                        returnAnimation(gen, a);
                }
                void animGrow(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Scale, 0, true);
                                a->from(0);
                                a->adds = true;
                        }
                        returnAnimation(gen, a);
                }
                void animShrink(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Scale, 0);
                                a->then = Animation::Then::Remove;
                        }
                        returnAnimation(gen, a);
                }
                void animWrite(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Reveal, 1);
                                a->from(0);
                                a->adds = true;
                                a->ease = Ease::Linear;
                        }
                        returnAnimation(gen, a);
                }
                void animTransform(asIScriptGeneric *gen) {
                        Arg<Shape> to(gen, 0);
                        if (!to) {
                                raise("transform() into null");
                                return;
                        }
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Morph, 1);
                                a->from(0);
                                to->addRef();
                                a->other = to.get();
                                a->then = Animation::Then::Become;
                                a->adds = true;
                        }
                        returnAnimation(gen, a);
                }
                void animColorTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->changeColor(Property::StrokeColor, gen->GetArgDWord(0) & 0xFFFFFF);
                        returnAnimation(gen, a);
                }
                void animFillTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->changeColor(Property::FillColor, gen->GetArgDWord(0) & 0xFFFFFF);
                                if (a->target->fillOpacity <= 0) a->change(Property::FillOpacity, 1);
                        }
                        returnAnimation(gen, a);
                }
                void animScaleTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(Property::Scale, gen->GetArgFloat(0));
                        returnAnimation(gen, a);
                }
                void animRotateTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(Property::Rotation, gen->GetArgFloat(0));
                        returnAnimation(gen, a);
                }
                void animTween(asIScriptGeneric *gen) {
                        Property p;
                        if (!propertyNamed(view(argString(gen, 0)), &p)) {
                                raise("tween(): no property `%s` (x, y, scale, rotation, opacity, lineWidth, "
                                      "fillOpacity, width, height, radius, reveal)",
                                      argString(gen, 0).cStr());
                                return;
                        }
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(p, gen->GetArgFloat(1));
                        returnAnimation(gen, a);
                }
                // Bars' animations: of the bars object, a bar's.
                Animation *barAnimation(asIScriptGeneric *gen, int i) {
                        Shape *s = self(gen);
                        if (!barIndex(s, i)) return nullptr;
                        Animation *a = Animation::make(s);
                        if (a == nullptr) raise("Out of memory for an animation");
                        return a;
                }
                void barsSwap(asIScriptGeneric *gen) {
                        int        i = int(gen->GetArgDWord(0)), j = int(gen->GetArgDWord(1));
                        Shape     *s = self(gen);
                        Animation *a = barIndex(s, j) ? barAnimation(gen, i) : nullptr;
                        if (a != nullptr) {
                                a->change(Property::BarOffset, float(j - i), false, uint16_t(i));
                                a->change(Property::BarOffset, float(i - j), false, uint16_t(j));
                                a->then = Animation::Then::SwapBars;
                                a->index = uint16_t(i);
                                a->index2 = uint16_t(j);
                        }
                        returnAnimation(gen, a);
                }
                void barsHighlight(asIScriptGeneric *gen) {
                        int   i = int(gen->GetArgDWord(0));
                        Color c = gen->GetArgCount() > 1 ? gen->GetArgDWord(1) & 0xFFFFFF : color("Script.Highlight");
                        Animation *a = barAnimation(gen, i);
                        if (a != nullptr) a->changeColor(Property::BarColor, c, uint16_t(i));
                        returnAnimation(gen, a);
                }
                void barsUnhighlight(asIScriptGeneric *gen) {
                        int        i = int(gen->GetArgDWord(0));
                        Animation *a = barAnimation(gen, i);
                        if (a != nullptr) a->changeColor(Property::BarColor, color("Script.Accent"), uint16_t(i));
                        returnAnimation(gen, a);
                }
                void barsSet(asIScriptGeneric *gen) {
                        int        i = int(gen->GetArgDWord(0));
                        Animation *a = barAnimation(gen, i);
                        if (a != nullptr) a->change(Property::BarValue, gen->GetArgFloat(1), false, uint16_t(i));
                        returnAnimation(gen, a);
                }

                // ---- Time and the scene

                void play(asIScriptGeneric *gen) {
                        AngelRegion *r = sceneRegion();
                        uint32_t     n = gen->GetArgCount() - 1;
                        Animation   *list[6];
                        bool         null = false;
                        for (uint32_t i = 0; i < n; i++) {
                                memcpy(&list[i], gen->GetAddressOfArg(i), sizeof list[i]);
                                null |= list[i] == nullptr;
                        }
                        if (null) {
                                raise("scene.play() got null, not an animation");
                        } else if (r != nullptr) {
                                r->play(list, n, gen->GetArgFloat(n));
                        }
                        // The arguments' references, the function's to let go.
                        for (uint32_t i = 0; i < n; i++) {
                                if (list[i] != nullptr) list[i]->release();
                        }
                }
                void playArray(asIScriptGeneric *gen) {
                        AngelRegion        *r = sceneRegion();
                        const CScriptArray *array = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        uint32_t            n = array != nullptr ? array->GetSize() : 0;
                        Animation          *list[AngelRegion::MaxPlaying];
                        if (n > AngelRegion::MaxPlaying) {
                                raise("scene.play() takes %u animations at most", unsigned(AngelRegion::MaxPlaying));
                                return;
                        }
                        for (uint32_t i = 0; i < n; i++) {
                                list[i] = *static_cast<Animation *const *>(array->At(i));
                                if (list[i] == nullptr) {
                                        raise("scene.play() got null, not an animation");
                                        return;
                                }
                        }
                        if (r != nullptr) r->play(list, n, gen->GetArgFloat(1));
                }
                void wait(asIScriptGeneric *gen) {
                        AngelRegion *r = sceneRegion();
                        if (r != nullptr) r->wait(gen->GetArgFloat(0));
                }
                void pause(asIScriptGeneric *) {
                        AngelRegion *r = sceneRegion();
                        if (r != nullptr) r->pause();
                }
                void sceneAdd(asIScriptGeneric *gen) {
                        Arg<Shape>   s(gen, 0);
                        AngelRegion *r = s ? sceneRegion() : nullptr;
                        if (r != nullptr) r->add(s.get());
                }
                void sceneRemove(asIScriptGeneric *gen) {
                        Arg<Shape>   s(gen, 0);
                        AngelRegion *r = s ? sceneRegion() : nullptr;
                        if (r != nullptr) r->remove(s.get());
                }
                void readText(asIScriptGeneric *gen) {
                        SceneHost  *h = host();
                        char        path[512];
                        const char *why = nullptr;
                        if (h == nullptr || !fileOf(view(argString(gen, 0)), path, sizeof path, nullptr, 0, &why)) {
                                raise("`%s` %s", argString(gen, 0).cStr(), why != nullptr ? why : "can't be read");
                                new (gen->GetAddressOfReturnLocation()) String();
                                return;
                        }
                        char    *data = nullptr;
                        uint32_t length = 0;
                        Error    e = h->platform().readFile(path, MaxRead, &data, &length);
                        if (e != Error::Ok) {
                                raise("`%s` %s", argString(gen, 0).cStr(),
                                      e == Error::NotFound ? "isn't there" : errorName(e));
                                new (gen->GetAddressOfReturnLocation()) String();
                                return;
                        }
                        String *s = new (gen->GetAddressOfReturnLocation()) String();
                        s->adopt(data, length);
                }

                // ---- Drawing on the scene: in draw(), on the region being drawn, in the scene's units
                //      (its pixels, times its fit when it was shrunk to fit: §8f)

                // The canvas being drawn on, or nullptr (raised) outside draw().
                Canvas *drawing() {
                        AngelRegion *r = region();
                        Canvas      *c = r != nullptr ? r->drawCanvas() : nullptr;
                        if (c == nullptr) raise("draw on the scene only in draw()");
                        return c;
                }
                Point at(float x, float y) {
                        AngelRegion *r = region();
                        return Point{x * r->fit() + float(r->drawX()), y * r->fit() + float(r->drawY())};
                }
                // A length in the scene's units, in the region's pixels.
                float units(float length) {
                        return length * region()->fit();
                }
                uint8_t opacityOf(float o) {
                        return o <= 0 ? 0 : o >= 1 ? 255 : uint8_t(o * 255 + 0.5f);
                }
                void canvasClear(asIScriptGeneric *gen) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        AngelRegion *r = region();
                        c->fill(Rect{r->drawX(), r->drawY(), r->place().width, r->place().height},
                                gen->GetArgDWord(0) & 0xFFFFFF);
                }
                void canvasPlot(asIScriptGeneric *gen) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        c->fill(Rect{int32_t(floorf(p.x)), int32_t(floorf(p.y)), 1, 1}, gen->GetArgDWord(2) & 0xFFFFFF);
                }
                void strokePoints(Canvas &c, const Point *pts, uint32_t n, bool closed, Color color, float width) {
                        Path path;
                        path.addStroke(pts, n, closed, width > 0 ? width : 1);
                        (void)fillPath(c, path, color, 255);
                        return;
                }
                void canvasLine(asIScriptGeneric *gen) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point pts[2] = {at(gen->GetArgFloat(0), gen->GetArgFloat(1)),
                                        at(gen->GetArgFloat(2), gen->GetArgFloat(3))};
                        strokePoints(*c, pts, 2, false, gen->GetArgDWord(4) & 0xFFFFFF, units(gen->GetArgFloat(5)));
                }
                void canvasRect(asIScriptGeneric *gen) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        float w = units(gen->GetArgFloat(2)), h = units(gen->GetArgFloat(3));
                        Point pts[4] = {p, Point{p.x + w, p.y}, Point{p.x + w, p.y + h}, Point{p.x, p.y + h}};
                        strokePoints(*c, pts, 4, true, gen->GetArgDWord(4) & 0xFFFFFF, units(gen->GetArgFloat(5)));
                }
                void canvasFillRect(asIScriptGeneric *gen) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        Path  path;
                        path.addRect(p.x, p.y, units(gen->GetArgFloat(2)), units(gen->GetArgFloat(3)));
                        (void)fillPath(*c, path, gen->GetArgDWord(4) & 0xFFFFFF, opacityOf(gen->GetArgFloat(5)));
                }
                void ellipse(asIScriptGeneric *gen, float rx, float ry, uint32_t colorArg, bool filled) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        Color color = gen->GetArgDWord(colorArg) & 0xFFFFFF;
                        float extra = gen->GetArgFloat(colorArg + 1);
                        Path  path;
                        rx = units(rx);
                        ry = units(ry);
                        if (filled) {
                                path.addEllipse(p.x, p.y, rx, ry);
                                (void)fillPath(*c, path, color, opacityOf(extra));
                                return;
                        }
                        Point *pts = points();
                        if (pts == nullptr) return;
                        uint32_t n = circleSides(rx > ry ? rx : ry);
                        ellipsePoints(pts, n, p.x, p.y, rx, ry, 0, false);
                        strokePoints(*c, pts, n, true, color, units(extra));
                }
                void canvasCircle(asIScriptGeneric *gen) {
                        ellipse(gen, gen->GetArgFloat(2), gen->GetArgFloat(2), 3, false);
                }
                void canvasFillCircle(asIScriptGeneric *gen) {
                        ellipse(gen, gen->GetArgFloat(2), gen->GetArgFloat(2), 3, true);
                }
                void canvasEllipse(asIScriptGeneric *gen) {
                        ellipse(gen, gen->GetArgFloat(2), gen->GetArgFloat(3), 4, false);
                }
                void canvasFillEllipse(asIScriptGeneric *gen) {
                        ellipse(gen, gen->GetArgFloat(2), gen->GetArgFloat(3), 4, true);
                }
                void polygon(asIScriptGeneric *gen, bool filled) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point   *pts = points();
                        uint32_t n = 0;
                        if (pts == nullptr ||
                            !pointsOf(static_cast<const CScriptArray *>(gen->GetArgAddress(0)), pts, MaxPoints, &n))
                                return;
                        for (uint32_t i = 0; i < n; i++) pts[i] = at(pts[i].x, pts[i].y);
                        Color color = gen->GetArgDWord(1) & 0xFFFFFF;
                        float extra = gen->GetArgCount() > 2 ? gen->GetArgFloat(2) : (filled ? 1.0f : 2.0f);
                        if (filled) {
                                Path path;
                                path.addPolygon(pts, n);
                                (void)fillPath(*c, path, color, opacityOf(extra));
                        } else if (n >= 2) {
                                strokePoints(*c, pts, n, true, color, units(extra));
                        }
                }
                void canvasPolygon(asIScriptGeneric *gen) {
                        polygon(gen, false);
                }
                void canvasFillPolygon(asIScriptGeneric *gen) {
                        polygon(gen, true);
                }
                void canvasText(asIScriptGeneric *gen) {
                        ScriptFont *f = fontArg(gen, 4);
                        Canvas     *c = drawing();
                        if (c == nullptr) {
                                if (f != nullptr) f->release();
                                return;
                        }
                        angel::Text text;
                        angel::formatValue(gen->GetEngine(), text, gen->GetArgAddress(2), gen->GetArgTypeId(2));
                        FontKey key = f != nullptr ? f->key() : sceneFont(theme().body.font);
                        if (f != nullptr) f->release();
                        AngelRegion *r = region();
                        Point        p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        StringView   t(text.data(), uint32_t(text.length()));
                        // Made at the size it shows; its width in the scene's units.
                        FontKey shown = rasterFont(r->context(), key);
                        r->context().fonts->draw(*c, shown, int32_t(lroundf(p.x)), int32_t(lroundf(p.y)), t,
                                                 gen->GetArgDWord(3) & 0xFFFFFF);
                        gen->SetReturnDWord(asDWORD(lroundf(float(r->context().fonts->width(shown, t)) / r->fit())));
                }

                // ---- Interactive pieces (§7.10)

                struct ScriptEvent {
                                uint32_t refs = 1;
                                int32_t  type = 0;
                                String   key;
                                String   text;
                                int32_t  x = 0;
                                int32_t  y = 0;
                                int32_t  button = 0;
                                int32_t  wheel = 0;
                                bool     repeat = false;
                };

                void eventAddRef(asIScriptGeneric *gen) {
                        static_cast<ScriptEvent *>(gen->GetObject())->refs++;
                }
                void eventRelease(asIScriptGeneric *gen) {
                        releaseEventObject(gen->GetObject());
                }

                void releaseCallback(void *callback) {
                        static_cast<asIScriptFunction *>(callback)->Release();
                        return;
                }

                // A widget's size from its label: the theme's text with room round it.
                void sizeWidget(Shape *s) {
                        AngelRegion *r = region();
                        float        tw = 0, th = 0;
                        if (r != nullptr) s->measureText(r->context(), &tw, &th);
                        if (s->kind == ShapeKind::Button) {
                                s->width = tw + 32;
                                s->height = th + 16;
                        } else {
                                float b = (th + 8) * 0.7f;
                                s->width = b + 8 + tw;
                                s->height = th + 8;
                        }
                        return;
                }
                void makeSlider(asIScriptGeneric *gen) {
                        Shape *s = newShape(ShapeKind::Slider, color("Script.Accent"));
                        if (s != nullptr) {
                                s->minimum = gen->GetArgFloat(0);
                                s->maximum = gen->GetArgFloat(1);
                                if (s->maximum < s->minimum) s->maximum = s->minimum;
                                float v = gen->GetArgFloat(2);
                                s->value = v < s->minimum ? s->minimum : v > s->maximum ? s->maximum : v;
                                s->width = gen->GetArgFloat(3);
                                s->height = 28;
                                s->lineWidth = 0;
                        }
                        returnShape(gen, s);
                }
                void makeLabelled(asIScriptGeneric *gen) {
                        ShapeKind     kind = ShapeKind(reinterpret_cast<uintptr_t>(gen->GetAuxiliary()));
                        const String &label = argString(gen, 0);
                        Shape        *s = newShape(kind, theme().body.color);
                        if (s != nullptr) {
                                s->lineWidth = 0;
                                if (kind == ShapeKind::Toggle) s->on = gen->GetArgByte(1) != 0;
                                if (!s->setText(label.data(), label.length())) {
                                        s->release();
                                        s = nullptr;
                                } else {
                                        sizeWidget(s);
                                }
                        }
                        returnShape(gen, s);
                }
                void labelSet(asIScriptGeneric *gen) {
                        const String &t = argString(gen, 0);
                        if (!self(gen)->setText(t.data(), t.length())) {
                                raise("Out of memory for text");
                                return;
                        }
                        sizeWidget(self(gen));
                }
                void getOn(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->on ? 1 : 0);
                }
                void setOn(asIScriptGeneric *gen) {
                        self(gen)->on = gen->GetArgByte(0) != 0;
                }
                void setValue(asIScriptGeneric *gen) {
                        Shape *s = self(gen);
                        float  v = gen->GetArgFloat(0);
                        s->value = v < s->minimum ? s->minimum : v > s->maximum ? s->maximum : v;
                }
                void onChange(asIScriptGeneric *gen) {
                        // The function's reference is this call's to keep.
                        Shape             *s = self(gen);
                        asIScriptFunction *f = nullptr;
                        memcpy(&f, gen->GetAddressOfArg(0), sizeof f);
                        if (s->callback != nullptr && s->releaseCallback != nullptr) s->releaseCallback(s->callback);
                        s->callback = f;
                        s->releaseCallback = f != nullptr ? releaseCallback : nullptr;
                }
                void startAnimation(asIScriptGeneric *gen) {
                        Arg<Animation> a(gen, 0);
                        if (!a) {
                                raise("scene.start() got null, not an animation");
                                return;
                        }
                        AngelRegion *r = sceneRegion();
                        if (r != nullptr) r->start(a.get(), gen->GetArgFloat(1));
                }

                // ---- Sound: a file of the deck's folder, heard when its slide is shown

                struct ScriptSound {
                                uint32_t  refs = 1;
                                int32_t   sound = -1; ///< The platform's (negative: none: it plays nothing).
                                Platform *platform = nullptr;
                };
                void soundAddRef(asIScriptGeneric *gen) {
                        static_cast<ScriptSound *>(gen->GetObject())->refs++;
                }
                void soundRelease(asIScriptGeneric *gen) {
                        auto *s = static_cast<ScriptSound *>(gen->GetObject());
                        if (--s->refs > 0) return;
                        if (s->sound >= 0) s->platform->closeSound(s->sound);
                        s->~ScriptSound();
                        free(s);
                }
                void makeSound(asIScriptGeneric *gen) {
                        AngelRegion *r = region();
                        char         path[512];
                        const char  *why = nullptr;
                        if (r == nullptr || !fileOf(view(argString(gen, 0)), path, sizeof path, nullptr, 0, &why)) {
                                raise("Sound `%s` %s", argString(gen, 0).cStr(),
                                      why != nullptr ? why : "can't be read");
                                return;
                        }
                        Platform &platform = r->owner().platform();
                        if (!platform.fileExists(path)) {
                                raise("Sound `%s` isn't there", argString(gen, 0).cStr());
                                return;
                        }
                        void *p = malloc(sizeof(ScriptSound));
                        if (p == nullptr) {
                                raise("Out of memory for a sound");
                                return;
                        }
                        auto *s = new (p) ScriptSound();
                        s->platform = &platform;
                        // Where there is no sound (the headless platform), one that plays nothing.
                        s->sound = platform.openSound(path);
                        gen->SetReturnAddress(s);
                }
                void soundPlay(asIScriptGeneric *gen) {
                        auto *s = static_cast<ScriptSound *>(gen->GetObject());
                        float v = gen->GetArgFloat(0);
                        if (region() != nullptr) region()->playSound(s->sound, v < 0 ? 0 : v > 1 ? 1 : v);
                }

                // ---- Step 13 (§8d): more shapes, heads, layout, copies

                // What drawing sizes text with now: the region's, or (angel before its stage) the host's.
                bool contextNow(SceneContext *out) {
                        if (region() != nullptr) {
                                *out = region()->context();
                                return true;
                        }
                        SceneHost *h = host();
                        if (h == nullptr) return false;
                        out->platform = &h->platform();
                        out->fonts = &h->fonts();
                        out->images = h->images();
                        out->theme = &h->theme();
                        return true;
                }

                // An object's box in its parent's pixels (the scene's, or its group's).
                struct Box {
                                float left = 0, top = 0, right = 0, bottom = 0;
                                float cx() const { return (left + right) / 2; }
                                float cy() const { return (top + bottom) / 2; }
                };
                Box boxOf(Shape *s, bool inScene = true) {
                        Box          b;
                        SceneContext context;
                        if (!contextNow(&context)) {
                                b.left = b.right = s->x;
                                b.top = b.bottom = s->y;
                                return b;
                        }
                        // In the scene's pixels (through its groups), or its group's (arrange()).
                        Rect r = shapeBounds(context, *s, inScene ? parentsOf(*s) : Affine());
                        b.left = float(r.x);
                        b.top = float(r.y);
                        b.right = float(r.x + r.width);
                        b.bottom = float(r.y + r.height);
                        return b;
                }
                void boxLeft(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(boxOf(self(gen)).left);
                }
                void boxRight(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(boxOf(self(gen)).right);
                }
                void boxTop(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(boxOf(self(gen)).top);
                }
                void boxBottom(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(boxOf(self(gen)).bottom);
                }

                enum class Side : uint8_t {
                        Left,
                        Right,
                        Above,
                        Below
                };

                void nextTo(asIScriptGeneric *gen) {
                        Shape     *s = self(gen);
                        Arg<Shape> other(gen, 0);
                        uint32_t   side = gen->GetArgDWord(1);
                        float      gap = gen->GetArgFloat(2);
                        if (!other) {
                                raise("nextTo() null");
                                return;
                        }
                        Box me = boxOf(s), it = boxOf(other.get());
                        switch (Side(side)) {
                                case Side::Left:
                                        s->x += it.left - gap - me.right;
                                        s->y += it.cy() - me.cy();
                                        break;
                                case Side::Right:
                                        s->x += it.right + gap - me.left;
                                        s->y += it.cy() - me.cy();
                                        break;
                                case Side::Above:
                                        s->y += it.top - gap - me.bottom;
                                        s->x += it.cx() - me.cx();
                                        break;
                                case Side::Below:
                                        s->y += it.bottom + gap - me.top;
                                        s->x += it.cx() - me.cx();
                                        break;
                        }
                        s->addRef();
                        gen->SetReturnAddress(s);
                }

                void copyShape(asIScriptGeneric *gen) {
                        Shape *c = self(gen)->clone();
                        if (c == nullptr) raise("Out of memory for a copy");
                        gen->SetReturnAddress(c);
                }

                void arrange(asIScriptGeneric *gen) {
                        // A group's objects in a row or a column, gap apart, the whole round its centre.
                        Shape   *g = self(gen);
                        uint32_t side = gen->GetArgDWord(0);
                        float    gap = gen->GetArgFloat(1);
                        bool     across = Side(side) == Side::Left || Side(side) == Side::Right;
                        bool     backwards = Side(side) == Side::Left || Side(side) == Side::Above;
                        float    at = 0, lo = 0, hi = 0;
                        for (uint32_t k = 0; k < g->childCount; k++) {
                                Shape *c = g->children[backwards ? g->childCount - 1 - k : k];
                                Box    b = boxOf(c, false);
                                if (across) {
                                        c->x += at - b.left;
                                        c->y -= b.cy();
                                        at += b.right - b.left + gap;
                                } else {
                                        c->y += at - b.top;
                                        c->x -= b.cx();
                                        at += b.bottom - b.top + gap;
                                }
                                if (k == 0) lo = 0;
                                hi = at - gap;
                        }
                        float middle = (lo + hi) / 2;
                        for (uint32_t k = 0; k < g->childCount; k++) {
                                if (across) {
                                        g->children[k]->x -= middle;
                                } else {
                                        g->children[k]->y -= middle;
                                }
                        }
                        g->addRef();
                        gen->SetReturnAddress(g);
                }

                // ---- Lines, arrows and arcs: their ends and heads

                template <ArrowHead Shape::*M> void getHead(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(self(gen)->*M));
                }
                template <ArrowHead Shape::*M> void setHead(asIScriptGeneric *gen) {
                        uint32_t h = gen->GetArgDWord(0);
                        self(gen)->*M = h <= uint32_t(ArrowHead::Bar) ? ArrowHead(h) : ArrowHead::None;
                }
                // A line's end @p which (0: its start) in its parent's pixels.
                void lineEnd(asIScriptGeneric *gen) {
                        Shape    *s = self(gen);
                        uintptr_t what = reinterpret_cast<uintptr_t>(gen->GetAuxiliary());
                        uint32_t  which = uint32_t(what >> 1), y = uint32_t(what & 1);
                        gen->SetReturnFloat(y != 0 ? s->y + s->ends[which].y : s->x + s->ends[which].x);
                }
                void setLineEnd(asIScriptGeneric *gen) {
                        // The other end stays; the line's place is its middle again.
                        Shape    *s = self(gen);
                        uintptr_t what = reinterpret_cast<uintptr_t>(gen->GetAuxiliary());
                        uint32_t  which = uint32_t(what >> 1), y = uint32_t(what & 1);
                        Point     e[2] = {{s->x + s->ends[0].x, s->y + s->ends[0].y},
                                          {s->x + s->ends[1].x, s->y + s->ends[1].y}};
                        if (y != 0) {
                                e[which].y = gen->GetArgFloat(0);
                        } else {
                                e[which].x = gen->GetArgFloat(0);
                        }
                        s->x = (e[0].x + e[1].x) / 2;
                        s->y = (e[0].y + e[1].y) / 2;
                        s->ends[0] = Point{e[0].x - s->x, e[0].y - s->y};
                        s->ends[1] = Point{e[1].x - s->x, e[1].y - s->y};
                }
                // Where a ray from a box's middle toward (@p tx, @p ty) leaves it.
                Point edgeToward(const Box &b, float tx, float ty) {
                        float cx = b.cx(), cy = b.cy(), dx = tx - cx, dy = ty - cy;
                        float hw = (b.right - b.left) / 2, hh = (b.bottom - b.top) / 2;
                        if (fabsf(dx) < 1e-6f && fabsf(dy) < 1e-6f) return Point{cx, cy};
                        float t = 1e30f;
                        if (fabsf(dx) > 1e-6f) t = hw / fabsf(dx);
                        if (fabsf(dy) > 1e-6f && hh / fabsf(dy) < t) t = hh / fabsf(dy);
                        return Point{cx + dx * t, cy + dy * t};
                }
                void between(asIScriptGeneric *gen) {
                        Shape     *s = self(gen);
                        Arg<Shape> a(gen, 0), b(gen, 1);
                        float      gap = gen->GetArgFloat(2);
                        if (!a || !b) {
                                raise("between() null");
                                return;
                        }
                        Box   ba = boxOf(a.get()), bb = boxOf(b.get());
                        Point from = edgeToward(ba, bb.cx(), bb.cy()), to = edgeToward(bb, ba.cx(), ba.cy());
                        float dx = to.x - from.x, dy = to.y - from.y, len = sqrtf(dx * dx + dy * dy);
                        if (len > 2 * gap && len > 0) {
                                from = Point{from.x + dx / len * gap, from.y + dy / len * gap};
                                to = Point{to.x - dx / len * gap, to.y - dy / len * gap};
                        }
                        s->x = (from.x + to.x) / 2;
                        s->y = (from.y + to.y) / 2;
                        s->ends[0] = Point{from.x - s->x, from.y - s->y};
                        s->ends[1] = Point{to.x - s->x, to.y - s->y};
                        s->addRef();
                        gen->SetReturnAddress(s);
                }

                // ---- The new kinds

                void makeEllipse(asIScriptGeneric *gen) {
                        Shape *s = newShape(ShapeKind::Ellipse, color("Script.Accent"));
                        if (s != nullptr) {
                                s->width = gen->GetArgFloat(0);
                                s->height = gen->GetArgFloat(1);
                        }
                        returnShape(gen, s);
                }
                void makeArc(asIScriptGeneric *gen) {
                        bool   sector = gen->GetAuxiliary() != nullptr;
                        Shape *s = newShape(sector ? ShapeKind::Sector : ShapeKind::Arc, color("Script.Accent"));
                        if (s != nullptr) {
                                s->radius = gen->GetArgFloat(0);
                                s->start = gen->GetArgFloat(1);
                                s->sweep = gen->GetArgFloat(2);
                        }
                        returnShape(gen, s);
                }
                void makeRegular(asIScriptGeneric *gen) {
                        bool    star = gen->GetAuxiliary() != nullptr;
                        int32_t n = int32_t(gen->GetArgDWord(0));
                        if (n < 3 || n > 100) {
                                raise("%s has 3 to 100 %s, not %d", star ? "A star" : "A regular polygon",
                                      star ? "points" : "sides", int(n));
                                return;
                        }
                        Shape *s = newShape(star ? ShapeKind::Star : ShapeKind::Regular, color("Script.Accent"));
                        if (s != nullptr) {
                                s->sides = uint32_t(n);
                                s->radius = gen->GetArgFloat(1);
                                if (star) {
                                        float inner = gen->GetArgFloat(2);
                                        s->innerRadius = inner > 0 ? inner : s->radius * 0.5f;
                                }
                        }
                        returnShape(gen, s);
                }
                void makeCurve(asIScriptGeneric *gen) {
                        Point   *pts = points();
                        uint32_t n = 0;
                        if (pts == nullptr ||
                            !pointsOf(static_cast<const CScriptArray *>(gen->GetArgAddress(0)), pts, MaxPoints, &n))
                                return;
                        if (n < 2) {
                                raise("A curve has 2 points at least");
                                return;
                        }
                        float x0 = pts[0].x, x1 = x0, y0 = pts[0].y, y1 = y0;
                        for (uint32_t i = 1; i < n; i++) {
                                x0 = pts[i].x < x0 ? pts[i].x : x0;
                                x1 = pts[i].x > x1 ? pts[i].x : x1;
                                y0 = pts[i].y < y0 ? pts[i].y : y0;
                                y1 = pts[i].y > y1 ? pts[i].y : y1;
                        }
                        Shape *s = newShape(ShapeKind::Curve, color("Script.Accent"));
                        if (s != nullptr) {
                                s->closed = gen->GetArgByte(1) != 0;
                                s->x = (x0 + x1) / 2;
                                s->y = (y0 + y1) / 2;
                                for (uint32_t i = 0; i < n; i++) pts[i] = Point{pts[i].x - s->x, pts[i].y - s->y};
                                if (!s->setPoints(pts, n)) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        returnShape(gen, s);
                }
                void curveCount(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->pointCount);
                }
                template <bool Shape::*M> void getFlag(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->*M ? 1 : 0);
                }
                template <bool Shape::*M> void setFlag(asIScriptGeneric *gen) {
                        self(gen)->*M = gen->GetArgByte(0) != 0;
                }
                void makeNumber(asIScriptGeneric *gen) {
                        float       v = gen->GetArgFloat(0);
                        int32_t     decimals = int32_t(gen->GetArgDWord(1));
                        ScriptFont *f = fontArg(gen, 2);
                        Shape      *s = newShape(ShapeKind::Number, theme().body.color);
                        if (s != nullptr) {
                                s->font = f;
                                f = nullptr;
                                s->decimals = decimals < 0 ? 0 : decimals > 9 ? 9 : decimals;
                                if (!s->setNumber(v)) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        if (f != nullptr) f->release();
                        returnShape(gen, s);
                }
                void numberSet(asIScriptGeneric *gen) {
                        if (!self(gen)->setNumber(gen->GetArgFloat(0))) raise("Out of memory for text");
                }
                void numberDecimals(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(uint32_t(self(gen)->decimals));
                }
                void numberSetDecimals(asIScriptGeneric *gen) {
                        Shape  *s = self(gen);
                        int32_t d = int32_t(gen->GetArgDWord(0));
                        s->decimals = d < 0 ? 0 : d > 9 ? 9 : d;
                        if (!s->setNumber(s->value)) raise("Out of memory for text");
                }
                void sidesGet(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->sides);
                }
                void sidesSet(asIScriptGeneric *gen) {
                        uint32_t n = gen->GetArgDWord(0);
                        if (n < 3 || n > 100) {
                                raise("3 to 100, not %u", unsigned(n));
                                return;
                        }
                        self(gen)->sides = n;
                }

                // ---- Axes

                void makeAxes(asIScriptGeneric *gen) {
                        Shape *s = newShape(ShapeKind::Axes, color("Script.Line"));
                        if (s != nullptr) {
                                s->width = gen->GetArgFloat(0);
                                s->height = gen->GetArgFloat(1);
                                s->xMin = gen->GetArgFloat(2);
                                s->xMax = gen->GetArgFloat(3);
                                s->yMin = gen->GetArgFloat(4);
                                s->yMax = gen->GetArgFloat(5);
                                s->lineWidth = 2;
                                if (!(s->xMax > s->xMin) || !(s->yMax > s->yMin)) {
                                        s->release();
                                        raise("Axes' ranges go from less to more (xMin < xMax, yMin < yMax)");
                                        return;
                                }
                        }
                        returnShape(gen, s);
                }
                float axesX(const Shape &s, float x) {
                        return s.x + s.scale * (-s.width / 2 + (x - s.xMin) / (s.xMax - s.xMin) * s.width);
                }
                float axesY(const Shape &s, float y) {
                        return s.y + s.scale * (s.height / 2 - (y - s.yMin) / (s.yMax - s.yMin) * s.height);
                }
                void axesToX(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(axesX(*self(gen), gen->GetArgFloat(0)));
                }
                void axesToY(asIScriptGeneric *gen) {
                        gen->SetReturnFloat(axesY(*self(gen), gen->GetArgFloat(0)));
                }
                void plot(asIScriptGeneric *gen) {
                        // The function's values over the x range, a curve of straight pieces in the
                        // scene's pixels (where the axes are now).
                        Shape             *axes = self(gen);
                        asIScriptFunction *f = nullptr;
                        memcpy(&f, gen->GetAddressOfArg(0), sizeof f);
                        bool     given = gen->GetArgCount() > 1;
                        Color    c = given ? gen->GetArgDWord(1) & 0xFFFFFF : color("Script.Accent");
                        int32_t  samples = given ? int32_t(gen->GetArgDWord(2)) : 100;
                        Point   *pts = points();
                        uint32_t n = 0;
                        if (samples < 2) samples = 2;
                        if (samples > int32_t(MaxPoints)) samples = int32_t(MaxPoints);
                        asIScriptContext *ctx = asGetActiveContext();
                        bool              failed = f == nullptr;
                        if (pts != nullptr && !failed && ctx != nullptr && ctx->PushState() >= 0) {
                                for (int32_t i = 0; i < samples; i++) {
                                        float x =
                                                axes->xMin + (axes->xMax - axes->xMin) * float(i) / float(samples - 1);
                                        if (ctx->Prepare(f) < 0) {
                                                failed = true;
                                                break;
                                        }
                                        ctx->SetArgFloat(0, x);
                                        if (ctx->Execute() != asEXECUTION_FINISHED) {
                                                failed = true;
                                                break;
                                        }
                                        float y = ctx->GetReturnFloat();
                                        if (!isfinite(y)) continue;
                                        pts[n++] = Point{axesX(*axes, x), axesY(*axes, y)};
                                }
                                ctx->PopState();
                        }
                        if (f != nullptr) f->Release();
                        if (failed) {
                                raise("plot(): the function couldn't be run for every x");
                                return;
                        }
                        if (n < 2) {
                                raise("plot(): fewer than 2 points had a value");
                                return;
                        }
                        float x0 = pts[0].x, x1 = x0, y0 = pts[0].y, y1 = y0;
                        for (uint32_t i = 1; i < n; i++) {
                                x0 = pts[i].x < x0 ? pts[i].x : x0;
                                x1 = pts[i].x > x1 ? pts[i].x : x1;
                                y0 = pts[i].y < y0 ? pts[i].y : y0;
                                y1 = pts[i].y > y1 ? pts[i].y : y1;
                        }
                        Shape *s = newShape(ShapeKind::Curve, c);
                        if (s != nullptr) {
                                s->smooth = false;
                                s->x = (x0 + x1) / 2;
                                s->y = (y0 + y1) / 2;
                                for (uint32_t i = 0; i < n; i++) pts[i] = Point{pts[i].x - s->x, pts[i].y - s->y};
                                if (!s->setPoints(pts, n)) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        returnShape(gen, s);
                }

                // ---- The new animations

                void animFadeTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(Property::Opacity, gen->GetArgFloat(0));
                        returnAnimation(gen, a);
                }
                void animRotateBy(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(Property::Rotation, gen->GetArgFloat(0), true);
                        returnAnimation(gen, a);
                }
                void animScaleBy(asIScriptGeneric *gen) {
                        // From its scale when made (relative changes add, and this multiplies).
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(Property::Scale, self(gen)->scale * gen->GetArgFloat(0));
                        returnAnimation(gen, a);
                }
                void animIndicate(asIScriptGeneric *gen) {
                        // A fifth bigger and the highlight's colour, and back.
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Scale, self(gen)->scale * 0.2f, true);
                                a->changeColor(Property::StrokeColor, color("Script.Highlight"));
                                a->ease = Ease::ThereAndBack;
                        }
                        returnAnimation(gen, a);
                }
                void animWiggle(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Rotation, 12, true);
                                a->change(Property::Scale, self(gen)->scale * 0.08f, true);
                                a->ease = Ease::Wiggle;
                        }
                        returnAnimation(gen, a);
                }
                template <Property P> void animTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(P, gen->GetArgFloat(0));
                        returnAnimation(gen, a);
                }

                // ---- The scene's order

                void sceneToFront(asIScriptGeneric *gen) {
                        Arg<Shape>   s(gen, 0);
                        AngelRegion *r = s ? sceneRegion() : nullptr;
                        if (r != nullptr) r->objects().toFront(s.get());
                }
                void sceneToBack(asIScriptGeneric *gen) {
                        Arg<Shape>   s(gen, 0);
                        AngelRegion *r = s ? sceneRegion() : nullptr;
                        if (r != nullptr) r->objects().toBack(s.get());
                }
                void sceneHas(asIScriptGeneric *gen) {
                        Arg<Shape>   s(gen, 0);
                        AngelRegion *r = region();
                        gen->SetReturnByte(s && r != nullptr && r->objects().has(s.get()) ? 1 : 0);
                }
                void sceneRemoveAll(asIScriptGeneric *) {
                        AngelRegion *r = sceneRegion();
                        if (r == nullptr) return;
                        while (r->objects().count() > 0) r->remove(r->objects().at(r->objects().count() - 1));
                }
                void playLagged(asIScriptGeneric *gen) {
                        // Each lag seconds after the one before.
                        const CScriptArray *array = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        float               lag = gen->GetArgFloat(2);
                        uint32_t            n = array != nullptr ? array->GetSize() : 0;
                        for (uint32_t i = 0; i < n; i++) {
                                Animation *a = *static_cast<Animation *const *>(array->At(i));
                                if (a != nullptr) a->delay += float(i) * (lag > 0 ? lag : 0);
                        }
                        playArray(gen);
                }

                // ---- Step 14 (§8e): composites, updaters, paths, charts, the camera, icons

                Shape *kid(Shape *s, uint32_t i) {
                        return i < s->childCount ? s->children[i] : nullptr;
                }

                // A child returned as a handle (raised if there is none).
                void returnKid(asIScriptGeneric *gen, Shape *s, uint32_t i, const char *what) {
                        Shape *c = kid(s, i);
                        if (c == nullptr) {
                                raise("No %s %u", what, unsigned(i));
                                return;
                        }
                        c->addRef();
                        gen->SetReturnAddress(c);
                }

                // Text for a composite, in @p key's font (null: the theme's text), centred at (x, y).
                Shape *textOf(StringView text, const FontKey *key, Color c, float x, float y) {
                        Shape *t = newShape(ShapeKind::Text, c);
                        if (t == nullptr) return nullptr;
                        if (key != nullptr) t->font = ScriptFont::make(*key);
                        if (!t->setText(text.data(), text.length())) {
                                t->release();
                                return nullptr;
                        }
                        t->x = x;
                        t->y = y;
                        return t;
                }

                // @p child into @p parent, its reference given to it; false (and let go) if it can't be.
                bool adopt(Shape *parent, Shape *child) {
                        if (child == nullptr) return false;
                        bool ok = parent->addChild(child);
                        child->release();
                        return ok;
                }

                FontKey smallFont() {
                        return sceneFont(theme().header.font);
                }

                FontKey fontSized(FontKey key, float most) {
                        if (float(key.size) > most) key.size = uint16_t(most < 8 ? 8 : most);
                        return key;
                }


                // Numbers (int or float) of an array, at most @p max; false (raised) past that.
                bool numbersOf(const CScriptArray *array, float *out, uint32_t max, uint32_t *count) {
                        uint32_t n = array != nullptr ? array->GetSize() : 0;
                        if (n > max) {
                                raise("%u values at most, not %u", unsigned(max), unsigned(n));
                                return false;
                        }
                        bool ints = n > 0 && array->GetElementTypeId() == asTYPEID_INT32;
                        for (uint32_t i = 0; i < n; i++) {
                                out[i] = ints ? float(*static_cast<const int32_t *>(array->At(i)))
                                              : *static_cast<const float *>(array->At(i));
                        }
                        *count = n;
                        return true;
                }

                // A fill and its opacity, animated.
                // How bright @p c looks, 0 to 1 (its relative luminance).
                float luminanceOf(Color c) {
                        auto linear = [](uint32_t v) {
                                float f = float(v) / 255;
                                return f <= 0.04045f ? f / 12.92f : powf((f + 0.055f) / 1.055f, 2.4f);
                        };
                        return 0.2126f * linear(c >> 16 & 0xFF) + 0.7152f * linear(c >> 8 & 0xFF) +
                               0.0722f * linear(c & 0xFF);
                }
                float contrastOf(Color a, Color b) {
                        float la = luminanceOf(a) + 0.05f, lb = luminanceOf(b) + 0.05f;
                        return la > lb ? la / lb : lb / la;
                }
                // Text on @p back: @p text if it reads there (4.5 to 1, as WCAG asks of text), else
                // black or white, whichever reads better.
                Color readableOn(Color back, Color text) {
                        if (contrastOf(back, text) >= 4.5f) return text;
                        return contrastOf(back, 0x000000) >= contrastOf(back, 0xFFFFFF) ? 0x000000 : 0xFFFFFF;
                }

                // @p s filled with @p c (@p opacity of it); @p label, the text on it, coloured to read
                // there (on the theme's background where the fill goes).
                Animation *fillAnimation(Shape *s, Color c, float opacity, Shape *label = nullptr) {
                        Animation *a = Animation::make(s);
                        if (a == nullptr) return nullptr;
                        a->changeColor(Property::FillColor, c);
                        a->change(Property::FillOpacity, opacity);
                        if (label != nullptr) {
                                Animation *t = Animation::make(label);
                                if (t != nullptr) {
                                        Color text = theme().body.color;
                                        t->changeColor(Property::StrokeColor,
                                                       readableOn(opacity > 0 ? c : theme().background, text));
                                        (void)a->join(t);
                                        t->release();
                                }
                        }
                        return a;
                }

                // @p part played with @p whole (its reference let go here).
                void joined(Animation *whole, Animation *part) {
                        if (whole != nullptr && part != nullptr) (void)whole->join(part);
                        if (part != nullptr) part->release();
                        return;
                }

                // An animation of nothing of @p s's own: what its parts do (joined to it).
                Animation *whole(Shape *s) {
                        Animation *a = Animation::make(s);
                        if (a == nullptr) raise("Out of memory for an animation");
                        return a;
                }

                // ---- 1. Arrays

                float arrayX(uint32_t n, float cell, uint32_t i) {
                        return (float(i) - float(n - 1) / 2) * cell;
                }

                void makeArray(asIScriptGeneric *gen) {
                        // An array<int> (a list of numbers: only it takes {3, 1, 4}), or any array (?).
                        int                 typeId = gen->GetArgTypeId(0);
                        asITypeInfo        *type = gen->GetEngine()->GetTypeInfoById(typeId);
                        const CScriptArray *values = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        if ((typeId & asTYPEID_OBJHANDLE) != 0 && values != nullptr)
                                values = *reinterpret_cast<const CScriptArray *const *>(values);
                        if (type == nullptr || strcmp(type->GetName(), "array") != 0) {
                                raise("SceneArray() of an array (numbers or text), not a %s",
                                      type != nullptr ? type->GetName() : "value");
                                return;
                        }
                        float    cell = gen->GetArgFloat(1);
                        uint32_t n = values != nullptr ? values->GetSize() : 0;
                        if (n > 64) {
                                raise("An array shows 64 values at most, not %u", unsigned(n));
                                return;
                        }
                        if (cell < 8) cell = 8;
                        Shape *a = newShape(ShapeKind::Array, color("Script.Line"));
                        if (a == nullptr) {
                                raise("Out of memory for an array");
                                return;
                        }
                        a->rows = 1;
                        a->columns = n;
                        a->step = cell;
                        FontKey code = fontSized(sceneFont(theme().code.font), cell * 0.45f), small = smallFont();
                        bool    ok = true;
                        for (uint32_t i = 0; i < n && ok; i++) {
                                Shape *c = newShape(ShapeKind::Rect, color("Script.Line"));
                                if (c != nullptr) {
                                        c->width = c->height = cell;
                                        c->x = arrayX(n, cell, i);
                                        c->fill = color("Script.Fill");
                                        c->fillOpacity = 1;
                                        c->lineWidth = 2;
                                }
                                ok = adopt(a, c);
                        }
                        for (uint32_t i = 0; i < n && ok; i++) {
                                angel::Text t;
                                angel::formatValue(gen->GetEngine(), t, values->At(i), values->GetElementTypeId());
                                ok = adopt(a, textOf(StringView(t.data(), uint32_t(t.length())), &code,
                                                     theme().body.color, arrayX(n, cell, i), 0));
                        }
                        for (uint32_t i = 0; i < n && ok; i++) {
                                char index[12];
                                snprintf(index, sizeof index, "%u", unsigned(i));
                                ok = adopt(a, textOf(StringView(index), &small, color("Script.Muted"),
                                                     arrayX(n, cell, i), cell / 2 + 14));
                        }
                        if (!ok) {
                                a->release();
                                raise("Out of memory for an array");
                                return;
                        }
                        returnShape(gen, a);
                }
                bool arrayIndex(Shape *a, uint32_t i) {
                        if (i < a->columns) return true;
                        raise("No cell %u (there are %u)", unsigned(i), unsigned(a->columns));
                        return false;
                }
                void arrayCount(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->columns);
                }
                void arrayCell(asIScriptGeneric *gen) {
                        Shape   *a = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        if (arrayIndex(a, i)) returnKid(gen, a, i, "cell");
                }
                void arrayText(asIScriptGeneric *gen) {
                        Shape   *a = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        if (arrayIndex(a, i)) returnKid(gen, a, a->columns + i, "cell");
                }
                void arrayValue(asIScriptGeneric *gen) {
                        Shape   *a = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        Shape   *t = arrayIndex(a, i) ? kid(a, a->columns + i) : nullptr;
                        new (gen->GetAddressOfReturnLocation()) String(
                                t != nullptr && t->text != nullptr ? t->text : "", t != nullptr ? t->textLength : 0);
                }
                void arrayIndices(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->indices ? 1 : 0);
                }
                void arraySetIndices(asIScriptGeneric *gen) {
                        Shape *a = self(gen);
                        a->indices = gen->GetArgByte(0) != 0;
                        for (uint32_t i = 0; i < a->columns; i++) {
                                Shape *t = kid(a, 2 * a->columns + i);
                                if (t != nullptr) t->visible = a->indices;
                        }
                }
                void arrayHighlight(asIScriptGeneric *gen) {
                        Shape   *a = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        Color c = gen->GetArgCount() > 1 ? gen->GetArgDWord(1) & 0xFFFFFF : color("Script.Highlight");
                        if (!arrayIndex(a, i)) return;
                        returnAnimation(gen, fillAnimation(kid(a, i), c, 1, kid(a, a->columns + i)));
                }
                void arrayUnhighlight(asIScriptGeneric *gen) {
                        Shape   *a = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        if (!arrayIndex(a, i)) return;
                        returnAnimation(gen, fillAnimation(kid(a, i), color("Script.Fill"), 1, kid(a, a->columns + i)));
                }
                void arraySwap(asIScriptGeneric *gen) {
                        // The values change places along arcs, one over, one under; then their order.
                        Shape   *a = self(gen);
                        uint32_t i = gen->GetArgDWord(0), j = gen->GetArgDWord(1);
                        if (!arrayIndex(a, i) || !arrayIndex(a, j)) return;
                        Animation *all = whole(a);
                        if (all == nullptr) return;
                        if (i != j) {
                                uint32_t n = a->columns;
                                all->then = Animation::Then::SwapChildren;
                                all->index = uint16_t(n + i);
                                all->index2 = uint16_t(n + j);
                                Shape *ti = kid(a, n + i), *tj = kid(a, n + j);
                                float  lift = a->step * 0.75f;
                                Shape *ends[2] = {ti, tj};
                                for (uint32_t k = 0; k < 2; k++) {
                                        Animation *across = Animation::make(ends[k]);
                                        Animation *up = Animation::make(ends[k]);
                                        if (across != nullptr) across->change(Property::X, ends[1 - k]->x);
                                        if (up != nullptr) {
                                                up->change(Property::Y, k == 0 ? -lift : lift, true);
                                                up->ease = Ease::ThereAndBack;
                                        }
                                        joined(all, across);
                                        joined(all, up);
                                }
                        }
                        returnAnimation(gen, all);
                }
                void arraySet(asIScriptGeneric *gen) {
                        // The value turns into the new one where it is.
                        Shape   *a = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        if (!arrayIndex(a, i)) return;
                        Shape      *t = kid(a, a->columns + i);
                        angel::Text v;
                        angel::formatValue(gen->GetEngine(), v, gen->GetArgAddress(1), gen->GetArgTypeId(1));
                        Shape *to = t->clone();
                        if (to == nullptr || !to->setText(v.data(), uint32_t(v.length()))) {
                                if (to != nullptr) to->release();
                                raise("Out of memory for a value");
                                return;
                        }
                        Animation *m = Animation::make(t);
                        if (m == nullptr) {
                                to->release();
                                raise("Out of memory for an animation");
                                return;
                        }
                        m->change(Property::Morph, 1);
                        m->from(0);
                        m->other = to;
                        m->then = Animation::Then::Become;
                        returnAnimation(gen, m);
                }
                void arrayPointTo(asIScriptGeneric *gen) {
                        // A labelled arrow under a cell: made the first time, moved after.
                        Shape        *a = self(gen);
                        const String &name = argString(gen, 0);
                        uint32_t      i = gen->GetArgDWord(1);
                        if (!arrayIndex(a, i)) return;
                        float x = arrayX(a->columns, a->step, i);
                        for (uint32_t k = 3 * a->columns; k < a->childCount; k++) {
                                Shape *p = a->children[k];
                                Shape *label = kid(p, 1);
                                if (label == nullptr || label->textLength != name.length() ||
                                    memcmp(label->text, name.data(), name.length()) != 0)
                                        continue;
                                Animation *m = Animation::make(p);
                                if (m != nullptr) m->change(Property::X, x);
                                returnAnimation(gen, m);
                                return;
                        }
                        Shape *p = newShape(ShapeKind::Group, color("Script.Accent"));
                        if (p == nullptr) {
                                raise("Out of memory for a pointer");
                                return;
                        }
                        p->x = x;
                        p->y = a->step / 2 + (a->indices ? 26 : 6);
                        Shape *arrow = newShape(ShapeKind::Arrow, color("Script.Accent"));
                        if (arrow != nullptr) {
                                arrow->ends[0] = Point{0, 30};
                                arrow->ends[1] = Point{0, 2};
                                arrow->head = ArrowHead::Triangle;
                        }
                        FontKey small = smallFont();
                        bool    ok = adopt(p, arrow) && adopt(p, textOf(StringView(name.data(), name.length()), &small,
                                                                        color("Script.Accent"), 0, 44));
                        p->addRef(); // ours, to animate
                        if (!ok || !adopt(a, p)) {
                                p->release();
                                raise("Out of memory for a pointer");
                                return;
                        }
                        Animation *m = Animation::make(p);
                        p->release();
                        if (m != nullptr) {
                                m->change(Property::Opacity, 0, true);
                                m->from(0);
                        }
                        returnAnimation(gen, m);
                }

                // ---- 1. Tables

                void makeTable(asIScriptGeneric *gen) {
                        uint32_t rows = gen->GetArgDWord(0), columns = gen->GetArgDWord(1);
                        float    cw = gen->GetArgFloat(2), ch = gen->GetArgFloat(3);
                        if (rows == 0 || columns == 0 || rows * columns > 400) {
                                raise("A table has 1 to 400 cells, not %u × %u", unsigned(rows), unsigned(columns));
                                return;
                        }
                        Shape *t = newShape(ShapeKind::Table, color("Script.Line"));
                        if (t == nullptr) {
                                raise("Out of memory for a table");
                                return;
                        }
                        t->rows = rows;
                        t->columns = columns;
                        t->width = cw;
                        t->height = ch;
                        FontKey key = fontSized(sceneFont(theme().body.font), ch * 0.5f);
                        bool    ok = true;
                        for (uint32_t k = 0; k < rows * columns && ok; k++) {
                                Shape *c = newShape(ShapeKind::Rect, color("Script.Line"));
                                if (c != nullptr) {
                                        c->width = cw;
                                        c->height = ch;
                                        c->x = (float(k % columns) - float(columns - 1) / 2) * cw;
                                        c->y = (float(k / columns) - float(rows - 1) / 2) * ch;
                                        c->fill = color("Script.Fill");
                                        c->fillOpacity = 0;
                                        c->lineWidth = 1.5f;
                                }
                                ok = adopt(t, c);
                        }
                        for (uint32_t k = 0; k < rows * columns && ok; k++) {
                                ok = adopt(t, textOf(StringView(""), &key, theme().body.color,
                                                     (float(k % columns) - float(columns - 1) / 2) * cw,
                                                     (float(k / columns) - float(rows - 1) / 2) * ch));
                        }
                        if (!ok) {
                                t->release();
                                raise("Out of memory for a table");
                                return;
                        }
                        returnShape(gen, t);
                }
                bool tableAt(Shape *t, uint32_t r, uint32_t c, uint32_t *k) {
                        if (r >= t->rows || c >= t->columns) {
                                raise("No cell (%u, %u): the table is %u × %u", unsigned(r), unsigned(c),
                                      unsigned(t->rows), unsigned(t->columns));
                                return false;
                        }
                        *k = r * t->columns + c;
                        return true;
                }
                void tableSet(asIScriptGeneric *gen) {
                        Shape   *t = self(gen);
                        uint32_t k;
                        if (!tableAt(t, gen->GetArgDWord(0), gen->GetArgDWord(1), &k)) return;
                        angel::Text v;
                        angel::formatValue(gen->GetEngine(), v, gen->GetArgAddress(2), gen->GetArgTypeId(2));
                        if (!kid(t, t->rows * t->columns + k)->setText(v.data(), uint32_t(v.length())))
                                raise("Out of memory for text");
                }
                void tableGet(asIScriptGeneric *gen) {
                        Shape   *t = self(gen);
                        uint32_t k = 0;
                        Shape   *x = tableAt(t, gen->GetArgDWord(0), gen->GetArgDWord(1), &k)
                                             ? kid(t, t->rows * t->columns + k)
                                             : nullptr;
                        new (gen->GetAddressOfReturnLocation()) String(
                                x != nullptr && x->text != nullptr ? x->text : "", x != nullptr ? x->textLength : 0);
                }
                void tableCell(asIScriptGeneric *gen) {
                        Shape   *t = self(gen);
                        uint32_t k;
                        if (tableAt(t, gen->GetArgDWord(0), gen->GetArgDWord(1), &k)) returnKid(gen, t, k, "cell");
                }
                void tableText(asIScriptGeneric *gen) {
                        Shape   *t = self(gen);
                        uint32_t k;
                        if (tableAt(t, gen->GetArgDWord(0), gen->GetArgDWord(1), &k))
                                returnKid(gen, t, t->rows * t->columns + k, "cell");
                }
                void tableRows(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->rows);
                }
                void tableColumns(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->columns);
                }
                void tableHeader(asIScriptGeneric *gen) {
                        gen->SetReturnByte(self(gen)->header ? 1 : 0);
                }
                void tableSetHeader(asIScriptGeneric *gen) {
                        // The first row on the fill, its text bold.
                        Shape *t = self(gen);
                        t->header = gen->GetArgByte(0) != 0;
                        for (uint32_t c = 0; c < t->columns; c++) {
                                kid(t, c)->fillOpacity = t->header ? 1 : 0;
                                Shape *x = kid(t, t->rows * t->columns + c);
                                if (x->font == nullptr) continue;
                                FontKey key = x->font->key();
                                key.style = t->header ? uint8_t(key.style | FontKey::Bold)
                                                      : uint8_t(key.style & ~FontKey::Bold);
                                ScriptFont *f = ScriptFont::make(key);
                                if (f == nullptr) continue;
                                x->font->release();
                                x->font = f;
                                x->measuredWidth = -1;
                                x->dropCaches();
                        }
                }
                void tableHighlight(asIScriptGeneric *gen) {
                        Shape   *t = self(gen);
                        uint32_t k;
                        if (!tableAt(t, gen->GetArgDWord(0), gen->GetArgDWord(1), &k)) return;
                        returnAnimation(gen, fillAnimation(kid(t, k), color("Script.Highlight"), 1,
                                                           kid(t, t->rows * t->columns + k)));
                }
                void tableHighlightLine(asIScriptGeneric *gen) {
                        // A row (the auxiliary 0) or a column (1).
                        Shape   *t = self(gen);
                        bool     column = gen->GetAuxiliary() != nullptr;
                        uint32_t i = gen->GetArgDWord(0);
                        if (i >= (column ? t->columns : t->rows)) {
                                raise("No %s %u", column ? "column" : "row", unsigned(i));
                                return;
                        }
                        Animation *all = whole(t);
                        uint32_t   n = column ? t->rows : t->columns;
                        for (uint32_t k = 0; k < n && all != nullptr; k++) {
                                uint32_t cell = column ? k * t->columns + i : i * t->columns + k;
                                joined(all, fillAnimation(kid(t, cell), color("Script.Highlight"), 1,
                                                          kid(t, t->rows * t->columns + cell)));
                        }
                        returnAnimation(gen, all);
                }
                void tableUnhighlightAll(asIScriptGeneric *gen) {
                        Shape     *t = self(gen);
                        Animation *all = whole(t);
                        for (uint32_t k = 0; k < t->rows * t->columns && all != nullptr; k++) {
                                bool head = t->header && k < t->columns;
                                joined(all, fillAnimation(kid(t, k), color("Script.Fill"), head ? 1.0f : 0.0f,
                                                          kid(t, t->rows * t->columns + k)));
                        }
                        returnAnimation(gen, all);
                }

                // ---- 2. Graphs: children its edges (drawn under), then its nodes (each a group)

                void makeGraph(asIScriptGeneric *gen) {
                        Shape *g = newShape(ShapeKind::Graph, color("Script.Line"));
                        returnShape(gen, g);
                }
                Shape *graphNode(Shape *g, uint32_t i) {
                        if (i < g->rows) return kid(g, g->columns + i);
                        raise("No node %u (there are %u)", unsigned(i), unsigned(g->rows));
                        return nullptr;
                }
                void settleNow(Shape *g) {
                        SceneContext context;
                        if (contextNow(&context)) settleGraph(context, *g);
                }
                void graphAddNode(asIScriptGeneric *gen) {
                        Shape *g = self(gen);
                        if (g->rows >= 200) {
                                raise("A graph has 200 nodes at most");
                                return;
                        }
                        angel::Text label;
                        angel::formatValue(gen->GetEngine(), label, gen->GetArgAddress(0), gen->GetArgTypeId(0));
                        Shape *node = newShape(ShapeKind::Group, color("Script.Line"));
                        Shape *circle = newShape(ShapeKind::Circle, color("Script.Line"));
                        if (circle != nullptr) {
                                circle->radius = 26;
                                circle->fill = color("Script.Fill");
                                circle->fillOpacity = 1;
                                circle->lineWidth = 2;
                        }
                        FontKey key = fontSized(sceneFont(theme().body.font), 22);
                        bool    ok = node != nullptr && adopt(node, circle) &&
                                  adopt(node, textOf(StringView(label.data(), uint32_t(label.length())), &key,
                                                     theme().body.color, 0, 0));
                        if (node != nullptr) node->x = float(g->rows) * 80;
                        if (!ok || !adopt(g, node)) {
                                raise("Out of memory for a node");
                                return;
                        }
                        gen->SetReturnDWord(g->rows++);
                }
                void graphAddEdge(asIScriptGeneric *gen) {
                        Shape   *g = self(gen);
                        uint32_t a = gen->GetArgDWord(0), b = gen->GetArgDWord(1);
                        bool     directed = gen->GetArgByte(2) != 0;
                        if (graphNode(g, a) == nullptr || graphNode(g, b) == nullptr) return;
                        if (g->columns >= 400) {
                                raise("A graph has 400 edges at most");
                                return;
                        }
                        uint16_t *list = static_cast<uint16_t *>(malloc((g->columns + 1) * 3 * sizeof(uint16_t)));
                        Shape    *line = newShape(directed ? ShapeKind::Arrow : ShapeKind::Line, color("Script.Line"));
                        if (list == nullptr || line == nullptr) {
                                free(list);
                                if (line != nullptr) line->release();
                                raise("Out of memory for an edge");
                                return;
                        }
                        line->lineWidth = 2;
                        line->head = directed ? ArrowHead::Triangle : ArrowHead::None;
                        if (g->columns > 0) memcpy(list, g->edges, g->columns * 3 * sizeof(uint16_t));
                        list[g->columns * 3] = uint16_t(a);
                        list[g->columns * 3 + 1] = uint16_t(b);
                        list[g->columns * 3 + 2] = directed ? 1 : 0;
                        bool ok = g->insertChild(line, g->columns) && g->setEdges(list, g->columns + 1);
                        line->release();
                        free(list);
                        if (!ok) {
                                raise("Out of memory for an edge");
                                return;
                        }
                        gen->SetReturnDWord(g->columns++);
                        settleNow(g);
                }
                void graphNodeOf(asIScriptGeneric *gen) {
                        Shape *g = self(gen);
                        Shape *n = graphNode(g, gen->GetArgDWord(0));
                        if (n == nullptr) return;
                        n->addRef();
                        gen->SetReturnAddress(n);
                }
                void graphNodes(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->rows);
                }
                void graphEdges(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->columns);
                }
                // Every node to (xs[i], ys[i]), the whole round the graph's centre, then its edges.
                void placeNodes(Shape *g, float *xs, float *ys) {
                        float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
                        for (uint32_t i = 0; i < g->rows; i++) {
                                if (i == 0 || xs[i] < x0) x0 = xs[i];
                                if (i == 0 || xs[i] > x1) x1 = xs[i];
                                if (i == 0 || ys[i] < y0) y0 = ys[i];
                                if (i == 0 || ys[i] > y1) y1 = ys[i];
                        }
                        for (uint32_t i = 0; i < g->rows; i++) {
                                Shape *n = kid(g, g->columns + i);
                                n->x = xs[i] - (x0 + x1) / 2;
                                n->y = ys[i] - (y0 + y1) / 2;
                        }
                        settleNow(g);
                        return;
                }
                void graphLayout(asIScriptGeneric *gen) {
                        // The auxiliary: 0 a row, 1 a circle, 2 a grid.
                        Shape   *g = self(gen);
                        uint32_t how = uint32_t(reinterpret_cast<uintptr_t>(gen->GetAuxiliary()));
                        uint32_t n = g->rows;
                        float   *xs = static_cast<float *>(malloc((n + 1) * 2 * sizeof(float)));
                        if (xs == nullptr) {
                                raise("Out of memory");
                                return;
                        }
                        float *ys = xs + n + 1;
                        for (uint32_t i = 0; i < n; i++) {
                                if (how == 0) {
                                        xs[i] = float(i) * gen->GetArgFloat(0);
                                        ys[i] = 0;
                                } else if (how == 1) {
                                        float r = gen->GetArgFloat(0),
                                              a = -3.14159265f / 2 + 2 * 3.14159265f * float(i) / float(n);
                                        xs[i] = r * cosf(a);
                                        ys[i] = r * sinf(a);
                                } else {
                                        uint32_t cols = gen->GetArgDWord(0);
                                        float    gap = gen->GetArgFloat(1);
                                        if (cols == 0) cols = 1;
                                        xs[i] = float(i % cols) * gap;
                                        ys[i] = float(i / cols) * gap;
                                }
                        }
                        placeNodes(g, xs, ys);
                        free(xs);
                }
                void graphLayoutTree(asIScriptGeneric *gen) {
                        // From root: each node's children the nodes its edges lead to (either way when
                        // undirected) not met yet; leaves left to right, a parent over its children.
                        Shape   *g = self(gen);
                        uint32_t root = gen->GetArgDWord(0), n = g->rows;
                        float    level = gen->GetArgFloat(1), sibling = gen->GetArgFloat(2);
                        if (graphNode(g, root) == nullptr) return;
                        size_t   words = size_t(n) * 4 + 1;
                        int32_t *mem = static_cast<int32_t *>(
                                malloc(words * sizeof(int32_t) + size_t(n + 1) * 2 * sizeof(float)));
                        if (mem == nullptr) {
                                raise("Out of memory");
                                return;
                        }
                        int32_t *parent = mem, *depth = mem + n, *order = mem + 2 * n, *seen = mem + 3 * n;
                        float   *xs = reinterpret_cast<float *>(mem + words), *ys = xs + n + 1;
                        for (uint32_t i = 0; i < n; i++) {
                                parent[i] = -1;
                                depth[i] = -1;
                                seen[i] = 0;
                        }
                        // Breadth first, in edge order.
                        uint32_t head = 0, tail = 0;
                        order[tail++] = int32_t(root);
                        depth[root] = 0;
                        while (head < tail) {
                                uint32_t u = uint32_t(order[head++]);
                                for (uint32_t k = 0; k < g->columns; k++) {
                                        uint32_t a = g->edges[k * 3], b = g->edges[k * 3 + 1];
                                        bool     both = g->edges[k * 3 + 2] == 0;
                                        uint32_t v = a == u ? b : (both && b == u ? a : n);
                                        if (v >= n || depth[v] >= 0) continue;
                                        depth[v] = depth[u] + 1;
                                        parent[v] = int32_t(u);
                                        order[tail++] = int32_t(v);
                                }
                        }
                        // Leaves get the next place; a parent sits over its children (its first, last).
                        float   next = 0;
                        int32_t deepest = 0;
                        for (uint32_t i = tail; i-- > 0;) {
                                uint32_t u = uint32_t(order[i]);
                                if (depth[u] > deepest) deepest = depth[u];
                        }
                        // Children's places first: by depth from the deepest up, leaves in BFS order.
                        for (uint32_t i = 0; i < tail; i++) xs[order[i]] = -1;
                        for (uint32_t i = 0; i < tail; i++) {
                                // A leaf when nothing has it as a parent.
                                uint32_t u = uint32_t(order[i]);
                                bool     leaf = true;
                                for (uint32_t k = 0; k < tail && leaf; k++) leaf = parent[order[k]] != int32_t(u);
                                if (leaf) seen[u] = 1;
                        }
                        // Leaves in the order a depth-first walk meets them, so subtrees stay together.
                        int32_t *stack = static_cast<int32_t *>(malloc(size_t(n + 1) * sizeof(int32_t)));
                        if (stack == nullptr) {
                                free(mem);
                                raise("Out of memory");
                                return;
                        }
                        uint32_t sp = 0;
                        stack[sp++] = int32_t(root);
                        while (sp > 0) {
                                uint32_t u = uint32_t(stack[--sp]);
                                if (seen[u] != 0) {
                                        xs[u] = next;
                                        next += sibling;
                                }
                                // Children pushed last first, so the first is met first.
                                for (uint32_t i = tail; i-- > 0;) {
                                        if (parent[order[i]] == int32_t(u)) stack[sp++] = order[i];
                                }
                        }
                        free(stack);
                        for (int32_t d = deepest; d >= 0; d--) {
                                for (uint32_t i = 0; i < tail; i++) {
                                        uint32_t u = uint32_t(order[i]);
                                        if (depth[u] != d || seen[u] != 0) continue;
                                        float lo = 0, hi = 0;
                                        bool  any = false;
                                        for (uint32_t k = 0; k < tail; k++) {
                                                uint32_t v = uint32_t(order[k]);
                                                if (parent[v] != int32_t(u)) continue;
                                                lo = any && lo < xs[v] ? lo : xs[v];
                                                hi = any && hi > xs[v] ? hi : xs[v];
                                                any = true;
                                        }
                                        xs[u] = (lo + hi) / 2;
                                }
                        }
                        for (uint32_t i = 0; i < n; i++) {
                                if (depth[i] < 0) {
                                        // Not reached from the root: a row under the tree.
                                        xs[i] = next;
                                        next += sibling;
                                        ys[i] = float(deepest + 1) * level;
                                } else {
                                        ys[i] = float(depth[i]) * level;
                                }
                        }
                        placeNodes(g, xs, ys);
                        free(mem);
                }
                void graphVisit(asIScriptGeneric *gen) {
                        // The node's circle fills in the highlight (the auxiliary: back to its fill).
                        Shape *g = self(gen);
                        Shape *n = graphNode(g, gen->GetArgDWord(0));
                        if (n == nullptr) return;
                        bool back = gen->GetAuxiliary() != nullptr;
                        returnAnimation(gen, fillAnimation(kid(n, 0), color(back ? "Script.Fill" : "Script.Highlight"),
                                                           1, kid(n, 1)));
                }
                Animation *edgeAnimation(Shape *g, uint32_t a, uint32_t b) {
                        for (uint32_t k = 0; k < g->columns; k++) {
                                uint32_t ea = g->edges[k * 3], eb = g->edges[k * 3 + 1];
                                if (!((ea == a && eb == b) || (ea == b && eb == a))) continue;
                                Animation *m = Animation::make(kid(g, k));
                                if (m != nullptr) {
                                        m->changeColor(Property::StrokeColor, color("Script.Highlight"));
                                        m->change(Property::LineWidth, 4);
                                }
                                return m;
                        }
                        raise("No edge between %u and %u", unsigned(a), unsigned(b));
                        return nullptr;
                }
                void graphHighlightEdge(asIScriptGeneric *gen) {
                        returnAnimation(gen, edgeAnimation(self(gen), gen->GetArgDWord(0), gen->GetArgDWord(1)));
                }
                void graphHighlightPath(asIScriptGeneric *gen) {
                        // A node, the edge to the next, the next...: an array to play (with a lag).
                        Shape              *g = self(gen);
                        const CScriptArray *path = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        asITypeInfo        *type = gen->GetEngine()->GetTypeInfoByDecl("array<SceneAnimation@>");
                        CScriptArray       *out = type != nullptr ? CScriptArray::Create(type) : nullptr;
                        if (out == nullptr) {
                                raise("Out of memory");
                                return;
                        }
                        uint32_t n = path != nullptr ? path->GetSize() : 0;
                        for (uint32_t i = 0; i < n; i++) {
                                uint32_t u = uint32_t(*static_cast<const int32_t *>(path->At(i)));
                                Shape   *node = graphNode(g, u);
                                if (node == nullptr) break;
                                if (i > 0) {
                                        Animation *e = edgeAnimation(
                                                g, uint32_t(*static_cast<const int32_t *>(path->At(i - 1))), u);
                                        if (e == nullptr) break;
                                        out->InsertLast(&e);
                                        e->release();
                                }
                                Animation *v = fillAnimation(kid(node, 0), color("Script.Highlight"), 1, kid(node, 1));
                                if (v == nullptr) break;
                                out->InsertLast(&v);
                                v->release();
                        }
                        gen->SetReturnAddress(out);
                }

                // ---- 3. Code: lines in focus, and changes

                void codeHighlight(asIScriptGeneric *gen) {
                        Shape              *s = self(gen);
                        const CScriptArray *lines = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        uint64_t            mask = 0;
                        for (uint32_t i = 0; lines != nullptr && i < lines->GetSize(); i++) {
                                int32_t line = *static_cast<const int32_t *>(lines->At(i));
                                if (line >= 1 && line <= 64) mask |= uint64_t(1) << (line - 1);
                        }
                        Animation *a = Animation::make(s);
                        if (a != nullptr) {
                                a->change(Property::Dim, mask != 0 ? 0.6f : 0);
                                a->then = Animation::Then::Focus;
                                a->mask = mask;
                        }
                        returnAnimation(gen, a);
                }
                uint32_t linesOf(const char *text, uint32_t length, StringView *out, uint32_t max) {
                        uint32_t n = 0, start = 0;
                        for (uint32_t i = 0; i <= length && n < max; i++) {
                                if (i < length && text[i] != '\n') continue;
                                uint32_t end = i;
                                if (end > start && text[end - 1] == '\r') end--;
                                if (i == length && start == length && n > 0) break; // no line after the last \n
                                out[n++] = StringView(text + start, end - start);
                                start = i + 1;
                        }
                        return n;
                }
                void codeLineCount(asIScriptGeneric *gen) {
                        Shape     *s = self(gen);
                        StringView lines[256];
                        gen->SetReturnDWord(s->text != nullptr ? linesOf(s->text, s->textLength, lines, 256) : 0);
                }
                void codeTransformTo(asIScriptGeneric *gen) {
                        // Lines kept slide to their new places, lines gone fade out, new ones fade in;
                        // each a line of its own while it moves (the code shows its box meanwhile).
                        Shape        *s = self(gen);
                        const String &next = argString(gen, 0);
                        SceneContext  context;
                        if (!contextNow(&context) || context.fonts == nullptr) {
                                raise("transformTo(): no fonts here");
                                return;
                        }
                        constexpr uint32_t Max = 128;
                        StringView         was[Max], now[Max];
                        uint32_t           n = s->text != nullptr ? linesOf(s->text, s->textLength, was, Max) : 0;
                        uint32_t           m = linesOf(next.data(), next.length(), now, Max);
                        // Lines kept: the longest common subsequence, by their text.
                        uint16_t *table = static_cast<uint16_t *>(calloc(size_t(n + 1) * (m + 1), sizeof(uint16_t)));
                        int32_t   match[Max], matched[Max];
                        Shape    *after = s->clone();
                        if (table == nullptr || after == nullptr || !after->setText(next.data(), next.length())) {
                                free(table);
                                if (after != nullptr) after->release();
                                raise("Out of memory for the code");
                                return;
                        }
                        for (uint32_t i = n; i-- > 0;) {
                                for (uint32_t j = m; j-- > 0;) {
                                        uint16_t &t = table[i * (m + 1) + j];
                                        if (was[i].equals(now[j])) {
                                                t = uint16_t(table[(i + 1) * (m + 1) + j + 1] + 1);
                                        } else {
                                                uint16_t down = table[(i + 1) * (m + 1) + j],
                                                         right = table[i * (m + 1) + j + 1];
                                                t = down > right ? down : right;
                                        }
                                }
                        }
                        for (uint32_t i = 0; i < Max; i++) match[i] = matched[i] = -1;
                        for (uint32_t i = 0, j = 0; i < n && j < m;) {
                                if (was[i].equals(now[j])) {
                                        match[i] = int32_t(j);
                                        matched[j] = int32_t(i);
                                        i++;
                                        j++;
                                } else if (table[(i + 1) * (m + 1) + j] >= table[i * (m + 1) + j + 1]) {
                                        i++;
                                } else {
                                        j++;
                                }
                        }
                        free(table);
                        float ow = 0, oh = 0, nw = 0, nh = 0;
                        s->measureText(context, &ow, &oh);
                        after->measureText(context, &nw, &nh);
                        FontKey     key = s->font != nullptr ? s->font->key() : sceneFont(theme().code.font);
                        FontMetrics fm = context.fonts->metrics(rasterFont(context, key));
                        float       lh = float(fm.lineHeight) / context.fit * s->scale, k = s->scale;
                        float       oldLeft = s->x - ow / 2 * k, oldTop = s->y - oh / 2 * k;
                        float       newLeft = s->x - nw / 2 * k, newTop = s->y - nh / 2 * k;
                        Animation  *all = whole(s);
                        if (all == nullptr) {
                                after->release();
                                return;
                        }
                        all->then = Animation::Then::NewCode;
                        all->other = after;
                        // A line of its own, at its place in a box whose top left is (left, top).
                        auto line = [&](StringView text, float left, float top, uint32_t row) -> Shape * {
                                Shape *l = s->clone();
                                if (l == nullptr || !l->setText(text.data(), text.length())) {
                                        if (l != nullptr) l->release();
                                        return nullptr;
                                }
                                l->fillOpacity = 0;
                                l->lineWidth = 0;
                                l->focus = 0;
                                l->dim = 0;
                                l->reveal = 1;
                                float w = 0, h = 0;
                                l->measureText(context, &w, &h);
                                l->x = left + w / 2 * k;
                                l->y = top + float(row) * lh + h / 2 * k;
                                return l;
                        };
                        for (uint32_t i = 0; i < n; i++) {
                                Shape *l = line(was[i], oldLeft, oldTop, i);
                                if (l == nullptr) continue;
                                Animation *a = Animation::make(l);
                                if (a != nullptr) {
                                        if (match[i] >= 0) {
                                                Shape *moved = line(was[i], newLeft, newTop, uint32_t(match[i]));
                                                if (moved != nullptr) {
                                                        a->change(Property::X, moved->x);
                                                        a->change(Property::Y, moved->y);
                                                        moved->release();
                                                }
                                        } else {
                                                a->change(Property::Opacity, 0);
                                        }
                                        a->adds = true;
                                        a->then = Animation::Then::Remove;
                                }
                                l->release();
                                joined(all, a);
                        }
                        for (uint32_t j = 0; j < m; j++) {
                                if (matched[j] >= 0) continue;
                                Shape *l = line(now[j], newLeft, newTop, j);
                                if (l == nullptr) continue;
                                Animation *a = Animation::make(l);
                                if (a != nullptr) {
                                        a->change(Property::Opacity, 0, true);
                                        a->from(0);
                                        a->adds = true;
                                        a->then = Animation::Then::Remove;
                                }
                                l->release();
                                joined(all, a);
                        }
                        returnAnimation(gen, all);
                }

                // ---- 4. Braces, callouts, labels

                Box targetBox(asIScriptGeneric *gen, asUINT n, bool *ok) {
                        Arg<Shape> target(gen, n);
                        *ok = static_cast<bool>(target);
                        if (!*ok) {
                                raise("Beside null");
                                return Box{};
                        }
                        return boxOf(target.get());
                }
                // Where beside a box on @p side, @p gap off it: the middle of that side, moved out.
                Point besideBox(const Box &b, uint32_t side, float gap) {
                        switch (side) {
                                case 0: return Point{b.left - gap, b.cy()};
                                case 1: return Point{b.right + gap, b.cy()};
                                case 2: return Point{b.cx(), b.top - gap};
                                default: return Point{b.cx(), b.bottom + gap};
                        }
                }
                void makeBrace(asIScriptGeneric *gen) {
                        bool     ok = false;
                        Box      b = targetBox(gen, 0, &ok);
                        uint32_t side = gen->GetArgDWord(1);
                        if (!ok) return;
                        const String &caption = argString(gen, 2);
                        bool          across = side >= 2;
                        float         length = across ? b.right - b.left : b.bottom - b.top, d = 18;
                        Point         at = besideBox(b, side, 6);
                        Shape        *brace = newShape(ShapeKind::Brace, color("Script.Line"));
                        Shape        *curve = newShape(ShapeKind::Curve, color("Script.Line"));
                        if (brace == nullptr || curve == nullptr) {
                                if (brace != nullptr) brace->release();
                                if (curve != nullptr) curve->release();
                                raise("Out of memory for a brace");
                                return;
                        }
                        brace->x = at.x;
                        brace->y = at.y;
                        // Along its side from one end to the other, its point outward at the middle.
                        // Its ends curl in, its middle comes to a point (as a printed brace's).
                        float half = length / 2, q = d * 0.45f;
                        Point shape[9] = {{-half, 0}, {-half + q, q},        {-d * 1.2f, q}, {-d * 0.35f, q * 1.1f},
                                          {0, d},     {d * 0.35f, q * 1.1f}, {d * 1.2f, q},  {half - q, q},
                                          {half, 0}};
                        float out = side == 0 || side == 2 ? -1.0f : 1.0f;
                        for (Point &p : shape) {
                                p.y *= out;
                                if (!across) p = Point{p.y, p.x};
                        }
                        curve->lineWidth = 2.5f;
                        ok = curve->setPoints(shape, 9);
                        ok = adopt(brace, curve) && ok;
                        if (ok && caption.length() > 0) {
                                FontKey key = fontSized(sceneFont(theme().body.font), 24);
                                Shape  *t = textOf(StringView(caption.data(), caption.length()), &key,
                                                   theme().body.color, 0, 0);
                                if (t != nullptr) {
                                        float        w = 0, h = 0;
                                        SceneContext context;
                                        if (contextNow(&context)) t->measureText(context, &w, &h);
                                        float reach = d + 6;
                                        t->x = !across ? out * (reach + w / 2) : 0;
                                        t->y = across ? out * (reach + h / 2) : 0;
                                }
                                ok = adopt(brace, t);
                        }
                        if (!ok) {
                                brace->release();
                                raise("Out of memory for a brace");
                                return;
                        }
                        returnShape(gen, brace);
                }
                // A composite's text: its last child, as a string.
                void lastText(asIScriptGeneric *gen) {
                        Shape *s = self(gen);
                        Shape *t = s->childCount > 0 ? s->children[s->childCount - 1] : nullptr;
                        bool   text = t != nullptr && t->kind == ShapeKind::Text;
                        new (gen->GetAddressOfReturnLocation())
                                String(text && t->text != nullptr ? t->text : "", text ? t->textLength : 0);
                }
                void setLastText(asIScriptGeneric *gen) {
                        Shape        *s = self(gen);
                        const String &v = argString(gen, 0);
                        Shape        *t = s->childCount > 0 ? s->children[s->childCount - 1] : nullptr;
                        if (t == nullptr || t->kind != ShapeKind::Text) return;
                        if (!t->setText(v.data(), v.length())) raise("Out of memory for text");
                }
                void makeCallout(asIScriptGeneric *gen) {
                        // Text in a rounded box beside the object, a pointer from the box to it.
                        const String &text = argString(gen, 0);
                        bool          ok = false;
                        Box           b = targetBox(gen, 1, &ok);
                        uint32_t      side = gen->GetArgDWord(2);
                        float         gap = gen->GetArgFloat(3);
                        if (!ok) return;
                        SceneContext context;
                        FontKey      key = fontSized(sceneFont(theme().body.font), 24);
                        Shape       *t = textOf(StringView(text.data(), text.length()), &key, theme().body.color, 0, 0);
                        float        w = 0, h = 0;
                        if (t != nullptr && contextNow(&context)) t->measureText(context, &w, &h);
                        float bw = w + 28, bh = h + 18;
                        Point near = besideBox(b, side, 0);
                        Point at = near;
                        switch (side) {
                                case 0: at.x -= gap + bw / 2; break;
                                case 1: at.x += gap + bw / 2; break;
                                case 2: at.y -= gap + bh / 2; break;
                                default: at.y += gap + bh / 2; break;
                        }
                        Shape *c = newShape(ShapeKind::Callout, color("Script.Line"));
                        Shape *box = newShape(ShapeKind::Rect, color("Script.Line"));
                        Shape *tip = newShape(ShapeKind::Polygon, color("Script.Line"));
                        Shape *cover = newShape(ShapeKind::Rect, color("Script.Fill"));
                        if (c == nullptr || box == nullptr || tip == nullptr || cover == nullptr || t == nullptr) {
                                Shape *made[] = {c, box, tip, cover, t};
                                for (Shape *x : made)
                                        if (x != nullptr) x->release();
                                raise("Out of memory for a callout");
                                return;
                        }
                        c->x = at.x;
                        c->y = at.y;
                        box->width = bw;
                        box->height = bh;
                        box->cornerRadius = 10;
                        box->fill = cover->fill = tip->fill = color("Script.Fill");
                        box->fillOpacity = cover->fillOpacity = tip->fillOpacity = 1;
                        box->lineWidth = tip->lineWidth = 2;
                        cover->lineWidth = 0;
                        // The pointer: its base on the box's edge facing the object, its point at it.
                        float tx = near.x - at.x, ty = near.y - at.y, base = 9;
                        Point pts[3];
                        if (side >= 2) {
                                float edge = side == 2 ? bh / 2 : -bh / 2;
                                pts[0] = Point{-base, edge - (side == 2 ? 1.0f : -1.0f)};
                                pts[1] = Point{tx, ty};
                                pts[2] = Point{base, pts[0].y};
                                cover->width = base * 2 - 2;
                                cover->height = 6;
                                cover->x = 0;
                                cover->y = edge + (side == 2 ? -3.0f : 3.0f);
                        } else {
                                float edge = side == 0 ? bw / 2 : -bw / 2;
                                pts[0] = Point{edge - (side == 0 ? 1.0f : -1.0f), -base};
                                pts[1] = Point{tx, ty};
                                pts[2] = Point{pts[0].x, base};
                                cover->width = 6;
                                cover->height = base * 2 - 2;
                                cover->x = edge + (side == 0 ? -3.0f : 3.0f);
                                cover->y = 0;
                        }
                        // Placed at the middle of its box; its points from there (as a polygon's).
                        float x0 = pts[0].x, x1 = x0, y0 = pts[0].y, y1 = y0;
                        for (const Point &p : pts) {
                                x0 = p.x < x0 ? p.x : x0;
                                x1 = p.x > x1 ? p.x : x1;
                                y0 = p.y < y0 ? p.y : y0;
                                y1 = p.y > y1 ? p.y : y1;
                        }
                        tip->x = (x0 + x1) / 2;
                        tip->y = (y0 + y1) / 2;
                        for (Point &p : pts) p = Point{p.x - tip->x, p.y - tip->y};
                        ok = tip->setPoints(pts, 3);
                        ok = adopt(c, tip) && ok;
                        ok = adopt(c, box) && ok;
                        ok = adopt(c, cover) && ok;
                        ok = adopt(c, t) && ok;
                        if (!ok) {
                                c->release();
                                raise("Out of memory for a callout");
                                return;
                        }
                        returnShape(gen, c);
                }
                void makeLabel(asIScriptGeneric *gen) {
                        // Text kept beside an object as it moves (the scene's settle()).
                        const String &text = argString(gen, 0);
                        Arg<Shape>    target(gen, 1);
                        if (!target) {
                                raise("A label beside null");
                                return;
                        }
                        Shape  *l = newShape(ShapeKind::Label, theme().body.color);
                        FontKey key = sceneFont(theme().body.font);
                        if (l == nullptr ||
                            !adopt(l, textOf(StringView(text.data(), text.length()), &key, theme().body.color, 0, 0))) {
                                if (l != nullptr) l->release();
                                raise("Out of memory for a label");
                                return;
                        }
                        target->addRef();
                        l->anchor = target.get();
                        l->anchorSide = uint8_t(gen->GetArgDWord(2));
                        l->anchorGap = gen->GetArgFloat(3);
                        SceneContext context;
                        if (contextNow(&context)) placeBeside(context, *l, *l->anchor, l->anchorSide, l->anchorGap);
                        returnShape(gen, l);
                }

                // ---- 5. Updaters and paths

                void follow(asIScriptGeneric *gen) {
                        Shape             *s = self(gen);
                        asIScriptFunction *f = nullptr;
                        memcpy(&f, gen->GetAddressOfArg(0), sizeof f);
                        if (s->updater != nullptr && s->releaseUpdater != nullptr) s->releaseUpdater(s->updater);
                        s->updater = f;
                        s->releaseUpdater = f != nullptr ? releaseCallback : nullptr;
                }
                void unfollow(asIScriptGeneric *gen) {
                        Shape *s = self(gen);
                        if (s->updater != nullptr && s->releaseUpdater != nullptr) s->releaseUpdater(s->updater);
                        s->updater = nullptr;
                        s->releaseUpdater = nullptr;
                }
                void animMoveAlong(asIScriptGeneric *gen) {
                        Arg<Shape> path(gen, 0);
                        if (!path) {
                                raise("moveAlong(null)");
                                return;
                        }
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Along, 1);
                                a->from(0);
                                a->index = gen->GetArgByte(1) != 0 ? 1 : 0;
                                path->addRef();
                                a->other = path.get();
                        }
                        returnAnimation(gen, a);
                }

                // ---- 6. Charts

                Color chartColor(uint32_t i) {
                        static constexpr Color Defaults[8] = {0x1A5FB4, 0xE66100, 0x26A269, 0x9141AC,
                                                              0xE5A50A, 0x1C71D8, 0xC01C28, 0x63452C};
                        char                   name[24];
                        snprintf(name, sizeof name, "Script.Chart%u", unsigned(i % 8 + 1));
                        Color c = i % 8 == 0   ? color("Script.Accent")
                                  : i % 8 == 1 ? color("Script.Highlight")
                                               : Defaults[i % 8];
                        (void)theme().scriptColor(StringView(name), &c);
                        return c;
                }
                // A chart's values (int or float) and labels; false (raised) if they don't fit.
                bool chartInput(asIScriptGeneric *gen, float *values, uint32_t *n, const CScriptArray **labels) {
                        if (!numbersOf(static_cast<const CScriptArray *>(gen->GetArgAddress(0)), values, 32, n))
                                return false;
                        *labels = static_cast<const CScriptArray *>(gen->GetArgAddress(1));
                        if (*n == 0) {
                                raise("A chart of no values");
                                return false;
                        }
                        return true;
                }
                StringView labelAt(const CScriptArray *labels, uint32_t i) {
                        if (labels == nullptr || i >= labels->GetSize()) return StringView();
                        const String *s = static_cast<const String *>(labels->At(i));
                        return StringView(s->data(), s->length());
                }
                bool wholeNumbers(const float *v, uint32_t n) {
                        for (uint32_t i = 0; i < n; i++)
                                if (v[i] != floorf(v[i])) return false;
                        return true;
                }
                Shape *numberOf(float v, int32_t decimals, const FontKey &key, float x, float y) {
                        Shape *t = newShape(ShapeKind::Number, theme().body.color);
                        if (t == nullptr) return nullptr;
                        t->font = ScriptFont::make(key);
                        t->decimals = decimals;
                        if (!t->setNumber(v)) {
                                t->release();
                                return nullptr;
                        }
                        t->x = x;
                        t->y = y;
                        return t;
                }
                // Bar charts: children a baseline, its bars, their labels, their values.
                void makeBarChart(asIScriptGeneric *gen) {
                        float               v[32];
                        uint32_t            n = 0;
                        const CScriptArray *labels = nullptr;
                        if (!chartInput(gen, v, &n, &labels)) return;
                        float w = gen->GetArgFloat(2), h = gen->GetArgFloat(3), most = 0;
                        for (uint32_t i = 0; i < n; i++) most = v[i] > most ? v[i] : most;
                        if (most <= 0) most = 1;
                        Shape *c = newShape(ShapeKind::BarChart, color("Script.Line"));
                        if (c == nullptr) {
                                raise("Out of memory for a chart");
                                return;
                        }
                        c->rows = n;
                        c->width = w;
                        c->height = h;
                        c->maximum = most;
                        Shape *base = newShape(ShapeKind::Line, color("Script.Line"));
                        if (base != nullptr) {
                                base->y = h / 2;
                                base->ends[0] = Point{-w / 2, 0};
                                base->ends[1] = Point{w / 2, 0};
                                base->lineWidth = 2;
                        }
                        bool    ok = adopt(c, base);
                        float   slot = w / float(n);
                        FontKey small = smallFont();
                        for (uint32_t i = 0; i < n && ok; i++) {
                                Shape *bar = newShape(ShapeKind::Rect, chartColor(i));
                                if (bar != nullptr) {
                                        bar->width = slot * 0.6f;
                                        bar->height = v[i] / most * h;
                                        bar->x = -w / 2 + (float(i) + 0.5f) * slot;
                                        bar->y = h / 2 - bar->height / 2;
                                        bar->fill = chartColor(i);
                                        bar->fillOpacity = 1;
                                        bar->lineWidth = 0;
                                }
                                ok = adopt(c, bar);
                        }
                        for (uint32_t i = 0; i < n && ok; i++)
                                ok = adopt(c, textOf(labelAt(labels, i), &small, theme().body.color,
                                                     -w / 2 + (float(i) + 0.5f) * slot, h / 2 + 16));
                        int32_t decimals = wholeNumbers(v, n) ? 0 : 1;
                        for (uint32_t i = 0; i < n && ok; i++)
                                ok = adopt(c, numberOf(v[i], decimals, small, -w / 2 + (float(i) + 0.5f) * slot,
                                                       h / 2 - v[i] / most * h - 14));
                        if (!ok) {
                                c->release();
                                raise("Out of memory for a chart");
                                return;
                        }
                        returnShape(gen, c);
                }
                // Pie charts: children its slices, then their labels.
                void pieLayout(Shape *c, const float *v, uint32_t n, float *start, float *sweep) {
                        float total = 0;
                        for (uint32_t i = 0; i < n; i++) total += v[i] > 0 ? v[i] : 0;
                        float at = -90;
                        for (uint32_t i = 0; i < n; i++) {
                                start[i] = at;
                                sweep[i] = total > 0 ? (v[i] > 0 ? v[i] : 0) / total * 360 : 0;
                                at += sweep[i];
                        }
                        (void)c;
                        return;
                }
                Point pieLabel(float radius, float start, float sweep) {
                        float a = (start + sweep / 2) * 3.14159265f / 180, r = radius + 30;
                        return Point{r * cosf(a), r * sinf(a)};
                }
                void makePieChart(asIScriptGeneric *gen) {
                        float               v[32] = {}, start[32] = {}, sweep[32] = {};
                        uint32_t            n = 0;
                        const CScriptArray *labels = nullptr;
                        if (!chartInput(gen, v, &n, &labels)) return;
                        float  radius = gen->GetArgFloat(2);
                        Shape *c = newShape(ShapeKind::PieChart, color("Script.Line"));
                        if (c == nullptr) {
                                raise("Out of memory for a chart");
                                return;
                        }
                        c->rows = n;
                        c->radius = radius;
                        pieLayout(c, v, n, start, sweep);
                        bool    ok = true;
                        FontKey small = smallFont();
                        for (uint32_t i = 0; i < n && ok; i++) {
                                Shape *slice = newShape(ShapeKind::Sector, theme().background);
                                if (slice != nullptr) {
                                        slice->radius = radius;
                                        slice->start = start[i];
                                        slice->sweep = sweep[i];
                                        slice->value = v[i];
                                        slice->fill = chartColor(i);
                                        slice->fillOpacity = 1;
                                        slice->lineWidth = 2;
                                }
                                ok = adopt(c, slice);
                        }
                        for (uint32_t i = 0; i < n && ok; i++) {
                                Point at = pieLabel(radius, start[i], sweep[i]);
                                ok = adopt(c, textOf(labelAt(labels, i), &small, theme().body.color, at.x, at.y));
                        }
                        if (!ok) {
                                c->release();
                                raise("Out of memory for a chart");
                                return;
                        }
                        returnShape(gen, c);
                }
                // Line charts: children its axes, its line, its dots, their labels.
                void makeLineChart(asIScriptGeneric *gen) {
                        float               v[32];
                        uint32_t            n = 0;
                        const CScriptArray *labels = nullptr;
                        if (!chartInput(gen, v, &n, &labels)) return;
                        float w = gen->GetArgFloat(2), h = gen->GetArgFloat(3), most = 0;
                        for (uint32_t i = 0; i < n; i++) most = v[i] > most ? v[i] : most;
                        if (most <= 0) most = 1;
                        Shape *c = newShape(ShapeKind::LineChart, color("Script.Line"));
                        Shape *axes = newShape(ShapeKind::Axes, color("Script.Line"));
                        if (c == nullptr || axes == nullptr) {
                                if (c != nullptr) c->release();
                                if (axes != nullptr) axes->release();
                                raise("Out of memory for a chart");
                                return;
                        }
                        c->rows = n;
                        c->width = w;
                        c->height = h;
                        c->maximum = most * 1.15f;
                        axes->width = w;
                        axes->height = h;
                        axes->xMin = -0.5f;
                        axes->xMax = float(n) - 0.5f;
                        axes->yMin = 0;
                        axes->yMax = c->maximum;
                        axes->xStep = float(n) * 10; // no ticks along x: the labels are its
                        axes->lineWidth = 2;
                        bool  ok = adopt(c, axes);
                        Point pts[32];
                        auto  px = [&](float i) {
                                return -w / 2 + (i + 0.5f) / float(n) * w;
                        };
                        auto py = [&](float y) {
                                return h / 2 - y / c->maximum * h;
                        };
                        for (uint32_t i = 0; i < n; i++) pts[i] = Point{px(float(i)), py(v[i])};
                        Shape *line = newShape(ShapeKind::Curve, chartColor(0));
                        if (line != nullptr) {
                                line->smooth = false;
                                ok = line->setPoints(pts, n) && ok;
                        }
                        ok = adopt(c, line) && ok;
                        for (uint32_t i = 0; i < n && ok; i++) {
                                Shape *dot = newShape(ShapeKind::Circle, chartColor(0));
                                if (dot != nullptr) {
                                        dot->radius = 5;
                                        dot->fill = chartColor(0);
                                        dot->fillOpacity = 1;
                                        dot->lineWidth = 0;
                                        dot->x = pts[i].x;
                                        dot->y = pts[i].y;
                                        dot->value = v[i];
                                }
                                ok = adopt(c, dot);
                        }
                        FontKey small = smallFont();
                        for (uint32_t i = 0; i < n && ok; i++)
                                ok = adopt(c, textOf(labelAt(labels, i), &small, theme().body.color, px(float(i)),
                                                     h / 2 + 16));
                        if (!ok) {
                                c->release();
                                raise("Out of memory for a chart");
                                return;
                        }
                        returnShape(gen, c);
                }
                bool chartIndex(Shape *c, uint32_t i) {
                        if (i < c->rows) return true;
                        raise("No value %u (there are %u)", unsigned(i), unsigned(c->rows));
                        return false;
                }
                void chartCount(asIScriptGeneric *gen) {
                        gen->SetReturnDWord(self(gen)->rows);
                }
                void chartValue(asIScriptGeneric *gen) {
                        Shape   *c = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        float    v = 0;
                        if (chartIndex(c, i)) {
                                if (c->kind == ShapeKind::BarChart) v = kid(c, 1 + 2 * c->rows + i)->value;
                                if (c->kind == ShapeKind::PieChart) v = kid(c, i)->value;
                                if (c->kind == ShapeKind::LineChart) v = kid(c, 2 + i)->value;
                        }
                        gen->SetReturnFloat(v);
                }
                void chartBuild(asIScriptGeneric *gen) {
                        // Bars from their base, slices round from their start, the line drawn along.
                        Shape     *c = self(gen);
                        Animation *all = whole(c);
                        uint32_t   n = c->rows;
                        for (uint32_t i = 0; i < n && all != nullptr; i++) {
                                if (c->kind == ShapeKind::BarChart) {
                                        Shape     *bar = kid(c, 1 + i);
                                        Animation *a = Animation::make(bar);
                                        if (a != nullptr) {
                                                a->change(Property::Height, bar->height);
                                                a->from(0);
                                                a->change(Property::Y, bar->y);
                                                a->from(c->height / 2);
                                        }
                                        joined(all, a);
                                        Animation *t = Animation::make(kid(c, 1 + 2 * n + i));
                                        if (t != nullptr) {
                                                t->change(Property::Value, kid(c, 1 + 2 * n + i)->value);
                                                t->from(0);
                                        }
                                        joined(all, t);
                                } else if (c->kind == ShapeKind::PieChart) {
                                        Shape     *slice = kid(c, i);
                                        Animation *a = Animation::make(slice);
                                        if (a != nullptr) {
                                                a->change(Property::Sweep, slice->sweep);
                                                a->from(0);
                                        }
                                        joined(all, a);
                                        Animation *t = Animation::make(kid(c, n + i));
                                        if (t != nullptr) {
                                                t->change(Property::Opacity, 0, true);
                                                t->from(0);
                                        }
                                        joined(all, t);
                                } else {
                                        Animation *d = Animation::make(kid(c, 2 + i));
                                        if (d != nullptr) {
                                                d->change(Property::Scale, 0, true);
                                                d->from(0);
                                        }
                                        joined(all, d);
                                }
                        }
                        if (c->kind == ShapeKind::LineChart && all != nullptr) {
                                Animation *a = Animation::make(kid(c, 1));
                                if (a != nullptr) {
                                        a->change(Property::Reveal, 1);
                                        a->from(0);
                                }
                                joined(all, a);
                        }
                        returnAnimation(gen, all);
                }
                void chartSetValue(asIScriptGeneric *gen) {
                        Shape   *c = self(gen);
                        uint32_t i = gen->GetArgDWord(0);
                        float    v = gen->GetArgFloat(1);
                        if (!chartIndex(c, i)) return;
                        Animation *all = whole(c);
                        uint32_t   n = c->rows;
                        if (all == nullptr) return;
                        if (c->kind == ShapeKind::BarChart) {
                                float      h = (v > 0 ? v : 0) / c->maximum * c->height;
                                Animation *a = Animation::make(kid(c, 1 + i));
                                if (a != nullptr) {
                                        a->change(Property::Height, h);
                                        a->change(Property::Y, c->height / 2 - h / 2);
                                }
                                joined(all, a);
                                Animation *t = Animation::make(kid(c, 1 + 2 * n + i));
                                if (t != nullptr) {
                                        t->change(Property::Value, v);
                                        t->change(Property::Y, c->height / 2 - h - 14);
                                }
                                joined(all, t);
                        } else if (c->kind == ShapeKind::PieChart) {
                                float values[32] = {}, start[32] = {}, sweep[32] = {};
                                for (uint32_t k = 0; k < n; k++) values[k] = k == i ? v : kid(c, k)->value;
                                kid(c, i)->value = v;
                                pieLayout(c, values, n, start, sweep);
                                for (uint32_t k = 0; k < n; k++) {
                                        Animation *a = Animation::make(kid(c, k));
                                        if (a != nullptr) {
                                                a->change(Property::Start, start[k]);
                                                a->change(Property::Sweep, sweep[k]);
                                        }
                                        joined(all, a);
                                        Point      at = pieLabel(c->radius, start[k], sweep[k]);
                                        Animation *l = Animation::make(kid(c, n + k));
                                        if (l != nullptr) {
                                                l->change(Property::X, at.x);
                                                l->change(Property::Y, at.y);
                                        }
                                        joined(all, l);
                                }
                        } else {
                                // The dot moves; the line turns into the line through the new values.
                                Shape *dot = kid(c, 2 + i), *line = kid(c, 1);
                                dot->value = v;
                                float      y = c->height / 2 - v / c->maximum * c->height;
                                Animation *a = Animation::make(dot);
                                if (a != nullptr) a->change(Property::Y, y);
                                joined(all, a);
                                Shape *to = line->clone();
                                if (to != nullptr && i < to->pointCount) {
                                        to->points[i].y = y;
                                        Animation *m = Animation::make(line);
                                        if (m != nullptr) {
                                                m->change(Property::Morph, 1);
                                                m->from(0);
                                                m->other = to;
                                                m->then = Animation::Then::Become;
                                                to = nullptr;
                                        }
                                        joined(all, m);
                                }
                                if (to != nullptr) to->release();
                        }
                        returnAnimation(gen, all);
                }

                // ---- 7. The camera

                Shape *cameraHere() {
                        AngelRegion *r = sceneRegion();
                        return r != nullptr ? r->camera() : nullptr;
                }
                template <float Shape::*M> void cameraGet(asIScriptGeneric *gen) {
                        // Before a region is (angel before its stage): as it starts.
                        AngelRegion *r = region();
                        if (r != nullptr) {
                                gen->SetReturnFloat(r->camera()->*M);
                                return;
                        }
                        int32_t w, h;
                        sceneSize(&w, &h);
                        gen->SetReturnFloat(M == &Shape::x ? float(w) / 2 : M == &Shape::y ? float(h) / 2 : 1.0f);
                }
                template <float Shape::*M> void cameraSet(asIScriptGeneric *gen) {
                        Shape *c = cameraHere();
                        if (c != nullptr) c->*M = gen->GetArgFloat(0);
                }
                void returnCamera(asIScriptGeneric *gen, Animation *a) {
                        returnAnimation(gen, a);
                }
                void cameraPan(asIScriptGeneric *gen) {
                        Shape *c = cameraHere();
                        if (c == nullptr) return;
                        Animation *a = Animation::make(c);
                        if (a != nullptr) {
                                a->change(Property::X, gen->GetArgFloat(0));
                                a->change(Property::Y, gen->GetArgFloat(1));
                        }
                        returnCamera(gen, a);
                }
                void cameraZoomTo(asIScriptGeneric *gen) {
                        Arg<Shape> target(gen, 0);
                        float      margin = gen->GetArgFloat(1);
                        Shape     *c = target ? cameraHere() : nullptr;
                        if (!target) raise("zoomTo(null)");
                        if (c == nullptr) return;
                        Box     b = boxOf(target.get());
                        int32_t w, h;
                        sceneSize(&w, &h);
                        float      bw = b.right - b.left + 2 * margin, bh = b.bottom - b.top + 2 * margin;
                        float      zx = bw > 0 ? float(w) / bw : 1, zy = bh > 0 ? float(h) / bh : 1;
                        Animation *a = Animation::make(c);
                        if (a != nullptr) {
                                a->change(Property::X, b.cx());
                                a->change(Property::Y, b.cy());
                                a->change(Property::Scale, zx < zy ? zx : zy);
                        }
                        returnCamera(gen, a);
                }
                void cameraZoomBy(asIScriptGeneric *gen) {
                        Shape *c = cameraHere();
                        if (c == nullptr) return;
                        Animation *a = Animation::make(c);
                        if (a != nullptr) a->change(Property::Scale, c->scale * gen->GetArgFloat(0));
                        returnCamera(gen, a);
                }
                void cameraReset(asIScriptGeneric *gen) {
                        Shape *c = cameraHere();
                        if (c == nullptr) return;
                        int32_t w, h;
                        sceneSize(&w, &h);
                        Animation *a = Animation::make(c);
                        if (a != nullptr) {
                                a->change(Property::X, float(w) / 2);
                                a->change(Property::Y, float(h) / 2);
                                a->change(Property::Scale, 1);
                                a->change(Property::Rotation, 0);
                        }
                        returnCamera(gen, a);
                }

                // ---- 8. Icons, marks, confetti

                void makeIcon(asIScriptGeneric *gen) {
                        const String &name = argString(gen, 0);
                        float         size = gen->GetArgFloat(1);
                        Shape        *s = newShape(ShapeKind::Icon, theme().body.color);
                        if (s != nullptr) {
                                s->width = s->height = size > 4 ? size : 4;
                                s->lineWidth = 0;
                                SceneHost *h = host();
                                uint32_t  *pixels = nullptr;
                                int32_t    side = 0;
                                if (h != nullptr &&
                                    h->platform().loadIcon(name.cStr(), int32_t(size), &pixels, &side) == Error::Ok) {
                                        s->iconPixels = pixels;
                                        s->iconSide = side;
                                }
                        }
                        returnShape(gen, s);
                }
                void makeMark(asIScriptGeneric *gen) {
                        uint32_t kind = gen->GetArgDWord(0);
                        float    size = gen->GetArgFloat(1);
                        Shape   *s = newShape(ShapeKind::Mark, kind == 0 ? 0x26A269 : 0xC01C28);
                        if (s != nullptr) {
                                s->sides = kind == 0 ? 0 : 1;
                                s->radius = size / 2;
                                s->lineWidth = size / 8 > 2 ? size / 8 : 2;
                        }
                        returnShape(gen, s);
                }
                void makeConfetti(asIScriptGeneric *gen) {
                        float   w = gen->GetArgFloat(0), h = gen->GetArgFloat(1);
                        int32_t sw, sh;
                        sceneSize(&sw, &sh);
                        Shape *s = newShape(ShapeKind::Confetti, 0);
                        if (s != nullptr) {
                                s->width = w > 0 ? w : float(sw);
                                s->height = h > 0 ? h : float(sh);
                                s->x = s->width / 2;
                                s->y = s->height / 2;
                                s->reveal = 0;
                                // Its pieces from the scene's numbers: the same each time it runs.
                                AngelRegion *r = region();
                                s->sides = 0x9E3779B9u * (r != nullptr ? r->objects().count() + 1 : 1) ^
                                           (r != nullptr ? uint32_t(r->place().index) * 7919u : 0);
                        }
                        returnShape(gen, s);
                }
                void confettiBurst(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::Reveal, 1);
                                a->from(0);
                                a->ease = Ease::Linear;
                                a->adds = true;
                                a->then = Animation::Then::Remove;
                        }
                        returnAnimation(gen, a);
                }

                // ---- Registering

                void check(int r, const char *what) {
                        angel::check(r, what);
                        return;
                }

                // The members every kind of object has, on type @p T: its properties, at(), and
                // the animations of it.
                void registerShapeMembers(asIScriptEngine *engine, const char *T) {
                        method(engine, T, "float get_x() const property", getFloat<&Shape::x>,
                               "Its centre's x, in the scene's pixels.");
                        method(engine, T, "void set_x(float) property", setFloat<&Shape::x>, nullptr);
                        method(engine, T, "float get_y() const property", getFloat<&Shape::y>,
                               "Its centre's y, in the scene's pixels (down).");
                        method(engine, T, "void set_y(float) property", setFloat<&Shape::y>, nullptr);
                        method(engine, T, "float get_scale() const property", getFloat<&Shape::scale>,
                               "Its size, 1 its own.");
                        method(engine, T, "void set_scale(float) property", setFloat<&Shape::scale>, nullptr);
                        method(engine, T, "float get_rotation() const property", getFloat<&Shape::rotation>,
                               "Turned so many degrees, clockwise, round its centre.");
                        method(engine, T, "void set_rotation(float) property", setFloat<&Shape::rotation>, nullptr);
                        method(engine, T, "float get_opacity() const property", getFloat<&Shape::opacity>,
                               "How opaque it is: 0 not seen, 1 wholly.");
                        method(engine, T, "void set_opacity(float) property", setFloat<&Shape::opacity>, nullptr);
                        method(engine, T, "bool get_visible() const property", getVisible, "Whether it is drawn.");
                        method(engine, T, "void set_visible(bool) property", setVisible, nullptr);
                        method(engine, T, "uint get_color() const property", getColor<&Shape::color>,
                               "Its lines' and text's colour (0xRRGGBB).");
                        method(engine, T, "void set_color(uint) property", setColor, nullptr);
                        method(engine, T, "uint get_fill() const property", getColor<&Shape::fill>,
                               "Its inside's colour (a closed shape's): set, it is filled.");
                        method(engine, T, "void set_fill(uint) property", setFill, nullptr);
                        method(engine, T, "float get_fillOpacity() const property", getFloat<&Shape::fillOpacity>,
                               "How opaque its inside is (0: not filled).");
                        method(engine, T, "void set_fillOpacity(float) property", setFloat<&Shape::fillOpacity>,
                               nullptr);
                        method(engine, T, "float get_lineWidth() const property", getFloat<&Shape::lineWidth>,
                               "Its lines' width in pixels (0: none).");
                        method(engine, T, "void set_lineWidth(float) property", setFloat<&Shape::lineWidth>, nullptr);
                        char decl[64];
                        snprintf(decl, sizeof decl, "%s@ at(float x, float y)", T);
                        method(engine, T, decl, shapeAt, "Placed with its centre at (x, y); itself, to go on with.");
                        // Its animations, for scene.play() or scene.start()
                        method(engine, T, "SceneAnimation@ move(float x, float y)", animMove, "Moves it to (x, y).");
                        method(engine, T, "SceneAnimation@ shift(float dx, float dy)", animShift,
                               "Moves it by (dx, dy).");
                        method(engine, T, "SceneAnimation@ fadeIn()", animFadeIn, "Brings it in, from transparent.");
                        method(engine, T, "SceneAnimation@ fadeOut()", animFadeOut, "Fades it away, and takes it out.");
                        method(engine, T, "SceneAnimation@ grow()", animGrow, "Brings it in, growing from nothing.");
                        method(engine, T, "SceneAnimation@ shrink()", animShrink,
                               "Shrinks it to nothing, and takes it out.");
                        method(engine, T, "SceneAnimation@ write()", animWrite,
                               "Brings it in as if drawn: text a letter at a time, a shape round its outline.");
                        method(engine, T, "SceneAnimation@ transform(SceneObject@ into)", animTransform,
                               "Turns it into another's shape, colour and place (it stays the object it was).");
                        method(engine, T, "SceneAnimation@ colorTo(uint color)", animColorTo, "Its colour to another.");
                        method(engine, T, "SceneAnimation@ fillTo(uint color)", animFillTo, "Its inside to a colour.");
                        method(engine, T, "SceneAnimation@ scaleTo(float scale)", animScaleTo, "Its scale to another.");
                        method(engine, T, "SceneAnimation@ rotateTo(float degrees)", animRotateTo,
                               "Turns it to so many degrees.");
                        method(engine, T, "SceneAnimation@ tween(const string &in property, float to)", animTween,
                               "A property to a value: x, y, scale, rotation, opacity, lineWidth, fillOpacity, width, "
                               "height, radius, reveal, cornerRadius, start, sweep, innerRadius, bend, value, dash, "
                               "headSize.");
                        // Step 13 (§8d)
                        method(engine, T, "float get_dash() const property", getFloat<&Shape::dash>,
                               "Its lines in dashes so long (0: whole).");
                        method(engine, T, "void set_dash(float) property", setFloat<&Shape::dash>, nullptr);
                        method(engine, T, "float get_dashGap() const property", getFloat<&Shape::dashGap>,
                               "Between its dashes (0: as long as a dash).");
                        method(engine, T, "void set_dashGap(float) property", setFloat<&Shape::dashGap>, nullptr);
                        method(engine, T, "float get_left() const property", boxLeft,
                               "Its box's left edge, in its scene's (or group's) pixels, as drawn.");
                        method(engine, T, "float get_right() const property", boxRight, "Its box's right edge.");
                        method(engine, T, "float get_top() const property", boxTop, "Its box's top edge.");
                        method(engine, T, "float get_bottom() const property", boxBottom, "Its box's bottom edge.");
                        snprintf(decl, sizeof decl, "%s@ copy() const", T);
                        method(engine, T, decl, copyShape,
                               "A new object like it (a group's objects copied too), not in the scene.");
                        char nextDecl[96];
                        snprintf(nextDecl, sizeof nextDecl, "%s@ nextTo(SceneObject@ other, Side side, float gap = 16)",
                                 T);
                        method(engine, T, nextDecl, nextTo,
                               "Moved beside another object's box, gap apart, centred on it; itself.");
                        method(engine, T, "SceneAnimation@ fadeTo(float opacity)", animFadeTo,
                               "Its opacity to another.");
                        method(engine, T, "SceneAnimation@ rotateBy(float degrees)", animRotateBy,
                               "Turns it so many degrees more (clockwise).");
                        method(engine, T, "SceneAnimation@ scaleBy(float factor)", animScaleBy,
                               "Its size times a factor.");
                        method(engine, T, "SceneAnimation@ indicate()", animIndicate,
                               "A pulse: a little bigger in the highlight's colour, and back.");
                        method(engine, T, "SceneAnimation@ wiggle()", animWiggle, "Wiggles it, and back.");
                        // Step 14 (§8e)
                        method(engine, T, "void follow(Updater@ updater)", follow,
                               "Calls a function for it before each frame (null: none): it can follow another.");
                        method(engine, T, "void unfollow()", unfollow, "No function called for it any more.");
                        method(engine, T, "SceneAnimation@ moveAlong(SceneObject@ path, bool turn = false)",
                               animMoveAlong,
                               "Moves it along another object's path or outline; turn: facing the way it goes.");
                        return;
                }

                // A line's, an arrow's, an arc's or a curve's heads, on type @p T.
                void registerHeads(asIScriptEngine *engine, const char *T) {
                        method(engine, T, "ArrowHead get_head() const property", getHead<&Shape::head>,
                               "What its end has (ArrowHead::None, Triangle, Stealth, Open, Circle, Square, Diamond, "
                               "Bar).");
                        method(engine, T, "void set_head(ArrowHead) property", setHead<&Shape::head>, nullptr);
                        method(engine, T, "ArrowHead get_tail() const property", getHead<&Shape::tail>,
                               "What its start has.");
                        method(engine, T, "void set_tail(ArrowHead) property", setHead<&Shape::tail>, nullptr);
                        method(engine, T, "float get_headSize() const property", getFloat<&Shape::headSize>,
                               "Its heads' length (0: from its line's width).");
                        method(engine, T, "void set_headSize(float) property", setFloat<&Shape::headSize>, nullptr);
                        return;
                }

                struct Kind {
                                const char *name;
                                ShapeKind   kind;
                };
                constexpr Kind kinds[] = {
                        {"SceneText", ShapeKind::Text},           {"SceneRect", ShapeKind::Rect},
                        {"SceneCircle", ShapeKind::Circle},       {"SceneLine", ShapeKind::Line},
                        {"SceneArrow", ShapeKind::Arrow},         {"ScenePolygon", ShapeKind::Polygon},
                        {"SceneImage", ShapeKind::Image},         {"SceneCode", ShapeKind::Code},
                        {"SceneGroup", ShapeKind::Group},         {"SceneBars", ShapeKind::Bars},
                        {"SceneGrid", ShapeKind::Grid},           {"SceneSlider", ShapeKind::Slider},
                        {"SceneButton", ShapeKind::Button},       {"SceneToggle", ShapeKind::Toggle},
                        {"SceneEllipse", ShapeKind::Ellipse},     {"SceneArc", ShapeKind::Arc},
                        {"SceneSector", ShapeKind::Sector},       {"SceneRegular", ShapeKind::Regular},
                        {"SceneStar", ShapeKind::Star},           {"SceneCurve", ShapeKind::Curve},
                        {"SceneNumber", ShapeKind::Number},       {"SceneAxes", ShapeKind::Axes},
                        {"SceneArray", ShapeKind::Array},         {"SceneTable", ShapeKind::Table},
                        {"SceneGraph", ShapeKind::Graph},         {"SceneBrace", ShapeKind::Brace},
                        {"SceneCallout", ShapeKind::Callout},     {"SceneLabel", ShapeKind::Label},
                        {"SceneBarChart", ShapeKind::BarChart},   {"ScenePieChart", ShapeKind::PieChart},
                        {"SceneLineChart", ShapeKind::LineChart}, {"SceneIcon", ShapeKind::Icon},
                        {"SceneMark", ShapeKind::Mark},           {"SceneConfetti", ShapeKind::Confetti},
                };

                // A deck's own Font, Event, Sound and Callback (angel has its own of each).
                void registerDeckTypes(asIScriptEngine *engine) {
                        check(engine->RegisterObjectType("Font", 0, asOBJ_REF), "Font");
                        check(engine->RegisterObjectType("Event", 0, asOBJ_REF), "Event");
                        check(engine->RegisterObjectType("Sound", 0, asOBJ_REF), "Sound");
                        check(engine->RegisterFuncdef("void Callback()"), "Callback");
                        check(engine->RegisterEnum("EventType"), "EventType");
                        static constexpr struct {
                                        const char *name;
                                        int         value;
                        } eventTypes[] = {{"KeyDown", 0},   {"KeyUp", 1},   {"MouseMove", 2},
                                          {"MouseDown", 3}, {"MouseUp", 4}, {"Wheel", 5}};
                        for (const auto &t : eventTypes)
                                check(engine->RegisterEnumValue("EventType", t.name, t.value), t.name);

                        // Fonts
                        behaviour(
                                engine, "Font", asBEHAVE_FACTORY, "Font@ f(const string &in name)", fontByName,
                                "A font as themes write one: \"IBM Plex Sans 40 bold\", \"unscii 32\" (what isn't said "
                                "is the theme's text's).");
                        behaviour(engine, "Font", asBEHAVE_ADDREF, "void f()", fontAddRef, nullptr);
                        behaviour(engine, "Font", asBEHAVE_RELEASE, "void f()", fontRelease, nullptr);
                        method(engine, "Font", "Font@ sized(int pixels) const", fontSized,
                               "The same font at another size.");
                        method(engine, "Font", "int get_size() const property", fontSize, "Its size in pixels.");

                        // Events
                        behaviour(engine, "Event", asBEHAVE_ADDREF, "void f()", eventAddRef, nullptr);
                        behaviour(engine, "Event", asBEHAVE_RELEASE, "void f()", eventRelease, nullptr);
                        check(engine->RegisterObjectProperty("Event", "const EventType type",
                                                             asOFFSET(ScriptEvent, type)),
                              "Event::type");
                        check(engine->RegisterObjectProperty("Event", "const string key", asOFFSET(ScriptEvent, key)),
                              "Event::key");
                        check(engine->RegisterObjectProperty("Event", "const string text", asOFFSET(ScriptEvent, text)),
                              "Event::text");
                        check(engine->RegisterObjectProperty("Event", "const int x", asOFFSET(ScriptEvent, x)),
                              "Event::x");
                        check(engine->RegisterObjectProperty("Event", "const int y", asOFFSET(ScriptEvent, y)),
                              "Event::y");
                        check(engine->RegisterObjectProperty("Event", "const int button",
                                                             asOFFSET(ScriptEvent, button)),
                              "Event::button");
                        check(engine->RegisterObjectProperty("Event", "const int wheel", asOFFSET(ScriptEvent, wheel)),
                              "Event::wheel");
                        check(engine->RegisterObjectProperty("Event", "const bool repeat",
                                                             asOFFSET(ScriptEvent, repeat)),
                              "Event::repeat");

                        // Sound
                        behaviour(engine, "Sound", asBEHAVE_FACTORY, "Sound@ f(const string &in file)", makeSound,
                                  "A sound from the script's folder (WAV, MP3, Ogg, FLAC).");
                        behaviour(engine, "Sound", asBEHAVE_ADDREF, "void f()", soundAddRef, nullptr);
                        behaviour(engine, "Sound", asBEHAVE_RELEASE, "void f()", soundRelease, nullptr);
                        method(engine, "Sound", "void play(float volume = 1, float pan = 0)", soundPlay,
                               "Plays it (over what plays), when its slide is shown; never in fast-forward. A deck's "
                               "sounds are heard from the middle: the pan is angel's.");
                        return;
                }

                void registerScene(asIScriptEngine *engine) {
                        // Types first: their members name one another.
                        check(engine->RegisterObjectType("Scene", 0, asOBJ_REF | asOBJ_NOCOUNT), "Scene");
                        check(engine->RegisterObjectType("SceneTheme", 0, asOBJ_REF | asOBJ_NOCOUNT), "SceneTheme");
                        check(engine->RegisterObjectType("SceneObject", 0, asOBJ_REF), "SceneObject");
                        for (const Kind &k : kinds) check(engine->RegisterObjectType(k.name, 0, asOBJ_REF), k.name);
                        check(engine->RegisterObjectType("SceneAnimation", 0, asOBJ_REF), "SceneAnimation");
                        check(engine->RegisterFuncdef("void Updater(SceneObject@ self)"), "Updater");
                        check(engine->RegisterEnum("Ease"), "Ease");
                        check(engine->RegisterEnumValue("Ease", "Linear", int(Ease::Linear)), "Ease::Linear");
                        check(engine->RegisterEnumValue("Ease", "In", int(Ease::In)), "Ease::In");
                        check(engine->RegisterEnumValue("Ease", "Out", int(Ease::Out)), "Ease::Out");
                        check(engine->RegisterEnumValue("Ease", "InOut", int(Ease::InOut)), "Ease::InOut");
                        check(engine->RegisterEnumValue("Ease", "Bounce", int(Ease::Bounce)), "Ease::Bounce");
                        check(engine->RegisterEnumValue("Ease", "ThereAndBack", int(Ease::ThereAndBack)),
                              "Ease::ThereAndBack");
                        check(engine->RegisterEnumValue("Ease", "Elastic", int(Ease::Elastic)), "Ease::Elastic");
                        check(engine->RegisterEnumValue("Ease", "Wiggle", int(Ease::Wiggle)), "Ease::Wiggle");
                        check(engine->RegisterEnum("ArrowHead"), "ArrowHead");
                        static constexpr const char *heads[] = {"None",   "Triangle", "Stealth", "Open",
                                                                "Circle", "Square",   "Diamond", "Bar"};
                        for (uint32_t i = 0; i < 8; i++)
                                check(engine->RegisterEnumValue("ArrowHead", heads[i], int(i)), heads[i]);
                        check(engine->RegisterEnum("Side"), "Side");
                        static constexpr const char *sides[] = {"Left", "Right", "Above", "Below"};
                        for (uint32_t i = 0; i < 4; i++)
                                check(engine->RegisterEnumValue("Side", sides[i], int(i)), sides[i]);
                        check(engine->RegisterFuncdef("float PlotFunction(float x)"), "PlotFunction");
                        check(engine->RegisterEnum("Mark"), "Mark");
                        check(engine->RegisterEnumValue("Mark", "Check", 0), "Mark::Check");
                        check(engine->RegisterEnumValue("Mark", "Cross", 1), "Mark::Cross");

                        // The theme
                        method(engine, "SceneTheme", "uint color(const string &in name) const", themeColor,
                               "A colour of the theme: Text, Background, Accent, H1, H2, H3, Code, Link, Rule, or "
                               "Script.NAME from its [Script] section (Script.Accent, Highlight, Muted, Line and Fill "
                               "always there).");
                        method(engine, "SceneTheme", "Font@ font(const string &in name) const", themeFont,
                               "A font of the theme: Text, H1, H2, H3, Code, Small.");

                        // Animations
                        behaviour(engine, "SceneAnimation", asBEHAVE_ADDREF, "void f()", animAddRef, nullptr);
                        behaviour(engine, "SceneAnimation", asBEHAVE_RELEASE, "void f()", animRelease, nullptr);
                        method(engine, "SceneAnimation", "SceneAnimation@ ease(Ease ease)", animEase,
                               "How it goes (Ease::InOut unless said); itself, to go on with.");
                        method(engine, "SceneAnimation", "SceneAnimation@ delay(float seconds)", animDelay,
                               "Begins so long after the play does; itself.");
                        method(engine, "SceneAnimation", "SceneAnimation@ duration(float seconds)", animDuration,
                               "Takes so long, not the play's time; itself.");

                        // Objects
                        behaviour(engine, "SceneObject", asBEHAVE_ADDREF, "void f()", shapeAddRef, nullptr);
                        behaviour(engine, "SceneObject", asBEHAVE_RELEASE, "void f()", shapeRelease, nullptr);
                        registerShapeMembers(engine, "SceneObject");
                        for (const Kind &k : kinds) {
                                behaviour(engine, k.name, asBEHAVE_ADDREF, "void f()", shapeAddRef, nullptr);
                                behaviour(engine, k.name, asBEHAVE_RELEASE, "void f()", shapeRelease, nullptr);
                                registerShapeMembers(engine, k.name);
                                method(engine, k.name, "SceneObject@ opImplCast()", implicitCast, nullptr);
                                char decl[48];
                                snprintf(decl, sizeof decl, "%s@ opCast()", k.name);
                                angel::methodAux(engine, "SceneObject", decl, downCast,
                                                 reinterpret_cast<void *>(uintptr_t(k.kind)), nullptr);
                        }
                        behaviour(engine, "SceneText", asBEHAVE_FACTORY,
                                  "SceneText@ f(const ?&in text, Font@ font = null)", makeText,
                                  "Text (a value, as print() shows it; lines at \\n, centred), in a font (null: the "
                                  "theme's text's).");
                        method(engine, "SceneText", "string get_text() const property", textGet, "What it says.");
                        method(engine, "SceneText", "void set_text(const string &in) property", textSet, nullptr);
                        method(engine, "SceneText", "float get_width() const property", textWidth,
                               "Its width in pixels, before scaling.");
                        method(engine, "SceneText", "float get_height() const property", textHeight,
                               "Its height in pixels, before scaling.");
                        behaviour(engine, "SceneRect", asBEHAVE_FACTORY, "SceneRect@ f(float width, float height)",
                                  makeRect, "A rectangle, outlined in the theme's accent (set fill to fill it).");
                        method(engine, "SceneRect", "float get_width() const property", getFloat<&Shape::width>,
                               "Its width.");
                        method(engine, "SceneRect", "void set_width(float) property", setFloat<&Shape::width>, nullptr);
                        method(engine, "SceneRect", "float get_height() const property", getFloat<&Shape::height>,
                               "Its height.");
                        method(engine, "SceneRect", "void set_height(float) property", setFloat<&Shape::height>,
                               nullptr);
                        behaviour(engine, "SceneCircle", asBEHAVE_FACTORY, "SceneCircle@ f(float radius)", makeCircle,
                                  "A circle, outlined in the theme's accent (set fill to fill it).");
                        method(engine, "SceneCircle", "float get_radius() const property", getFloat<&Shape::radius>,
                               "Its radius.");
                        method(engine, "SceneCircle", "void set_radius(float) property", setFloat<&Shape::radius>,
                               nullptr);
                        angel::behaviourAux(engine, "SceneLine", asBEHAVE_FACTORY, "SceneLine@ f()", makeEmptyLine,
                                            nullptr, "A line to place by between(), or by its ends' properties.");
                        angel::behaviourAux(engine, "SceneArrow", asBEHAVE_FACTORY, "SceneArrow@ f()", makeEmptyLine,
                                            reinterpret_cast<void *>(uintptr_t(1)),
                                            "An arrow to place by between(), or by its ends' properties.");
                        angel::behaviourAux(engine, "SceneLine", asBEHAVE_FACTORY,
                                            "SceneLine@ f(float x0, float y0, float x1, float y1)", makeLine, nullptr,
                                            "A line from (x0, y0) to (x1, y1); placed at its middle.");
                        angel::behaviourAux(engine, "SceneArrow", asBEHAVE_FACTORY,
                                            "SceneArrow@ f(float x0, float y0, float x1, float y1)", makeLine,
                                            reinterpret_cast<void *>(uintptr_t(1)),
                                            "An arrow from (x0, y0), its head at (x1, y1); placed at its middle.");
                        behaviour(engine, "ScenePolygon", asBEHAVE_FACTORY,
                                  "ScenePolygon@ f(const array<float> &in xy)", makePolygon,
                                  "A polygon through points x0, y0, x1, y1...; placed at the middle of its box.");
                        behaviour(engine, "SceneImage", asBEHAVE_FACTORY, "SceneImage@ f(const string &in file)",
                                  makeImage, "A picture from the script's folder (PNG, JPEG, BMP), at its own size.");
                        behaviour(engine, "SceneImage", asBEHAVE_FACTORY,
                                  "SceneImage@ f(const string &in file, float width, float height)", makeImage,
                                  "A picture from the script's folder, at a size.");
                        method(engine, "SceneImage", "float get_width() const property", getFloat<&Shape::width>,
                               "Its width.");
                        method(engine, "SceneImage", "void set_width(float) property", setFloat<&Shape::width>,
                               nullptr);
                        method(engine, "SceneImage", "float get_height() const property", getFloat<&Shape::height>,
                               "Its height.");
                        method(engine, "SceneImage", "void set_height(float) property", setFloat<&Shape::height>,
                               nullptr);
                        behaviour(engine, "SceneCode", asBEHAVE_FACTORY,
                                  "SceneCode@ f(const string &in text, const string &in language = \"\")", makeCode,
                                  "Code in the theme's code font, coloured as the language's (angel, c, cpp...), on "
                                  "its box.");
                        behaviour(engine, "SceneGroup", asBEHAVE_FACTORY, "SceneGroup@ f()", makeGroup,
                                  "Objects moved, turned, scaled and faded together: theirs are from its centre.");
                        method(engine, "SceneGroup", "void add(SceneObject@ object)", groupAdd,
                               "Puts an object in it.");
                        method(engine, "SceneGroup", "void remove(SceneObject@ object)", groupRemove,
                               "Takes an object out of it.");
                        method(engine, "SceneGroup", "uint get_count() const property", groupCount,
                               "The objects in it.");
                        behaviour(engine, "SceneBars", asBEHAVE_FACTORY,
                                  "SceneBars@ f(const array<float> &in values, float width = 600, float height = 300)",
                                  makeBars, "Values as bars, side by side in width × height, the largest the height.");
                        method(engine, "SceneBars", "float value(int index) const", barsValue, "A bar's value.");
                        method(engine, "SceneBars", "uint get_count() const property", barsCount, "How many bars.");
                        method(engine, "SceneBars", "SceneAnimation@ swap(int a, int b)", barsSwap,
                               "Two bars change places.");
                        method(engine, "SceneBars", "SceneAnimation@ highlight(int index)", barsHighlight,
                               "A bar turns the theme's highlight (Script.Highlight).");
                        method(engine, "SceneBars", "SceneAnimation@ highlight(int index, uint color)", barsHighlight,
                               "A bar turns a colour.");
                        method(engine, "SceneBars", "SceneAnimation@ unhighlight(int index)", barsUnhighlight,
                               "A bar turns back to the accent (Script.Accent).");
                        method(engine, "SceneBars", "SceneAnimation@ set(int index, float value)", barsSet,
                               "A bar grows or shrinks to a value.");
                        behaviour(engine, "SceneGrid", asBEHAVE_FACTORY,
                                  "SceneGrid@ f(float width, float height, float step)", makeGrid,
                                  "Lines step apart over width × height, from its centre out.");

                        // Step 13 (§8d): lines' and arrows' heads and ends, the new shapes
                        const char *const lines[] = {"SceneLine", "SceneArrow"};
                        for (const char *L : lines) {
                                registerHeads(engine, L);
                                method(engine, L, "float get_bend() const property", getFloat<&Shape::bend>,
                                       "Degrees of an arc from its start to its end (0: straight; more: clockwise).");
                                method(engine, L, "void set_bend(float) property", setFloat<&Shape::bend>, nullptr);
                                angel::methodAux(engine, L, "float get_startX() const property", lineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(0)), "Its start's x.");
                                angel::methodAux(engine, L, "float get_startY() const property", lineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(1)), "Its start's y.");
                                angel::methodAux(engine, L, "float get_endX() const property", lineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(2)), "Its end's x.");
                                angel::methodAux(engine, L, "float get_endY() const property", lineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(3)), "Its end's y.");
                                angel::methodAux(engine, L, "void set_startX(float) property", setLineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(0)), nullptr);
                                angel::methodAux(engine, L, "void set_startY(float) property", setLineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(1)), nullptr);
                                angel::methodAux(engine, L, "void set_endX(float) property", setLineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(2)), nullptr);
                                angel::methodAux(engine, L, "void set_endY(float) property", setLineEnd,
                                                 reinterpret_cast<void *>(uintptr_t(3)), nullptr);
                                char between_[96];
                                snprintf(between_, sizeof between_,
                                         "%s@ between(SceneObject@ a, SceneObject@ b, float gap = 4)", L);
                                method(engine, L, between_, between,
                                       "From one object's box to another's, facing each other, gap off each; itself.");
                                method(engine, L, "SceneAnimation@ bendTo(float degrees)", animTo<Property::Bend>,
                                       "Its bend to another.");
                        }
                        method(engine, "SceneRect", "float get_cornerRadius() const property",
                               getFloat<&Shape::cornerRadius>, "Its corners' roundness (0: square).");
                        method(engine, "SceneRect", "void set_cornerRadius(float) property",
                               setFloat<&Shape::cornerRadius>, nullptr);
                        behaviour(engine, "SceneEllipse", asBEHAVE_FACTORY,
                                  "SceneEllipse@ f(float width, float height)", makeEllipse,
                                  "An ellipse width × height, outlined in the theme's accent.");
                        method(engine, "SceneEllipse", "float get_width() const property", getFloat<&Shape::width>,
                               "Its width.");
                        method(engine, "SceneEllipse", "void set_width(float) property", setFloat<&Shape::width>,
                               nullptr);
                        method(engine, "SceneEllipse", "float get_height() const property", getFloat<&Shape::height>,
                               "Its height.");
                        method(engine, "SceneEllipse", "void set_height(float) property", setFloat<&Shape::height>,
                               nullptr);
                        angel::behaviourAux(
                                engine, "SceneArc", asBEHAVE_FACTORY,
                                "SceneArc@ f(float radius, float start, float sweep)", makeArc, nullptr,
                                "Part of a circle's outline round its place: from start degrees, sweep more "
                                "(clockwise from 3 o'clock).");
                        angel::behaviourAux(engine, "SceneSector", asBEHAVE_FACTORY,
                                            "SceneSector@ f(float radius, float start, float sweep)", makeArc,
                                            reinterpret_cast<void *>(uintptr_t(1)),
                                            "A slice of a circle (a pie's), from start degrees, sweep more.");
                        const char *const arcs[] = {"SceneArc", "SceneSector"};
                        for (const char *A : arcs) {
                                method(engine, A, "float get_radius() const property", getFloat<&Shape::radius>,
                                       "Its radius.");
                                method(engine, A, "void set_radius(float) property", setFloat<&Shape::radius>, nullptr);
                                method(engine, A, "float get_start() const property", getFloat<&Shape::start>,
                                       "Where it starts: degrees clockwise from 3 o'clock.");
                                method(engine, A, "void set_start(float) property", setFloat<&Shape::start>, nullptr);
                                method(engine, A, "float get_sweep() const property", getFloat<&Shape::sweep>,
                                       "How far it goes round, degrees (negative: anticlockwise).");
                                method(engine, A, "void set_sweep(float) property", setFloat<&Shape::sweep>, nullptr);
                                method(engine, A, "SceneAnimation@ sweepTo(float degrees)", animTo<Property::Sweep>,
                                       "Its sweep to another (a pie filling, an arc drawn round).");
                        }
                        registerHeads(engine, "SceneArc");
                        method(engine, "SceneSector", "float get_innerRadius() const property",
                               getFloat<&Shape::innerRadius>, "A ring's slice: its hole's radius (0: none).");
                        method(engine, "SceneSector", "void set_innerRadius(float) property",
                               setFloat<&Shape::innerRadius>, nullptr);
                        angel::behaviourAux(engine, "SceneRegular", asBEHAVE_FACTORY,
                                            "SceneRegular@ f(uint sides, float radius)", makeRegular, nullptr,
                                            "A regular polygon, its corners radius from its centre, one at the top.");
                        method(engine, "SceneRegular", "uint get_sides() const property", sidesGet, "Its sides.");
                        method(engine, "SceneRegular", "void set_sides(uint) property", sidesSet, nullptr);
                        angel::behaviourAux(engine, "SceneStar", asBEHAVE_FACTORY,
                                            "SceneStar@ f(uint points, float radius, float innerRadius = 0)",
                                            makeRegular, reinterpret_cast<void *>(uintptr_t(1)),
                                            "A star of so many points, radius out, innerRadius in (0: half).");
                        method(engine, "SceneStar", "uint get_points() const property", sidesGet, "Its points.");
                        method(engine, "SceneStar", "void set_points(uint) property", sidesSet, nullptr);
                        method(engine, "SceneStar", "float get_innerRadius() const property",
                               getFloat<&Shape::innerRadius>, "Between its points, from its centre.");
                        method(engine, "SceneStar", "void set_innerRadius(float) property",
                               setFloat<&Shape::innerRadius>, nullptr);
                        const char *const regulars[] = {"SceneRegular", "SceneStar"};
                        for (const char *R : regulars) {
                                method(engine, R, "float get_radius() const property", getFloat<&Shape::radius>,
                                       "Its corners' distance from its centre.");
                                method(engine, R, "void set_radius(float) property", setFloat<&Shape::radius>, nullptr);
                        }
                        behaviour(engine, "SceneCurve", asBEHAVE_FACTORY,
                                  "SceneCurve@ f(const array<float> &in xy, bool closed = false)", makeCurve,
                                  "A smooth curve through points x0, y0, x1, y1...; closed: back to the first.");
                        method(engine, "SceneCurve", "bool get_closed() const property", getFlag<&Shape::closed>,
                               "Whether it goes back to its first point (and can be filled).");
                        method(engine, "SceneCurve", "void set_closed(bool) property", setFlag<&Shape::closed>,
                               nullptr);
                        method(engine, "SceneCurve", "bool get_smooth() const property", getFlag<&Shape::smooth>,
                               "Whether it bends smoothly through its points (false: straight between them).");
                        method(engine, "SceneCurve", "void set_smooth(bool) property", setFlag<&Shape::smooth>,
                               nullptr);
                        method(engine, "SceneCurve", "uint get_count() const property", curveCount,
                               "How many points it goes through.");
                        registerHeads(engine, "SceneCurve");
                        behaviour(engine, "SceneNumber", asBEHAVE_FACTORY,
                                  "SceneNumber@ f(float value, int decimals = 0, Font@ font = null)", makeNumber,
                                  "A number as text, with so many decimals, in a font (null: the theme's).");
                        method(engine, "SceneNumber", "float get_value() const property", getFloat<&Shape::value>,
                               "The number it shows.");
                        method(engine, "SceneNumber", "void set_value(float) property", numberSet, nullptr);
                        method(engine, "SceneNumber", "int get_decimals() const property", numberDecimals,
                               "Its decimals (0 to 9).");
                        method(engine, "SceneNumber", "void set_decimals(int) property", numberSetDecimals, nullptr);
                        method(engine, "SceneNumber", "SceneAnimation@ countTo(float value)", animTo<Property::Value>,
                               "Counts to another value, the text following.");
                        behaviour(engine, "SceneAxes", asBEHAVE_FACTORY,
                                  "SceneAxes@ f(float width, float height, float xMin, float xMax, float yMin, float "
                                  "yMax)",
                                  makeAxes,
                                  "Axes width × height for x from xMin to xMax and y from yMin to yMax, with ticks and "
                                  "their numbers.");
                        method(engine, "SceneAxes", "float get_width() const property", getFloat<&Shape::width>,
                               "Its width.");
                        method(engine, "SceneAxes", "void set_width(float) property", setFloat<&Shape::width>, nullptr);
                        method(engine, "SceneAxes", "float get_height() const property", getFloat<&Shape::height>,
                               "Its height.");
                        method(engine, "SceneAxes", "void set_height(float) property", setFloat<&Shape::height>,
                               nullptr);
                        method(engine, "SceneAxes", "float get_xMin() const property", getFloat<&Shape::xMin>,
                               "The least x.");
                        method(engine, "SceneAxes", "void set_xMin(float) property", setFloat<&Shape::xMin>, nullptr);
                        method(engine, "SceneAxes", "float get_xMax() const property", getFloat<&Shape::xMax>,
                               "The greatest x.");
                        method(engine, "SceneAxes", "void set_xMax(float) property", setFloat<&Shape::xMax>, nullptr);
                        method(engine, "SceneAxes", "float get_yMin() const property", getFloat<&Shape::yMin>,
                               "The least y.");
                        method(engine, "SceneAxes", "void set_yMin(float) property", setFloat<&Shape::yMin>, nullptr);
                        method(engine, "SceneAxes", "float get_yMax() const property", getFloat<&Shape::yMax>,
                               "The greatest y.");
                        method(engine, "SceneAxes", "void set_yMax(float) property", setFloat<&Shape::yMax>, nullptr);
                        method(engine, "SceneAxes", "float get_xStep() const property", getFloat<&Shape::xStep>,
                               "Between x's ticks (0: a round step, about a fifth of the range).");
                        method(engine, "SceneAxes", "void set_xStep(float) property", setFloat<&Shape::xStep>, nullptr);
                        method(engine, "SceneAxes", "float get_yStep() const property", getFloat<&Shape::yStep>,
                               "Between y's ticks (0: a round step, about a fifth of the range).");
                        method(engine, "SceneAxes", "void set_yStep(float) property", setFloat<&Shape::yStep>, nullptr);
                        method(engine, "SceneAxes", "float toX(float x) const", axesToX,
                               "Where x is across the scene (its pixels), the axes unturned.");
                        method(engine, "SceneAxes", "float toY(float y) const", axesToY, "Where y is down the scene.");
                        method(engine, "SceneAxes", "SceneCurve@ plot(PlotFunction@ f)", plot,
                               "A curve of f over the x range, in the theme's accent, where the axes are now.");
                        method(engine, "SceneAxes", "SceneCurve@ plot(PlotFunction@ f, uint color, int samples = 100)",
                               plot, "A curve of f, in a colour, from so many values.");
                        // Step 14 (§8e): arrays and tables
                        // Numbers or text: not float too, which a list like {3, 1, 4} would fit as well as int.
                        const char *const arrays[] = {"SceneArray@ f(const array<int> &in values, float cellSize = 64)",
                                                      "SceneArray@ f(const ?&in values, float cellSize = 64)"};
                        for (const char *decl : arrays) {
                                behaviour(
                                        engine, "SceneArray", asBEHAVE_FACTORY, decl, makeArray,
                                        "Values in a row of cells (as print() shows them), their indices under them.");
                        }
                        const char *A = "SceneArray";
                        method(engine, A, "uint get_count() const property", arrayCount, "How many cells.");
                        method(engine, A, "SceneRect@ cell(uint index)", arrayCell, "A cell's box.");
                        method(engine, A, "SceneText@ text(uint index)", arrayText, "A cell's value, as text.");
                        method(engine, A, "string value(uint index) const", arrayValue, "A cell's value.");
                        method(engine, A, "bool get_indices() const property", arrayIndices,
                               "Whether its indices show under its cells.");
                        method(engine, A, "void set_indices(bool) property", arraySetIndices, nullptr);
                        method(engine, A, "SceneAnimation@ highlight(uint index)", arrayHighlight,
                               "A cell turns the highlight.");
                        method(engine, A, "SceneAnimation@ highlight(uint index, uint color)", arrayHighlight,
                               "A cell turns a colour.");
                        method(engine, A, "SceneAnimation@ unhighlight(uint index)", arrayUnhighlight,
                               "A cell back to its fill.");
                        method(engine, A, "SceneAnimation@ swap(uint a, uint b)", arraySwap,
                               "Two values change places, one over and one under.");
                        method(engine, A, "SceneAnimation@ set(uint index, const ?&in value)", arraySet,
                               "A value turns into another.");
                        method(engine, A, "SceneAnimation@ pointTo(const string &in name, uint index)", arrayPointTo,
                               "A named arrow under a cell (i, j, lo...): appears the first time, moves after.");
                        behaviour(
                                engine, "SceneTable", asBEHAVE_FACTORY,
                                "SceneTable@ f(uint rows, uint columns, float cellWidth = 120, float cellHeight = 48)",
                                makeTable, "A table of rows × columns cells, empty until set.");
                        const char *T = "SceneTable";
                        method(engine, T, "void set(uint row, uint column, const ?&in value)", tableSet,
                               "A cell's value (as print() shows it), at once.");
                        method(engine, T, "string get(uint row, uint column) const", tableGet, "A cell's value.");
                        method(engine, T, "SceneRect@ cell(uint row, uint column)", tableCell, "A cell's box.");
                        method(engine, T, "SceneText@ text(uint row, uint column)", tableText, "A cell's text.");
                        method(engine, T, "uint get_rows() const property", tableRows, "Its rows.");
                        method(engine, T, "uint get_columns() const property", tableColumns, "Its columns.");
                        method(engine, T, "bool get_header() const property", tableHeader,
                               "Whether its first row is a header (filled, bold).");
                        method(engine, T, "void set_header(bool) property", tableSetHeader, nullptr);
                        method(engine, T, "SceneAnimation@ highlight(uint row, uint column)", tableHighlight,
                               "A cell turns the highlight.");
                        angel::methodAux(engine, T, "SceneAnimation@ highlightRow(uint row)", tableHighlightLine,
                                         nullptr, "A row turns the highlight.");
                        angel::methodAux(engine, T, "SceneAnimation@ highlightColumn(uint column)", tableHighlightLine,
                                         reinterpret_cast<void *>(uintptr_t(1)), "A column turns the highlight.");
                        method(engine, T, "SceneAnimation@ unhighlightAll()", tableUnhighlightAll,
                               "Every cell back to how it was.");

                        // Graphs
                        behaviour(engine, "SceneGraph", asBEHAVE_FACTORY, "SceneGraph@ f()", makeGraph,
                                  "Nodes and the edges between them; edges follow their nodes.");
                        const char *G = "SceneGraph";
                        method(engine, G, "uint addNode(const ?&in label)", graphAddNode,
                               "A node (a circle and its label); its number.");
                        method(engine, G, "uint addEdge(uint from, uint to, bool directed = true)", graphAddEdge,
                               "An edge (an arrow when directed); its number.");
                        method(engine, G, "SceneGroup@ node(uint index)", graphNodeOf,
                               "A node, to move or animate (its circle, then its label).");
                        method(engine, G, "uint get_nodes() const property", graphNodes, "How many nodes.");
                        method(engine, G, "uint get_edges() const property", graphEdges, "How many edges.");
                        angel::methodAux(engine, G, "void layoutRow(float gap = 90)", graphLayout, nullptr,
                                         "Its nodes in a row, gap apart.");
                        angel::methodAux(engine, G, "void layoutCircle(float radius = 150)", graphLayout,
                                         reinterpret_cast<void *>(uintptr_t(1)),
                                         "Its nodes round a circle, the first at the top.");
                        angel::methodAux(engine, G, "void layoutGrid(uint columns, float gap = 100)", graphLayout,
                                         reinterpret_cast<void *>(uintptr_t(2)),
                                         "Its nodes in a grid, so many columns.");
                        method(engine, G, "void layoutTree(uint root = 0, float levelGap = 90, float siblingGap = 70)",
                               graphLayoutTree, "A tree from root down its edges, a level a row.");
                        angel::methodAux(engine, G, "SceneAnimation@ visit(uint index)", graphVisit, nullptr,
                                         "A node fills in the highlight.");
                        angel::methodAux(engine, G, "SceneAnimation@ unvisit(uint index)", graphVisit,
                                         reinterpret_cast<void *>(uintptr_t(1)), "A node back to its fill.");
                        method(engine, G, "SceneAnimation@ highlightEdge(uint a, uint b)", graphHighlightEdge,
                               "The edge between two nodes in the highlight, thicker.");
                        method(engine, G, "array<SceneAnimation@>@ highlightPath(const array<int> &in nodes)",
                               graphHighlightPath, "Nodes and the edges between them, in turn: play them with a lag.");

                        // Code
                        method(engine, "SceneCode", "SceneAnimation@ highlight(const array<int> &in lines)",
                               codeHighlight, "Lines (from 1) in focus, the others dimmed; none: all as they are.");
                        method(engine, "SceneCode", "SceneAnimation@ transformTo(const string &in code)",
                               codeTransformTo,
                               "Into other code: lines kept slide to their places, the rest fade out and in.");
                        method(engine, "SceneCode", "float get_dim() const property", getFloat<&Shape::dim>,
                               "How much lines out of focus are dimmed (0 to 1).");
                        method(engine, "SceneCode", "void set_dim(float) property", setFloat<&Shape::dim>, nullptr);
                        method(engine, "SceneCode", "uint get_lineCount() const property", codeLineCount, "Its lines.");

                        // Braces, callouts, labels
                        behaviour(engine, "SceneBrace", asBEHAVE_FACTORY,
                                  "SceneBrace@ f(SceneObject@ target, Side side, const string &in text = \"\")",
                                  makeBrace,
                                  "A curly brace along a side of an object (where it is now), its text beyond.");
                        behaviour(
                                engine, "SceneCallout", asBEHAVE_FACTORY,
                                "SceneCallout@ f(const string &in text, SceneObject@ target, Side side = Side::Above, "
                                "float gap = 24)",
                                makeCallout, "Text in a box beside an object, pointing to it.");
                        behaviour(engine, "SceneLabel", asBEHAVE_FACTORY,
                                  "SceneLabel@ f(const string &in text, SceneObject@ target, Side side = Side::Above, "
                                  "float "
                                  "gap = 8)",
                                  makeLabel, "Text kept beside an object as it moves.");
                        const char *const captioned[] = {"SceneBrace", "SceneCallout", "SceneLabel"};
                        for (const char *C : captioned) {
                                method(engine, C, "string get_text() const property", lastText, "What it says.");
                                method(engine, C, "void set_text(const string &in) property", setLastText, nullptr);
                        }

                        // Charts
                        const char *const charts[] = {"SceneBarChart", "ScenePieChart", "SceneLineChart"};
                        for (const char *C : charts) {
                                bool pie = C[5] == 'P';
                                // float only: a list like {12, 30} is made one (int too would make it ambiguous).
                                const char *const kindsOfValues[] = {"array<float>"};
                                for (const char *values : kindsOfValues) {
                                        char decl[200];
                                        if (pie) {
                                                snprintf(decl, sizeof decl,
                                                         "%s@ f(const %s &in values, const array<string> &in labels, "
                                                         "float "
                                                         "radius = 120)",
                                                         C, values);
                                        } else {
                                                snprintf(decl, sizeof decl,
                                                         "%s@ f(const %s &in values, const array<string> &in labels, "
                                                         "float "
                                                         "width = 500, float height = 260)",
                                                         C, values);
                                        }
                                        behaviour(engine, C, asBEHAVE_FACTORY, decl,
                                                  pie           ? makePieChart
                                                  : C[5] == 'B' ? makeBarChart
                                                                : makeLineChart,
                                                  pie ? "Values as slices of a pie, labelled, in the theme's chart "
                                                        "colours."
                                                  : C[5] == 'B'
                                                          ? "Values as bars over their labels, each's value above."
                                                          : "Values as a line over their labels, a dot each.");
                                }
                                method(engine, C, "uint get_count() const property", chartCount, "How many values.");
                                method(engine, C, "float value(uint index) const", chartValue, "A value.");
                                method(engine, C, "SceneAnimation@ build()", chartBuild,
                                       "Builds it: bars grow from their base, slices sweep round, the line is drawn.");
                                method(engine, C, "SceneAnimation@ setValue(uint index, float value)", chartSetValue,
                                       "A value changes, and the chart with it.");
                        }

                        // Icons, marks, confetti
                        behaviour(engine, "SceneIcon", asBEHAVE_FACTORY,
                                  "SceneIcon@ f(const string &in name, float size = 64)", makeIcon,
                                  "An icon of the system's theme by name (folder, audio-volume-high...), so big.");
                        behaviour(engine, "SceneMark", asBEHAVE_FACTORY, "SceneMark@ f(Mark mark, float size = 48)",
                                  makeMark, "A check mark (green) or a cross (red); write() draws it.");
                        behaviour(engine, "SceneConfetti", asBEHAVE_FACTORY,
                                  "SceneConfetti@ f(float width = 0, float height = 0)", makeConfetti,
                                  "Confetti over so much of the scene (0: all of it); burst() lets it fall.");
                        method(engine, "SceneConfetti", "SceneAnimation@ burst()", confettiBurst,
                               "The confetti falls, and is gone at its end.");
                        method(engine, "SceneGroup", "SceneGroup@ arrange(Side side, float gap = 16)", arrange,
                               "Its objects in a row (Side::Right, Left) or a column (Below, Above), gap apart, round "
                               "its centre; itself.");

                        // Widgets
                        behaviour(engine, "SceneSlider", asBEHAVE_FACTORY,
                                  "SceneSlider@ f(float minimum = 0, float maximum = 1, float value = 0, float width = "
                                  "300)",
                                  makeSlider,
                                  "A slider from minimum to maximum: dragged, or moved by the arrows when focused.");
                        method(engine, "SceneSlider", "float get_value() const property", getFloat<&Shape::value>,
                               "Where it is.");
                        method(engine, "SceneSlider", "void set_value(float) property", setValue, nullptr);
                        method(engine, "SceneSlider", "float get_minimum() const property", getFloat<&Shape::minimum>,
                               "Its least value.");
                        method(engine, "SceneSlider", "float get_maximum() const property", getFloat<&Shape::maximum>,
                               "Its greatest value.");
                        angel::behaviourAux(engine, "SceneButton", asBEHAVE_FACTORY,
                                            "SceneButton@ f(const string &in label)", makeLabelled,
                                            reinterpret_cast<void *>(uintptr_t(ShapeKind::Button)),
                                            "A button: clicked, or Space or Enter when focused.");
                        angel::behaviourAux(
                                engine, "SceneToggle", asBEHAVE_FACTORY,
                                "SceneToggle@ f(const string &in label, bool on = false)", makeLabelled,
                                reinterpret_cast<void *>(uintptr_t(ShapeKind::Toggle)),
                                "A box ticked or not, with its label: clicked, or Space or Enter when focused.");
                        method(engine, "SceneToggle", "bool get_on() const property", getOn, "Whether it is ticked.");
                        method(engine, "SceneToggle", "void set_on(bool) property", setOn, nullptr);
                        const char *const widgets[] = {"SceneSlider", "SceneButton", "SceneToggle"};
                        for (const char *w : widgets) {
                                method(engine, w, "void onChange(Callback@ callback)", onChange,
                                       "Calls a function when the user changes it (a button: presses it).");
                        }
                        for (const char *w : widgets) {
                                if (w == widgets[0]) continue; // a slider has no label
                                method(engine, w, "string get_label() const property", textGet, "What it says.");
                                method(engine, w, "void set_label(const string &in) property", labelSet, nullptr);
                        }

                        // The scene: its size, its theme, time, its objects, and draw()'s drawing
                        const char *S = "Scene";
                        property(engine, "Scene scene", &sceneGlobal,
                                 "The scene: a deck's region of a slide, or angel's stage (a window that plays it).");
                        method(engine, S, "int get_width() const property", sceneWidth, "Its width in pixels.");
                        method(engine, S, "int get_height() const property", sceneHeight, "Its height in pixels.");
                        method(engine, S, "SceneTheme@ get_theme() const property", sceneTheme,
                               "Its theme (a slide's; angel's stage's): colours and fonts.");
                        static constexpr const char *plays[] = {
                                "void play(SceneAnimation@ a, float seconds = 1)",
                                "void play(SceneAnimation@ a, SceneAnimation@ b, float seconds = 1)",
                                "void play(SceneAnimation@ a, SceneAnimation@ b, SceneAnimation@ c, float seconds = 1)",
                                "void play(SceneAnimation@ a, SceneAnimation@ b, SceneAnimation@ c, SceneAnimation@ d, "
                                "float seconds = 1)",
                                "void play(SceneAnimation@ a, SceneAnimation@ b, SceneAnimation@ c, SceneAnimation@ d, "
                                "SceneAnimation@ e, float seconds = 1)",
                                "void play(SceneAnimation@ a, SceneAnimation@ b, SceneAnimation@ c, SceneAnimation@ d, "
                                "SceneAnimation@ e, SceneAnimation@ f, float seconds = 1)",
                        };
                        for (const char *decl : plays) {
                                method(engine, S, decl, play,
                                       "Plays animations together, so many seconds; returns when they end.");
                        }
                        method(engine, S, "void play(const array<SceneAnimation@> &in animations, float seconds = 1)",
                               playArray, "Plays animations together, so many seconds; returns when they end.");
                        method(engine, S,
                               "void play(const array<SceneAnimation@> &in animations, float seconds, float lag)",
                               playLagged, "Plays animations, each lag seconds after the one before.");
                        method(engine, S, "void toFront(SceneObject@ object)", sceneToFront,
                               "Draws an object over the others.");
                        method(engine, S, "void toBack(SceneObject@ object)", sceneToBack,
                               "Draws an object under the others.");
                        method(engine, S, "bool has(SceneObject@ object) const", sceneHas,
                               "Whether an object is in the scene.");
                        method(engine, S, "void removeAll()", sceneRemoveAll, "Takes every object out of the scene.");
                        method(engine, S, "float get_cameraX() const property", cameraGet<&Shape::x>,
                               "The scene's point at its middle, across (its width / 2 unmoved).");
                        method(engine, S, "void set_cameraX(float) property", cameraSet<&Shape::x>, nullptr);
                        method(engine, S, "float get_cameraY() const property", cameraGet<&Shape::y>,
                               "The scene's point at its middle, down.");
                        method(engine, S, "void set_cameraY(float) property", cameraSet<&Shape::y>, nullptr);
                        method(engine, S, "float get_zoom() const property", cameraGet<&Shape::scale>,
                               "How much the camera magnifies (1: as it is).");
                        method(engine, S, "void set_zoom(float) property", cameraSet<&Shape::scale>, nullptr);
                        method(engine, S, "SceneAnimation@ panTo(float x, float y)", cameraPan,
                               "The camera to a point of the scene (at its middle).");
                        method(engine, S, "SceneAnimation@ zoomTo(SceneObject@ object, float margin = 40)",
                               cameraZoomTo, "The camera onto an object, filling the scene with margin round it.");
                        method(engine, S, "SceneAnimation@ zoomBy(float factor)", cameraZoomBy,
                               "The camera's zoom times a factor.");
                        method(engine, S, "SceneAnimation@ resetCamera()", cameraReset,
                               "The camera back to as it was.");
                        method(engine, S, "void wait(float seconds = 1)", wait, "Waits, the scene as it is.");
                        method(engine, S, "void pause()", pause,
                               "Waits for Next: a beat (a step of a slide; angel's stage: Space). Previous comes back "
                               "to it.");
                        method(engine, S, "void add(SceneObject@ object)", sceneAdd,
                               "Puts an object in the scene, as it is.");
                        method(engine, S, "void remove(SceneObject@ object)", sceneRemove,
                               "Takes an object out of the scene.");
                        method(engine, S, "void start(SceneAnimation@ animation, float seconds = 1)", startAnimation,
                               "Starts an animation that runs alongside (in event(), tick() or a callback, where "
                               "play() can't wait).");
                        method(engine, S, "void clear(uint color = Color::Black)", canvasClear,
                               "In draw(): the scene in one colour.");
                        method(engine, S, "void plot(float x, float y, uint color)", canvasPlot, "In draw(): a pixel.");
                        method(engine, S,
                               "void line(float x0, float y0, float x1, float y1, uint color, float width = 2)",
                               canvasLine, "In draw(): a line.");
                        method(engine, S,
                               "void rect(float x, float y, float width, float height, uint color, float lineWidth = "
                               "2)",
                               canvasRect, "In draw(): a rectangle's outline, its top left at (x, y).");
                        method(engine, S,
                               "void fillRect(float x, float y, float width, float height, uint color, float opacity = "
                               "1)",
                               canvasFillRect, "In draw(): a rectangle, filled.");
                        method(engine, S, "void circle(float x, float y, float radius, uint color, float width = 2)",
                               canvasCircle, "In draw(): a circle's outline round (x, y).");
                        method(engine, S,
                               "void fillCircle(float x, float y, float radius, uint color, float opacity = 1)",
                               canvasFillCircle, "In draw(): a circle, filled.");
                        method(engine, S,
                               "void ellipse(float x, float y, float rx, float ry, uint color, float width = 2)",
                               canvasEllipse, "In draw(): an ellipse's outline round (x, y).");
                        method(engine, S,
                               "void fillEllipse(float x, float y, float rx, float ry, uint color, float opacity = 1)",
                               canvasFillEllipse, "In draw(): an ellipse, filled.");
                        method(engine, S, "void polygon(const array<float> &in xy, uint color, float width = 2)",
                               canvasPolygon, "In draw(): a polygon's outline through x0, y0, x1, y1...");
                        method(engine, S, "void fillPolygon(const array<float> &in xy, uint color, float opacity = 1)",
                               canvasFillPolygon, "In draw(): a polygon, filled.");
                        method(engine, S, "int text(float x, float y, const ?&in value, uint color, Font@ font = null)",
                               canvasText,
                               "In draw(): text, its top left at (x, y), in a font (null: the theme's); its width.");
                        def(engine, "string readText(const string &in file)", readText,
                            "A text file from the script's folder (data for the script).");
                        return;
                }

        } // namespace

        void *sceneObject() {
                return &sceneGlobal;
        }

        void *makeEventObject(const ScriptInput &in) {
                void *p = malloc(sizeof(ScriptEvent));
                if (p == nullptr) return nullptr;
                auto *e = new (p) ScriptEvent();
                switch (in.type) {
                        case ScriptInput::Type::KeyDown: e->type = 0; break;
                        case ScriptInput::Type::KeyUp: e->type = 1; break;
                        case ScriptInput::Type::PointerMove: e->type = 2; break;
                        case ScriptInput::Type::PointerDown: e->type = 3; break;
                        case ScriptInput::Type::PointerUp: e->type = 4; break;
                        case ScriptInput::Type::Wheel: e->type = 5; break;
                }
                char name[24];
                angel::keyName(in.key, name, sizeof name);
                e->key = String(name);
                if (in.character >= 0x20 && in.character != 0x7F) {
                        // As UTF-8.
                        char     t[4];
                        uint32_t c = in.character, n = 0;
                        if (c < 0x80) {
                                t[n++] = char(c);
                        } else if (c < 0x800) {
                                t[n++] = char(0xC0 | (c >> 6));
                                t[n++] = char(0x80 | (c & 0x3F));
                        } else if (c < 0x10000) {
                                t[n++] = char(0xE0 | (c >> 12));
                                t[n++] = char(0x80 | ((c >> 6) & 0x3F));
                                t[n++] = char(0x80 | (c & 0x3F));
                        } else {
                                t[n++] = char(0xF0 | (c >> 18));
                                t[n++] = char(0x80 | ((c >> 12) & 0x3F));
                                t[n++] = char(0x80 | ((c >> 6) & 0x3F));
                                t[n++] = char(0x80 | (c & 0x3F));
                        }
                        e->text = String(t, n);
                }
                e->x = in.x;
                e->y = in.y;
                e->button = in.button;
                e->wheel = in.wheel;
                e->repeat = in.repeat;
                return e;
        }

        void releaseEventObject(void *event) {
                auto *e = static_cast<ScriptEvent *>(event);
                if (--e->refs > 0) return;
                e->~ScriptEvent();
                free(e);
                return;
        }

        void registerSceneApi(asIScriptEngine *engine) {
                registerScene(engine);
                return;
        }

        void registerDeckApi(asIScriptEngine *engine) {
                registerDeckTypes(engine);
                registerScene(engine);
                return;
        }

        void printDeckApi(bool help) {
                asIScriptEngine *engine = angel::createEngine(nullptr, nullptr, true);
                if (engine == nullptr) return;
                angel::registerString(engine);
                angel::registerLanguage(engine);
                angel::registerColors(engine);
                registerDeckApi(engine);
                angel::printApi(engine, help);
                engine->ShutDownAndRelease();
                angel::forgetHelp();
                return;
        }

} // namespace preso
