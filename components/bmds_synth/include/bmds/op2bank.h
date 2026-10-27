/**
 * @file      op2bank.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An OPL instrument bank in DMX's OP2 format (`devplan/audio.md` §26), the FM synth's
 *        General MIDI instruments: Doom's `GENMIDI` lump, DMXOPL3, Freedoom's.
 *
 * The file: the magic `#OPL_II#`, 175 instruments of 36 bytes (the 128 GM programs, then the
 * drums for keys 35 to 81), then their names (32 bytes each; a bank cut before them has none).
 * Read in place: the bytes are the caller's (a file mapped from `/rom`, or a copy), and must
 * outlive it. Every value is masked to its register's bits, so any bytes past the header are
 * an instrument (a bank is untrusted input).
 *
 * @par Thread Safety
 * Immutable once open: any thread.
 */
class Op2Bank {
        public:
                static constexpr size_t  Instruments = 175; ///< The programs and the drums.
                static constexpr size_t  Programs = 128;    ///< GM's programs, first.
                static constexpr uint8_t FirstDrumKey = 35; ///< The first drum's key (Acoustic Bass Drum).
                static constexpr uint8_t LastDrumKey = 81;  ///< The last's (Open Triangle).
                static constexpr size_t  InstrumentBytes = 36;
                static constexpr size_t  NameBytes = 32;
                static constexpr size_t  HeaderBytes = 8;
                static constexpr size_t  MinBytes = HeaderBytes + Instruments * InstrumentBytes;
                static constexpr size_t  FullBytes = MinBytes + Instruments * NameBytes;

                /** @brief An instrument's flags. */
                enum Flag : uint16_t {
                        FixedPitch = 0x0001,     ///< Plays `fixedNote` whatever the key (the drums).
                        DelayedVibrato = 0x0002, ///< DMX's, unused (as by its driver).
                        DoubleVoice = 0x0004,    ///< Both voices sound, the second detuned by `fineTune`.
                };

                /** @brief An operator's registers, as the chip has them. */
                struct Operator {
                                uint8_t character = 0;      ///< 0x20: tremolo, vibrato, sustaining, KSR, multiple.
                                uint8_t attackDecay = 0;    ///< 0x60: attack rate (high), decay rate (low).
                                uint8_t sustainRelease = 0; ///< 0x80: sustain level (high), release rate (low).
                                uint8_t waveform = 0;       ///< 0xE0: 0 to 7 (OPL3).
                                uint8_t keyScale = 0;       ///< 0x40's high bits: key scale level, 0 to 3.
                                uint8_t level = 0;          ///< 0x40's low bits: total level, 0 (loud) to 63.
                };

                /** @brief A 2-operator voice. */
                struct Voice {
                                Operator modulator;
                                Operator carrier;
                                uint8_t  feedback = 0;     ///< 0 to 7: the modulator's on itself.
                                bool     additive = false; ///< The connection: both heard, no FM.
                                int16_t  noteOffset = 0;   ///< Semitones added to the key.
                };

                /** @brief An instrument: one voice, or two. */
                struct Instrument {
                                uint16_t flags = 0;      ///< `Flag` bits.
                                uint8_t  fineTune = 128; ///< The second voice's tuning: 128 none, 1/64 semitone steps.
                                uint8_t  fixedNote = 0;  ///< The key played with `FixedPitch`.
                                Voice    voices[2];
                };

                /**
                 * @brief Reads @p data's header (kept, not copied).
                 * @return `Ok`, or `Invalid` (not an OP2 bank, or cut short).
                 */
                Error open(ByteSpan data);

                /** @brief Returns true once open. */
                bool isOpen() const { return _data != nullptr; }

                /**
                 * @brief Returns instrument @p index (a program, or `Programs` + a drum); an empty
                 *        one past the end.
                 */
                Instrument instrument(size_t index) const;

                /** @brief Returns the drum instrument's index for @p key, or `Instruments` (none). */
                static size_t drumIndex(uint8_t key) {
                        return key >= FirstDrumKey && key <= LastDrumKey ? Programs + (key - FirstDrumKey)
                                                                         : Instruments;
                }

                /** @brief Returns instrument @p index's name ("" if the bank has none). */
                StringView name(size_t index) const;

                /** @brief Returns the bank's bytes. */
                size_t bytes() const { return _size; }

        private:
                const uint8_t *_data = nullptr;
                size_t         _size = 0;
};

BMDS_NAMESPACE_END
