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
#include <bmds/synthengine.h>
#include <bmds/synthlimiter.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A General MIDI synth with an OPL's voices (`devplan/audio.md` §26): an `Op2Bank`'s
 *        instruments, MIDI messages placed on frames, rendered as float samples.
 *
 * Not an emulation: only sounding voices cost anything, and there are as many as configured.
 * Each is the chip's 2-operator voice: phase accumulators, the OPL3's eight waveforms from its
 * log-sin and exponent tables (attenuations added, no multiply), FM or additive, the modulator's
 * feedback, and the chip's envelopes (its rates, key scaling, sustain level, percussive or
 * sustained), tremolo and vibrato. Envelopes are worked out every 32 frames and ramped between.
 *
 * The MIDI side is what DMX's driver added, here in the engine: programs and the drum channel
 * (10), a bank's double-voice instruments, velocity, volume and expression (TinySoundFont's curves), pan,
 * pitch bend (RPN 0 sets its range), modulation (vibrato), sustain, all notes and sounds off,
 * reset controllers and GM System On. Real-time bytes, pressure and bank select are taken and
 * ignored.
 *
 * A `SynthEngine`: its counters, frame count, events and configuration are every engine's. Its memory
 * (the voices in internal RAM, the event queue in PSRAM) is allocated by `open()`; `queue()` and
 * `render()` never allocate.
 *
 * @par Thread Safety
 * Not thread-safe: one thread at a time (its fd serializes).
 */
class FmSynth : public SynthEngine {
        public:
                static constexpr size_t TickFrames = 32;  ///< Envelopes and LFOs are worked out this often (0.67 ms).
                static constexpr size_t BlockFrames = 64; ///< A voice renders up to this many frames at once.

                FmSynth() = default;
                ~FmSynth() override;

                /**
                 * @brief Starts playing @p bank's instruments (it must outlive it, until `close()`).
                 * @return `Ok`, `Invalid` (a rate, channel count, voice count or gain; the bank not
                 *         open), `InvalidState` (open), `NoMem`.
                 */
                Error open(const Op2Bank &bank, const Config &config);

                /** @brief Stops: its memory goes. */
                void close();

                Kind kind() const override { return Kind::Fm; }
                bool isOpen() const override { return _state != nullptr; }

                Error         queue(const Event &event) override;
                void          render(float *out, size_t frames) override; ///< @hotpath
                void          reset() override;
                Stats         stats() const override;
                const Config &config() const override { return _config; }
                size_t        presets() const override { return Op2Bank::Instruments; }
                size_t        instrumentBytes() const override { return _bank != nullptr ? _bank->bytes() : 0; }

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
                SynthLimiter   _limiter;
};

BMDS_NAMESPACE_END
