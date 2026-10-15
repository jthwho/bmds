/**
 * @file      hiddescriptor.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A HID report descriptor's input elements (devplan/controls.md §7a).
 */

#include <bmds/hiddescriptor.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxUsages = 16; // local usages kept per main item
        constexpr size_t MaxIds = 16;    // report IDs whose input offsets are counted
        constexpr size_t MaxPush = 4;
        constexpr size_t MaxDepth = 8; // collections whose application is tracked

        struct Globals {
                        uint32_t page = 0;
                        int32_t  logicalMin = 0;
                        uint32_t logicalMax = 0; // as given
                        uint8_t  maxBytes = 0;   // its item's size
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

        int32_t signedData(uint32_t v, size_t n) {
                if (n == 1) return static_cast<int8_t>(v);
                if (n == 2) return static_cast<int16_t>(v);
                return static_cast<int32_t>(v);
        }

        // The maximum as the minimum says to read it: signed beside a negative minimum, else
        // unsigned (descriptors write 255 as 0xff in one byte).
        int32_t logicalMax(const Globals &g) {
                if (g.logicalMin < 0) return signedData(g.logicalMax, g.maxBytes);
                return g.logicalMax > 0x7fffffffu ? 0x7fffffff : static_cast<int32_t>(g.logicalMax);
        }

} // namespace

void HidDescriptor::parse(Span<const uint8_t> descriptor, Visitor visit, void *arg) {
        Globals  g;
        Globals  stack[MaxPush];
        size_t   pushed = 0;
        uint32_t usages[MaxUsages] = {};
        size_t   usageCount = 0;
        uint32_t usageMin = 0, usageMax = 0;
        bool     hasRange = false;
        uint8_t  ids[MaxIds] = {};
        uint32_t offsets[MaxIds] = {}; // input bits so far, per report ID
        size_t   idCount = 0;
        uint32_t applications[MaxDepth] = {}; // the application in force at each depth
        uint8_t  indices[MaxDepth] = {};
        int      depth = 0;
        uint8_t  opened = 0; // application collections opened so far

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
        // The innermost tracked collection (past MaxDepth, the deepest tracked one's), or -1.
        auto top = [&depth]() {
                return (depth > int(MaxDepth) ? int(MaxDepth) : depth) - 1;
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
                if (type == 1) { // global
                        switch (tag) {
                                case 0x0: g.page = value; break;
                                case 0x1: g.logicalMin = n == 0 ? 0 : signedData(value, n); break;
                                case 0x2:
                                        g.logicalMax = value;
                                        g.maxBytes = static_cast<uint8_t>(n);
                                        break;
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
                        if (tag == 0xa) { // a collection: an application one sets what follows
                                uint32_t application = top() < 0 ? 0 : applications[top()];
                                uint8_t  index = top() < 0 ? 0 : indices[top()];
                                if (value == 0x01) {
                                        application = usageCount > 0 ? usages[0] : hasRange ? usageMin : 0;
                                        index = opened++;
                                }
                                if (depth < int(MaxDepth)) {
                                        applications[depth] = application;
                                        indices[depth] = index;
                                }
                                ++depth;
                        } else if (tag == 0xc) { // its end
                                if (depth > 0) --depth;
                        } else if (tag == 0x8) {                    // an input
                                uint32_t  *offset = offsetOf(g.id); // null past MaxIds IDs: not counted
                                const bool variable = (value & Variable) != 0, constant = (value & Constant) != 0;
                                Element    e;
                                e.application = top() < 0 ? 0 : applications[top()];
                                e.collection = top() < 0 ? 0 : indices[top()];
                                e.logicalMin = g.logicalMin;
                                e.logicalMax = logicalMax(g);
                                e.size = static_cast<uint8_t>(g.size);
                                e.reportId = g.id;
                                e.flags = static_cast<uint8_t>(value & (Constant | Variable | Relative | NullState));
                                const bool visible = offset != nullptr && !constant && g.size != 0 && g.size <= 32;
                                for (uint32_t i = 0; visible && i < g.count; ++i) {
                                        const uint32_t bit = *offset + i * g.size;
                                        if (bit > 0xffff) break;
                                        if (!variable) { // an array: each element names one of the range
                                                e.usage = hasRange ? usageMin : usageCount > 0 ? usages[0] : 0;
                                                e.usageMax = hasRange     ? usageMax
                                                             : usageCount ? usages[usageCount - 1]
                                                                          : 0;
                                        } else if (i < usageCount) {
                                                e.usage = usages[i];
                                        } else if (hasRange) {
                                                e.usage = usageMin + i < usageMax ? usageMin + i : usageMax;
                                        } else {
                                                e.usage = usageCount > 0 ? usages[usageCount - 1] : 0;
                                        }
                                        if (variable) e.usageMax = e.usage;
                                        e.offset = static_cast<uint16_t>(bit);
                                        visit(arg, e);
                                }
                                if (offset != nullptr) *offset += g.size * g.count;
                        }
                        usageCount = 0; // the locals go with each main item
                        hasRange = false;
                }
                p += n;
        }
        return;
}

bool HidDescriptor::extract(Span<const uint8_t> data, uint16_t offset, uint8_t size, bool isSigned, int32_t &out) {
        if (size == 0 || size > 32) return false;
        const uint32_t first = offset / 8u;
        const uint32_t last = (uint32_t(offset) + size - 1u) / 8u;
        if (last >= data.size()) return false;
        uint64_t bits = 0;
        for (uint32_t b = last + 1; b-- > first;) bits = bits << 8 | data[b];
        bits >>= offset % 8u;
        const uint64_t mask = size >= 32 ? 0xffffffffull : (1ull << size) - 1u;
        uint32_t       v = static_cast<uint32_t>(bits & mask);
        if (isSigned && size < 32 && (v & (1u << (size - 1))) != 0) v |= ~uint32_t(mask);
        out = isSigned ? static_cast<int32_t>(v) : static_cast<int32_t>(v & 0x7fffffffu);
        return true;
}

BMDS_NAMESPACE_END
