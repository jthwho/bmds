/**
 * @file      graph.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A graph of samples over time (devplan/sysmon.md §3.5).
 */

#include <cstring>
#include <bmds/bevel.h>
#include <bmds/font.h>
#include <bmds/graph.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t FieldColor = 0x000000;
        constexpr uint32_t GridColor = 0x003a00;
        constexpr uint32_t CaptionColor = 0xe0e0e0;
        constexpr uint32_t WarnColor = 0xffb000;
        constexpr uint32_t CritColor = 0xff2020;
        constexpr uint16_t Border = 2; // the health's, inside the bevel
        constexpr size_t   BatchRects = 32;

        // Fills rectangles of one colour, a batch at a time.
        class Fills {
                public:
                        Fills(Painter &painter, uint32_t rgb) : _painter(painter), _rgb(rgb) {}
                        ~Fills() { flush(); }
                        Fills(const Fills &) = delete;
                        Fills &operator=(const Fills &) = delete;

                        void add(const Rect &r) {
                                if (r.isEmpty()) return;
                                if (_count == BatchRects) flush();
                                _rects[_count++] = r;
                                return;
                        }

                private:
                        void flush() {
                                if (_count != 0) (void)_painter.fill(Span<const Rect>(_rects, _count), _rgb);
                                _count = 0;
                                return;
                        }

                        Painter &_painter;
                        uint32_t _rgb;
                        Rect     _rects[BatchRects];
                        size_t   _count = 0;
        };

} // namespace

Graph::Graph(Widget *parent) : Widget(parent) {}

void Graph::setSamples(size_t series, Span<const int32_t> samples) {
        if (series >= MaxSeries) return;
        const size_t n = samples.size() < Capacity ? samples.size() : Capacity;
        const size_t first = samples.size() - n;
        if (n == _counts[series] && std::memcmp(_samples[series], samples.data() + first, n * sizeof(int32_t)) == 0)
                return;
        std::memcpy(_samples[series], samples.data() + first, n * sizeof(int32_t));
        _counts[series] = uint16_t(n);
        update();
        return;
}

void Graph::setSeriesCount(size_t count) {
        _series = uint8_t(count == 0 ? 1 : count > MaxSeries ? MaxSeries : count);
        update();
        return;
}

void Graph::setColor(size_t series, uint32_t rgb) {
        if (series >= MaxSeries || _colors[series] == rgb) return;
        _colors[series] = rgb;
        update();
        return;
}

void Graph::setMaximum(int64_t maximum) {
        if (_maximum == maximum) return;
        _maximum = maximum;
        update();
        return;
}

void Graph::setSlots(size_t slots) {
        _slots = uint16_t(slots == 0 ? 1 : slots > UINT16_MAX ? UINT16_MAX : slots);
        update();
        return;
}

void Graph::setLevels(int64_t warn, int64_t crit) {
        if (_warn == warn && _crit == crit) return;
        _warn = warn;
        _crit = crit;
        update();
        return;
}

void Graph::setCaption(StringView caption) {
        char   next[MaxCaption + 1];
        size_t n = caption.size() < MaxCaption ? caption.size() : MaxCaption;
        if (n < caption.size())
                while (n > 0 && (static_cast<uint8_t>(caption[n]) & 0xc0u) == 0x80u) --n;
        std::memcpy(next, caption.data(), n);
        next[n] = '\0';
        if (std::strcmp(next, _caption) == 0) return;
        std::memcpy(_caption, next, sizeof(_caption));
        update();
        return;
}

void Graph::setHealth(Health health) {
        if (_health == health) return;
        _health = health;
        update();
        return;
}

int64_t Graph::scale() const {
        if (_maximum > 0) return _maximum;
        int64_t top = 0;
        for (size_t s = 0; s < _series; ++s)
                for (size_t i = 0; i < _counts[s]; ++i) top = _samples[s][i] > top ? _samples[s][i] : top;
        // The levels stay in sight: a quiet graph shows how far it is from them.
        if (_warn != NoLevel && _warn > top) top = _warn;
        if (_crit != NoLevel && _crit > top) top = _crit;
        return GraphLayout::niceMaximum(top);
}

void Graph::paint(Painter &painter) {
        const Rect &r = geometry();
        drawBevel(painter, r, Bevel::Style::Status);
        Rect in = Bevel::inside(r, Bevel::Style::Status);
        if (in.width <= 2 * Border || in.height <= 2 * Border) return;
        if (_health != Health::Normal) {
                const uint32_t color = _health == Health::Critical ? CritColor : WarnColor;
                Fills          frame(painter, color);
                frame.add(Rect{in.x, in.y, in.width, Border});
                frame.add(Rect{in.x, uint16_t(in.bottom() - Border), in.width, Border});
                frame.add(Rect{in.x, uint16_t(in.y + Border), Border, uint16_t(in.height - 2 * Border)});
                frame.add(Rect{uint16_t(in.right() - Border), uint16_t(in.y + Border), Border,
                               uint16_t(in.height - 2 * Border)});
        }
        in = Rect{uint16_t(in.x + Border), uint16_t(in.y + Border), uint16_t(in.width - 2 * Border),
                  uint16_t(in.height - 2 * Border)};
        (void)painter.fill(in, FieldColor);
        const int64_t top = scale();
        {
                Fills grid(painter, GridColor);
                for (int q = 1; q < 4; ++q) grid.add(Rect{in.x, uint16_t(in.y + in.height * q / 4), in.width, 1});
        }
        for (size_t s = 0; s < _series; ++s) {
                Fills bars(painter, _colors[s]);
                GraphLayout::forEachRun(Span<const int32_t>(_samples[s], _counts[s]), top, in.width, in.height, _slots,
                                        [&](const GraphLayout::Run &run) {
                                                bars.add(Rect{uint16_t(in.x + run.x),
                                                              uint16_t(in.y + in.height - run.height), run.width,
                                                              run.height});
                                        });
        }
        const auto level = [&](int64_t value, uint32_t color) {
                if (value == NoLevel || value <= 0 || value > top) return;
                const uint16_t h = GraphLayout::barHeight(value, top, in.height);
                (void)painter.fill(Rect{in.x, uint16_t(in.y + in.height - (h > 0 ? h : 1)), in.width, 1}, color);
        };
        level(_warn, WarnColor);
        level(_crit, CritColor);
        if (_caption[0] != '\0' && in.height >= GuiStyle::TextHeight + 4) {
                const uint16_t x = uint16_t((in.x + 2 + GuiStyle::Column - 1) & ~(GuiStyle::Column - 1));
                const uint16_t room = uint16_t(in.right() > x ? in.right() - x : 0);
                (void)painter.text(x, in.y + 2, StringView(_caption), Fonts::unscii16, CaptionColor, FieldColor, room);
        }
        return;
}

BMDS_NAMESPACE_END
