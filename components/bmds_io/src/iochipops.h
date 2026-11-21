/**
 * @file      iochipops.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The chip's pad registers (devplan/io.md §3.1): each chip implements these in
 * chips/<target>/iochipops.cpp, and nothing else in BMDS touches a pad's
 * registers for the I/O view. Target only.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/iopad.h>
#include <bmds/iosetup.h>

BMDS_NAMESPACE_BEGIN

namespace IoChipOps {

        /**
         * @brief Reads pad @p pad's configuration and level.
         * @nonblocking
         */
        void readPad(unsigned pad, IoPadState &out);

        /**
         * @brief Where input signal @p signal comes from through the GPIO matrix:
         *        a pad, `IoChip::constZeroInput` or `constOneInput`; or -1 when the
         *        matrix doesn't route it (it comes through IO_MUX, or isn't used).
         * @nonblocking
         */
        int inputSource(unsigned signal);

        /**
         * @brief A pad's registers as they were, to put back (devplan/io.md §10.1): its IO_MUX
         *        word, its matrix output, its pin word, its enable and latch bits, and the
         *        domain bits beside it (the LP IO mux; a USB PHY's pad enable on 24-27).
         */
        struct Saved {
                        uint32_t ioMux = 0;
                        uint32_t outSel = 0;
                        uint32_t pin = 0;
                        bool     enabled = false;
                        bool     latch = false;
                        bool     lpIo = false;
                        int8_t   usbPad = -1; // a USB PHY's pad enable bit, or -1 (not a USB pad)
        };

        /** @brief Saves pad @p pad's registers. @nonblocking */
        void savePad(unsigned pad, Saved &out);

        /** @brief Writes pad @p pad's registers back as @p saved has them. @nonblocking */
        void restorePad(unsigned pad, const Saved &saved);

        /**
         * @brief Sets pad @p pad up as @p setup says (its input signals apart:
         *        `routeInput()`): the function, the level, the direction, then the matrix
         *        output (`esp32p4.md`: a direction set after the signal takes the pad back).
         * @nonblocking
         */
        void configurePad(unsigned pad, const IoPinSetup &setup);

        /** @brief Sets an output's latch. @nonblocking */
        void setLevel(unsigned pad, bool high);

        /** @brief Input signal @p signal's selector word (to put back with `setInputWord()`). @nonblocking */
        uint32_t inputWord(unsigned signal);

        /** @brief Writes input signal @p signal's selector word. @nonblocking */
        void setInputWord(unsigned signal, uint32_t word);

        /** @brief Routes input signal @p signal from pad @p pad through the matrix. @nonblocking */
        void routeInput(unsigned signal, unsigned pad);

} // namespace IoChipOps

BMDS_NAMESPACE_END
