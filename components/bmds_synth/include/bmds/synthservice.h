/**
 * @file      synthservice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/synthengine.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The built-in synths as MIDI ports that are always there (`devplan/audio.md` §25, §26):
 *        `fm` (the FM engine on `synth/fm-bank`) and `wavetable` (the wavetable on `synth/font`),
 *        one service run twice. `synth/system` names the port `system` (what programs play) is
 *        routed to: `fm` by default, as a sound card's FM chip was.
 *
 * While its setting is on (`synth/fm`, `synth/wavetable`), a service's port (kind `system`, a
 * destination) exists. Idle, it costs the port, a ring of 256 events (PSRAM) and a control
 * thread (its stack in PSRAM) waiting. The first event opens a synth on its engine and a
 * real-time stream on `/dev/dsp` (the device's period), and starts the audio thread; 5 s with no
 * events and no voice sounding, both are closed. While the wavetable is the system synth and
 * `synth/keep-fonts` is on, its font is loaded at the start (and when `synth/font` changes) and
 * left in the font cache, so the first note doesn't wait for the card; otherwise the first note
 * loads it, and a notification says so.
 *
 * @par Thread Safety
 * `init()` once, at boot; `status()` from any task.
 */
namespace SynthServices {

        /** @brief A service's state. */
        struct Status {
                        bool     on = false;      ///< Its setting: the port exists.
                        bool     playing = false; ///< The synth and its stream are open.
                        bool     system = false;  ///< `synth/system` names it.
                        uint8_t  port = 0xff;     ///< Its MIDI port (`MidiRouter::NoPort`: none).
                        uint32_t events = 0;      ///< Events played, since boot.
                        uint32_t dropped = 0;     ///< Events dropped by a full ring.
                        uint32_t starts = 0;      ///< Times it started playing.
                        uint32_t late = 0;        ///< Its stream's late periods, all told.
                        uint32_t peakVoices = 0;  ///< The most at once, since boot.
        };

        /**
         * @brief Boot stage: both services, following their settings, their ports and threads
         *        started if on. After MIDI, the audio device and the fonts.
         * @return `Ok`, or the error starting a thread.
         */
        Error init();

        /** @brief Returns the service of engine @p kind's state (`Default`: nothing). */
        Status status(SynthEngine::Kind kind);

        /** @brief Returns its port's name: "fm", "wavetable". */
        const char *portName(SynthEngine::Kind kind);

} // namespace SynthServices

BMDS_NAMESPACE_END
