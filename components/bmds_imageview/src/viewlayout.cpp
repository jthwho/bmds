/**
 * @file      viewlayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Where the image viewer's picture goes (devplan/images.md §4b).
 */

#include "viewlayout.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Align = 4;

        int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
                return v < lo ? lo : v > hi ? hi : v;
        }

} // namespace

ViewLayout ViewLayout::make(const Input &in) {
        ViewLayout l;
        if (in.sourceWidth == 0 || in.sourceHeight == 0 || in.viewWidth < Align || in.viewHeight == 0) return l;
        // The zoom: the fit's (never above 100%), or the one asked for.
        uint32_t z = in.zoom != 0 ? in.zoom : Actual;
        if (in.fit) {
                const uint64_t zx = uint64_t(in.viewWidth) * Actual / in.sourceWidth;
                const uint64_t zy = uint64_t(in.viewHeight) * Actual / in.sourceHeight;
                z = uint32_t(zx < zy ? zx : zy);
                if (z > Actual) z = Actual;
                if (z == 0) z = 1;
        }
        l.zoom = z;
        // The whole picture at it: across a multiple of 4, down from that width (the aspect kept). A fit
        // from the view's sides themselves (the zoom in thousandths would lose pixels).
        uint32_t w = 0;
        if (in.fit && z < Actual) {
                const bool acrossLimits =
                        uint64_t(in.viewWidth) * in.sourceHeight <= uint64_t(in.viewHeight) * in.sourceWidth;
                w = acrossLimits ? in.viewWidth : uint32_t(uint64_t(in.sourceWidth) * in.viewHeight / in.sourceHeight);
        } else {
                w = uint32_t(uint64_t(in.sourceWidth) * z / Actual);
        }
        w &= ~(Align - 1);
        if (w < Align) w = Align;
        uint32_t h = uint32_t((uint64_t(in.sourceHeight) * w + in.sourceWidth / 2) / in.sourceWidth);
        if (in.fit && h > in.viewHeight) h = in.viewHeight;
        if (h == 0) h = 1;
        l.width = w;
        l.height = h;
        // Across: centred if it fits, else panned in bounds (in fours).
        const uint32_t viewW = in.viewWidth & ~(Align - 1);
        if (w <= viewW) {
                l.shownWidth = w;
                l.x = ((viewW - w) / 2) & ~(Align - 1);
        } else {
                l.shownWidth = viewW;
                l.panX = clamp(in.panX, 0, int32_t(w - viewW)) & ~int32_t(Align - 1);
        }
        if (h <= in.viewHeight) {
                l.shownHeight = h;
                l.y = (in.viewHeight - h) / 2;
        } else {
                l.shownHeight = in.viewHeight;
                l.panY = clamp(in.panY, 0, int32_t(h - in.viewHeight));
        }
        // The part that shows, in the decoded picture.
        l.cropX = uint32_t(uint64_t(l.panX) * in.sourceWidth / w);
        l.cropY = uint32_t(uint64_t(l.panY) * in.sourceHeight / h);
        l.cropWidth = uint32_t((uint64_t(l.shownWidth) * in.sourceWidth + w - 1) / w);
        l.cropHeight = uint32_t((uint64_t(l.shownHeight) * in.sourceHeight + h - 1) / h);
        if (l.cropX + l.cropWidth > in.sourceWidth) l.cropWidth = in.sourceWidth - l.cropX;
        if (l.cropY + l.cropHeight > in.sourceHeight) l.cropHeight = in.sourceHeight - l.cropY;
        if (l.cropWidth == 0) l.cropWidth = 1;
        if (l.cropHeight == 0) l.cropHeight = 1;
        return l;
}

uint32_t ViewLayout::step(uint32_t zoom, bool in) {
        constexpr size_t N = sizeof(Steps) / sizeof(Steps[0]);
        if (in) {
                for (size_t i = 0; i < N; ++i)
                        if (Steps[i] > zoom) return Steps[i];
                return Steps[N - 1];
        }
        for (size_t i = N; i > 0; --i)
                if (Steps[i - 1] < zoom) return Steps[i - 1];
        return Steps[0];
}

BMDS_NAMESPACE_END
