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

        constexpr size_t MaxUsages = 16; // local usages kept per main item
        constexpr size_t MaxIds = 16;    // report IDs whose input offsets are counted
        constexpr size_t MaxPush = 4;

        struct Globals {
                        uint32_t page = 0;
                        int32_t  logicalMin = 0;
                        uint32_t size = 0;
                        uint32_t count = 0;
                        uint8_t  id = 0;
        };

        // A short item's data, unsigned and sign-extended.
        uint32_t unsignedData(const uint8_t *p, size_t n) {
                uint32_t v = 0;
                for (size_t i = n; i-- > 0;) v = v << 8 | p[i];
                return v;
        }

        int32_t signedData(const uint8_t *p, size_t n) {
                const uint32_t v = unsignedData(p, n);
                if (n == 1) return static_cast<int8_t>(v);
                if (n == 2) return static_cast<int16_t>(v);
                return static_cast<int32_t>(v);
        }

        // @p field of @p data (@p length bytes), or false past the end.
        bool extract(const uint8_t *data, size_t length, const MouseLayout::Field &field, int32_t &out) {
                const uint32_t first = field.offset / 8u;
                const uint32_t last = (uint32_t(field.offset) + field.size - 1u) / 8u;
                if (last >= length) return false;
                uint64_t bits = 0;
                for (uint32_t b = last + 1; b-- > first;) bits = bits << 8 | data[b];
                bits >>= field.offset % 8u;
                const uint64_t mask = field.size >= 32 ? 0xffffffffull : (1ull << field.size) - 1u;
                uint32_t       v = static_cast<uint32_t>(bits & mask);
                if (field.isSigned && field.size < 32 && (v & (1u << (field.size - 1))) != 0) v |= ~uint32_t(mask);
                out = field.isSigned ? static_cast<int32_t>(v) : static_cast<int32_t>(v & 0x7fffffffu);
                return true;
        }

        int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
                return v < lo ? lo : v > hi ? hi : v;
        }

} // namespace

MouseLayout MouseLayout::parse(Span<const uint8_t> descriptor) {
        MouseLayout layout;
        Globals     g;
        Globals     stack[MaxPush];
        size_t      pushed = 0;
        uint32_t    usages[MaxUsages] = {};
        size_t      usageCount = 0;
        uint32_t    usageMin = 0, usageMax = 0;
        bool        hasRange = false;
        uint8_t     ids[MaxIds] = {};
        uint32_t    offsets[MaxIds] = {}; // input bits so far, per report ID
        size_t      idCount = 0;
        int         depth = 0;
        int         mouseDepth = -1; // the mouse's application collection, while inside it
        bool        mouseSeen = false;
        bool        fieldSeen = false; // the mouse's report ID is taken from its first field

        // A usage as given (a 4-byte one carries its page), with the page in effect.
        auto full = [&g](uint32_t v, size_t n) {
                return n == 4 ? v : (g.page << 16 | (v & 0xffffu));
        };
        auto offsetOf = [&](uint8_t id) -> uint32_t * {
                for (size_t i = 0; i < idCount; ++i)
                        if (ids[i] == id) return &offsets[i];
                if (idCount == MaxIds) return nullptr;
                ids[idCount] = id;
                offsets[idCount] = 0;
                return &offsets[idCount++];
        };

        const uint8_t *p = descriptor.data();
        const uint8_t *end = p + descriptor.size();
        while (p < end) {
                const uint8_t prefix = *p++;
                if (prefix == 0xfe) { // a long item: its size, its tag, its data
                        if (end - p < 2) break;
                        const size_t n = p[0];
                        p += 2;
                        if (size_t(end - p) < n) break;
                        p += n;
                        continue;
                }
                const size_t n = (prefix & 3u) == 3u ? 4 : (prefix & 3u);
                if (size_t(end - p) < n) break;
                const uint8_t  type = (prefix >> 2) & 3u;
                const uint8_t  tag = prefix >> 4;
                const uint32_t value = unsignedData(p, n);
                const int32_t  svalue = signedData(p, n);
                if (type == 1) { // global
                        switch (tag) {
                                case 0x0: g.page = value; break;
                                case 0x1: g.logicalMin = n == 0 ? 0 : svalue; break;
                                case 0x7: g.size = value; break;
                                case 0x8: g.id = static_cast<uint8_t>(value); break;
                                case 0x9: g.count = value; break;
                                case 0xa:
                                        if (pushed < MaxPush) stack[pushed++] = g;
                                        break;
                                case 0xb:
                                        if (pushed > 0) g = stack[--pushed];
                                        break;
                                default: break;
                        }
                } else if (type == 2) { // local
                        if (tag == 0x0 && usageCount < MaxUsages) usages[usageCount++] = full(value, n);
                        if (tag == 0x1) {
                                usageMin = full(value, n);
                                hasRange = true;
                        }
                        if (tag == 0x2) usageMax = full(value, n);
                } else if (type == 0) {   // main
                        if (tag == 0xa) { // a collection
                                if (mouseDepth < 0 && !mouseSeen && value == 0x01 && usageCount > 0 &&
                                    usages[0] == Mouse)
                                        mouseDepth = depth;
                                ++depth;
                        } else if (tag == 0xc) { // its end
                                --depth;
                                if (mouseDepth >= 0 && depth == mouseDepth) {
                                        mouseDepth = -1;
                                        mouseSeen = true;
                                }
                        } else if (tag == 0x8) {                    // an input
                                uint32_t  *offset = offsetOf(g.id); // null past MaxIds IDs: not counted
                                const bool variable = (value & 0x2u) != 0, constant = (value & 0x1u) != 0;
                                const bool mine = offset != nullptr && mouseDepth >= 0 && variable && !constant;
                                for (uint32_t i = 0; mine && i < g.count; ++i) {
                                        uint32_t usage = 0;
                                        if (i < usageCount) {
                                                usage = usages[i];
                                        } else if (hasRange) {
                                                usage = usageMin + i < usageMax ? usageMin + i : usageMax;
                                        } else if (usageCount > 0) {
                                                usage = usages[usageCount - 1];
                                        }
                                        const uint32_t bit = *offset + i * g.size;
                                        if (bit > 0xffff || g.size == 0 || g.size > 32) continue;
                                        if (fieldSeen && g.id != layout._reportId) continue; // the mouse's ID only
                                        const Field f{static_cast<uint16_t>(bit), static_cast<uint8_t>(g.size),
                                                      g.logicalMin < 0};
                                        bool        taken = true;
                                        if (usage == X) {
                                                layout._x = f;
                                        } else if (usage == Y) {
                                                layout._y = f;
                                        } else if (usage == Wheel) {
                                                layout._wheel = f;
                                        } else if ((usage >> 16) == ButtonPage && g.size == 1 &&
                                                   (usage & 0xffffu) >= 1 && (usage & 0xffffu) <= 8) {
                                                const uint32_t button = usage & 0xffffu;
                                                if (button == 1) layout._buttons = static_cast<uint16_t>(bit);
                                                if (button > layout._buttonCount)
                                                        layout._buttonCount = static_cast<uint8_t>(button);
                                        } else {
                                                taken = false;
                                        }
                                        if (taken && !fieldSeen) {
                                                fieldSeen = true;
                                                layout._reportId = g.id;
                                        }
                                }
                                if (offset != nullptr) *offset += g.size * g.count;
                        }
                        usageCount = 0; // the locals go with each main item
                        hasRange = false;
                }
                p += n;
        }
        if (!layout.isValid()) layout = MouseLayout();
        return layout;
}

bool MouseLayout::decode(Span<const uint8_t> report, PointerEvent &event) const {
        const uint8_t *data = report.data();
        size_t         length = report.size();
        if (_reportId != 0) {
                if (length == 0 || data[0] != _reportId) return false;
                ++data;
                --length;
        }
        int32_t x = 0, y = 0, wheel = 0, buttons = 0;
        if (!extract(data, length, _x, x) || !extract(data, length, _y, y)) return false;
        if (_wheel.size != 0 && !extract(data, length, _wheel, wheel)) wheel = 0;
        if (_buttonCount != 0 && !extract(data, length, Field{_buttons, _buttonCount, false}, buttons)) buttons = 0;
        event = PointerEvent();
        event.dx = static_cast<int16_t>(clamp(x, -32768, 32767));
        event.dy = static_cast<int16_t>(clamp(y, -32768, 32767));
        event.wheel = static_cast<int8_t>(clamp(wheel, -128, 127));
        event.buttons = static_cast<uint8_t>(buttons);
        return true;
}

BMDS_NAMESPACE_END
