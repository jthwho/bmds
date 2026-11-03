/**
 * @file      types.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Preso's core (devplan/preso.md §2): what every part shares. The core includes no platform
 * header (no BMDS, no SDL) and no C++ standard library, so it builds for the board, the host
 * and a PC alike.
 */

#ifndef PRESO_TYPES_H
#define PRESO_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace preso {

        /** @brief Why something failed. */
        enum class Error : uint8_t {
                Ok = 0,
                NoMemory,     ///< An allocation failed.
                NotFound,     ///< No such file.
                Io,           ///< Reading or writing failed.
                TooBig,       ///< Past a limit of Preso's (a deck's size).
                Invalid,      ///< Not something Preso can use.
                NotSupported, ///< The platform can't do it.
        };

        /** @brief A name for @p error, for messages. */
        const char *errorName(Error error);

        /**
         * @brief Bytes that live elsewhere (a deck's source, an arena): a pointer and a length,
         *        not NUL-terminated.
         *
         * @par Thread Safety
         * A value: as safe as the bytes it points at.
         */
        class StringView {
                public:
                        constexpr StringView() = default;
                        constexpr StringView(const char *data, uint32_t length) : _data(data), _length(length) {}
                        StringView(const char *text)
                            : _data(text), _length(text != nullptr ? uint32_t(strlen(text)) : 0) {}

                        const char *data() const { return _data; }
                        uint32_t    length() const { return _length; }
                        bool        isEmpty() const { return _length == 0; }
                        char        operator[](uint32_t i) const { return _data[i]; }

                        bool equals(StringView other) const {
                                return _length == other._length &&
                                       (_length == 0 || memcmp(_data, other._data, _length) == 0);
                        }
                        bool operator==(StringView other) const { return equals(other); }
                        /** @brief The same, ASCII letters in either case. */
                        bool equalsIgnoringCase(StringView other) const;
                        bool startsWith(StringView prefix) const {
                                return prefix._length <= _length && memcmp(_data, prefix._data, prefix._length) == 0;
                        }
                        bool endsWith(StringView suffix) const {
                                return suffix._length <= _length &&
                                       memcmp(_data + _length - suffix._length, suffix._data, suffix._length) == 0;
                        }
                        /** @brief Where @p c first is, or -1. */
                        int32_t indexOf(char c, uint32_t from = 0) const;
                        /** @brief Where @p text first is, or -1. */
                        int32_t indexOf(StringView text, uint32_t from = 0) const;

                        StringView mid(uint32_t start, uint32_t length = UINT32_MAX) const;
                        StringView left(uint32_t length) const { return mid(0, length); }
                        /** @brief Without spaces, tabs and line ends at either end. */
                        StringView trimmed() const;

                        /**
                         * @brief A whole decimal number (an optional sign, digits), or @p fallback if
                         *        it isn't one or doesn't fit.
                         */
                        int32_t toInt(int32_t fallback) const;
                        /** @brief A decimal number with an optional fraction, or @p fallback. */
                        float toFloat(float fallback) const;

                private:
                        const char *_data = nullptr;
                        uint32_t    _length = 0;
        };

        /** @brief Spaces and tabs (not line ends). */
        inline bool isBlank(char c) {
                return c == ' ' || c == '\t';
        }
        /** @brief Spaces, tabs and line ends. */
        inline bool isSpace(char c) {
                return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        /**
         * @brief Calls @p each with every piece of @p text between runs of spaces (spaces, tabs,
         *        line ends), in order.
         */
        template <typename Fn> void forEachWord(StringView text, Fn each) {
                uint32_t i = 0;
                while (i < text.length()) {
                        while (i < text.length() && isSpace(text[i])) i++;
                        uint32_t start = i;
                        while (i < text.length() && !isSpace(text[i])) i++;
                        if (i > start) each(text.mid(start, i - start));
                }
                return;
        }

        /**
         * @brief Calls @p each with every line of @p text (without its `\n` or `\r\n`) and the
         *        offset it starts at.
         */
        template <typename Fn> void forEachLine(StringView text, Fn each) {
                uint32_t start = 0;
                while (start < text.length()) {
                        int32_t    end = text.indexOf('\n', start);
                        uint32_t   stop = end < 0 ? text.length() : uint32_t(end);
                        StringView line = text.mid(start, stop - start);
                        if (line.endsWith("\r")) line = line.left(line.length() - 1);
                        each(line, start);
                        start = stop + 1;
                }
                return;
        }

} // namespace preso

#endif // PRESO_TYPES_H
