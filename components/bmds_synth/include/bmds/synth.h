/**
 * @file      synth.h
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
#include <bmds/soundfont.h>

struct tsf;

BMDS_NAMESPACE_BEGIN

/**
 * @brief A General MIDI synth on a `SoundFont` (`devplan/audio.md` §22):
 *        MIDI messages placed on frames, rendered as float samples.
 *
 * The frame count is its own: the frames `render()` has made. An event for a
 * frame already made plays at the next render's start; one for a later frame
 * waits until then, so a player writes ahead and every event lands on its
 * sample. Channel 10 (9 here) is the drums.
 *
 * Takes: note off and on, key pressure (ignored), control changes (bank,
 * volume, pan, expression, sustain, RPN pitch-bend range, all notes and
 * sounds off, reset), program change, pitch bend, and the GM System On and
 * GS reset SysExes (whole, in one event). Real-time bytes are ignored.
 *
 * Its memory (the voices, the event queue, the channels) is allocated by
 * `open()`; `queue()` and `render()` never allocate.
 *
 * @par Thread Safety
 * Not thread-safe: one thread at a time (its fd serializes). `open()` and
 * `close()` of synths on one font must also be serialized with each other:
 * TinySoundFont counts a font's users without atomics.
 */
class Synth {
        public:
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
                };

                Synth() = default;
                ~Synth();
                Synth(const Synth &) = delete;
                Synth &operator=(const Synth &) = delete;

                /**
                 * @brief Starts playing @p font (which must outlive it, until `close()`).
                 * @return `Ok`, `Invalid` (a rate, channel count, voice count or gain),
                 *         `InvalidState` (open), `NoMem`.
                 */
                Error open(SoundFont &font, const Config &config);

                /** @brief Stops: its memory goes. */
                void close();

                /** @brief Returns true while open. */
                bool isOpen() const { return _tsf != nullptr; }

                /**
                 * @brief Queues @p event, in frame order (events for one frame keep
                 *        their order).
                 * @return `Ok`, `Invalid` (no bytes, or more than `MaxEventBytes`),
                 *         `Full` (counted), `InvalidState` (not open).
                 * @nonblocking
                 */
                Error queue(const Event &event);

                /**
                 * @brief Makes @p frames frames into @p out (interleaved, the
                 *        configured channels, full scale ±1.0), the queued events at
                 *        their frames.
                 * @nonblocking
                 */
                void render(float *out, size_t frames);

                /**
                 * @brief Ends every note (a release of a few milliseconds, against a
                 *        click), resets the channels and forgets what is queued; the
                 *        frame count stays.
                 */
                void reset();

                /** @brief Returns the counters. */
                Stats stats() const;

                /** @brief Returns the configuration (with the defaults filled in). */
                const Config &config() const { return _config; }

        private:
                void apply(const Event &event);
                bool initChannels(); // false: no memory (only the first time)

                ::tsf   *_tsf = nullptr;
                Config   _config;
                Event   *_events = nullptr; // a ring, in frame order
                size_t   _head = 0;         // the oldest
                size_t   _count = 0;
                uint64_t _frames = 0;
                Stats    _stats;
};

BMDS_NAMESPACE_END
