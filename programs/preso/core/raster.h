/**
 * @file      raster.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Smooth shapes on a Canvas (devplan/preso.md §7.9): paths filled with each pixel's exact
 * coverage (signed areas accumulated along a row, as font rasterizers do), lines as the
 * polygons around them, and coverage masks and pictures drawn moved, scaled and turned. What a
 * deck's scripts draw with, so shapes stay smooth as they move.
 */

#ifndef PRESO_RASTER_H
#define PRESO_RASTER_H

#include "canvas.h"
#include "images.h"

namespace preso {

        struct Point {
                        float x = 0;
                        float y = 0;
        };

        /** @brief x' = a·x + c·y + e, y' = b·x + d·y + f. */
        struct Affine {
                        float a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;

                        Point apply(Point p) const { return Point{a * p.x + c * p.y + e, b * p.x + d * p.y + f}; }
                        /** @brief This after @p first (first applied, then this). */
                        Affine after(const Affine &first) const;
                        /** @brief The transform that undoes this; false if it can't (a scale of 0). */
                        bool          inverted(Affine *out) const;
                        static Affine translation(float x, float y) { return Affine{1, 0, 0, 1, x, y}; }
                        static Affine scaling(float sx, float sy) { return Affine{sx, 0, 0, sy, 0, 0}; }
                        /** @brief By @p degrees, clockwise on the screen (y down). */
                        static Affine rotation(float degrees);
        };

        /**
         * @brief Closed outlines of points: a shape to fill. Where outlines overlap, the shape is
         *        where any of them is (outlines that turn the other way cut holes).
         *
         * @par Thread Safety
         * Not thread-safe.
         */
        class Path {
                public:
                        Path() = default;
                        ~Path();
                        Path(const Path &) = delete;
                        Path &operator=(const Path &) = delete;

                        /** @brief Starts a new outline at @p p (the one before closed). */
                        void moveTo(Point p);
                        void lineTo(Point p);
                        void clear();

                        void addRect(float x, float y, float width, float height);
                        /** @brief An ellipse round (@p cx, @p cy), in as many sides as look round. */
                        void addEllipse(float cx, float cy, float rx, float ry);
                        void addPolygon(const Point *points, uint32_t count);
                        /**
                         * @brief The outline of a line @p width wide along @p count points (closed:
                         *        back to the first), with round joins and ends.
                         */
                        void addStroke(const Point *points, uint32_t count, bool closed, float width);

                        /** @brief Every point through @p transform. */
                        void transform(const Affine &transform);
                        /** @brief The box round every point. False if there are none. */
                        bool bounds(float *x0, float *y0, float *x1, float *y1) const;

                        uint32_t     pointCount() const { return _count; }
                        const Point *points() const { return _points; }
                        /** @brief Where each outline starts, in points (outlineCount() of them). */
                        const uint32_t *outlines() const { return _starts; }
                        uint32_t        outlineCount() const { return _outlines; }
                        /** @brief False if a point or an outline couldn't be added (no memory). */
                        bool isComplete() const { return !_failed; }

                private:
                        bool grow(uint32_t points);

                        Point    *_points = nullptr;
                        uint32_t  _count = 0;
                        uint32_t  _capacity = 0;
                        uint32_t *_starts = nullptr;
                        uint32_t  _outlines = 0;
                        uint32_t  _startCapacity = 0;
                        bool      _failed = false;
        };

        /** @brief Sides for a circle of radius @p r to look round (8 to 256). */
        uint32_t circleSides(float r);

        /**
         * @brief @p n points round an ellipse (@p cx, @p cy, radii @p rx, @p ry) from angle @p start
         *        (radians), clockwise on the screen, or the other way with @p reverse. Two sines a
         *        circle: the rest by turning a step (libm's are double, which the board's core
         *        computes in software).
         */
        void ellipsePoints(Point *out, uint32_t n, float cx, float cy, float rx, float ry, float start, bool reverse);

        /**
         * @brief Fills @p path in @p color, @p alpha of it (0-255), inside the canvas's clip.
         * @return Error::NoMemory if a row's buffer couldn't be had, else Ok.
         */
        Error fillPath(Canvas &canvas, const Path &path, Color color, uint8_t alpha);

        /**
         * @brief A coverage mask (@p width × @p height, a byte a pixel, @p stride bytes a row)
         *        drawn in @p color, @p alpha of it, through @p transform (the mask's pixels to the
         *        canvas's), sampled between its pixels when it is scaled or turned. With @p source,
         *        only that part of the mask (text being written).
         */
        void drawMask(Canvas &canvas, const uint8_t *mask, int32_t width, int32_t height, uint32_t stride,
                      const Affine &transform, Color color, uint8_t alpha, const Rect *source = nullptr);

        /** @brief A scaled picture drawn through @p transform, @p alpha of it. */
        void drawPicture(Canvas &canvas, const ScaledImage &image, const Affine &transform, uint8_t alpha);

        /** @brief @p rect's box through @p transform: its bounds, whole pixels round them. */
        Rect transformedBounds(const Affine &transform, float x, float y, float width, float height);

} // namespace preso

#endif // PRESO_RASTER_H
