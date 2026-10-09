/**
 * @file      decoder.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/sampleformat.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/wavfile.h>

BMDS_NAMESPACE_BEGIN

/** @brief A sound file's coding (`bmds_decode_info_t::codec`'s numbers: append only). */
enum class Codec : uint8_t {
        Wav,    ///< RIFF/WAVE (`WavFile`).
        Mp3,    ///< MPEG-1, 2 and 2.5 audio, layers I to III (minimp3).
        Vorbis, ///< Ogg Vorbis (stb_vorbis).
        Flac,   ///< FLAC, native (dr_flac).
        Midi,   ///< A Standard MIDI File, played by the synth (`bmds_synth`, registered by `addFormat()`).
};

/**
 * @brief Decodes a sound file into PCM, whatever its format
 *        (`devplan/audio.md` §7, §19).
 *
 * `open()` finds the format from the file's first bytes and returns the
 * decoder for it, with what it found (`info()`) and the file's tags. Then
 * `decode()` gives interleaved samples, whole frames, in `format()`: the
 * decoder's own (`Info::format`) unless `setFormat()` chose 16-bit, 32-bit or
 * float. `seek()` goes to an exact frame.
 *
 * Mono and stereo only (`MaxChannels`), at the file's rate: the engine
 * converts. Everything is allocated in `MemSpace::Psram` and freed by
 * `close()`; nothing is on the caller's stack beyond a few hundred bytes.
 * Decoding runs on the caller's thread, so its time is the caller's.
 *
 * Malformed input is an error, never more than that: the decoders are
 * fuzzed (`tests/fuzz`). Damage inside an MP3 or a FLAC is passed over (the
 * frame is lost), as players do.
 *
 * @par Thread Safety
 * One decoder is used by one thread at a time; separate decoders are
 * independent.
 */
class Decoder {
        public:
                /** @brief Reads the file (`WavFile::ReadAt`: exactly what is asked, at an offset). */
                using ReadAt = WavFile::ReadAt;

                /** @brief The file a decoder reads. */
                struct Source {
                                ReadAt   read = nullptr; ///< Reads it.
                                void    *arg = nullptr;  ///< @p read's argument.
                                uint64_t size = 0;       ///< Its size in bytes.
                };

                /** @brief `Info::frames` when the length isn't known. */
                static constexpr uint64_t UnknownFrames = UINT64_MAX;

                /** @brief The most channels a decoder gives (more is `NotSupported`). */
                static constexpr unsigned MaxChannels = 2;

                /** @brief What `open()` found. */
                struct Info {
                                Codec        codec = Codec::Wav;
                                SampleFormat format = SampleFormat::S16Le; ///< The decoder's own.
                                uint8_t      channels = 0;
                                uint32_t     rateHz = 0;
                                uint64_t     frames = UnknownFrames; ///< The length, at `rateHz`.
                                bool         estimated = false;      ///< `frames` from the size and bitrate.
                                uint32_t     bitrate = 0;            ///< Bits a second, the file's average; 0 unknown.
                                uint8_t      sourceBits = 0;         ///< A lossless file's sample size; 0 lossy.
                };

                /** @brief A tag: its key in capitals (Vorbis comment names) and its value, UTF-8. */
                struct Tag {
                                StringView key;
                                StringView value;
                };

                Decoder(const Decoder &) = delete;
                Decoder &operator=(const Decoder &) = delete;

                /**
                 * @brief Opens the sound file @p source reads.
                 * @return The decoder, or `Invalid` (not a sound file known),
                 *         `NotSupported` (a known format in a form not played:
                 *         Ogg Opus, more than two channels...), `NoMem`, or the
                 *         source's error.
                 */
                static Result<Decoder *> open(const Source &source);

                /** @brief Opens a format another component adds (`addFormat()`). */
                using Opener = Result<Decoder *> (*)(const Source &source);

                /** @brief Formats `addFormat()` takes. */
                static constexpr size_t MaxAddedFormats = 2;

                /**
                 * @brief Adds a format that starts with the 4 bytes @p magic, opened by
                 *        @p open (the MIDI files the synth plays: `bmds_synth` sits above
                 *        this component). At boot, before any `open()`.
                 * @return `Ok`, `Full`, `Invalid`.
                 */
                static Error addFormat(const char *magic, Opener open);

                /** @brief Frees @p decoder (null is fine). */
                static void close(Decoder *decoder);

                /** @brief Returns @p codec's name: `wav`, `mp3`, `vorbis`, `flac`. */
                static const char *name(Codec codec);

                /** @brief Returns what `open()` found. */
                const Info &info() const { return _info; }

                /** @brief Returns the format `decode()` gives. */
                SampleFormat format() const { return _format; }

                /** @brief Returns the bytes one frame takes in `format()`. */
                size_t frameBytes() const { return SampleFormats::bytesPerSample(_format) * _info.channels; }

                /**
                 * @brief Chooses the format `decode()` gives: `Info::format`,
                 *        `S16Le`, `S32Le` or `F32Le`.
                 * @return `Ok`, or `Invalid` for another.
                 */
                Error setFormat(SampleFormat format);

                /** @brief Returns the number of tags. */
                size_t tagCount() const { return _tagCount; }

                /** @brief Returns tag @p index, or empty views past the last. */
                Tag tag(size_t index) const;

                /** @brief Returns the first tag with @p key (in capitals)'s value, or an empty view. */
                StringView tag(StringView key) const;

                /**
                 * @brief Decodes into @p out as many whole frames as fit.
                 * @return Bytes given (a multiple of `frameBytes()`); 0 with
                 *         `EndOfFile` at the end; `BufferTooSmall` if not one
                 *         frame fits; `CorruptData` if the file can't be
                 *         decoded further; or the source's error.
                 */
                Result<size_t> decode(WritableByteSpan out);

                /**
                 * @brief Goes to frame @p frame (0 the first), exactly.
                 * @return `Ok`, `OutOfRange` (past a known end), `CorruptData`,
                 *         or the source's error.
                 */
                Error seek(uint64_t frame);

                /** @brief Returns the frame the next `decode()` starts at. */
                uint64_t position() const { return _position; }

        protected:
                explicit Decoder(const Source &source) : _source(source) {}
                virtual ~Decoder();

                /**
                 * @brief Decodes up to @p frames frames, at least one unless at
                 *        the end, into @p out in `Info::format`.
                 * @return Frames given, 0 with `EndOfFile` at the end, or an error.
                 */
                virtual Result<size_t> decodeFrames(uint8_t *out, size_t frames) = 0;

                /** @brief Goes to @p frame (`seek()` has checked it against a known end). */
                virtual Error seekFrame(uint64_t frame) = 0;

                /**
                 * @brief Reads exactly `buf.size()` bytes at @p offset, through a
                 *        small cache (one-byte reads are cheap).
                 * @return `Ok`, `EndOfFile` (the file is shorter), or the source's error.
                 */
                Error read(uint64_t offset, WritableByteSpan buf);

                /** @brief Adds a tag (@p key folded to capitals); full or bad ones are dropped. */
                void addTag(StringView key, StringView value);

                /** @brief Adds a Vorbis comment, `KEY=value`. */
                void addComment(StringView comment);

                /** @brief Sets up the conversion buffers once `_info` is known. */
                Error finishOpen();

                Source   _source;
                Info     _info;
                uint64_t _position = 0;

        private:
                static constexpr size_t CacheBytes = 4096;
                static constexpr size_t ChunkFrames = 256; // a conversion's frames at a time
                static constexpr size_t TagBytes = 4096;   // all tags' keys and values

                SampleFormat _format = SampleFormat::S16Le;
                uint8_t     *_cache = nullptr;
                uint64_t     _cacheOffset = 0;
                size_t       _cacheBytes = 0;
                uint8_t     *_native = nullptr; // ChunkFrames in Info::format
                float       *_floats = nullptr; // ChunkFrames stereo
                char        *_tags = nullptr;   // KEY\0value\0...
                size_t       _tagBytes = 0;
                size_t       _tagCount = 0;
};

BMDS_NAMESPACE_END
