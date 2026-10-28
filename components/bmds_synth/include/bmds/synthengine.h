/**
 * @file      synthengine.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a General MIDI synth does, whatever makes its sound (`devplan/audio.md` §26):
 *        MIDI messages placed on frames, rendered as float samples, and its counters. The
 *        wavetable synth (`Synth`, a SoundFont) and the FM synth (`FmSynth`, an OP2 bank) are
 *        two; synth fds, the system's synth services and MIDI files play either.
 *
 * The frame count is its own: the frames `render()` has made. An event for a frame already
 * made plays at the next render's start; one for a later frame waits until then, so a player
 * writes ahead and every event lands on its sample. Channel 10 (9 here) is the drums. The output
 * passes a limiter (`SynthLimiter`), so a peak never clips.
 *
 * @par Thread Safety
 * As each engine: one thread at a time.
 */
class SynthEngine {
        public:
                /** @brief The engines (`bmds_synth_config_t.engine`'s values). */
                enum class Kind : uint8_t {
                        Default = 0,   ///< Asked for: the system synth's (a request, never an engine's).
                        Wavetable = 1, ///< A SoundFont's samples.
                        Fm = 2,        ///< An OPL's voices on an OP2 bank.
                };

                static constexpr uint16_t DefaultVoices = 32; ///< Voices when `Config::maxVoices` is 0.
                static constexpr uint16_t MaxVoices = 256;    ///< The most `Config::maxVoices` takes.
                static constexpr size_t   QueueEvents = 1024; ///< Events waiting at most.
                static constexpr size_t   MaxEventBytes = 6;  ///< A message's bytes (GM System On).
                static constexpr float    MinGainDb = -40;    ///< `Config::gainDb`'s range.
                static constexpr float    MaxGainDb = 12;

                /** @brief How it renders. */
                struct Config {
                                uint32_t rateHz = 48000;
                                uint8_t  channels = 2;  ///< 1 or 2 (interleaved).
                                uint16_t maxVoices = 0; ///< 0: `DefaultVoices`.
                                float    gainDb =
                                        0; ///< The output's gain, `MinGainDb` to `MaxGainDb` (headroom: below 0).
                };

                /** @brief A message for a frame (the ABI's `bmds_synth_event_t`). */
                struct Event {
                                uint64_t frame = 0;
                                uint8_t  length = 0;
                                uint8_t  flags = 0;
                                uint8_t  data[MaxEventBytes] = {};
                };

                /** @brief Its counters. */
                struct Stats {
                                uint64_t frames = 0;       ///< Frames made.
                                uint32_t voices = 0;       ///< Playing now.
                                uint32_t maxVoices = 0;    ///< Its limit.
                                uint32_t peakVoices = 0;   ///< The most at once.
                                uint32_t notesAtLimit = 0; ///< Note-ons with every voice busy (one stolen or dropped).
                                uint32_t queued = 0;       ///< Events waiting.
                                uint32_t queueFull = 0;    ///< Events refused (the queue was full).
                                uint32_t ignored = 0;      ///< Messages it doesn't take.
                                uint32_t limited = 0;      ///< Frames its output limiter turned down (`SynthLimiter`).
                };

                virtual ~SynthEngine() = default;

                /** @brief Returns which it is. */
                virtual Kind kind() const = 0;

                /** @brief Returns true while open. */
                virtual bool isOpen() const = 0;

                /**
                 * @brief Queues @p event, in frame order (events for one frame keep their order).
                 * @return `Ok`, `Invalid` (no bytes, or more than `MaxEventBytes`), `Full`
                 *         (counted), `InvalidState` (not open).
                 * @nonblocking
                 */
                virtual Error queue(const Event &event) = 0;

                /**
                 * @brief Makes @p frames frames into @p out (interleaved, the configured channels,
                 *        full scale ±1.0), the queued events at their frames.
                 * @nonblocking
                 */
                virtual void render(float *out, size_t frames) = 0;

                /**
                 * @brief Ends every note (a release of a few milliseconds, against a click), resets
                 *        the channels and forgets what is queued; the frame count stays.
                 */
                virtual void reset() = 0;

                /** @brief Returns the counters. */
                virtual Stats stats() const = 0;

                /** @brief Returns the configuration (with the defaults filled in). */
                virtual const Config &config() const = 0;

                /** @brief Returns its instruments: a font's presets, a bank's instruments. */
                virtual size_t presets() const = 0;

                /** @brief Returns the memory its instruments hold (shared by every synth on them). */
                virtual size_t instrumentBytes() const = 0;

        protected:
                SynthEngine() = default;
                SynthEngine(const SynthEngine &) = delete;
                SynthEngine &operator=(const SynthEngine &) = delete;
};

BMDS_NAMESPACE_END
