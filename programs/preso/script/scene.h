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
                        /** @brief The region's pixels a scene's unit (below 1: shrunk to fit, §8f). */
                        float fit = 1;
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
                // Step 13 (§8d)
                Ellipse,
                Arc,
                Sector,
                Regular,
                Star,
                Curve,
                Number,
                Axes,
                // Step 14 (§8e): composites (their children drawn as a group's), and the rest
                Array,
                Table,
                Graph,
                Brace,
                Callout,
                Label,
                BarChart,
                PieChart,
                LineChart,
                Icon,
                Mark,
                Confetti,
                Camera, ///< A region's view (never drawn).
        };

        /** @brief Whether @p kind is drawn and measured as its children are (a group's way). */
        bool isComposite(ShapeKind kind);

        /** @brief What a line's (or an arc's) end has. Scripts' ArrowHead: append only. */
        enum class ArrowHead : uint8_t {
                None,
                Triangle,
                Stealth, ///< A triangle swept back.
                Open,    ///< A V of two lines.
                Circle,
                Square,
                Diamond,
                Bar, ///< A line across.
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
                        float    radius = 0;       ///< Circle, Arc, Sector, Regular, Star.
                        float    step = 0;         ///< Grid: between its lines.
                        Point    ends[2];          ///< Line, Arrow: from its centre.
                        Point   *points = nullptr; ///< Polygon, Curve: from its centre.
                        uint32_t pointCount = 0;
                        // Lines, arrows and arcs: their ends; every outline: its dashes (§8d)
                        ArrowHead head = ArrowHead::None; ///< At its end.
                        ArrowHead tail = ArrowHead::None; ///< At its start.
                        float     headSize = 0;           ///< 0: from its line's width.
                        float     bend = 0;               ///< Line, Arrow: degrees of an arc, clockwise.
                        float     dash = 0;               ///< A dash's length (0: solid).
                        float     dashGap = 0;            ///< 0: as long as a dash.
                        float     cornerRadius = 0;       ///< Rect.
                        float     start = 0;              ///< Arc, Sector: degrees clockwise from 3 o'clock.
                        float     sweep = 90;             ///< Arc, Sector: degrees clockwise.
                        float     innerRadius = 0;        ///< Sector, Star.
                        uint32_t  sides = 0;              ///< Regular: sides; Star: points.
                        bool      closed = false;         ///< Curve: back to its first point.
                        bool      smooth = true;          ///< Curve: through its points smoothly.
                        int32_t   decimals = 0;           ///< Number.
                        // Axes: their ranges and ticks
                        float xMin = 0, xMax = 1, yMin = 0, yMax = 1, xStep = 0, yStep = 0;
                        // Composites (§8e)
                        uint32_t  rows = 0;         ///< Array: 1; Table: its rows; Graph: nodes; charts: values.
                        uint32_t  columns = 0;      ///< Array: cells; Table: its columns; Graph: edges.
                        uint16_t *edges = nullptr;  ///< Graph: from, to, directed (1) for each.
                        bool      indices = true;   ///< Array: its indices shown.
                        bool      header = false;   ///< Table: its first row a header.
                        Shape    *anchor = nullptr; ///< Label: what it is kept beside (a reference held).
                        uint8_t   anchorSide = 0;   ///< The scripts' Side.
                        float     anchorGap = 8;
                        uint64_t  focus = 0;            ///< Code: the lines not dimmed, a bit each (1 << (line - 1)).
                        float     dim = 0;              ///< Code: how much the other lines are dimmed (0-1).
                        uint32_t *iconPixels = nullptr; ///< Icon: 0xAARRGGBB, iconSide square (owned).
                        int32_t   iconSide = 0;
                        /** @brief follow()'s (api.cpp's), called before each frame; let go with releaseUpdater. */
                        void *updater = nullptr;
                        void (*releaseUpdater)(void *updater) = nullptr;
                        // Text and Code
                        char       *text = nullptr;
                        uint32_t    textLength = 0;
                        ScriptFont *font = nullptr; ///< nullptr: the theme's body (Code: its code) font.
                        int32_t     syntax = -1;    ///< Code: its language (-1: plain).
                        // Image
                        char *path = nullptr;
                        // Group (and composites)
                        Shape *parent =
                                nullptr; ///< The group it was last added to (not a reference: cleared as it goes).
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
                        float value = 0;       ///< Slider's, Number's.
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
                        /** @brief A Number's value, its text with its decimals. */
                        bool setNumber(float v);
                        /** @brief A new object like this one (its children copied too), not in a scene. */
                        Shape *clone() const;
                        /** @brief Every plain field of @p other's place, look and geometry (not what it owns). */
                        void copyLook(const Shape &other);
                        bool setPath(const char *data, uint32_t length);
                        bool setPoints(const Point *data, uint32_t count);
                        bool setBars(const float *values, uint32_t count, Color barColor);
                        /** @brief Adds @p child to a group (a reference taken). False: no memory, or a loop. */
                        bool addChild(Shape *child);
                        /** @brief As addChild(), at @p at (0: drawn first). */
                        bool insertChild(Shape *child, uint32_t at);
                        /** @brief Children @p i and @p j change places in the order (not where they are). */
                        void swapChildren(uint32_t i, uint32_t j);
                        /** @brief A graph's edges' records (from, to, directed), copied. */
                        bool setEdges(const uint16_t *data, uint32_t count);
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
                        /** @brief Drawn over the others (@p shape in it). */
                        void toFront(Shape *shape);
                        /** @brief Drawn under the others. */
                        void toBack(Shape *shape);
                        void clear();

                        /** @brief Draws every visible object, the region's top left at (@p x, @p y). */
                        void draw(Canvas &canvas, SceneContext &context, int32_t x, int32_t y);
                        /** @brief What the objects cover, in the region's pixels (empty: nothing). */
                        Rect     bounds(SceneContext &context);
                        /**
                         * @brief Before a frame is drawn or measured: labels kept beside what they are
                         *        anchored to, graphs' edges on their nodes (updaters are the region's).
                         */
                        void settle(SceneContext &context);
                        /** @brief Draws every visible object through @p view (the region's place and camera). */
                        void     draw(Canvas &canvas, SceneContext &context, const Affine &view);
                        uint32_t count() const { return _count; }
                        Shape   *at(uint32_t index) const { return _shapes[index]; }

                private:
                        Shape  **_shapes = nullptr;
                        uint32_t _count = 0;
                        uint32_t _capacity = 0;
        };

        /** @brief Draws @p shape through @p parent (its group's, the region's place), @p opacity of it. */
        void drawShape(Canvas &canvas, SceneContext &context, Shape &shape, const Affine &parent, float opacity);
        /**
         * @brief A theme's font in a scene: its size over the scene's fit, so its text shows at the
         *        theme's size in a region shrunk to fit (devplan/preso.md §8f).
         */
        FontKey sceneFont(const SceneContext &context, FontKey key);
        /** @brief A scene's font as it is drawn: its size times the scene's fit. */
        FontKey rasterFont(const SceneContext &context, FontKey key);
        /** @brief The pixels @p shape covers through @p parent. */
        Rect shapeBounds(SceneContext &context, Shape &shape, const Affine &parent);
        /** @brief Its groups' transforms, outermost first: its own pixels into the scene's are these after its own. */
        Affine parentsOf(const Shape &shape);
        /** @brief A number that changes when anything of @p shape's look does (its children's too). */
        uint32_t shapeSignature(const Shape &shape);

        /**
         * @brief The point @p t (0 to 1) of the way along @p path's outline or path, through
         *        @p parent, and the way it goes there (a unit, into @p way if not null); false if it
         *        has none (text, a picture).
         */
        bool pointAlong(SceneContext &context, const Shape &path, const Affine &parent, float t, Point *at, Point *way);

        /** @brief Moves @p s so its box (in its parent's pixels) is beside @p other's, @p gap apart. */
        void placeBeside(SceneContext &context, Shape &s, Shape &other, uint8_t side, float gap);

        /** @brief A graph's edges put on its nodes' boxes (each frame, and as nodes are laid out). */
        void settleGraph(SceneContext &context, Shape &graph);

} // namespace preso

#endif // PRESO_SCRIPT_SCENE_H
