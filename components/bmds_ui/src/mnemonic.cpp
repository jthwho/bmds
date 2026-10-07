/**
 * @file      mnemonic.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Keys marked in texts with & (devplan/gui.md §4r).
 */

#include <bmds/mnemonic.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace Mnemonic {

        Stripped strip(StringView text, Span<char> out) {
                Stripped result;
                if (out.size() == 0) return result;
                size_t n = 0;
                bool   marked = false;
                for (size_t i = 0; i < text.size();) {
                        if (text[i] == '&') {
                                ++i;
                                if (i >= text.size()) break; // a & at the end
                                if (text[i] != '&') marked = true;
                        }
                        // A whole character, or nothing more.
                        size_t         next = i;
                        const uint32_t cp = Utf8::decode(text, next);
                        if (n + (next - i) + 1 > out.size()) break;
                        if (marked && result.at == None) {
                                result.at = n;
                                result.key = fold(cp);
                        }
                        marked = false;
                        for (; i < next; ++i) out[n++] = text[i];
                }
                out[n] = '\0';
                result.length = n;
                return result;
        }

        uint32_t keyOf(const KeyEvent &event) {
                if (event.action == KeyEvent::Action::Release || event.hasCtrl() ||
                    (event.modifiers & KeyEvent::Gui) != 0)
                        return 0;
                if (event.codepoint > 0x20 && event.codepoint != 0x7f) return fold(event.codepoint);
                if (!event.hasAlt()) return 0;
                if (event.usage >= HidKey::A && event.usage <= HidKey::Z) return 'a' + (event.usage - HidKey::A);
                if (event.usage >= HidKey::Digit1 && event.usage < HidKey::Digit0)
                        return '1' + (event.usage - HidKey::Digit1);
                if (event.usage == HidKey::Digit0) return '0';
                return 0;
        }

} // namespace Mnemonic

BMDS_NAMESPACE_END
