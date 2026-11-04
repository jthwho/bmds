/**
 * @file      api.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The deck profile (devplan/preso.md §7.4, §7.7): what a deck's scripts have beside the
 * language (libangel's strings, maths, print, colours). The scene's objects, animations, time
 * (play, wait, pause), the theme, the region, draw()'s Canvas, and text files read from the
 * deck's folder. Nothing reaches outside the deck. Userspace ABI once released: append only
 * (tests/preso/api.lock, from `preso --api`).
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

                // ---- Helpers

                AngelRegion *region() {
                        return runningRegion();
                }

                const Theme &theme() {
                        AngelRegion *r = region();
                        return r != nullptr && r->place().theme != nullptr ? *r->place().theme : Theme::paper();
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
                        FontKey key = theme().body.font;
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

                // ---- The theme and the region

                struct Global {
                                int unused;
                };
                Global themeObject;
                Global regionObject;

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
                        ScriptFont *f = ScriptFont::make(key);
                        if (f == nullptr) raise("Out of memory for a font");
                        gen->SetReturnAddress(f);
                }
                void regionWidth(asIScriptGeneric *gen) {
                        AngelRegion *r = region();
                        gen->SetReturnDWord(asDWORD(r != nullptr ? r->place().width : 0));
                }
                void regionHeight(asIScriptGeneric *gen) {
                        AngelRegion *r = region();
                        gen->SetReturnDWord(asDWORD(r != nullptr ? r->place().height : 0));
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
                        Arg<ScriptFont> f(gen, 1);
                        Shape          *s = newShape(ShapeKind::Text, theme().body.color);
                        if (s != nullptr) {
                                if (f) {
                                        f->addRef();
                                        s->font = f.get();
                                }
                                if (!s->setText(text.data(), uint32_t(text.length()))) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
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
                        static Point points[256];
                        uint32_t     n = 0;
                        if (!pointsOf(static_cast<const CScriptArray *>(gen->GetArgAddress(0)), points, 256, &n))
                                return;
                        if (n < 3) {
                                raise("A polygon has 3 points at least");
                                return;
                        }
                        // Placed at the middle of its box; its points from there.
                        float x0 = points[0].x, x1 = x0, y0 = points[0].y, y1 = y0;
                        for (uint32_t i = 1; i < n; i++) {
                                x0 = points[i].x < x0 ? points[i].x : x0;
                                x1 = points[i].x > x1 ? points[i].x : x1;
                                y0 = points[i].y < y0 ? points[i].y : y0;
                                y1 = points[i].y > y1 ? points[i].y : y1;
                        }
                        Shape *s = newShape(ShapeKind::Polygon, color("Script.Accent"));
                        if (s != nullptr) {
                                s->x = (x0 + x1) / 2;
                                s->y = (y0 + y1) / 2;
                                for (uint32_t i = 0; i < n; i++)
                                        points[i] = Point{points[i].x - s->x, points[i].y - s->y};
                                if (!s->setPoints(points, n)) {
                                        s->release();
                                        s = nullptr;
                                }
                        }
                        returnShape(gen, s);
                }
                void makeImage(asIScriptGeneric *gen) {
                        AngelRegion *r = region();
                        StringView   path = view(argString(gen, 0));
                        char         resolved[512];
                        const char  *why = nullptr;
                        if (r == nullptr || !r->owner().resolve(path, resolved, sizeof resolved, &why)) {
                                raise("Image `%s` %s", argString(gen, 0).cStr(),
                                      why != nullptr ? why : "can't be read");
                                return;
                        }
                        float w = 0, h = 0;
                        if (gen->GetArgCount() >= 3) {
                                w = gen->GetArgFloat(1);
                                h = gen->GetArgFloat(2);
                        } else {
                                int32_t iw = 0, ih = 0;
                                Error   e = r->owner().images() != nullptr ? r->owner().images()->size(path, &iw, &ih)
                                                                           : Error::NotSupported;
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
                                AngelRegion *r = region();
                                if (language.length() > 0 && r != nullptr)
                                        s->syntax = r->owner().platform().findSyntax(view(language));
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
                        animSelf(gen)->ease = e <= uint32_t(Ease::Bounce) ? Ease(e) : Ease::InOut;
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

                // An animation of the first argument (raised if it is null).
                Animation *animationOf(asIScriptGeneric *gen) {
                        Arg<Shape> s(gen, 0);
                        if (!s) {
                                raise("An animation of null");
                                return nullptr;
                        }
                        Animation *a = Animation::make(s.get());
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
                                a->change(Property::X, gen->GetArgFloat(1));
                                a->change(Property::Y, gen->GetArgFloat(2));
                        }
                        returnAnimation(gen, a);
                }
                void animShift(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->change(Property::X, gen->GetArgFloat(1), true);
                                a->change(Property::Y, gen->GetArgFloat(2), true);
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
                        Arg<Shape> to(gen, 1);
                        if (!to) {
                                Arg<Shape> object(gen, 0); // its reference let go too
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
                        if (a != nullptr) a->changeColor(Property::StrokeColor, gen->GetArgDWord(1) & 0xFFFFFF);
                        returnAnimation(gen, a);
                }
                void animFillTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) {
                                a->changeColor(Property::FillColor, gen->GetArgDWord(1) & 0xFFFFFF);
                                if (a->target->fillOpacity <= 0) a->change(Property::FillOpacity, 1);
                        }
                        returnAnimation(gen, a);
                }
                void animScaleTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(Property::Scale, gen->GetArgFloat(1));
                        returnAnimation(gen, a);
                }
                void animRotateTo(asIScriptGeneric *gen) {
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(Property::Rotation, gen->GetArgFloat(1));
                        returnAnimation(gen, a);
                }
                void animTween(asIScriptGeneric *gen) {
                        Property p;
                        if (!propertyNamed(view(argString(gen, 1)), &p)) {
                                raise("tween(): no property `%s` (x, y, scale, rotation, opacity, lineWidth, "
                                      "fillOpacity, width, height, radius, reveal)",
                                      argString(gen, 1).cStr());
                                return;
                        }
                        Animation *a = animationOf(gen);
                        if (a != nullptr) a->change(p, gen->GetArgFloat(2));
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
                        AngelRegion *r = region();
                        uint32_t     n = gen->GetArgCount() - 1;
                        Animation   *list[6];
                        bool         null = false;
                        for (uint32_t i = 0; i < n; i++) {
                                memcpy(&list[i], gen->GetAddressOfArg(i), sizeof list[i]);
                                null |= list[i] == nullptr;
                        }
                        if (null) {
                                raise("play() got null, not an animation");
                        } else if (r != nullptr) {
                                r->play(list, n, gen->GetArgFloat(n));
                        }
                        // The arguments' references, the function's to let go.
                        for (uint32_t i = 0; i < n; i++) {
                                if (list[i] != nullptr) list[i]->release();
                        }
                }
                void playArray(asIScriptGeneric *gen) {
                        AngelRegion        *r = region();
                        const CScriptArray *array = static_cast<const CScriptArray *>(gen->GetArgAddress(0));
                        uint32_t            n = array != nullptr ? array->GetSize() : 0;
                        Animation          *list[AngelRegion::MaxPlaying];
                        if (n > AngelRegion::MaxPlaying) {
                                raise("play() takes %u animations at most", unsigned(AngelRegion::MaxPlaying));
                                return;
                        }
                        for (uint32_t i = 0; i < n; i++) {
                                list[i] = *static_cast<Animation *const *>(array->At(i));
                                if (list[i] == nullptr) {
                                        raise("play() got null, not an animation");
                                        return;
                                }
                        }
                        if (r != nullptr) r->play(list, n, gen->GetArgFloat(1));
                }
                void wait(asIScriptGeneric *gen) {
                        if (region() != nullptr) region()->wait(gen->GetArgFloat(0));
                }
                void pause(asIScriptGeneric *) {
                        if (region() != nullptr) region()->pause();
                }
                void sceneAdd(asIScriptGeneric *gen) {
                        Arg<Shape> s(gen, 0);
                        if (s && region() != nullptr) region()->add(s.get());
                }
                void sceneRemove(asIScriptGeneric *gen) {
                        Arg<Shape> s(gen, 0);
                        if (s && region() != nullptr) region()->remove(s.get());
                }
                void readText(asIScriptGeneric *gen) {
                        AngelRegion *r = region();
                        char         path[512];
                        const char  *why = nullptr;
                        if (r == nullptr || !r->owner().resolve(view(argString(gen, 0)), path, sizeof path, &why)) {
                                raise("`%s` %s", argString(gen, 0).cStr(), why != nullptr ? why : "can't be read");
                                new (gen->GetAddressOfReturnLocation()) String();
                                return;
                        }
                        char    *data = nullptr;
                        uint32_t length = 0;
                        Error    e = r->owner().platform().readFile(path, MaxRead, &data, &length);
                        if (e != Error::Ok) {
                                raise("`%s` %s", argString(gen, 0).cStr(),
                                      e == Error::NotFound ? "isn't there" : errorName(e));
                                new (gen->GetAddressOfReturnLocation()) String();
                                return;
                        }
                        String *s = new (gen->GetAddressOfReturnLocation()) String();
                        s->adopt(data, length);
                }

                // ---- draw()'s Canvas: on the region being drawn, in its pixels

                struct ScriptCanvas {
                                uint32_t refs = 1;
                };

                void canvasAddRef(asIScriptGeneric *gen) {
                        static_cast<ScriptCanvas *>(gen->GetObject())->refs++;
                }
                void canvasRelease(asIScriptGeneric *gen) {
                        auto *c = static_cast<ScriptCanvas *>(gen->GetObject());
                        if (--c->refs == 0) {
                                c->~ScriptCanvas();
                                free(c);
                        }
                }
                // The canvas being drawn on, or nullptr (raised) outside draw().
                Canvas *drawing() {
                        AngelRegion *r = region();
                        Canvas      *c = r != nullptr ? r->drawCanvas() : nullptr;
                        if (c == nullptr) raise("A Canvas is drawn on only in draw()");
                        return c;
                }
                Point at(float x, float y) {
                        AngelRegion *r = region();
                        return Point{x + float(r->drawX()), y + float(r->drawY())};
                }
                uint8_t opacityOf(float o) {
                        return o <= 0 ? 0 : o >= 1 ? 255 : uint8_t(o * 255 + 0.5f);
                }
                void canvasWidth(asIScriptGeneric *gen) {
                        regionWidth(gen);
                }
                void canvasHeight(asIScriptGeneric *gen) {
                        regionHeight(gen);
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
                        strokePoints(*c, pts, 2, false, gen->GetArgDWord(4) & 0xFFFFFF, gen->GetArgFloat(5));
                }
                void canvasRect(asIScriptGeneric *gen) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        float w = gen->GetArgFloat(2), h = gen->GetArgFloat(3);
                        Point pts[4] = {p, Point{p.x + w, p.y}, Point{p.x + w, p.y + h}, Point{p.x, p.y + h}};
                        strokePoints(*c, pts, 4, true, gen->GetArgDWord(4) & 0xFFFFFF, gen->GetArgFloat(5));
                }
                void canvasFillRect(asIScriptGeneric *gen) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        Path  path;
                        path.addRect(p.x, p.y, gen->GetArgFloat(2), gen->GetArgFloat(3));
                        (void)fillPath(*c, path, gen->GetArgDWord(4) & 0xFFFFFF, opacityOf(gen->GetArgFloat(5)));
                }
                void ellipse(asIScriptGeneric *gen, float rx, float ry, uint32_t colorArg, bool filled) {
                        Canvas *c = drawing();
                        if (c == nullptr) return;
                        Point p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        Color color = gen->GetArgDWord(colorArg) & 0xFFFFFF;
                        float extra = gen->GetArgFloat(colorArg + 1);
                        Path  path;
                        if (filled) {
                                path.addEllipse(p.x, p.y, rx, ry);
                                (void)fillPath(*c, path, color, opacityOf(extra));
                                return;
                        }
                        static Point pts[256];
                        uint32_t     n = circleSides(rx > ry ? rx : ry);
                        ellipsePoints(pts, n, p.x, p.y, rx, ry, 0, false);
                        strokePoints(*c, pts, n, true, color, extra);
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
                        static Point pts[1024];
                        uint32_t     n = 0;
                        if (!pointsOf(static_cast<const CScriptArray *>(gen->GetArgAddress(0)), pts, 1024, &n)) return;
                        for (uint32_t i = 0; i < n; i++) pts[i] = at(pts[i].x, pts[i].y);
                        Color color = gen->GetArgDWord(1) & 0xFFFFFF;
                        float extra = gen->GetArgCount() > 2 ? gen->GetArgFloat(2) : (filled ? 1.0f : 2.0f);
                        if (filled) {
                                Path path;
                                path.addPolygon(pts, n);
                                (void)fillPath(*c, path, color, opacityOf(extra));
                        } else if (n >= 2) {
                                strokePoints(*c, pts, n, true, color, extra);
                        }
                }
                void canvasPolygon(asIScriptGeneric *gen) {
                        polygon(gen, false);
                }
                void canvasFillPolygon(asIScriptGeneric *gen) {
                        polygon(gen, true);
                }
                void canvasText(asIScriptGeneric *gen) {
                        Arg<ScriptFont> f(gen, 4);
                        Canvas         *c = drawing();
                        if (c == nullptr) return;
                        angel::Text text;
                        angel::formatValue(gen->GetEngine(), text, gen->GetArgAddress(2), gen->GetArgTypeId(2));
                        FontKey      key = f ? f->key() : theme().body.font;
                        AngelRegion *r = region();
                        Point        p = at(gen->GetArgFloat(0), gen->GetArgFloat(1));
                        StringView   t(text.data(), uint32_t(text.length()));
                        r->context().fonts->draw(*c, key, int32_t(lroundf(p.x)), int32_t(lroundf(p.y)), t,
                                                 gen->GetArgDWord(3) & 0xFFFFFF);
                        gen->SetReturnDWord(asDWORD(r->context().fonts->width(key, t)));
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
                                raise("start() got null, not an animation");
                                return;
                        }
                        if (region() != nullptr) region()->start(a.get(), gen->GetArgFloat(1));
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
                        if (r == nullptr || !r->owner().resolve(view(argString(gen, 0)), path, sizeof path, &why)) {
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

                // ---- Registering

                void check(int r, const char *what) {
                        angel::check(r, what);
                        return;
                }

                // The members every kind of object has, on type @p T.
                void registerShapeMembers(asIScriptEngine *engine, const char *T) {
                        method(engine, T, "float get_x() const property", getFloat<&Shape::x>,
                               "Its centre's x, in the region's pixels.");
                        method(engine, T, "void set_x(float) property", setFloat<&Shape::x>, nullptr);
                        method(engine, T, "float get_y() const property", getFloat<&Shape::y>,
                               "Its centre's y, in the region's pixels (down).");
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
                        return;
                }

                struct Kind {
                                const char *name;
                                ShapeKind   kind;
                };
                constexpr Kind kinds[] = {
                        {"Text", ShapeKind::Text},     {"Rect", ShapeKind::Rect},     {"Circle", ShapeKind::Circle},
                        {"Line", ShapeKind::Line},     {"Arrow", ShapeKind::Arrow},   {"Polygon", ShapeKind::Polygon},
                        {"Image", ShapeKind::Image},   {"Code", ShapeKind::Code},     {"Group", ShapeKind::Group},
                        {"Bars", ShapeKind::Bars},     {"Grid", ShapeKind::Grid},     {"Slider", ShapeKind::Slider},
                        {"Button", ShapeKind::Button}, {"Toggle", ShapeKind::Toggle},
                };

                void registerAll(asIScriptEngine *engine) {
                        // Types first: their members name one another.
                        check(engine->RegisterObjectType("Font", 0, asOBJ_REF), "Font");
                        check(engine->RegisterObjectType("Theme", 0, asOBJ_REF | asOBJ_NOCOUNT), "Theme");
                        check(engine->RegisterObjectType("Region", 0, asOBJ_REF | asOBJ_NOCOUNT), "Region");
                        check(engine->RegisterObjectType("Shape", 0, asOBJ_REF), "Shape");
                        for (const Kind &k : kinds) check(engine->RegisterObjectType(k.name, 0, asOBJ_REF), k.name);
                        check(engine->RegisterObjectType("Animation", 0, asOBJ_REF), "Animation");
                        check(engine->RegisterObjectType("Canvas", 0, asOBJ_REF), "Canvas");
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
                        check(engine->RegisterEnum("Ease"), "Ease");
                        check(engine->RegisterEnumValue("Ease", "Linear", int(Ease::Linear)), "Ease::Linear");
                        check(engine->RegisterEnumValue("Ease", "In", int(Ease::In)), "Ease::In");
                        check(engine->RegisterEnumValue("Ease", "Out", int(Ease::Out)), "Ease::Out");
                        check(engine->RegisterEnumValue("Ease", "InOut", int(Ease::InOut)), "Ease::InOut");
                        check(engine->RegisterEnumValue("Ease", "Bounce", int(Ease::Bounce)), "Ease::Bounce");

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

                        // The theme and the region
                        method(engine, "Theme", "uint color(const string &in name) const", themeColor,
                               "A colour of the slide's theme: Text, Background, Accent, H1, H2, H3, Code, Link, "
                               "Rule, or Script.NAME from its [Script] section (Script.Accent, Highlight, Muted, "
                               "Line and Fill always there).");
                        method(engine, "Theme", "Font@ font(const string &in name) const", themeFont,
                               "A font of the slide's theme: Text, H1, H2, H3, Code, Small.");
                        property(engine, "const Theme theme", &themeObject,
                                 "The slide's theme: its colours and fonts.");
                        method(engine, "Region", "int get_width() const property", regionWidth,
                               "The region's width in pixels.");
                        method(engine, "Region", "int get_height() const property", regionHeight,
                               "The region's height in pixels.");
                        property(engine, "const Region region", &regionObject, "The script's region of the slide.");

                        // Objects
                        behaviour(engine, "Shape", asBEHAVE_ADDREF, "void f()", shapeAddRef, nullptr);
                        behaviour(engine, "Shape", asBEHAVE_RELEASE, "void f()", shapeRelease, nullptr);
                        registerShapeMembers(engine, "Shape");
                        for (const Kind &k : kinds) {
                                behaviour(engine, k.name, asBEHAVE_ADDREF, "void f()", shapeAddRef, nullptr);
                                behaviour(engine, k.name, asBEHAVE_RELEASE, "void f()", shapeRelease, nullptr);
                                registerShapeMembers(engine, k.name);
                                method(engine, k.name, "Shape@ opImplCast()", implicitCast, nullptr);
                                char decl[48];
                                snprintf(decl, sizeof decl, "%s@ opCast()", k.name);
                                angel::methodAux(engine, "Shape", decl, downCast,
                                                 reinterpret_cast<void *>(uintptr_t(k.kind)), nullptr);
                        }
                        behaviour(engine, "Text", asBEHAVE_FACTORY, "Text@ f(const ?&in text, Font@ font = null)",
                                  makeText,
                                  "Text (a value, as print() shows it; lines at \\n, centred), in a font (null: the "
                                  "theme's text's).");
                        method(engine, "Text", "string get_text() const property", textGet, "What it says.");
                        method(engine, "Text", "void set_text(const string &in) property", textSet, nullptr);
                        method(engine, "Text", "float get_width() const property", textWidth,
                               "Its width in pixels, before scaling.");
                        method(engine, "Text", "float get_height() const property", textHeight,
                               "Its height in pixels, before scaling.");
                        behaviour(engine, "Rect", asBEHAVE_FACTORY, "Rect@ f(float width, float height)", makeRect,
                                  "A rectangle, outlined in the theme's accent (set fill to fill it).");
                        method(engine, "Rect", "float get_width() const property", getFloat<&Shape::width>,
                               "Its width.");
                        method(engine, "Rect", "void set_width(float) property", setFloat<&Shape::width>, nullptr);
                        method(engine, "Rect", "float get_height() const property", getFloat<&Shape::height>,
                               "Its height.");
                        method(engine, "Rect", "void set_height(float) property", setFloat<&Shape::height>, nullptr);
                        behaviour(engine, "Circle", asBEHAVE_FACTORY, "Circle@ f(float radius)", makeCircle,
                                  "A circle, outlined in the theme's accent (set fill to fill it).");
                        method(engine, "Circle", "float get_radius() const property", getFloat<&Shape::radius>,
                               "Its radius.");
                        method(engine, "Circle", "void set_radius(float) property", setFloat<&Shape::radius>, nullptr);
                        angel::behaviourAux(engine, "Line", asBEHAVE_FACTORY,
                                            "Line@ f(float x0, float y0, float x1, float y1)", makeLine, nullptr,
                                            "A line from (x0, y0) to (x1, y1); placed at its middle.");
                        angel::behaviourAux(engine, "Arrow", asBEHAVE_FACTORY,
                                            "Arrow@ f(float x0, float y0, float x1, float y1)", makeLine,
                                            reinterpret_cast<void *>(uintptr_t(1)),
                                            "An arrow from (x0, y0), its head at (x1, y1); placed at its middle.");
                        behaviour(engine, "Polygon", asBEHAVE_FACTORY, "Polygon@ f(const array<float> &in xy)",
                                  makePolygon,
                                  "A polygon through points x0, y0, x1, y1...; placed at the middle of its box.");
                        behaviour(engine, "Image", asBEHAVE_FACTORY, "Image@ f(const string &in file)", makeImage,
                                  "A picture from the deck's folder (PNG, JPEG, BMP), at its own size.");
                        behaviour(engine, "Image", asBEHAVE_FACTORY,
                                  "Image@ f(const string &in file, float width, float height)", makeImage,
                                  "A picture from the deck's folder, at a size.");
                        method(engine, "Image", "float get_width() const property", getFloat<&Shape::width>,
                               "Its width.");
                        method(engine, "Image", "void set_width(float) property", setFloat<&Shape::width>, nullptr);
                        method(engine, "Image", "float get_height() const property", getFloat<&Shape::height>,
                               "Its height.");
                        method(engine, "Image", "void set_height(float) property", setFloat<&Shape::height>, nullptr);
                        behaviour(engine, "Code", asBEHAVE_FACTORY,
                                  "Code@ f(const string &in text, const string &in language = \"\")", makeCode,
                                  "Code in the theme's code font, coloured as the language's (angel, c, cpp...), on "
                                  "its box.");
                        behaviour(engine, "Group", asBEHAVE_FACTORY, "Group@ f()", makeGroup,
                                  "Objects moved, turned, scaled and faded together: theirs are from its centre.");
                        method(engine, "Group", "void add(Shape@ object)", groupAdd, "Puts an object in it.");
                        method(engine, "Group", "void remove(Shape@ object)", groupRemove,
                               "Takes an object out of it.");
                        method(engine, "Group", "uint get_count() const property", groupCount, "The objects in it.");
                        behaviour(engine, "Bars", asBEHAVE_FACTORY,
                                  "Bars@ f(const array<float> &in values, float width = 600, float height = 300)",
                                  makeBars, "Values as bars, side by side in width × height, the largest the height.");
                        method(engine, "Bars", "float value(int index) const", barsValue, "A bar's value.");
                        method(engine, "Bars", "uint get_count() const property", barsCount, "How many bars.");
                        method(engine, "Bars", "Animation@ swap(int a, int b)", barsSwap, "Two bars change places.");
                        method(engine, "Bars", "Animation@ highlight(int index)", barsHighlight,
                               "A bar turns the theme's highlight (Script.Highlight).");
                        method(engine, "Bars", "Animation@ highlight(int index, uint color)", barsHighlight,
                               "A bar turns a colour.");
                        method(engine, "Bars", "Animation@ unhighlight(int index)", barsUnhighlight,
                               "A bar turns back to the accent (Script.Accent).");
                        method(engine, "Bars", "Animation@ set(int index, float value)", barsSet,
                               "A bar grows or shrinks to a value.");
                        behaviour(engine, "Grid", asBEHAVE_FACTORY, "Grid@ f(float width, float height, float step)",
                                  makeGrid, "Lines step apart over width × height, from its centre out.");

                        // Widgets
                        behaviour(engine, "Slider", asBEHAVE_FACTORY,
                                  "Slider@ f(float minimum = 0, float maximum = 1, float value = 0, float width = 300)",
                                  makeSlider,
                                  "A slider from minimum to maximum: dragged, or moved by the arrows when focused.");
                        method(engine, "Slider", "float get_value() const property", getFloat<&Shape::value>,
                               "Where it is.");
                        method(engine, "Slider", "void set_value(float) property", setValue, nullptr);
                        method(engine, "Slider", "float get_minimum() const property", getFloat<&Shape::minimum>,
                               "Its least value.");
                        method(engine, "Slider", "float get_maximum() const property", getFloat<&Shape::maximum>,
                               "Its greatest value.");
                        angel::behaviourAux(engine, "Button", asBEHAVE_FACTORY, "Button@ f(const string &in label)",
                                            makeLabelled, reinterpret_cast<void *>(uintptr_t(ShapeKind::Button)),
                                            "A button: clicked, or Space or Enter when focused.");
                        angel::behaviourAux(
                                engine, "Toggle", asBEHAVE_FACTORY,
                                "Toggle@ f(const string &in label, bool on = false)", makeLabelled,
                                reinterpret_cast<void *>(uintptr_t(ShapeKind::Toggle)),
                                "A box ticked or not, with its label: clicked, or Space or Enter when focused.");
                        method(engine, "Toggle", "bool get_on() const property", getOn, "Whether it is ticked.");
                        method(engine, "Toggle", "void set_on(bool) property", setOn, nullptr);
                        const char *const widgets[] = {"Slider", "Button", "Toggle"};
                        for (const char *w : widgets) {
                                method(engine, w, "void onChange(Callback@ callback)", onChange,
                                       "Calls a function when the user changes it (a button: presses it).");
                        }
                        for (const char *w : widgets) {
                                if (w == widgets[0]) continue; // a slider has no label
                                method(engine, w, "string get_label() const property", textGet, "What it says.");
                                method(engine, w, "void set_label(const string &in) property", labelSet, nullptr);
                        }

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
                        def(engine, "void start(Animation@ animation, float seconds = 1)", startAnimation,
                            "Starts an animation that runs alongside (in event(), tick() or a callback, where play() "
                            "can't wait).");

                        // Sound
                        behaviour(engine, "Sound", asBEHAVE_FACTORY, "Sound@ f(const string &in file)", makeSound,
                                  "A sound from the deck's folder (WAV, MP3, Ogg, FLAC).");
                        behaviour(engine, "Sound", asBEHAVE_ADDREF, "void f()", soundAddRef, nullptr);
                        behaviour(engine, "Sound", asBEHAVE_RELEASE, "void f()", soundRelease, nullptr);
                        method(engine, "Sound", "void play(float volume = 1)", soundPlay,
                               "Plays it (over what plays), when its slide is shown; never in fast-forward.");

                        // Animations
                        behaviour(engine, "Animation", asBEHAVE_ADDREF, "void f()", animAddRef, nullptr);
                        behaviour(engine, "Animation", asBEHAVE_RELEASE, "void f()", animRelease, nullptr);
                        method(engine, "Animation", "Animation@ ease(Ease ease)", animEase,
                               "How it goes (Ease::InOut unless said); itself, to go on with.");
                        method(engine, "Animation", "Animation@ delay(float seconds)", animDelay,
                               "Begins so long after the play does; itself.");
                        method(engine, "Animation", "Animation@ duration(float seconds)", animDuration,
                               "Takes so long, not the play's time; itself.");
                        def(engine, "Animation@ move(Shape@ object, float x, float y)", animMove,
                            "Moves it to (x, y).");
                        def(engine, "Animation@ shift(Shape@ object, float dx, float dy)", animShift,
                            "Moves it by (dx, dy).");
                        def(engine, "Animation@ fadeIn(Shape@ object)", animFadeIn, "Brings it in, from transparent.");
                        def(engine, "Animation@ fadeOut(Shape@ object)", animFadeOut,
                            "Fades it away, and takes it out.");
                        def(engine, "Animation@ grow(Shape@ object)", animGrow, "Brings it in, growing from nothing.");
                        def(engine, "Animation@ shrink(Shape@ object)", animShrink,
                            "Shrinks it to nothing, and takes it out.");
                        def(engine, "Animation@ write(Shape@ object)", animWrite,
                            "Brings it in as if drawn: text a letter at a time, a shape round its outline.");
                        def(engine, "Animation@ transform(Shape@ object, Shape@ into)", animTransform,
                            "Turns it into another's shape, colour and place (it stays the object it was).");
                        def(engine, "Animation@ colorTo(Shape@ object, uint color)", animColorTo,
                            "Its colour to another.");
                        def(engine, "Animation@ fillTo(Shape@ object, uint color)", animFillTo,
                            "Its inside to a colour.");
                        def(engine, "Animation@ scaleTo(Shape@ object, float scale)", animScaleTo,
                            "Its scale to another.");
                        def(engine, "Animation@ rotateTo(Shape@ object, float degrees)", animRotateTo,
                            "Turns it to so many degrees.");
                        def(engine, "Animation@ tween(Shape@ object, const string &in property, float to)", animTween,
                            "A property to a value: x, y, scale, rotation, opacity, lineWidth, fillOpacity, width, "
                            "height, radius, reveal.");

                        // Time and the scene
                        static constexpr const char *plays[] = {
                                "void play(Animation@ a, float seconds = 1)",
                                "void play(Animation@ a, Animation@ b, float seconds = 1)",
                                "void play(Animation@ a, Animation@ b, Animation@ c, float seconds = 1)",
                                "void play(Animation@ a, Animation@ b, Animation@ c, Animation@ d, float seconds = 1)",
                                "void play(Animation@ a, Animation@ b, Animation@ c, Animation@ d, Animation@ e, float "
                                "seconds = 1)",
                                "void play(Animation@ a, Animation@ b, Animation@ c, Animation@ d, Animation@ e, "
                                "Animation@ f, float seconds = 1)",
                        };
                        for (const char *decl : plays) {
                                def(engine, decl, play,
                                    "Plays animations together, so many seconds; returns when they end.");
                        }
                        def(engine, "void play(const array<Animation@> &in animations, float seconds = 1)", playArray,
                            "Plays animations together, so many seconds; returns when they end.");
                        def(engine, "void wait(float seconds = 1)", wait, "Waits, the region as it is.");
                        def(engine, "void pause()", pause,
                            "Waits for Next: a beat, a step of the slide (Previous comes back to it).");
                        def(engine, "void add(Shape@ object)", sceneAdd, "Puts an object in the region, as it is.");
                        def(engine, "void remove(Shape@ object)", sceneRemove, "Takes an object out of the region.");
                        def(engine, "string readText(const string &in file)", readText,
                            "A text file from the deck's folder (data for the script).");

                        // draw()'s Canvas
                        behaviour(engine, "Canvas", asBEHAVE_ADDREF, "void f()", canvasAddRef, nullptr);
                        behaviour(engine, "Canvas", asBEHAVE_RELEASE, "void f()", canvasRelease, nullptr);
                        const char *C = "Canvas";
                        method(engine, C, "int width() const", canvasWidth, "The region's width.");
                        method(engine, C, "int height() const", canvasHeight, "The region's height.");
                        method(engine, C, "void clear(uint color = Color::Black)", canvasClear,
                               "The region in one colour.");
                        method(engine, C, "void plot(float x, float y, uint color)", canvasPlot, "A pixel.");
                        method(engine, C,
                               "void line(float x0, float y0, float x1, float y1, uint color, float width = 2)",
                               canvasLine, "A line.");
                        method(engine, C,
                               "void rect(float x, float y, float width, float height, uint color, float lineWidth = "
                               "2)",
                               canvasRect, "A rectangle's outline, its top left at (x, y).");
                        method(engine, C,
                               "void fillRect(float x, float y, float width, float height, uint color, float opacity = "
                               "1)",
                               canvasFillRect, "A rectangle, filled.");
                        method(engine, C, "void circle(float x, float y, float radius, uint color, float width = 2)",
                               canvasCircle, "A circle's outline round (x, y).");
                        method(engine, C,
                               "void fillCircle(float x, float y, float radius, uint color, float opacity = 1)",
                               canvasFillCircle, "A circle, filled.");
                        method(engine, C,
                               "void ellipse(float x, float y, float rx, float ry, uint color, float width = 2)",
                               canvasEllipse, "An ellipse's outline round (x, y).");
                        method(engine, C,
                               "void fillEllipse(float x, float y, float rx, float ry, uint color, float opacity = 1)",
                               canvasFillEllipse, "An ellipse, filled.");
                        method(engine, C, "void polygon(const array<float> &in xy, uint color, float width = 2)",
                               canvasPolygon, "A polygon's outline through x0, y0, x1, y1...");
                        method(engine, C, "void fillPolygon(const array<float> &in xy, uint color, float opacity = 1)",
                               canvasFillPolygon, "A polygon, filled.");
                        method(engine, C, "int text(float x, float y, const ?&in value, uint color, Font@ font = null)",
                               canvasText, "Text, its top left at (x, y), in a font (null: the theme's); its width.");
                        return;
                }

        } // namespace

        void *makeCanvasObject() {
                void *p = malloc(sizeof(ScriptCanvas));
                return p != nullptr ? new (p) ScriptCanvas() : nullptr;
        }

        void releaseCanvasObject(void *canvas) {
                auto *c = static_cast<ScriptCanvas *>(canvas);
                if (--c->refs == 0) {
                        c->~ScriptCanvas();
                        free(c);
                }
                return;
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

        void registerDeckApi(asIScriptEngine *engine) {
                registerAll(engine);
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
