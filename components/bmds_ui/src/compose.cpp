/**
 * @file      compose.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/compose.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The plan's working space: rectangles still to be covered by lower layers.
        constexpr size_t PlanWork = 128;

} // namespace

Result<size_t> planCompose(Span<const Rect> layers, const Rect &bottom, const Region &damage, Span<ComposePiece> out) {
        Rect   work[2][PlanWork];
        Rect  *open = work[0];
        Rect  *next = work[1];
        size_t openCount = 0;
        for (const Rect &r : damage.rects()) open[openCount++] = r;

        size_t pieces = 0;
        for (size_t layer = 0; layer <= layers.size() && openCount > 0; ++layer) {
                const Rect &l = layer < layers.size() ? layers[layer] : bottom;
                size_t      nextCount = 0;
                for (size_t i = 0; i < openCount; ++i) {
                        const Rect piece = open[i].intersected(l);
                        if (piece.isEmpty()) {
                                next[nextCount++] = open[i];
                                continue;
                        }
                        if (pieces == out.size()) return makeError<size_t>(Error::Full);
                        out[pieces++] = ComposePiece{static_cast<uint16_t>(layer), piece};
                        Rect         rest[4];
                        const size_t n = subtract(open[i], l, rest);
                        if (nextCount + n > PlanWork) return makeError<size_t>(Error::Full);
                        for (size_t k = 0; k < n; ++k) next[nextCount++] = rest[k];
                }
                Rect *t = open;
                open = next;
                next = t;
                openCount = nextCount;
        }
        return makeResult(pieces);
}

BMDS_NAMESPACE_END
