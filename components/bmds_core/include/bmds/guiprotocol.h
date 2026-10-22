/**
 * @file      guiprotocol.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/list.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The GUI protocol's numbers and its codec (`devplan/guiprograms.md` §4a, §8): what a
 *        program and the GUI service say on a connection, local or remote.
 *
 * Little-endian, every field 4-aligned. A request is `uint16 opcode`, `uint16 flags`, `uint32
 * length` (the whole message, a multiple of 4), its body. A reply, event or error is `uint16
 * kind`, `uint16 code`, `uint32 length`, `uint32 sequence`, its body. Values are tagged (`Tag`).
 * The numbers are userspace ABI once shipped: appended only. The SDK's C library (`bmds/gui.h`)
 * speaks the same bytes; the host tests check one against the other.
 *
 * @par Thread Safety
 * Values; a writer or reader is one caller's.
 */
namespace GuiProtocol {

        /** @brief The protocol's version: the major never changes once shipped; the minor grows. */
        constexpr uint16_t Major = 1;
        constexpr uint16_t Minor = 0;

        /** @brief `Hello`'s first 8 bytes. */
        constexpr char Magic[8] = {'B', 'M', 'D', 'S', 'G', 'U', 'I', '\0'};

        /** @brief A request's header's bytes, and an answer's. */
        constexpr size_t RequestHeaderBytes = 8;
        constexpr size_t AnswerHeaderBytes = 12;

        /** @brief The largest message either side sends (announced at setup). */
        constexpr uint32_t MaxMessage = 256u * 1024u;

        /** @brief Requests. */
        enum class Opcode : uint16_t {
                Hello = 0, ///< The setup: the connection's first message (sequence 0).
                InternNames = 1,
                NameOf = 2,
                Describe = 3,
                Create = 4,
                Destroy = 5,
                Set = 6,
                Get = 7,
                Call = 8,
                Listen = 9,
                PutImage = 10,  ///< Reserved (step 3).
                PutPixels = 11, ///< Reserved (step 3).
                Sync = 12,
                Bell = 13, ///< Reserved.
        };

        /** @brief `Call`'s flag: answer with the method's result. */
        constexpr uint16_t WantResult = 0x1;

        /** @brief What the server sends. */
        enum class Kind : uint16_t {
                Reply = 1, ///< `code`: the request's opcode.
                Event = 2, ///< `code`: 0; the body names the event.
                Error = 3, ///< `code`: an `ErrorCode`.
        };

        /** @brief Errors (`Kind::Error`'s code). */
        enum class ErrorCode : uint16_t {
                BadRequest = 1, ///< No such opcode, or a body that doesn't parse.
                BadLength = 2,
                BadId = 3,   ///< Not the client's, not there, or already used.
                BadName = 4, ///< An atom that isn't one, or a name the class hasn't.
                BadClass = 5,
                BadValue = 6, ///< A value of the wrong tag or out of range.
                BadAccess = 7,
                BadMatch = 8, ///< A parent that can't hold that child.
                NoMem = 9,
                BadImplementation = 10,
        };

        /** @brief `Hello`'s answer's status. */
        enum class HelloStatus : uint32_t {
                Accepted = 0,
                BadVersion = 1,
                Refused = 2,
                Busy = 3,
        };

        /** @brief A value's tag. */
        enum class Tag : uint32_t {
                None = 0,
                Int = 1,    ///< int32.
                Long = 2,   ///< int64.
                Bool = 3,   ///< uint32 0 or 1.
                String = 4, ///< uint32 length, UTF-8, padded.
                Atom = 5,
                Id = 6,
                Rect = 7,  ///< int32 x, y; uint32 width, height.
                Color = 8, ///< uint32 0xRRGGBB, uint32 a palette role's atom (0: the colour).
                List = 9,  ///< uint32 element tag, uint32 count, the elements untagged.
        };

        /** @brief A rectangle's value. */
        struct Rect {
                        int32_t  x = 0;
                        int32_t  y = 0;
                        uint32_t width = 0;
                        uint32_t height = 0;
        };

        /** @brief A decoded value; text and a list's elements point into the message. */
        struct Value {
                        Tag        tag = Tag::None;
                        int64_t    number = 0;           ///< Int, Long, Bool, Atom, Id, Color's RGB.
                        uint32_t   role = 0;             ///< Color's palette role (an atom; 0: none).
                        StringView text;                 ///< String.
                        Rect       rect;                 ///< Rect.
                        Tag        elements = Tag::None; ///< List: the elements' tag.
                        uint32_t   count = 0;            ///< List: how many.
                        ByteSpan   items;                ///< List: the elements' bytes (read with `Reader`).

                        static Value integer(int32_t v) { return make(Tag::Int, v); }
                        static Value boolean(bool v) { return make(Tag::Bool, v ? 1 : 0); }
                        static Value atom(uint32_t v) { return make(Tag::Atom, v); }
                        static Value id(uint32_t v) { return make(Tag::Id, v); }
                        static Value string(StringView v) {
                                Value r;
                                r.tag = Tag::String;
                                r.text = v;
                                return r;
                        }

                private:
                        static Value make(Tag tag, int64_t number) {
                                Value r;
                                r.tag = tag;
                                r.number = number;
                                return r;
                        }
        };

        /** @brief Returns the bytes @p n takes once padded to 4. */
        constexpr size_t padded(size_t n) {
                return (n + 3) & ~size_t(3);
        }

        /**
         * @brief Writes messages into a growing buffer: `begin...()`, the fields, `end()` (which
         *        sets the length and pads). A failed allocation is kept (`ok()`) and the rest
         *        ignored.
         */
        class Writer {
                public:
                        explicit Writer(List<uint8_t> &out) : _out(out) {}

                        void beginRequest(Opcode opcode, uint16_t flags = 0);
                        void beginAnswer(Kind kind, uint16_t code, uint32_t sequence);
                        /** @brief Ends the message begun: its length set. */
                        void end();

                        void u16(uint16_t v);
                        void u32(uint32_t v);
                        void i32(int32_t v) { u32(uint32_t(v)); }
                        void i64(int64_t v);
                        void bytes(ByteSpan data); ///< Padded to 4.
                        void string(StringView s); ///< Length, bytes, padding.
                        void value(const Value &v);
                        /** @brief A list's tag, element tag and count; the elements follow, untagged. */
                        void listHeader(Tag elements, uint32_t count);
                        /** @brief One element of a list: @p v without its tag. */
                        void element(const Value &v);

                        bool ok() const { return _ok; }

                private:
                        void put(const uint8_t *p, size_t n);

                        List<uint8_t> &_out;
                        size_t         _start = 0;
                        bool           _ok = true;
        };

        /** @brief Reads fields from a message's bytes; past the end, `ok()` turns false and reads give 0. */
        class Reader {
                public:
                        explicit Reader(ByteSpan data) : _data(data) {}

                        uint16_t   u16();
                        uint32_t   u32();
                        int32_t    i32() { return int32_t(u32()); }
                        int64_t    i64();
                        ByteSpan   bytes(size_t n); ///< And skips the padding.
                        StringView string();
                        bool       value(Value &out);            ///< False: not a value (and `ok()` false).
                        bool       element(Tag tag, Value &out); ///< A list's element of @p tag.
                        void       skip(size_t n);

                        bool   ok() const { return _ok; }
                        size_t remaining() const { return _ok ? _data.size() - _at : 0; }
                        size_t position() const { return _at; }

                private:
                        bool fits(size_t n);

                        ByteSpan _data;
                        size_t   _at = 0;
                        bool     _ok = true;
        };

        /**
         * @brief How long the message at the start of @p buffered is, from its header (@p header
         *        bytes: `RequestHeaderBytes` or `AnswerHeaderBytes`).
         * @return Its length; 0 if the header isn't all there yet; -1 if the length is wrong (not a
         *         multiple of 4, shorter than the header, or longer than `MaxMessage`).
         */
        int64_t messageLength(ByteSpan buffered, size_t header);

} // namespace GuiProtocol

BMDS_NAMESPACE_END
