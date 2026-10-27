/**
 * @file      fmsynth.h
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
#include <bmds/op2bank.h>
#include <bmds/synth.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A General MIDI synth with an OPL's voices (`devplan/audio.md` §26): an `Op2Bank`'s
 *        instruments, MIDI messages placed on frames, rendered as float samples.
 *
 * Not an emulation: only sounding voices cost anything, and there are as many as configured.
 * Each is the chip's 2-operator voice: phase accumulators, the OPL3's eight waveforms from its
 * log-sin and exponent tables (attenuations added, no multiply), FM or additive, the modulator's
 * feedback, and the chip's envelopes (its rates, key scaling, sustain level, percussive or
 * sustained), tremolo and vibrato. Envelopes are worked out every 16 frames and ramped between.
 *
 * The MIDI side is what DMX's driver added, here in the engine: programs and the drum channel
 * (10), a bank's double-voice instruments, velocity, volume and expression (GM's curve), pan,
 * pitch bend (RPN 0 sets its range), modulation (vibrato), sustain, all notes and sounds off,
 * reset controllers and GM System On. Real-time bytes, pressure and bank select are taken and
 * ignored.
 *
 * Its counters and frame count are `Synth`'s, as are its events and configuration. Its memory
 * (the voices in internal RAM, the event queue in PSRAM) is allocated by `open()`; `queue()` and
 * `render()` never allocate.
 *
 * @par Thread Safety
 * Not thread-safe: one thread at a time (its fd serializes).
 */
class FmSynth {
        public:
                using Config = Synth::Config;
                using Event = Synth::Event;
                using Stats = Synth::Stats;

                static constexpr size_t TickFrames = 16;  ///< Envelopes and LFOs are worked out this often.
                static constexpr size_t BlockFrames = 64; ///< A voice renders up to this many frames at once.

                FmSynth() = default;
                ~FmSynth();
                FmSynth(const FmSynth &) = delete;
                FmSynth &operator=(const FmSynth &) = delete;

                /**
                 * @brief Starts playing @p bank's instruments (it must outlive it, until `close()`).
                 * @return `Ok`, `Invalid` (a rate, channel count, voice count or gain; the bank not
                 *         open), `InvalidState` (open), `NoMem`.
                 */
                Error open(const Op2Bank &bank, const Config &config);

                /** @brief Stops: its memory goes. */
                void close();

                /** @brief Returns true while open. */
                bool isOpen() const { return _state != nullptr; }

                /**
                 * @brief Queues @p event, in frame order (events for one frame keep their order).
                 * @return `Ok`, `Invalid` (no bytes, or more than `Synth::MaxEventBytes`), `Full`
                 *         (counted), `InvalidState` (not open).
                 * @nonblocking
                 */
                Error queue(const Event &event);

                /**
                 * @brief Makes @p frames frames into @p out (interleaved, the configured channels,
                 *        full scale ±1.0), the queued events at their frames.
                 * @nonblocking
                 * @hotpath
                 */
                void render(float *out, size_t frames);

                /**
                 * @brief Ends every note (a release of a few milliseconds, against a click), resets
                 *        the channels and forgets what is queued; the frame count stays.
                 */
                void reset();

                /** @brief Returns the counters. */
                Stats stats() const;

                /** @brief Returns the configuration (with the defaults filled in). */
                const Config &config() const { return _config; }

                struct State; ///< Its voices and channels (opaque).

        private:
                void apply(const Event &event, uint64_t frame);

                State         *_state = nullptr; // internal RAM: the voices, the channels, the mix
                const Op2Bank *_bank = nullptr;
                Config         _config;
                Event         *_events = nullptr; // a ring in PSRAM, in frame order
                size_t         _head = 0;
                size_t         _count = 0;
                uint64_t       _frames = 0;
                Stats          _stats;
};

BMDS_NAMESPACE_END
