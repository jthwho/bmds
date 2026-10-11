/**
 * @file      jpegcodec.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SoC's JPEG codec (devplan/images.md §3c), over ESP-IDF's esp_driver_jpeg: our lock, the
 * buffer rules (zero-copy or a bounce buffer), the numbering and the counters.
 */

#include <cstring>
#include <driver/jpeg_decode.h>
#include <driver/jpeg_encode.h>
#include <esp_private/dma2d.h>
#include <soc/dma2d_struct.h>
#include <atomic>
#include <bmds/board.h>
#include <bmds/jpegcodec.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/staticobject.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<JpegCodec> codecs[JpegCodec::MaxCodecs];

        // The engines' own timeout for a picture (a 12-megapixel one takes well under a second).
        constexpr int EngineTimeoutMs = 3000;

        size_t alignUp(size_t n) {
                return (n + JpegCodec::Align - 1) & ~(JpegCodec::Align - 1);
        }

        // The format the codec writes for @p want: grey of a grey JPEG; B, G, R, R, G, B or 5-6-5 of
        // a colour one; anything else is converted from B, G, R.
        PixelFormat nativeFor(const JpegHeader &h, PixelFormat want) {
                if (h.sampling == JpegHeader::Sampling::Gray) return PixelFormat::Gray8;
                switch (want) {
                        case PixelFormat::Bgr24:
                        case PixelFormat::Rgb24:
                        case PixelFormat::Rgb565: return want;
                        default: return PixelFormat::Bgr24;
                }
        }

        // The decoder's output and byte order for a native format: "RGB888" in its BGR order is bytes
        // B, G, R (the screen's), in its RGB order R, G, B; "RGB565" in BGR order a little-endian word.
        void decodeConfig(PixelFormat f, bool bt709, jpeg_decode_cfg_t &cfg) {
                cfg.conv_std = bt709 ? JPEG_YUV_RGB_CONV_STD_BT709 : JPEG_YUV_RGB_CONV_STD_BT601;
                cfg.rgb_order =
                        f == PixelFormat::Rgb24 ? JPEG_DEC_RGB_ELEMENT_ORDER_RGB : JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
                cfg.output_format = f == PixelFormat::Rgb565  ? JPEG_DECODE_OUT_FORMAT_RGB565
                                    : f == PixelFormat::Gray8 ? JPEG_DECODE_OUT_FORMAT_GRAY
                                                              : JPEG_DECODE_OUT_FORMAT_RGB888;
                return;
        }

        // Rows of @p from (any format) into @p to (any format), @p width × @p height.
        void convert(const ImageBuffer &from, const ImageBuffer &to, uint32_t width, uint32_t height) {
                const unsigned fb = bytesPerPixel(from.format), tb = bytesPerPixel(to.format);
                uint32_t       argb[64];
                for (uint32_t y = 0; y < height; ++y) {
                        const uint8_t *s = from.row(y);
                        uint8_t       *d = to.row(y);
                        if (from.format == to.format) {
                                std::memcpy(d, s, size_t(width) * fb);
                                continue;
                        }
                        for (uint32_t x = 0; x < width; x += 64) {
                                const uint32_t n = width - x < 64 ? width - x : 64;
                                Pixels::toArgb(s + size_t(x) * fb, n, argb, from.format);
                                Pixels::fromArgb(argb, n, d + size_t(x) * tb, to.format, 0);
                        }
                }
                return;
        }

        // JFIF's YCbCr is full range; ESP-IDF's 2D-DMA turns YUV to RGB with video's limited-range
        // coefficients (Y 16-235), which stretch a JPEG's colours. While the codec decodes (this flag,
        // under its lock), the wrapper below loads full-range ones instead: 256 × out = a·Y + b·Cb +
        // c·Cr + d, a row each for R, G and B (the hardware's form; its fields two's complement).
        std::atomic<uint8_t> fullRange{0}; // 0: leave it; 1: BT.601; 2: BT.709

        struct Row {
                        int a, b, c, d;
        };
        constexpr Row FullBt601[3] = {{256, 0, 359, -45824}, {256, -88, -183, 34816}, {256, 454, 0, -57984}};
        constexpr Row FullBt709[3] = {{256, 0, 403, -51456}, {256, -48, -120, 21632}, {256, 475, 0, -60672}};

        bool isYuvToRgb(dma2d_csc_rx_option_t o) {
                switch (o) {
                        case DMA2D_CSC_RX_YUV420_TO_RGB888_601:
                        case DMA2D_CSC_RX_YUV420_TO_RGB565_601:
                        case DMA2D_CSC_RX_YUV420_TO_RGB888_709:
                        case DMA2D_CSC_RX_YUV420_TO_RGB565_709:
                        case DMA2D_CSC_RX_YUV422_TO_RGB888_601:
                        case DMA2D_CSC_RX_YUV422_TO_RGB565_601:
                        case DMA2D_CSC_RX_YUV422_TO_RGB888_709:
                        case DMA2D_CSC_RX_YUV422_TO_RGB565_709:
                        case DMA2D_CSC_RX_YUV444_TO_RGB888_601:
                        case DMA2D_CSC_RX_YUV444_TO_RGB565_601:
                        case DMA2D_CSC_RX_YUV444_TO_RGB888_709:
                        case DMA2D_CSC_RX_YUV444_TO_RGB565_709: return true;
                        default: return false;
                }
        }

        void setRow(volatile dma2d_color_param_reg_t &reg, const Row &r) {
                dma2d_color_param_reg_t v;
                v.val[0] = 0;
                v.val[1] = 0;
                v.a = uint32_t(r.a) & 0x3ff;
                v.b = uint32_t(r.b) & 0x7ff;
                v.c = uint32_t(r.c) & 0x3ff;
                v.d = uint32_t(r.d) & 0x3ffff;
                reg.val[0] = v.val[0];
                reg.val[1] = v.val[1];
                return;
        }

        Error errorOf(esp_err_t e) {
                switch (e) {
                        case ESP_OK: return Error::Ok;
                        case ESP_ERR_TIMEOUT: return Error::Timeout;
                        case ESP_ERR_NO_MEM: return Error::NoMem;
                        case ESP_ERR_NOT_SUPPORTED: return Error::NotSupported;
                        default: return Error::CorruptData;
                }
        }

        // Its counters, jpegN (devplan/sysmon.md §6): names, units and labels are an interface once shipped.
        using Stat::Kind;
        using Stat::Unit;

        enum StatIndex : size_t {
                StatDecodes,
                StatEncodes,
                StatBounces,
                StatErrors,
                StatPixels,
                StatBusyUs,
                StatLastUs,
                StatMaxUs,
                StatCount
        };

        constexpr Stat::Desc CodecStats[] = {
                {"decodes", Unit::Count, Kind::Total, 0, {}, "", "Pictures decoded"},
                {"encodes", Unit::Count, Kind::Total, 0, {}, "", "Pictures encoded"},
                {"bounces", Unit::Count, Kind::Total, 0, {}, "", "Of them, through a bounce buffer (a copy)"},
                {"errors", Unit::Count, Kind::Total, 0, {}, "", "Pictures the codec failed on"},
                {"pixels", Unit::Count, Kind::Total, 0, {}, "", "Pixels decoded and encoded"},
                {"busy_us", Unit::Micros, Kind::Total, 0, {}, "", "Time spent on pictures"},
                {"last_us", Unit::Micros, Kind::Level, 0, {}, "", "The last picture's time"},
                {"max_us", Unit::Micros, Kind::Peak, 0, {}, "", "The longest picture's time"},
        };
        static_assert(sizeof(CodecStats) / sizeof(CodecStats[0]) == StatCount);

} // namespace

// The 2D-DMA's colour conversion set up (ESP-IDF's, wrapped by the link: CMakeLists.txt): as asked,
// then full range while the codec decodes. Only RX channel 0 converts colour (the hardware's).
extern "C" esp_err_t __real_dma2d_configure_color_space_conversion(dma2d_channel_handle_t    chan,
                                                                   const dma2d_csc_config_t *config);
extern "C" esp_err_t __wrap_dma2d_configure_color_space_conversion(dma2d_channel_handle_t    chan,
                                                                   const dma2d_csc_config_t *config) {
        const esp_err_t r = __real_dma2d_configure_color_space_conversion(chan, config);
        const uint8_t   full = fullRange.load(std::memory_order_acquire);
        if (r == ESP_OK && full != 0 && config != nullptr && isYuvToRgb(config->rx_csc_option)) {
                const Row *t = full == 2 ? FullBt709 : FullBt601;
                setRow(DMA2D.in_channel[0].in_color_param_group.param_h, t[0]);
                setRow(DMA2D.in_channel[0].in_color_param_group.param_m, t[1]);
                setRow(DMA2D.in_channel[0].in_color_param_group.param_l, t[2]);
        }
        return r;
}

Result<JpegCodec *> JpegCodec::codec(size_t number) {
        if (number >= MaxCodecs || number >= board().jpegCodecs) return makeError<JpegCodec *>(Error::NotSupported);
        StaticObject<JpegCodec> &slot = codecs[number];
        if (!slot.isCreated()) {
                slot.create(uint8_t(number));
                slot->_stats.setName("jpeg", int(number));
                const Error err = Stats::add(slot->_stats);
                if (err.isError()) bmdsWarn("jpeg%u's counters aren't shown: %s", unsigned(number), err.desc());
        }
        return makeResult(&*slot);
}

JpegCodec::JpegCodec(uint8_t number) : _number(number), _stats(CodecStats, StatCount, readStats, this, nullptr) {}

bool JpegCodec::isNative(const JpegHeader &header, PixelFormat format) {
        return nativeFor(header, format) == format;
}

JpegCodec::Layout JpegCodec::layoutFor(const JpegHeader &header, PixelFormat format) {
        Layout l;
        l.width = header.paddedWidth();
        l.height = header.paddedHeight();
        l.stride = size_t(l.width) * bytesPerPixel(nativeFor(header, format));
        l.bytes = alignUp(l.stride * l.height);
        return l;
}

bool JpegCodec::isZeroCopy(const JpegHeader &header, const ImageBuffer &out) {
        if (!isNative(header, out.format)) return false;
        const Layout l = layoutFor(header, out.format);
        // The rows past the picture's (its padding) need only be room, not part of the picture.
        return (reinterpret_cast<uintptr_t>(out.pixels) & (Align - 1)) == 0 && out.stride == l.stride &&
               out.room() >= l.bytes;
}

Error JpegCodec::decode(ByteSpan jpeg, const ImageBuffer &out, const Decode &how, unsigned int timeoutMs) {
        auto [h, herr] = JpegHeader::parse(jpeg);
        if (herr.isError()) return herr;
        if (!h.isHardware()) return Error::NotSupported;
        Mutex::Locker lock(_lock, timeoutMs);
        if (!lock.isLocked()) return Error::Timeout;
        return decodeLocked(jpeg, h, out, how);
}

Error JpegCodec::tryDecode(ByteSpan jpeg, const ImageBuffer &out, const Decode &how) {
        auto [h, herr] = JpegHeader::parse(jpeg);
        if (herr.isError()) return herr;
        if (!h.isHardware()) return Error::NotSupported;
        if (_lock.tryLock().isError()) return Error::WouldBlock;
        const Error err = decodeLocked(jpeg, h, out, how);
        _lock.unlock();
        return err;
}

Error JpegCodec::makeDecoder() {
        if (_decoder != nullptr) return Error();
        jpeg_decode_engine_cfg_t cfg = {};
        cfg.timeout_ms = EngineTimeoutMs;
        jpeg_decoder_handle_t e = nullptr;
        const esp_err_t       r = jpeg_new_decoder_engine(&cfg, &e);
        if (r != ESP_OK) return r == ESP_ERR_NO_MEM ? Error::NoMem : Error::IoError;
        _decoder = e;
        return Error();
}

Error JpegCodec::makeEncoder() {
        if (_encoder != nullptr) return Error();
        jpeg_encode_engine_cfg_t cfg = {};
        cfg.timeout_ms = EngineTimeoutMs;
        jpeg_encoder_handle_t e = nullptr;
        const esp_err_t       r = jpeg_new_encoder_engine(&cfg, &e);
        if (r != ESP_OK) return r == ESP_ERR_NO_MEM ? Error::NoMem : Error::IoError;
        _encoder = e;
        return Error();
}

Error JpegCodec::decodeLocked(ByteSpan jpeg, const JpegHeader &h, const ImageBuffer &out, const Decode &how) {
        if (out.pixels == nullptr || out.width < h.width || out.height < h.height ||
            out.stride < size_t(h.width) * bytesPerPixel(out.format))
                return Error::BufferTooSmall;
        const uint64_t start = System::uptimeUs();
        Error          err = makeDecoder();
        if (err.isError()) return err;
        const PixelFormat native = nativeFor(h, out.format);
        const Layout      l = layoutFor(h, out.format);
        const bool        direct = isZeroCopy(h, out);
        uint8_t          *to = direct ? out.pixels : static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, l.bytes));
        if (to == nullptr) return Error::NoMem;
        jpeg_decode_cfg_t cfg = {};
        decodeConfig(native, how.bt709, cfg);
        fullRange.store(how.bt709 ? 2 : 1, std::memory_order_release);
        uint32_t        written = 0;
        const esp_err_t r = jpeg_decoder_process(static_cast<jpeg_decoder_handle_t>(_decoder), &cfg, jpeg.data(),
                                                 uint32_t(jpeg.size()), to, uint32_t(l.bytes), &written);
        fullRange.store(0, std::memory_order_release);
        err = errorOf(r);
        if (err.isOk() && !direct) {
                ImageBuffer from;
                from.pixels = to;
                from.width = l.width;
                from.height = l.height;
                from.stride = l.stride;
                from.format = native;
                convert(from, out, h.width, h.height);
        }
        if (!direct) Memory::free(to);
        count(true, !direct, err, uint64_t(h.width) * h.height, start);
        return err;
}

Error JpegCodec::encode(const ImageBuffer &in, const Encode &how, Sink sink, void *arg, unsigned int timeoutMs) {
        if (!in.isValid() || in.width > 65535 || in.height > 65535 || how.quality < 1 || how.quality > 100 ||
            how.sampling == JpegHeader::Sampling::Other)
                return Error::Invalid;
        Mutex::Locker lock(_lock, timeoutMs);
        if (!lock.isLocked()) return Error::Timeout;
        return encodeLocked(in, how, sink, arg);
}

Error JpegCodec::encodeLocked(const ImageBuffer &in, const Encode &how, Sink sink, void *arg) {
        const uint64_t start = System::uptimeUs();
        Error          err = makeEncoder();
        if (err.isError()) return err;
        const bool grey = how.sampling == JpegHeader::Sampling::Gray;
        // The codec's input: grey for grey, else B, G, R (its "RGB888" reads those, as a screenshot
        // shows; its "RGB565" is big-endian by name, unchecked: converted from too).
        const PixelFormat native = grey ? PixelFormat::Gray8 : PixelFormat::Bgr24;
        const size_t   stride = size_t(in.width) * bytesPerPixel(native);
        const bool     direct = native == in.format && in.stride == stride; // the input has no alignment rule
        const uint8_t *from = in.pixels;
        uint8_t       *bounce = nullptr;
        if (!direct) {
                bounce = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, alignUp(stride * in.height)));
                if (bounce == nullptr) return Error::NoMem;
                ImageBuffer to;
                to.pixels = bounce;
                to.width = in.width;
                to.height = in.height;
                to.stride = stride;
                to.format = native;
                convert(in, to, in.width, in.height);
                from = bounce;
        }
        // The stream's room: a picture's worth at the top qualities, half of it below.
        const size_t raw = size_t(in.width) * in.height * (grey ? 1 : 3);
        const size_t room = alignUp((how.quality > 90 ? raw : raw / 2) + 16 * 1024);
        auto        *stream = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, room));
        if (stream == nullptr) {
                Memory::free(bounce);
                return Error::NoMem;
        }
        jpeg_encode_cfg_t cfg = {};
        cfg.width = in.width;
        cfg.height = in.height;
        cfg.image_quality = how.quality;
        switch (native) {
                case PixelFormat::Gray8: cfg.src_type = JPEG_ENCODE_IN_FORMAT_GRAY; break;
                default: cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB888; break;
        }
        switch (how.sampling) {
                case JpegHeader::Sampling::Gray: cfg.sub_sample = JPEG_DOWN_SAMPLING_GRAY; break;
                case JpegHeader::Sampling::Yuv444: cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV444; break;
                case JpegHeader::Sampling::Yuv422: cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422; break;
                default: cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420; break;
        }
        uint32_t        written = 0;
        const esp_err_t r = jpeg_encoder_process(static_cast<jpeg_encoder_handle_t>(_encoder), &cfg, from,
                                                 uint32_t(stride * in.height), stream, uint32_t(room), &written);
        err = r == ESP_OK ? Error() : r == ESP_ERR_TIMEOUT ? Error(Error::Timeout) : Error(Error::IoError);
        Memory::free(bounce);
        if (err.isOk()) err = sink(arg, ByteSpan(stream, written));
        Memory::free(stream);
        count(false, !direct, r == ESP_OK ? Error() : err, uint64_t(in.width) * in.height, start);
        return err;
}

void JpegCodec::count(bool decoding, bool bounced, Error err, uint64_t pixels, uint64_t startUs) {
        const uint32_t us = uint32_t(System::uptimeUs() - startUs);
        if (err.isError()) {
                ++_counts.errors;
                return;
        }
        ++(decoding ? _counts.decodes : _counts.encodes);
        if (bounced) ++_counts.bounces;
        _counts.pixels += pixels;
        _counts.busyUs += us;
        _counts.lastUs = us;
        if (us > _counts.maxUs) _counts.maxUs = us;
        return;
}

JpegCodec::Counts JpegCodec::counts() const {
        return _counts; // a torn read of a counter is harmless here
}

void JpegCodec::readStats(void *arg, Span<int64_t> v) {
        const Counts c = static_cast<JpegCodec *>(arg)->counts();
        v[StatDecodes] = c.decodes;
        v[StatEncodes] = c.encodes;
        v[StatBounces] = c.bounces;
        v[StatErrors] = c.errors;
        v[StatPixels] = int64_t(c.pixels);
        v[StatBusyUs] = int64_t(c.busyUs);
        v[StatLastUs] = c.lastUs;
        v[StatMaxUs] = c.maxUs;
        return;
}

BMDS_NAMESPACE_END
