/**
 * @file      dragdrop.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Drag and drop's pure parts (devplan/clipboard.md §5f).
 */

#include <initializer_list>
#include <bmds/dragdrop.h>

BMDS_NAMESPACE_BEGIN

namespace DropActions {

        DropAction choose(uint8_t offered, bool ctrl, bool shift, DropAction targetDefault, DropAction preferred) {
                if (ctrl || shift) {
                        const DropAction asked = ctrl && shift ? DropAction::Link
                                                 : ctrl        ? DropAction::Copy
                                                               : DropAction::Move;
                        return allows(offered, asked) ? asked : DropAction::None;
                }
                if (allows(offered, targetDefault)) return targetDefault;
                if (allows(offered, preferred)) return preferred;
                for (const DropAction a : {DropAction::Copy, DropAction::Move, DropAction::Link})
                        if (allows(offered, a)) return a;
                return DropAction::None;
        }

} // namespace DropActions

namespace ShellQuote {

        bool isPlain(StringView word) {
                if (word.isEmpty()) return false;
                for (const char c : word) {
                        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                        StringView("._/+-:@%,=").contains(c);
                        if (!ok) return false;
                }
                return true;
        }

        Error append(String &out, StringView word) {
                if (isPlain(word)) return out.append(word);
                Error err = out.append('\'');
                for (size_t i = 0; i < word.size() && err.isOk(); ++i)
                        err = word[i] == '\'' ? out.append(StringView("'\\''")) : out.append(word[i]);
                if (err.isOk()) err = out.append('\'');
                return err;
        }

} // namespace ShellQuote

namespace DragImage {

        namespace {

                // Where a side of @p size starts to hold [lo, hi) (as much as fits from lo), centred
                // on [itemLo, itemHi), and @p press inside.
                int place(int itemLo, int itemHi, int press, int size) {
                        int start = (itemLo + itemHi) / 2 - size / 2;
                        if (itemHi - itemLo <= size) {
                                if (start > itemLo) start = itemLo;
                                if (start + size < itemHi) start = itemHi - size;
                        }
                        if (press < start) start = press;
                        if (press >= start + size) start = press - size + 1;
                        return start;
                }

        } // namespace

        Box around(const Box &item, int pressX, int pressY) {
                Box b;
                b.width = Max;
                b.height = Max;
                b.x = place(item.x, item.x + item.width, pressX, Max);
                b.y = place(item.y, item.y + item.height, pressY, Max);
                return b;
        }

        bool holds(const Box &box, const Box &item) {
                return item.width > 0 && item.height > 0 && item.x >= box.x && item.y >= box.y &&
                       item.x + item.width <= box.x + box.width && item.y + item.height <= box.y + box.height;
        }

} // namespace DragImage

BMDS_NAMESPACE_END
