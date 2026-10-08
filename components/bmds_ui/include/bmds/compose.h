/**
 * @file      compose.h
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
#include <bmds/region.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/** @brief One copy of a compose: a rectangle of the screen from a layer. */
struct ComposePiece {
                uint16_t layer = 0; ///< Index into the layers given.
                Rect     rect;      ///< Screen coordinates, inside the layer.
};

/**
 * @brief Plans a compose: which part of each layer shows in @p damage.
 *
 * @p layers are the opaque layers' screen rectangles, the top one first;
 * @p bottom is one more under them (layer `layers.size()`, the desktop
 * covering the screen; empty: none). Each damaged pixel comes from
 * the topmost layer covering it; pixels no layer covers are left out. The
 * pieces are disjoint, so nothing is drawn twice. Horizontal edges are
 * only the inputs', so inputs on even columns give even pieces.
 *
 * @return The pieces written to @p out, or `Full` (more pieces than @p out
 *         holds, or than the plan's working space: the caller paints the
 *         damage's bounds bottom to top instead).
 */
Result<size_t> planCompose(Span<const Rect> layers, const Rect &bottom, const Region &damage, Span<ComposePiece> out);

BMDS_NAMESPACE_END
