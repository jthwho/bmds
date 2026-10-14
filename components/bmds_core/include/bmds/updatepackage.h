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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An update package (`.bmup`, devplan/update.md §2; made by `tools/mkupdate`): the images
 *        to flash, their hashes, and a signature over them. An on-disk format: append only.
 *
 * Little-endian. The head, which a check reads first, is:
 * - a 136-byte header: `BMUP`, u16 format (1), u16 the header's size (136), u32 flags (0; any
 *   other is a package this code doesn't read),
 *   u32 the section count, u32 `BMDS_ABI_VERSION`, u16 the least chip revision (major * 100 +
 *   minor, as IDF's image header), u16 0, the board (32 bytes, `CONFIG_BMDS_BOARD_DIR`, NUL
 *   padded), the version's text (32, `PROJECT_VER`: "0.2.0+72cba49a"), the build id (32, the
 *   app's ELF SHA-256), i64 when it was made (seconds since 1970), the version (u16 major, minor,
 *   patch: `bmds/version.h`'s, what is compared), u16 0;
 * - the sections, 48 bytes each: u32 type, u32 flags (0, as the header's), u32 offset, u32 size,
 *   then BLAKE2s-256 of its bytes;
 * - an Ed25519 signature over the header and the sections.
 *
 * The sections' bytes follow, anywhere after the head within the file. Every hash is in the
 * signed bytes, so a package checks whole once its signature does and each section's bytes hash
 * as listed.
 *
 * @par Thread Safety
 * Pure functions over plain data.
 */
namespace UpdatePackage {

        /** @brief The header's bytes, a section's, and the most sections a package has. */
        constexpr size_t HeaderBytes = 136;
        constexpr size_t SectionBytes = 48; ///< See `HeaderBytes`.
        constexpr size_t MaxSections = 16;  ///< See `HeaderBytes`.

        /** @brief The most a head is: what to read before `parse()`. */
        constexpr size_t MaxHeadBytes = HeaderBytes + MaxSections * SectionBytes + Ed25519::SignatureBytes;

        /** @brief The format this code reads. */
        constexpr uint16_t Format = 1;

        /** @brief Bytes of a section's hash (BLAKE2s-256). */
        constexpr size_t HashBytes = 32;

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

        /** @brief One section. */
        struct Section {
                        uint32_t type = 0;
                        uint32_t offset = 0; ///< In the file.
                        uint32_t size = 0;
                        uint8_t  hash[HashBytes] = {};
        };

        /** @brief A package's head, parsed. */
        struct Package {
                        uint32_t abiVersion = 0;
                        uint16_t minChipRevision = 0;
                        char     board[33] = {};   ///< NUL-terminated.
                        char     versionText[33] = {}; ///< NUL-terminated (`PROJECT_VER`).
                        Version  version;
                        uint8_t  buildId[32] = {};
                        int64_t  madeAt = 0; ///< Seconds since 1970.
                        size_t   sectionCount = 0;
                        Section  sections[MaxSections];
                        size_t   headBytes = 0;   ///< The head's bytes, signature included.
                        size_t   signedBytes = 0; ///< The header and sections: what is signed.
                        uint8_t  signature[Ed25519::SignatureBytes] = {};

                        /** @brief Returns the first section of @p type, or nullptr. */
                        const Section *find(SectionType type) const;
        };

        /** @brief What a check found. */
        enum class Verdict : uint8_t {
                Ok,
                NotAPackage,  ///< No `BMUP`.
                NewerFormat,  ///< A format this code doesn't read.
                Malformed,    ///< The head's fields don't hold together.
                Truncated,    ///< Shorter than its head or a section says.
                BadSignature, ///< Signed by none of the keys, or altered since.
                BadHash,      ///< A section's bytes don't hash as listed.
        };

        /** @brief A public key packages may be signed with. */
        struct Key {
                        uint8_t bytes[Ed25519::PublicKeyBytes] = {};
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
         *        @p out: the fields, and that every section lies within the file after the head and
         *        no type is listed twice. The signature isn't checked here.
         */
        Verdict parse(Span<const uint8_t> head, uint64_t fileBytes, Package &out);

        /** @brief Returns `Ok` if @p package's signature over @p head is one of @p keys'. */
        Verdict verify(const Package &package, Span<const uint8_t> head, Span<const Key> keys);

} // namespace UpdatePackage

BMDS_NAMESPACE_END
