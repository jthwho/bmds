/**
 * @file      flacdecoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * FLAC through `Decoder` (devplan/audio.md §19), on dr_flac
 * (thirdparty/dr_flac). The tags are read here, from the VORBIS_COMMENT
 * block, so that dr_flac never loads a metadata block (a picture can be
 * megabytes).
 */

#include <cstring>
#include <dr_flac.h>
#include "decoders.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t   MaxCommentBytes = 1024; // a longer one (lyrics...) is passed over
        constexpr unsigned MaxBlocks = 64;         // metadata blocks looked through for the comments

        class FlacDecoder final : public Decoder {
                public:
                        explicit FlacDecoder(const Source &source) : Decoder(source) {}

                        ~FlacDecoder() override {
                                if (_flac != nullptr) drflac_close(_flac);
                        }

                        Error init(uint64_t start) {
                                _start = start;
                                readComments();
                                drflac_allocation_callbacks alloc = {};
                                alloc.pUserData = this;
                                alloc.onMalloc = [](size_t size, void *user) {
                                        void *p = Memory::alloc(MemSpace::Psram, size);
                                        if (p == nullptr) static_cast<FlacDecoder *>(user)->_outOfMemory = true;
                                        return p;
                                };
                                alloc.onFree = [](void *p, void *) {
                                        Memory::free(p);
                                };
                                _pos = 0;
                                _flac = drflac_open(onRead, onSeek, onTell, this, &alloc);
                                if (_flac == nullptr)
                                        return _readError.isError() ? _readError
                                               : _outOfMemory       ? Error::NoMem
                                                                    : Error::Invalid;
                                _info.codec = Codec::Flac;
                                _info.channels = _flac->channels;
                                _info.rateHz = _flac->sampleRate;
                                _info.sourceBits = _flac->bitsPerSample;
                                _info.format = _flac->bitsPerSample <= 16 ? SampleFormat::S16Le : SampleFormat::S32Le;
                                if (_flac->totalPCMFrameCount != 0) {
                                        _info.frames = _flac->totalPCMFrameCount;
                                        _info.bitrate = static_cast<uint32_t>((_source.size - start) * 8 *
                                                                              _info.rateHz / _flac->totalPCMFrameCount);
                                }
                                return finishOpen();
                        }

                protected:
                        Result<size_t> decodeFrames(uint8_t *out, size_t frames) override {
                                const drflac_uint64 n =
                                        _info.format == SampleFormat::S16Le
                                                ? drflac_read_pcm_frames_s16(_flac, frames,
                                                                             reinterpret_cast<drflac_int16 *>(out))
                                                : drflac_read_pcm_frames_s32(_flac, frames,
                                                                             reinterpret_cast<drflac_int32 *>(out));
                                if (_readError.isError()) {
                                        const Error err = _readError;
                                        _readError = Error::Ok;
                                        return Result<size_t>(static_cast<size_t>(n), err);
                                }
                                if (n > 0) return makeResult(static_cast<size_t>(n));
                                return Result<size_t>(0, Error::EndOfFile);
                        }

                        Error seekFrame(uint64_t frame) override {
                                if (drflac_seek_to_pcm_frame(_flac, frame)) return Error::Ok;
                                if (_readError.isError()) {
                                        const Error err = _readError;
                                        _readError = Error::Ok;
                                        return err;
                                }
                                return Error::CorruptData;
                        }

                private:
                        // The VORBIS_COMMENT block's comments, if there is one.
                        void readComments() {
                                uint64_t p = _start + 4; // past "fLaC"
                                for (unsigned i = 0; i < MaxBlocks; ++i) {
                                        uint8_t h[4];
                                        if (read(p, WritableByteSpan(h, sizeof(h))).isError()) return;
                                        const uint32_t size = static_cast<uint32_t>(h[1] << 16 | h[2] << 8 | h[3]);
                                        p += 4;
                                        if ((h[0] & 0x7f) == 4) {
                                                readCommentBlock(p, size);
                                                return;
                                        }
                                        if (h[0] & 0x80) return; // the last
                                        p += size;
                                }
                                return;
                        }

                        void readCommentBlock(uint64_t p, uint32_t size) {
                                const uint64_t end = p + size;
                                uint8_t        n[4];
                                // The vendor string, then the count, then each comment: little-endian lengths.
                                if (read(p, WritableByteSpan(n, 4)).isError()) return;
                                p += 4 + static_cast<uint64_t>(Decoders::le32(n));
                                if (p + 4 > end || read(p, WritableByteSpan(n, 4)).isError()) return;
                                uint32_t count = Decoders::le32(n);
                                p += 4;
                                char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxCommentBytes));
                                if (text == nullptr) return;
                                for (; count > 0 && p + 4 <= end; --count) {
                                        if (read(p, WritableByteSpan(n, 4)).isError()) break;
                                        const uint32_t len = Decoders::le32(n);
                                        p += 4;
                                        if (len > end - p) break;
                                        if (len <= MaxCommentBytes &&
                                            read(p, WritableByteSpan(reinterpret_cast<uint8_t *>(text), len)).isOk())
                                                addComment(StringView(text, len));
                                        p += len;
                                }
                                Memory::free(text);
                                return;
                        }

                        static size_t onRead(void *user, void *out, size_t bytes) {
                                auto          *d = static_cast<FlacDecoder *>(user);
                                const uint64_t left = d->_source.size - d->_start - d->_pos;
                                const size_t   n = left < bytes ? static_cast<size_t>(left) : bytes;
                                if (n == 0) return 0;
                                const Error err =
                                        d->read(d->_start + d->_pos, WritableByteSpan(static_cast<uint8_t *>(out), n));
                                if (err.isError()) {
                                        if (err != Error::EndOfFile && d->_readError.isOk()) d->_readError = err;
                                        return 0;
                                }
                                d->_pos += n;
                                return n;
                        }

                        static drflac_bool32 onSeek(void *user, int offset, drflac_seek_origin origin) {
                                auto          *d = static_cast<FlacDecoder *>(user);
                                const uint64_t size = d->_source.size - d->_start;
                                const int64_t  base = origin == DRFLAC_SEEK_SET   ? 0
                                                      : origin == DRFLAC_SEEK_CUR ? static_cast<int64_t>(d->_pos)
                                                                                  : static_cast<int64_t>(size);
                                const int64_t  to = base + offset;
                                if (to < 0 || static_cast<uint64_t>(to) > size) return DRFLAC_FALSE;
                                d->_pos = static_cast<uint64_t>(to);
                                return DRFLAC_TRUE;
                        }

                        static drflac_bool32 onTell(void *user, drflac_int64 *cursor) {
                                *cursor = static_cast<drflac_int64>(static_cast<FlacDecoder *>(user)->_pos);
                                return DRFLAC_TRUE;
                        }

                        drflac  *_flac = nullptr;
                        uint64_t _start = 0; // "fLaC" (past an ID3v2 tag)
                        uint64_t _pos = 0;   // dr_flac's position, from _start
                        Error    _readError = Error::Ok;
                        bool     _outOfMemory = false; // dr_flac's failure was an allocation's
        };

} // namespace

Result<Decoder *> Decoders::openFlac(const Decoder::Source &source, uint64_t start) {
        FlacDecoder *d = make<FlacDecoder>(source);
        if (d == nullptr) return makeError<Decoder *>(Error::NoMem);
        const Error err = d->init(start);
        if (err.isError()) {
                Decoder::close(d);
                return makeError<Decoder *>(err);
        }
        return makeResult<Decoder *>(d);
}

BMDS_NAMESPACE_END
