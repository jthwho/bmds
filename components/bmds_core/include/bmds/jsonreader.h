/**
 * @file      jsonreader.h
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
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Reads JSON (RFC 8259) a token at a time (`devplan/json.md` §3b): a
 *        pull parser in constant memory, over a whole text or a stream.
 *
 * @code
 * char       scratch[256];
 * JsonReader json(text, scratch, sizeof(scratch));
 * for (JsonReader::Token t = json.next(); t != JsonReader::Token::End; t = json.next()) {
 *         if (t == JsonReader::Token::Error) return json.error();
 *         ...
 * }
 * @endcode
 *
 * Keys and strings are decoded into the caller's scratch buffer (`text()`,
 * valid until the next call); a number's text is kept there as written, for
 * `toInt64()`, `toDouble()` and the rest. It is strict: what RFC 8259 doesn't
 * allow (comments, trailing commas, a byte-order mark, invalid UTF-8, lone
 * surrogates) is an error, as is nesting deeper than `MaxDepth`.
 *
 * Errors: `CorruptData` (not JSON: `line()` and `column()` say where),
 * `BufferTooSmall` (a key, string or number longer than the scratch),
 * `OutOfRange` (too deep), or the source's.
 *
 * @par Thread Safety
 * Not thread-safe: one reader, one thread. The text or buffers given must
 * outlive it.
 */
class JsonReader {
        public:
                /** @brief What `next()` found. */
                enum class Token : uint8_t {
                        BeginObject, ///< `{`.
                        EndObject,   ///< `}`.
                        BeginArray,  ///< `[`.
                        EndArray,    ///< `]`.
                        Key,         ///< An object's key: `text()`.
                        String,      ///< A string: `text()`.
                        Number,      ///< A number: `text()` as written, `toInt64()` and the rest.
                        True,        ///< `true`.
                        False,       ///< `false`.
                        Null,        ///< `null`.
                        End,         ///< The document is done (and only whitespace followed it).
                        Error,       ///< `error()`; every later call returns it again.
                };

                /**
                 * @brief Reads up to @p bytes of the document into @p buf.
                 * @return The bytes read (0 at the end), or the error that stops the reading.
                 */
                using Source = Result<size_t> (*)(void *arg, char *buf, size_t bytes);

                /** @brief The deepest nesting of objects and arrays. */
                static constexpr size_t MaxDepth = 64;

                /**
                 * @brief Reads the whole document @p text, decoding into @p scratch
                 *        (its last byte is kept for a NUL: `text()` holds at most
                 *        @p scratchBytes - 1).
                 */
                JsonReader(StringView text, char *scratch, size_t scratchBytes);

                /**
                 * @brief Reads a stream: @p source fills @p buffer (@p bufferBytes,
                 *        at least 1) as the reading needs; decoding into @p scratch.
                 */
                JsonReader(Source source, void *arg, char *buffer, size_t bufferBytes, char *scratch,
                           size_t scratchBytes);

                JsonReader(const JsonReader &) = delete;
                JsonReader &operator=(const JsonReader &) = delete;

                /** @brief Reads the next token. */
                Token next();

                /**
                 * @brief After a `BeginObject` or `BeginArray`, reads to its end (to
                 *        pass over what the caller doesn't know); after anything else,
                 *        nothing (the value was the token).
                 * @return `Ok`, or the error found.
                 */
                Error skip();

                /** @brief A `Key`'s or `String`'s decoded UTF-8 (it may hold NULs), or a `Number`'s text. */
                StringView text() const { return StringView(_scratch, _length); }

                /**
                 * @brief Reads the `Number` just returned as an integer, exactly.
                 * @return `Ok`; `Invalid` (it has a fraction or an exponent, or isn't
                 *         a number); `OutOfRange` (past the type).
                 */
                Error toInt64(int64_t &out) const;

                /** @brief As `toInt64()`, unsigned (a minus sign is `OutOfRange`). */
                Error toUint64(uint64_t &out) const;

                /** @brief Reads the `Number` just returned as a `double` (`strtod`). @return `Ok` or `Invalid`. */
                Error toDouble(double &out) const;

                /** @brief Reads the `Number` just returned as a `float` (`strtof`). @return `Ok` or `Invalid`. */
                Error toFloat(float &out) const;

                /** @brief Returns the error that stopped the reading (`Ok`: none). */
                bmds::Error error() const { return _error; }

                /** @brief Returns the line of the next character (from 1): where an error was found. */
                uint32_t line() const { return _line; }

                /** @brief Returns the column of the next character, in bytes (from 1). */
                uint32_t column() const { return _column; }

                /** @brief Returns how deep the reader is now (0: at the top). */
                size_t depth() const { return _depth; }

        private:
                enum class State : uint8_t {
                        Value,      // a value next (the top, after `:` or an array's `,`)
                        ValueOrEnd, // just after `[`
                        KeyOrEnd,   // just after `{`
                        Key,        // after an object's `,`
                        AfterKey,   // a key read: `:` and a value next
                        AfterValue, // `,`, an end, or (at the top) the end of the text
                        Done,       // `End` returned
                        Failed,     // `Error` returned
                };

                int   peek(); // the next byte, -1 at the end, -2 after the source failed
                void  advance();
                void  skipWhitespace();
                Token fail(bmds::Error err);
                Token value();
                Token string(Token kind);
                Token number();
                Token literal(const char *word, Token kind);
                Token close(bool isObject);
                bool  push(bool isObject);
                bool  store(char ch);
                bool  inObject() const { return _depth > 0 && ((_objects >> (_depth - 1)) & 1u) != 0; }

                Source      _source = nullptr;
                void       *_arg = nullptr;
                const char *_data;        // the text, or `_buffer`
                char       *_buffer;      // the stream's (null for a text)
                size_t      _bufferBytes; // its size
                size_t      _have;        // bytes in `_buffer`
                size_t      _pos = 0;     // the next of them
                char       *_scratch;
                size_t      _scratchBytes;
                size_t      _length = 0;
                uint64_t    _objects = 0; // a bit a level: 1 an object
                uint32_t    _line = 1;
                uint32_t    _column = 1;
                bmds::Error _error;
                State       _state = State::Value;
                uint8_t     _depth = 0;
                bool        _isNumber = false; // the token just read is a number
                bool        _integer = false;  // and has no fraction or exponent
                bool        _ended = false;    // the source returned 0
};

BMDS_NAMESPACE_END
