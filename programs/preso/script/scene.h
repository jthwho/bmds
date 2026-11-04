/**
 * @file      scene.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A script region's objects (devplan/preso.md §7.4, §7.9): shapes kept from frame to frame and
 * drawn in native code, so a script's own work is a few calls a beat. Positions are centres in
 * the region's pixels (Manim's), rotations degrees clockwise. Nothing here knows AngelScript:
 * api.cpp gives scripts these objects.
 */

#ifndef PRESO_SCRIPT_SCENE_H
#define PRESO_SCRIPT_SCENE_H

#include "../core/canvas.h"
#include "../core/fonts.h"
#include "../core/images.h"
#include "../core/raster.h"
#include "../core/theme.h"

namespace preso {

        /** @brief What a scene draws with: the deck's fonts, pictures and theme. */
        struct SceneContext {
                        Platform    *platform = nullptr;
                        FontCache   *fonts = nullptr;
                        ImageCache  *images = nullptr;
                        const Theme *theme = nullptr;
        };

        /** @brief A font a script names: a theme's, or its own (the family kept here). */
        class ScriptFont {
                public:
                        static ScriptFont *make(const FontKey &key);
                        void               addRef() { _refs++; }
                        void               release();
                        const FontKey     &key() const { return _key; }

                private:
                        ScriptFont() = default;
                        ~ScriptFont() = default;

                        FontKey  _key;
                        char     _family[64] = {};
                        uint32_t _refs = 1;
        };

        enum class ShapeKind : uint8_t {
                Text,
                Rect,
                Circle,
                Line,
                Arrow,
                Polygon,
                Image,
                Code,
                Group,
                Bars,
                Grid,
                // Widgets (§7.10): their text the label.
                Slider,
                Button,
                Toggle,
        };

        /** @brief A bar of Bars: its value, colour, and where it is (in bar widths from its place). */
        struct Bar {
                        float value = 0;
                        Color color = 0;
                        float offset = 0;
        };

        /**
         * @brief An object of a scene: a shape, text, a picture, or a group of them. Counted
         *        references (the script's handles, a group's, a scene's, animations').
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class Shape {
                public:
                        static Shape *make(ShapeKind kind);
                        void          addRef() { _refs++; }
                        void          release();

                        ShapeKind kind;
                        // ---- Where and how (every kind)
                        float x = 0;
                        float y = 0;
                        float scale = 1;
                        float rotation = 0; ///< Degrees, clockwise.
                        float opacity = 1;
                        bool  visible = true;
                        Color color = 0;       ///< Lines, outlines, text.
                        Color fill = 0;        ///< Inside a closed shape.
                        float fillOpacity = 0; ///< 0: not filled.
                        float lineWidth = 3;
                        /** @brief How much of it is drawn (write()): 0 to 1. */
                        float reveal = 1;
                        // ---- Its geometry
                        float    width = 0;        ///< Rect, Image, Bars, Grid.
                        float    height = 0;       ///< Rect, Image, Bars, Grid.
                        float    radius = 0;       ///< Circle.
                        float    step = 0;         ///< Grid: between its lines.
                        Point    ends[2];          ///< Line, Arrow: from its centre.
                        Point   *points = nullptr; ///< Polygon: from its centre.
                        uint32_t pointCount = 0;
                        // Text and Code
                        char       *text = nullptr;
                        uint32_t    textLength = 0;
                        ScriptFont *font = nullptr; ///< nullptr: the theme's body (Code: its code) font.
                        int32_t     syntax = -1;    ///< Code: its language (-1: plain).
                        // Image
                        char *path = nullptr;
                        // Group
                        Shape  **children = nullptr;
                        uint32_t childCount = 0;
                        uint32_t childCapacity = 0;
                        // Bars
                        Bar     *bars = nullptr;
                        uint32_t barCount = 0;
                        float    barMax = 1;
                        // Transform: what it turns into, how far.
                        Shape *morphTo = nullptr;
                        float  morph = 0;
                        // Widgets
                        float minimum = 0;
                        float maximum = 1;
                        float value = 0;       ///< Slider's.
                        bool  on = false;      ///< Toggle's.
                        bool  pressed = false; ///< Button's, while held.
                        bool  focused = false; ///< Its region entered and it the one keys move.
                        /** @brief What onChange() gave (api.cpp's), let go with @p releaseCallback. */
                        void *callback = nullptr;
                        void (*releaseCallback)(void *callback) = nullptr;
                        bool isWidget() const {
                                return kind == ShapeKind::Slider || kind == ShapeKind::Button ||
                                       kind == ShapeKind::Toggle;
                        }
                        /** @brief A slider's value at @p x (its own pixels from its centre), kept within its range. */
                        float sliderValueAt(float x) const;

                        bool setText(const char *data, uint32_t length);
                        bool setPath(const char *data, uint32_t length);
                        bool setPoints(const Point *data, uint32_t count);
                        bool setBars(const float *values, uint32_t count, Color barColor);
                        /** @brief Adds @p child to a group (a reference taken). False: no memory, or a loop. */
                        bool addChild(Shape *child);
                        void removeChild(Shape *child);
                        /** @brief Whether @p other is this or inside it. */
                        bool contains(const Shape *other) const;
                        /** @brief Takes @p other's look and geometry (a transform's end). */
                        bool become(const Shape &other);

                        /** @brief Text's size before scaling: its lines' widest and their height (kept). */
                        void  measureText(SceneContext &context, float *w, float *h);
                        float measuredWidth = -1; ///< measureText()'s, -1: not yet (the text or font changed).
                        float measuredHeight = 0;

                        // ---- Drawing's caches (scene.cpp's)
                        uint8_t    *mask = nullptr; ///< Text: coverage; Code: its sprite's alpha.
                        int32_t     maskWidth = 0;
                        int32_t     maskHeight = 0;
                        uint32_t    maskShown = UINT32_MAX; ///< Characters drawn in it.
                        ScaledImage sprite;                 ///< Code: its colours, premultiplied.
                        void        dropCaches();

                private:
                        explicit Shape(ShapeKind k) : kind(k) {}
                        ~Shape();

                        uint32_t _refs = 1;
        };

        /**
         * @brief A region's objects, drawn in the order they were added.
         *
         * @par Thread Safety
         * Not thread-safe: the core's thread.
         */
        class Scene {
                public:
                        Scene() = default;
                        ~Scene() { clear(); }
                        Scene(const Scene &) = delete;
                        Scene &operator=(const Scene &) = delete;

                        /** @brief Adds @p shape (a reference taken; once: added again, it stays where it is). */
                        bool add(Shape *shape);
                        void remove(Shape *shape);
                        bool has(const Shape *shape) const;
                        void clear();

                        /** @brief Draws every visible object, the region's top left at (@p x, @p y). */
                        void draw(Canvas &canvas, SceneContext &context, int32_t x, int32_t y);
                        /** @brief What the objects cover, in the region's pixels (empty: nothing). */
                        Rect     bounds(SceneContext &context);
                        uint32_t count() const { return _count; }
                        Shape   *at(uint32_t index) const { return _shapes[index]; }

                private:
                        Shape  **_shapes = nullptr;
                        uint32_t _count = 0;
                        uint32_t _capacity = 0;
        };

        /** @brief Draws @p shape through @p parent (its group's, the region's place), @p opacity of it. */
        void drawShape(Canvas &canvas, SceneContext &context, Shape &shape, const Affine &parent, float opacity);
        /** @brief The pixels @p shape covers through @p parent. */
        Rect shapeBounds(SceneContext &context, Shape &shape, const Affine &parent);
        /** @brief A number that changes when anything of @p shape's look does (its children's too). */
        uint32_t shapeSignature(const Shape &shape);

} // namespace preso

#endif // PRESO_SCRIPT_SCENE_H
