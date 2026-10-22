/**
 * @file      windowplacement.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/windowplacement.h>

BMDS_NAMESPACE_BEGIN

bool WindowPlacement::parse(StringView text, WindowPlacement &out) {
        uint32_t values[4] = {};
        size_t   n = 0;
        text = text.trimmed();
        while (n < 4 && !text.isEmpty()) {
                size_t   i = 0;
                uint32_t v = 0;
                while (i < text.size() && text[i] >= '0' && text[i] <= '9' && v <= 0xffff)
                        v = v * 10 + uint32_t(text[i++] - '0');
                if (i == 0 || v > 0xffff) return false;
                values[n++] = v;
                text = text.mid(i).trimmed();
        }
        if (n != 4 || values[2] == 0 || values[3] == 0) return false;
        if (!text.isEmpty() && text != "maximized") return false;
        out.normal = Rect{uint16_t(values[0]), uint16_t(values[1]), uint16_t(values[2]), uint16_t(values[3])};
        out.maximized = !text.isEmpty();
        return true;
}

String WindowPlacement::format() const {
        String text(MemSpace::Psram);
        (void)text.appendf("%u %u %u %u%s", unsigned(normal.x), unsigned(normal.y), unsigned(normal.width),
                           unsigned(normal.height), maximized ? " maximized" : "");
        return text;
}

BMDS_NAMESPACE_END
