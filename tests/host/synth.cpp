/**
 * @file      synth.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The synth (devplan/audio.md §22): a SoundFont made here (one preset, a
 * looped sine), loaded and played: events on their frames, the pitch, the
 * queue, the voice limit, damaged fonts; the fuzzer's seeds and finds.
 */

#include <doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <bmds/decoder.h>
#include <bmds/mididecoder.h>
#include <bmds/soundfont.h>
#include <bmds/synth.h>

using bmds::Error;
using bmds::SoundFont;
using bmds::Synth;

namespace {

        // A SoundFont 2: one preset ("Sine", bank 0, program 0) on one instrument playing one
        // sample, a 440 Hz sine at 44 kHz (exactly 100 samples a cycle), looped, its key A4.
        class FontWriter {
                public:
                        std::vector<uint8_t> build() {
                                std::vector<uint8_t> smpl;
                                for (int i = 0; i < SampleFrames + 46; ++i) {
                                        const double v =
                                                i < SampleFrames ? std::sin(2 * M_PI * i / 100.0) * 30000.0 : 0;
                                        put16(smpl, static_cast<uint16_t>(static_cast<int16_t>(v)));
                                }
                                std::vector<uint8_t> info = chunk("ifil", {2, 0, 1, 0});
                                std::vector<uint8_t> pdta;
                                append(pdta, chunk("phdr", cat(phdr("Sine", 0, 0, 0), phdr("EOP", 0, 0, 1))));
                                append(pdta, chunk("pbag", cat(bag(0, 0), bag(1, 0))));
                                append(pdta, chunk("pmod", std::vector<uint8_t>(10, 0)));
                                append(pdta, chunk("pgen", cat(gen(41, 0), gen(0, 0)))); // instrument 0
                                append(pdta, chunk("inst", cat(inst("Sine", 0), inst("EOI", 1))));
                                append(pdta, chunk("ibag", cat(bag(0, 0), bag(2, 0))));
                                append(pdta, chunk("imod", std::vector<uint8_t>(10, 0)));
                                append(pdta, chunk("igen", cat(gen(54, 1), gen(53, 0), gen(0, 0)))); // looped; sample 0
                                append(pdta, chunk("shdr", cat(shdr("Sine", 0, SampleFrames, 1000, 2000, 44000, 69),
                                                               shdr("EOS", 0, 0, 0, 0, 0, 0))));
                                std::vector<uint8_t> body = {'s', 'f', 'b', 'k'};
                                append(body, list("INFO", info));
                                append(body, list("sdta", chunk("smpl", smpl)));
                                append(body, list("pdta", pdta));
                                return chunk("RIFF", body);
                        }

                        static constexpr int SampleFrames = 4000;

                private:
                        using Bytes = std::vector<uint8_t>;
                        static void put16(Bytes &b, uint16_t v) {
                                b.push_back(static_cast<uint8_t>(v));
                                b.push_back(static_cast<uint8_t>(v >> 8));
                        }
                        static void put32(Bytes &b, uint32_t v) {
                                put16(b, static_cast<uint16_t>(v));
                                put16(b, static_cast<uint16_t>(v >> 16));
                        }
                        static void name(Bytes &b, const char *n) {
                                char buf[20] = {};
                                std::strncpy(buf, n, sizeof(buf) - 1);
                                b.insert(b.end(), buf, buf + 20);
                        }
                        static void append(Bytes &to, const Bytes &from) {
                                to.insert(to.end(), from.begin(), from.end());
                        }
                        static Bytes cat(const Bytes &a, const Bytes &b) {
                                Bytes r = a;
                                append(r, b);
                                return r;
                        }
                        static Bytes cat(const Bytes &a, const Bytes &b, const Bytes &c) { return cat(cat(a, b), c); }
                        static Bytes chunk(const char *id, const Bytes &data) {
                                Bytes r(id, id + 4);
                                put32(r, static_cast<uint32_t>(data.size()));
                                append(r, data);
                                if (data.size() % 2 != 0) r.push_back(0);
                                return r;
                        }
                        static Bytes list(const char *type, const Bytes &data) {
                                Bytes body(type, type + 4);
                                append(body, data);
                                return chunk("LIST", body);
                        }
                        static Bytes phdr(const char *n, uint16_t preset, uint16_t bank, uint16_t bagIndex) {
                                Bytes r;
                                name(r, n);
                                put16(r, preset);
                                put16(r, bank);
                                put16(r, bagIndex);
                                put32(r, 0);
                                put32(r, 0);
                                put32(r, 0);
                                return r;
                        }
                        static Bytes bag(uint16_t genIndex, uint16_t modIndex) {
                                Bytes r;
                                put16(r, genIndex);
                                put16(r, modIndex);
                                return r;
                        }
                        static Bytes gen(uint16_t oper, uint16_t amount) {
                                Bytes r;
                                put16(r, oper);
                                put16(r, amount);
                                return r;
                        }
                        static Bytes inst(const char *n, uint16_t bagIndex) {
                                Bytes r;
                                name(r, n);
                                put16(r, bagIndex);
                                return r;
                        }
                        static Bytes shdr(const char *n, uint32_t start, uint32_t end, uint32_t loopStart,
                                          uint32_t loopEnd, uint32_t rate, uint8_t key) {
                                Bytes r;
                                name(r, n);
                                put32(r, start);
                                put32(r, end);
                                put32(r, loopStart);
                                put32(r, loopEnd);
                                put32(r, rate);
                                r.push_back(key);
                                r.push_back(0);
                                put16(r, 0);
                                put16(r, n[0] == 'E' && n[1] == 'O' ? 0 : 1); // mono
                                return r;
                        }
        };

        // A font in memory, read in pieces.
        class MemoryReader final : public SoundFont::Reader {
                public:
                        explicit MemoryReader(const std::vector<uint8_t> &data) : _data(data) {}
                        Error read(void *out, size_t bytes) override {
                                if (bytes > _data.size() - _at) return Error::EndOfFile;
                                std::memcpy(out, _data.data() + _at, bytes);
                                _at += bytes;
                                return Error::Ok;
                        }
                        Error skip(size_t bytes) override {
                                if (bytes > _data.size() - _at) return Error::EndOfFile;
                                _at += bytes;
                                return Error::Ok;
                        }

                private:
                        const std::vector<uint8_t> &_data;
                        size_t                      _at = 0;
        };

        SoundFont *loadFont(const std::vector<uint8_t> &data) {
                MemoryReader reader(data);
                auto [font, err] = SoundFont::load(reader);
                return err.isOk() ? font : nullptr;
        }

        Synth::Event message(uint64_t frame, std::initializer_list<uint8_t> bytes) {
                Synth::Event e;
                e.frame = frame;
                e.length = static_cast<uint8_t>(bytes.size());
                size_t i = 0;
                for (uint8_t b : bytes) e.data[i++] = b;
                return e;
        }

        // The rising zero crossings of channel 0 in @p samples (interleaved by @p channels).
        int crossings(const std::vector<float> &samples, int channels) {
                int n = 0;
                for (size_t i = static_cast<size_t>(channels); i < samples.size(); i += static_cast<size_t>(channels))
                        if (samples[i - static_cast<size_t>(channels)] < 0 && samples[i] >= 0) ++n;
                return n;
        }

} // namespace

TEST_CASE("SoundFont: loaded, its presets; damaged ones refused") {
        const std::vector<uint8_t> data = FontWriter().build();
        SoundFont                 *font = loadFont(data);
        REQUIRE(font != nullptr);
        CHECK(font->presetCount() == 1);
        CHECK(std::string(font->preset(0).name) == "Sine");
        CHECK(font->preset(0).bank == 0);
        CHECK(font->preset(0).number == 0);
        CHECK(font->bytes() >= (FontWriter::SampleFrames + 46) * 2); // the samples, 16-bit
        font->destroy();
        // Not a SoundFont, cut short, empty.
        std::vector<uint8_t> junk(1000, 0x55);
        CHECK(loadFont(junk) == nullptr);
        for (size_t cut : {size_t(0), size_t(12), size_t(100), data.size() / 2, data.size() - 20}) {
                std::vector<uint8_t> part(data.begin(), data.begin() + static_cast<long>(cut));
                SoundFont           *f = loadFont(part);
                if (f != nullptr) f->destroy(); // what loads must be whole enough to play
                CHECK_MESSAGE(f == nullptr, "cut at ", cut);
        }
}

TEST_CASE("Synth: events on their frames, the pitch, note off") {
        const std::vector<uint8_t> data = FontWriter().build();
        SoundFont                 *font = loadFont(data);
        REQUIRE(font != nullptr);
        Synth synth;
        CHECK(synth.queue(message(0, {0x90, 69, 100})) == Error::InvalidState);
        Synth::Config c;
        c.rateHz = 48000;
        c.channels = 2;
        Synth::Config bad = c;
        bad.channels = 3;
        CHECK(synth.open(*font, bad) == Error::Invalid);
        bad = c;
        bad.rateHz = 1000;
        CHECK(synth.open(*font, bad) == Error::Invalid);
        bad = c;
        bad.maxVoices = Synth::MaxVoices + 1;
        CHECK(synth.open(*font, bad) == Error::Invalid);
        REQUIRE(synth.open(*font, c) == Error::Ok);
        CHECK(synth.open(*font, c) == Error::InvalidState);
        CHECK(synth.config().maxVoices == Synth::DefaultVoices);

        // A note on at frame 100 of the first 256: silence before it, sound after.
        CHECK(synth.queue(message(100, {0x90, 69, 127})) == Error::Ok);
        CHECK(synth.stats().queued == 1);
        std::vector<float> out(256 * 2, 1.0f);
        synth.render(out.data(), 256);
        bool silent = true;
        for (size_t i = 0; i < 200; ++i) silent &= out[i] == 0.0f;
        CHECK(silent);
        float peak = 0;
        for (size_t i = 200; i < out.size(); ++i) peak = std::fmax(peak, std::fabs(out[i]));
        CHECK(peak > 0.01f);
        CHECK(synth.stats().voices == 1);
        CHECK(synth.stats().frames == 256);

        // A second at its key: 440 Hz, the sample's 44 kHz played at 48 kHz.
        std::vector<float> second(48000 * 2);
        synth.render(second.data(), 48000);
        CHECK(crossings(second, 2) >= 438);
        CHECK(crossings(second, 2) <= 442);
        float right = 0;
        for (size_t i = 1; i < second.size(); i += 2) right = std::fmax(right, std::fabs(second[i]));
        CHECK(right > 0.01f); // both channels
        CHECK(right < 1.0f);

        // An octave up (A5), and an event already late: at the next render's start.
        synth.reset(); // a quick release: gone within the next render
        synth.render(second.data(), 4800);
        CHECK(synth.stats().voices == 0);
        CHECK(synth.queue(message(0, {0x90, 81, 127})) == Error::Ok);
        synth.render(second.data(), 48000);
        CHECK(crossings(second, 2) >= 876);
        CHECK(crossings(second, 2) <= 884);

        // Note off: the release ends it.
        CHECK(synth.queue(message(synth.stats().frames, {0x80, 81, 0})) == Error::Ok);
        synth.render(second.data(), 48000);
        CHECK(synth.stats().voices == 0);
        synth.close();
        CHECK_FALSE(synth.isOpen());
        font->destroy();
}

TEST_CASE("Synth: the queue's order and limit, the voice limit, mono, messages") {
        const std::vector<uint8_t> data = FontWriter().build();
        SoundFont                 *font = loadFont(data);
        REQUIRE(font != nullptr);
        Synth         synth;
        Synth::Config c;
        c.channels = 1;
        c.maxVoices = 4;
        REQUIRE(synth.open(*font, c) == Error::Ok);
        CHECK(synth.queue(Synth::Event()) == Error::Invalid);
        Synth::Event big = message(0, {0x90, 60, 100});
        big.length = Synth::MaxEventBytes + 1;
        CHECK(synth.queue(big) == Error::Invalid);

        // Six notes on four voices, queued out of order.
        for (uint8_t k = 0; k < 6; ++k)
                CHECK(synth.queue(message(50 - 10 * k, {0x90, static_cast<uint8_t>(60 + k), 100})) == Error::Ok);
        std::vector<float> out(64);
        synth.render(out.data(), 64);
        Synth::Stats s = synth.stats();
        CHECK(s.voices <= 4);
        CHECK(s.peakVoices <= 4);
        CHECK(s.notesAtLimit == 2);
        CHECK(s.queued == 0);

        // Program change, controllers, pitch bend, pressure: taken; System On resets.
        CHECK(synth.queue(message(64, {0xc0, 0})) == Error::Ok);
        CHECK(synth.queue(message(64, {0xb0, 7, 100})) == Error::Ok);
        CHECK(synth.queue(message(64, {0xe0, 0, 0x50})) == Error::Ok);
        CHECK(synth.queue(message(64, {0xd0, 40})) == Error::Ok);
        CHECK(synth.queue(message(64, {0xf8})) == Error::Ok);
        CHECK(synth.queue(message(64, {0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7})) == Error::Ok);
        CHECK(synth.queue(message(64, {0xf0, 0x43, 0x10, 0x4c, 0x00, 0xf7})) == Error::Ok); // another SysEx
        synth.render(out.data(), 64);
        CHECK(synth.stats().ignored == 1);

        // A full queue.
        for (size_t i = 0; i < Synth::QueueEvents; ++i) CHECK(synth.queue(message(1000000, {0xfe})) == Error::Ok);
        CHECK(synth.queue(message(1000000, {0xfe})) == Error::Full);
        CHECK(synth.stats().queueFull == 1);
        synth.reset();
        CHECK(synth.stats().queued == 0);
        synth.close();
        font->destroy();
}

namespace {

        std::vector<uint8_t> readFile(const std::filesystem::path &path) {
                std::vector<uint8_t> data;
                FILE                *f = std::fopen(path.c_str(), "rb");
                if (f == nullptr) return data;
                uint8_t buf[4096];
                size_t  n;
                while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.insert(data.end(), buf, buf + n);
                std::fclose(f);
                return data;
        }

        // Every preset by program and bank, keys across the keyboard, the drums, then notes off.
        void playAll(SoundFont &font) {
                Synth         synth;
                Synth::Config c;
                c.maxVoices = 16;
                REQUIRE(synth.open(font, c) == Error::Ok);
                std::vector<float> out(2 * 1024);
                uint64_t           frame = 0;
                for (size_t p = 0; p < font.presetCount(); ++p) {
                        const SoundFont::Preset pr = font.preset(p);
                        CHECK(synth.queue(message(frame, {0xb0, 0, static_cast<uint8_t>(pr.bank & 0x7f)})) ==
                              Error::Ok);
                        CHECK(synth.queue(message(frame, {0xc0, static_cast<uint8_t>(pr.number & 0x7f)})) == Error::Ok);
                        for (uint8_t key : {uint8_t(0), uint8_t(60), uint8_t(127)})
                                CHECK(synth.queue(message(frame + key, {0x90, key, 100})) == Error::Ok);
                        CHECK(synth.queue(message(frame, {0x99, 38, 100})) == Error::Ok);
                        synth.render(out.data(), 1024);
                        for (float v : out) REQUIRE(std::isfinite(v));
                        frame += 1024;
                        CHECK(synth.queue(message(frame, {0xb0, 123, 0})) == Error::Ok);
                        synth.render(out.data(), 1024);
                        frame += 1024;
                }
                return;
        }

} // namespace

TEST_CASE("SoundFont: the fuzzer's seeds play, its finds load safely or not at all") {
        const std::filesystem::path dir(BMDS_SYNTH_DIR);
        for (const char *seed : {"sine.sf2", "rich.sf2"}) {
                const std::vector<uint8_t> data = readFile(dir / seed);
                REQUIRE_MESSAGE(!data.empty(), seed);
                SoundFont *font = loadFont(data);
                REQUIRE_MESSAGE(font != nullptr, seed);
                playAll(*font);
                font->destroy();
        }
        size_t finds = 0;
        for (const auto &entry : std::filesystem::directory_iterator(dir / "fuzz")) {
                const std::vector<uint8_t> data = readFile(entry.path());
                SoundFont                 *font = loadFont(data);
                if (font != nullptr) {
                        playAll(*font);
                        font->destroy();
                }
                ++finds;
        }
        CHECK(finds >= 9);
}

TEST_CASE("MidiDecoder: a MIDI file decoded through the synth, sought, ended") {
        // The test font, through the hooks the kernel gives its shared fonts.
        static std::vector<uint8_t> fontData;
        static SoundFont           *font = nullptr;
        fontData = readFile(std::filesystem::path(BMDS_SYNTH_DIR) / "sine.sf2");
        font = loadFont(fontData);
        REQUIRE(font != nullptr);
        bmds::MidiDecoder::setFonts(
                [](const Synth::Config &c, Synth &s) -> bmds::Result<SoundFont *> {
                        const Error err = s.open(*font, c);
                        return err.isOk() ? bmds::makeResult(font) : bmds::makeError<SoundFont *>(err);
                },
                [](Synth &s, SoundFont *) { s.close(); });
        static bool added = false;
        if (!added) REQUIRE(bmds::Decoder::addFormat("MThd", bmds::MidiDecoder::open) == Error::Ok);
        added = true;

        const std::vector<uint8_t> file = readFile(std::filesystem::path(BMDS_SYNTH_DIR) / "scale.mid");
        REQUIRE(!file.empty());
        struct In {
                        const std::vector<uint8_t> *data;
        } in{&file};
        auto readAt = [](void *arg, uint64_t offset, bmds::WritableByteSpan buf) -> Error {
                const auto &d = *static_cast<In *>(arg)->data;
                if (offset > d.size() || d.size() - offset < buf.size()) return Error::EndOfFile;
                std::memcpy(buf.data(), d.data() + offset, buf.size());
                return Error::Ok;
        };
        auto [d, err] = bmds::Decoder::open({readAt, &in, file.size()});
        REQUIRE(err == Error::Ok);
        const bmds::Decoder::Info info = d->info();
        CHECK(info.codec == bmds::Codec::Midi);
        CHECK(std::string(bmds::Decoder::name(info.codec)) == "midi");
        CHECK(info.format == bmds::SampleFormat::F32Le);
        CHECK(info.channels == 2);
        CHECK(info.rateHz == 48000);
        CHECK(info.frames == 336000); // 5 s and the 2 s release
        // Every frame, then EndOfFile (never nothing without an error).
        std::vector<uint8_t> buf(4096 * 8);
        uint64_t             frames = 0;
        float                peak = 0;
        for (;;) {
                auto [n, derr] = d->decode(bmds::WritableByteSpan(buf.data(), buf.size()));
                for (size_t i = 0; i < n / 4; ++i) {
                        float v;
                        std::memcpy(&v, buf.data() + i * 4, 4);
                        REQUIRE(std::isfinite(v));
                        peak = std::fmax(peak, std::fabs(v));
                }
                frames += n / 8;
                if (derr == Error::EndOfFile) break;
                REQUIRE(derr == Error::Ok);
                REQUIRE(n > 0);
        }
        CHECK(frames == info.frames);
        CHECK(peak > 0.05f);
        // Sought to 1.5 s: F4 (349.2 Hz) starts there, alone (the drums stop at 2.5 beats). A note
        // begun before the seek isn't played (the seek replays controllers and programs only).
        REQUIRE(d->seek(72000) == Error::Ok);
        std::vector<float> e4(12000 * 2);
        auto [n, serr] = d->decode(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(e4.data()), e4.size() * 4));
        CHECK(serr == Error::Ok);
        CHECK(n == e4.size() * 4);
        CHECK(crossings(e4, 2) >= 86);
        CHECK(crossings(e4, 2) <= 89);
        // In S16 too, and sought past the end.
        CHECK(d->setFormat(bmds::SampleFormat::S16Le) == Error::Ok);
        CHECK(d->seek(info.frames + 1) == Error::OutOfRange);
        REQUIRE(d->seek(info.frames - 10) == Error::Ok);
        auto [m, eerr] = d->decode(bmds::WritableByteSpan(buf.data(), buf.size()));
        CHECK(m == 10 * 4);
        auto [z, zerr] = d->decode(bmds::WritableByteSpan(buf.data(), buf.size()));
        CHECK(z == 0);
        CHECK(zerr == Error::EndOfFile);
        (void)eerr;
        bmds::Decoder::close(d);
        // Not a MIDI file: refused.
        std::vector<uint8_t> junk = file;
        junk[20] = 0xff;
        junk.resize(30);
        In junkIn{&junk};
        CHECK(bmds::Decoder::open({readAt, &junkIn, junk.size()}).error().isError());
        // The fuzzer's finds: decoded to their end, or refused.
        size_t finds = 0;
        for (const auto &entry :
             std::filesystem::directory_iterator(std::filesystem::path(BMDS_SYNTH_DIR) / "fuzz-midi")) {
                const std::vector<uint8_t> found = readFile(entry.path());
                In                         foundIn{&found};
                auto [fd, ferr] = bmds::Decoder::open({readAt, &foundIn, found.size()});
                ++finds;
                if (ferr.isError()) continue;
                for (int i = 0; i < 64; ++i) {
                        auto [got, derr] = fd->decode(bmds::WritableByteSpan(buf.data(), buf.size()));
                        if (derr.isError()) break;
                        REQUIRE(got > 0);
                }
                bmds::Decoder::close(fd);
        }
        CHECK(finds >= 4);
        // The hooks gone with the font: a MIDI file is then NotSupported, not played on a freed font.
        bmds::MidiDecoder::setFonts(nullptr, nullptr);
        CHECK(bmds::Decoder::open({readAt, &in, file.size()}).error() == Error::NotSupported);
        font->destroy();
        font = nullptr;
}

TEST_CASE("Synth: the output's gain") {
        const std::vector<uint8_t> data = FontWriter().build();
        SoundFont                 *font = loadFont(data);
        REQUIRE(font != nullptr);
        auto peakAt = [&](float gainDb) {
                Synth         synth;
                Synth::Config c;
                c.gainDb = gainDb;
                REQUIRE(synth.open(*font, c) == Error::Ok);
                CHECK(synth.queue(message(0, {0x90, 69, 127})) == Error::Ok);
                std::vector<float> out(9600 * 2);
                synth.render(out.data(), 9600);
                float peak = 0;
                for (float v : out) peak = std::fmax(peak, std::fabs(v));
                return peak;
        };
        const float unity = peakAt(0);
        const float low = peakAt(-10);
        REQUIRE(unity > 0.01f);
        CHECK(low / unity == doctest::Approx(0.3162).epsilon(0.02)); // -10 dB
        Synth         synth;
        Synth::Config c;
        c.gainDb = Synth::MaxGainDb + 1;
        CHECK(synth.open(*font, c) == Error::Invalid);
        c.gainDb = NAN;
        CHECK(synth.open(*font, c) == Error::Invalid);
        font->destroy();
}
