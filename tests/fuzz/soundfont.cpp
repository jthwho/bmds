/**
 * @file      soundfont.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for SoundFonts (devplan/audio.md §22): load, play notes
 * across every preset, the drums and the keyboard, with controllers and pitch
 * bend, render, close; nothing may fault, read out of bounds, overflow or
 * leak, and every sample must be finite.
 */

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <bmds/soundfont.h>
#include <bmds/synth.h>
#include "hostmemory.h"

using bmds::Error;
using bmds::SoundFont;
using bmds::Synth;

namespace {

        class Input final : public SoundFont::Reader {
                public:
                        Input(const uint8_t *data, size_t size) : _data(data), _size(size) {}
                        Error read(void *out, size_t bytes) override {
                                if (bytes > _size - _at) return Error::EndOfFile;
                                std::memcpy(out, _data + _at, bytes);
                                _at += bytes;
                                return Error::Ok;
                        }
                        Error skip(size_t bytes) override {
                                if (bytes > _size - _at) return Error::EndOfFile;
                                _at += bytes;
                                return Error::Ok;
                        }

                private:
                        const uint8_t *_data;
                        size_t         _size;
                        size_t         _at = 0;
        };

        Synth::Event message(uint64_t frame, uint8_t a, uint8_t b, uint8_t c) {
                Synth::Event e;
                e.frame = frame;
                e.length = 3;
                e.data[0] = a;
                e.data[1] = b;
                e.data[2] = c;
                return e;
        }

        void play(Synth &synth, uint64_t frame) {
                static float out[2 * 1024];
                synth.render(out, 1024);
                for (float v : out)
                        if (!std::isfinite(v)) abort();
                (void)frame;
                return;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        const size_t before = HostMemory::liveAllocations();
        Input        in(data, size);
        auto [font, err] = SoundFont::load(in);
        if (err.isOk()) {
                for (size_t i = 0; i < font->presetCount(); ++i) (void)font->preset(i).name[0];
                Synth         synth;
                Synth::Config c;
                c.maxVoices = 16;
                if (synth.open(*font, c).isError()) abort();
                uint64_t frame = 0;
                // Each preset (by program, as GM files choose them), a low, middle and high key,
                // soft and loud; the drums; a bend, a pan, a sustain; then notes off.
                const size_t presets = font->presetCount() < 8 ? font->presetCount() : 8;
                for (size_t p = 0; p < presets; ++p) {
                        const SoundFont::Preset pr = font->preset(p);
                        Synth::Event            program;
                        program.frame = frame;
                        program.length = 2;
                        program.data[0] = 0xc0;
                        program.data[1] = static_cast<uint8_t>(pr.number & 0x7f);
                        (void)synth.queue(message(frame, 0xb0, 0, static_cast<uint8_t>(pr.bank & 0x7f)));
                        (void)synth.queue(program);
                        for (uint8_t key : {uint8_t(0), uint8_t(36), uint8_t(60), uint8_t(96), uint8_t(127)})
                                (void)synth.queue(message(frame + key, 0x90, key, key == 60 ? 127 : 20));
                        (void)synth.queue(message(frame + 10, 0x99, 38, 100));
                        (void)synth.queue(message(frame + 20, 0xe0, 0, 0x7f));
                        (void)synth.queue(message(frame + 30, 0xb0, 10, 0));
                        (void)synth.queue(message(frame + 40, 0xb0, 64, 127));
                        play(synth, frame);
                        frame += 1024;
                        (void)synth.queue(message(frame, 0xb0, 123, 0));
                        (void)synth.queue(message(frame, 0xb0, 64, 0));
                        play(synth, frame);
                        frame += 1024;
                }
                synth.close();
                font->destroy();
        }
        if (HostMemory::liveAllocations() != before && getenv("BMDS_NO_LEAK_ABORT") == nullptr) abort(); // a leak
        return 0;
}
