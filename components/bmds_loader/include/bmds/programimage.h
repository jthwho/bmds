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
                        uint32_t flags = 0;       ///< Its `BMDS_PROGRAM_*` flags.
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

        /**
         * @brief A patch `read()` records: the offset of the word to patch, and
         *        `PatchUpper` for a `lui`/`auipc` (else a word gains the base).
         */
        using Patch = uint32_t;

        /** @brief In a `Patch`: the base goes into the upper immediate (`R_RISCV_HI20`). */
        inline constexpr Patch PatchUpper = 1u << 31;

        static_assert(MaxImageBytes <= PatchUpper, "a patch's offset fits below PatchUpper");

        /**
         * @brief Reads an inspected program (@p fileSize bytes from @p source) for
         *        placing later, any number of times: its segment, unrelocated, into
         *        @p bytes (`info.fileSize` bytes), and the patches `load()` would
         *        apply into @p patches (`info.relocations`).
         * @return As `load()`.
         * @blocking
         */
        Error read(Source &source, uint32_t fileSize, const Info &info, uint8_t *bytes, Patch *patches,
                   Problem &problem);

        /**
         * @brief Places a program `read()` gave: copies @p bytes into @p image
         *        (`info.imageSize` bytes), zeroes its bss and applies @p patches
         *        for @p base (a multiple of 4096). The same image as `load()`'s.
         * @nonblocking
         */
        void place(const Info &info, const uint8_t *bytes, const Patch *patches, uint8_t *image, uint32_t base);

        /** @brief Where a program file's icon is (`bmds_icon_note_t`, `devplan/gui.md` §4j). */
        struct IconRef {
                        uint16_t width = 0;
                        uint16_t height = 0;
                        uint32_t format = 0; ///< `BMDS_ICON_FORMAT_PNG`.
                        uint32_t offset = 0; ///< The image's bytes in the file.
                        uint32_t length = 0;
        };

        /**
         * @brief Finds the icon of the program in @p source (@p fileSize bytes)
         *        nearest @p side pixels: that size, else the next bigger, else
         *        the biggest. Only its place is found (nothing big is read).
         * @return `Ok`, `NotFound` (none: no icon notes, or not an ELF file),
         *         or an error reading.
         */
        Error findIcon(Source &source, uint32_t fileSize, uint16_t side, IconRef &out);

} // namespace ProgramImage

BMDS_NAMESPACE_END
