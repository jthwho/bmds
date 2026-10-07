/**
 * @file      bevel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/bevel.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Role = WindowPalette::Role;

        // A style's rings, outside in: the lit side's shade and the shaded side's.
        struct Ring {
                        Role lit;
                        Role shaded;
        };

        struct Look {
                        Ring   rings[2];
                        size_t count;
        };

        Look lookOf(Bevel::Style style) {
                switch (style) {
                        case Bevel::Style::Raised:
                                return Look{
                                        {{Role::FaceHighlight, Role::FaceDark}, {Role::FaceLight, Role::FaceShadow}},
                                        2};
                        case Bevel::Style::Sunken:
                                return Look{
                                        {{Role::FaceDark, Role::FaceHighlight}, {Role::FaceShadow, Role::FaceLight}},
                                        2};
                        case Bevel::Style::Window:
                                return Look{
                                        {{Role::FaceLight, Role::FaceDark}, {Role::FaceHighlight, Role::FaceShadow}},
                                        2};
                        case Bevel::Style::Field:
                                return Look{
                                        {{Role::FaceShadow, Role::FaceHighlight}, {Role::FaceDark, Role::FaceLight}},
                                        2};
                        case Bevel::Style::Status:
                                return Look{{{Role::FaceShadow, Role::FaceHighlight}, {Role::Face, Role::Face}}, 1};
                        case Bevel::Style::Thin:
                                return Look{{{Role::FaceHighlight, Role::FaceShadow}, {Role::Face, Role::Face}}, 1};
                }
                return Look{{{Role::Face, Role::Face}, {Role::Face, Role::Face}}, 0};
        }

} // namespace

uint16_t Bevel::width(Style style) {
        return static_cast<uint16_t>(lookOf(style).count);
}

Rect Bevel::inside(const Rect &rect, Style style) {
        const uint16_t w = width(style);
        if (rect.width <= 2 * w || rect.height <= 2 * w) return Rect{rect.x, rect.y, 0, 0};
        return Rect{uint16_t(rect.x + w), uint16_t(rect.y + w), uint16_t(rect.width - 2 * w),
                    uint16_t(rect.height - 2 * w)};
}

size_t Bevel::edges(const Rect &rect, Style style, Edge (&out)[MaxEdges]) {
        const Look look = lookOf(style);
        size_t     n = 0;
        Rect       r = rect;
        for (size_t i = 0; i < look.count && !r.isEmpty(); ++i) {
                const Ring &ring = look.rings[i];
                const auto  right = static_cast<uint16_t>(r.right() - 1);
                const auto  bottom = static_cast<uint16_t>(r.bottom() - 1);
                // The shaded side owns the corners it meets: bottom row whole, right column above it.
                out[n++] = Edge{Rect{r.x, bottom, r.width, 1}, ring.shaded};
                if (r.height > 1) out[n++] = Edge{Rect{right, r.y, 1, uint16_t(r.height - 1)}, ring.shaded};
                if (r.width > 1 && r.height > 1) out[n++] = Edge{Rect{r.x, r.y, uint16_t(r.width - 1), 1}, ring.lit};
                if (r.width > 1 && r.height > 2)
                        out[n++] = Edge{Rect{r.x, uint16_t(r.y + 1), 1, uint16_t(r.height - 2)}, ring.lit};
                if (r.width <= 2 || r.height <= 2) break;
                r = Rect{uint16_t(r.x + 1), uint16_t(r.y + 1), uint16_t(r.width - 2), uint16_t(r.height - 2)};
        }
        return n;
}

BMDS_NAMESPACE_END
