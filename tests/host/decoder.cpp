/**
 * @file      decoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The decoders (devplan/audio.md §19) on the files in tests/host/audio: a
 * second of 440 Hz left and 880 Hz right, made by ffmpeg, in each format.
 */

#include <doctest.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <bmds/decodedfile.h>
#include <bmds/decoder.h>
#include <bmds/memorystream.h>
#include "hostmemory.h"

using bmds::Codec;
using bmds::Decoder;
using bmds::Error;
using bmds::SampleFormat;
using bmds::StringView;

namespace {

        std::vector<uint8_t> load(const char *name) {
                std::ifstream in(std::string(BMDS_AUDIO_DIR) + "/" + name, std::ios::binary);
                REQUIRE(in.good());
                return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        Error readVector(void *arg, uint64_t offset, bmds::WritableByteSpan buf) {
                const auto &v = *static_cast<const std::vector<uint8_t> *>(arg);
                if (offset > v.size() || v.size() - offset < buf.size()) return Error::EndOfFile;
                if (!buf.isEmpty())
                        std::memcpy(buf.data(), v.data() + offset, buf.size()); // an empty file has no data()
                return Error::Ok;
        }

        bmds::Result<Decoder *> open(const std::vector<uint8_t> &v) {
                return Decoder::open({readVector, const_cast<std::vector<uint8_t> *>(&v), v.size()});
        }

        // Everything from where @p d is, as float; the last error in @p err.
        std::vector<float> decodeAll(Decoder &d, Error &err, size_t bufBytes = 4096) {
                REQUIRE(d.setFormat(SampleFormat::F32Le).isOk());
                std::vector<float>   out;
                std::vector<uint8_t> buf(bufBytes);
                for (;;) {
                        auto [n, e] = d.decode(bmds::WritableByteSpan(buf.data(), buf.size()));
                        const auto *f = reinterpret_cast<const float *>(buf.data());
                        out.insert(out.end(), f, f + n / sizeof(float));
                        if (e.isError()) {
                                err = e;
                                return out;
                        }
                }
        }

        // Sign changes a second in channel @p ch of @p channels at @p rate, over the middle half
        // (past an encoder's silence at the start and its padding at the end).
        double crossingRate(const std::vector<float> &s, unsigned channels, unsigned ch, unsigned rate) {
                const size_t frames = s.size() / channels;
                size_t       n = 0;
                for (size_t i = frames / 4 + 1; i < frames * 3 / 4; ++i)
                        if ((s[(i - 1) * channels + ch] < 0) != (s[i * channels + ch] < 0)) ++n;
                return double(n) * rate / double(frames * 3 / 4 - frames / 4 - 1);
        }

        float peak(const std::vector<float> &s) {
                float p = 0;
                for (float x : s) p = std::max(p, std::fabs(x));
                return p;
        }

        struct File {
                        const char  *name;
                        Codec        codec;
                        unsigned     rate, channels;
                        SampleFormat format;
                        uint64_t     frames; // exact, or 0: estimated
        };
        constexpr File Files[] = {
                {"tone.wav", Codec::Wav, 44100, 2, SampleFormat::S16Le, 4410},
                {"tone.mp3", Codec::Mp3, 44100, 2, SampleFormat::F32Le, 44100},
                {"tone-noxing.mp3", Codec::Mp3, 44100, 2, SampleFormat::F32Le, 0},
                {"tone-mono22.mp3", Codec::Mp3, 22050, 1, SampleFormat::F32Le, 22050},
                {"tone.ogg", Codec::Vorbis, 44100, 2, SampleFormat::F32Le, 44100},
                {"tone.flac", Codec::Flac, 44100, 2, SampleFormat::S16Le, 44100},
                {"tone24.flac", Codec::Flac, 44100, 2, SampleFormat::S32Le, 44100},
        };

} // namespace

TEST_CASE("Decoder: each format found by its content, with its length") {
        for (const File &f : Files) {
                INFO(std::string(f.name));
                const auto v = load(f.name);
                auto [d, err] = open(v);
                REQUIRE(err.isOk());
                CHECK(d->info().codec == f.codec);
                CHECK(d->info().rateHz == f.rate);
                CHECK(d->info().channels == f.channels);
                CHECK(d->info().format == f.format);
                CHECK(d->format() == f.format);
                if (f.frames != 0) {
                        CHECK(d->info().frames == f.frames);
                        CHECK_FALSE(d->info().estimated);
                } else {
                        CHECK(d->info().estimated);
                }
                CHECK(d->info().bitrate > 0);
                Error              end = Error::Ok;
                std::vector<float> s = decodeAll(*d, end);
                CHECK(end == Error::EndOfFile);
                const uint64_t decoded = s.size() / f.channels;
                if (f.frames != 0)
                        CHECK(decoded == f.frames); // gapless: LAME's delay and padding trimmed
                else
                        CHECK(decoded >= 44100);
                CHECK(d->position() == decoded);
                Decoder::close(d);
        }
}

TEST_CASE("Decoder: what it refuses") {
        const auto opus = load("tone.opus");
        CHECK(open(opus).error() == Error::NotSupported);
        const std::vector<uint8_t> text = {'h', 'e', 'l', 'l', 'o', '\n'};
        CHECK(open(text).error() == Error::Invalid);
        const std::vector<uint8_t> empty;
        CHECK(open(empty).error() == Error::Invalid);
        CHECK(Decoder::open({}).error() == Error::Invalid);
        // A header with nothing after it.
        auto flac = load("tone.flac");
        flac.resize(4);
        CHECK(open(flac).error() == Error::Invalid);
        auto [d, err] = open(load("tone.flac"));
        REQUIRE(err.isOk());
        CHECK(d->setFormat(SampleFormat::U8) == Error::Invalid);
        uint8_t small[3];
        CHECK(d->decode(bmds::WritableByteSpan(small, sizeof(small))).error() == Error::BufferTooSmall);
        CHECK(d->seek(44101) == Error::OutOfRange);
        Decoder::close(d);
        Decoder::close(nullptr);
}

TEST_CASE("Decoder: the tones, in their channels, at their level") {
        for (const File &f : Files) {
                INFO(std::string(f.name));
                const auto v = load(f.name);
                auto [d, err] = open(v);
                REQUIRE(err.isOk());
                Error                    end;
                const std::vector<float> s = decodeAll(*d, end);
                // Two sign changes a cycle: 440 Hz on the left, 880 Hz on the right.
                CHECK(crossingRate(s, f.channels, 0, f.rate) == doctest::Approx(880).epsilon(0.01));
                if (f.channels == 2)
                        CHECK(crossingRate(s, f.channels, 1, f.rate) == doctest::Approx(1760).epsilon(0.01));
                // ffmpeg's sine is at -18 dBFS, halved: 0.0625.
                CHECK(peak(s) == doctest::Approx(0.0625).epsilon(0.1));
                Decoder::close(d);
        }
}

TEST_CASE("Decoder: FLAC is the WAV's samples exactly; 16 and 24 bits") {
        const auto wavFile = load("tone.wav");
        const auto flacFile = load("tone.flac");
        auto [w, werr] = open(wavFile);
        auto [f, ferr] = open(flacFile);
        REQUIRE(werr.isOk());
        REQUIRE(ferr.isOk());
        std::vector<int16_t> a(4410 * 2), b(4410 * 2);
        auto   na = w->decode(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(a.data()), a.size() * 2));
        size_t got = 0;
        while (got < b.size() * 2) {
                auto n = f->decode(
                        bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(b.data()) + got, b.size() * 2 - got));
                REQUIRE(n.error().isOk());
                got += n.value();
        }
        CHECK(na.value() == a.size() * 2);
        CHECK(a == b);
        Decoder::close(w);
        Decoder::close(f);
        // 24 bits as s32le: the top 24 bits, the low byte zero.
        const auto f24File = load("tone24.flac");
        auto [g, gerr] = open(f24File);
        REQUIRE(gerr.isOk());
        CHECK(g->info().sourceBits == 24);
        int32_t s[512];
        REQUIRE(g->decode(bmds::WritableByteSpan(reinterpret_cast<uint8_t *>(s), sizeof(s))).value() == sizeof(s));
        bool lowZero = true, anyBits = false;
        for (int32_t x : s) {
                lowZero = lowZero && (x & 0xff) == 0;
                anyBits = anyBits || (x & 0xff00) != 0;
        }
        CHECK(lowZero);
        CHECK(anyBits);
        Decoder::close(g);
}

TEST_CASE("Decoder: tags from ID3v2.3, ID3v2.4, ID3v1, Vorbis comments and FLAC's") {
        for (const char *name : {"tone.mp3", "tone-noxing.mp3", "tone.ogg", "tone.flac"}) {
                INFO(std::string(name));
                const auto v = load(name);
                auto [d, err] = open(v);
                REQUIRE(err.isOk());
                CHECK(d->tag("TITLE") == StringView("Tone"));
                CHECK(d->tag("ARTIST") == StringView("BMDS"));
                CHECK(d->tag("ALBUM") == StringView("Tests"));
                CHECK(d->tag("TRACKNUMBER") == StringView("3"));
                CHECK(d->tag("NOTHING").isEmpty());
                CHECK(d->tag(d->tagCount()).key.isEmpty());
                Decoder::close(d);
        }
        const auto v = load("tone-mono22.mp3");
        auto [d, err] = open(v);
        REQUIRE(err.isOk());
        CHECK(d->tag("TITLE") == StringView("Mono"));
        CHECK(d->tag("ARTIST") == StringView("BMDS \xc3\xa9t\xc3\xa9")); // Latin-1 to UTF-8
        CHECK(d->tag("DATE") == StringView("2026"));
        CHECK(d->tag("TRACKNUMBER") == StringView("7"));
        Decoder::close(d);
}

TEST_CASE("Decoder: seeks are exact: the samples a straight decode gives there") {
        for (const File &f : Files) {
                INFO(std::string(f.name));
                const auto v = load(f.name);
                auto [d, err] = open(v);
                REQUIRE(err.isOk());
                Error                    end;
                const std::vector<float> all = decodeAll(*d, end);
                const uint64_t           frames = all.size() / f.channels;
                for (uint64_t at : {uint64_t(0), uint64_t(1), uint64_t(575), uint64_t(1152), uint64_t(1153),
                                    uint64_t(4000), uint64_t(20000), frames - 300, frames}) {
                        if (at > frames) continue;
                        CAPTURE(at);
                        REQUIRE(d->seek(at).isOk());
                        CHECK(d->position() == at);
                        std::vector<float> got = decodeAll(*d, end, 2048);
                        CHECK(end == Error::EndOfFile);
                        REQUIRE(got.size() == all.size() - at * f.channels);
                        size_t differ = 0;
                        for (size_t i = 0; i < got.size(); ++i)
                                if (std::fabs(got[i] - all[at * f.channels + i]) > 1e-6f) ++differ;
                        CHECK(differ == 0);
                }
                Decoder::close(d);
        }
}

TEST_CASE("Decoder: formats asked for agree with float") {
        const auto v = load("tone.ogg");
        auto [d, err] = open(v);
        REQUIRE(err.isOk());
        Error                    end;
        const std::vector<float> f = decodeAll(*d, end);
        for (SampleFormat format : {SampleFormat::S16Le, SampleFormat::S32Le}) {
                REQUIRE(d->seek(0).isOk());
                REQUIRE(d->setFormat(format).isOk());
                CHECK(d->frameBytes() == (format == SampleFormat::S16Le ? 4u : 8u));
                std::vector<uint8_t> buf(1000 * d->frameBytes());
                REQUIRE(d->decode(bmds::WritableByteSpan(buf.data(), buf.size())).value() == buf.size());
                for (size_t i = 0; i < 2000; ++i) {
                        const double x = format == SampleFormat::S16Le
                                                 ? reinterpret_cast<const int16_t *>(buf.data())[i] / 32768.0
                                                 : reinterpret_cast<const int32_t *>(buf.data())[i] / 2147483648.0;
                        CHECK(std::fabs(x - f[i]) < 1.0 / 16384);
                }
        }
        Decoder::close(d);
}

// A sample of what the fuzzers (tests/fuzz) do at length.
TEST_CASE("Decoder: cut and damaged files are errors or noise, never more") {
        uint32_t   seed = 12345;
        const auto random = [&seed]() {
                seed = seed * 1103515245u + 12345u;
                return seed >> 8;
        };
        for (const File &f : Files) {
                INFO(std::string(f.name));
                const auto          whole = load(f.name);
                std::vector<size_t> cuts;
                for (size_t n = 0; n < 96 && n < whole.size(); ++n) cuts.push_back(n);
                for (size_t n = 96; n < whole.size(); n += whole.size() / 24) cuts.push_back(n);
                for (size_t n : cuts) {
                        std::vector<uint8_t> v(whole.begin(), whole.begin() + static_cast<ptrdiff_t>(n));
                        auto [d, err] = open(v);
                        if (err.isError()) continue;
                        Error end;
                        (void)decodeAll(*d, end);
                        CHECK(end != Error::Ok);
                        Decoder::close(d);
                }
                for (int i = 0; i < 24; ++i) {
                        std::vector<uint8_t> v = whole;
                        for (int k = 0; k < 4; ++k)
                                v[random() % v.size()] ^= static_cast<uint8_t>(1u << (random() % 8));
                        auto [d, err] = open(v);
                        if (err.isError()) continue;
                        Error end;
                        (void)decodeAll(*d, end);
                        (void)d->seek(d->info().frames != Decoder::UnknownFrames ? d->info().frames / 2 : 1000);
                        (void)decodeAll(*d, end);
                        Decoder::close(d);
                }
        }
}

TEST_CASE("Decoder: each allocation failing is NoMem, and nothing is left") {
        for (const File &f : Files) {
                INFO(std::string(f.name));
                const auto   v = load(f.name);
                const size_t before = HostMemory::liveAllocations();
                for (long k = 0;; ++k) {
                        CAPTURE(k);
                        Decoder *d = nullptr;
                        {
                                HostMemory::FailGuard fail(k);
                                auto                  opened = open(v);
                                if (opened.error().isError()) {
                                        CHECK(opened.error() == Error::NoMem);
                                        CHECK(HostMemory::liveAllocations() == before);
                                        continue;
                                }
                                d = opened.value();
                                // Converting and decoding with allocations failing: an error or less, never a fault.
                                if (d->setFormat(SampleFormat::S16Le).isError()) CHECK(d->format() == d->info().format);
                                std::vector<uint8_t> buf(4096);
                                while (d->decode(bmds::WritableByteSpan(buf.data(), buf.size())).error().isOk()) {}
                        }
                        Decoder::close(d);
                        CHECK(HostMemory::liveAllocations() == before);
                        break;
                }
        }
}

TEST_CASE("Decoder: what the fuzzers found (tests/host/audio/fuzz) stays fixed") {
        size_t files = 0;
        for (const auto &entry : std::filesystem::directory_iterator(std::string(BMDS_AUDIO_DIR) + "/fuzz")) {
                const std::string name = "fuzz/" + entry.path().filename().string();
                INFO(name);
                const auto v = load(name.c_str());
                ++files;
                auto [d, err] = open(v);
                if (err.isError()) continue;
                Error end;
                (void)decodeAll(*d, end);
                (void)d->seek(d->info().frames != Decoder::UnknownFrames ? d->info().frames / 3 : 1000);
                (void)decodeAll(*d, end);
                Decoder::close(d);
        }
        CHECK(files >= 3);
}

namespace {

        // A seekable file in memory, as the VFS's are.
        class VectorStream final : public bmds::Stream {
                public:
                        explicit VectorStream(std::vector<uint8_t> data) : _data(std::move(data)) {}
                        ~VectorStream() = default;

                        bmds::Result<size_t> read(bmds::WritableByteSpan buf, unsigned int) override {
                                if (_pos >= _data.size()) return bmds::Result<size_t>(0, Error::EndOfFile);
                                const size_t n = std::min(buf.size(), _data.size() - static_cast<size_t>(_pos));
                                std::memcpy(buf.data(), _data.data() + _pos, n);
                                _pos += n;
                                return bmds::makeResult(n);
                        }
                        bmds::Result<size_t> write(bmds::ByteSpan, unsigned int) override {
                                return bmds::makeError<size_t>(Error::NotSupported);
                        }
                        bmds::Result<uint64_t> seek(int64_t offset, SeekFrom from) override {
                                const int64_t base = from == SeekFrom::Start     ? 0
                                                     : from == SeekFrom::Current ? int64_t(_pos)
                                                                                 : int64_t(_data.size());
                                if (base + offset < 0) return bmds::makeError<uint64_t>(Error::Invalid);
                                _pos = uint64_t(base + offset);
                                return bmds::makeResult(_pos);
                        }

                private:
                        std::vector<uint8_t> _data;
                        uint64_t             _pos = 0;
        };

        std::vector<uint8_t> readAll(bmds::FileHandle &h, size_t piece) {
                std::vector<uint8_t> out, buf(piece);
                for (;;) {
                        auto [n, err] = h.read(bmds::WritableByteSpan(buf.data(), buf.size()), bmds::WaitForever);
                        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<ptrdiff_t>(n));
                        if (err.isError()) {
                                CHECK(err == Error::EndOfFile);
                                return out;
                        }
                }
        }

} // namespace

TEST_CASE("DecodedFile: a sound file read as PCM through a handle, in any pieces, and sought") {
        const size_t before = HostMemory::liveAllocations();
        {
                VectorStream file(load("tone.flac"));
                auto [src, serr] = bmds::FileHandle::open(file, bmds::FileHandle::Read);
                REQUIRE(serr.isOk());
                auto [h, err] = bmds::DecodedFile::open(src, SampleFormat::S16Le, false);
                REQUIRE(err.isOk());
                src.reset(); // the decoded file keeps its own reference
                REQUIRE(h->stream().decodedFile() != nullptr);
                CHECK(h->stream().audioFile() == nullptr);
                CHECK(h->stream().decodedFile()->decoder().info().codec == Codec::Flac);
                CHECK_FALSE(h->canWrite());
                CHECK(h->stream().seek(0, bmds::Stream::SeekFrom::End).value() == 44100u * 4);
                REQUIRE(h->stream().seek(0, bmds::Stream::SeekFrom::Start).isOk());
                const std::vector<uint8_t> whole = readAll(*h, 4096);
                CHECK(whole.size() == 44100u * 4);
                REQUIRE(h->stream().seek(0, bmds::Stream::SeekFrom::Start).isOk());
                CHECK(readAll(*h, 3) == whole); // pieces smaller than a frame
                // Into the middle of a frame, and from there.
                CHECK(h->stream().seek(1001, bmds::Stream::SeekFrom::Start).value() == 1001u);
                CHECK(h->stream().seek(0, bmds::Stream::SeekFrom::Current).value() == 1001u);
                const std::vector<uint8_t> rest = readAll(*h, 1000);
                CHECK(std::vector<uint8_t>(whole.begin() + 1001, whole.end()) == rest);
                CHECK(h->stream().seek(-4, bmds::Stream::SeekFrom::End).value() == whole.size() - 4);
                CHECK(readAll(*h, 64) == std::vector<uint8_t>(whole.end() - 4, whole.end()));
                CHECK(h->stream().seek(1, bmds::Stream::SeekFrom::End).error() == Error::Invalid);
                CHECK(h->stream().seek(-1, bmds::Stream::SeekFrom::Start).error() == Error::Invalid);
                uint8_t b = 0;
                CHECK(h->write(bmds::ByteSpan(&b, 1), bmds::WaitForever).error() == Error::BadDescriptor);
        }
        {
                // From a copy of a buffer, in the decoder's own format.
                std::vector<uint8_t> mp3 = load("tone.mp3");
                auto [h, err] =
                        bmds::DecodedFile::open(bmds::ByteSpan(mp3.data(), mp3.size()), SampleFormat::S16Le, true);
                REQUIRE(err.isOk());
                std::fill(mp3.begin(), mp3.end(), 0); // the caller's buffer is free at once
                CHECK(h->stream().decodedFile()->decoder().format() == SampleFormat::F32Le);
                CHECK(readAll(*h, 5000).size() == 44100u * 8);
        }
        {
                // What it refuses.
                auto [n, nerr] = bmds::FileHandle::open(bmds::NullStream::instance(), bmds::FileHandle::Read);
                REQUIRE(nerr.isOk());
                CHECK(bmds::DecodedFile::open(n, SampleFormat::S16Le, false).error() == Error::NotSupported);
                const std::vector<uint8_t> text = {'n', 'o', 'p', 'e'};
                CHECK(bmds::DecodedFile::open(bmds::ByteSpan(text.data(), text.size()), SampleFormat::S16Le, false)
                              .error() == Error::Invalid);
                const std::vector<uint8_t> flac = load("tone.flac");
                CHECK(bmds::DecodedFile::open(bmds::ByteSpan(flac.data(), flac.size()), SampleFormat::U8, false)
                              .error() == Error::Invalid);
                CHECK(bmds::DecodedFile::open(bmds::FileHandle::Ptr(), SampleFormat::S16Le, false).error() ==
                      Error::BadDescriptor);
        }
        CHECK(HostMemory::liveAllocations() == before);
}
