/**
 * @file      base64.h
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
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Decodes base64 (RFC 4648, the standard alphabet) as it arrives, in
 *        pieces of any size: OSC 52's payload (`devplan/clipboard.md` C11).
 *
 * Spaces, tabs, CRs and LFs are skipped. Padding is optional: a text that
 * ends without it is completed by `finish()`. After a bad character, or data
 * after padding, the decoder stays failed until `reset()`.
 *
 * @par Thread Safety
 * A value: one decoder per stream.
 */
class Base64Decoder {
        public:
                /** @brief The most bytes `put()` writes for @p chars characters. */
                static constexpr size_t maxOutput(size_t chars) { return (chars + 3) / 4 * 3; }

                /** @brief The most bytes `finish()` writes. */
                static constexpr size_t MaxFinish = 2;

                /**
                 * @brief Decodes @p text into @p out, which has room for
                 *        `maxOutput(text.size())` bytes.
                 * @return The bytes written (up to a failure: `isFailed()`).
                 */
                size_t put(StringView text, uint8_t *out);

                /**
                 * @brief Ends the text: writes what an unpadded end leaves (at most
                 *        `MaxFinish` bytes) into @p out and starts again.
                 * @return The bytes written, or `CorruptData` (a bad character,
                 *         data after padding, or an end in the middle of a byte).
                 */
                Result<size_t> finish(uint8_t *out);

                /** @brief Returns true after a bad character or data after padding. */
                bool isFailed() const { return _failed; }

                /** @brief Starts again. */
                void reset() {
                        _bits = 0;
                        _count = _pad = 0;
                        _failed = _ended = false;
                        return;
                }

        private:
                uint32_t _bits = 0;  // the quantum's sextets so far
                uint8_t  _count = 0; // sextets in it
                uint8_t  _pad = 0;   // padding characters seen in it
                bool     _failed = false;
                bool     _ended = false; // a padded quantum was the last
};

/**
 * @brief Encodes base64 (RFC 4648, the standard alphabet, padded) in pieces of
 *        any size: an OSC 52 query's reply.
 *
 * @par Thread Safety
 * A value: one encoder per stream.
 */
class Base64Encoder {
        public:
                /** @brief The most characters `put()` writes for @p bytes bytes. */
                static constexpr size_t maxOutput(size_t bytes) { return (bytes + 2) / 3 * 4; }

                /** @brief The most characters `finish()` writes. */
                static constexpr size_t MaxFinish = 4;

                /**
                 * @brief Encodes @p data into @p out, which has room for
                 *        `maxOutput(data.size())` characters.
                 * @return The characters written.
                 */
                size_t put(ByteSpan data, char *out);

                /**
                 * @brief Ends the data: writes the last, padded, quantum (at most
                 *        `MaxFinish` characters) into @p out and starts again.
                 * @return The characters written.
                 */
                size_t finish(char *out);

        private:
                uint8_t _held[2] = {}; // bytes not yet a whole quantum
                uint8_t _count = 0;
};

BMDS_NAMESPACE_END
