/**
 * @file      wavfile.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A WAV file's header (devplan/audio.md §18).
 */

#include <cstring>
#include <bmds/wavfile.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The format tags (Microsoft's mmreg.h numbers).
        constexpr uint16_t TagPcm = 1;
        constexpr uint16_t TagFloat = 3;
        constexpr uint16_t TagALaw = 6;
        constexpr uint16_t TagMuLaw = 7;
        constexpr uint16_t TagExtensible = 0xfffe;

        // KSDATAFORMAT_SUBTYPE_*: the tag in the first two bytes, then these.
        constexpr uint8_t GuidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                          0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

        constexpr size_t   FmtBytes = 16;        // through wBitsPerSample
        constexpr size_t   ExtensibleBytes = 40; // with cbSize, the valid bits, the mask and the GUID
        constexpr uint32_t StreamedSize = 0xffffffffu;

        uint16_t le16(const uint8_t *p) {
                return static_cast<uint16_t>(p[0] | p[1] << 8);
        }

        uint32_t le32(const uint8_t *p) {
                return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                       static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
        }

        // The sample format of @p tag at @p bits a sample.
        Result<SampleFormat> formatOf(uint16_t tag, uint16_t bits) {
                switch (tag) {
                        case TagPcm:
                                switch (bits) {
                                        case 8: return makeResult(SampleFormat::U8);
                                        case 16: return makeResult(SampleFormat::S16Le);
                                        case 24: return makeResult(SampleFormat::S24_3Le);
                                        // 24 valid bits in 32 are at the top: as s32le.
                                        case 32: return makeResult(SampleFormat::S32Le);
                                        default: break;
                                }
                                break;
                        case TagFloat:
                                if (bits == 32) return makeResult(SampleFormat::F32Le);
                                break;
                        case TagALaw:
                                if (bits == 8) return makeResult(SampleFormat::ALaw);
                                break;
                        case TagMuLaw:
                                if (bits == 8) return makeResult(SampleFormat::MuLaw);
                                break;
                        default: break;
                }
                return makeError<SampleFormat>(Error::NotSupported);
        }

        // The `fmt ` chunk's @p size bytes at @p offset.
        Error readFormat(WavFile::ReadAt read, void *arg, uint64_t offset, uint32_t size, WavFile::Info &info) {
                if (size < FmtBytes) return Error::Invalid;
                uint8_t      f[ExtensibleBytes] = {};
                const size_t n = size < ExtensibleBytes ? size : ExtensibleBytes;
                const Error  err = read(arg, offset, WritableByteSpan(f, n));
                if (err.isError()) return err == Error::EndOfFile ? Error::Invalid : err;
                uint16_t       tag = le16(f);
                const uint16_t channels = le16(f + 2);
                const uint32_t rate = le32(f + 4);
                const uint16_t blockAlign = le16(f + 12);
                const uint16_t bits = le16(f + 14);
                if (tag == TagExtensible) {
                        if (n < ExtensibleBytes || le16(f + 16) < ExtensibleBytes - 18) return Error::Invalid;
                        if (std::memcmp(f + 26, GuidTail, sizeof(GuidTail)) != 0) return Error::NotSupported;
                        tag = le16(f + 24);
                }
                if (channels == 0 || rate == 0) return Error::Invalid;
                if (channels > 2) return Error::NotSupported;
                auto [format, ferr] = formatOf(tag, bits);
                if (ferr.isError()) return ferr;
                if (blockAlign != SampleFormats::bytesPerSample(format) * channels) return Error::Invalid;
                info.format = format;
                info.channels = static_cast<uint8_t>(channels);
                info.rateHz = rate;
                return Error::Ok;
        }

} // namespace

namespace WavFile {

        Result<Info> parse(ReadAt read, void *arg, uint64_t fileSize) {
                uint8_t head[12];
                Error   err = read(arg, 0, WritableByteSpan(head, sizeof(head)));
                if (err.isError()) return makeError<Info>(err == Error::EndOfFile ? Error::Invalid : err);
                if (std::memcmp(head + 8, "WAVE", 4) != 0) return makeError<Info>(Error::Invalid);
                if (std::memcmp(head, "RIFX", 4) == 0 || std::memcmp(head, "RF64", 4) == 0)
                        return makeError<Info>(Error::NotSupported);
                if (std::memcmp(head, "RIFF", 4) != 0) return makeError<Info>(Error::Invalid);
                Info     info;
                bool     haveFormat = false;
                uint64_t at = sizeof(head);
                while (at + 8 <= fileSize) {
                        uint8_t chunk[8];
                        err = read(arg, at, WritableByteSpan(chunk, sizeof(chunk)));
                        if (err.isError()) return makeError<Info>(err == Error::EndOfFile ? Error::Invalid : err);
                        const uint32_t size = le32(chunk + 4);
                        const uint64_t body = at + 8;
                        if (std::memcmp(chunk, "fmt ", 4) == 0) {
                                if (body + size > fileSize) return makeError<Info>(Error::Invalid);
                                err = readFormat(read, arg, body, size, info);
                                if (err.isError()) return makeError<Info>(err);
                                haveFormat = true;
                        } else if (std::memcmp(chunk, "data", 4) == 0) {
                                if (!haveFormat) return makeError<Info>(Error::Invalid);
                                const uint64_t left = fileSize - body;
                                uint64_t       bytes = size;
                                if (size == 0 || size == StreamedSize || bytes > left) bytes = left;
                                const size_t frame = SampleFormats::bytesPerSample(info.format) * info.channels;
                                info.dataOffset = body;
                                info.dataBytes = bytes - bytes % frame;
                                return makeResult(info);
                        }
                        at = body + size + (size & 1u); // chunks are padded to even lengths
                }
                return makeError<Info>(Error::Invalid);
        }

} // namespace WavFile

BMDS_NAMESPACE_END
