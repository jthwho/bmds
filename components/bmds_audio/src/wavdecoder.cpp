/**
 * @file      wavdecoder.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * WAV through `Decoder` (devplan/audio.md §19): the samples as they are, in
 * the file's own format.
 */

#include "decoders.h"

BMDS_NAMESPACE_BEGIN

namespace {

        class WavDecoder final : public Decoder {
                public:
                        explicit WavDecoder(const Source &source) : Decoder(source) {}

                        Error init() {
                                auto [wav, err] = WavFile::parse(_source.read, _source.arg, _source.size);
                                if (err.isError()) return err;
                                _dataOffset = wav.dataOffset;
                                _info.codec = Codec::Wav;
                                _info.format = wav.format;
                                _info.channels = wav.channels;
                                _info.rateHz = wav.rateHz;
                                _bytes = SampleFormats::bytesPerSample(wav.format) * wav.channels;
                                _info.frames = wav.dataBytes / _bytes;
                                _info.bitrate = static_cast<uint32_t>(_bytes * 8 * wav.rateHz);
                                _info.sourceBits = static_cast<uint8_t>(SampleFormats::bytesPerSample(wav.format) * 8);
                                return finishOpen();
                        }

                protected:
                        Result<size_t> decodeFrames(uint8_t *out, size_t frames) override {
                                const uint64_t left = _info.frames - _position;
                                if (left == 0) return Result<size_t>(0, Error::EndOfFile);
                                const size_t n = left < frames ? static_cast<size_t>(left) : frames;
                                const Error  err = _source.read(_source.arg, _dataOffset + _position * _bytes,
                                                                WritableByteSpan(out, n * _bytes));
                                // Shorter than its header said: the end.
                                if (err == Error::EndOfFile) return Result<size_t>(0, Error::EndOfFile);
                                if (err.isError()) return makeError<size_t>(err);
                                return makeResult(n);
                        }

                        Error seekFrame(uint64_t frame) override {
                                (void)frame; // any frame of the file: read where asked
                                return Error::Ok;
                        }

                private:
                        uint64_t _dataOffset = 0;
                        size_t   _bytes = 1;
        };

} // namespace

Result<Decoder *> Decoders::openWav(const Decoder::Source &source) {
        WavDecoder *d = make<WavDecoder>(source);
        if (d == nullptr) return makeError<Decoder *>(Error::NoMem);
        const Error err = d->init();
        if (err.isError()) {
                Decoder::close(d);
                return makeError<Decoder *>(err);
        }
        return makeResult<Decoder *>(d);
}

BMDS_NAMESPACE_END
