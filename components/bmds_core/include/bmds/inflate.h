/**
 * @file      inflate.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Decompresses DEFLATE (RFC 1951) and zlib (RFC 1950) data in one
 *        go: the whole input in, the whole output out (which is the window,
 *        so no other memory is used). For PNG images (`devplan/gui.md` §4h);
 *        small and simple rather than fast.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace Inflate {

        /**
         * @brief Decompresses raw DEFLATE data @p in into @p out.
         * @return The bytes written; `CorruptData` (bad data, or input ending
         *         early), or `BufferTooSmall` (more output than @p out holds).
         */
        Result<size_t> raw(ByteSpan in, WritableByteSpan out);

        /**
         * @brief Decompresses a zlib stream (its header, DEFLATE data, Adler-32
         *        checked) @p in into @p out.
         * @return The bytes written; `CorruptData`, `NotSupported` (a preset
         *         dictionary, or not DEFLATE), or `BufferTooSmall`.
         */
        Result<size_t> zlib(ByteSpan in, WritableByteSpan out);

        /** @brief Returns the Adler-32 of @p data continuing from @p adler (1 to start). */
        uint32_t adler32(ByteSpan data, uint32_t adler = 1);

} // namespace Inflate

BMDS_NAMESPACE_END
