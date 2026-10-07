/**
 * @file      bevelpainter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/bevel.h>
#include <bmds/painter.h>

BMDS_NAMESPACE_BEGIN

void drawBevel(Painter &painter, const Rect &rect, Bevel::Style style) {
        Bevel::Edge  edges[Bevel::MaxEdges];
        const size_t n = Bevel::edges(rect, style, edges);
        // One fill a shade: the edges of a shade gathered.
        bool done[Bevel::MaxEdges] = {};
        for (size_t i = 0; i < n; ++i) {
                if (done[i]) continue;
                Rect   same[Bevel::MaxEdges];
                size_t k = 0;
                for (size_t j = i; j < n; ++j) {
                        if (done[j] || edges[j].role != edges[i].role) continue;
                        same[k++] = edges[j].rect;
                        done[j] = true;
                }
                (void)painter.fill(Span<const Rect>(same, k), painter.palette().color(edges[i].role));
        }
        return;
}

BMDS_NAMESPACE_END
