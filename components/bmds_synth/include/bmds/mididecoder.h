/**
 * @file      mididecoder.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/decoder.h>
#include <bmds/result.h>
#include <bmds/synthengine.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Standard MIDI Files as a decoder (`devplan/audio.md` §22, §26): played by the system
 *        synth's engine, so `play`, the file browser and `audio_decode` take them as they take
 *        sound files. Registered with `Decoder::addFormat()` ("MThd") at boot.
 *
 * Portable: the synth comes from `setSynths()`'s calls (the kernel's, on its settings, on the
 * board; a test engine on the host).
 *
 * Float, stereo, 48 kHz. Its length is the file's and two seconds for the
 * last notes' release. Seeking replays the file's controller and program
 * changes up to the frame, without its notes.
 */
namespace MidiDecoder {

        /**
         * @brief Something a MIDI file did as it played, on the frame it did it, for a
         *        view of the channels (`devplan/mediaplayer.md` §6h).
         */
        struct Activity {
                        /** @brief What it was. */
                        enum class Kind : uint8_t {
                                NoteOn,        ///< `a` the key, `b` the velocity (0: an off).
                                NoteOff,       ///< `a` the key.
                                Program,       ///< `a` the program.
                                Control,       ///< `a` the controller, `b` the value.
                                Tempo,         ///< `value` microseconds a quarter note.
                                TimeSignature, ///< `a` the beats a bar, `b` the beat's note (4: a quarter).
                                Reset,         ///< Every channel as at the start (a snapshot follows).
                        };

                        uint64_t frame = 0;
                        uint32_t value = 0;
                        Kind     kind = Kind::Reset;
                        uint8_t  channel = 0; ///< 0 to 15.
                        uint8_t  a = 0;
                        uint8_t  b = 0;
        };

        /** @brief Opens the synth to play files with, on @p config (its engine and instruments the opener's). */
        using OpenSynth = Result<SynthEngine *> (*)(const SynthEngine::Config &config);

        /** @brief Closes what `OpenSynth` opened. */
        using CloseSynth = void (*)(SynthEngine *synth);

        /** @brief Sets where the synths come from (before the format is added). */
        void setSynths(OpenSynth open, CloseSynth close);

        /** @brief `Decoder::Opener` for MIDI files (`NotSupported` before `setFonts()`). */
        Result<Decoder *> open(const Decoder::Source &source);

} // namespace MidiDecoder

BMDS_NAMESPACE_END
