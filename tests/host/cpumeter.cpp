/**
 * @file      cpumeter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tray's CPU meter (devplan/sysmon.md §3.5, step 5): its picture (the frame, the cores' bars in
 * either form, the alarm's ring, CPU time not counted) and the panel menu's edits of panel/items.
 */

#include <doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "cpumeterpicture.h"
#include "panelitems.h"

using bmds::CpuMeterPicture;
using bmds::PanelItems;
using bmds::Span;
namespace Stat = bmds::Stat;

namespace {

        using Picture = uint32_t[CpuMeterPicture::Side * CpuMeterPicture::Side];
        constexpr int     Side = CpuMeterPicture::Side;
        constexpr int64_t Full = 1000000;

        uint32_t at(const Picture &p, int x, int y) {
                return p[y * Side + x];
        }

        // The rows of column @p x that are bars, as a bit mask (bit y).
        uint32_t barRows(const Picture &p, int x) {
                uint32_t rows = 0;
                for (int y = 0; y < Side; ++y)
                        if (at(p, x, y) == CpuMeterPicture::BarColor) rows |= 1u << y;
                return rows;
        }

        uint32_t rowsFrom(int first, int last) {
                uint32_t rows = 0;
                for (int y = first; y <= last; ++y) rows |= 1u << y;
                return rows;
        }

        bool ringIs(const Picture &p, int inset, uint32_t argb) {
                for (int i = inset; i < Side - inset; ++i)
                        if (at(p, i, inset) != argb || at(p, i, Side - 1 - inset) != argb || at(p, inset, i) != argb ||
                            at(p, Side - 1 - inset, i) != argb)
                                return false;
                return true;
        }

} // namespace

TEST_CASE("CpuMeterPicture: an idle meter is a grey frame round a black field") {
        // An idle core's sliver (under half a pixel) shows nothing.
        int32_t idle[CpuMeterPicture::Samples];
        for (int32_t &s : idle) s = 2000;
        Picture                p;
        CpuMeterPicture::Input in;
        in.coreCount = 2;
        in.full = Full;
        in.cores[0] = Span<const int32_t>(idle, CpuMeterPicture::Samples);
        in.cores[1] = Span<const int32_t>(idle, CpuMeterPicture::Samples);
        CpuMeterPicture::draw(in, p);
        CHECK(ringIs(p, 0, CpuMeterPicture::FrameColor));
        for (int y = 1; y < Side - 1; ++y)
                for (int x = 1; x < Side - 1; ++x) CHECK(at(p, x, y) == CpuMeterPicture::FieldColor);
}

TEST_CASE("CpuMeterPicture: cores in their own bands, the newest at the right") {
        int32_t busy[CpuMeterPicture::Samples];
        for (int32_t &s : busy) s = int32_t(Full);
        const int32_t          one[] = {int32_t(Full)};
        Picture                p;
        CpuMeterPicture::Input in;
        in.coreCount = 2;
        in.full = Full;
        in.cores[0] = Span<const int32_t>(busy, CpuMeterPicture::Samples);
        in.cores[1] = Span<const int32_t>(one, 1);
        CpuMeterPicture::draw(in, p);
        // Core 0's band is rows 2-7, its top row a gap: full bars are rows 3-7, every column inside.
        for (int x = 2; x < Side - 2; ++x) {
                const uint32_t core1 = x == Side - 3 ? rowsFrom(9, 13) : 0; // one sample: the last column
                CHECK(barRows(p, x) == (rowsFrom(3, 7) | core1));
        }
        CHECK(barRows(p, 1) == 0);
        CHECK(barRows(p, Side - 2) == 0);
}

TEST_CASE("CpuMeterPicture: combined is the cores' mean over the whole height") {
        int32_t busy[4], idle[4];
        for (int i = 0; i < 4; ++i) {
                busy[i] = int32_t(Full);
                idle[i] = 0;
        }
        Picture                p;
        CpuMeterPicture::Input in;
        in.coreCount = 2;
        in.full = Full;
        in.form = CpuMeterPicture::Form::Combined;
        in.cores[0] = Span<const int32_t>(busy, 4);
        in.cores[1] = Span<const int32_t>(idle, 4);
        CpuMeterPicture::draw(in, p);
        for (int x = 0; x < Side; ++x)
                CHECK(barRows(p, x) == (x >= Side - 2 - 4 && x < Side - 2 ? rowsFrom(8, 13) : 0u));
}

TEST_CASE("CpuMeterPicture: an alarm colours both of the frame's pixels") {
        Picture                p;
        CpuMeterPicture::Input in;
        in.coreCount = 2;
        in.alarm = Stat::Health::Critical;
        CpuMeterPicture::draw(in, p);
        CHECK(ringIs(p, 0, CpuMeterPicture::CritColor));
        CHECK(ringIs(p, 1, CpuMeterPicture::CritColor));
        CpuMeterPicture::Input calm;
        calm.coreCount = 2;
        Picture q;
        CpuMeterPicture::draw(calm, q);
        CHECK(ringIs(q, 1, CpuMeterPicture::FieldColor));
        CHECK(at(p, 2, 2) == CpuMeterPicture::FieldColor);
        in.alarm = Stat::Health::Warning;
        CpuMeterPicture::draw(in, p);
        CHECK(ringIs(p, 1, CpuMeterPicture::WarnColor));
}

TEST_CASE("CpuMeterPicture: CPU time not counted: no bars, dimmed, still alarming") {
        int32_t busy[CpuMeterPicture::Samples];
        for (int32_t &s : busy) s = int32_t(Full);
        Picture                p;
        CpuMeterPicture::Input in;
        in.coreCount = 2;
        in.full = Full;
        in.counted = false;
        in.cores[0] = Span<const int32_t>(busy, CpuMeterPicture::Samples);
        in.cores[1] = Span<const int32_t>(busy, CpuMeterPicture::Samples);
        CpuMeterPicture::draw(in, p);
        for (int x = 0; x < Side; ++x) CHECK(barRows(p, x) == 0);
        CHECK(at(p, 5, 5) == 0x7f000000u);
        CHECK(ringIs(p, 0, 0x7f808080u));
        in.alarm = Stat::Health::Warning;
        CpuMeterPicture::draw(in, p);
        CHECK(ringIs(p, 0, CpuMeterPicture::WarnColor));
        CHECK(at(p, 5, 5) == 0x7f000000u);
}

TEST_CASE("PanelItems: an item taken out or put back, after the tray") {
        char out[64];
        auto with = [&](const char *list, const char *kind, bool shown) {
                const size_t n = PanelItems::withItem(list, kind, shown, bmds::Span<char>(out, sizeof(out)));
                CHECK(n == std::strlen(out));
                return std::string(out, n);
        };
        CHECK(with("launcher;windows;tray;clock", "clock", false) == "launcher;windows;tray");
        CHECK(with("launcher; CLOCK ;tray;clock", "clock", false) == "launcher;tray");
        CHECK(with("launcher;windows;tray", "clock", true) == "launcher;windows;tray;clock");
        CHECK(with("launcher;tray;windows", "clock", true) == "launcher;tray;clock;windows"); // beside it
        CHECK(with("launcher;volume;spacer", "clock", true) == "launcher;volume;clock;spacer");
        CHECK(with("launcher;windows", "clock", true) == "launcher;windows;clock");
        CHECK(with("launcher;Clock", "clock", true) == "launcher;Clock"); // as asked already
        CHECK(PanelItems::has("launcher; Clock", "clock"));
        CHECK_FALSE(PanelItems::has("launcher;tray", "clock"));
        char small[8];
        CHECK(PanelItems::withItem("launcher;tray", "clock", true, bmds::Span<char>(small, sizeof(small))) == 0);
}
