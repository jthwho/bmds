/**
 * @file      main.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * bmds-synthwav: renders a MIDI file to a WAV file through the system's synths, as the board
 * plays it (devplan/audio.md §26), so a bank or a font can be heard on a PC:
 *
 *   bmds-synthwav [-b BANK.op2 | -f FONT.sf2] [-g DB] [-v VOICES] [-s SECONDS] IN.mid OUT.wav
 *
 * -b renders through the FM synth on an OP2 bank, -f through the wavetable synth on a
 * SoundFont (one of them is needed); -g is the output's gain in dB (-10, synth/gain's default),
 * -v the voices (32), -s the release after the last event (2). Writes 48 kHz 16-bit stereo, and
 * prints the level, the voices and the render's speed. Exit status: 0, 1 on a failure, 2 on usage errors.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <bmds/fmsynth.h>
#include <bmds/op2bank.h>
#include <bmds/soundfont.h>
#include <bmds/synth.h>

extern "C" {
#include "../../sdk/game/game_internal.h"
}

namespace {

        constexpr uint32_t Rate = 48000;
        constexpr size_t   Chunk = 256; // frames rendered at a time; events queued a chunk ahead

        bool readFile(const char *path, std::vector<uint8_t> &out) {
                FILE *f = std::fopen(path, "rb");
                if (f == nullptr) return false;
                uint8_t buf[65536];
                size_t  n = 0;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
                std::fclose(f);
                return true;
        }

        void put16(std::vector<uint8_t> &b, uint32_t v) {
                b.push_back(uint8_t(v));
                b.push_back(uint8_t(v >> 8));
                return;
        }

        void put32(std::vector<uint8_t> &b, uint32_t v) {
                put16(b, v & 0xffff);
                put16(b, v >> 16);
                return;
        }

        bool writeWav(const char *path, const std::vector<int16_t> &samples) {
                std::vector<uint8_t> h;
                const uint32_t       data = uint32_t(samples.size() * 2);
                h.insert(h.end(), {'R', 'I', 'F', 'F'});
                put32(h, 36 + data);
                h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
                put32(h, 16);
                put16(h, 1); // PCM
                put16(h, 2);
                put32(h, Rate);
                put32(h, Rate * 4);
                put16(h, 4);
                put16(h, 16);
                h.insert(h.end(), {'d', 'a', 't', 'a'});
                put32(h, data);
                FILE *f = std::fopen(path, "wb");
                if (f == nullptr) return false;
                bool ok = std::fwrite(h.data(), 1, h.size(), f) == h.size();
                ok = ok && std::fwrite(samples.data(), 2, samples.size(), f) == samples.size();
                return std::fclose(f) == 0 && ok;
        }

        // A SoundFont read from memory.
        class MemoryReader final : public bmds::SoundFont::Reader {
                public:
                        explicit MemoryReader(const std::vector<uint8_t> &d) : _data(d) {}
                        bmds::Error read(void *out, size_t bytes) override {
                                if (bytes > _data.size() - _pos) return bmds::Error::EndOfFile;
                                std::memcpy(out, _data.data() + _pos, bytes);
                                _pos += bytes;
                                return bmds::Error::Ok;
                        }
                        bmds::Error skip(size_t bytes) override {
                                if (bytes > _data.size() - _pos) return bmds::Error::EndOfFile;
                                _pos += bytes;
                                return bmds::Error::Ok;
                        }

                private:
                        const std::vector<uint8_t> &_data;
                        size_t                      _pos = 0;
        };

        struct Totals {
                        double   seconds = 0; // rendering
                        double   sumSquares = 0;
                        float    peak = 0;
                        uint64_t clipped = 0;
        };

        // Plays @p smf through @p synth (a Synth or an FmSynth) into @p out.
        template <typename S>
        int play(S &synth, bmds_game_smf_t &smf, double tailSeconds, std::vector<int16_t> &out, Totals &t) {
                bmds_game_smf_event_t e;
                int                   have = bmds_game_smf_next(&smf, &e);
                uint64_t              frame = 0;
                uint64_t              lastEvent = 0;
                float                 buf[Chunk * 2];
                for (;;) {
                        // Every event before the next chunk's end.
                        while (have == 1) {
                                const uint64_t at = uint64_t(e.ns < 0 ? 0 : e.ns) * Rate / 1000000000u;
                                if (at >= frame + Chunk) break;
                                typename S::Event ev;
                                ev.frame = at;
                                if (e.kind == BMDS_GAME_SMF_MESSAGE) {
                                        ev.length = e.length;
                                        std::memcpy(ev.data, e.data, e.length);
                                } else if (e.count <= bmds::Synth::MaxEventBytes) {
                                        ev.length = uint8_t(e.count);
                                        std::memcpy(ev.data, e.bytes, e.count);
                                }
                                if (ev.length != 0 && synth.queue(ev).isError()) {
                                        std::fprintf(stderr, "bmds-synthwav: the synth's queue is full\n");
                                        return 1;
                                }
                                lastEvent = at;
                                have = bmds_game_smf_next(&smf, &e);
                        }
                        if (have != 1 && frame > lastEvent + uint64_t(tailSeconds * Rate)) break;
                        const auto start = std::chrono::steady_clock::now();
                        synth.render(buf, Chunk);
                        t.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                        for (size_t i = 0; i < Chunk * 2; ++i) {
                                const float x = buf[i];
                                t.sumSquares += double(x) * x;
                                t.peak = std::fabs(x) > t.peak ? std::fabs(x) : t.peak;
                                if (std::fabs(x) > 1.0f) ++t.clipped;
                                const float c = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
                                out.push_back(int16_t(std::lrint(c * 32767.0f)));
                        }
                        frame += Chunk;
                }
                const bmds::Synth::Stats st = synth.stats();
                std::printf("voices: %u at most of %u, %u notes at the limit; %u messages ignored\n", st.peakVoices,
                            st.maxVoices, st.notesAtLimit, st.ignored);
                return 0;
        }

        int usage() {
                std::fprintf(stderr,
                             "usage: bmds-synthwav [-b BANK.op2 | -f FONT.sf2] [-g DB] [-v VOICES] [-s SECONDS] "
                             "IN.mid OUT.wav\n");
                return 2;
        }

} // namespace

int main(int argc, char **argv) {
        const char *bankPath = nullptr;
        const char *fontPath = nullptr;
        double      gain = -10; // synth/gain's default
        long        voices = 32;
        double      tail = 2;
        int         i = 1;
        for (; i < argc && argv[i][0] == '-'; ++i) {
                if (i + 1 >= argc) return usage();
                const char  opt = argv[i][1];
                const char *arg = argv[++i];
                if (opt == 'b') {
                        bankPath = arg;
                } else if (opt == 'f') {
                        fontPath = arg;
                } else if (opt == 'g') {
                        gain = std::atof(arg);
                } else if (opt == 'v') {
                        voices = std::atol(arg);
                } else if (opt == 's') {
                        tail = std::atof(arg);
                } else {
                        return usage();
                }
        }
        if (argc - i != 2 || (bankPath == nullptr) == (fontPath == nullptr) || voices < 1 || voices > 256)
                return usage();
        std::vector<uint8_t> midi;
        if (!readFile(argv[i], midi)) {
                std::fprintf(stderr, "bmds-synthwav: can't read %s\n", argv[i]);
                return 1;
        }
        static bmds_game_smf_t smf;
        if (bmds_game_smf_open(&smf, midi.data(), midi.size()) != 0) {
                std::fprintf(stderr, "bmds-synthwav: %s isn't a MIDI file\n", argv[i]);
                return 1;
        }
        bmds::Synth::Config c;
        c.rateHz = Rate;
        c.channels = 2;
        c.maxVoices = uint16_t(voices);
        c.gainDb = float(gain);
        std::vector<uint8_t> instruments;
        if (!readFile(bankPath != nullptr ? bankPath : fontPath, instruments)) {
                std::fprintf(stderr, "bmds-synthwav: can't read %s\n", bankPath != nullptr ? bankPath : fontPath);
                return 1;
        }
        std::vector<int16_t> out;
        Totals               t;
        int                  rc = 0;
        if (bankPath != nullptr) {
                bmds::Op2Bank bank;
                bmds::FmSynth synth;
                if (bank.open(bmds::ByteSpan(instruments.data(), instruments.size())).isError() ||
                    synth.open(bank, c).isError()) {
                        std::fprintf(stderr, "bmds-synthwav: %s isn't an OP2 bank\n", bankPath);
                        return 1;
                }
                rc = play(synth, smf, tail, out, t);
        } else {
                MemoryReader reader(instruments);
                auto         font = bmds::SoundFont::load(reader);
                if (font.isError()) {
                        std::fprintf(stderr, "bmds-synthwav: %s isn't a SoundFont\n", fontPath);
                        return 1;
                }
                {
                        bmds::Synth synth;
                        if (synth.open(*font.value(), c).isError()) {
                                std::fprintf(stderr, "bmds-synthwav: can't open the synth\n");
                                font.value()->destroy();
                                return 1;
                        }
                        rc = play(synth, smf, tail, out, t);
                }
                font.value()->destroy();
        }
        if (rc != 0) return rc;
        const double seconds = double(out.size()) / 2 / Rate;
        std::printf("%.1f s of sound: level %.1f dBFS RMS, peak %.1f dBFS, %llu samples clipped\n", seconds,
                    10 * std::log10(t.sumSquares / double(out.size()) + 1e-20), 20 * std::log10(t.peak + 1e-20),
                    static_cast<unsigned long long>(t.clipped));
        std::printf("rendered in %.3f s (%.0fx real time)\n", t.seconds, seconds / (t.seconds > 0 ? t.seconds : 1e-9));
        if (!writeWav(argv[i + 1], out)) {
                std::fprintf(stderr, "bmds-synthwav: can't write %s\n", argv[i + 1]);
                return 1;
        }
        return 0;
}
