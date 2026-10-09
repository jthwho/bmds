/**
 * @file      vorbisdecoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Ogg Vorbis through `Decoder` (devplan/audio.md §19), on stb_vorbis
 * (thirdparty/stb_vorbis, read through BMDS's callback patch). stb_vorbis
 * takes all its memory from one buffer it is given, so it never allocates
 * nor uses alloca(): the buffer starts at 128 KB and doubles until the
 * file's setup fits.
 */

#include <cstring>
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#include "decoders.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t FirstArenaBytes = 128 * 1024;
        constexpr size_t MaxArenaBytes = 4 * 1024 * 1024;

        class VorbisDecoder final : public Decoder {
                public:
                        explicit VorbisDecoder(const Source &source) : Decoder(source) {}

                        ~VorbisDecoder() override {
                                if (_vorbis != nullptr) stb_vorbis_close(_vorbis);
                                Memory::free(_arena);
                        }

                        Error init() {
                                // stb_vorbis's offsets are 32-bit, and its file mode stops at 2 GB.
                                if (_source.size > INT32_MAX) return Error::NotSupported;
                                stb_vorbis_io io = {};
                                io.read = readAt;
                                io.user = this;
                                io.length = static_cast<unsigned int>(_source.size);
                                int error = VORBIS__no_error;
                                for (size_t bytes = FirstArenaBytes; _vorbis == nullptr; bytes *= 2) {
                                        Memory::free(_arena);
                                        _arena = static_cast<char *>(Memory::allocAligned(MemSpace::Psram, bytes, 8));
                                        if (_arena == nullptr) return Error::NoMem;
                                        stb_vorbis_alloc alloc = {_arena, static_cast<int>(bytes)};
                                        _vorbis = stb_vorbis_open_io(&io, &error, &alloc);
                                        if (_vorbis == nullptr && (error != VORBIS_outofmem || bytes >= MaxArenaBytes))
                                                return errorOf(error);
                                }
                                const stb_vorbis_info vi = stb_vorbis_get_info(_vorbis);
                                _info.codec = Codec::Vorbis;
                                _info.format = SampleFormat::F32Le;
                                _info.channels =
                                        vi.channels > 0 && vi.channels < 256 ? static_cast<uint8_t>(vi.channels) : 0;
                                _info.rateHz = vi.sample_rate;
                                const stb_vorbis_comment vc = stb_vorbis_get_comment(_vorbis);
                                for (int i = 0; i < vc.comment_list_length; ++i)
                                        if (vc.comment_list[i] != nullptr) addComment(StringView(vc.comment_list[i]));
                                const Error err = finishOpen();
                                if (err.isError()) return err;
                                const unsigned int samples = stb_vorbis_stream_length_in_samples(_vorbis);
                                if (samples != 0) {
                                        _info.frames = samples;
                                        _info.bitrate =
                                                static_cast<uint32_t>(_source.size * 8 * _info.rateHz / samples);
                                }
                                // The length was found at the end: back to the start.
                                return stb_vorbis_seek_start(_vorbis) ? Error::Ok
                                                                      : errorOf(stb_vorbis_get_error(_vorbis));
                        }

                protected:
                        Result<size_t> decodeFrames(uint8_t *out, size_t frames) override {
                                if (_atEnd) return Result<size_t>(0, Error::EndOfFile);
                                const size_t most = static_cast<size_t>(INT32_MAX) / MaxChannels;
                                const int    n = stb_vorbis_get_samples_float_interleaved(
                                        _vorbis, _info.channels, reinterpret_cast<float *>(out),
                                        static_cast<int>((frames < most ? frames : most) * _info.channels));
                                if (_readError.isError())
                                        return Result<size_t>(static_cast<size_t>(n > 0 ? n : 0), takeReadError());
                                if (n > 0) return makeResult(static_cast<size_t>(n));
                                const int error = stb_vorbis_get_error(_vorbis);
                                return Result<size_t>(0, error == VORBIS__no_error || error == VORBIS_unexpected_eof
                                                                 ? Error::EndOfFile
                                                                 : Error::CorruptData);
                        }

                        Error seekFrame(uint64_t frame) override {
                                if (frame > UINT32_MAX) return Error::OutOfRange;
                                // The end: stb_vorbis finds no frame holding it, so it isn't asked.
                                _atEnd = _info.frames != UnknownFrames && frame >= _info.frames;
                                if (_atEnd) return Error::Ok;
                                if (stb_vorbis_seek(_vorbis, static_cast<unsigned int>(frame))) return Error::Ok;
                                if (_readError.isError()) return takeReadError();
                                const int error = stb_vorbis_get_error(_vorbis);
                                return error == VORBIS_seek_invalid ? Error::OutOfRange : errorOf(error);
                        }

                private:
                        static int readAt(void *user, unsigned int at, unsigned char *data, int n) {
                                auto       *d = static_cast<VorbisDecoder *>(user);
                                const Error err = d->read(at, WritableByteSpan(data, static_cast<size_t>(n)));
                                // The source's own errors are told as they are, not as a damaged file.
                                if (err.isError() && err != Error::EndOfFile && d->_readError.isOk())
                                        d->_readError = err;
                                return err.isOk() ? 1 : 0;
                        }

                        Error takeReadError() {
                                const Error err = _readError;
                                _readError = Error::Ok;
                                return err;
                        }

                        Error errorOf(int error) {
                                if (_readError.isError()) return takeReadError();
                                switch (error) {
                                        case VORBIS_outofmem: return Error::NoMem;
                                        case VORBIS_feature_not_supported:
                                        case VORBIS_too_many_channels:
                                        case VORBIS_ogg_skeleton_not_supported: return Error::NotSupported;
                                        case VORBIS_missing_capture_pattern:
                                        case VORBIS_invalid_first_page:
                                        case VORBIS_unexpected_eof: return Error::Invalid;
                                        default: return Error::CorruptData;
                                }
                        }

                        stb_vorbis *_vorbis = nullptr;
                        char       *_arena = nullptr;
                        Error       _readError = Error::Ok;
                        bool        _atEnd = false; // sought to the end
        };

} // namespace

Result<Decoder *> Decoders::openVorbis(const Decoder::Source &source) {
        VorbisDecoder *d = make<VorbisDecoder>(source);
        if (d == nullptr) return makeError<Decoder *>(Error::NoMem);
        const Error err = d->init();
        if (err.isError()) {
                Decoder::close(d);
                return makeError<Decoder *>(err);
        }
        return makeResult<Decoder *>(d);
}

BMDS_NAMESPACE_END
