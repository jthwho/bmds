/**
 * @file      images.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's pictures (devplan/images.md §3d): the decoders behind one front, and encoding.
 */

#include <cstring>
#include <bmds/bmp.h>
#include <bmds/images.h>
#include <bmds/imagescaler.h>
#include <bmds/jpegcodec.h>
#include <bmds/memory.h>
#include <bmds/png.h>
#include <bmds/pngdecoder.h>
#include <bmds/softjpeg.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        BMDS_SYSCONF(imageDecodeMax, Images::DecodeMaxKey, Config::Type::Int, "8",
                     "The most megapixels a picture is decoded at (bigger JPEGs come reduced)", 1, 30, nullptr, nullptr)

        bool isPng(ByteSpan f) {
                return f.size() >= 8 && std::memcmp(f.data(), "\x89PNG\r\n\x1a\n", 8) == 0;
        }

        bool isJpeg(ByteSpan f) {
                return f.size() >= 3 && f[0] == 0xff && f[1] == 0xd8 && f[2] == 0xff;
        }

        bool isBmp(ByteSpan f) {
                return f.size() >= 2 && f[0] == 'B' && f[1] == 'M';
        }

        JpegCodec *codec() {
                auto [c, err] = JpegCodec::codec(0);
                return err.isOk() ? c : nullptr;
        }

        // Decodes as @p report's plan says into @p out (the plan's size, unturned).
        Error decodePlanned(ByteSpan file, const Images::Options &o, const Images::Report &report,
                            const ImageBuffer &out) {
                switch (report.plan.decoder) {
                        case ImagePlan::Decoder::Png: return Png::decode(file, out, o.background, MemSpace::Psram);
                        case ImagePlan::Decoder::Bmp: return Bmp::decode(file, out, o.background);
                        case ImagePlan::Decoder::Hardware: {
                                JpegCodec *c = codec();
                                if (c == nullptr) return Error::NotSupported;
                                JpegCodec::Decode how;
                                how.format = out.format;
                                return c->decode(file, out, how, WaitForever);
                        }
                        default: {
                                // Its tables in internal RAM for speed, else PSRAM.
                                const Error err = SoftJpeg::decode(file, out, report.plan.shift, MemSpace::Internal);
                                if (err != Error::NoMem) return err;
                                return SoftJpeg::decode(file, out, report.plan.shift, MemSpace::Psram);
                        }
                }
        }

        Error toSink(void *arg, ByteSpan data) {
                auto *s = static_cast<std::pair<Images::Sink, void *> *>(arg);
                return s->first(s->second, data);
        }

} // namespace

// ---- Picture, FileBytes

Error Images::Picture::allocate(uint32_t width, uint32_t height, PixelFormat format, MemSpace space, size_t stride,
                                size_t bytes) {
        reset();
        if (width == 0 || height == 0) return Error::Invalid;
        if (stride == 0) stride = size_t(width) * bytesPerPixel(format);
        if (bytes == 0) bytes = stride * height;
        _memory = Memory::alloc(space, bytes);
        if (_memory == nullptr) return Error::NoMem;
        _image.pixels = static_cast<uint8_t *>(_memory);
        _image.width = width;
        _image.height = height;
        _image.stride = stride;
        _image.format = format;
        _image.capacity = bytes;
        return Error();
}

void Images::Picture::reset() {
        Memory::free(_memory);
        _memory = nullptr;
        _image = ImageBuffer{};
        return;
}

Images::FileBytes::~FileBytes() {
        Memory::free(_data);
}

Images::FileBytes &Images::FileBytes::operator=(FileBytes &&other) noexcept {
        if (this != &other) {
                Memory::free(_data);
                _data = std::exchange(other._data, nullptr);
                _size = std::exchange(other._size, 0);
        }
        return *this;
}

Error Images::FileBytes::read(StringView path, const Vfs::Context &context) {
        Memory::free(_data);
        _data = nullptr;
        _size = 0;
        auto [st, serr] = Vfs::stat(path, context);
        if (serr.isError()) return serr;
        if (st.type != FileType::File) return Error::IsDir;
        if (st.size > MaxFileBytes) return Error::OutOfRange;
        if (st.size == 0) return Error::CorruptData;
        auto [file, oerr] = Vfs::open(path, Vfs::Read, context);
        if (oerr.isError()) return oerr;
        _data = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, size_t(st.size)));
        if (_data == nullptr) return Error::NoMem;
        size_t got = 0;
        while (got < st.size) {
                auto [n, rerr] = file->read(WritableByteSpan(_data + got, size_t(st.size) - got), WaitForever);
                if (rerr.isError()) return rerr;
                if (n == 0) break;
                got += n;
        }
        _size = got;
        return got == 0 ? Error(Error::CorruptData) : Error();
}

Error Images::FileBytes::read(FileHandle &file) {
        Memory::free(_data);
        _data = nullptr;
        _size = 0;
        size_t room = 0;
        for (;;) {
                if (_size == room) { // full: twice the room
                        if (room >= MaxFileBytes) return Error::OutOfRange;
                        const size_t bigger = room == 0 ? 256 * 1024 : (room * 2 < MaxFileBytes ? room * 2 : MaxFileBytes);
                        auto        *grown = static_cast<uint8_t *>(Memory::alloc(MemSpace::Psram, bigger));
                        if (grown == nullptr) return Error::NoMem;
                        if (_size != 0) std::memcpy(grown, _data, _size);
                        Memory::free(_data);
                        _data = grown;
                        room = bigger;
                }
                auto [n, err] = file.read(WritableByteSpan(_data + _size, room - _size), WaitForever);
                if (err == Error::EndOfFile || (err.isOk() && n == 0)) break;
                if (err.isError()) return err;
                _size += n;
        }
        return _size == 0 ? Error(Error::CorruptData) : Error();
}

// ---- Decoding

Result<ImageInfo> Images::info(ByteSpan file, JpegHeader *jpeg) {
        if (isPng(file)) return Png::info(file);
        if (isJpeg(file)) {
                auto [h, err] = JpegHeader::parse(file);
                if (err.isError()) return makeError<ImageInfo>(err);
                if (jpeg != nullptr) *jpeg = h;
                ImageInfo i = h.info();
                if (codec() == nullptr) i.flags = uint8_t(i.flags & ~ImageInfo::Hardware);
                return makeResult(i);
        }
        if (isBmp(file)) return Bmp::info(file);
        return makeError<ImageInfo>(Error::NotSupported);
}

uint64_t Images::budgetPixels() {
        const Result<int32_t> mp = SysConf::getInt(StringView(DecodeMaxKey));
        return uint64_t(mp.isOk() ? mp.value() : 8) * 1'000'000u;
}

Error Images::plan(ByteSpan file, const Options &o, Report &report) {
        report = Report{};
        JpegHeader h;
        auto [info, ierr] = Images::info(file, &h);
        if (ierr.isError()) return ierr;
        report.info = info;
        ImagePlan::Want want;
        want.minWidth = o.minWidth;
        want.minHeight = o.minHeight;
        // A box: the picture's fit in it (upright when oriented) is the smallest wanted.
        if (o.boxWidth != 0 && o.boxHeight != 0) {
                const ImageScaler::Size up = o.orient ? ImageScaler::oriented(info.width, info.height, info.orientation)
                                                      : ImageScaler::Size{info.width, info.height};
                const ImageScaler::Size f = ImageScaler::fit(up.width, up.height, o.boxWidth, o.boxHeight, false);
                want.minWidth = want.minWidth > f.width ? want.minWidth : f.width;
                want.minHeight = want.minHeight > f.height ? want.minHeight : f.height;
        }
        if (o.orient && info.orientation >= 5) std::swap(want.minWidth, want.minHeight); // turned after
        want.budgetPixels = budgetPixels();
        want.hardware = o.hardware && codec() != nullptr;
        auto [p, perr] = ImagePlan::make(info, info.codec == ImageCodec::Jpeg ? &h : nullptr, want);
        if (perr.isError()) return perr;
        report.plan = p;
        const ImageScaler::Size s = o.orient ? ImageScaler::oriented(p.width, p.height, info.orientation)
                                             : ImageScaler::Size{p.width, p.height};
        report.width = s.width;
        report.height = s.height;
        return Error();
}

Error Images::decode(ByteSpan file, const Options &o, const ImageBuffer &out, Report *report) {
        Report r;
        Error  err = plan(file, o, r);
        if (err.isError()) return err;
        if (out.pixels == nullptr || out.width < r.width || out.height < r.height || out.format != o.format)
                return Error::BufferTooSmall;
        const bool     turn = o.orient && r.info.orientation != 1;
        const uint64_t start = System::uptimeUs();
        if (!turn) {
                err = decodePlanned(file, o, r, out);
                r.decodeUs = uint32_t(System::uptimeUs() - start);
        } else {
                Picture upright;
                err = upright.allocate(r.plan.width, r.plan.height, o.format, MemSpace::Psram);
                if (err.isOk()) err = decodePlanned(file, o, r, upright.image());
                r.decodeUs = uint32_t(System::uptimeUs() - start);
                if (err.isOk()) {
                        ImageBuffer target = out;
                        target.width = r.width;
                        target.height = r.height;
                        err = ImageScaler::orient(upright.image(), target, r.info.orientation);
                }
                r.orientUs = uint32_t(System::uptimeUs() - start) - r.decodeUs;
        }
        if (report != nullptr) *report = r;
        return err;
}

Error Images::load(ByteSpan file, const Options &o, Picture &out, Report *report) {
        Report r;
        Error  err = plan(file, o, r);
        if (err.isError()) return err;
        const bool turn = o.orient && r.info.orientation != 1;
        // The codec's own layout: decoded into without a copy.
        if (!turn && r.plan.decoder == ImagePlan::Decoder::Hardware) {
                const JpegHeader h = JpegHeader::parse(file).value(); // parsed once already by plan()
                if (JpegCodec::isNative(h, o.format)) {
                        const JpegCodec::Layout l = JpegCodec::layoutFor(h, o.format);
                        err = out.allocate(r.width, r.height, o.format, MemSpace::PsramDma, l.stride, l.bytes);
                        if (err.isError()) return err;
                        const uint64_t start = System::uptimeUs();
                        err = decodePlanned(file, o, r, out.image());
                        r.decodeUs = uint32_t(System::uptimeUs() - start);
                        r.zeroCopy = JpegCodec::isZeroCopy(h, out.image());
                        if (err.isError()) out.reset();
                        if (report != nullptr) *report = r;
                        return err;
                }
        }
        err = out.allocate(r.width, r.height, o.format, MemSpace::Psram);
        if (err.isOk()) err = decode(file, o, out.image(), &r);
        if (err.isError()) out.reset();
        if (report != nullptr) *report = r;
        return err;
}

// ---- Encoding

Error Images::encode(const ImageBuffer &in, const Encode &how, Sink sink, void *arg) {
        if (!in.isValid()) return Error::Invalid;
        if (how.codec == ImageCodec::Jpeg) {
                JpegCodec *c = codec();
                if (c == nullptr) return Error::NotSupported;
                JpegCodec::Encode e;
                e.quality = how.quality;
                e.sampling = how.sampling;
                return c->encode(in, e, sink, arg, WaitForever);
        }
        if (how.codec != ImageCodec::Png) return Error::NotSupported;
        if (in.width > 65535 || in.height > 65535) return Error::Invalid;
        // Png::write takes B, G, R rows: others are converted first.
        Picture     bgr;
        ImageBuffer src = in;
        if (in.format != PixelFormat::Bgr24) {
                Error err = bgr.allocate(in.width, in.height, PixelFormat::Bgr24, MemSpace::Psram);
                if (err.isError()) return err;
                uint32_t argb[64];
                for (uint32_t y = 0; y < in.height; ++y)
                        for (uint32_t x = 0; x < in.width; x += 64) {
                                const uint32_t n = in.width - x < 64 ? in.width - x : 64;
                                Pixels::toArgb(in.row(y) + size_t(x) * bytesPerPixel(in.format), n, argb, in.format);
                                Pixels::fromArgb(argb, n, bgr.image().row(y) + size_t(x) * 3, PixelFormat::Bgr24, 0);
                        }
                src = bgr.image();
        }
        std::pair<Sink, void *> s{sink, arg};
        return Png::write(src.pixels, uint16_t(src.width), uint16_t(src.height), src.stride, how.level, toSink, &s,
                          nullptr);
}

ImageCodec Images::codecForName(StringView name) {
        const size_t dot = name.lastIndexOf('.');
        if (dot == StringView::NPos) return ImageCodec::Unknown;
        const StringView ext = name.mid(dot + 1);
        const auto       is = [&ext](const char *e) {
                const StringView v(e);
                if (v.size() != ext.size()) return false;
                for (size_t i = 0; i < v.size(); ++i)
                        if ((ext[i] | 0x20) != v[i]) return false;
                return true;
        };
        if (is("png")) return ImageCodec::Png;
        if (is("jpg") || is("jpeg")) return ImageCodec::Jpeg;
        if (is("bmp")) return ImageCodec::Bmp;
        return ImageCodec::Unknown;
}

const char *Images::decoderName(ImagePlan::Decoder decoder) {
        switch (decoder) {
                case ImagePlan::Decoder::Png: return "png";
                case ImagePlan::Decoder::Bmp: return "bmp";
                case ImagePlan::Decoder::Hardware: return "hardware";
                default: return "software";
        }
}

BMDS_NAMESPACE_END
