/**
 * @file      decodedfile.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/decoder.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a decoded fd refers to (`audio_decode`, `devplan/audio.md` §19):
 *        a sound file read as PCM.
 *
 * `read()` decodes on the reading thread: whole frames when the buffer holds
 * one, else a frame's bytes a piece at a time. `seek()` is in bytes of that
 * PCM (a frame's start or inside one) and exact; `SeekFrom::End` needs a
 * known length. The source is a file's handle, kept open until the last
 * close, or a copy of a buffer. Everything is in `MemSpace::Psram`, freed
 * when the last handle goes.
 *
 * Reads never wait for anything but the file: `readiness()` is always
 * readable.
 *
 * @par Thread Safety
 * Thread-safe: the calls serialize on its lock.
 */
class DecodedFile final : public Stream {
        public:
                /**
                 * @brief Decodes the file @p source (readable and seekable),
                 *        giving @p format (`Decoder::setFormat()`), or the
                 *        decoder's own if @p native.
                 * @return A read-only handle, or `Decoder::open()`'s errors,
                 *         `Invalid` (a format not offered), `NotSupported` (a
                 *         source that doesn't seek), `NoMem`.
                 */
                static Result<FileHandle::Ptr> open(const FileHandle::Ptr &source, SampleFormat format, bool native);

                /**
                 * @brief As the other `open()`, from a copy of @p data.
                 */
                static Result<FileHandle::Ptr> open(ByteSpan data, SampleFormat format, bool native);

                /** @brief Returns the decoder (for its info and tags; read through the stream). */
                const Decoder &decoder() const { return *_decoder; }

                Result<size_t>   read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t>   write(ByteSpan data, unsigned int timeoutMs) override;
                Result<uint64_t> seek(int64_t offset, SeekFrom from) override;
                DecodedFile     *decodedFile() override { return this; }
                void             handleClosed(uint8_t mode) override;

        private:
                DecodedFile() = default;
                ~DecodedFile() = default;

                static Result<FileHandle::Ptr> finish(DecodedFile *file, SampleFormat format, bool native);
                static Error                   readSource(void *arg, uint64_t offset, WritableByteSpan buf);
                static Error                   readCopy(void *arg, uint64_t offset, WritableByteSpan buf);
                void                           destroy();

                static constexpr size_t MaxFrameBytes = Decoder::MaxChannels * 4;

                Mutex           _lock;
                Decoder        *_decoder = nullptr;
                FileHandle::Ptr _source; // or:
                uint8_t        *_copy = nullptr;
                size_t          _copyBytes = 0;
                uint64_t        _position = 0;              // in bytes of PCM
                uint8_t         _carry[MaxFrameBytes] = {}; // a frame read in pieces
                size_t          _carryBytes = 0;
                size_t          _carryPos = 0;
};

BMDS_NAMESPACE_END
