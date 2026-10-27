/**
 * @file      benchsynth.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The synths' benchmarks (devplan/audio.md §26, bench.md): one FM voice's 64-frame block by its
 * operators' setup, and 1 to 32 sustained voices through each engine, so the two compare on
 * the board. Nothing is played: the blocks are rendered and dropped.
 */

#include <cstdio>
#include <cstring>
#include <bmds/bench.h>
#include <bmds/fmsynth.h>
#include <bmds/memory.h>
#include <bmds/op2bank.h>
#include <bmds/stopwatch.h>
#include <bmds/string.h>
#include <bmds/synth.h>
#include <bmds/synthfile.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t Block = FmSynth::BlockFrames;
        constexpr int    Blocks = 500; // a sample's blocks
        constexpr float  BlockUs = float(Block) * 1e6f / 48000.0f;

        // A voice's sixteen OP2 bytes: the modulator, feedback and connection, the carrier, a
        // spare, the note offset.
        struct VoiceBytes {
                        const char *name;
                        uint8_t     bytes[16];
        };

        // Sustaining operators at their fastest attack, no decay.
        constexpr VoiceBytes FmCases[] = {
                {"sine", {0x21, 0xf0, 0x0f, 0, 0, 0x20, 0x00, 0x21, 0xf0, 0x0f, 0, 0, 0, 0, 0, 0}},
                {"sine_fb", {0x21, 0xf0, 0x0f, 0, 0, 0x20, 0x0e, 0x21, 0xf0, 0x0f, 0, 0, 0, 0, 0, 0}},
                {"additive", {0x21, 0xf0, 0x0f, 0, 0, 0x20, 0x01, 0x21, 0xf0, 0x0f, 0, 0, 0, 0, 0, 0}},
                // DMXOPL3's piano's first voice (MIT, DMXOPL3 by Sneakernets): waveforms 2 and 4, feedback 7.
                {"piano",
                 {0x33, 0xe1, 0x23, 0x02, 0x80, 0x25, 0x0e, 0x31, 0xf1, 0xf4, 0x04, 0x00, 0x09, 0, 0xf4, 0xff}},
        };

        // An OP2 bank in PSRAM, every instrument @p voice.
        uint8_t *makeBank(const uint8_t (&voice)[16]) {
                auto *bank = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Op2Bank::MinBytes));
                if (bank == nullptr) return nullptr;
                std::memcpy(bank, "#OPL_II#", Op2Bank::HeaderBytes);
                for (size_t i = 0; i < Op2Bank::Instruments; ++i) {
                        uint8_t *p = bank + Op2Bank::HeaderBytes + i * Op2Bank::InstrumentBytes;
                        std::memset(p, 0, 4);
                        p[2] = 128;
                        std::memcpy(p + 4, voice, 16);
                        std::memcpy(p + 20, voice, 16);
                }
                return bank;
        }

        template <typename S> void noteOn(S &synth, uint8_t key) {
                typename S::Event e;
                e.frame = synth.stats().frames;
                e.length = 3;
                e.data[0] = 0x90;
                e.data[1] = key;
                e.data[2] = 100;
                (void)synth.queue(e);
                return;
        }

        template <typename S> void program(S &synth, uint8_t number) {
                typename S::Event e;
                e.frame = synth.stats().frames;
                e.length = 2;
                e.data[0] = 0xc0;
                e.data[1] = number;
                (void)synth.queue(e);
                return;
        }

        // A block of @p synth, its median µs over `Blocks` blocks.
        template <typename S> Error measureBlocks(BenchContext &ctx, S &synth, const char *metric) {
                float out[Block * 2];
                for (int i = 0; i < 20; ++i) synth.render(out, Block); // past the attack
                return ctx.measure(metric, "us", BenchBetter::Lower, [&](double &v) {
                        Stopwatch watch;
                        for (int i = 0; i < Blocks; ++i) synth.render(out, Block);
                        v = double(watch.elapsedUs()) / Blocks;
                        return Error(Error::Ok);
                });
        }

        // 1, 8, 16 and 32 voices sustaining, a note each.
        template <typename S> Error measureVoices(BenchContext &ctx, S &synth) {
                uint32_t playing = 0;
                for (const uint32_t n : {1u, 8u, 16u, 32u}) {
                        while (playing < n) noteOn(synth, uint8_t(36 + playing++ * 2));
                        char metric[24];
                        std::snprintf(metric, sizeof(metric), "voices_%u_us", unsigned(n));
                        if (Error err = measureBlocks(ctx, synth, metric); err.isError()) return err;
                        const Synth::Stats st = synth.stats();
                        if (st.voices < n) ctx.note("%s: only %u voices sounding", metric, unsigned(st.voices));
                }
                return Error::Ok;
        }

        Synth::Config config() {
                Synth::Config c;
                c.rateHz = 48000;
                c.channels = 2;
                c.maxVoices = 32;
                return c;
        }

        // One FM voice's block by its setup, and an empty synth's (the fixed cost).
        Error runFmVoice(BenchContext &ctx) {
                for (const VoiceBytes &k : FmCases) {
                        uint8_t *bytes = makeBank(k.bytes);
                        if (bytes == nullptr) return Error::NoMem;
                        Op2Bank bank;
                        Error   err = bank.open(ByteSpan(bytes, Op2Bank::MinBytes));
                        FmSynth synth;
                        if (err.isOk()) err = synth.open(bank, config());
                        if (err.isOk() && &k == &FmCases[0]) err = measureBlocks(ctx, synth, "empty_us");
                        if (err.isOk()) {
                                noteOn(synth, 69);
                                char metric[24];
                                std::snprintf(metric, sizeof(metric), "%s_us", k.name);
                                err = measureBlocks(ctx, synth, metric);
                        }
                        synth.close();
                        Memory::free(bytes);
                        if (err.isError()) return err;
                }
                ctx.note("a 64-frame block is %.0f us of sound", double(BlockUs));
                return Error::Ok;
        }

        Error runFmVoices(BenchContext &ctx) {
                uint8_t *bytes = makeBank(FmCases[3].bytes);
                if (bytes == nullptr) return Error::NoMem;
                Op2Bank bank;
                Error   err = bank.open(ByteSpan(bytes, Op2Bank::MinBytes));
                FmSynth synth;
                if (err.isOk()) err = synth.open(bank, config());
                if (err.isOk()) err = measureVoices(ctx, synth);
                synth.close();
                Memory::free(bytes);
                return err;
        }

        // The default font (synth/font), an organ (sustaining, looped).
        Error runWavetableVoices(BenchContext &ctx) {
                String path;
                if (Error err = SoundFonts::defaultFont(path); err.isError()) return err;
                Synth synth;
                auto [font, err] = SoundFonts::openSynth(path.view(), config(), synth);
                if (err.isError()) return err;
                program(synth, 16);
                err = measureVoices(ctx, synth);
                SoundFonts::closeSynth(synth, font);
                return err;
        }

} // namespace

BMDS_BENCH(synthFmVoice, "synth.fm.voice", runFmVoice, BenchQuick,
           "One FM voice's 64-frame block (stereo, 48 kHz) by its operators: sine, feedback, additive, a piano "
           "(us a block)")
BMDS_BENCH(synthFmVoices, "synth.fm.voices", runFmVoices, BenchQuick,
           "1 to 32 FM voices sustaining, a 64-frame block (us)")
BMDS_BENCH(synthWavetableVoices, "synth.wavetable.voices", runWavetableVoices, BenchQuick,
           "1 to 32 wavetable voices (synth/font's organ) sustaining, a 64-frame block (us)")

BMDS_NAMESPACE_END
