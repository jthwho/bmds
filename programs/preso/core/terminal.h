/**
 * @file      terminal.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Keys typed on the terminal Preso was started from (devplan/preso.md §6b): over SSH, the
 * laptop's keyboard moves the deck. The bytes a terminal sends (letters, Enter, the escape
 * sequences of arrows, Home, End, PageUp and PageDown) become the keys they are, as HID usages
 * with the characters they type, so they are mapped as the screen's keys are.
 */

#ifndef PRESO_TERMINAL_H
#define PRESO_TERMINAL_H

#include "platform.h"

namespace preso {

        /**
         * @brief A terminal's bytes to keys, one byte at a time (a sequence may come in pieces).
         *
         * @par Thread Safety
         * Not thread-safe: one reader.
         */
        class TerminalKeys {
                public:
                        /**
                         * @brief Takes byte @p c. @return True with @p key (an Event::Type::Key) when a
                         *        key is complete.
                         */
                        bool push(char c, Event *key);

                private:
                        char     _sequence[8] = {}; ///< An escape sequence so far (from its ESC).
                        uint32_t _sequenceLength = 0;
                        uint32_t _utf8 = 0;     ///< A character's bits so far.
                        uint32_t _utf8Left = 0; ///< Its bytes still to come.
        };

        /**
         * @brief What key @p key typed on the terminal does in @p mode: as defaultAction(), and the
         *        letters (n, p, j, k, g, G, b, w, o, i, c, q); Escape alone does nothing outside a
         *        script region, so a stray key can't end the talk. Action::None: a plain key.
         */
        Action terminalAction(const Event &key, InputMode mode);

} // namespace preso

#endif // PRESO_TERMINAL_H
