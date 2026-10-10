/**
 * @file      panelitems.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Edits of panel/items (devplan/sysmon.md §3.5, step 5).
 */

#include <cstring>
#include <bmds/inireader.h>

#include "panelitems.h"

BMDS_NAMESPACE_BEGIN

bool PanelItems::has(StringView list, StringView kind) {
        for (StringView name = IniReader::nextListItem(list); !name.isEmpty(); name = IniReader::nextListItem(list))
                if (name.equalsIgnoreCase(kind)) return true;
        return false;
}

size_t PanelItems::withItem(StringView list, StringView kind, bool shown, Span<char> out) {
        const bool present = has(list, kind);
        // Where it goes in: after the last tray, else at the end.
        size_t     trays = 0;
        StringView scan = list;
        for (StringView name = IniReader::nextListItem(scan); !name.isEmpty(); name = IniReader::nextListItem(scan))
                trays += name.equalsIgnoreCase("tray") || name.equalsIgnoreCase("volume") ? 1 : 0;
        const bool insert = shown && !present;
        size_t     n = 0;
        size_t     seen = 0;
        bool       inserted = false;
        auto       put = [&](StringView item) {
                const size_t need = (n > 0 ? 1 : 0) + item.size();
                if (n + need + 1 > out.size()) return false;
                if (n > 0) out[n++] = ';';
                std::memcpy(out.data() + n, item.data(), item.size());
                n += item.size();
                return true;
        };
        for (StringView name = IniReader::nextListItem(list); !name.isEmpty(); name = IniReader::nextListItem(list)) {
                if (!shown && name.equalsIgnoreCase(kind)) continue;
                if (!put(name)) return 0;
                if (insert && (name.equalsIgnoreCase("tray") || name.equalsIgnoreCase("volume")) && ++seen == trays) {
                        if (!put(kind)) return 0;
                        inserted = true;
                }
        }
        if (insert && !inserted && !put(kind)) return 0;
        if (out.size() == 0) return 0;
        out[n] = '\0';
        return n;
}

BMDS_NAMESPACE_END
