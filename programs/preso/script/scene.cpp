/**
 * @file      scene.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Shapes are filled and outlined with the core's exact coverage (core/raster); text is drawn
 * once into a coverage mask (white on black, its green the coverage) and the mask drawn moved,
 * scaled, turned and faded; code into a sprite of its colours and their coverage.
 */

#include "scene.h"
#include <math.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace preso {

        namespace {

                constexpr uint32_t MorphPoints = 96; ///< A transform's outlines, resampled.
                constexpr uint32_t MaxCodeSpans = 64;
                constexpr uint32_t MaxOutline = 256; ///< A circle's sides at most (circleSides()).

                // Outlines' points while a shape is drawn (the stack is a program's, and small). A
                // group's drawing uses none of them while its children use them.
                Point outlineLocal[MaxOutline];
                Point outlineWorld[MaxOutline];
                Point outlinePart[MaxOutline + 2];
                Point morphOutlineA[MaxOutline];
                Point morphOutlineB[MaxOutline];
                Point morphFromEven[MorphPoints];
                Point morphToEven[MorphPoints];
                Point morphBetween[MorphPoints];

                float lerp(float a, float b, float t) {
                        return a + (b - a) * t;
                }

                Color lerpColor(Color a, Color b, float t) {
                        uint32_t amount = uint32_t(t * 256 + 0.5f);
                        return mixColors(a, b, amount > 256 ? 256 : amount);
                }

                uint8_t alphaOf(float opacity) {
                        if (opacity <= 0) return 0;
                        if (opacity >= 1) return 255;
                        return uint8_t(opacity * 255 + 0.5f);
                }

                char *copyText(const char *data, uint32_t length) {
                        char *p = static_cast<char *>(malloc(length + 1));
                        if (p == nullptr) return nullptr;
                        memcpy(p, data, length);
                        p[length] = '\0';
                        return p;
                }

        } // namespace

        // ---- ScriptFont

        ScriptFont *ScriptFont::make(const FontKey &key) {
                void *p = malloc(sizeof(ScriptFont));
                if (p == nullptr) return nullptr;
                auto *f = new (p) ScriptFont();
                f->_key = key;
                strncpy(f->_family, key.family != nullptr ? key.family : "", sizeof f->_family - 1);
                f->_key.family = f->_family;
                return f;
        }

        void ScriptFont::release() {
                if (--_refs > 0) return;
                this->~ScriptFont();
                free(this);
                return;
        }

        // ---- Shape

        Shape *Shape::make(ShapeKind kind) {
                void *p = malloc(sizeof(Shape));
                return p != nullptr ? new (p) Shape(kind) : nullptr;
        }

        Shape::~Shape() {
                dropCaches();
                free(points);
                free(text);
                free(path);
                free(bars);
                if (font != nullptr) font->release();
                for (uint32_t i = 0; i < childCount; i++) children[i]->release();
                free(children);
                if (morphTo != nullptr) morphTo->release();
                if (callback != nullptr && releaseCallback != nullptr) releaseCallback(callback);
        }

        float Shape::sliderValueAt(float px) const {
                // The track is the width less a thumb's radius at each end.
                float r = height / 2, from = -width / 2 + r, to = width / 2 - r;
                float t = to > from ? (px - from) / (to - from) : 0;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                return minimum + (maximum - minimum) * t;
        }

        void Shape::release() {
                if (--_refs > 0) return;
                this->~Shape();
                free(this);
                return;
        }

        void Shape::dropCaches() {
                free(mask);
                mask = nullptr;
                maskWidth = maskHeight = 0;
                maskShown = UINT32_MAX;
                freeScaled(sprite);
                return;
        }

        bool Shape::setText(const char *data, uint32_t length) {
                char *copy = copyText(data, length);
                if (copy == nullptr) return false;
                free(text);
                text = copy;
                textLength = length;
                measuredWidth = -1;
                dropCaches();
                return true;
        }

        bool Shape::setPath(const char *data, uint32_t length) {
                char *copy = copyText(data, length);
                if (copy == nullptr) return false;
                free(path);
                path = copy;
                return true;
        }

        bool Shape::setPoints(const Point *data, uint32_t count) {
                Point *copy = nullptr;
                if (count > 0) {
                        copy = static_cast<Point *>(malloc(count * sizeof(Point)));
                        if (copy == nullptr) return false;
                        memcpy(copy, data, count * sizeof(Point));
                }
                free(points);
                points = copy;
                pointCount = count;
                return true;
        }

        bool Shape::setBars(const float *values, uint32_t count, Color barColor) {
                Bar *copy = count > 0 ? static_cast<Bar *>(calloc(count, sizeof(Bar))) : nullptr;
                if (count > 0 && copy == nullptr) return false;
                float most = 0;
                for (uint32_t i = 0; i < count; i++) {
                        copy[i].value = values[i];
                        copy[i].color = barColor;
                        if (values[i] > most) most = values[i];
                }
                free(bars);
                bars = copy;
                barCount = count;
                barMax = most > 0 ? most : 1;
                return true;
        }

        bool Shape::contains(const Shape *other) const {
                if (other == this) return true;
                for (uint32_t i = 0; i < childCount; i++) {
                        if (children[i]->contains(other)) return true;
                }
                return false;
        }

        bool Shape::addChild(Shape *child) {
                if (child == nullptr || child->contains(this)) return false;
                for (uint32_t i = 0; i < childCount; i++) {
                        if (children[i] == child) return true;
                }
                if (childCount == childCapacity) {
                        uint32_t capacity = childCapacity != 0 ? childCapacity * 2 : 8;
                        auto    *grown = static_cast<Shape **>(realloc(children, capacity * sizeof(Shape *)));
                        if (grown == nullptr) return false;
                        children = grown;
                        childCapacity = capacity;
                }
                child->addRef();
                children[childCount++] = child;
                return true;
        }

        void Shape::removeChild(Shape *child) {
                for (uint32_t i = 0; i < childCount; i++) {
                        if (children[i] != child) continue;
                        memmove(children + i, children + i + 1, (childCount - i - 1) * sizeof(Shape *));
                        childCount--;
                        child->release();
                        return;
                }
                return;
        }

        bool Shape::become(const Shape &o) {
                // Its geometry and look; it stays the object the script holds.
                if (o.text != nullptr && !setText(o.text, o.textLength)) return false;
                if (o.path != nullptr && !setPath(o.path, uint32_t(strlen(o.path)))) return false;
                if (!setPoints(o.points, o.pointCount)) return false;
                if (o.barCount > 0) {
                        Bar *copy = static_cast<Bar *>(malloc(o.barCount * sizeof(Bar)));
                        if (copy == nullptr) return false;
                        memcpy(copy, o.bars, o.barCount * sizeof(Bar));
                        free(bars);
                        bars = copy;
                        barCount = o.barCount;
                        barMax = o.barMax;
                }
                if (o.font != nullptr) o.font->addRef();
                if (font != nullptr) font->release();
                font = o.font;
                measuredWidth = -1;
                for (uint32_t i = 0; i < o.childCount; i++) addChild(o.children[i]);
                kind = o.kind;
                x = o.x;
                y = o.y;
                scale = o.scale;
                rotation = o.rotation;
                opacity = o.opacity;
                color = o.color;
                fill = o.fill;
                fillOpacity = o.fillOpacity;
                lineWidth = o.lineWidth;
                width = o.width;
                height = o.height;
                radius = o.radius;
                step = o.step;
                ends[0] = o.ends[0];
                ends[1] = o.ends[1];
                syntax = o.syntax;
                dropCaches();
                return true;
        }

        // ---- Text

        namespace {

                FontKey fontOf(SceneContext &context, const Shape &s) {
                        if (s.font != nullptr) return s.font->key();
                        if (context.theme == nullptr) return FontKey();
                        return s.kind == ShapeKind::Code ? context.theme->code.font : context.theme->body.font;
                }

                uint32_t charactersIn(StringView text) {
                        uint32_t n = 0;
                        for (uint32_t i = 0; i < text.length(); i++) n += (uint8_t(text[i]) & 0xC0) != 0x80;
                        return n;
                }

                // The first @p n characters' bytes of @p text.
                uint32_t bytesOf(StringView text, uint32_t n) {
                        uint32_t i = 0;
                        for (; i < text.length(); i++) {
                                if ((uint8_t(text[i]) & 0xC0) != 0x80) {
                                        if (n == 0) break;
                                        n--;
                                }
                        }
                        return i;
                }

                int32_t codePad(const FontKey &font) {
                        return int32_t(font.size) / 2;
                }

                // A white-on-black canvas's green, as coverage.
                void coverageOf(const Canvas &canvas, uint8_t *mask) {
                        int32_t w = canvas.width(), h = canvas.height();
                        for (int32_t y = 0; y < h; y++) {
                                const uint8_t *row = canvas.pixels() + size_t(y) * canvas.stride();
                                for (int32_t x = 0; x < w; x++) {
                                        uint32_t g;
                                        if (canvas.format() == PixelFormat::Rgb565) {
                                                uint32_t p = reinterpret_cast<const uint16_t *>(row)[x];
                                                g = (p >> 5) & 0x3F;
                                                g = (g << 2) | (g >> 4);
                                        } else {
                                                g = (reinterpret_cast<const uint32_t *>(row)[x] >> 8) & 0xFF;
                                        }
                                        mask[size_t(y) * uint32_t(w) + uint32_t(x)] = uint8_t(g);
                                }
                        }
                        return;
                }

                // Text's lines, each called with its text and number.
                template <typename F> void eachTextLine(const Shape &s, F f) {
                        StringView all(s.text != nullptr ? s.text : "", s.textLength);
                        uint32_t   n = 0; // forEachLine gives where each starts: they are counted here
                        forEachLine(all, [&](StringView line, uint32_t) { f(line, n++); });
                        return;
                }

                // Draws @p s's text (its first @p shown characters) into @p canvas in @p color (Code: its
                // colours), at its place in the whole text's box.
                void renderText(SceneContext &context, const Shape &s, Canvas &canvas, uint32_t shown, bool colored) {
                        FontKey     key = fontOf(context, s);
                        FontMetrics m = context.fonts->metrics(key);
                        bool        code = s.kind == ShapeKind::Code;
                        int32_t     pad = code ? codePad(key) : 2;
                        int32_t     inner = canvas.width() - 2 * pad;
                        int32_t     tab = context.fonts->width(key, "    ");
                        uint32_t    state = 0;
                        Color       plain = code && context.theme != nullptr ? context.theme->code.color : 0xFFFFFF;
                        eachTextLine(s, [&](StringView line, uint32_t n) {
                                int32_t y = pad + int32_t(n) * m.lineHeight;
                                if (!code) {
                                        // Centred, each line in the place it has when whole.
                                        int32_t    x = pad + (inner - context.fonts->width(key, line)) / 2;
                                        uint32_t   count = charactersIn(line);
                                        StringView part = line.left(bytesOf(line, shown));
                                        shown -= count < shown ? count : shown;
                                        if (!part.isEmpty()) context.fonts->draw(canvas, key, x, y, part, 0xFFFFFF);
                                        return;
                                }
                                SyntaxSpan spans[MaxCodeSpans];
                                uint32_t ns = s.syntax >= 0 ? context.platform->colorLine(s.syntax, line, &state, spans,
                                                                                          MaxCodeSpans)
                                                            : 0;
                                int32_t  x = pad;
                                uint32_t at = 0, span = 0;
                                while (at < line.length() && shown > 0) {
                                        uint32_t end = line.length();
                                        uint8_t  style = 0;
                                        if (span < ns && spans[span].start <= at) {
                                                end = spans[span].end < line.length() ? spans[span].end : line.length();
                                                style = spans[span].style;
                                                span++;
                                        } else if (span < ns) {
                                                end = spans[span].start;
                                        }
                                        if (end <= at) continue;
                                        Color color = 0xFFFFFF;
                                        if (colored) {
                                                color = style < 16 && context.theme != nullptr &&
                                                                        context.theme->syntax[style] != 0
                                                                ? context.theme->syntax[style]
                                                                : plain;
                                        }
                                        StringView run = line.mid(at, end - at);
                                        at = end;
                                        uint32_t k = 0;
                                        while (k < run.length() && shown > 0) {
                                                if (run[k] == '\t') {
                                                        int32_t col = x - pad;
                                                        x = pad + (tab > 0 ? (col / tab + 1) * tab : col);
                                                        k++;
                                                        shown--;
                                                        continue;
                                                }
                                                uint32_t start = k;
                                                while (k < run.length() && run[k] != '\t') k++;
                                                StringView piece = run.mid(start, k - start);
                                                uint32_t   count = charactersIn(piece);
                                                if (count > shown) piece = piece.left(bytesOf(piece, shown));
                                                shown -= count < shown ? count : shown;
                                                context.fonts->draw(canvas, key, x, y, piece, color);
                                                x += context.fonts->width(key, piece);
                                        }
                                }
                        });
                        return;
                }

                uint32_t textCharacters(const Shape &s) {
                        uint32_t n = 0;
                        eachTextLine(s, [&](StringView line, uint32_t) { n += charactersIn(line); });
                        return n;
                }

                // The text's mask (and Code's sprite) for its first @p shown characters, made if it
                // isn't already. False if there wasn't the memory.
                bool textMask(SceneContext &context, Shape &s, uint32_t shown) {
                        if (s.mask != nullptr && s.maskShown == shown) return true;
                        s.dropCaches();
                        float w = 0, h = 0;
                        s.measureText(context, &w, &h);
                        int32_t mw = int32_t(ceilf(w)), mh = int32_t(ceilf(h));
                        if (mw <= 0 || mh <= 0) return false;
                        PixelFormat format = context.platform->pixelFormat();
                        uint32_t    stride = uint32_t(mw) * bytesPerPixel(format);
                        uint8_t    *pixels = static_cast<uint8_t *>(malloc(stride * uint32_t(mh)));
                        s.mask = static_cast<uint8_t *>(malloc(size_t(mw) * size_t(mh)));
                        if (pixels == nullptr || s.mask == nullptr) {
                                free(pixels);
                                s.dropCaches();
                                return false;
                        }
                        Canvas canvas;
                        canvas.attach(pixels, stride, mw, mh, format);
                        canvas.fill(Rect{0, 0, mw, mh}, 0x000000);
                        renderText(context, s, canvas, shown, false);
                        coverageOf(canvas, s.mask);
                        bool ok = true;
                        if (s.kind == ShapeKind::Code) {
                                // Its colours over black are already times their coverage.
                                canvas.fill(Rect{0, 0, mw, mh}, 0x000000);
                                renderText(context, s, canvas, shown, true);
                                s.sprite.width = mw;
                                s.sprite.height = mh;
                                s.sprite.stride = stride;
                                s.sprite.format = format;
                                s.sprite.pixels = pixels;
                                s.sprite.alpha = static_cast<uint8_t *>(malloc(size_t(mw) * size_t(mh)));
                                if (s.sprite.alpha != nullptr) {
                                        memcpy(s.sprite.alpha, s.mask, size_t(mw) * size_t(mh));
                                } else {
                                        ok = false;
                                }
                                pixels = nullptr; // the sprite's now
                        }
                        free(pixels);
                        s.maskWidth = mw;
                        s.maskHeight = mh;
                        s.maskShown = shown;
                        if (!ok) s.dropCaches();
                        return ok;
                }

                void drawShapeText(Canvas &canvas, SceneContext &context, Shape &s, const Affine &at, Color color,
                                   float alpha) {
                        // Its text centred on @p at's origin, in @p color.
                        if (s.text == nullptr || s.textLength == 0) return;
                        uint32_t total = textCharacters(s);
                        if (!textMask(context, s, total)) return;
                        Affine from = at.after(Affine::translation(-float(s.maskWidth) / 2, -float(s.maskHeight) / 2));
                        drawMask(canvas, s.mask, s.maskWidth, s.maskHeight, uint32_t(s.maskWidth), from, color,
                                 alphaOf(alpha));
                        return;
                }

        } // namespace

        void Shape::measureText(SceneContext &context, float *w, float *h) {
                if (measuredWidth >= 0) {
                        *w = measuredWidth;
                        *h = measuredHeight;
                        return;
                }
                FontKey     key = fontOf(context, *this);
                FontMetrics m = context.fonts->metrics(key);
                int32_t     widest = 0;
                uint32_t    lines = 0;
                eachTextLine(*this, [&](StringView line, uint32_t) {
                        int32_t lw = context.fonts->width(key, line);
                        // Tabs are four spaces' stops: a little over is as good here.
                        for (uint32_t i = 0; i < line.length(); i++) {
                                if (line[i] == '\t') lw += context.fonts->width(key, "    ");
                        }
                        if (lw > widest) widest = lw;
                        lines++;
                });
                if (lines == 0) lines = 1;
                int32_t pad = kind == ShapeKind::Code ? codePad(key) : 2;
                *w = measuredWidth = float(widest + 2 * pad);
                *h = measuredHeight = float(int32_t(lines) * m.lineHeight + 2 * pad);
                return;
        }

        // ---- Outlines

        namespace {

                // A closed shape's outline in its own coordinates (from its centre), clockwise from
                // its top left; Line and Arrow aren't closed.
                uint32_t outline(const Shape &s, Point *out, uint32_t max) {
                        switch (s.kind) {
                                case ShapeKind::Rect: {
                                        if (max < 4) return 0;
                                        float w = s.width / 2, h = s.height / 2;
                                        out[0] = Point{-w, -h};
                                        out[1] = Point{w, -h};
                                        out[2] = Point{w, h};
                                        out[3] = Point{-w, h};
                                        return 4;
                                }
                                case ShapeKind::Circle: {
                                        uint32_t n = circleSides(s.radius * (s.scale > 1 ? s.scale : 1));
                                        if (n > max) n = max;
                                        ellipsePoints(out, n, 0, 0, s.radius, s.radius, -0.75f * 3.14159265f, false);
                                        return n;
                                }
                                case ShapeKind::Polygon: {
                                        uint32_t n = s.pointCount < max ? s.pointCount : max;
                                        memcpy(out, s.points, n * sizeof(Point));
                                        return n;
                                }
                                default: return 0;
                        }
                }

                bool morphable(const Shape &s) {
                        return s.kind == ShapeKind::Rect || s.kind == ShapeKind::Circle ||
                               (s.kind == ShapeKind::Polygon && s.pointCount >= 3);
                }

                // @p n points along a closed outline, evenly by length, from its first.
                void resample(const Point *in, uint32_t count, Point *out, uint32_t n) {
                        float total = 0;
                        for (uint32_t i = 0; i < count; i++) {
                                Point a = in[i], b = in[(i + 1) % count];
                                total += sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                        }
                        uint32_t seg = 0;
                        float    segStart = 0;
                        for (uint32_t k = 0; k < n; k++) {
                                float want = total * float(k) / float(n);
                                for (;;) {
                                        Point a = in[seg % count], b = in[(seg + 1) % count];
                                        float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                                        if (segStart + len >= want || seg + 1 >= count) {
                                                float t = len > 0 ? (want - segStart) / len : 0;
                                                if (t > 1) t = 1;
                                                out[k] = Point{lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
                                                break;
                                        }
                                        segStart += len;
                                        seg++;
                                }
                        }
                        return;
                }

                // The first @p fraction of a polyline's length (closed: back to its first point),
                // into @p out; its points.
                uint32_t partOf(const Point *in, uint32_t count, bool closed, float fraction, Point *out) {
                        uint32_t segments = closed ? count : count - 1;
                        float    total = 0;
                        for (uint32_t i = 0; i < segments; i++) {
                                Point a = in[i], b = in[(i + 1) % count];
                                total += sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                        }
                        float    left = total * fraction;
                        uint32_t n = 0;
                        out[n++] = in[0];
                        for (uint32_t i = 0; i < segments && left > 0; i++) {
                                Point a = in[i], b = in[(i + 1) % count];
                                float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                                if (len <= left) {
                                        out[n++] = b;
                                        left -= len;
                                } else {
                                        float t = left / len;
                                        out[n++] = Point{lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
                                        left = 0;
                                }
                        }
                        return n;
                }

                Affine localOf(float x, float y, float rotation, float scale) {
                        return Affine::translation(x, y).after(
                                Affine::rotation(rotation).after(Affine::scaling(scale, scale)));
                }

                Affine localOf(const Shape &s) {
                        return localOf(s.x, s.y, s.rotation, s.scale);
                }

                // A closed outline (in world pixels already) filled and drawn round.
                void drawOutline(Canvas &canvas, const Point *pts, uint32_t n, Color stroke, float lineWidth,
                                 Color fill, float fillAlpha, float alpha) {
                        Path path;
                        if (fillAlpha > 0) {
                                path.addPolygon(pts, n);
                                (void)fillPath(canvas, path, fill, alphaOf(fillAlpha * alpha));
                        }
                        if (lineWidth > 0) {
                                path.clear();
                                path.addStroke(pts, n, true, lineWidth);
                                (void)fillPath(canvas, path, stroke, alphaOf(alpha));
                        }
                        return;
                }

                Color themed(SceneContext &context, const char *name, Color otherwise) {
                        Color c = otherwise;
                        if (context.theme != nullptr) (void)context.theme->scriptColor(StringView(name), &c);
                        return c;
                }

                void fillBox(Canvas &canvas, const Affine &world, float x, float y, float w, float h, Color color,
                             float alpha) {
                        Point box[4] = {world.apply(Point{x, y}), world.apply(Point{x + w, y}),
                                        world.apply(Point{x + w, y + h}), world.apply(Point{x, y + h})};
                        Path  path;
                        path.addPolygon(box, 4);
                        (void)fillPath(canvas, path, color, alphaOf(alpha));
                        return;
                }

                void ringBox(Canvas &canvas, const Affine &world, float x, float y, float w, float h, float line,
                             Color color, float alpha) {
                        Point box[4] = {world.apply(Point{x, y}), world.apply(Point{x + w, y}),
                                        world.apply(Point{x + w, y + h}), world.apply(Point{x, y + h})};
                        Path  path;
                        path.addStroke(box, 4, true, line);
                        (void)fillPath(canvas, path, color, alphaOf(alpha));
                        return;
                }

                void drawShapeText(Canvas &canvas, SceneContext &context, Shape &s, const Affine &at, Color color,
                                   float alpha);

                // A widget, in the theme: the accent where it acts, the muted colour where it rests.
                void drawWidget(Canvas &canvas, SceneContext &context, Shape &s, const Affine &world, float alpha) {
                        Color accent = themed(context, "Script.Accent", 0x1A5FB4);
                        Color muted = themed(context, "Script.Muted", 0x808080);
                        Color fill = themed(context, "Script.Fill", 0xF0F0F0);
                        Color text = context.theme != nullptr ? context.theme->body.color : 0;
                        float w = s.width, h = s.height;
                        if (s.focused) ringBox(canvas, world, -w / 2 - 4, -h / 2 - 4, w + 8, h + 8, 2, accent, alpha);
                        switch (s.kind) {
                                case ShapeKind::Slider: {
                                        float r = h / 2, from = -w / 2 + r, to = w / 2 - r;
                                        float t = s.maximum > s.minimum
                                                          ? (s.value - s.minimum) / (s.maximum - s.minimum)
                                                          : 0;
                                        t = t < 0 ? 0 : t > 1 ? 1 : t;
                                        float at = from + (to - from) * t;
                                        Point track[2] = {world.apply(Point{from, 0}), world.apply(Point{to, 0})},
                                              done[2] = {world.apply(Point{from, 0}), world.apply(Point{at, 0})};
                                        float scale = sqrtf(fabsf(world.a * world.d - world.b * world.c));
                                        Path  path;
                                        path.addStroke(track, 2, false, h / 4 * scale);
                                        (void)fillPath(canvas, path, muted, alphaOf(alpha));
                                        path.clear();
                                        path.addStroke(done, 2, false, h / 4 * scale);
                                        (void)fillPath(canvas, path, accent, alphaOf(alpha));
                                        path.clear();
                                        Point c = world.apply(Point{at, 0});
                                        path.addEllipse(c.x, c.y, r * 0.8f * scale, r * 0.8f * scale);
                                        (void)fillPath(canvas, path, accent, alphaOf(alpha));
                                        break;
                                }
                                case ShapeKind::Button: {
                                        fillBox(canvas, world, -w / 2, -h / 2, w, h, s.pressed ? accent : fill, alpha);
                                        ringBox(canvas, world, -w / 2, -h / 2, w, h, 2, accent, alpha);
                                        drawShapeText(canvas, context, s, world, s.pressed ? fill : text, alpha);
                                        break;
                                }
                                case ShapeKind::Toggle: {
                                        // A box at its left, ticked when on; its label after it.
                                        float b = h * 0.7f, x0 = -w / 2;
                                        fillBox(canvas, world, x0, -b / 2, b, b, s.on ? accent : fill, alpha);
                                        ringBox(canvas, world, x0, -b / 2, b, b, 2, accent, alpha);
                                        if (s.on) {
                                                Point tick[3] = {world.apply(Point{x0 + b * 0.22f, 0}),
                                                                 world.apply(Point{x0 + b * 0.42f, b * 0.22f}),
                                                                 world.apply(Point{x0 + b * 0.78f, -b * 0.25f})};
                                                Path  path;
                                                path.addStroke(tick, 3, false, b / 7);
                                                (void)fillPath(canvas, path, fill, alphaOf(alpha));
                                        }
                                        float tw = 0, th = 0;
                                        s.measureText(context, &tw, &th);
                                        Affine label = world.after(Affine::translation(x0 + b + 8 + tw / 2, 0));
                                        drawShapeText(canvas, context, s, label, text, alpha);
                                        break;
                                }
                                default: break;
                        }
                        return;
                }

                // The shape's own drawing through @p world (its place included), @p alpha of it.
                void drawBody(Canvas &canvas, SceneContext &context, Shape &s, const Affine &world, float alpha) {
                        float scale = sqrtf(fabsf(world.a * world.d - world.b * world.c));
                        float lw = s.lineWidth * scale;
                        switch (s.kind) {
                                case ShapeKind::Rect:
                                case ShapeKind::Circle:
                                case ShapeKind::Polygon: {
                                        Point   *pts = outlineWorld;
                                        uint32_t n = outline(s, outlineLocal, MaxOutline);
                                        if (n < 2) break;
                                        for (uint32_t i = 0; i < n; i++) pts[i] = world.apply(outlineLocal[i]);
                                        if (s.reveal >= 1 && s.kind != ShapeKind::Polygon) {
                                                // A rectangle's or circle's outline: the ring between it grown and
                                                // shrunk by half the line (sharp corners, no overlaps to fill twice).
                                                if (s.fillOpacity > 0) {
                                                        Path path;
                                                        path.addPolygon(pts, n);
                                                        (void)fillPath(canvas, path, s.fill,
                                                                       alphaOf(s.fillOpacity * alpha));
                                                }
                                                if (s.lineWidth <= 0) break;
                                                float    h = s.lineWidth / 2;
                                                Path     ring;
                                                uint32_t m = 0;
                                                if (s.kind == ShapeKind::Rect) {
                                                        float w2 = s.width / 2, h2 = s.height / 2;
                                                        Point o[4] = {{-w2 - h, -h2 - h},
                                                                      {w2 + h, -h2 - h},
                                                                      {w2 + h, h2 + h},
                                                                      {-w2 - h, h2 + h}};
                                                        for (Point &p : o) p = world.apply(p);
                                                        ring.addPolygon(o, 4);
                                                        if (w2 > h && h2 > h) {
                                                                Point in[4] = {{-w2 + h, -h2 + h},
                                                                               {-w2 + h, h2 - h},
                                                                               {w2 - h, h2 - h},
                                                                               {w2 - h, -h2 + h}};
                                                                for (Point &p : in) p = world.apply(p);
                                                                ring.addPolygon(in, 4);
                                                        }
                                                } else {
                                                        // The same sides for both, the inner one the other way round.
                                                        m = n;
                                                        float ro = s.radius + h, ri = s.radius - h;
                                                        ellipsePoints(pts, m, 0, 0, ro, ro, 0, false);
                                                        for (uint32_t i = 0; i < m; i++) pts[i] = world.apply(pts[i]);
                                                        ring.addPolygon(pts, m);
                                                        if (ri > 0) {
                                                                ellipsePoints(pts, m, 0, 0, ri, ri, 0, true);
                                                                for (uint32_t i = 0; i < m; i++)
                                                                        pts[i] = world.apply(pts[i]);
                                                                ring.addPolygon(pts, m);
                                                        }
                                                }
                                                (void)fillPath(canvas, ring, s.color, alphaOf(alpha));
                                                break;
                                        }
                                        if (s.reveal >= 1) {
                                                drawOutline(canvas, pts, n, s.color, lw, s.fill, s.fillOpacity, alpha);
                                                break;
                                        }
                                        // Written: its outline drawn round so far, its inside fading in.
                                        if (s.fillOpacity > 0 && s.reveal > 0) {
                                                Path path;
                                                path.addPolygon(pts, n);
                                                (void)fillPath(canvas, path, s.fill,
                                                               alphaOf(s.fillOpacity * alpha * s.reveal));
                                        }
                                        Point   *part = outlinePart;
                                        uint32_t m = partOf(pts, n, true, s.reveal, part);
                                        if (lw > 0 && m >= 2) {
                                                Path path;
                                                path.addStroke(part, m, false, lw);
                                                (void)fillPath(canvas, path, s.color, alphaOf(alpha));
                                        }
                                        break;
                                }
                                case ShapeKind::Line:
                                case ShapeKind::Arrow: {
                                        Point a = s.ends[0], b = s.ends[1];
                                        float dx = b.x - a.x, dy = b.y - a.y;
                                        float length = sqrtf(dx * dx + dy * dy);
                                        if (length <= 0) break;
                                        float shown = length * (s.reveal < 0 ? 0 : s.reveal > 1 ? 1 : s.reveal);
                                        float ux = dx / length, uy = dy / length;
                                        Point end{a.x + ux * shown, a.y + uy * shown};
                                        float head = s.kind == ShapeKind::Arrow
                                                             ? (s.lineWidth * 5 > 16 ? s.lineWidth * 5 : 16)
                                                             : 0;
                                        if (head > shown) head = shown;
                                        Point shaft[2] = {world.apply(a), world.apply(Point{end.x - ux * head * 0.8f,
                                                                                            end.y - uy * head * 0.8f})};
                                        Path  path;
                                        path.addStroke(shaft, 2, false, lw);
                                        if (head > 0) {
                                                // The head: a triangle at the end.
                                                Point tip = end, back{end.x - ux * head, end.y - uy * head};
                                                float half = head * 0.45f;
                                                Point tri[3] = {
                                                        world.apply(tip),
                                                        world.apply(Point{back.x - uy * half, back.y + ux * half}),
                                                        world.apply(Point{back.x + uy * half, back.y - ux * half})};
                                                path.addPolygon(tri, 3);
                                        }
                                        (void)fillPath(canvas, path, s.color, alphaOf(alpha));
                                        break;
                                }
                                case ShapeKind::Text:
                                case ShapeKind::Code: {
                                        uint32_t total = textCharacters(s);
                                        uint32_t shown =
                                                s.reveal >= 1 ? total : uint32_t(float(total) * s.reveal + 0.5f);
                                        float w = 0, h = 0;
                                        s.measureText(context, &w, &h);
                                        Affine at = world.after(Affine::translation(-w / 2, -h / 2));
                                        if (s.kind == ShapeKind::Code && context.theme != nullptr &&
                                            s.fillOpacity > 0) {
                                                // Its box, as a slide's code has.
                                                Path  path;
                                                Point box[4] = {at.apply(Point{0, 0}), at.apply(Point{w, 0}),
                                                                at.apply(Point{w, h}), at.apply(Point{0, h})};
                                                path.addPolygon(box, 4);
                                                (void)fillPath(canvas, path, s.fill, alphaOf(s.fillOpacity * alpha));
                                        }
                                        if (shown == 0) break;
                                        if (s.kind == ShapeKind::Code) {
                                                if (textMask(context, s, shown))
                                                        drawPicture(canvas, s.sprite, at, alphaOf(alpha));
                                                break;
                                        }
                                        // Text's mask made whole once; written, each line shown as far as it has got.
                                        if (!textMask(context, s, total)) break;
                                        if (shown >= total) {
                                                drawMask(canvas, s.mask, s.maskWidth, s.maskHeight,
                                                         uint32_t(s.maskWidth), at, s.color, alphaOf(alpha));
                                                break;
                                        }
                                        FontKey     key = fontOf(context, s);
                                        FontMetrics m = context.fonts->metrics(key);
                                        int32_t     inner = s.maskWidth - 4;
                                        uint32_t    left = shown;
                                        eachTextLine(s, [&](StringView line, uint32_t n) {
                                                if (left == 0) return;
                                                uint32_t count = charactersIn(line);
                                                int32_t  x = 2 + (inner - context.fonts->width(key, line)) / 2;
                                                int32_t  upTo =
                                                        count <= left
                                                                 ? s.maskWidth
                                                                 : x + context.fonts->width(
                                                                              key, line.left(bytesOf(line, left)));
                                                left -= count < left ? count : left;
                                                Rect band{0, 2 + int32_t(n) * m.lineHeight, upTo, m.lineHeight};
                                                if (n == 0) {
                                                        band.y = 0;
                                                        band.height += 2;
                                                }
                                                drawMask(canvas, s.mask, s.maskWidth, s.maskHeight,
                                                         uint32_t(s.maskWidth), at, s.color, alphaOf(alpha), &band);
                                        });
                                        break;
                                }
                                case ShapeKind::Image: {
                                        if (s.path == nullptr || context.images == nullptr) break;
                                        int32_t w = int32_t(s.width + 0.5f), h = int32_t(s.height + 0.5f);
                                        if (w <= 0 || h <= 0) break;
                                        const Palette *palette =
                                                context.theme != nullptr && context.theme->palette.count > 0
                                                        ? &context.theme->palette
                                                        : nullptr;
                                        const ScaledImage *image =
                                                context.images->get(StringView(s.path), w, h, Rect{0, 0, w, h}, 0,
                                                                    palette, canvas.format());
                                        if (image == nullptr) break;
                                        Affine at = world.after(Affine::translation(-float(w) / 2, -float(h) / 2));
                                        drawPicture(canvas, *image, at, alphaOf(alpha * (s.reveal < 1 ? s.reveal : 1)));
                                        break;
                                }
                                case ShapeKind::Group: {
                                        for (uint32_t i = 0; i < s.childCount; i++) {
                                                Shape *c = s.children[i];
                                                if (c->visible) drawShape(canvas, context, *c, world, alpha);
                                        }
                                        break;
                                }
                                case ShapeKind::Bars: {
                                        if (s.barCount == 0) break;
                                        float slot = s.width / float(s.barCount);
                                        float bw = slot * 0.7f;
                                        float grow = s.reveal < 0 ? 0 : s.reveal > 1 ? 1 : s.reveal;
                                        for (uint32_t i = 0; i < s.barCount; i++) {
                                                const Bar &bar = s.bars[i];
                                                float      bh = bar.value / s.barMax * s.height * grow;
                                                if (bh <= 0) continue;
                                                float x0 =
                                                        -s.width / 2 + (float(i) + bar.offset) * slot + (slot - bw) / 2;
                                                float y1 = s.height / 2;
                                                Point box[4] = {world.apply(Point{x0, y1 - bh}),
                                                                world.apply(Point{x0 + bw, y1 - bh}),
                                                                world.apply(Point{x0 + bw, y1}),
                                                                world.apply(Point{x0, y1})};
                                                Path  path;
                                                path.addPolygon(box, 4);
                                                (void)fillPath(canvas, path, bar.color, alphaOf(alpha));
                                        }
                                        break;
                                }
                                case ShapeKind::Slider:
                                case ShapeKind::Button:
                                case ShapeKind::Toggle: drawWidget(canvas, context, s, world, alpha); break;
                                case ShapeKind::Grid: {
                                        if (s.step <= 0) break;
                                        Path  path;
                                        float w = s.width / 2, h = s.height / 2;
                                        float lineW = (s.lineWidth > 0 ? s.lineWidth : 1) * scale;
                                        float sx = s.step;
                                        // From the centre out, so its middle lines are on the axes.
                                        const float signs[2] = {1.0f, -1.0f};
                                        for (float gx = 0; gx <= w; gx += sx) {
                                                for (float sign : signs) {
                                                        if (gx == 0 && sign < 0) continue;
                                                        Point l[2] = {world.apply(Point{sign * gx, -h}),
                                                                      world.apply(Point{sign * gx, h})};
                                                        path.addStroke(l, 2, false, lineW);
                                                }
                                        }
                                        for (float gy = 0; gy <= h; gy += sx) {
                                                for (float sign : signs) {
                                                        if (gy == 0 && sign < 0) continue;
                                                        Point l[2] = {world.apply(Point{-w, sign * gy}),
                                                                      world.apply(Point{w, sign * gy})};
                                                        path.addStroke(l, 2, false, lineW);
                                                }
                                        }
                                        (void)fillPath(canvas, path, s.color,
                                                       alphaOf(alpha * (s.reveal < 1 ? s.reveal : 1)));
                                        break;
                                }
                        }
                        return;
                }

                // The box round the shape's own drawing, in its own coordinates.
                void localBox(SceneContext &context, Shape &s, float *x0, float *y0, float *x1, float *y1) {
                        float pad = s.lineWidth;
                        float w = 0, h = 0;
                        switch (s.kind) {
                                case ShapeKind::Rect:
                                case ShapeKind::Image:
                                case ShapeKind::Bars:
                                case ShapeKind::Grid:
                                        w = s.width / 2 + pad;
                                        h = s.height / 2 + pad;
                                        break;
                                case ShapeKind::Slider:
                                case ShapeKind::Button:
                                case ShapeKind::Toggle:
                                        // Its focus ring, a few pixels round it.
                                        w = s.width / 2 + 6;
                                        h = s.height / 2 + 6;
                                        break;
                                case ShapeKind::Circle: w = h = s.radius + pad; break;
                                case ShapeKind::Text:
                                case ShapeKind::Code:
                                        s.measureText(context, &w, &h);
                                        w = w / 2 + 2;
                                        h = h / 2 + 2;
                                        break;
                                case ShapeKind::Line:
                                case ShapeKind::Arrow: {
                                        float head = s.kind == ShapeKind::Arrow
                                                             ? (s.lineWidth * 5 > 16 ? s.lineWidth * 5 : 16)
                                                             : 0;
                                        pad = pad + head;
                                        *x0 = (s.ends[0].x < s.ends[1].x ? s.ends[0].x : s.ends[1].x) - pad;
                                        *x1 = (s.ends[0].x > s.ends[1].x ? s.ends[0].x : s.ends[1].x) + pad;
                                        *y0 = (s.ends[0].y < s.ends[1].y ? s.ends[0].y : s.ends[1].y) - pad;
                                        *y1 = (s.ends[0].y > s.ends[1].y ? s.ends[0].y : s.ends[1].y) + pad;
                                        return;
                                }
                                case ShapeKind::Polygon: {
                                        *x0 = *y0 = *x1 = *y1 = 0;
                                        for (uint32_t i = 0; i < s.pointCount; i++) {
                                                const Point &p = s.points[i];
                                                if (i == 0 || p.x < *x0) *x0 = p.x;
                                                if (i == 0 || p.x > *x1) *x1 = p.x;
                                                if (i == 0 || p.y < *y0) *y0 = p.y;
                                                if (i == 0 || p.y > *y1) *y1 = p.y;
                                        }
                                        *x0 -= pad;
                                        *y0 -= pad;
                                        *x1 += pad;
                                        *y1 += pad;
                                        return;
                                }
                                case ShapeKind::Group: break; // its children's (shapeBounds)
                        }
                        *x0 = -w;
                        *y0 = -h;
                        *x1 = w;
                        *y1 = h;
                        return;
                }

        } // namespace

        void drawShape(Canvas &canvas, SceneContext &context, Shape &s, const Affine &parent, float opacity) {
                float alpha = opacity * s.opacity;
                if (!s.visible || alpha <= 0) return;
                Shape *to = s.morphTo;
                if (to == nullptr || s.morph <= 0) {
                        drawBody(canvas, context, s, parent.after(localOf(s)), alpha);
                        return;
                }
                float  t = s.morph > 1 ? 1 : s.morph;
                Affine from = parent.after(localOf(s)), into = parent.after(localOf(*to));
                if (morphable(s) && morphable(*to)) {
                        // Outline into outline: each resampled, point to point.
                        Point *a = morphOutlineA, *b = morphOutlineB, *pa = morphFromEven, *pb = morphToEven,
                              *pts = morphBetween;
                        uint32_t na = outline(s, a, MaxOutline), nb = outline(*to, b, MaxOutline);
                        for (uint32_t i = 0; i < na; i++) a[i] = from.apply(a[i]);
                        for (uint32_t i = 0; i < nb; i++) b[i] = into.apply(b[i]);
                        resample(a, na, pa, MorphPoints);
                        resample(b, nb, pb, MorphPoints);
                        for (uint32_t i = 0; i < MorphPoints; i++)
                                pts[i] = Point{lerp(pa[i].x, pb[i].x, t), lerp(pa[i].y, pb[i].y, t)};
                        float scale = lerp(s.scale, to->scale, t);
                        drawOutline(canvas, pts, MorphPoints, lerpColor(s.color, to->color, t),
                                    lerp(s.lineWidth, to->lineWidth, t) * scale, lerpColor(s.fill, to->fill, t),
                                    lerp(s.fillOpacity, to->fillOpacity, t), lerp(alpha, opacity * to->opacity, t));
                        return;
                }
                // Anything else: one fades into the other as both move to where it ends.
                Affine between = parent.after(localOf(lerp(s.x, to->x, t), lerp(s.y, to->y, t),
                                                      lerp(s.rotation, to->rotation, t), lerp(s.scale, to->scale, t)));
                drawBody(canvas, context, s, between, alpha * (1 - t));
                drawBody(canvas, context, *to, between, opacity * to->opacity * t);
                return;
        }

        Rect shapeBounds(SceneContext &context, Shape &s, const Affine &parent) {
                if (!s.visible || s.opacity <= 0) return Rect{};
                Affine world = parent.after(localOf(s));
                Rect   r;
                if (s.kind == ShapeKind::Group) {
                        for (uint32_t i = 0; i < s.childCount; i++)
                                r = uniteRects(r, shapeBounds(context, *s.children[i], world));
                } else {
                        float x0, y0, x1, y1;
                        localBox(context, s, &x0, &y0, &x1, &y1);
                        r = transformedBounds(world, x0, y0, x1 - x0, y1 - y0);
                }
                if (s.morphTo != nullptr && s.morph > 0) {
                        // Between the two: what both cover, and the way between.
                        Rect to = shapeBounds(context, *s.morphTo, parent);
                        r = uniteRects(r, to);
                }
                return r;
        }

        uint32_t shapeSignature(const Shape &s) {
                // What it looks like, as a number: FNV-1a over its fields (its children's, its bars').
                uint32_t h = 2166136261u;
                auto     word = [&h](uint32_t v) {
                        for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 16777619u;
                };
                auto real = [&word](float f) {
                        uint32_t v;
                        memcpy(&v, &f, sizeof v);
                        word(v);
                };
                word(uint32_t(s.kind));
                real(s.x);
                real(s.y);
                real(s.scale);
                real(s.rotation);
                real(s.opacity);
                word(s.visible ? 1 : 0);
                word(s.color);
                word(s.fill);
                real(s.fillOpacity);
                real(s.lineWidth);
                real(s.reveal);
                real(s.width);
                real(s.height);
                real(s.radius);
                real(s.morph);
                word(s.textLength);
                word(uint32_t(reinterpret_cast<uintptr_t>(s.text)));
                for (uint32_t i = 0; i < s.barCount; i++) {
                        real(s.bars[i].value);
                        real(s.bars[i].offset);
                        word(s.bars[i].color);
                }
                real(s.value);
                word((s.on ? 1u : 0u) | (s.pressed ? 2u : 0u) | (s.focused ? 4u : 0u));
                for (uint32_t i = 0; i < s.childCount; i++) word(shapeSignature(*s.children[i]));
                if (s.morphTo != nullptr) word(shapeSignature(*s.morphTo));
                return h;
        }

        // ---- Scene

        bool Scene::add(Shape *shape) {
                if (shape == nullptr || has(shape)) return true;
                if (_count == _capacity) {
                        uint32_t capacity = _capacity != 0 ? _capacity * 2 : 16;
                        auto    *grown = static_cast<Shape **>(realloc(_shapes, capacity * sizeof(Shape *)));
                        if (grown == nullptr) return false;
                        _shapes = grown;
                        _capacity = capacity;
                }
                shape->addRef();
                _shapes[_count++] = shape;
                return true;
        }

        void Scene::remove(Shape *shape) {
                for (uint32_t i = 0; i < _count; i++) {
                        if (_shapes[i] != shape) continue;
                        memmove(_shapes + i, _shapes + i + 1, (_count - i - 1) * sizeof(Shape *));
                        _count--;
                        shape->release();
                        return;
                }
                return;
        }

        bool Scene::has(const Shape *shape) const {
                for (uint32_t i = 0; i < _count; i++) {
                        if (_shapes[i] == shape) return true;
                }
                return false;
        }

        void Scene::clear() {
                // Released last first: a group's children may be the scene's too.
                while (_count > 0) _shapes[--_count]->release();
                free(_shapes);
                _shapes = nullptr;
                _capacity = 0;
                return;
        }

        void Scene::draw(Canvas &canvas, SceneContext &context, int32_t x, int32_t y) {
                Affine at = Affine::translation(float(x), float(y));
                for (uint32_t i = 0; i < _count; i++) drawShape(canvas, context, *_shapes[i], at, 1);
                return;
        }

        Rect Scene::bounds(SceneContext &context) {
                Rect r;
                for (uint32_t i = 0; i < _count; i++) r = uniteRects(r, shapeBounds(context, *_shapes[i], Affine()));
                return r;
        }

} // namespace preso
