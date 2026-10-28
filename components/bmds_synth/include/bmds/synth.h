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
#include <bmds/synthengine.h>
#include <bmds/synthlimiter.h>

struct tsf;

BMDS_NAMESPACE_BEGIN

/**
 * @brief The wavetable synth: a General MIDI synth on a `SoundFont` (`devplan/audio.md` §22), a
 *        `SynthEngine`.
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
class Synth : public SynthEngine {
        public:
                Synth() = default;
                ~Synth() override;

                /**
                 * @brief Starts playing @p font (which must outlive it, until `close()`).
                 * @return `Ok`, `Invalid` (a rate, channel count, voice count or gain),
                 *         `InvalidState` (open), `NoMem`.
                 */
                Error open(SoundFont &font, const Config &config);

                /** @brief Stops: its memory goes. */
                void close();

                Kind kind() const override { return Kind::Wavetable; }
                bool isOpen() const override { return _tsf != nullptr; }

                /**
                 * @brief Queues @p event, in frame order (events for one frame keep
                 *        their order).
                 * @return `Ok`, `Invalid` (no bytes, or more than `MaxEventBytes`),
                 *         `Full` (counted), `InvalidState` (not open).
                 * @nonblocking
                 */
                Error queue(const Event &event) override;

                /**
                 * @brief Makes @p frames frames into @p out (interleaved, the
                 *        configured channels, full scale ±1.0), the queued events at
                 *        their frames.
                 * @nonblocking
                 */
                void render(float *out, size_t frames) override;

                /**
                 * @brief Ends every note (a release of a few milliseconds, against a
                 *        click), resets the channels and forgets what is queued; the
                 *        frame count stays.
                 */
                void reset() override;

                Stats         stats() const override;
                const Config &config() const override { return _config; }
                size_t        presets() const override;
                size_t        instrumentBytes() const override;

        private:
                void apply(const Event &event);
                bool initChannels(); // false: no memory (only the first time)

                ::tsf     *_tsf = nullptr;
                SoundFont   *_font = nullptr; // open()'s
                SynthLimiter _limiter;
                Config   _config;
                Event   *_events = nullptr; // a ring, in frame order
                size_t   _head = 0;         // the oldest
                size_t   _count = 0;
                uint64_t _frames = 0;
                Stats    _stats;
};

BMDS_NAMESPACE_END
