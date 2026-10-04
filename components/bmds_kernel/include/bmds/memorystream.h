/**
 * @file      memorystream.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/memspace.h>
#include <bmds/stream.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A stream over a growable memory buffer.
 *
 * Writes append; reads consume from the front and return `EndOfFile` once
 * everything written has been read. Used to capture the output of `$(...)`
 * and to hold here-documents. Never blocks; a write fails with `NoMem`
 * (nothing written) if the buffer can't grow.
 *
 * @par Thread Safety
 * Not thread-safe; used by one task.
 */
class MemoryStream final : public Stream {
        public:
                /**
                 * @brief Constructs an empty stream (no allocation until written).
                 * @param space Where the buffer grows.
                 */
                explicit MemoryStream(MemSpace space) : _data(space) {}

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;

                using Stream::write;

                /** @brief Returns everything written and not yet read. */
                StringView contents() const { return _data.view().mid(_readPos); }

                /** @brief Returns everything written and not yet read, as a String; empties the stream. */
                String takeContents();

                /** @brief Discards the contents and frees the buffer. */
                void clear();

        private:
                String _data;
                size_t _readPos = 0;
};

/**
 * @brief `/dev/null`: reads end at once, writes vanish.
 *
 * @par Thread Safety
 * Thread-safe (stateless).
 */
class NullStream final : public Stream {
        public:
                /** @brief Returns the shared instance. */
                static NullStream &instance();

                Result<size_t> read(WritableByteSpan, unsigned int) override {
                        return makeError<size_t>(Error::EndOfFile);
                }
                Result<size_t> write(ByteSpan data, unsigned int) override { return makeResult(data.size()); }

                using Stream::write;

                constexpr NullStream() = default;
};

BMDS_NAMESPACE_END
