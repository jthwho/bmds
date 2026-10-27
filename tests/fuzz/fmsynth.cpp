/**
 * @file      fmsynth.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the FM synth (devplan/audio.md §26): an OP2 bank (the input's first
 * 11,908 bytes) and a MIDI stream played through it (the rest: a frame step, a length and the
 * message's bytes, again and again), on a configuration the stream's first byte picks; nothing
 * may fault, read out of bounds, overflow or leak, and every sample must be finite.
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <bmds/fmsynth.h>
#include <bmds/op2bank.h>
#include "hostmemory.h"

using bmds::ByteSpan;
using bmds::Error;
using bmds::FmSynth;
using bmds::Op2Bank;

namespace {

        void play(FmSynth &synth, size_t frames) {
                static float out[2 * 512];
                synth.render(out, frames);
                for (size_t i = 0; i < frames * synth.config().channels; ++i)
                        if (!std::isfinite(out[i])) abort();
                return;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        const size_t before = HostMemory::liveAllocations();
        const size_t bankBytes = size < Op2Bank::FullBytes ? size : Op2Bank::FullBytes;
        Op2Bank      bank;
        if (bank.open(ByteSpan(data, bankBytes)).isError()) return 0;
        for (size_t i = 0; i < Op2Bank::Instruments; ++i) (void)bank.name(i).size();
        const uint8_t *p = data + bankBytes;
        const uint8_t *end = data + size;
        {
                FmSynth         synth;
                FmSynth::Config c;
                const uint8_t   pick = p < end ? *p++ : 0;
                c.channels = (pick & 1) != 0 ? 1 : 2;
                c.maxVoices = uint16_t(1 + (pick >> 1) % 48);
                c.rateHz = (pick & 0x80) != 0 ? 22050 : 48000;
                if (synth.open(bank, c).isError()) abort();
                uint64_t frame = 0;
                while (end - p >= 2) {
                        frame += uint64_t(*p++) * 4;
                        FmSynth::Event e;
                        e.frame = frame;
                        e.length = uint8_t(1 + *p++ % bmds::Synth::MaxEventBytes);
                        for (uint8_t k = 0; k < e.length; ++k) e.data[k] = p < end ? *p++ : 0;
                        if (synth.queue(e) == Error::Full) {
                                play(synth, 512);
                                (void)synth.queue(e);
                        }
                        if (frame > synth.stats().frames + 4096) play(synth, 512);
                }
                for (int i = 0; i < 16; ++i) play(synth, 512);
                synth.reset();
                play(synth, 64);
        }
        if (HostMemory::liveAllocations() != before) abort();
        return 0;
}
