/**
 * @file      mp3decoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * MPEG audio through `Decoder` (devplan/audio.md §19), on minimp3
 * (thirdparty/minimp3). minimp3 decodes one frame from a buffer; this file
 * feeds it, and adds what a file needs: ID3 tags, the length and LAME's
 * gapless trim from a Xing, Info or VBRI frame, and exact seeks through an
 * index of frame offsets built as frames go by.
 */

#include <cstring>
#include <minimp3.h>
#include "decoders.h"

BMDS_NAMESPACE_BEGIN

namespace {

        namespace Mpeg = Decoders::MpegHeader;

        constexpr size_t   InBytes = 16384;         // the input buffer: several frames, for minimp3's sync
        constexpr size_t   MinInBytes = 4096;       // refilled below this: more than a frame
        constexpr uint64_t SyncSearchBytes = 65536; // to the first frame, past junk
        constexpr unsigned IndexEvery = 16;         // frames between index entries
        constexpr unsigned PredecodeFrames = 2;     // decoded and dropped before a seek's frame
        constexpr size_t   ReservoirBytes = 511;    // layer III's bit reservoir at most
        constexpr size_t   MaxTextBytes = 1024;     // an ID3 text frame read at most

        // ---- UTF-8 from ID3's text encodings

        class Utf8 {
                public:
                        Utf8(char *buf, size_t size) : _buf(buf), _size(size) {}

                        void put(uint32_t c) {
                                char   t[4];
                                size_t n = 0;
                                if (c < 0x80) {
                                        t[n++] = static_cast<char>(c);
                                } else if (c < 0x800) {
                                        t[n++] = static_cast<char>(0xc0 | c >> 6);
                                        t[n++] = static_cast<char>(0x80 | (c & 0x3f));
                                } else if (c < 0x10000) {
                                        t[n++] = static_cast<char>(0xe0 | c >> 12);
                                        t[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                                        t[n++] = static_cast<char>(0x80 | (c & 0x3f));
                                } else {
                                        t[n++] = static_cast<char>(0xf0 | c >> 18);
                                        t[n++] = static_cast<char>(0x80 | ((c >> 12) & 0x3f));
                                        t[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3f));
                                        t[n++] = static_cast<char>(0x80 | (c & 0x3f));
                                }
                                if (n > _size - _len) {
                                        _size = _len; // full: nothing more, not half a character
                                        return;
                                }
                                std::memcpy(_buf + _len, t, n);
                                _len += n;
                                return;
                        }

                        // The text without spaces or NULs at its end.
                        StringView text() const {
                                size_t n = _len;
                                while (n > 0 && (_buf[n - 1] == ' ' || _buf[n - 1] == '\0')) --n;
                                return StringView(_buf, n);
                        }

                private:
                        char  *_buf;
                        size_t _size;
                        size_t _len = 0;
        };

        // The first string of an ID3 text frame's data @p p (its encoding byte first).
        StringView id3Text(const uint8_t *p, size_t n, char *buf, size_t size) {
                Utf8 out(buf, size);
                if (n < 1) return {};
                const uint8_t enc = p[0];
                ++p;
                --n;
                if (enc == 0 || enc == 3) { // Latin-1, UTF-8
                        for (size_t i = 0; i < n && p[i] != 0; ++i) {
                                if (enc == 0) {
                                        out.put(p[i]);
                                } else {
                                        // UTF-8 as it is; a broken sequence's bytes become U+FFFD.
                                        const uint8_t c = p[i];
                                        size_t        more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
                                        if (c >= 0x80 && more == 0) {
                                                out.put(0xfffd);
                                                continue;
                                        }
                                        uint32_t cp = more == 0 ? c : c & (0x3f >> more);
                                        size_t   j = 1;
                                        for (; j <= more && i + j < n && (p[i + j] & 0xc0) == 0x80; ++j)
                                                cp = cp << 6 | (p[i + j] & 0x3f);
                                        if (j <= more || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
                                                out.put(0xfffd);
                                                i += j - 1;
                                                continue;
                                        }
                                        out.put(cp);
                                        i += more;
                                }
                        }
                } else if (enc == 1 || enc == 2) { // UTF-16 with a BOM, UTF-16BE
                        bool   big = enc == 2;
                        size_t i = 0;
                        if (enc == 1 && n >= 2) {
                                if (p[0] == 0xfe && p[1] == 0xff)
                                        big = true, i = 2;
                                else if (p[0] == 0xff && p[1] == 0xfe)
                                        big = false, i = 2;
                        }
                        for (; i + 1 < n; i += 2) {
                                uint32_t c = big ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
                                if (c == 0) break;
                                if (c >= 0xd800 && c <= 0xdbff && i + 3 < n) {
                                        const uint32_t lo =
                                                big ? (p[i + 2] << 8 | p[i + 3]) : (p[i + 3] << 8 | p[i + 2]);
                                        if (lo >= 0xdc00 && lo <= 0xdfff) {
                                                c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
                                                i += 2;
                                        } else {
                                                c = 0xfffd;
                                        }
                                } else if (c >= 0xd800 && c <= 0xdfff) {
                                        c = 0xfffd;
                                }
                                out.put(c);
                        }
                }
                return out.text();
        }

        // The Vorbis comment name of an ID3 text frame, or null for one not kept.
        const char *id3Key(const char *id) {
                static constexpr struct {
                                const char *id3, *id22, *key;
                } Keys[] = {
                        {"TIT2", "TT2", "TITLE"},       {"TPE1", "TP1", "ARTIST"},      {"TALB", "TAL", "ALBUM"},
                        {"TRCK", "TRK", "TRACKNUMBER"}, {"TYER", "TYE", "DATE"},        {"TDRC", nullptr, "DATE"},
                        {"TCON", "TCO", "GENRE"},       {"TPE2", "TP2", "ALBUMARTIST"},
                };
                for (const auto &k : Keys)
                        if (std::strcmp(id, k.id3) == 0 || (k.id22 != nullptr && std::strcmp(id, k.id22) == 0))
                                return k.key;
                return nullptr;
        }

        class Mp3Decoder final : public Decoder {
                public:
                        explicit Mp3Decoder(const Source &source) : Decoder(source) {}

                        ~Mp3Decoder() override {
                                Memory::free(_scratch);
                                Memory::free(_in);
                                Memory::free(_pcm);
                                Memory::free(_index);
                        }

                        Error init(uint64_t start);

                protected:
                        Result<size_t> decodeFrames(uint8_t *out, size_t frames) override;
                        Error          seekFrame(uint64_t frame) override;

                private:
                        Error            findFirstFrame(uint64_t start, uint64_t &at, uint8_t *h);
                        void             readInfoFrame(uint64_t at, const uint8_t *h);
                        void             readId3v2(uint64_t bytes);
                        void             readId3v1(const uint8_t *t);
                        Error            fill();
                        Result<bool>     nextFrame();
                        Result<uint64_t> frameOffset(uint64_t frame);
                        void             restart(uint64_t offset, uint64_t frame);
                        void             addIndex(uint64_t frame, uint64_t offset);
                        size_t           frameBytesOf(const uint8_t *h) const;

                        mp3dec_t  _dec = {};
                        void     *_scratch = nullptr;
                        uint8_t  *_in = nullptr; // InBytes of the file from _inOffset
                        uint64_t  _inOffset = 0;
                        size_t    _inLen = 0;
                        size_t    _inPos = 0;
                        float    *_pcm = nullptr; // a frame's samples, interleaved
                        size_t    _pcmFrames = 0;
                        size_t    _pcmPos = 0;
                        uint8_t   _first[Mpeg::Bytes] = {}; // the first frame's header: the stream's form
                        uint64_t  _audioStart = 0;          // frame 0 (past a Xing frame)
                        uint64_t  _audioEnd = 0;            // before ID3v1 and APE tags
                        uint64_t  _frame = 0;               // the next frame minimp3 decodes
                        uint64_t  _dropUntil = 0;           // frames before it are dropped (priming, the delay)
                        size_t    _dropWithin = 0;          // and these samples of that frame
                        uint32_t  _delay = 0;               // gapless: samples before the first
                        unsigned  _samplesPerFrame = 1152;
                        size_t    _freeFormatBytes = 0; // a free-format stream's frame, without padding
                        uint32_t *_index = nullptr;     // offsets of frames 0, 16, 32...
                        size_t    _indexCount = 0;
                        size_t    _indexCapacity = 0;
        };

        // ---- Opening

        Error Mp3Decoder::init(uint64_t start) {
                _scratch = Memory::allocAligned(MemSpace::Psram, mp3dec_scratch_bytes(), alignof(float));
                _in = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, InBytes));
                _pcm = static_cast<float *>(
                        Memory::alloc(MemSpace::Psram, MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(float)));
                if (_scratch == nullptr || _in == nullptr || _pcm == nullptr) return Error::NoMem;
                _info.codec = Codec::Mp3;
                _info.format = SampleFormat::F32Le;
                // Tags at the end: ID3v1, and an APEv2 tag before it (skipped).
                _audioEnd = _source.size;
                if (_audioEnd >= start + 128) {
                        uint8_t t[128];
                        if (read(_audioEnd - 128, WritableByteSpan(t, sizeof(t))).isOk() &&
                            std::memcmp(t, "TAG", 3) == 0) {
                                _audioEnd -= 128;
                                if (start == 0) readId3v1(t);
                        }
                }
                if (_audioEnd >= start + 32) {
                        uint8_t a[32];
                        if (read(_audioEnd - 32, WritableByteSpan(a, sizeof(a))).isOk() &&
                            std::memcmp(a, "APETAGEX", 8) == 0) {
                                const uint64_t bytes =
                                        Decoders::le32(a + 12) + ((Decoders::le32(a + 20) & 0x80000000u) ? 32 : 0);
                                if (bytes <= _audioEnd - start) _audioEnd -= bytes;
                        }
                }
                if (start != 0) readId3v2(start);
                uint64_t at = 0;
                uint8_t  h[Mpeg::Bytes];
                Error    err = findFirstFrame(start, at, h);
                if (err.isError()) return err;
                std::memcpy(_first, h, sizeof(_first));
                _freeFormatBytes = static_cast<size_t>(_dec.free_format_bytes);
                _samplesPerFrame = Mpeg::samples(h);
                _info.channels = static_cast<uint8_t>(Mpeg::channels(h));
                _info.rateHz = Mpeg::rateHz(h);
                _audioStart = at;
                readInfoFrame(at, h);
                _dropUntil = _delay / _samplesPerFrame;
                _dropWithin = _delay % _samplesPerFrame;
                restart(_audioStart, 0);
                addIndex(0, _audioStart);
                return finishOpen();
        }

        // Finds the first frame at or after @p start: its offset and header.
        Error Mp3Decoder::findFirstFrame(uint64_t start, uint64_t &at, uint8_t *h) {
                restart(start, 0);
                while (_inOffset + _inPos < start + SyncSearchBytes) {
                        Error err = fill();
                        if (err.isError()) return err;
                        const size_t avail = _inLen - _inPos;
                        if (avail == 0) break;
                        mp3dec_frame_info_t fi = {};
                        (void)mp3dec_decode_frame_scratch(&_dec, _in + _inPos, static_cast<int>(avail), nullptr, &fi,
                                                          _scratch);
                        if (fi.hz != 0) {
                                at = _inOffset + _inPos + static_cast<size_t>(fi.frame_offset);
                                std::memcpy(h, _in + _inPos + fi.frame_offset, Mpeg::Bytes);
                                return Mpeg::isValid(h) ? Error::Ok : Error::Invalid;
                        }
                        if (fi.frame_bytes == 0) break; // a frame cut off at the end
                        _inPos += static_cast<size_t>(fi.frame_bytes);
                }
                return Error::Invalid;
        }

        // A Xing, Info or VBRI frame at @p at: the length, the bitrate, LAME's delay and padding.
        void Mp3Decoder::readInfoFrame(uint64_t at, const uint8_t *h) {
                const size_t   bytes = frameBytesOf(h);
                const uint64_t audioBytes = _audioEnd - at;
                _info.bitrate = Mpeg::bitrateKbps(h) * 1000;
                if (bytes != 0 && _info.bitrate != 0) {
                        // A constant bitrate's length, unless a header says better.
                        _info.frames = audioBytes / bytes * _samplesPerFrame;
                        _info.estimated = true;
                }
                if (Mpeg::layer(h) != 3 || bytes == 0) return;
                uint8_t      f[Mpeg::Bytes + 32 + 4 + 120 + 36] = {};
                const size_t n = bytes < sizeof(f) ? bytes : sizeof(f);
                if (read(at, WritableByteSpan(f, n)).isError()) return;
                size_t   p = Mpeg::sideInfoBytes(h) + Mpeg::Bytes;
                uint32_t frames = 0;
                uint32_t streamBytes = 0;
                if (p + 8 <= n && (std::memcmp(f + p, "Xing", 4) == 0 || std::memcmp(f + p, "Info", 4) == 0)) {
                        const uint32_t flags = Decoders::be32(f + p + 4);
                        p += 8;
                        if (flags & 1) {
                                if (p + 4 > n) return;
                                frames = Decoders::be32(f + p);
                                p += 4;
                        }
                        if (flags & 2) {
                                if (p + 4 > n) return;
                                streamBytes = Decoders::be32(f + p);
                                p += 4;
                        }
                        if (flags & 4) p += 100; // the table of contents
                        if (flags & 8) p += 4;   // the quality
                        // LAME's extension (or Lavc's, the same): the encoder's delay and padding.
                        int32_t delay = 0, padding = 0;
                        if (p + 24 <= n && f[p] != 0) {
                                const uint8_t *t = f + p + 21;
                                delay = ((t[0] << 4) | (t[1] >> 4)) + 529; // and the decoder's own
                                padding = (((t[1] & 0x0f) << 8) | t[2]) - 529;
                        }
                        _audioStart = at + bytes; // the Xing frame is silence
                        if ((flags & 1) == 0) return;
                        uint64_t samples = static_cast<uint64_t>(frames) * _samplesPerFrame;
                        if (samples >= static_cast<uint64_t>(delay)) {
                                samples -= static_cast<uint64_t>(delay);
                                _delay = static_cast<uint32_t>(delay);
                        }
                        if (padding > 0 && samples >= static_cast<uint64_t>(padding))
                                samples -= static_cast<uint64_t>(padding);
                        _info.frames = samples;
                        _info.estimated = false;
                } else if (Mpeg::Bytes + 32 + 18 <= n && std::memcmp(f + Mpeg::Bytes + 32, "VBRI", 4) == 0) {
                        streamBytes = Decoders::be32(f + Mpeg::Bytes + 32 + 10);
                        frames = Decoders::be32(f + Mpeg::Bytes + 32 + 14);
                        _audioStart = at + bytes;
                        _info.frames = static_cast<uint64_t>(frames) * _samplesPerFrame;
                        _info.estimated = false;
                } else {
                        return;
                }
                const uint64_t total = static_cast<uint64_t>(frames) * _samplesPerFrame;
                if (streamBytes != 0 && total != 0)
                        _info.bitrate =
                                static_cast<uint32_t>(static_cast<uint64_t>(streamBytes) * 8 * _info.rateHz / total);
                return;
        }

        // ID3v2's text frames (2.2, 2.3, 2.4) in the tag's first @p bytes.
        void Mp3Decoder::readId3v2(uint64_t bytes) {
                uint8_t h[10];
                if (read(0, WritableByteSpan(h, sizeof(h))).isError()) return;
                const unsigned version = h[3];
                const uint8_t  flags = h[5];
                if (version < 2 || version > 4) return;
                if ((flags & 0x80) && version < 4) return; // unsynchronised throughout: rare, not read
                uint64_t       p = 10;
                const uint64_t end = bytes - ((flags & 0x10) ? 10 : 0);
                if (version >= 3 && (flags & 0x40)) { // an extended header
                        uint8_t x[4];
                        if (read(p, WritableByteSpan(x, sizeof(x))).isError()) return;
                        p += version == 3 ? 4 + Decoders::be32(x)
                                          : (static_cast<uint64_t>(x[0]) << 21 | x[1] << 14 | x[2] << 7 | x[3]);
                }
                const size_t headerBytes = version == 2 ? 6 : 10;
                char         text[256];
                uint8_t     *data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, MaxTextBytes));
                if (data == nullptr) return;
                while (p + headerBytes <= end) {
                        uint8_t f[10];
                        if (read(p, WritableByteSpan(f, headerBytes)).isError()) break;
                        if (f[0] == 0) break; // padding
                        char     id[5] = {};
                        uint64_t size = 0;
                        uint16_t frameFlags = 0;
                        if (version == 2) {
                                std::memcpy(id, f, 3);
                                size = static_cast<uint64_t>(f[3]) << 16 | f[4] << 8 | f[5];
                        } else {
                                std::memcpy(id, f, 4);
                                size = version == 3 ? Decoders::be32(f + 4)
                                                    : (static_cast<uint64_t>(f[4] & 0x7f) << 21 | (f[5] & 0x7f) << 14 |
                                                       (f[6] & 0x7f) << 7 | (f[7] & 0x7f));
                                frameFlags = static_cast<uint16_t>(f[8] << 8 | f[9]);
                        }
                        p += headerBytes;
                        if (size > end - p) break;
                        // Compressed, encrypted, unsynchronised or with a length before it: passed over.
                        const bool plain = version == 2 || (version == 3 && (frameFlags & 0x00e0) == 0) ||
                                           (version == 4 && (frameFlags & 0x004f) == 0);
                        const char *key = id[0] == 'T' && plain ? id3Key(id) : nullptr;
                        if (key != nullptr && size > 1) {
                                const size_t n = size < MaxTextBytes ? static_cast<size_t>(size) : MaxTextBytes;
                                if (read(p, WritableByteSpan(data, n)).isOk())
                                        addTag(key, id3Text(data, n, text, sizeof(text)));
                        }
                        p += size;
                }
                Memory::free(data);
                return;
        }

        // ID3v1 (and 1.1's track) from the file's last 128 bytes @p t.
        void Mp3Decoder::readId3v1(const uint8_t *t) {
                char       text[96];
                const auto field = [&](const char *key, size_t at, size_t n) {
                        uint8_t f[31] = {};
                        std::memcpy(f + 1, t + at, n); // as Latin-1 text with its encoding byte
                        addTag(key, id3Text(f, n + 1, text, sizeof(text)));
                };
                field("TITLE", 3, 30);
                field("ARTIST", 33, 30);
                field("ALBUM", 63, 30);
                field("DATE", 93, 4);
                if (t[125] == 0 && t[126] != 0) {
                        char      track[4];
                        const int n = t[126] >= 100 ? 3 : t[126] >= 10 ? 2 : 1;
                        for (int i = n - 1, v = t[126]; i >= 0; --i, v /= 10)
                                track[i] = static_cast<char>('0' + v % 10);
                        addTag("TRACKNUMBER", StringView(track, static_cast<size_t>(n)));
                }
                return;
        }

        // ---- Decoding

        size_t Mp3Decoder::frameBytesOf(const uint8_t *h) const {
                const size_t bytes = Mpeg::frameBytes(h);
                if (bytes != 0 || _freeFormatBytes == 0) return bytes;
                const size_t padding = (h[2] >> 1) & 1;
                return _freeFormatBytes + padding * (Mpeg::layer(h) == 1 ? 4 : 1);
        }

        void Mp3Decoder::restart(uint64_t offset, uint64_t frame) {
                mp3dec_init(&_dec);
                _inOffset = offset;
                _inLen = 0;
                _inPos = 0;
                _pcmFrames = 0;
                _pcmPos = 0;
                _frame = frame;
                return;
        }

        // Keeps at least MinInBytes ahead, or what is left of the audio.
        Error Mp3Decoder::fill() {
                if (_inLen - _inPos >= MinInBytes) return Error::Ok;
                std::memmove(_in, _in + _inPos, _inLen - _inPos);
                _inOffset += _inPos;
                _inLen -= _inPos;
                _inPos = 0;
                const uint64_t at = _inOffset + _inLen;
                if (at >= _audioEnd) return Error::Ok;
                const uint64_t left = _audioEnd - at;
                const size_t   n = left < InBytes - _inLen ? static_cast<size_t>(left) : InBytes - _inLen;
                const Error    err = read(at, WritableByteSpan(_in + _inLen, n));
                if (err.isError()) return err == Error::EndOfFile ? Error::Ok : err;
                _inLen += n;
                return Error::Ok;
        }

        // Decodes the next frame into _pcm: true if it gave samples to play, false if not, or the end.
        Result<bool> Mp3Decoder::nextFrame() {
                const Error err = fill();
                if (err.isError()) return makeError<bool>(err);
                const size_t avail = _inLen - _inPos;
                if (avail == 0) return Result<bool>(false, Error::EndOfFile);
                mp3dec_frame_info_t fi = {};
                const int           samples =
                        mp3dec_decode_frame_scratch(&_dec, _in + _inPos, static_cast<int>(avail), _pcm, &fi, _scratch);
                if (fi.frame_bytes == 0) return Result<bool>(false, Error::EndOfFile); // a last frame cut off
                const uint64_t at = _inOffset + _inPos + static_cast<size_t>(fi.frame_offset);
                _inPos += static_cast<size_t>(fi.frame_bytes);
                if (fi.hz == 0) return makeResult(false); // junk passed over
                const uint64_t frame = _frame++;
                if (frame % IndexEvery == 0) addIndex(frame, at);
                if (samples <= 0 || frame < _dropUntil || static_cast<uint32_t>(fi.hz) != _info.rateHz)
                        return makeResult(false);
                // A frame in the other channel mode than the stream's: mixed down or doubled.
                const size_t n = static_cast<size_t>(samples);
                if (static_cast<unsigned>(fi.channels) != _info.channels) {
                        if (_info.channels == 1) {
                                for (size_t i = 0; i < n; ++i) _pcm[i] = (_pcm[2 * i] + _pcm[2 * i + 1]) * 0.5f;
                        } else {
                                for (size_t i = n; i-- > 0;) _pcm[2 * i] = _pcm[2 * i + 1] = _pcm[i];
                        }
                }
                _pcmFrames = n;
                _pcmPos = frame == _dropUntil ? (_dropWithin < n ? _dropWithin : n) : 0;
                return makeResult(_pcmPos < _pcmFrames);
        }

        Result<size_t> Mp3Decoder::decodeFrames(uint8_t *out, size_t frames) {
                // A known length ends it: LAME's padding isn't played.
                if (!_info.estimated && _info.frames != UnknownFrames) {
                        if (_position >= _info.frames) return Result<size_t>(0, Error::EndOfFile);
                        if (frames > _info.frames - _position) frames = static_cast<size_t>(_info.frames - _position);
                }
                while (_pcmPos >= _pcmFrames) {
                        auto [got, err] = nextFrame();
                        if (err.isError()) return Result<size_t>(0, err);
                        (void)got;
                }
                const size_t n = _pcmFrames - _pcmPos < frames ? _pcmFrames - _pcmPos : frames;
                std::memcpy(out, _pcm + _pcmPos * _info.channels, n * _info.channels * sizeof(float));
                _pcmPos += n;
                return makeResult(n);
        }

        // ---- Seeking

        void Mp3Decoder::addIndex(uint64_t frame, uint64_t offset) {
                if (frame != static_cast<uint64_t>(_indexCount) * IndexEvery || offset > UINT32_MAX) return;
                if (_indexCount == _indexCapacity) {
                        const size_t capacity = _indexCapacity == 0 ? 256 : _indexCapacity * 2;
                        auto        *grown =
                                static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, capacity * sizeof(uint32_t)));
                        if (grown == nullptr) return; // seeks scan further instead
                        if (_indexCount != 0) std::memcpy(grown, _index, _indexCount * sizeof(uint32_t));
                        Memory::free(_index);
                        _index = grown;
                        _indexCapacity = capacity;
                }
                _index[_indexCount++] = static_cast<uint32_t>(offset);
                return;
        }

        // The offset of frame @p frame: from the index, then header to header.
        Result<uint64_t> Mp3Decoder::frameOffset(uint64_t frame) {
                if (_indexCount == 0) return makeError<uint64_t>(Error::CorruptData);
                size_t i = static_cast<size_t>(frame / IndexEvery < _indexCount ? frame / IndexEvery : _indexCount - 1);
                uint64_t f = static_cast<uint64_t>(i) * IndexEvery;
                uint64_t at = _index[i];
                while (f < frame) {
                        uint8_t h[Mpeg::Bytes];
                        if (at + Mpeg::Bytes > _audioEnd) return makeError<uint64_t>(Error::OutOfRange);
                        const Error err = read(at, WritableByteSpan(h, sizeof(h)));
                        if (err.isError()) return makeError<uint64_t>(err);
                        // Damage: the index stops being exact here, so seeks past it aren't.
                        if (!Mpeg::isValid(h) || !Mpeg::isSameStream(h, _first))
                                return makeError<uint64_t>(Error::CorruptData);
                        const size_t bytes = frameBytesOf(h);
                        if (bytes == 0) return makeError<uint64_t>(Error::NotSupported);
                        at += bytes;
                        ++f;
                        if (f % IndexEvery == 0) addIndex(f, at);
                }
                if (at >= _audioEnd) return makeError<uint64_t>(Error::OutOfRange);
                return makeResult(at);
        }

        Error Mp3Decoder::seekFrame(uint64_t frame) {
                const uint64_t sample = frame + _delay;
                const uint64_t target = sample / _samplesPerFrame;
                // Back far enough for the bit reservoir: two frames, then until 511 bytes of data.
                uint64_t start = target > PredecodeFrames ? target - PredecodeFrames : 0;
                auto [at, err] = frameOffset(start);
                if (err == Error::OutOfRange) {
                        // At the end (or past an estimated one): nothing more to decode.
                        restart(_audioEnd, target);
                        return Error::Ok;
                }
                if (err.isError()) return err;
                for (size_t filled = 0; start > 0 && filled < ReservoirBytes && Mpeg::layer(_first) == 3;) {
                        auto [before, berr] = frameOffset(start - 1);
                        if (berr.isError()) break;
                        filled += static_cast<size_t>(at - before) - Mpeg::Bytes - Mpeg::sideInfoBytes(_first);
                        at = before;
                        --start;
                }
                restart(at, start);
                _dropUntil = target;
                _dropWithin = static_cast<size_t>(sample % _samplesPerFrame);
                return Error::Ok;
        }

} // namespace

Result<Decoder *> Decoders::openMp3(const Decoder::Source &source, uint64_t start) {
        Mp3Decoder *d = make<Mp3Decoder>(source);
        if (d == nullptr) return makeError<Decoder *>(Error::NoMem);
        const Error err = d->init(start);
        if (err.isError()) {
                Decoder::close(d);
                return makeError<Decoder *>(err);
        }
        return makeResult<Decoder *>(d);
}

BMDS_NAMESPACE_END
