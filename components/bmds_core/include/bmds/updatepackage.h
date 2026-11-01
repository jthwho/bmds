/**
 * @file      updatepackage.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/ed25519.h>
#include <bmds/signedcontainer.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An update package (`.bmup`, devplan/update.md §2; made by `tools/mkupdate`): the images
 *        to flash, their hashes, and a signature over them, in a `SignedContainer`. An on-disk
 *        format: append only.
 *
 * Little-endian. The header (144 bytes) is the container's prefix (`BMUP`, format 1, the header's
 * size, flags, the section count, the key id), then: u32 `BMDS_ABI_VERSION`, u16 the least chip
 * revision (major * 100 + minor, as IDF's image header), u16 0, the board (32 bytes,
 * `CONFIG_BMDS_BOARD_DIR`, NUL padded), the version's text (32, `PROJECT_VER`:
 * "0.2.0+72cba49a"), the build id (32, the app's ELF SHA-256), i64 when it was made (seconds
 * since 1970), the version (u16 major, minor, patch: `bmds/version.h`'s, what is compared), u16 0.
 *
 * @par Thread Safety
 * Pure functions over plain data.
 */
namespace UpdatePackage {

        /** @brief The header's bytes, a section's, and the most sections a package has. */
        constexpr size_t HeaderBytes = 144;
        constexpr size_t SectionBytes = SignedContainer::SectionBytes; ///< See `HeaderBytes`.
        constexpr size_t MaxSections = SignedContainer::MaxSections;   ///< See `HeaderBytes`.

        /** @brief The format this code reads. */
        constexpr uint16_t Format = 1;

        /** @brief What an update package is to `SignedContainer`. */
        constexpr SignedContainer::Kind ContainerKind = {{'B', 'M', 'U', 'P'}, Format, HeaderBytes};

        /** @brief The most a head is: what to read before `parse()`. */
        constexpr size_t MaxHeadBytes = SignedContainer::maxHeadBytes(ContainerKind);

        /** @brief Bytes of a section's hash (BLAKE2s-256). */
        constexpr size_t HashBytes = SignedContainer::HashBytes;

        /** @brief What a section holds. Unknown types are skipped (newer packages, older code). */
        enum class SectionType : uint32_t {
                Firmware = 1, ///< The main firmware's image, for `ota_0`.
                Rom = 2,      ///< The ROM's image (`BMRO`), for `rom`.
                Recovery = 3, ///< The recovery firmware's image, for `ota_1`.
                Notes = 4,    ///< Release notes, UTF-8 text.
        };

        /** @brief A version (`BMDS_VERSION_*`), ordered major, minor, patch. */
        struct Version {
                        uint16_t major = 0;
                        uint16_t minor = 0;
                        uint16_t patch = 0;

                        /** @brief Orders versions: major, then minor, then patch. */
                        auto operator<=>(const Version &) const = default;
        };

        using Section = SignedContainer::Section;
        using Verdict = SignedContainer::Verdict;
        using Key = SignedContainer::Key;

        /** @brief A package's head, parsed. */
        struct Package : SignedContainer::Head {
                        uint32_t abiVersion = 0;
                        uint16_t minChipRevision = 0;
                        char     board[33] = {};       ///< NUL-terminated.
                        char     versionText[33] = {}; ///< NUL-terminated (`PROJECT_VER`).
                        Version  version;
                        uint8_t  buildId[32] = {};
                        int64_t  madeAt = 0; ///< Seconds since 1970.

                        using Head::find;

                        /** @brief Returns the first section of @p type, or nullptr. */
                        const Section *find(SectionType type) const { return find(static_cast<uint32_t>(type)); }
        };

        /**
         * @brief Reads the number at the start of a version's text ("0.2.0+72cba49a": 0.2.0) into
         *        @p out. @return false if it doesn't start with three dotted numbers below 65536.
         */
        bool parseVersion(const char *text, Version &out);

        /** @brief Returns a short English description of @p v ("its signature doesn't check"). */
        const char *verdictText(Verdict v);

        /**
         * @brief How many bytes the head is, from its first `HeaderBytes` in @p header (the section
         *        count). @return `Ok`, or why it isn't a package this code reads.
         */
        Verdict headBytes(Span<const uint8_t> header, size_t &bytes);

        /**
         * @brief Parses the head in @p head (at least `headBytes()`) of a file of @p fileBytes into
         *        @p out: the fields, and the container's checks (`SignedContainer::parse()`). The
         *        signature isn't checked here.
         */
        Verdict parse(Span<const uint8_t> head, uint64_t fileBytes, Package &out);

        /**
         * @brief Returns `Ok` if @p package's signature over @p head is by one of @p keys, its
         *        index in @p keys to @p index.
         */
        Verdict verify(const Package &package, Span<const uint8_t> head, Span<const Key> keys, size_t &index);

        /** @brief Bytes of an app image's header (IDF's `esp_image_header_t`). */
        constexpr size_t AppHeaderBytes = 24;

        /**
         * @brief The chip revisions an app image runs on (major * 100 + minor), from its header.
         *        A `max` of 0 or 0xFFFF means no greatest.
         */
        struct ChipRevisions {
                        uint16_t min = 0;
                        uint16_t max = 0xffff;
        };

        /**
         * @brief Reads the revisions from the app image header at the start of @p image into @p out.
         * @return false if @p image is shorter than `AppHeaderBytes`, or isn't an app image (its
         *         magic) for the chip @p chipId (`CONFIG_IDF_FIRMWARE_CHIP_ID`).
         */
        bool appRevisions(Span<const uint8_t> image, uint16_t chipId, ChipRevisions &out);

        /**
         * @brief Whether the bootloader would start an image of @p revisions on a chip of
         *        @p revision, by its rule: at least `min`; at most `max` when `max` is set, unless
         *        @p ignoreMax (the eFuse that disables the major version's check).
         */
        bool runsOn(const ChipRevisions &revisions, uint16_t revision, bool ignoreMax);

} // namespace UpdatePackage

BMDS_NAMESPACE_END
