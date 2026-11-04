/**
 * @file      notes.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The presenter's notes (devplan/preso.md §6, §6b), written to standard output: over SSH they
 * land in the laptop's terminal while the slides show on the board's screen. For each slide and
 * step shown: a rule with its number, title and the timer, its notes, and the next slide's
 * title and first lines.
 */

#ifndef PRESO_NOTES_H
#define PRESO_NOTES_H

#include "buffer.h"
#include "deck.h"

namespace preso {

        /** @brief The timer: the talk's time and the slide's (nanoseconds; talk < 0: not started). */
        struct NotesTime {
                        int64_t talk = -1;
                        int64_t slide = 0;
        };

        /**
         * @brief The rule that heads the notes: `── 3/12  Title ───── 0:04:12  slide 0:42  1:10
         *        behind`, @p width columns wide (the timer at its right; with the deck's
         *        `duration`, the pace).
         */
        void writeRule(Buffer &out, const Deck &deck, uint32_t slide, const NotesTime &time, uint32_t width);

        /**
         * @brief The notes for slide @p slide at step @p step of @p steps: the rule, the step, the
         *        slide's notes, then the next slide's number, title and first lines.
         */
        void writeNotes(Buffer &out, const Deck &deck, uint32_t slide, uint32_t step, uint32_t steps,
                        const NotesTime &time, uint32_t width);

        /** @brief How the talk stands against `duration` at @p slide: seconds behind (negative: ahead). */
        int64_t paceSeconds(const Deck &deck, uint32_t slide, int64_t talkNs);

} // namespace preso

#endif // PRESO_NOTES_H
