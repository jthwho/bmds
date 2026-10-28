/**
 * @file      benchsynth.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The synths' benchmarks (devplan/audio.md §26, bench.md): one FM voice's 64-frame block by its
 * operators' setup, and 1 to 32 sustained voices through each engine, so the two compare on
 * the board; a song through both; the synth module's 32 to 128 voices of a real instrument, on one
 * core or both (devplan/synthmodule.md). Nothing is played: the blocks are rendered and dropped.
 */

#include <cstdio>
#include <cstring>
#include <bmds/bench.h>
#include <bmds/fmsynth.h>
#include <bmds/memory.h>
#include <bmds/midisequence.h>
#include <bmds/op2bank.h>
#include <bmds/stopwatch.h>
#include <bmds/string.h>
#include <bmds/synth.h>
#include <bmds/synthfile.h>
#include <bmds/task.h>
#include <bmds/vfs.h>
#include <atomic>

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

        // A display's load, as a game makes it: a task on the other core copying 4 MB of PSRAM
        // round and round, pushing the shared L2 cache's lines (flash's and PSRAM's) out.
        struct Busy {
                        std::atomic<bool> stop{false};
                        uint8_t          *a = nullptr;
                        uint8_t          *b = nullptr;
                        Task              task;

                        static constexpr size_t Bytes = 2 * 1024 * 1024;

                        static void entry(void *arg) {
                                auto *self = static_cast<Busy *>(arg);
                                while (!self->stop.load(std::memory_order_relaxed)) {
                                        std::memcpy(self->b, self->a, Bytes);
                                        std::memcpy(self->a, self->b, Bytes);
                                }
                                for (;;) Task::sleep(1000);
                        }

                        Error start() {
                                a = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Bytes));
                                b = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, Bytes));
                                if (a == nullptr || b == nullptr) return Error::NoMem;
                                Task::Config tc;
                                tc.name = "benchbusy";
                                tc.stackBytes = 2048;
                                tc.priority = 1;
                                tc.core = Task::currentCore() == 0 ? 1 : 0;
                                return task.start(tc, entry, this);
                        }

                        ~Busy() {
                                stop.store(true);
                                Task::sleep(50);
                                (void)task.stop();
                                Memory::free(a);
                                Memory::free(b);
                        }
        };

        // With busy=1, measured beside a Busy.
        Error withBusy(BenchContext &ctx, Error (*fn)(BenchContext &)) {
                if (ctx.param("busy") != "1") return fn(ctx);
                void *mem = Memory::alloc(MemSpace::Internal, sizeof(Busy));
                if (mem == nullptr) return Error::NoMem;
                auto *busy = ::new (mem) Busy();
                Error err = busy->start();
                if (err.isOk()) {
                        Task::sleep(100);
                        err = fn(ctx);
                }
                busy->~Busy();
                Memory::free(mem);
                return err;
        }

        // 1, 8, 16 and 32 voices sustaining, a note each.
        template <typename S> Error measureVoices(BenchContext &ctx, S &synth) {
                uint32_t playing = 0;
                for (const uint32_t n : {1u, 8u, 16u, 32u}) {
                        while (playing < n) noteOn(synth, uint8_t(36 + playing++ * 2));
                        char metric[24];
                        std::snprintf(metric, sizeof(metric), "voices_%u_us", unsigned(n));
                        if (Error err = measureBlocks(ctx, synth, metric); err.isError()) return err;
                        const SynthEngine::Stats st = synth.stats();
                        if (st.voices < n) ctx.note("%s: only %u voices sounding", metric, unsigned(st.voices));
                }
                return Error::Ok;
        }

        SynthEngine::Config config() {
                SynthEngine::Config c;
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

        Error fmVoices(BenchContext &ctx) {
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
        Error wavetableVoices(BenchContext &ctx) {
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

        Error runFmVoices(BenchContext &ctx) {
                return withBusy(ctx, fmVoices);
        }

        Error runWavetableVoices(BenchContext &ctx) {
                return withBusy(ctx, wavetableVoices);
        }

        // ---- A song: a MIDI file rendered as fast as it goes, through each engine

        constexpr size_t SongChunk = 256;

        // The file at @p path into @p seq.
        Error loadSong(StringView path, MidiSequence &seq) {
                auto [info, serr] = Vfs::stat(path, Vfs::Context());
                if (serr.isError()) return serr;
                if (info.size > MidiSequence::MaxFileBytes) return Error::Invalid;
                auto [file, err] = Vfs::open(path, Vfs::Read, Vfs::Context());
                if (err.isError()) return err;
                const size_t size = size_t(info.size);
                auto        *data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size != 0 ? size : 1));
                if (data == nullptr) return Error::NoMem;
                Error rerr = Error::Ok;
                for (size_t done = 0; done < size && rerr.isOk();) {
                        auto [n, e] = file->read(WritableByteSpan(data + done, size - done), WaitForever);
                        rerr = e.isError() ? e : n == 0 ? Error::Invalid : Error::Ok;
                        done += n;
                }
                if (rerr.isOk()) rerr = seq.load(ByteSpan(data, size));
                Memory::free(data);
                return rerr;
        }

        // The song through @p engine once: its messages queued a chunk ahead, every chunk rendered;
        // the voices sounding summed a chunk.
        void playSong(SynthEngine &engine, const MidiSequence &seq, uint64_t &voiceChunks, uint64_t &chunks) {
                engine.reset();
                float          out[SongChunk * 2];
                const uint64_t base = engine.stats().frames;
                const uint64_t end = (uint64_t(seq.lengthMs()) + 2000) * 48;
                size_t         next = 0;
                for (uint64_t frame = 0; frame < end; frame += SongChunk) {
                        while (next < seq.count() && uint64_t(seq.message(next).ms) * 48 < frame + SongChunk) {
                                const MidiSequence::Message &m = seq.message(next++);
                                SynthEngine::Event           e;
                                e.frame = base + uint64_t(m.ms) * 48;
                                e.length = m.length;
                                std::memcpy(e.data, m.data, m.length);
                                (void)engine.queue(e);
                        }
                        engine.render(out, SongChunk);
                        voiceChunks += engine.stats().voices;
                        ++chunks;
                }
                return;
        }

        Error runSong(BenchContext &ctx) {
                MidiSequence seq;
                if (Error err = loadSong(ctx.param("file"), seq); err.isError()) return err;
                const double seconds = double(seq.lengthMs() + 2000) / 1000.0;
                for (const SynthEngine::Kind kind : {SynthEngine::Kind::Fm, SynthEngine::Kind::Wavetable}) {
                        const bool fm = kind == SynthEngine::Kind::Fm;
                        auto [engine, err] = SynthEngines::open(kind, StringView(), config());
                        if (err.isError()) {
                                ctx.note("%s: %s", SynthEngines::name(kind), err.desc());
                                continue;
                        }
                        uint64_t voiceChunks = 0;
                        uint64_t chunks = 0;
                        err = ctx.measure(fm ? "fm_us_per_s" : "wavetable_us_per_s", "us", BenchBetter::Lower,
                                          [&](double &v) {
                                                  voiceChunks = chunks = 0;
                                                  Stopwatch watch;
                                                  playSong(*engine, seq, voiceChunks, chunks);
                                                  v = double(watch.elapsedUs()) / seconds;
                                                  return Error(Error::Ok);
                                          });
                        if (err.isOk() && chunks != 0)
                                ctx.report(fm ? "fm_voices_avg" : "wavetable_voices_avg", "voices", BenchBetter::Lower,
                                           double(voiceChunks) / double(chunks));
                        SynthEngines::close(engine);
                        if (err.isError()) return err;
                }
                ctx.note("%.1f s of music, %u messages", seconds, unsigned(seq.count()));
                return Error::Ok;
        }

        // ---- The synth module's questions (devplan/synthmodule.md step 1): many voices of a real
        // instrument, its samples read through PSRAM as they play (not one short loop the cache
        // keeps), on one core or on both at once.

        constexpr uint16_t ModuleVoices = SynthEngine::MaxVoices; // 128 notes, two voices each in stereo
        constexpr int      ModuleBlocks = 150;                    // 0.2 s: before the piano's shortest samples end

        SynthEngine::Config moduleConfig() {
                SynthEngine::Config c = config();
                c.maxVoices = ModuleVoices;
                return c;
        }

        template <typename S> void control(S &synth, uint8_t channel, uint8_t number, uint8_t value) {
                typename S::Event e;
                e.frame = synth.stats().frames;
                e.length = 3;
                e.data[0] = uint8_t(0xb0 | channel);
                e.data[1] = number;
                e.data[2] = value;
                (void)synth.queue(e);
                return;
        }

        // @p n notes of program @p number from silence, on 8 channels (not the drums'), a note a
        // block as a player's would start: each voice at its own place in its sample.
        void startVoices(Synth &synth, uint8_t number, uint32_t n) {
                float out[Block * 2];
                for (uint8_t ch = 0; ch < 8; ++ch) {
                        control(synth, ch, 120, 0); // all sound off
                        SynthEngine::Event e;
                        e.frame = synth.stats().frames;
                        e.length = 2;
                        e.data[0] = uint8_t(0xc0 | ch);
                        e.data[1] = number;
                        (void)synth.queue(e);
                }
                synth.render(out, Block);
                for (uint32_t i = 0; i < n; ++i) {
                        SynthEngine::Event e;
                        e.frame = synth.stats().frames;
                        e.length = 3;
                        e.data[0] = uint8_t(0x90 | (i % 8));
                        e.data[1] = uint8_t(28 + (i / 8) * 4 + (i % 4)); // 28 to 91
                        e.data[2] = 100;
                        (void)synth.queue(e);
                        synth.render(out, Block);
                }
                return;
        }

        // The other core's load: the same voices on a synth of its own, started again and again.
        struct OtherCore {
                        std::atomic<bool> stop{false};
                        std::atomic<bool> stopped{false};
                        Synth            *synth = nullptr;
                        uint8_t           number = 0;
                        uint32_t          voices = 0;
                        Task              task;

                        static void entry(void *arg) {
                                auto *self = static_cast<OtherCore *>(arg);
                                float out[Block * 2];
                                while (!self->stop.load(std::memory_order_relaxed)) {
                                        startVoices(*self->synth, self->number, self->voices);
                                        for (int i = 0; i < 2 * ModuleBlocks && !self->stop.load(); ++i)
                                                self->synth->render(out, Block);
                                }
                                self->stopped.store(true);
                                for (;;) Task::sleep(1000);
                        }
        };

        Error runModule(BenchContext &ctx) {
                uint64_t number = 0;
                uint64_t cores = 1;
                if (Error err = ctx.paramUnsigned("program", number); err.isError()) return err;
                if (Error err = ctx.paramUnsigned("cores", cores); err.isError()) return err;
                if (number > 127 || cores < 1 || cores > 2) return Error::Invalid;
                String path;
                if (ctx.param("font").isEmpty()) {
                        if (Error err = SoundFonts::defaultFont(path); err.isError()) return err;
                } else if (Error err = path.assign(ctx.param("font")); err.isError()) {
                        return err;
                }
                Synth synth;
                Synth other;
                auto [font, err] = SoundFonts::openSynth(path.view(), moduleConfig(), synth);
                if (err.isError()) return err;
                SoundFont *otherFont = nullptr;
                if (cores == 2) {
                        auto [f, oerr] = SoundFonts::openSynth(path.view(), moduleConfig(), other);
                        otherFont = f;
                        err = oerr;
                }
                ctx.note("%.*s, program %u, %u MB of samples", int(path.view().size()), path.view().data(),
                         unsigned(number), unsigned((synth.instrumentBytes() + 512 * 1024) >> 20));
                for (const uint32_t n : {32u, 64u, 96u, 128u}) {
                        if (err.isError()) break;
                        void *mem = nullptr;
                        if (cores == 2) {
                                mem = Memory::alloc(MemSpace::Internal, sizeof(OtherCore));
                                if (mem == nullptr) {
                                        err = Error::NoMem;
                                        break;
                                }
                        }
                        OtherCore *load = mem != nullptr ? ::new (mem) OtherCore() : nullptr;
                        if (load != nullptr) {
                                load->synth = &other;
                                load->number = uint8_t(number);
                                load->voices = n;
                                Task::Config tc;
                                tc.name = "benchcore";
                                tc.stackBytes = 4096;
                                tc.priority = 2;
                                tc.core = Task::currentCore() == 0 ? 1 : 0;
                                err = load->task.start(tc, OtherCore::entry, load);
                        }
                        uint32_t fewest = ModuleVoices;
                        char     metric[24];
                        std::snprintf(metric, sizeof(metric), "notes_%u_us", unsigned(n));
                        if (err.isOk())
                                err = ctx.measure(metric, "us", BenchBetter::Lower, [&](double &v) {
                                        startVoices(synth, uint8_t(number), n);
                                        float     out[Block * 2];
                                        Stopwatch watch;
                                        for (int i = 0; i < ModuleBlocks; ++i) synth.render(out, Block);
                                        v = double(watch.elapsedUs()) / ModuleBlocks;
                                        const uint32_t sounding = synth.stats().voices;
                                        if (sounding < fewest) fewest = sounding;
                                        return Error(Error::Ok);
                                });
                        if (err.isOk())
                                ctx.note("%s: %u voices sounding at the end (the fewest)", metric, unsigned(fewest));
                        if (load != nullptr) {
                                load->stop.store(true);
                                while (!load->stopped.load()) Task::sleep(5);
                                (void)load->task.stop();
                                load->~OtherCore();
                                Memory::free(mem);
                        }
                }
                if (otherFont != nullptr) SoundFonts::closeSynth(other, otherFont);
                SoundFonts::closeSynth(synth, font);
                ctx.note("a 64-frame block is %.0f us of sound", double(BlockUs));
                return err;
        }

        constexpr BenchParam ModuleParams[] = {
                {"program", "0", "The GM program every voice plays (0: the piano, its samples unlooped)"},
                {"cores", "1", "2: the same voices on a second synth on the other core at the same time"},
                {"font", "", "A SoundFont (empty: synth/font)"},
        };

        constexpr BenchParam SongParams[] = {
                {"file", "/sd0/p0/share/bench/scale.mid", "A MIDI file"},
        };

        constexpr BenchParam VoicesParams[] = {
                {"busy", "0", "1: beside a task copying PSRAM on the other core (a game's load)"},
        };

} // namespace

BMDS_BENCH(synthFmVoice, "synth.fm.voice", runFmVoice, BenchQuick,
           "One FM voice's 64-frame block (stereo, 48 kHz) by its operators: sine, feedback, additive, a piano "
           "(us a block)")
BMDS_BENCH_WITH(synthFmVoices, "synth.fm.voices", runFmVoices, BenchQuick, VoicesParams,
                "1 to 32 FM voices sustaining, a 64-frame block (us)")
BMDS_BENCH_WITH(synthSong, "synth.song", runSong, BenchQuick, SongParams,
                "A MIDI file rendered as fast as it goes through the FM synth and the wavetable (us a second of it, "
                "and the voices sounding)")
BMDS_BENCH_WITH(synthWavetableVoices, "synth.wavetable.voices", runWavetableVoices, BenchQuick, VoicesParams,
                "1 to 32 wavetable voices (synth/font's organ) sustaining, a 64-frame block (us)")

BMDS_BENCH_WITH(synthModule, "synth.module", runModule, BenchQuick, ModuleParams,
                "32 to 128 notes of one wavetable instrument started a block apart, a 64-frame block (us), on "
                "one core or both")

BMDS_NAMESPACE_END
