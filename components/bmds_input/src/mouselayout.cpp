/**
 * @file      mouselayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A report-protocol mouse's layout from its HID report descriptor
 * (devplan/input.md §5a).
 */

#include <bmds/hiddescriptor.h>
#include <bmds/mouselayout.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Usages, page in the high half.
        constexpr uint32_t GenericDesktop = 0x01;
        constexpr uint32_t ButtonPage = 0x09;
        constexpr uint32_t Mouse = GenericDesktop << 16 | 0x02;
        constexpr uint32_t X = GenericDesktop << 16 | 0x30;
        constexpr uint32_t Y = GenericDesktop << 16 | 0x31;
        constexpr uint32_t Wheel = GenericDesktop << 16 | 0x38;

        int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
                return v < lo ? lo : v > hi ? hi : v;
        }

        bool extract(Span<const uint8_t> data, const MouseLayout::Field &field, int32_t &out) {
                return HidDescriptor::extract(data, field.offset, field.size, field.isSigned, out);
        }

} // namespace

// The first mouse collection's variable inputs, of the report ID its first known field has.
struct MouseLayout::Builder {
                MouseLayout layout;
                int         collection = -1; // the mouse's, once found
                bool        fieldSeen = false;

                static void visit(void *arg, const HidDescriptor::Element &e) {
                        auto &b = *static_cast<Builder *>(arg);
                        if (e.application != Mouse || (e.flags & HidDescriptor::Variable) == 0) return;
                        if (b.collection < 0) b.collection = e.collection;
                        if (e.collection != b.collection) return;
                        if (b.fieldSeen && e.reportId != b.layout._reportId) return; // the mouse's ID only
                        MouseLayout &l = b.layout;
                        const Field  f{e.offset, e.size, e.isSigned()};
                        if (e.usage == X) {
                                l._x = f;
                        } else if (e.usage == Y) {
                                l._y = f;
                        } else if (e.usage == Wheel) {
                                l._wheel = f;
                        } else if ((e.usage >> 16) == ButtonPage && e.size == 1 && (e.usage & 0xffffu) >= 1 &&
                                   (e.usage & 0xffffu) <= 8) {
                                const uint32_t button = e.usage & 0xffffu;
                                if (button == 1) l._buttons = e.offset;
                                if (button > l._buttonCount) l._buttonCount = static_cast<uint8_t>(button);
                        } else {
                                return;
                        }
                        if (!b.fieldSeen) {
                                b.fieldSeen = true;
                                l._reportId = e.reportId;
                        }
                        return;
                }
};

MouseLayout MouseLayout::parse(Span<const uint8_t> descriptor) {
        Builder b;
        HidDescriptor::parse(descriptor, Builder::visit, &b);
        if (!b.layout.isValid()) return MouseLayout();
        return b.layout;
}

bool MouseLayout::decode(Span<const uint8_t> report, PointerEvent &event) const {
        const uint8_t *data = report.data();
        size_t         length = report.size();
        if (_reportId != 0) {
                if (length == 0 || data[0] != _reportId) return false;
                ++data;
                --length;
        }
        const Span<const uint8_t> fields(data, length);
        int32_t                   x = 0, y = 0, wheel = 0, buttons = 0;
        if (!extract(fields, _x, x) || !extract(fields, _y, y)) return false;
        if (_wheel.size != 0 && !extract(fields, _wheel, wheel)) wheel = 0;
        if (_buttonCount != 0 && !extract(fields, Field{_buttons, _buttonCount, false}, buttons)) buttons = 0;
        event = PointerEvent();
        event.dx = static_cast<int16_t>(clamp(x, -32768, 32767));
        event.dy = static_cast<int16_t>(clamp(y, -32768, 32767));
        event.wheel = static_cast<int8_t>(clamp(wheel, -128, 127));
        event.buttons = static_cast<uint8_t>(buttons);
        return true;
}

BMDS_NAMESPACE_END
