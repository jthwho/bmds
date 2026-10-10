/**
 * @file      cpumeterpicture.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tray's CPU meter's picture (devplan/sysmon.md §3.5, step 5).
 */

#include <bmds/graphlayout.h>
#include <bmds/icon.h>

#include "cpumeterpicture.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint16_t Side = CpuMeterPicture::Side;

        void fill(uint32_t (&out)[Side * Side], uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint32_t argb) {
                for (uint16_t r = y; r < y + h && r < Side; ++r)
                        for (uint16_t c = x; c < x + w && c < Side; ++c) out[r * Side + c] = argb;
                return;
        }

        void ring(uint32_t (&out)[Side * Side], uint16_t inset, uint32_t argb) {
                const uint16_t n = uint16_t(Side - 2 * inset);
                fill(out, inset, inset, n, 1, argb);
                fill(out, inset, uint16_t(Side - 1 - inset), n, 1, argb);
                fill(out, inset, inset, 1, n, argb);
                fill(out, uint16_t(Side - 1 - inset), inset, 1, n, argb);
                return;
        }

        // @p samples as bars standing on row @p bottom (exclusive), @p height at most, in the inside.
        // Under half a pixel is none: a graph this small would show an idle core as a line.
        void bars(uint32_t (&out)[Side * Side], Span<const int32_t> samples, int64_t full, uint16_t bottom,
                  uint16_t height) {
                constexpr size_t n = CpuMeterPicture::Samples;
                int32_t          kept[n];
                const size_t     count = samples.size() < n ? samples.size() : n;
                for (size_t i = 0; i < count; ++i) {
                        const int32_t v = samples[samples.size() - count + i];
                        kept[i] = int64_t(v) * 2 * height < full ? 0 : v;
                }
                GraphLayout::forEachRun(
                        Span<const int32_t>(kept, count), full, n, height, n, [&](const GraphLayout::Run &run) {
                                fill(out, uint16_t(CpuMeterPicture::Frame + run.x), uint16_t(bottom - run.height),
                                     run.width, run.height, CpuMeterPicture::BarColor);
                        });
                return;
        }

} // namespace

void CpuMeterPicture::draw(const Input &in, uint32_t (&out)[Side * Side]) {
        fill(out, 0, 0, Side, Side, FieldColor);
        const int64_t full = in.full > 0 ? in.full : 1;
        if (in.counted && in.coreCount > 0) {
                if (in.form == Form::Cores && in.coreCount >= 2) {
                        bars(out, in.cores[0], full, Frame + Band, BandBars);
                        bars(out, in.cores[1], full, Frame + 2 * Band, BandBars);
                } else {
                        // The mean of the cores' newest samples (as many as the shortest has).
                        const size_t cores = in.coreCount < 2 ? in.coreCount : 2;
                        size_t       n = Samples;
                        for (size_t c = 0; c < cores; ++c) n = in.cores[c].size() < n ? in.cores[c].size() : n;
                        int32_t mean[Samples] = {};
                        for (size_t i = 0; i < n; ++i) {
                                int64_t sum = 0;
                                for (size_t c = 0; c < cores; ++c) sum += in.cores[c][in.cores[c].size() - n + i];
                                mean[i] = int32_t(sum / int64_t(cores));
                        }
                        bars(out, Span<const int32_t>(mean, n), full, Frame + Samples, Samples);
                }
        }
        ring(out, 0, FrameColor);
        if (!in.counted)
                for (uint32_t &p : out) p = halfAlpha(p);
        if (in.alarm != Stat::Health::Normal) {
                const uint32_t color = in.alarm == Stat::Health::Critical ? CritColor : WarnColor;
                ring(out, 0, color);
                ring(out, 1, color);
        }
        return;
}

BMDS_NAMESPACE_END
