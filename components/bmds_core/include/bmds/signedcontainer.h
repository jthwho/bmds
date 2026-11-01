/**
 * @file      signedcontainer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/ed25519.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The signed container that update packages (`.bmup`), packages (`.bmpk`) and package
 *        indexes (`.bmpi`) share (devplan/packages.md §3): a header, a table of sections with
 *        their hashes, and a signature over both. An on-disk format: append only.
 *
 * Little-endian. The head, which a check reads first, is:
 * - the header: a 24-byte prefix every kind shares, then the kind's own fields. The prefix is
 *   the magic (4 bytes), u16 the format, u16 the header's size, u32 flags (0; any other is a
 *   file this code doesn't read), u32 the section count, then the signing key's id (8 bytes,
 *   `keyId()`);
 * - the sections, 48 bytes each: u32 type, u32 flags (0, as the header's), u32 offset, u32 size,
 *   then BLAKE2s-256 of its bytes;
 * - an Ed25519 signature over the header and the sections.
 *
 * The sections' bytes follow, anywhere after the head within the file. Every hash is in the
 * signed bytes, so a file checks whole once its signature does and each section's bytes hash as
 * listed. No section type is listed twice; type 0 is never one.
 *
 * An **unsigned** file has a zero key id and a zero signature (`isUnsigned()`): what a board
 * writes, having no private key (an index of a card's folder, devplan/packages.md §21). Only a
 * caller that needs no signature accepts one.
 *
 * @par Thread Safety
 * Pure functions over plain data.
 */
namespace SignedContainer {

        /** @brief Bytes of the prefix every header starts with, and of a section's entry. */
        constexpr size_t PrefixBytes = 24;
        constexpr size_t SectionBytes = 48; ///< See `PrefixBytes`.

        /** @brief The most sections a file has. */
        constexpr size_t MaxSections = 16;

        /** @brief Bytes of a section's hash (BLAKE2s-256). */
        constexpr size_t HashBytes = 32;

        /** @brief Bytes of a key's id: the first of the BLAKE2s-256 of its public key. */
        constexpr size_t KeyIdBytes = 8;

        /** @brief What a check found. */
        enum class Verdict : uint8_t {
                Ok,
                NotAPackage,  ///< Not the magic expected.
                NewerFormat,  ///< A format, or flags, this code doesn't read.
                Malformed,    ///< The head's fields don't hold together.
                Truncated,    ///< Shorter than its head or a section says.
                BadSignature, ///< Signed by none of the keys, or altered since.
                BadHash,      ///< A section's bytes don't hash as listed.
        };

        /** @brief A public key a file may be signed with. */
        struct Key {
                        uint8_t bytes[Ed25519::PublicKeyBytes] = {};
        };

        /** @brief What a caller expects: the kind's magic, its format, and its header's size. */
        struct Kind {
                        char     magic[4] = {};
                        uint16_t format = 0;
                        uint16_t headerBytes = 0; ///< `PrefixBytes` and the kind's own fields.
        };

        /** @brief One section. */
        struct Section {
                        uint32_t type = 0;
                        uint32_t offset = 0; ///< In the file.
                        uint32_t size = 0;
                        uint8_t  hash[HashBytes] = {};
        };

        /** @brief A file's head, parsed: what every kind has. */
        struct Head {
                        uint8_t keyId[KeyIdBytes] = {};
                        size_t  sectionCount = 0;
                        Section sections[MaxSections];
                        size_t  headBytes = 0;   ///< The head's bytes, signature included.
                        size_t  signedBytes = 0; ///< The header and sections: what is signed.
                        uint8_t signature[Ed25519::SignatureBytes] = {};

                        /** @brief Returns the first section of @p type, or nullptr. */
                        const Section *find(uint32_t type) const;
        };

        /** @brief The most a head of @p kind is: what to read before `parse()`. */
        constexpr size_t maxHeadBytes(const Kind &kind) {
                return kind.headerBytes + MaxSections * SectionBytes + Ed25519::SignatureBytes;
        }

        /** @brief A section to write: its type and bytes. */
        struct Part {
                        uint32_t type = 0;
                        ByteSpan bytes;
        };

        /** @brief Returns true if @p parsed is unsigned: its key id and signature are zero. */
        bool isUnsigned(const Head &parsed);

        /**
         * @brief Writes an unsigned file of @p kind to @p out: the prefix, @p body (the kind's own
         *        fields, `kind.headerBytes - PrefixBytes` bytes), the table of @p parts with their
         *        hashes, a zero signature, then each part's bytes in turn.
         * @return `Ok`; `Invalid` (@p body the wrong size, too many parts, a type 0 or listed
         *         twice, or more than 4 GB); `NoMem`.
         */
        Error buildUnsigned(const Kind &kind, ByteSpan body, Span<const Part> parts, List<uint8_t> &out);

        /** @brief The longest comment a key line may have. */
        constexpr size_t MaxKeyCommentBytes = 255;

        /**
         * @brief Reads a key line, keys/'s format: `ed25519`, a space, 64 hex digits, then
         *        optionally a space and a comment (UTF-8 without control characters, at most
         *        `MaxKeyCommentBytes`) to the line's end. The key to @p key; the comment, trimmed,
         *        to @p comment (a view into @p line).
         * @return `Ok`, or `CorruptData` for a line that isn't one.
         */
        Error parseKey(StringView line, Key &key, StringView &comment);

        /** @brief Writes @p key's id to @p out. */
        void keyId(const Key &key, uint8_t (&out)[KeyIdBytes]);

        /** @brief Returns a short English description of @p v ("its signature doesn't check"). */
        const char *verdictText(Verdict v);

        /**
         * @brief How many bytes the head is, from its header in @p header (at least
         *        `kind.headerBytes`; the section count). @return `Ok`, or why it isn't a file of
         *        @p kind this code reads.
         */
        Verdict headBytes(Span<const uint8_t> header, const Kind &kind, size_t &bytes);

        /**
         * @brief Parses the head in @p head (at least `headBytes()`) of a file of @p fileBytes into
         *        @p out: the prefix, and that every section lies within the file after the head and
         *        no type is listed twice. The kind's own fields are the caller's; the signature
         *        isn't checked here.
         */
        Verdict parse(Span<const uint8_t> head, uint64_t fileBytes, const Kind &kind, Head &out);

        /**
         * @brief Returns `Ok` if @p parsed's signature over @p head is by one of @p keys: only keys
         *        with the head's key id are tried. Its index in @p keys to @p index on `Ok`.
         */
        Verdict verify(const Head &parsed, Span<const uint8_t> head, Span<const Key> keys, size_t &index);

} // namespace SignedContainer

BMDS_NAMESPACE_END
