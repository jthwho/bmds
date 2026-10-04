/**
 * @file      programimage.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reading and moving a BMDS program file (the format in the SDK's
 *        bmds/program.h): ELF, linked at 0, relocations kept.
 *
 * `inspect()` checks everything the format promises (headers, the BMDS
 * note, the single segment, every relocation) without loading anything;
 * `load()` then copies the segment into memory, zeroes its bss and
 * applies the relocations for the address it will run at. The file is
 * read through a `Source`, so neither needs it in memory: the loader reads
 * from a file on the card, the host tests and `bmds-elfcheck` from memory.
 *
 * A file that fails is never guessed at: the reason is a `Problem`.
 *
 * @par Thread Safety
 * Stateless; reentrant.
 */
namespace ProgramImage {

        /** @brief Where the file comes from: exact reads at an offset. */
        class Source {
                public:
                        /**
                         * @brief Reads exactly `out.size()` bytes at @p offset.
                         * @return `Ok`, `EndOfFile` if the file is shorter, or the read's error.
                         */
                        virtual Error readAt(uint32_t offset, WritableByteSpan out) = 0;

                protected:
                        ~Source() = default;
        };

        /** @brief A source over bytes in memory. */
        class MemorySource final : public Source {
                public:
                        explicit MemorySource(ByteSpan bytes) : _bytes(bytes) {}
                        Error readAt(uint32_t offset, WritableByteSpan out) override;

                private:
                        ByteSpan _bytes;
        };

        /** @brief Why a file isn't a loadable program (for messages). */
        struct Problem {
                        const char *what = nullptr; ///< A fixed description, e.g. "unsupported relocation type".
                        uint32_t    value = 0;      ///< The offending value (a type, an offset), if `hasValue`.
                        bool        hasValue = false;
        };

        /** @brief What `inspect()` learns about a program. */
        struct Info {
                        uint32_t entry = 0;       ///< Entry point: an offset into the image.
                        uint32_t fileOffset = 0;  ///< Where the segment starts in the file.
                        uint32_t fileSize = 0;    ///< Bytes of the segment in the file.
                        uint32_t imageSize = 0;   ///< Bytes of memory it needs (with bss).
                        uint32_t abiVersion = 0;  ///< The ABI version it was built against.
                        uint32_t stackSize = 0;   ///< Stack it asks for (0: the default).
                        uint32_t relocations = 0; ///< Patches `load()` applies.
                        uint32_t tlsOffset = 0;   ///< Its thread-local block, if `tlsSize` (informational).
                        uint32_t tlsSize = 0;     ///< Bytes of thread-local storage.
        };

        /** @brief The largest image accepted (bytes of memory). */
        inline constexpr uint32_t MaxImageBytes = 16u * 1024u * 1024u;

        /**
         * @brief Checks that @p fileSize bytes from @p source are a loadable program.
         * @return `Ok` with @p info filled, `NotSupported` (not a BMDS program, or
         *         one this kernel can't run: @p problem says why), `Invalid` (a
         *         damaged file), or the source's read error.
         * @blocking Reads the file's headers and relocation sections.
         */
        Error inspect(Source &source, uint32_t fileSize, Info &info, Problem &problem);

        /**
         * @brief Loads an inspected program (@p fileSize bytes from @p source) into
         *        @p image (`info.imageSize` bytes) and relocates it to run at @p base.
         *
         * @p base is the address the image will have when it runs (a multiple of
         * 4096); on the target it is @p image itself. The caller syncs the caches
         * afterwards.
         *
         * @return `Ok`, or what `inspect()` would have said (the file changed).
         * @blocking
         */
        Error load(Source &source, uint32_t fileSize, const Info &info, uint8_t *image, uint32_t base,
                   Problem &problem);

} // namespace ProgramImage

BMDS_NAMESPACE_END
