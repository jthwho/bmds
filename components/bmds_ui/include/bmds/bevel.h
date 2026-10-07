/**
 * @file      bevel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/rect.h>
#include <bmds/windowpalette.h>

BMDS_NAMESPACE_BEGIN

class Painter;

/**
 * @brief The edges that give a face depth (`devplan/gui.md` §4e): light
 *        from the top left, as the classic desktops drew them (Windows 95's
 *        3D look, Motif's shadows).
 *
 * A two-pixel bevel is two rings, each a lit side (top and left) and a shaded
 * side (bottom and right; it takes the corners where they meet: the top right
 * and bottom left pixels). The colors are the palette's four face shades,
 * `face-highlight` (lightest) to `face-dark`. Pure: `edges()` gives the
 * rectangles, `drawBevel()` draws them.
 *
 * @par Thread Safety
 * Reentrant.
 */
struct Bevel {
                /** @brief How a face stands. Append only. */
                enum class Style : uint8_t {
                        Raised, ///< A button: out of the face, two pixels.
                        Sunken, ///< A pressed or checked button: into it, two pixels.
                        Window, ///< A window, a menu, a panel: out, two pixels, its outer line softer.
                        Field,  ///< A text field: into the face, two pixels.
                        Status, ///< A status field (a panel's clock): into it, one pixel.
                        Thin,   ///< A flat button under the pointer: out, one pixel.
                };

                /** @brief One edge: a rectangle and its shade. */
                struct Edge {
                                Rect                rect;
                                WindowPalette::Role role = WindowPalette::Role::Face;
                };

                /** @brief The most edges a bevel has. */
                static constexpr size_t MaxEdges = 8;

                /** @brief Returns how many pixels deep @p style's edges are (1 or 2). */
                static uint16_t width(Style style);

                /** @brief Returns @p rect less @p style's edges (empty if they fill it). */
                static Rect inside(const Rect &rect, Style style);

                /**
                 * @brief Writes @p style's edges around the inside of @p rect into
                 *        @p out: disjoint, covering exactly its outer ring(s).
                 * @return Their count (0 for an empty rectangle).
                 */
                static size_t edges(const Rect &rect, Style style, Edge (&out)[MaxEdges]);
};

/**
 * @brief Draws @p style's edges around the inside of @p rect with @p painter
 *        (one fill a shade).
 */
void drawBevel(Painter &painter, const Rect &rect, Bevel::Style style);

BMDS_NAMESPACE_END
