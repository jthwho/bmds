/**
 * @file      clipboardreader.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/clipboard.h>
#include <bmds/clipboardconversions.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads a selection as the type wanted, converted on the way
 *        (`devplan/clipboard.md` §4b): what `clip`, `/dev/clip0` and the
 *        toolkit read with.
 *
 * `open()` finds the shortest way from an offered type (`ClipboardConversions`),
 * asks for that type, and `read()` passes it through the built-in converters
 * on it. A way through a command (a filter program) is `NotSupported` until
 * the reader can start one (step 6).
 *
 * About 2 KB: made in PSRAM by its users, not on a stack.
 *
 * @par Thread Safety
 * One reader per thread.
 */
class ClipboardReader {
        public:
                /** @brief Reads from @p clipboard. */
                explicit ClipboardReader(Clipboard &clipboard) : _clipboard(clipboard) {}

                ClipboardReader(const ClipboardReader &) = delete;
                ClipboardReader &operator=(const ClipboardReader &) = delete;

                /**
                 * @brief Opens @p selection as @p wanted.
                 * @return `Ok`, `Empty` (nothing there), `NotFound` (no way to that
                 *         type), `Invalid` (not a type), `NotSupported` (only a
                 *         command converts), or `Clipboard::request()`'s errors.
                 */
                Error open(Clipboard::Selection selection, StringView wanted);

                /**
                 * @brief Reads converted bytes, waiting up to @p timeoutMs for the
                 *        owner's.
                 * @return Bytes read (at least one); 0 with `EndOfFile` at the end;
                 *         `Timeout`, `Interrupted`, ...
                 * @blocking
                 */
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs);

                /** @brief Returns the offered type being read (normal form; empty before `open()`). */
                StringView offeredType() const { return StringView(_offered); }

                /** @brief Returns the converters on the way (0: as offered). */
                size_t steps() const { return _steps; }

        private:
                static constexpr size_t InBytes = 256;
                static constexpr size_t MidBytes = ClipboardConverter::maxOutput(InBytes);
                static constexpr size_t OutBytes = ClipboardConverter::maxOutput(MidBytes);

                // Fills _out from the source through the converters; false at the end.
                Result<bool> fill(unsigned int timeoutMs);

                Clipboard         &_clipboard;
                FileHandle::Ptr    _source;
                ClipboardConverter _converters[ClipboardConversions::MaxSteps] = {
                        ClipboardConverter(ClipboardConversions::Builtin::None),
                        ClipboardConverter(ClipboardConversions::Builtin::None)};
                uint8_t _steps = 0;
                bool    _ended = false;
                size_t  _outAt = 0;
                size_t  _outLength = 0;
                char    _offered[ClipboardType::BufferBytes] = {};
                uint8_t _in[InBytes];
                uint8_t _mid[MidBytes];
                uint8_t _out[OutBytes];
};

BMDS_NAMESPACE_END
