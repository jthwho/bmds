/**
 * @file      terminalkeys.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/inputevent.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Keys as the bytes a terminal sends (xterm's, `devplan/input.md` §4).
 *
 * Text as UTF-8; Ctrl+letter as 0x01–0x1a (and xterm's Ctrl+2 to Ctrl+8);
 * Alt as an Escape before the key; Enter `\r`, Backspace 0x7f, Tab, Escape;
 * the cursor, editing and function keys as `ESC [`/`ESC O` sequences, with
 * xterm's `;m` modifier parameter when Shift, Alt or Ctrl is down.
 *
 * @par Thread Safety
 * Stateless.
 */
namespace TerminalKeys {

        /** @brief The longest encoding (`ESC [ 2 4 ; 8 ~` is 7 bytes). */
        constexpr size_t MaxBytes = 16;

        /**
         * @brief Encodes @p event into @p out.
         * @param applicationCursor The program asked for application cursor
         *        keys (DECCKM): unmodified arrows, Home and End as `ESC O x`.
         * @return The bytes written; 0 for a release or a key that sends nothing
         *         (a modifier, a lock key).
         */
        size_t encode(const KeyEvent &event, bool applicationCursor, char (&out)[MaxBytes]);

} // namespace TerminalKeys

BMDS_NAMESPACE_END
