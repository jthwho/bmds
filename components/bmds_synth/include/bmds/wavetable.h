/**
 * @file      wavetable.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The built-in synth as a MIDI port that is always there (`wavetable`,
 *        `devplan/audio.md` §25): the system's default synth, as a sound card's
 *        FM chip was. `midi/connect`'s default takes `system` (what programs
 *        play) to it.
 *
 * While `synth/wavetable` is on, the port (kind `system`, a destination)
 * exists. Idle, it costs the port, a ring of 256 events (PSRAM) and a control
 * thread (`wavetable`, its stack in PSRAM) waiting. The first event opens a
 * synth on `synth/font` and a real-time stream on `/dev/dsp` (128 frames), and
 * starts the audio thread; 5 s with no events and no voice sounding, both are
 * closed. While `synth/keep-fonts` is on, the font is loaded once at the start
 * (and when `synth/font` changes) and left in the font cache, so the first note
 * doesn't wait for the card.
 *
 * @par Thread Safety
 * `init()` once, at boot; `status()` from any task.
 */
namespace Wavetable {

        /** @brief The service's state. */
        struct Status {
                        bool     on = false;      ///< `synth/wavetable`: the port exists.
                        bool     playing = false; ///< The synth and its stream are open.
                        uint8_t  port = 0xff;     ///< Its MIDI port (`MidiRouter::NoPort`: none).
                        uint32_t events = 0;      ///< Events played, since boot.
                        uint32_t dropped = 0;     ///< Events dropped by a full ring.
                        uint32_t starts = 0;      ///< Times it started playing.
                        uint32_t late = 0;        ///< Its stream's late periods, all told.
                        uint32_t peakVoices = 0;  ///< The most at once, since boot.
        };

        /**
         * @brief Boot stage: follows `synth/wavetable`, its port and thread started
         *        if on. After MIDI, the audio device and the fonts.
         * @return `Ok`, or the error starting the thread.
         */
        Error init();

        /** @brief Returns its state. */
        Status status();

} // namespace Wavetable

BMDS_NAMESPACE_END
