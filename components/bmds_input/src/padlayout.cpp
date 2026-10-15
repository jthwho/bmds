/**
 * @file      padlayout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A HID joystick's or game pad's layout from its report descriptor (devplan/controls.md §7a).
 */

#include <bmds/hiddescriptor.h>
#include <bmds/padlayout.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Usages, page in the high half.
        constexpr uint32_t GenericDesktop = 0x01;
        constexpr uint32_t ButtonPage = 0x09;
        constexpr uint32_t Joystick = GenericDesktop << 16 | 0x04;
        constexpr uint32_t GamePad = GenericDesktop << 16 | 0x05;
        constexpr uint32_t MultiAxis = GenericDesktop << 16 | 0x08;
        constexpr uint32_t X = GenericDesktop << 16 | 0x30;
        constexpr uint32_t Wheel = GenericDesktop << 16 | 0x38;
        constexpr uint32_t HatSwitch = GenericDesktop << 16 | 0x39;

        // A hat's eight directions, clockwise from up.
        constexpr uint8_t Directions[8] = {
                RawPad::HatUp,    RawPad::HatUp | RawPad::HatRight,
                RawPad::HatRight, RawPad::HatDown | RawPad::HatRight,
                RawPad::HatDown,  RawPad::HatDown | RawPad::HatLeft,
                RawPad::HatLeft,  RawPad::HatUp | RawPad::HatLeft,
        };

} // namespace

// The first pad collection's absolute variable inputs, of the report ID its first field has.
struct PadLayout::Builder {
                PadLayout layout;
                int       collection = -1; // the pad's, once found
                bool      fieldSeen = false;

                static void visit(void *arg, const HidDescriptor::Element &e) {
                        auto &b = *static_cast<Builder *>(arg);
                        if (e.application != Joystick && e.application != GamePad && e.application != MultiAxis) return;
                        if ((e.flags & (HidDescriptor::Variable | HidDescriptor::Relative)) != HidDescriptor::Variable)
                                return;
                        if (b.collection < 0) b.collection = e.collection;
                        if (e.collection != b.collection) return;
                        if (b.fieldSeen && e.reportId != b.layout._reportId) return; // the pad's ID only
                        PadLayout  &l = b.layout;
                        const Field f{e.logicalMin, e.logicalMax, e.offset, e.size, e.isSigned()};
                        if (e.usage >= X && e.usage <= Wheel) {
                                if (l._axisCount == RawPad::MaxAxes) return;
                                l._axisUsages[l._axisCount] = static_cast<uint8_t>(e.usage);
                                l._axes[l._axisCount++] = f;
                        } else if (e.usage == HatSwitch) {
                                if (l._hatCount == RawPad::MaxHats) return;
                                l._hats[l._hatCount++] = f;
                        } else if ((e.usage >> 16) == ButtonPage && (e.usage & 0xffffu) >= 1 &&
                                   (e.usage & 0xffffu) <= RawPad::MaxButtons) {
                                const uint32_t button = e.usage & 0xffffu;
                                l._buttons[button - 1] = e.offset;
                                l._buttonMask |= 1u << (button - 1);
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

PadLayout PadLayout::parse(Span<const uint8_t> descriptor) {
        Builder b;
        HidDescriptor::parse(descriptor, Builder::visit, &b);
        if (!b.layout.isValid()) return PadLayout();
        return b.layout;
}

bool PadLayout::decode(Span<const uint8_t> report, RawPad &pad) const {
        const uint8_t *data = report.data();
        size_t         length = report.size();
        if (_reportId != 0) {
                if (length == 0 || data[0] != _reportId) return false;
                ++data;
                --length;
        }
        const Span<const uint8_t> fields(data, length);
        pad = RawPad();
        int32_t v = 0;
        for (size_t i = 0; i < _axisCount; ++i) {
                const Field &f = _axes[i];
                if (!HidDescriptor::extract(fields, f.offset, f.size, f.isSigned, v)) return false;
                if (f.max <= f.min) continue;
                const int64_t scaled = (int64_t(v) - f.min) * 65535 / (int64_t(f.max) - f.min) - 32768;
                pad.axes[i] = static_cast<int16_t>(scaled < -32768 ? -32768 : scaled > 32767 ? 32767 : scaled);
        }
        for (size_t i = 0; i < _hatCount; ++i) {
                const Field &f = _hats[i];
                if (!HidDescriptor::extract(fields, f.offset, f.size, f.isSigned, v)) return false;
                if (v < f.min || v > f.max) continue; // its null state: centred
                const int32_t span = f.max - f.min + 1, direction = v - f.min;
                if (span == 8) pad.hats[i] = Directions[direction];
                if (span == 4) pad.hats[i] = Directions[direction * 2];
        }
        uint32_t mask = _buttonMask;
        while (mask != 0) {
                const unsigned b = static_cast<unsigned>(__builtin_ctz(mask));
                mask &= mask - 1;
                const uint16_t bit = _buttons[b];
                if (bit / 8u >= length) return false;
                if ((data[bit / 8u] >> (bit % 8u) & 1u) != 0) pad.buttons |= 1u << b;
        }
        return true;
}

BMDS_NAMESPACE_END
