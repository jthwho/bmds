/**
 * @file      imageplan.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Choosing a decoder (devplan/images.md §3d).
 */

#include <bmds/imageplan.h>
#include <bmds/softjpeg.h>

BMDS_NAMESPACE_BEGIN

Result<ImagePlan> ImagePlan::make(const ImageInfo &info, const JpegHeader *jpeg, const Want &want) {
        ImagePlan p;
        p.width = info.width;
        p.height = info.height;
        const auto fits = [&want](uint64_t w, uint64_t h) {
                return want.budgetPixels == 0 || w * h <= want.budgetPixels;
        };
        if (info.codec == ImageCodec::Png || info.codec == ImageCodec::Bmp) {
                if (!fits(info.width, info.height)) return makeError<ImagePlan>(Error::OutOfRange);
                p.decoder = info.codec == ImageCodec::Png ? Decoder::Png : Decoder::Bmp;
                return makeResult(p);
        }
        if (info.codec != ImageCodec::Jpeg || jpeg == nullptr) return makeError<ImagePlan>(Error::NotSupported);
        // The codec, whole, when it takes the picture and its padded size fits.
        if (want.hardware && jpeg->isHardware() && fits(jpeg->paddedWidth(), jpeg->paddedHeight())) {
                p.decoder = Decoder::Hardware;
                return makeResult(p);
        }
        if (!jpeg->isSoftware()) return makeError<ImagePlan>(Error::NotSupported);
        // TJpgDec: as far as the smallest wanted allows, and far enough to fit.
        unsigned shift = 0;
        while (shift < SoftJpeg::MaxShift && want.minWidth != 0 && want.minHeight != 0 &&
               SoftJpeg::reduced(info.width, shift + 1) >= want.minWidth &&
               SoftJpeg::reduced(info.height, shift + 1) >= want.minHeight)
                ++shift;
        while (!fits(SoftJpeg::reduced(info.width, shift), SoftJpeg::reduced(info.height, shift))) {
                if (shift == SoftJpeg::MaxShift) return makeError<ImagePlan>(Error::OutOfRange);
                ++shift;
        }
        p.decoder = Decoder::Software;
        p.shift = uint8_t(shift);
        p.width = SoftJpeg::reduced(info.width, shift);
        p.height = SoftJpeg::reduced(info.height, shift);
        return makeResult(p);
}

BMDS_NAMESPACE_END
