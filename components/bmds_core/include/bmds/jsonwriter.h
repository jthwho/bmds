/**
 * @file      jsonwriter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Writes JSON (RFC 8259) as a stream (`devplan/json.md` §3a): through
 *        a sink, from a small buffer of its own, so no document is ever held.
 *
 * @code
 * JsonWriter json(sink, &file);
 * json.beginObject();
 * json.member("format", 1);
 * json.key("results");
 * json.beginArray();
 * ...
 * json.endArray();
 * json.endObject();
 * if (Error err = json.finish(); err.isError()) return err;
 * @endcode
 *
 * Errors are sticky: the first (the sink's, or a misuse: a value where a key
 * belongs, an end that doesn't match, deeper than `MaxDepth`) stops all
 * writing, and `finish()` returns it.
 *
 * @par Thread Safety
 * Not thread-safe: one writer, one thread.
 */
class JsonWriter {
        public:
                /** @brief Takes @p bytes of output. @return `Ok`, or the error that stops the writing. */
                using Sink = Error (*)(void *arg, StringView bytes);

                /** @brief The deepest nesting of objects and arrays. */
                static constexpr size_t MaxDepth = 64;

                /** @brief Writes through @p sink, handing it @p arg. */
                JsonWriter(Sink sink, void *arg) : _sink(sink), _arg(arg) {}

                /** @brief Appends to @p out. */
                explicit JsonWriter(String &out) : _sink(appendTo), _arg(&out) {}

                JsonWriter(const JsonWriter &) = delete;
                JsonWriter &operator=(const JsonWriter &) = delete;

                /** @brief Pretty-prints, @p spaces a level (0, the default: compact). Set before writing. */
                void setIndent(uint8_t spaces) { _indent = spaces; }

                /** @brief Opens an object. */
                void beginObject() { open('{', true); }

                /** @brief Closes the object open. */
                void endObject() { close('}', true); }

                /** @brief Opens an array. */
                void beginArray() { open('[', false); }

                /** @brief Closes the array open. */
                void endArray() { close(']', false); }

                /** @brief Writes an object's key: a value must follow. */
                void key(StringView name);

                /** @brief Writes a string (UTF-8; escaped as needed). */
                void value(StringView text);

                /** @brief Writes a string (not a boolean: a pointer would convert to one). */
                void value(const char *text) { value(StringView(text)); }

                /** @brief Writes a boolean. */
                void value(bool flag) { scalar(flag ? StringView("true") : StringView("false")); }

                /** @brief Writes a number: shortest exact form; NaN and the infinities as `null`. */
                void value(double number);

                /** @brief Writes any integer exactly. */
                template <typename T>
                        requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
                void value(T number) {
                        if constexpr (std::is_signed_v<T>) {
                                writeSigned(static_cast<int64_t>(number));
                        } else {
                                writeUnsigned(static_cast<uint64_t>(number));
                        }
                        return;
                }

                /** @brief Writes `null`. */
                void null() { scalar("null"); }

                /** @brief Writes a key and its value. */
                template <typename T> void member(StringView name, const T &v) {
                        key(name);
                        value(v);
                        return;
                }

                /** @brief Writes a key and a string. */
                void member(StringView name, const char *text) {
                        key(name);
                        value(StringView(text));
                        return;
                }

                /**
                 * @brief Hands the rest of the output to the sink.
                 * @return `Ok`; the first error; or `Invalid` if a container is
                 *         still open (or nothing was written).
                 */
                Error finish();

                /** @brief Returns the first error so far (`Ok`: none). */
                Error error() const { return _error; }

                /** @brief Returns how deep the writer is now (0: at the top). */
                size_t depth() const { return _depth; }

        private:
                static Error appendTo(void *arg, StringView bytes);

                void open(char ch, bool isObject);
                void close(char ch, bool isObject);
                bool beginValue(); // the separator and indent before a value; false: stop
                void scalar(StringView text);
                void writeSigned(int64_t number);
                void writeUnsigned(uint64_t number);
                void writeString(StringView text);
                void newline();
                void put(StringView bytes);
                void put(char ch);
                void flush();
                void fail(Error err);
                bool inObject() const { return _depth > 0 && ((_objects >> (_depth - 1)) & 1u) != 0; }

                Sink     _sink;
                void    *_arg;
                uint64_t _objects = 0; // a bit a level: 1 an object
                Error    _error;
                uint8_t  _depth = 0;
                uint8_t  _indent = 0;
                bool     _first = true;     // nothing written yet in the container open
                bool     _afterKey = false; // a key written: its value next
                bool     _done = false;     // a whole value at the top written
                uint8_t  _used = 0;
                char     _buffer[128];
};

BMDS_NAMESPACE_END
