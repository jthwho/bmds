/**
 * @file      decoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Sound files into PCM (devplan/audio.md §19): the format found from the
 * first bytes, then what every decoder shares (the read cache, the tags, the
 * conversion to the format asked for).
 */

#include <cstring>
#include "decoders.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t ProbeBytes = 64;
        constexpr size_t MaxTags = 64;

        bool startsWith(const uint8_t *p, size_t n, const char *text) {
                const size_t len = std::strlen(text);
                return n >= len && std::memcmp(p, text, len) == 0;
        }

        // An Ogg stream's first packet, in its first page at @p p: Vorbis, or another codec.
        Result<Decoder *> openOgg(const Decoder::Source &source, const uint8_t *p, size_t n) {
                if (n < 27) return makeError<Decoder *>(Error::Invalid);
                const size_t first = 27 + p[26];
                if (first + 7 <= n && startsWith(p + first, n - first, "\x01vorbis"))
                        return Decoders::openVorbis(source);
                // Opus, FLAC, Speex, Theora...: Ogg, but nothing this plays.
                return makeError<Decoder *>(first < n ? Error::NotSupported : Error::Invalid);
        }

} // namespace

// ---- Opening

Result<Decoder *> Decoder::open(const Source &source) {
        if (source.read == nullptr) return makeError<Decoder *>(Error::Invalid);
        uint8_t      p[ProbeBytes] = {};
        const size_t n = source.size < ProbeBytes ? static_cast<size_t>(source.size) : ProbeBytes;
        Error        err = source.read(source.arg, 0, WritableByteSpan(p, n));
        if (err.isError()) return makeError<Decoder *>(err == Error::EndOfFile ? Error::Invalid : err);
        if (startsWith(p, n, "RIFF") && n >= 12 && std::memcmp(p + 8, "WAVE", 4) == 0) return Decoders::openWav(source);
        if (startsWith(p, n, "OggS")) return openOgg(source, p, n);
        if (startsWith(p, n, "fLaC")) return Decoders::openFlac(source, 0);
        const uint64_t tag = n >= 10 ? Decoders::id3v2Bytes(p) : 0;
        if (tag != 0) {
                // An ID3v2 tag is mostly before MP3, sometimes before FLAC.
                uint8_t after[4] = {};
                if (tag + sizeof(after) <= source.size &&
                    source.read(source.arg, tag, WritableByteSpan(after, sizeof(after))).isOk() &&
                    std::memcmp(after, "fLaC", 4) == 0)
                        return Decoders::openFlac(source, tag);
                return Decoders::openMp3(source, tag);
        }
        // Bare MPEG audio: a header, and another where the first frame ends.
        if (n >= Decoders::MpegHeader::Bytes && Decoders::MpegHeader::isValid(p)) {
                const size_t bytes = Decoders::MpegHeader::frameBytes(p);
                uint8_t      next[Decoders::MpegHeader::Bytes] = {};
                if (bytes != 0 && bytes + sizeof(next) <= source.size &&
                    source.read(source.arg, bytes, WritableByteSpan(next, sizeof(next))).isOk() &&
                    Decoders::MpegHeader::isValid(next) && Decoders::MpegHeader::isSameStream(p, next))
                        return Decoders::openMp3(source, 0);
        }
        return makeError<Decoder *>(Error::Invalid);
}

void Decoder::close(Decoder *decoder) {
        if (decoder == nullptr) return;
        decoder->~Decoder();
        Memory::free(decoder);
        return;
}

Decoder::~Decoder() {
        Memory::free(_cache);
        Memory::free(_native);
        Memory::free(_floats);
        Memory::free(_tags);
}

const char *Decoder::name(Codec codec) {
        switch (codec) {
                case Codec::Wav: return "wav";
                case Codec::Mp3: return "mp3";
                case Codec::Vorbis: return "vorbis";
                case Codec::Flac: return "flac";
        }
        return "?";
}

Error Decoder::finishOpen() {
        if (_info.channels < 1 || _info.channels > MaxChannels || _info.rateHz == 0) return Error::NotSupported;
        _format = _info.format;
        return Error::Ok;
}

// ---- Decoding

Error Decoder::setFormat(SampleFormat format) {
        if (format != _info.format && format != SampleFormat::S16Le && format != SampleFormat::S32Le &&
            format != SampleFormat::F32Le)
                return Error::Invalid;
        if (format != _info.format) {
                if (_native == nullptr)
                        _native = static_cast<uint8_t *>(
                                Memory::alloc(MemSpace::Psram,
                                              ChunkFrames * MaxChannels * SampleFormats::bytesPerSample(_info.format)));
                if (_floats == nullptr)
                        _floats = static_cast<float *>(Memory::alloc(MemSpace::Psram, ChunkFrames * 2 * sizeof(float)));
                if (_native == nullptr || _floats == nullptr) return Error::NoMem;
        }
        _format = format;
        return Error::Ok;
}

Result<size_t> Decoder::decode(WritableByteSpan out) {
        const size_t bytes = frameBytes();
        size_t       frames = out.size() / bytes;
        if (frames == 0) return makeError<size_t>(Error::BufferTooSmall);
        if (_format == _info.format) {
                auto [n, err] = decodeFrames(out.data(), frames);
                _position += n;
                return Result<size_t>(n * bytes, err);
        }
        // Another format: through float, a chunk at a time.
        size_t done = 0;
        while (done < frames) {
                const size_t want = frames - done < ChunkFrames ? frames - done : ChunkFrames;
                auto [n, err] = decodeFrames(_native, want);
                if (n > 0) {
                        SampleFormats::toFloat(_info.format, _info.channels, _native, n, _floats);
                        SampleFormats::fromFloat(_format, _info.channels, _floats, n, out.data() + done * bytes);
                        done += n;
                        _position += n;
                }
                if (err.isError()) {
                        if (done > 0 && err == Error::EndOfFile) break; // the end, next time
                        return Result<size_t>(done * bytes, err);
                }
                if (n < want) break; // what was there; more next time
        }
        return makeResult(done * bytes);
}

Error Decoder::seek(uint64_t frame) {
        if (_info.frames != UnknownFrames && !_info.estimated && frame > _info.frames) return Error::OutOfRange;
        const Error err = seekFrame(frame);
        if (err.isOk()) _position = frame;
        return err;
}

// ---- Reading the file

Error Decoder::read(uint64_t offset, WritableByteSpan buf) {
        if (offset > _source.size || buf.size() > _source.size - offset) return Error::EndOfFile;
        if (buf.size() >= CacheBytes) return _source.read(_source.arg, offset, buf);
        if (_cache == nullptr) {
                _cache = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, CacheBytes));
                if (_cache == nullptr) return Error::NoMem;
        }
        if (offset < _cacheOffset || offset + buf.size() > _cacheOffset + _cacheBytes) {
                const uint64_t left = _source.size - offset;
                const size_t   n = left < CacheBytes ? static_cast<size_t>(left) : CacheBytes;
                _cacheBytes = 0;
                const Error err = _source.read(_source.arg, offset, WritableByteSpan(_cache, n));
                if (err.isError()) return err;
                _cacheOffset = offset;
                _cacheBytes = n;
        }
        std::memcpy(buf.data(), _cache + (offset - _cacheOffset), buf.size());
        return Error::Ok;
}

// ---- Tags

void Decoder::addTag(StringView key, StringView value) {
        if (key.isEmpty() || value.isEmpty() || _tagCount >= MaxTags) return;
        const size_t need = key.size() + value.size() + 2;
        if (need > TagBytes - _tagBytes) return;
        for (char c : key)
                if (c < 0x20 || c > 0x7d || c == '=') return; // Vorbis comment names: printable ASCII, no '='
        if (_tags == nullptr) {
                _tags = static_cast<char *>(Memory::alloc(MemSpace::Psram, TagBytes));
                if (_tags == nullptr) return;
        }
        char *p = _tags + _tagBytes;
        for (char c : key) *p++ = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
        *p++ = '\0';
        std::memcpy(p, value.data(), value.size());
        p[value.size()] = '\0';
        _tagBytes += need;
        ++_tagCount;
        return;
}

void Decoder::addComment(StringView comment) {
        const size_t eq = comment.indexOf('=');
        if (eq == StringView::NPos) return;
        addTag(comment.left(eq), comment.mid(eq + 1));
        return;
}

Decoder::Tag Decoder::tag(size_t index) const {
        if (index >= _tagCount) return {};
        const char *p = _tags;
        for (size_t i = 0; i < index; ++i) {
                p += std::strlen(p) + 1;
                p += std::strlen(p) + 1;
        }
        const char *value = p + std::strlen(p) + 1;
        return {StringView(p), StringView(value)};
}

StringView Decoder::tag(StringView key) const {
        for (size_t i = 0; i < _tagCount; ++i) {
                const Tag t = tag(i);
                if (t.key == key) return t.value;
        }
        return {};
}

// ---- Shared by the formats

uint64_t Decoders::id3v2Bytes(const uint8_t *h) {
        if (std::memcmp(h, "ID3", 3) != 0 || h[3] == 0xff || h[4] == 0xff) return 0;
        if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0; // not syncsafe
        const uint64_t size = static_cast<uint64_t>(h[6]) << 21 | static_cast<uint64_t>(h[7]) << 14 |
                              static_cast<uint64_t>(h[8]) << 7 | h[9];
        return 10 + size + ((h[5] & 0x10) ? 10 : 0); // a footer
}

namespace {

        // Bitrates in kb/s: [MPEG-1 or not][layer - 1][index].
        constexpr uint16_t Bitrates[2][3][15] = {
                {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
                 {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
                 {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
                {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
                 {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
                 {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
        };
        constexpr uint16_t Rates[3] = {44100, 48000, 32000};

        unsigned version(const uint8_t *h) {
                return (h[1] >> 3) & 3;
        } // 0: 2.5, 2: 2, 3: 1
        bool isMpeg1(const uint8_t *h) {
                return version(h) == 3;
        }

} // namespace

bool Decoders::MpegHeader::isValid(const uint8_t *h) {
        return h[0] == 0xff && (h[1] & 0xe0) == 0xe0 && version(h) != 1 && ((h[1] >> 1) & 3) != 0 &&
               (h[2] >> 4) != 15 && ((h[2] >> 2) & 3) != 3;
}

bool Decoders::MpegHeader::isSameStream(const uint8_t *a, const uint8_t *b) {
        // Version, layer, and rate; not the bitrate, padding or channels' mode.
        return (a[1] & 0xfe) == (b[1] & 0xfe) && (a[2] & 0x0c) == (b[2] & 0x0c);
}

unsigned Decoders::MpegHeader::layer(const uint8_t *h) {
        return 4 - ((h[1] >> 1) & 3);
}

unsigned Decoders::MpegHeader::rateHz(const uint8_t *h) {
        const unsigned shift = version(h) == 3 ? 0 : version(h) == 2 ? 1 : 2;
        return Rates[(h[2] >> 2) & 3] >> shift;
}

unsigned Decoders::MpegHeader::channels(const uint8_t *h) {
        return (h[3] >> 6) == 3 ? 1 : 2;
}

unsigned Decoders::MpegHeader::bitrateKbps(const uint8_t *h) {
        return Bitrates[isMpeg1(h) ? 1 : 0][layer(h) - 1][h[2] >> 4];
}

unsigned Decoders::MpegHeader::samples(const uint8_t *h) {
        if (layer(h) == 1) return 384;
        return layer(h) == 3 && !isMpeg1(h) ? 576 : 1152;
}

size_t Decoders::MpegHeader::frameBytes(const uint8_t *h) {
        const unsigned kbps = bitrateKbps(h);
        if (kbps == 0) return 0;
        const unsigned padding = (h[2] >> 1) & 1;
        if (layer(h) == 1) return (12 * kbps * 1000 / rateHz(h) + padding) * 4;
        return samples(h) / 8 * kbps * 1000 / rateHz(h) + padding;
}

size_t Decoders::MpegHeader::sideInfoBytes(const uint8_t *h) {
        const size_t crc = (h[1] & 1) ? 0 : 2;
        if (isMpeg1(h)) return crc + (channels(h) == 1 ? 17 : 32);
        return crc + (channels(h) == 1 ? 9 : 17);
}

BMDS_NAMESPACE_END
