/**
 * @file      pkgfile.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/result.h>
#include <bmds/signedcontainer.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Packages (`NAME_VERSION.bmpk`) and package indexes (`index.bmpi`), devplan/packages.md
 *        §3 and §8; made by `tools/mkpkg`. Both are `SignedContainer`s. On-disk formats: append
 *        only.
 *
 * Little-endian, text fields NUL padded. A package's header (128 bytes) is the container's
 * prefix (`BMPK`, format 1), then: the name (32 bytes, `isValidName()`), the version (32,
 * `PkgVersion::isValid()`), the architecture (16, `isValidArch()`: `esp32p4`, or `all` for data),
 * u32 the least `BMDS_ABI_VERSION`, u32 0, u64 the installed bytes (the files' sizes added up),
 * i64 when it was made (seconds since 1970). Its sections: the manifest (`PkgManifest`), the data,
 * the description and the icons.
 *
 * The data is each `f` entry of the manifest in turn, in blocks of `BlockBytes` (the last of a
 * file shorter; an empty file has none). A block is a u32, its low 31 bits the bytes that follow
 * and its top bit set if they are stored as they are, then those bytes: raw DEFLATE that
 * inflates to exactly the block's size, or the block itself. So a block is decoded with one
 * `BlockBytes` buffer each way, for a file of any size.
 *
 * An index's header (72 bytes) is the prefix (`BMPI`, format 1), then i64 when it was made, u32
 * the number of packages listed, u32 0, and a label (32 bytes: the repository's name, printable
 * ASCII, may be empty). Its sections: the stanzas (`PkgIndex`), and the listed packages' icons, so
 * a store shows packages that aren't installed.
 *
 * An icons section is a run of entries: u8 the name's bytes, the name (a package's, in an
 * index; none in a package, whose icons are its own), u16 width, u16 height, u32 the PNG's bytes,
 * then the PNG, whose own header must say the same size. Square, 1 to `MaxIconSide` pixels, at
 * most `MaxIconBytes`; `IconReader` reads them.
 *
 * @par Thread Safety
 * Pure functions over plain data.
 */
namespace PkgFile {

        /** @brief The formats this code reads. */
        constexpr uint16_t Format = 1;

        /** @brief A package's header bytes and an index's. */
        constexpr size_t PackageHeaderBytes = 128;
        constexpr size_t IndexHeaderBytes = 72; ///< See `PackageHeaderBytes`.

        /** @brief What a package and an index are to `SignedContainer`. */
        constexpr SignedContainer::Kind PackageKind = {{'B', 'M', 'P', 'K'}, Format, PackageHeaderBytes};
        constexpr SignedContainer::Kind IndexKind = {
                {'B', 'M', 'P', 'I'}, Format, IndexHeaderBytes}; ///< See `PackageKind`.

        /** @brief The most a package's head is, and an index's: what to read before parsing. */
        constexpr size_t MaxPackageHeadBytes = SignedContainer::maxHeadBytes(PackageKind);
        constexpr size_t MaxIndexHeadBytes = SignedContainer::maxHeadBytes(IndexKind); ///< See `MaxPackageHeadBytes`.

        /** @brief The longest name, version and architecture (their fields' sizes). */
        constexpr size_t NameBytes = 32;
        constexpr size_t VersionBytes = 32; ///< See `NameBytes`.
        constexpr size_t ArchBytes = 16;    ///< See `NameBytes`.
        constexpr size_t LabelBytes = 32;   ///< An index's label.

        /** @brief A data block's most bytes, decoded; and its size word's stored flag. */
        constexpr size_t   BlockBytes = 65536;
        constexpr uint32_t BlockStored = 0x80000000u; ///< See `BlockBytes`.

        /** @brief An icon's largest side and bytes. */
        constexpr uint16_t MaxIconSide = 64;
        constexpr size_t   MaxIconBytes = 16384; ///< See `MaxIconSide`.

        /** @brief The architecture of a package that runs anywhere (data). */
        constexpr const char *ArchAll = "all";

        /** @brief What a package's section holds. Unknown types are skipped. */
        enum class SectionType : uint32_t {
                Manifest = 1,    ///< The entries and fields, UTF-8 text (`PkgManifest`).
                Data = 2,        ///< The files' bytes, in blocks.
                Description = 3, ///< What the package is, UTF-8 text.
                Icons = 4,       ///< Its icons (16 and 32 pixels, as the theme's), PNG.
        };

        /** @brief What an index's section holds. */
        enum class IndexSectionType : uint32_t {
                Stanzas = 1, ///< A stanza per package, UTF-8 text (`PkgIndex`).
                Icons = 2,   ///< The packages' icons, PNG, each named.
        };

        using Section = SignedContainer::Section;
        using Verdict = SignedContainer::Verdict;
        using Key = SignedContainer::Key;

        /** @brief A package's head, parsed. */
        struct Package : SignedContainer::Head {
                        char     name[NameBytes + 1] = {};       ///< NUL-terminated.
                        char     version[VersionBytes + 1] = {}; ///< NUL-terminated.
                        char     arch[ArchBytes + 1] = {};       ///< NUL-terminated.
                        uint32_t abiVersion = 0;
                        uint64_t installedBytes = 0;
                        int64_t  madeAt = 0; ///< Seconds since 1970.

                        using Head::find;

                        /** @brief Returns the first section of @p type, or nullptr. */
                        const Section *find(SectionType type) const { return find(static_cast<uint32_t>(type)); }
        };

        /** @brief An index's head, parsed. */
        struct Index : SignedContainer::Head {
                        int64_t  madeAt = 0; ///< Seconds since 1970.
                        uint32_t packageCount = 0;
                        char     label[LabelBytes + 1] = {}; ///< NUL-terminated.

                        using Head::find;

                        /** @brief Returns the first section of @p type, or nullptr. */
                        const Section *find(IndexSectionType type) const { return find(static_cast<uint32_t>(type)); }
        };

        /**
         * @brief Returns true if @p name is a package's name: 1 to `NameBytes` of lowercase
         *        letters, digits and `+ - .`, starting with a letter or digit.
         */
        bool isValidName(StringView name);

        /** @brief Returns true if @p arch is an architecture: 1 to `ArchBytes` of lowercase letters and digits. */
        bool isValidArch(StringView arch);

        /** @brief Returns a short English description of @p v ("not a package"). */
        const char *verdictText(Verdict v);

        /**
         * @brief Parses a package's head in @p head (at least `SignedContainer::headBytes()`) of a
         *        file of @p fileBytes into @p out: the container's checks, and the fields' (a valid
         *        name, version and architecture). The signature isn't checked here.
         */
        Verdict parsePackage(Span<const uint8_t> head, uint64_t fileBytes, Package &out);

        /** @brief Parses an index's head, as `parsePackage()`. */
        Verdict parseIndex(Span<const uint8_t> head, uint64_t fileBytes, Index &out);

        /**
         * @brief Writes an unsigned index (`SignedContainer::buildUnsigned()`) to @p out: its
         *        @p stanzas (`PkgIndex` text) and @p icons (named entries, `appendIcon()`; no
         *        section when empty), listing @p count packages, made at @p madeAt, labelled
         *        @p label (printable ASCII, at most `LabelBytes`).
         * @return `Ok`; `Invalid` (the label); `NoMem`.
         */
        Error buildIndex(StringView stanzas, ByteSpan icons, uint32_t count, int64_t madeAt, StringView label,
                         List<uint8_t> &out);

        /**
         * @brief Appends an icons section's entry to @p section: @p name (a package's, or empty in a
         *        package's own section), the square @p side, and @p png.
         * @return `Ok`; `Invalid` (a name over 255 bytes, a side of 0 or over `MaxIconSide`, a PNG
         *         over `MaxIconBytes`); `NoMem`.
         */
        Error appendIcon(List<uint8_t> &section, StringView name, uint16_t side, ByteSpan png);

        /** @brief A data block's size word, read. */
        struct Block {
                        uint32_t bytes = 0;      ///< That follow the word.
                        bool     stored = false; ///< As they are, not compressed.
        };

        /**
         * @brief Reads a block's size word @p word (its 4 bytes, little-endian, as a number) into
         *        @p out, for a block that decodes to @p decodedBytes (1 to `BlockBytes`).
         * @return `CorruptData` if the block can't be one: stored but not @p decodedBytes, or
         *         compressed into more than `BlockBytes` or none.
         */
        Error readBlock(uint32_t word, size_t decodedBytes, Block &out);

        /**
         * @brief Decodes a block's bytes @p in (after its word) into @p out, which must be exactly
         *        the block's decoded size (1 to `BlockBytes`).
         * @return `CorruptData` if they don't decode to exactly that.
         */
        Error decodeBlock(const Block &block, ByteSpan in, WritableByteSpan out);

        /** @brief One icon of an icons section. Views into the section. */
        struct Icon {
                        StringView name; ///< The package's (an index's), or empty (a package's own).
                        uint16_t   side = 0;
                        ByteSpan   png;
        };

        /** @brief Reads an icons section's entries in turn. */
        class IconReader {
                public:
                        /**
                         * @brief A reader of @p section, which must outlive it; @p named for an
                         *        index's (each entry names a valid package), not for a package's
                         *        (no entry has a name).
                         */
                        IconReader(ByteSpan section, bool named) : _section(section), _named(named) {}

                        /**
                         * @brief Reads the next icon into @p out.
                         * @return `Ok`; `EndOfFile` after the last; `CorruptData` for an entry
                         *         that isn't one (cut short, a bad name or size, not a PNG of
                         *         its size).
                         */
                        Error next(Icon &out);

                private:
                        ByteSpan _section;
                        size_t   _pos = 0;
                        bool     _named = false;
        };

} // namespace PkgFile

BMDS_NAMESPACE_END
