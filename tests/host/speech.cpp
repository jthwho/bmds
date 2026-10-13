/**
 * @file      speech.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Text to speech (devplan/speech.md): the speaker on the en-US voice, its
 * files mapped read-only at odd addresses (every byte read in place, a write
 * a fault, as from /rom): speech made, the same each time; markup obeyed or
 * read; speed; UTF-8 split; NUL; two speakers; damaged voices.
 */

#include <doctest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <bmds/speaker.h>

using bmds::ByteSpan;
using bmds::Error;
using bmds::Speaker;

namespace {

        // A file's bytes in pages of their own, read-only, starting @p skew bytes into the first.
        class Mapped {
                public:
                        Mapped(const std::string &path, size_t skew) {
                                FILE *f = std::fopen(path.c_str(), "rb");
                                REQUIRE_MESSAGE(f != nullptr, path);
                                std::fseek(f, 0, SEEK_END);
                                _size = static_cast<size_t>(std::ftell(f));
                                std::rewind(f);
                                _total = (_size + skew + 4095) & ~size_t(4095);
                                void *m = mmap(nullptr, _total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                                               0);
                                REQUIRE(m != MAP_FAILED);
                                _map = static_cast<uint8_t *>(m);
                                REQUIRE(std::fread(_map + skew, 1, _size, f) == _size);
                                std::fclose(f);
                                REQUIRE(mprotect(_map, _total, PROT_READ) == 0);
                                _data = _map + skew;
                        }
                        Mapped(const Mapped &) = delete;
                        Mapped &operator=(const Mapped &) = delete;
                        ~Mapped() { munmap(_map, _total); }

                        ByteSpan span() const { return ByteSpan(_data, _size); }
                        ByteSpan span(size_t size) const { return ByteSpan(_data, size); }

                private:
                        uint8_t *_map = nullptr;
                        uint8_t *_data = nullptr;
                        size_t   _size = 0;
                        size_t   _total = 0;
        };

        std::string voiceFile(const char *name) {
                return std::string(BMDS_SPEECH_DIR) + "/" + name;
        }

        struct EnUs {
                        Mapped         analysis{voiceFile("en-US_ta.bin"), 1};
                        Mapped         signal{voiceFile("en-US_lh0_sg.bin"), 3};
                        Speaker::Voice voice() const { return {analysis.span(), signal.span()}; }
        };

        ByteSpan bytes(const std::string &s) {
                return ByteSpan(reinterpret_cast<const uint8_t *>(s.data()), s.size());
        }

        // Puts all of @p text, rendering whenever Pico's input is full; the samples made.
        std::vector<int16_t> say(Speaker &speaker, const std::string &text) {
                std::vector<int16_t> out;
                int16_t              block[300];
                size_t               at = 0;
                for (;;) {
                        if (at < text.size()) {
                                auto [n, err] = speaker.put(bytes(text.substr(at)));
                                REQUIRE(err.isOk());
                                at += n;
                        }
                        auto [made, rerr] = speaker.render(bmds::Span<int16_t>(block, 300));
                        REQUIRE(rerr.isOk());
                        out.insert(out.end(), block, block + made);
                        if (made == 0 && at == text.size()) return out;
                }
        }

        // Says @p text to its end (a NUL after it).
        std::vector<int16_t> sayOnce(const EnUs &voice, const std::string &text, bool markup = false,
                                     Speaker::Prosody prosody = {}) {
                Speaker speaker;
                REQUIRE(speaker.open(voice.voice(), markup, prosody).isOk());
                return say(speaker, text + std::string(1, '\0'));
        }

} // namespace

TEST_CASE("speech: a sentence, the same each time") {
        EnUs                       voice;
        const std::vector<int16_t> a = sayOnce(voice, "Hello, this is BMDS speaking.");
        // About two seconds at 16 kHz, and not silence.
        CHECK(a.size() > Speaker::RateHz);
        CHECK(a.size() < 4 * Speaker::RateHz);
        int peak = 0;
        for (int16_t s : a) peak = s > peak ? s : (-s > peak ? -s : peak);
        CHECK(peak > 1000);
        CHECK(sayOnce(voice, "Hello, this is BMDS speaking.") == a);
}

TEST_CASE("speech: the pool") {
        EnUs    voice;
        Speaker speaker;
        CHECK(speaker.poolUsed() == 0);
        REQUIRE(speaker.open(voice.voice(), false, {}).isOk());
        CHECK(speaker.poolUsed() > 1000000);
        CHECK(speaker.poolUsed() < Speaker::PoolBytes);
        CHECK(speaker.open(voice.voice(), false, {}) == Error::InvalidState);
        speaker.close();
        CHECK_FALSE(speaker.isOpen());
        CHECK(speaker.poolUsed() == 0);
}

TEST_CASE("speech: markup obeyed, or read as text; speed") {
        EnUs              voice;
        const std::string tagged = "<speed level='300'>Fast words here.";
        const size_t      plain = sayOnce(voice, "Fast words here.").size();
        const size_t      obeyed = sayOnce(voice, tagged, true).size();
        const size_t      read = sayOnce(voice, tagged, false).size();
        CHECK(obeyed < plain * 2 / 3);
        CHECK(read > plain * 2);
        // The engine's speed is the tag's.
        Speaker::Prosody fast;
        fast.speed = 300;
        CHECK(sayOnce(voice, "Fast words here.", false, fast).size() == obeyed);
        // Pitch and volume change the samples, not the length.
        Speaker::Prosody high;
        high.pitch = 150;
        high.volume = 50;
        const std::vector<int16_t> changed = sayOnce(voice, "Fast words here.", false, high);
        CHECK(changed != sayOnce(voice, "Fast words here."));
        // At the lowest pitch, still below full scale (its peaks rise; the volume follows).
        Speaker::Prosody low;
        low.pitch = Speaker::MinPitch;
        int lowPeak = 0;
        for (int16_t s : sayOnce(voice, "It was the best of times, it was the worst of times.", false, low))
                lowPeak = s > lowPeak ? s : (-s > lowPeak ? -s : lowPeak);
        CHECK(lowPeak > 8000);
        CHECK(lowPeak < 30000);
        // Out of range.
        Speaker          speaker;
        Speaker::Prosody bad;
        bad.speed = 10;
        CHECK(speaker.open(voice.voice(), false, bad) == Error::Invalid);
        CHECK_FALSE(speaker.isOpen());
        REQUIRE(speaker.open(voice.voice(), false, {}).isOk());
        bad = {};
        bad.pitch = 201;
        CHECK(speaker.setProsody(bad) == Error::Invalid);
        bad = {};
        bad.volume = 0;
        CHECK(speaker.setProsody(bad) == Error::Invalid);
}

TEST_CASE("speech: UTF-8 split between puts") {
        EnUs                       voice;
        const std::string          text = "Caf\xc3\xa9 cr\xc3\xa8me, na\xc3\xafve.";
        const std::vector<int16_t> whole = sayOnce(voice, text);
        Speaker                    speaker;
        REQUIRE(speaker.open(voice.voice(), false, {}).isOk());
        for (char c : text) REQUIRE(speaker.put(ByteSpan(reinterpret_cast<const uint8_t *>(&c), 1)).value() == 1);
        const std::vector<int16_t> split = say(speaker, std::string(1, '\0'));
        CHECK(split.size() == whole.size());
        CHECK(split == whole);
}

TEST_CASE("speech: the last text waits for more, or a NUL") {
        EnUs    voice;
        Speaker speaker;
        REQUIRE(speaker.open(voice.voice(), false, {}).isOk());
        // A sentence's end is said once text follows it; the rest waits.
        const std::vector<int16_t> first = say(speaker, "The first sentence. And the next");
        CHECK(first.size() > Speaker::RateHz / 2);
        CHECK(say(speaker, "Hello there").empty());
        const std::vector<int16_t> spoken = say(speaker, std::string(1, '\0'));
        CHECK(spoken.size() > Speaker::RateHz / 2);
        // Said once.
        CHECK(say(speaker, "").empty());
}

TEST_CASE("speech: reset drops what wasn't said") {
        EnUs    voice;
        Speaker speaker;
        REQUIRE(speaker.open(voice.voice(), false, {}).isOk());
        REQUIRE(speaker.put(bytes("This sentence is dropped. So is this one.")).isOk());
        int16_t block[64];
        REQUIRE(speaker.render(bmds::Span<int16_t>(block, 64)).value() == 64);
        speaker.reset();
        CHECK(say(speaker, "").empty());
        // And it speaks again after.
        CHECK(say(speaker, std::string("Again.") + '\0').size() > Speaker::RateHz / 4);
}

TEST_CASE("speech: two speakers at once") {
        EnUs                       voice;
        const std::vector<int16_t> one = sayOnce(voice, "The first speaker.");
        const std::vector<int16_t> two = sayOnce(voice, "And now the second one.");
        Speaker                    a, b;
        REQUIRE(a.open(voice.voice(), false, {}).isOk());
        REQUIRE(b.open(voice.voice(), false, {}).isOk());
        REQUIRE(a.put(bytes(std::string("The first speaker.") + '\0')).isOk());
        REQUIRE(b.put(bytes(std::string("And now the second one.") + '\0')).isOk());
        std::vector<int16_t> gotA, gotB;
        int16_t              block[100];
        for (bool more = true; more;) {
                const size_t na = a.render(bmds::Span<int16_t>(block, 100)).value();
                gotA.insert(gotA.end(), block, block + na);
                const size_t nb = b.render(bmds::Span<int16_t>(block, 100)).value();
                gotB.insert(gotB.end(), block, block + nb);
                more = na != 0 || nb != 0;
        }
        CHECK(gotA.size() == one.size());
        CHECK(gotA == one);
        CHECK(gotB.size() == two.size());
        CHECK(gotB == two);
}

TEST_CASE("speech: damaged voices") {
        EnUs    voice;
        Speaker speaker;
        // Its files either way round make the voice (its knowledge, all together).
        REQUIRE(speaker.open({voice.signal.span(), voice.analysis.span()}, false, {}).isOk());
        speaker.close();
        // One twice: no voice.
        CHECK(speaker.open({voice.analysis.span(), voice.analysis.span()}, false, {}) == Error::CorruptData);
        // Two languages' text analysis, no signal generation.
        {
                Mapped other(voiceFile("en-GB_ta.bin"), 0);
                CHECK(speaker.open({voice.analysis.span(), other.span()}, false, {}) == Error::NotSupported);
        }
        // Two languages' halves: a voice, whatever it sounds like.
        {
                Mapped other(voiceFile("de-DE_gl0_sg.bin"), 0);
                REQUIRE(speaker.open({voice.analysis.span(), other.span()}, false, {}).isOk());
                CHECK(say(speaker, std::string("Guten Tag, hello.") + '\0').size() > Speaker::RateHz / 4);
                speaker.close();
        }
        // Cut short, anywhere in the header.
        for (size_t size : {size_t(0), size_t(5), size_t(13), size_t(15), size_t(100), size_t(140)})
                CHECK(speaker.open({voice.analysis.span(size), voice.signal.span()}, false, {}) == Error::CorruptData);
        // Not a voice.
        const std::vector<uint8_t> junk(4096, 0x5a);
        CHECK(speaker.open({ByteSpan(junk.data(), junk.size()), voice.signal.span()}, false, {}) == Error::CorruptData);
        CHECK_FALSE(speaker.isOpen());
        // And then a good one.
        CHECK(speaker.open(voice.voice(), false, {}).isOk());
}

TEST_CASE("speech: the volume Pico gets") {
        // The loudest voice's level at the measured pitches (devplan/speech.md §13), between them
        // interpolated, beyond them held; a voice's boost and the volume on top.
        auto at = [](uint16_t pitch, uint16_t volume = 100, float boost = 0) {
                Speaker::Prosody p;
                p.pitch = pitch;
                p.volume = volume;
                return Speaker::picoVolume(p, boost);
        };
        CHECK(at(100) == 41);
        CHECK(at(50) == 29);
        CHECK(at(200) == 54);
        CHECK(at(55) == 30);
        CHECK(at(10) == at(50));
        CHECK(at(300) == at(200));
        for (uint16_t p = 51; p <= 200; ++p) CHECK(at(p) >= at(p - 1));
        CHECK(at(100, 50) == 21);
        CHECK(at(100, 1) == 1);
        CHECK(at(200, 500, 3.7f) == 412);
        CHECK(at(100, 100, 3.1f) == 59);
}

TEST_CASE("speech: not open") {
        Speaker speaker;
        int16_t block[4];
        CHECK(speaker.put(bytes("x")).error() == Error::InvalidState);
        CHECK(speaker.render(bmds::Span<int16_t>(block, 4)).error() == Error::InvalidState);
        CHECK(speaker.setProsody({}) == Error::InvalidState);
        speaker.reset();
        speaker.close();
}
