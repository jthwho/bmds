/**
 * @file      imagecalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The SDK's pictures (devplan/images.md §3f): image_info, image_decode and image_encode (slots
 * 93-95), the kernel's Images front.
 */

#include <cstddef>
#include <cstdint>
#include <bmds/errno.h>
#include <bmds/images.h>
#include <bmds/jpegcodec.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::ImageBuffer;
using bmds::ImageCodec;
using bmds::KernelCall;
using bmds::PixelFormat;
using bmds::Process;
namespace Images = bmds::Images;

namespace {

        int32_t errnoOf(Error err) {
                if (err == Error::BufferTooSmall) return -BMDS_EOVERFLOW;
                if (err == Error::CorruptData) return -BMDS_EIO;
                return Process::errnoFor(err);
        }

        // The request's options, checked.
        int32_t optionsOf(const bmds_image_decode_t &d, Images::Options &o) {
                if (d.format >= bmds::PixelFormatCount) return -BMDS_EINVAL;
                if ((d.flags & ~uint32_t(BMDS_IMAGE_ORIENT | BMDS_IMAGE_NO_HARDWARE)) != 0) return -BMDS_EINVAL;
                o.format = PixelFormat(d.format);
                o.background = d.background & 0xffffffu;
                o.minWidth = d.min_width;
                o.minHeight = d.min_height;
                o.boxWidth = d.box_width;
                o.boxHeight = d.box_height;
                o.orient = (d.flags & BMDS_IMAGE_ORIENT) != 0;
                o.hardware = (d.flags & BMDS_IMAGE_NO_HARDWARE) == 0;
                return 0;
        }

        // The file: @p fd read to its end, or @p data (copied: a program's memory may change).
        int32_t fileOf(KernelCall &call, int32_t fd, const void *data, uint32_t length, Images::FileBytes &file,
                       bmds::ByteSpan &bytes) {
                if (fd == -1) {
                        if (data == nullptr || length == 0) return -BMDS_EFAULT;
                        bytes = bmds::ByteSpan(static_cast<const uint8_t *>(data), length);
                        return 0;
                }
                const bmds::FileHandle::Ptr h = call->handle(fd);
                if (!h || !h->canRead()) return -BMDS_EBADF;
                const Error err = file.read(*h);
                if (err.isError()) return errnoOf(err);
                bytes = file.bytes();
                return 0;
        }

        void fill(const Images::Report &r, const bmds::ByteSpan file, const Images::Options &o, bmds_image_info_t &i) {
                i.codec = uint32_t(r.info.codec);
                i.width = r.info.width;
                i.height = r.info.height;
                i.flags = r.info.flags; // ImageInfo's bits are the SDK's
                i.orientation = r.info.orientation;
                i.decode_width = r.width;
                i.decode_height = r.height;
                i.decoder = uint32_t(r.plan.decoder);
                i.shift = r.plan.shift;
                // The codec's own layout, when it decodes as asked (not turned).
                if (r.plan.decoder == bmds::ImagePlan::Decoder::Hardware && !(o.orient && r.info.orientation != 1)) {
                        const bmds::JpegHeader h = bmds::JpegHeader::parse(file).value();
                        if (bmds::JpegCodec::isNative(h, o.format)) {
                                const bmds::JpegCodec::Layout l = bmds::JpegCodec::layoutFor(h, o.format);
                                i.align = uint32_t(bmds::JpegCodec::Align);
                                i.stride = uint32_t(l.stride);
                                i.bytes = uint32_t(l.bytes);
                        }
                }
                return;
        }

        Error toFd(void *arg, bmds::ByteSpan data) {
                auto *h = static_cast<bmds::FileHandle *>(arg);
                while (!data.isEmpty()) {
                        auto [n, err] = h->write(data, bmds::WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::IoError;
                        data = bmds::ByteSpan(data.data() + n, data.size() - n);
                }
                return Error::Ok;
        }

        struct Counted {
                        bmds::FileHandle *handle;
                        size_t            bytes = 0;
        };

        Error countedToFd(void *arg, bmds::ByteSpan data) {
                auto *c = static_cast<Counted *>(arg);
                c->bytes += data.size();
                return toFd(c->handle, data);
        }

} // namespace

extern "C" {

int32_t bmds_k_image_info(int32_t fd, const void *data, uint32_t length, const bmds_image_decode_t *want,
                          bmds_image_info_t *info) {
        KernelCall          call;
        bmds_image_decode_t d = {};
        d.size = sizeof(d);
        if (want != nullptr) {
                const int32_t r = bmds::detail::copyIn(want, d);
                if (r < 0) return r;
        }
        bmds_image_info_t out;
        int32_t           r = bmds::detail::copyIn(info, out);
        if (r < 0) return r;
        Images::Options o;
        r = optionsOf(d, o);
        if (r < 0) return r;
        Images::FileBytes file;
        bmds::ByteSpan    bytes;
        r = fileOf(call, fd, data, length, file, bytes);
        if (r < 0) return r;
        Images::Report report;
        const Error    err = Images::plan(bytes, o, report);
        out = bmds_image_info_t{};
        if (report.info.codec != ImageCodec::Unknown) fill(report, bytes, o, out); // what is known
        const int32_t c = bmds::detail::copyOut(out, info);
        return err.isError() ? errnoOf(err) : c;
}

int32_t bmds_k_image_decode(int32_t fd, const bmds_image_decode_t *request, bmds_image_info_t *info) {
        KernelCall          call;
        bmds_image_decode_t d;
        int32_t             r = bmds::detail::copyIn(request, d);
        if (r < 0) return r;
        bmds_image_info_t out = {};
        if (info != nullptr) {
                r = bmds::detail::copyIn(info, out);
                if (r < 0) return r;
        }
        Images::Options o;
        r = optionsOf(d, o);
        if (r < 0) return r;
        if (d.out == nullptr) return -BMDS_EFAULT;
        Images::FileBytes file;
        bmds::ByteSpan    bytes;
        r = fileOf(call, fd, d.data, d.length, file, bytes);
        if (r < 0) return r;
        Images::Report report;
        Error          err = Images::plan(bytes, o, report);
        if (err.isError()) return errnoOf(err);
        ImageBuffer buf;
        buf.pixels = static_cast<uint8_t *>(d.out);
        buf.width = report.width;
        buf.height = report.height;
        buf.format = o.format;
        buf.stride = d.stride != 0 ? d.stride : size_t(report.width) * bmds::bytesPerPixel(o.format);
        buf.capacity = d.out_size;
        if (buf.stride < size_t(report.width) * bmds::bytesPerPixel(o.format) || buf.bytes() > d.out_size)
                return -BMDS_EOVERFLOW;
        err = Images::decode(bytes, o, buf, &report);
        if (err.isError()) return errnoOf(err);
        if (info == nullptr) return 0;
        out = bmds_image_info_t{};
        fill(report, bytes, o, out);
        return bmds::detail::copyOut(out, info);
}

int32_t bmds_k_image_encode(int32_t fd, const bmds_image_encode_t *request) {
        KernelCall          call;
        bmds_image_encode_t e;
        const int32_t       r = bmds::detail::copyIn(request, e);
        if (r < 0) return r;
        if (e.pixels == nullptr) return -BMDS_EFAULT;
        if ((e.codec != BMDS_IMAGE_PNG && e.codec != BMDS_IMAGE_JPEG) || e.format >= bmds::PixelFormatCount ||
            e.width == 0 || e.height == 0 || e.quality > 100 || e.sampling > BMDS_JPEG_GRAY ||
            e.level > BMDS_PNG_STORED)
                return -BMDS_EINVAL;
        ImageBuffer in;
        in.pixels = const_cast<uint8_t *>(static_cast<const uint8_t *>(e.pixels)); // read only
        in.width = e.width;
        in.height = e.height;
        in.format = PixelFormat(e.format);
        in.stride = e.stride != 0 ? e.stride : size_t(e.width) * bmds::bytesPerPixel(in.format);
        if (!in.isValid()) return -BMDS_EINVAL;
        const bmds::FileHandle::Ptr h = call->handle(fd);
        if (!h || !h->canWrite()) return -BMDS_EBADF;
        Images::Encode how;
        how.codec = ImageCodec(e.codec);
        how.quality = uint8_t(e.quality == 0 ? 90 : e.quality);
        constexpr bmds::JpegHeader::Sampling Samplings[] = {
                bmds::JpegHeader::Sampling::Yuv420, bmds::JpegHeader::Sampling::Yuv422,
                bmds::JpegHeader::Sampling::Yuv444, bmds::JpegHeader::Sampling::Gray};
        how.sampling = Samplings[e.sampling];
        how.level = e.level == 0 ? 2 : e.level == BMDS_PNG_STORED ? 0 : int(e.level);
        Counted     c{h.get()};
        const Error err = Images::encode(in, how, countedToFd, &c);
        if (err.isError()) return errnoOf(err);
        return c.bytes > INT32_MAX ? INT32_MAX : int32_t(c.bytes);
}

} // extern "C"
