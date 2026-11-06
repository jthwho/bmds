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
#include <stdio.h>
#include <new>
#include <stdlib.h>
#include <string.h>

namespace preso {

        namespace {

                constexpr uint32_t MorphPoints = 96; ///< A transform's outlines, resampled.
                constexpr uint32_t MaxCodeSpans = 64;
                constexpr uint32_t MaxOutline = 512; ///< An outline's points (a circle's 256 sides; curves).
                constexpr float    Pi = 3.14159265f;

                // Outlines' points while a shape is drawn (the stack is a program's, and small). A
                // group's drawing uses none of them while its children use them. Allocated when a
                // shape is first drawn, not in the program's .bss: angel has this code whether its
                // script draws a scene or not.
                struct Scratch {
                                Point outlineLocal[MaxOutline];
                                Point outlineWorld[MaxOutline];
                                Point outlinePart[MaxOutline + 2];
                                Point morphOutlineA[MaxOutline];
                                Point morphOutlineB[MaxOutline];
                                Point morphFromEven[MorphPoints];
                                Point morphToEven[MorphPoints];
                                Point morphBetween[MorphPoints];
                };
                Scratch *scratchSpace = nullptr;

                Scratch *scratch() {
                        if (scratchSpace == nullptr) scratchSpace = static_cast<Scratch *>(malloc(sizeof(Scratch)));
                        return scratchSpace;
                }

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
                for (uint32_t i = 0; i < childCount; i++) {
                        if (children[i]->parent == this) children[i]->parent = nullptr;
                        children[i]->release();
                }
                free(children);
                if (morphTo != nullptr) morphTo->release();
                if (callback != nullptr && releaseCallback != nullptr) releaseCallback(callback);
                if (updater != nullptr && releaseUpdater != nullptr) releaseUpdater(updater);
                if (anchor != nullptr) anchor->release();
                free(edges);
                free(iconPixels);
        }

        bool isComposite(ShapeKind kind) {
                switch (kind) {
                        case ShapeKind::Group:
                        case ShapeKind::Array:
                        case ShapeKind::Table:
                        case ShapeKind::Graph:
                        case ShapeKind::Brace:
                        case ShapeKind::Callout:
                        case ShapeKind::Label:
                        case ShapeKind::BarChart:
                        case ShapeKind::PieChart:
                        case ShapeKind::LineChart: return true;
                        default: return false;
                }
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
                child->parent = this;
                children[childCount++] = child;
                return true;
        }

        void Shape::removeChild(Shape *child) {
                for (uint32_t i = 0; i < childCount; i++) {
                        if (children[i] != child) continue;
                        if (child->parent == this) child->parent = nullptr;
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
                copyLook(o);
                dropCaches();
                return true;
        }

        void Shape::copyLook(const Shape &o) {
                // Every plain field of its place, look and geometry (not what it owns).
                kind = o.kind;
                x = o.x;
                y = o.y;
                scale = o.scale;
                rotation = o.rotation;
                opacity = o.opacity;
                visible = o.visible;
                color = o.color;
                fill = o.fill;
                fillOpacity = o.fillOpacity;
                lineWidth = o.lineWidth;
                reveal = o.reveal;
                width = o.width;
                height = o.height;
                radius = o.radius;
                step = o.step;
                ends[0] = o.ends[0];
                ends[1] = o.ends[1];
                head = o.head;
                tail = o.tail;
                headSize = o.headSize;
                bend = o.bend;
                dash = o.dash;
                dashGap = o.dashGap;
                cornerRadius = o.cornerRadius;
                start = o.start;
                sweep = o.sweep;
                innerRadius = o.innerRadius;
                sides = o.sides;
                closed = o.closed;
                smooth = o.smooth;
                decimals = o.decimals;
                xMin = o.xMin;
                xMax = o.xMax;
                yMin = o.yMin;
                yMax = o.yMax;
                xStep = o.xStep;
                yStep = o.yStep;
                minimum = o.minimum;
                maximum = o.maximum;
                value = o.value;
                on = o.on;
                syntax = o.syntax;
                rows = o.rows;
                columns = o.columns;
                indices = o.indices;
                header = o.header;
                anchorSide = o.anchorSide;
                anchorGap = o.anchorGap;
                focus = o.focus;
                dim = o.dim;
                return;
        }

        bool Shape::insertChild(Shape *child, uint32_t at) {
                if (!addChild(child)) return false;
                if (at >= childCount) return true;
                Shape *last = children[childCount - 1];
                memmove(children + at + 1, children + at, (childCount - 1 - at) * sizeof(Shape *));
                children[at] = last;
                return true;
        }

        void Shape::swapChildren(uint32_t i, uint32_t j) {
                if (i >= childCount || j >= childCount) return;
                Shape *t = children[i];
                children[i] = children[j];
                children[j] = t;
                return;
        }

        bool Shape::setEdges(const uint16_t *data, uint32_t count) {
                uint16_t *copy = nullptr;
                if (count > 0) {
                        copy = static_cast<uint16_t *>(malloc(count * 3 * sizeof(uint16_t)));
                        if (copy == nullptr) return false;
                        memcpy(copy, data, count * 3 * sizeof(uint16_t));
                }
                free(edges);
                edges = copy;
                return true;
        }

        Shape *Shape::clone() const {
                Shape *c = make(kind);
                if (c == nullptr) return nullptr;
                c->copyLook(*this);
                bool ok = (text == nullptr || c->setText(text, textLength)) &&
                          (path == nullptr || c->setPath(path, uint32_t(strlen(path)))) &&
                          c->setPoints(points, pointCount);
                if (ok && barCount > 0) {
                        c->bars = static_cast<Bar *>(malloc(barCount * sizeof(Bar)));
                        ok = c->bars != nullptr;
                        if (ok) {
                                memcpy(c->bars, bars, barCount * sizeof(Bar));
                                c->barCount = barCount;
                                c->barMax = barMax;
                        }
                }
                if (font != nullptr) {
                        font->addRef();
                        c->font = font;
                }
                if (ok && edges != nullptr) ok = c->setEdges(edges, columns);
                if (ok && iconPixels != nullptr) {
                        size_t bytes = size_t(iconSide) * size_t(iconSide) * 4;
                        c->iconPixels = static_cast<uint32_t *>(malloc(bytes));
                        ok = c->iconPixels != nullptr;
                        if (ok) {
                                memcpy(c->iconPixels, iconPixels, bytes);
                                c->iconSide = iconSide;
                        }
                }
                if (anchor != nullptr) {
                        anchor->addRef();
                        c->anchor = anchor;
                }
                for (uint32_t i = 0; ok && i < childCount; i++) {
                        Shape *child = children[i]->clone();
                        ok = child != nullptr && c->addChild(child);
                        if (child != nullptr) child->release(); // the group's reference kept
                }
                if (!ok) {
                        c->release();
                        return nullptr;
                }
                return c;
        }

        bool Shape::setNumber(float v) {
                value = v;
                char shown[48];
                int  n = snprintf(shown, sizeof shown, "%.*f",
                                 decimals < 0   ? 0
                                  : decimals > 9 ? 9
                                                 : decimals,
                                  double(v));
                // -0 shows as 0.
                if (n > 1 && shown[0] == '-') {
                        bool zero = true;
                        for (int i = 1; i < n; i++) zero &= shown[i] == '0' || shown[i] == '.';
                        if (zero) memmove(shown, shown + 1, size_t(n--));
                }
                if (text != nullptr && textLength == uint32_t(n) && memcmp(text, shown, size_t(n)) == 0) return true;
                return setText(shown, uint32_t(n));
        }

        // ---- Text

        namespace {

                FontKey sizedBy(FontKey key, float by) {
                        float size = float(key.size) * by;
                        key.size = uint16_t(size < 1 ? 1 : size > 4000 ? 4000 : lroundf(size));
                        return key;
                }

        } // namespace

        FontKey sceneFont(const SceneContext &context, FontKey key) {
                return context.fit == 1 ? key : sizedBy(key, 1 / context.fit);
        }

        FontKey rasterFont(const SceneContext &context, FontKey key) {
                return context.fit == 1 ? key : sizedBy(key, context.fit);
        }

        namespace {

                // The font @p s is in, in the scene's units.
                FontKey fontOf(SceneContext &context, const Shape &s) {
                        if (s.font != nullptr) return s.font->key();
                        if (context.theme == nullptr) return FontKey();
                        return sceneFont(context, s.kind == ShapeKind::Code ? context.theme->code.font
                                                                            : context.theme->body.font);
                }

                // The font @p s's text is made in: as it shows.
                FontKey shownFontOf(SceneContext &context, const Shape &s) {
                        return rasterFont(context, fontOf(context, s));
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

                // The size of @p s's text as it shows (in the region's pixels: made at its font times
                // the scene's fit), its padding included.
                void shownTextSize(SceneContext &context, const Shape &s, int32_t *w, int32_t *h) {
                        FontKey     key = shownFontOf(context, s);
                        FontMetrics m = context.fonts->metrics(key);
                        int32_t     widest = 0;
                        uint32_t    lines = 0;
                        eachTextLine(s, [&](StringView line, uint32_t) {
                                int32_t lw = context.fonts->width(key, line);
                                // Tabs are four spaces' stops: a little over is as good here.
                                for (uint32_t i = 0; i < line.length(); i++) {
                                        if (line[i] == '\t') lw += context.fonts->width(key, "    ");
                                }
                                if (lw > widest) widest = lw;
                                lines++;
                        });
                        if (lines == 0) lines = 1;
                        int32_t pad = s.kind == ShapeKind::Code ? codePad(key) : 2;
                        *w = widest + 2 * pad;
                        *h = int32_t(lines) * m.lineHeight + 2 * pad;
                        return;
                }

                // Draws @p s's text (its first @p shown characters) into @p canvas in @p color (Code: its
                // colours), at its place in the whole text's box.
                void renderText(SceneContext &context, const Shape &s, Canvas &canvas, uint32_t shown, bool colored) {
                        FontKey     key = shownFontOf(context, s);
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

                // How far @p text reaches in a code's line: tabs to their stops, four spaces apart.
                int32_t codeWidth(SceneContext &context, const FontKey &key, StringView text, int32_t tab) {
                        int32_t  x = 0;
                        uint32_t k = 0;
                        while (k < text.length()) {
                                if (text[k] == '\t') {
                                        x = tab > 0 ? (x / tab + 1) * tab : x;
                                        k++;
                                        continue;
                                }
                                uint32_t start = k;
                                while (k < text.length() && text[k] != '\t') k++;
                                x += context.fonts->width(key, text.mid(start, k - start));
                        }
                        return x;
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
                        int32_t mw = 0, mh = 0;
                        shownTextSize(context, s, &mw, &mh);
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
                        Affine from =
                                at.after(Affine::scaling(1 / context.fit, 1 / context.fit))
                                        .after(Affine::translation(-float(s.maskWidth) / 2, -float(s.maskHeight) / 2));
                        drawMask(canvas, s.mask, s.maskWidth, s.maskHeight, uint32_t(s.maskWidth), from, color,
                                 alphaOf(alpha));
                        return;
                }

        } // namespace

        void Shape::measureText(SceneContext &context, float *w, float *h) {
                // Its text's size as it shows, in the scene's units.
                if (measuredWidth < 0) {
                        int32_t sw = 0, sh = 0;
                        shownTextSize(context, *this, &sw, &sh);
                        measuredWidth = float(sw) / context.fit;
                        measuredHeight = float(sh) / context.fit;
                }
                *w = measuredWidth;
                *h = measuredHeight;
                return;
        }

        // ---- Outlines

        namespace {

                // Part of a circle round (@p cx, @p cy): from @p start degrees, @p sweep more (both
                // clockwise from 3 o'clock), as many points as look round at @p scale; into @p out.
                uint32_t arcPoints(Point *out, uint32_t max, float cx, float cy, float r, float start, float sweep,
                                   float scale) {
                        if (max < 2) return 0;
                        float    turn = fabsf(sweep) > 360 ? 360 : fabsf(sweep);
                        uint32_t n = uint32_t(float(circleSides(fabsf(r) * (scale > 1 ? scale : 1))) * turn / 360) + 2;
                        if (n > max) n = max;
                        float a = start * Pi / 180, step = (sweep < 0 ? -turn : turn) * Pi / 180 / float(n - 1);
                        float c = cosf(a), sn = sinf(a), cs = cosf(step), ss = sinf(step);
                        for (uint32_t i = 0; i < n; i++) {
                                out[i] = Point{cx + r * c, cy + r * sn};
                                float nc = c * cs - sn * ss;
                                sn = sn * cs + c * ss;
                                c = nc;
                        }
                        return n;
                }

                // A smooth curve through @p count points (Catmull-Rom), @p closed back to the first.
                uint32_t smoothed(const Point *in, uint32_t count, bool closed, Point *out, uint32_t max) {
                        if (count < 3) {
                                uint32_t n = count < max ? count : max;
                                memcpy(out, in, n * sizeof(Point));
                                return n;
                        }
                        uint32_t segments = closed ? count : count - 1;
                        uint32_t sub = (max - 1) / segments;
                        if (sub > 8) sub = 8;
                        if (sub < 1) sub = 1;
                        uint32_t n = 0;
                        auto     at = [&](int32_t i) {
                                if (closed) return in[(i % int32_t(count) + int32_t(count)) % int32_t(count)];
                                return in[i < 0 ? 0 : i >= int32_t(count) ? count - 1 : uint32_t(i)];
                        };
                        for (uint32_t i = 0; i < segments; i++) {
                                Point p0 = at(int32_t(i) - 1), p1 = at(int32_t(i)), p2 = at(int32_t(i) + 1),
                                      p3 = at(int32_t(i) + 2);
                                for (uint32_t k = 0; k < sub && n < max; k++) {
                                        float t = float(k) / float(sub), t2 = t * t, t3 = t2 * t;
                                        auto  f = [&](float a0, float a1, float a2, float a3) {
                                                return 0.5f *
                                                       (2 * a1 + (a2 - a0) * t + (2 * a0 - 5 * a1 + 4 * a2 - a3) * t2 +
                                                        (3 * a1 - a0 - 3 * a2 + a3) * t3);
                                        };
                                        out[n++] = Point{f(p0.x, p1.x, p2.x, p3.x), f(p0.y, p1.y, p2.y, p3.y)};
                                }
                        }
                        if (!closed && n < max) out[n++] = in[count - 1];
                        return n;
                }

                bool isOpen(const Shape &s) {
                        return s.kind == ShapeKind::Line || s.kind == ShapeKind::Arrow || s.kind == ShapeKind::Arc ||
                               (s.kind == ShapeKind::Curve && !s.closed);
                }

                // A closed shape's outline in its own coordinates (from its centre), clockwise from
                // its top left; open ones have none (openPath()).
                uint32_t outline(const Shape &s, Point *out, uint32_t max) {
                        switch (s.kind) {
                                case ShapeKind::Rect: {
                                        float w = s.width / 2, h = s.height / 2;
                                        float r = s.cornerRadius;
                                        if (r > w) r = w;
                                        if (r > h) r = h;
                                        if (r <= 0.25f) {
                                                if (max < 4) return 0;
                                                out[0] = Point{-w, -h};
                                                out[1] = Point{w, -h};
                                                out[2] = Point{w, h};
                                                out[3] = Point{-w, h};
                                                return 4;
                                        }
                                        // Each corner a quarter circle, clockwise from the top left's.
                                        const Point corners[4] = {
                                                {-w + r, -h + r}, {w - r, -h + r}, {w - r, h - r}, {-w + r, h - r}};
                                        uint32_t n = 0;
                                        for (uint32_t k = 0; k < 4; k++) {
                                                n += arcPoints(out + n, (max - n) / (4 - k), corners[k].x, corners[k].y,
                                                               r, 180 + 90 * float(k), 90, s.scale);
                                        }
                                        return n;
                                }
                                case ShapeKind::Circle: {
                                        uint32_t n = circleSides(s.radius * (s.scale > 1 ? s.scale : 1));
                                        if (n > max) n = max;
                                        ellipsePoints(out, n, 0, 0, s.radius, s.radius, -0.75f * Pi, false);
                                        return n;
                                }
                                case ShapeKind::Ellipse: {
                                        float    rx = s.width / 2, ry = s.height / 2;
                                        uint32_t n = circleSides((rx > ry ? rx : ry) * (s.scale > 1 ? s.scale : 1));
                                        if (n > max) n = max;
                                        ellipsePoints(out, n, 0, 0, rx, ry, -0.75f * Pi, false);
                                        return n;
                                }
                                case ShapeKind::Sector: {
                                        // Its outer arc, then its inner one back (or its centre).
                                        uint32_t n = arcPoints(out, max / 2, 0, 0, s.radius, s.start, s.sweep, s.scale);
                                        if (s.innerRadius > 0) {
                                                n += arcPoints(out + n, max - n, 0, 0, s.innerRadius, s.start + s.sweep,
                                                               -s.sweep, s.scale);
                                        } else if (n < max && fabsf(s.sweep) < 360) {
                                                out[n++] = Point{0, 0};
                                        }
                                        return n;
                                }
                                case ShapeKind::Regular:
                                case ShapeKind::Star: {
                                        // From its top, clockwise; a star's points between its radii.
                                        bool     star = s.kind == ShapeKind::Star;
                                        uint32_t corners = s.sides < 3 ? 3 : s.sides;
                                        uint32_t n = star ? corners * 2 : corners;
                                        if (n > max) n = max;
                                        for (uint32_t i = 0; i < n; i++) {
                                                float a = -Pi / 2 + 2 * Pi * float(i) / float(n);
                                                float r = star && (i & 1) != 0 ? s.innerRadius : s.radius;
                                                out[i] = Point{r * cosf(a), r * sinf(a)};
                                        }
                                        return n;
                                }
                                case ShapeKind::Polygon: {
                                        uint32_t n = s.pointCount < max ? s.pointCount : max;
                                        memcpy(out, s.points, n * sizeof(Point));
                                        return n;
                                }
                                case ShapeKind::Curve: {
                                        if (!s.closed) return 0;
                                        if (!s.smooth) {
                                                uint32_t n = s.pointCount < max ? s.pointCount : max;
                                                memcpy(out, s.points, n * sizeof(Point));
                                                return n;
                                        }
                                        return smoothed(s.points, s.pointCount, true, out, max);
                                }
                                default: return 0;
                        }
                }

                // An open shape's path in its own coordinates (from its centre): a line (bent: an
                // arc from its start to its end), an arc, an open curve.
                uint32_t openPath(const Shape &s, Point *out, uint32_t max) {
                        switch (s.kind) {
                                case ShapeKind::Line:
                                case ShapeKind::Arrow: {
                                        Point a = s.ends[0], b = s.ends[1];
                                        float theta = s.bend > 350 ? 350 : s.bend < -350 ? -350 : s.bend;
                                        float dx = b.x - a.x, dy = b.y - a.y, chord = sqrtf(dx * dx + dy * dy);
                                        if (fabsf(theta) < 0.5f || chord <= 0 || max < 3) {
                                                if (max < 2) return 0;
                                                out[0] = a;
                                                out[1] = b;
                                                return 2;
                                        }
                                        // The circle through both ends whose arc between them turns theta:
                                        // its centre off the chord's middle, on its right going a to b.
                                        float    t = theta * Pi / 180, r = chord / (2 * sinf(t / 2));
                                        float    nx = -dy / chord, ny = dx / chord, d = r * cosf(t / 2);
                                        float    cx = (a.x + b.x) / 2 + nx * d, cy = (a.y + b.y) / 2 + ny * d;
                                        float    from = atan2f(a.y - cy, a.x - cx) * 180 / Pi;
                                        uint32_t n = arcPoints(out, max, cx, cy, fabsf(r), from, theta, s.scale);
                                        if (n >= 2) {
                                                out[0] = a; // its ends exactly
                                                out[n - 1] = b;
                                        }
                                        return n;
                                }
                                case ShapeKind::Arc:
                                        return arcPoints(out, max, 0, 0, s.radius, s.start, s.sweep, s.scale);
                                case ShapeKind::Curve: {
                                        if (s.closed) return 0;
                                        if (!s.smooth) {
                                                uint32_t n = s.pointCount < max ? s.pointCount : max;
                                                memcpy(out, s.points, n * sizeof(Point));
                                                return n;
                                        }
                                        return smoothed(s.points, s.pointCount, false, out, max);
                                }
                                default: return 0;
                        }
                }

                bool morphable(const Shape &s) {
                        switch (s.kind) {
                                case ShapeKind::Rect:
                                case ShapeKind::Circle:
                                case ShapeKind::Ellipse:
                                case ShapeKind::Sector:
                                case ShapeKind::Regular:
                                case ShapeKind::Star: return true;
                                case ShapeKind::Polygon: return s.pointCount >= 3;
                                case ShapeKind::Curve: return s.closed && s.pointCount >= 3;
                                default: return false;
                        }
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

                // A line @p width wide along @p count points into @p path, in dashes @p dash long
                // with @p gap between (dash 0: whole).
                void strokeInto(Path &path, const Point *pts, uint32_t count, bool closed, float width, float dash,
                                float gap) {
                        if (count < 2 || width <= 0) return;
                        if (dash <= 0.5f) {
                                path.addStroke(pts, count, closed, width);
                                return;
                        }
                        if (gap <= 0) gap = dash;
                        // Along the segments, on for dash, off for gap; each dash a stroke of its own.
                        constexpr uint32_t Max = 64;
                        Point              piece[Max];
                        uint32_t           n = 0;
                        bool               on = true;
                        float              left = dash;
                        uint32_t           segments = closed ? count : count - 1;
                        for (uint32_t i = 0; i < segments; i++) {
                                Point a = pts[i], b = pts[(i + 1) % count];
                                float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                                float done = 0;
                                if (on && n == 0) piece[n++] = a;
                                while (len - done > left) {
                                        done += left;
                                        float t = done / len;
                                        Point p{lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
                                        if (on) {
                                                piece[n++] = p;
                                                path.addStroke(piece, n, false, width);
                                                n = 0;
                                        } else {
                                                piece[n++] = p;
                                        }
                                        on = !on;
                                        left = on ? dash : gap;
                                }
                                left -= len - done;
                                if (on) {
                                        piece[n++] = b;
                                        if (n == Max) {
                                                path.addStroke(piece, n, false, width);
                                                piece[0] = b;
                                                n = 1;
                                        }
                                }
                        }
                        if (on && n >= 2) path.addStroke(piece, n, false, width);
                        return;
                }

                // A closed outline (in world pixels already) filled and drawn round.
                void drawOutline(Canvas &canvas, const Point *pts, uint32_t n, Color stroke, float lineWidth,
                                 Color fill, float fillAlpha, float alpha, float dash = 0, float gap = 0) {
                        Path path;
                        if (fillAlpha > 0) {
                                path.addPolygon(pts, n);
                                (void)fillPath(canvas, path, fill, alphaOf(fillAlpha * alpha));
                        }
                        if (lineWidth > 0) {
                                path.clear();
                                strokeInto(path, pts, n, true, lineWidth, dash, gap);
                                (void)fillPath(canvas, path, stroke, alphaOf(alpha));
                        }
                        return;
                }

                // A head's length in its own pixels: as asked, or from the line's width.
                float headLength(const Shape &s) {
                        if (s.headSize > 0) return s.headSize;
                        return s.lineWidth * 5 > 16 ? s.lineWidth * 5 : 16;
                }

                // How far a line stops short of its end for a head of @p type @p length long (world
                // pixels), so it doesn't show past the head's point.
                float headInset(ArrowHead type, float length) {
                        switch (type) {
                                case ArrowHead::Triangle: return length * 0.6f;
                                case ArrowHead::Stealth: return length * 0.55f;
                                case ArrowHead::Diamond: return length * 0.5f;
                                default: return 0;
                        }
                }

                // A head of @p type into @p path at @p tip, @p dir the way the line goes out of it
                // (a unit), @p length long, its lines @p width wide: every part turned as strokes
                // are, so where it meets the line they add up.
                void addHead(Path &path, ArrowHead type, Point tip, Point dir, float length, float width) {
                        Point side{-dir.y, dir.x};
                        auto  at = [&](float back, float across) {
                                return Point{tip.x - dir.x * back + side.x * across,
                                             tip.y - dir.y * back + side.y * across};
                        };
                        switch (type) {
                                case ArrowHead::None: break;
                                case ArrowHead::Triangle: {
                                        Point p[3] = {tip, at(length, length * 0.45f), at(length, -length * 0.45f)};
                                        path.addSolid(p, 3);
                                        break;
                                }
                                case ArrowHead::Stealth: {
                                        Point p[4] = {tip, at(length, length * 0.42f), at(length * 0.7f, 0),
                                                      at(length, -length * 0.42f)};
                                        path.addSolid(p, 4);
                                        break;
                                }
                                case ArrowHead::Open: {
                                        Point v[3] = {at(length * 0.8f, length * 0.45f), tip,
                                                      at(length * 0.8f, -length * 0.45f)};
                                        path.addStroke(v, 3, false, width);
                                        break;
                                }
                                case ArrowHead::Circle: {
                                        Point    p[64];
                                        uint32_t n = circleSides(length * 0.3f);
                                        if (n > 64) n = 64;
                                        ellipsePoints(p, n, tip.x, tip.y, length * 0.3f, length * 0.3f, 0, true);
                                        path.addSolid(p, n);
                                        break;
                                }
                                case ArrowHead::Square: {
                                        float h = length * 0.3f;
                                        Point p[4] = {at(-h, h), at(-h, -h), at(h, -h), at(h, h)};
                                        path.addSolid(p, 4);
                                        break;
                                }
                                case ArrowHead::Diamond: {
                                        Point p[4] = {tip, at(length * 0.5f, length * 0.3f), at(length, 0),
                                                      at(length * 0.5f, -length * 0.3f)};
                                        path.addSolid(p, 4);
                                        break;
                                }
                                case ArrowHead::Bar: {
                                        Point v[2] = {at(0, length * 0.45f), at(0, -length * 0.45f)};
                                        path.addStroke(v, 2, false, width);
                                        break;
                                }
                        }
                        return;
                }

                // Cuts @p length off a polyline's end (@p atStart: its start), keeping 2 points.
                uint32_t trimmed(Point *pts, uint32_t n, float length, bool atStart) {
                        if (n < 2 || length <= 0) return n;
                        if (atStart) {
                                // Reversed, cut at the end, reversed back.
                                for (uint32_t i = 0; i < n / 2; i++) {
                                        Point t = pts[i];
                                        pts[i] = pts[n - 1 - i];
                                        pts[n - 1 - i] = t;
                                }
                                n = trimmed(pts, n, length, false);
                                for (uint32_t i = 0; i < n / 2; i++) {
                                        Point t = pts[i];
                                        pts[i] = pts[n - 1 - i];
                                        pts[n - 1 - i] = t;
                                }
                                return n;
                        }
                        while (n >= 2) {
                                Point a = pts[n - 2], b = pts[n - 1];
                                float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                                if (len > length || n == 2) {
                                        float t = len > 0 ? (len - length) / len : 0;
                                        if (t < 0) t = 0;
                                        pts[n - 1] = Point{lerp(a.x, b.x, t), lerp(a.y, b.y, t)};
                                        return n;
                                }
                                length -= len;
                                n--;
                        }
                        return n;
                }

                // The way a polyline goes out of its end (@p atStart: out of its start), a unit.
                Point outward(const Point *pts, uint32_t n, bool atStart) {
                        Point a = atStart ? pts[1] : pts[n - 2], b = atStart ? pts[0] : pts[n - 1];
                        // Past points too close to tell a way from.
                        for (uint32_t k = 2; k < n && fabsf(b.x - a.x) + fabsf(b.y - a.y) < 0.01f; k++)
                                a = atStart ? pts[k] : pts[n - 1 - k];
                        float dx = b.x - a.x, dy = b.y - a.y, len = sqrtf(dx * dx + dy * dy);
                        return len > 0 ? Point{dx / len, dy / len} : Point{1, 0};
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
                // A line, an arrow, an arc or an open curve: its path (as far as it is written), its
                // dashes and its heads, one fill (the heads turned as its strokes are: no seam).
                void drawOpen(Canvas &canvas, Shape &s, const Affine &world, float alpha) {
                        Scratch *sc = scratch();
                        if (sc == nullptr) return;
                        float    scale = sqrtf(fabsf(world.a * world.d - world.b * world.c));
                        float    lw = s.lineWidth * scale;
                        uint32_t n = openPath(s, sc->outlineLocal, MaxOutline);
                        if (n < 2 || lw <= 0) return;
                        Point *pts = sc->outlineWorld;
                        for (uint32_t i = 0; i < n; i++) pts[i] = world.apply(sc->outlineLocal[i]);
                        if (s.reveal < 1) {
                                n = partOf(pts, n, false, s.reveal < 0 ? 0 : s.reveal, sc->outlinePart);
                                pts = sc->outlinePart;
                                if (n < 2) return;
                        }
                        float total = 0;
                        for (uint32_t i = 0; i + 1 < n; i++) {
                                total += sqrtf((pts[i + 1].x - pts[i].x) * (pts[i + 1].x - pts[i].x) +
                                               (pts[i + 1].y - pts[i].y) * (pts[i + 1].y - pts[i].y));
                        }
                        if (total <= 0) return;
                        // Heads no longer than the line has room for.
                        float heads = (s.head != ArrowHead::None ? 1.0f : 0) + (s.tail != ArrowHead::None ? 1.0f : 0);
                        float length = headLength(s) * scale;
                        if (heads > 0 && length * heads > total) length = total / heads;
                        Point endTip = pts[n - 1], startTip = pts[0];
                        Point endWay = outward(pts, n, false), startWay = outward(pts, n, true);
                        n = trimmed(pts, n, headInset(s.head, length), false);
                        n = trimmed(pts, n, headInset(s.tail, length), true);
                        Path path;
                        strokeInto(path, pts, n, false, lw, s.dash * scale, s.dashGap * scale);
                        addHead(path, s.head, endTip, endWay, length, lw);
                        addHead(path, s.tail, startTip, startWay, length, lw);
                        (void)fillPath(canvas, path, s.color, alphaOf(alpha));
                        return;
                }

                // An axes' step when the script gave none: 1, 2 or 5 times a power of ten, about five
                // to its range.
                float niceStep(float range) {
                        if (!(range > 0)) return 1;
                        float raw = range / 5, power = powf(10, floorf(log10f(raw))), f = raw / power;
                        return (f < 1.5f ? 1 : f < 3.5f ? 2 : f < 7.5f ? 5 : 10) * power;
                }

                // Axes: a line for x and one for y (at 0 when it is in range, else at an edge), heads
                // at their far ends, ticks every step with their numbers in the theme's small font.
                void drawAxes(Canvas &canvas, SceneContext &context, Shape &s, const Affine &world, float alpha) {
                        float scale = sqrtf(fabsf(world.a * world.d - world.b * world.c));
                        float lw = s.lineWidth * scale;
                        float w = s.width / 2, h = s.height / 2;
                        float xr = s.xMax - s.xMin, yr = s.yMax - s.yMin;
                        if (!(xr > 0) || !(yr > 0) || lw <= 0) return;
                        auto px = [&](float x) {
                                return -w + (x - s.xMin) / xr * s.width;
                        };
                        auto py = [&](float y) {
                                return h - (y - s.yMin) / yr * s.height;
                        };
                        float axisY = s.yMin <= 0 && s.yMax >= 0 ? py(0) : h;
                        float axisX = s.xMin <= 0 && s.xMax >= 0 ? px(0) : -w;
                        Path  path;
                        Point xa[2] = {world.apply(Point{-w, axisY}), world.apply(Point{w, axisY})};
                        Point ya[2] = {world.apply(Point{axisX, h}), world.apply(Point{axisX, -h})};
                        float head = headLength(s) * scale * 0.7f;
                        Point xt = xa[1], yt = ya[1];
                        Point xw = outward(xa, 2, false), yw = outward(ya, 2, false);
                        (void)trimmed(xa, 2, headInset(ArrowHead::Triangle, head), false);
                        (void)trimmed(ya, 2, headInset(ArrowHead::Triangle, head), false);
                        path.addStroke(xa, 2, false, lw);
                        path.addStroke(ya, 2, false, lw);
                        addHead(path, ArrowHead::Triangle, xt, xw, head, lw);
                        addHead(path, ArrowHead::Triangle, yt, yw, head, lw);
                        // The theme's small font, at the theme's size zoomed with the camera.
                        FontKey key =
                                context.theme != nullptr ? sceneFont(context, context.theme->header.font) : FontKey{};
                        key.size = uint16_t(float(key.size) * (scale > 0.25f ? scale : 0.25f));
                        FontMetrics m = context.fonts != nullptr ? context.fonts->metrics(key) : FontMetrics{};
                        float       tick = 6;
                        struct Label {
                                        Point at;
                                        char  text[24];
                                        bool  below;
                        };
                        Label    labels[48];
                        uint32_t count = 0;
                        auto     ticks = [&](float lo, float hi, float step, bool alongX) {
                                if (!(step > 0) || (hi - lo) / step > 200) return;
                                for (float v = ceilf(lo / step - 1e-4f) * step; v <= hi + step * 1e-4f; v += step) {
                                        if (fabsf(v) < step * 1e-4f) continue; // 0 is where the axes cross
                                        Point t[2];
                                        if (alongX) {
                                                t[0] = world.apply(Point{px(v), axisY - tick / 2});
                                                t[1] = world.apply(Point{px(v), axisY + tick / 2});
                                        } else {
                                                t[0] = world.apply(Point{axisX - tick / 2, py(v)});
                                                t[1] = world.apply(Point{axisX + tick / 2, py(v)});
                                        }
                                        path.addStroke(t, 2, false, lw);
                                        if (count < 48) {
                                                Label &l = labels[count++];
                                                l.at = alongX ? world.apply(Point{px(v), axisY + tick + 2})
                                                                  : world.apply(Point{axisX - tick - 4, py(v)});
                                                l.below = alongX;
                                                snprintf(l.text, sizeof l.text, "%g", double(v));
                                        }
                                }
                        };
                        ticks(s.xMin, s.xMax, s.xStep > 0 ? s.xStep : niceStep(xr), true);
                        ticks(s.yMin, s.yMax, s.yStep > 0 ? s.yStep : niceStep(yr), false);
                        (void)fillPath(canvas, path, s.color, alphaOf(alpha));
                        if (context.fonts == nullptr || m.lineHeight <= 0 || alpha < 0.5f) return;
                        Color text = context.theme != nullptr ? context.theme->body.color : s.color;
                        for (uint32_t i = 0; i < count; i++) {
                                const Label &l = labels[i];
                                StringView   t(l.text);
                                int32_t      tw = context.fonts->width(key, t);
                                int32_t      x = l.below ? int32_t(l.at.x) - tw / 2 : int32_t(l.at.x) - tw;
                                int32_t      y = l.below ? int32_t(l.at.y) : int32_t(l.at.y) - m.lineHeight / 2;
                                context.fonts->draw(canvas, key, x, y, t, text);
                        }
                        return;
                }

                // A check mark's or a cross's strokes (its own pixels), the cross's two after one another.
                uint32_t markStrokes(const Shape &s, Point (*out)[3], uint32_t *counts) {
                        float r = s.radius;
                        if (s.sides == 0) {
                                out[0][0] = Point{-r * 0.8f, -r * 0.05f};
                                out[0][1] = Point{-r * 0.25f, r * 0.55f};
                                out[0][2] = Point{r * 0.8f, -r * 0.6f};
                                counts[0] = 3;
                                return 1;
                        }
                        out[0][0] = Point{-r * 0.7f, -r * 0.7f};
                        out[0][1] = Point{r * 0.7f, r * 0.7f};
                        out[1][0] = Point{r * 0.7f, -r * 0.7f};
                        out[1][1] = Point{-r * 0.7f, r * 0.7f};
                        counts[0] = counts[1] = 2;
                        return 2;
                }

                void drawMark(Canvas &canvas, Shape &s, const Affine &world, float alpha) {
                        float    scale = sqrtf(fabsf(world.a * world.d - world.b * world.c));
                        Point    strokes[2][3];
                        uint32_t counts[2];
                        uint32_t n = markStrokes(s, strokes, counts);
                        // Written: the strokes drawn in turn, as a hand would.
                        float shown = (s.reveal < 0 ? 0 : s.reveal > 1 ? 1 : s.reveal) * float(n);
                        Path  path;
                        for (uint32_t k = 0; k < n && shown > 0; k++) {
                                Point world3[3], part[4];
                                for (uint32_t i = 0; i < counts[k]; i++) world3[i] = world.apply(strokes[k][i]);
                                float    f = shown >= 1 ? 1 : shown;
                                uint32_t m = partOf(world3, counts[k], false, f, part);
                                if (m >= 2) path.addStroke(part, m, false, s.lineWidth * scale);
                                shown -= 1;
                        }
                        (void)fillPath(canvas, path, s.color, alphaOf(alpha));
                        return;
                }

                // A number from @p seed and @p i, 0 to 1 (the same each time: Previous replays it).
                float noise(uint32_t seed, uint32_t i, uint32_t k) {
                        uint32_t h = seed * 2654435761u ^ (i + 1) * 2246822519u ^ (k + 1) * 3266489917u;
                        h ^= h >> 15;
                        h *= 2246822519u;
                        h ^= h >> 13;
                        h *= 3266489917u;
                        h ^= h >> 16;
                        return float(h & 0xFFFFFF) / float(0xFFFFFF);
                }

                // Confetti at reveal (0 to 1, burst()'s): pieces from above its box, falling, swaying
                // and turning, gone at the end.
                void drawConfetti(Canvas &canvas, SceneContext &context, Shape &s, const Affine &world, float alpha) {
                        float t = s.reveal;
                        if (t <= 0 || t >= 1) return;
                        static constexpr Color Defaults[8] = {0xE01B24, 0xF6D32D, 0x33D17A, 0x3584E4,
                                                              0x9141AC, 0xFF7800, 0xF66151, 0x26A269};
                        float                  w = s.width, h = s.height;
                        float                  fade = t > 0.8f ? (1 - t) / 0.2f : 1;
                        for (uint32_t i = 0; i < 80; i++) {
                                float x0 = (noise(s.sides, i, 0) - 0.5f) * w;
                                float fall = 0.6f + noise(s.sides, i, 1) * 0.6f;
                                float y = -h / 2 - 20 - noise(s.sides, i, 2) * 60 + (h + 100) * t * fall;
                                float x = x0 + sinf(t * (6 + noise(s.sides, i, 3) * 8) + noise(s.sides, i, 4) * 6) * 24;
                                float turn = noise(s.sides, i, 5) * 360 + t * (360 + noise(s.sides, i, 6) * 720);
                                float pw = 5 + noise(s.sides, i, 7) * 6, ph = 3 + noise(s.sides, i, 8) * 3;
                                Affine piece = world.after(localOf(x, y, turn, 1));
                                Point  box[4] = {piece.apply(Point{-pw / 2, -ph / 2}),
                                                 piece.apply(Point{pw / 2, -ph / 2}), piece.apply(Point{pw / 2, ph / 2}),
                                                 piece.apply(Point{-pw / 2, ph / 2})};
                                char   name[24];
                                snprintf(name, sizeof name, "Script.Chart%u", unsigned(i % 8 + 1));
                                Color c = Defaults[i % 8];
                                if (context.theme != nullptr) (void)context.theme->scriptColor(StringView(name), &c);
                                Path path;
                                path.addPolygon(box, 4);
                                (void)fillPath(canvas, path, c, alphaOf(alpha * fade));
                        }
                        return;
                }

                void drawIcon(Canvas &canvas, SceneContext &context, Shape &s, const Affine &world, float alpha) {
                        // Scaled to the size it shows (a scene shrunk to fit: smaller), drawn scaled back.
                        int32_t w = int32_t(s.width * context.fit + 0.5f), h = int32_t(s.height * context.fit + 0.5f);
                        if (w <= 0 || h <= 0) return;
                        Affine shown = world.after(Affine::scaling(1 / context.fit, 1 / context.fit));
                        if (s.iconPixels == nullptr) {
                                // None found: a box where it would be, as a missing picture's.
                                Color c = context.theme != nullptr ? context.theme->placeholder : 0xB0B0B0;
                                Point box[4] = {shown.apply(Point{-float(w) / 2, -float(h) / 2}),
                                                shown.apply(Point{float(w) / 2, -float(h) / 2}),
                                                shown.apply(Point{float(w) / 2, float(h) / 2}),
                                                shown.apply(Point{-float(w) / 2, float(h) / 2})};
                                drawOutline(canvas, box, 4, c, 2, c, 0, alpha);
                                return;
                        }
                        if (s.sprite.pixels == nullptr || s.sprite.width != w || s.sprite.height != h ||
                            s.sprite.format != canvas.format()) {
                                freeScaled(s.sprite);
                                Picture p;
                                p.pixels = s.iconPixels;
                                p.layout = Picture::Layout::Bgra32;
                                p.width = p.height = s.iconSide;
                                p.stride = uint32_t(s.iconSide) * 4;
                                p.alpha = true;
                                const Palette *palette = context.theme != nullptr && context.theme->palette.count > 0
                                                                 ? &context.theme->palette
                                                                 : nullptr;
                                if (scalePicture(p, w, h, nullptr, 0, palette, canvas.format(), &s.sprite) != Error::Ok)
                                        return;
                        }
                        Affine at = shown.after(Affine::translation(-float(w) / 2, -float(h) / 2));
                        drawPicture(canvas, s.sprite, at, alphaOf(alpha * (s.reveal < 1 ? s.reveal : 1)));
                        return;
                }

                void drawBody(Canvas &canvas, SceneContext &context, Shape &s, const Affine &world, float alpha) {
                        float scale = sqrtf(fabsf(world.a * world.d - world.b * world.c));
                        float lw = s.lineWidth * scale;
                        switch (s.kind) {
                                case ShapeKind::Curve:
                                        if (!s.closed) {
                                                drawOpen(canvas, s, world, alpha);
                                                break;
                                        }
                                        [[fallthrough]];
                                case ShapeKind::Rect:
                                case ShapeKind::Circle:
                                case ShapeKind::Polygon:
                                case ShapeKind::Ellipse:
                                case ShapeKind::Sector:
                                case ShapeKind::Regular:
                                case ShapeKind::Star: {
                                        Scratch *sc = scratch();
                                        if (sc == nullptr) break;
                                        Point   *pts = sc->outlineWorld;
                                        uint32_t n = outline(s, sc->outlineLocal, MaxOutline);
                                        if (n < 2) break;
                                        for (uint32_t i = 0; i < n; i++) pts[i] = world.apply(sc->outlineLocal[i]);
                                        bool ringed = (s.kind == ShapeKind::Rect && s.cornerRadius <= 0.25f) ||
                                                      s.kind == ShapeKind::Circle;
                                        if (s.reveal >= 1 && ringed && s.dash <= 0.5f) {
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
                                                drawOutline(canvas, pts, n, s.color, lw, s.fill, s.fillOpacity, alpha,
                                                            s.dash * scale, s.dashGap * scale);
                                                break;
                                        }
                                        // Written: its outline drawn round so far, its inside fading in.
                                        if (s.fillOpacity > 0 && s.reveal > 0) {
                                                Path path;
                                                path.addPolygon(pts, n);
                                                (void)fillPath(canvas, path, s.fill,
                                                               alphaOf(s.fillOpacity * alpha * s.reveal));
                                        }
                                        Point   *part = sc->outlinePart;
                                        uint32_t m = partOf(pts, n, true, s.reveal, part);
                                        if (lw > 0 && m >= 2) {
                                                Path path;
                                                strokeInto(path, part, m, false, lw, s.dash * scale, s.dashGap * scale);
                                                (void)fillPath(canvas, path, s.color, alphaOf(alpha));
                                        }
                                        break;
                                }
                                case ShapeKind::Line:
                                case ShapeKind::Arrow:
                                case ShapeKind::Arc: drawOpen(canvas, s, world, alpha); break;
                                case ShapeKind::Axes: drawAxes(canvas, context, s, world, alpha); break;
                                case ShapeKind::Mark: drawMark(canvas, s, world, alpha); break;
                                case ShapeKind::Confetti: drawConfetti(canvas, context, s, world, alpha); break;
                                case ShapeKind::Icon: drawIcon(canvas, context, s, world, alpha); break;
                                case ShapeKind::Camera: break; // a view, not drawn
                                case ShapeKind::Text:
                                case ShapeKind::Number:
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
                                        // Its text was made at the size it shows: drawn scaled back.
                                        at = at.after(Affine::scaling(1 / context.fit, 1 / context.fit));
                                        // Its text made whole once (a code's picture, a text's mask);
                                        // written, each line shown as far as it has got.
                                        if (!textMask(context, s, total)) break;
                                        bool code = s.kind == ShapeKind::Code;
                                        auto part = [&](const Rect *band) {
                                                if (code) {
                                                        drawPicture(canvas, s.sprite, at, alphaOf(alpha), band);
                                                } else {
                                                        drawMask(canvas, s.mask, s.maskWidth, s.maskHeight,
                                                                 uint32_t(s.maskWidth), at, s.color, alphaOf(alpha),
                                                                 band);
                                                }
                                        };
                                        FontKey     key = shownFontOf(context, s);
                                        FontMetrics m = context.fonts->metrics(key);
                                        int32_t     pad = code ? codePad(key) : 2;
                                        if (shown >= total) {
                                                part(nullptr);
                                        } else {
                                                int32_t  inner = s.maskWidth - 2 * pad;
                                                int32_t  tab = context.fonts->width(key, "    ");
                                                uint32_t left = shown;
                                                eachTextLine(s, [&](StringView line, uint32_t n) {
                                                        if (left == 0) return;
                                                        uint32_t count = charactersIn(line);
                                                        int32_t  upTo = s.maskWidth;
                                                        if (count > left) {
                                                                StringView before = line.left(bytesOf(line, left));
                                                                upTo = code ? pad + codeWidth(context, key, before, tab)
                                                                            : pad +
                                                                                       (inner - context.fonts->width(
                                                                                                        key, line)) /
                                                                                               2 +
                                                                                       context.fonts->width(key,
                                                                                                            before);
                                                        }
                                                        left -= count < left ? count : left;
                                                        Rect band{0, pad + int32_t(n) * m.lineHeight, upTo,
                                                                  m.lineHeight};
                                                        if (n == 0) {
                                                                band.y = 0;
                                                                band.height += pad;
                                                        }
                                                        part(&band);
                                                });
                                        }
                                        if (code && s.focus != 0 && s.dim > 0 && context.theme != nullptr) {
                                                // The lines not in focus under the box's colour, so much.
                                                uint32_t lines = 0;
                                                eachTextLine(s, [&](StringView, uint32_t) { lines++; });
                                                Color back = s.fillOpacity > 0 ? s.fill : context.theme->background;
                                                for (uint32_t k = 0; k < lines && k < 64; k++) {
                                                        if ((s.focus >> k & 1) != 0) continue;
                                                        fillBox(canvas, at, 0,
                                                                float(pad) + float(k) * float(m.lineHeight),
                                                                w * context.fit, float(m.lineHeight), back,
                                                                s.dim * alpha);
                                                }
                                        }
                                        break;
                                }
                                case ShapeKind::Image: {
                                        if (s.path == nullptr || context.images == nullptr) break;
                                        // Scaled to the size it shows, drawn scaled back (as an icon).
                                        int32_t w = int32_t(s.width * context.fit + 0.5f),
                                                h = int32_t(s.height * context.fit + 0.5f);
                                        if (w <= 0 || h <= 0) break;
                                        const Palette *palette =
                                                context.theme != nullptr && context.theme->palette.count > 0
                                                        ? &context.theme->palette
                                                        : nullptr;
                                        const ScaledImage *image =
                                                context.images->get(StringView(s.path), w, h, Rect{0, 0, w, h}, 0,
                                                                    palette, canvas.format());
                                        if (image == nullptr) break;
                                        Affine at = world.after(Affine::scaling(1 / context.fit, 1 / context.fit))
                                                            .after(Affine::translation(-float(w) / 2, -float(h) / 2));
                                        drawPicture(canvas, *image, at, alphaOf(alpha * (s.reveal < 1 ? s.reveal : 1)));
                                        break;
                                }
                                case ShapeKind::Group:
                                case ShapeKind::Array:
                                case ShapeKind::Table:
                                case ShapeKind::Graph:
                                case ShapeKind::Brace:
                                case ShapeKind::Callout:
                                case ShapeKind::Label:
                                case ShapeKind::BarChart:
                                case ShapeKind::PieChart:
                                case ShapeKind::LineChart: {
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
                                        // Its middle lines on the axes, the rest a step apart out from
                                        // them; added left to right and top to bottom (a row's edges in
                                        // order: no sorting them).
                                        int32_t nx = int32_t(w / sx + 1e-4f), ny = int32_t(h / sx + 1e-4f);
                                        for (int32_t i = -nx; i <= nx; i++) {
                                                Point l[2] = {world.apply(Point{float(i) * sx, -h}),
                                                              world.apply(Point{float(i) * sx, h})};
                                                path.addStroke(l, 2, false, lineW);
                                        }
                                        for (int32_t i = -ny; i <= ny; i++) {
                                                Point l[2] = {world.apply(Point{-w, float(i) * sx}),
                                                              world.apply(Point{w, float(i) * sx})};
                                                path.addStroke(l, 2, false, lineW);
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
                                case ShapeKind::Circle:
                                case ShapeKind::Regular:
                                case ShapeKind::Star:
                                case ShapeKind::Sector: w = h = s.radius + pad; break;
                                case ShapeKind::Ellipse:
                                        w = s.width / 2 + pad;
                                        h = s.height / 2 + pad;
                                        break;
                                case ShapeKind::Axes:
                                        // Its numbers round it too.
                                        w = s.width / 2 + pad + 48;
                                        h = s.height / 2 + pad + 32;
                                        break;
                                case ShapeKind::Text:
                                case ShapeKind::Number:
                                case ShapeKind::Code:
                                        s.measureText(context, &w, &h);
                                        w = w / 2 + 2;
                                        h = h / 2 + 2;
                                        break;
                                case ShapeKind::Line:
                                case ShapeKind::Arrow:
                                case ShapeKind::Arc:
                                case ShapeKind::Curve: {
                                        // Its path's box, its heads round it.
                                        Scratch *sc = scratch();
                                        uint32_t n = 0;
                                        Point    two[2] = {s.ends[0], s.ends[1]};
                                        Point   *pts = two;
                                        if (sc != nullptr) {
                                                pts = sc->outlineLocal;
                                                n = isOpen(s) ? openPath(s, pts, MaxOutline)
                                                              : outline(s, pts, MaxOutline);
                                        } else if (s.kind == ShapeKind::Line || s.kind == ShapeKind::Arrow) {
                                                n = 2;
                                        }
                                        if (s.head != ArrowHead::None || s.tail != ArrowHead::None)
                                                pad += headLength(s);
                                        *x0 = *y0 = *x1 = *y1 = 0;
                                        for (uint32_t i = 0; i < n; i++) {
                                                const Point &p = pts[i];
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
                                case ShapeKind::Icon:
                                        w = s.width / 2;
                                        h = s.height / 2;
                                        break;
                                case ShapeKind::Mark: w = h = s.radius + s.lineWidth; break;
                                case ShapeKind::Confetti:
                                        w = s.width / 2;
                                        h = s.height / 2;
                                        break;
                                default: break;
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
                if (morphable(s) && morphable(*to) && scratch() != nullptr) {
                        // Outline into outline: each resampled, point to point.
                        Scratch *sc = scratch();
                        Point   *a = sc->morphOutlineA, *b = sc->morphOutlineB, *pa = sc->morphFromEven,
                              *pb = sc->morphToEven, *pts = sc->morphBetween;
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
                if (isComposite(s.kind)) {
                        for (uint32_t i = 0; i < s.childCount; i++)
                                r = uniteRects(r, shapeBounds(context, *s.children[i], world));
                } else if (s.kind == ShapeKind::Camera) {
                        return Rect{};
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
                real(s.ends[0].x);
                real(s.ends[0].y);
                real(s.ends[1].x);
                real(s.ends[1].y);
                word(uint32_t(s.head) | uint32_t(s.tail) << 8 | (s.closed ? 1u << 16 : 0) | (s.smooth ? 1u << 17 : 0));
                real(s.headSize);
                real(s.bend);
                real(s.dash);
                real(s.dashGap);
                real(s.cornerRadius);
                real(s.start);
                real(s.sweep);
                real(s.innerRadius);
                word(s.sides);
                real(s.xMin);
                real(s.xMax);
                real(s.yMin);
                real(s.yMax);
                real(s.xStep);
                real(s.yStep);
                word(s.rows);
                word(s.columns);
                word(uint32_t(s.focus) ^ uint32_t(s.focus >> 32));
                real(s.dim);
                word(uint32_t(s.iconSide) | (s.indices ? 1u << 16 : 0) | (s.header ? 1u << 17 : 0));
                for (uint32_t i = 0; s.edges != nullptr && i < s.columns * 3; i++) word(s.edges[i]);
                word(s.pointCount);
                for (uint32_t i = 0; i < s.pointCount; i++) {
                        real(s.points[i].x);
                        real(s.points[i].y);
                }
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

        void Scene::toFront(Shape *shape) {
                // Drawn last: over the others.
                for (uint32_t i = 0; i < _count; i++) {
                        if (_shapes[i] != shape) continue;
                        memmove(_shapes + i, _shapes + i + 1, (_count - i - 1) * sizeof(Shape *));
                        _shapes[_count - 1] = shape;
                        return;
                }
                return;
        }

        void Scene::toBack(Shape *shape) {
                for (uint32_t i = 0; i < _count; i++) {
                        if (_shapes[i] != shape) continue;
                        memmove(_shapes + 1, _shapes, i * sizeof(Shape *));
                        _shapes[0] = shape;
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
                draw(canvas, context, Affine::translation(float(x), float(y)));
                return;
        }

        void Scene::draw(Canvas &canvas, SceneContext &context, const Affine &view) {
                for (uint32_t i = 0; i < _count; i++) drawShape(canvas, context, *_shapes[i], view, 1);
                return;
        }

        Rect Scene::bounds(SceneContext &context) {
                Rect r;
                for (uint32_t i = 0; i < _count; i++) r = uniteRects(r, shapeBounds(context, *_shapes[i], Affine()));
                return r;
        }


        // ---- Step 14 (§8e): paths, placing, graphs, settling

        bool pointAlong(SceneContext &context, const Shape &path, const Affine &parent, float t, Point *at,
                        Point *way) {
                (void)context;
                Scratch *sc = scratch();
                if (sc == nullptr) return false;
                bool     open = isOpen(path);
                uint32_t n = open ? openPath(path, sc->outlineLocal, MaxOutline)
                                  : outline(path, sc->outlineLocal, MaxOutline);
                if (n < 2) return false;
                Affine world = parent.after(localOf(path));
                Point *pts = sc->outlineWorld;
                for (uint32_t i = 0; i < n; i++) pts[i] = world.apply(sc->outlineLocal[i]);
                uint32_t segments = open ? n - 1 : n;
                float    total = 0;
                for (uint32_t i = 0; i < segments; i++) {
                        Point a = pts[i], b = pts[(i + 1) % n];
                        total += sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                }
                float want = total * (t < 0 ? 0 : t > 1 ? 1 : t);
                for (uint32_t i = 0; i < segments; i++) {
                        Point a = pts[i], b = pts[(i + 1) % n];
                        float len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                        if (want > len && i + 1 < segments) {
                                want -= len;
                                continue;
                        }
                        float f = len > 0 ? want / len : 0;
                        if (f > 1) f = 1;
                        *at = Point{lerp(a.x, b.x, f), lerp(a.y, b.y, f)};
                        if (way != nullptr) *way = len > 0 ? Point{(b.x - a.x) / len, (b.y - a.y) / len} : Point{1, 0};
                        return true;
                }
                *at = pts[0];
                if (way != nullptr) *way = Point{1, 0};
                return true;
        }

        Affine parentsOf(const Shape &shape) {
                Affine a;
                for (const Shape *p = shape.parent; p != nullptr; p = p->parent) a = localOf(*p).after(a);
                return a;
        }

        void placeBeside(SceneContext &context, Shape &s, Shape &other, uint8_t side, float gap) {
                // In the scene's pixels: either may be in a group (an array's cell).
                Rect me = shapeBounds(context, s, parentsOf(s)), it = shapeBounds(context, other, parentsOf(other));
                if (me.isEmpty() || it.isEmpty()) return;
                float mcx = float(me.x) + float(me.width) / 2, mcy = float(me.y) + float(me.height) / 2;
                float icx = float(it.x) + float(it.width) / 2, icy = float(it.y) + float(it.height) / 2;
                switch (side) {
                        case 0: // Left
                                s.x += float(it.x) - gap - float(me.x + me.width);
                                s.y += icy - mcy;
                                break;
                        case 1: // Right
                                s.x += float(it.x + it.width) + gap - float(me.x);
                                s.y += icy - mcy;
                                break;
                        case 2: // Above
                                s.y += float(it.y) - gap - float(me.y + me.height);
                                s.x += icx - mcx;
                                break;
                        default: // Below
                                s.y += float(it.y + it.height) + gap - float(me.y);
                                s.x += icx - mcx;
                                break;
                }
                return;
        }

        namespace {

                // Where a ray from a box's middle toward (@p tx, @p ty) leaves the ellipse inside it (a
                // graph's node is a circle).
                Point leavingRound(const Rect &b, float tx, float ty) {
                        float cx = float(b.x) + float(b.width) / 2, cy = float(b.y) + float(b.height) / 2;
                        float dx = tx - cx, dy = ty - cy, rx = float(b.width) / 2, ry = float(b.height) / 2;
                        if ((fabsf(dx) < 1e-6f && fabsf(dy) < 1e-6f) || rx <= 0 || ry <= 0) return Point{cx, cy};
                        float t = 1 / sqrtf((dx / rx) * (dx / rx) + (dy / ry) * (dy / ry));
                        return Point{cx + dx * t, cy + dy * t};
                }

        } // namespace

        void settleGraph(SceneContext &context, Shape &g) {
                // Children: its edges first (drawn under), then its nodes.
                Affine none;
                for (uint32_t k = 0; k < g.columns && k < g.childCount && g.edges != nullptr; k++) {
                        uint32_t a = g.columns + g.edges[k * 3], b = g.columns + g.edges[k * 3 + 1];
                        if (a >= g.childCount || b >= g.childCount) continue;
                        Rect ra = shapeBounds(context, *g.children[a], none),
                             rb = shapeBounds(context, *g.children[b], none);
                        if (ra.isEmpty() || rb.isEmpty()) continue;
                        Point from =
                                leavingRound(ra, float(rb.x) + float(rb.width) / 2, float(rb.y) + float(rb.height) / 2);
                        Point to =
                                leavingRound(rb, float(ra.x) + float(ra.width) / 2, float(ra.y) + float(ra.height) / 2);
                        Shape &line = *g.children[k];
                        line.x = (from.x + to.x) / 2;
                        line.y = (from.y + to.y) / 2;
                        line.ends[0] = Point{from.x - line.x, from.y - line.y};
                        line.ends[1] = Point{to.x - line.x, to.y - line.y};
                }
                return;
        }

        namespace {

                void settleShape(SceneContext &context, Shape &s) {
                        if (isComposite(s.kind)) {
                                for (uint32_t i = 0; i < s.childCount; i++) settleShape(context, *s.children[i]);
                        }
                        if (s.kind == ShapeKind::Graph) settleGraph(context, s);
                        if (s.anchor != nullptr) placeBeside(context, s, *s.anchor, s.anchorSide, s.anchorGap);
                        return;
                }

        } // namespace

        void Scene::settle(SceneContext &context) {
                for (uint32_t i = 0; i < _count; i++) settleShape(context, *_shapes[i]);
                return;
        }

} // namespace preso
