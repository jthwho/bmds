/**
 * @file      stringview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <compare>
#include <string_view>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Non-owning, read-only view of a character sequence.
 *
 * A thin, constexpr wrapper over `std::string_view` with a Qt-style API.
 * The viewed characters are not necessarily NUL-terminated. The view does
 * not own the data; the caller ensures it outlives the view.
 *
 * Substring operations (`left()`, `right()`, `mid()`) clamp out-of-range
 * arguments rather than failing.
 *
 * @par Thread Safety
 * Distinct instances may be used concurrently. Read-only; safe to share
 * between tasks as long as the underlying data doesn't change.
 */
class StringView {
        public:
                /** @brief The underlying standard type. */
                using StdView = std::string_view;

                /** @brief Iterator type. */
                using ConstIterator = StdView::const_iterator;

                /** @brief "Not found" / "to the end" index. */
                static constexpr size_t NPos = StdView::npos;

                /** @brief Constructs an empty view. */
                constexpr StringView() = default;

                /**
                 * @brief Views a NUL-terminated string.
                 * @param str The string; null gives an empty view.
                 */
                constexpr StringView(const char *str) : _view(str != nullptr ? StdView(str) : StdView()) {}

                /**
                 * @brief Views @p size characters starting at @p data.
                 * @param data Start of the characters.
                 * @param size Number of characters.
                 */
                constexpr StringView(const char *data, size_t size) : _view(data, size) {}

                /** @brief Views the contents of a `std::string_view`. */
                constexpr StringView(StdView view) : _view(view) {}

                /** @brief Compares contents for equality. */
                constexpr bool operator==(const StringView &other) const { return _view == other._view; }

                /** @brief Lexicographic three-way comparison of contents. */
                constexpr std::strong_ordering operator<=>(const StringView &other) const {
                        return _view <=> other._view;
                }

                /** @brief Returns the character at @p index (unchecked). */
                constexpr char operator[](size_t index) const { return _view[index]; }

                /** @brief Returns a pointer to the first character (not necessarily NUL-terminated). */
                constexpr const char *data() const { return _view.data(); }

                /** @brief Returns the number of characters. */
                constexpr size_t size() const { return _view.size(); }

                /** @brief Returns true if the view has no characters. */
                constexpr bool isEmpty() const { return _view.empty(); }

                /** @brief Returns the first character; the view must not be empty. */
                constexpr char front() const { return _view.front(); }

                /** @brief Returns the last character; the view must not be empty. */
                constexpr char back() const { return _view.back(); }

                /** @brief Returns an iterator to the first character. */
                constexpr ConstIterator begin() const { return _view.begin(); }

                /** @brief Returns an iterator past the last character. */
                constexpr ConstIterator end() const { return _view.end(); }

                /**
                 * @brief Returns the first @p count characters.
                 * @param count Number of characters (clamped to `size()`).
                 */
                constexpr StringView left(size_t count) const { return slice(0, count); }

                /**
                 * @brief Returns the last @p count characters.
                 * @param count Number of characters (clamped to `size()`).
                 */
                constexpr StringView right(size_t count) const {
                        return count >= size() ? *this : slice(size() - count, count);
                }

                /**
                 * @brief Returns @p count characters starting at @p pos.
                 * @param pos   Start index; past the end gives an empty view.
                 * @param count Number of characters (clamped); `NPos` means "to the end".
                 */
                constexpr StringView mid(size_t pos, size_t count = NPos) const {
                        return pos >= size() ? StringView() : slice(pos, count);
                }

                /** @brief Returns true if the view begins with @p prefix. */
                constexpr bool startsWith(StringView prefix) const {
                        return prefix.size() <= size() && left(prefix.size()) == prefix;
                }

                /** @brief Returns true if the view begins with @p ch. */
                constexpr bool startsWith(char ch) const { return !isEmpty() && front() == ch; }

                /** @brief Returns true if the view ends with @p suffix. */
                constexpr bool endsWith(StringView suffix) const {
                        return suffix.size() <= size() && right(suffix.size()) == suffix;
                }

                /** @brief Returns true if the view ends with @p ch. */
                constexpr bool endsWith(char ch) const { return !isEmpty() && back() == ch; }

                /** @brief Returns true if @p str occurs in the view. */
                constexpr bool contains(StringView str) const { return indexOf(str) != NPos; }

                /** @brief Returns true if @p ch occurs in the view. */
                constexpr bool contains(char ch) const { return indexOf(ch) != NPos; }

                /**
                 * @brief Finds the first occurrence of @p ch at or after @p from.
                 * @return The index, or `NPos`.
                 */
                constexpr size_t indexOf(char ch, size_t from = 0) const { return _view.find(ch, from); }

                /**
                 * @brief Finds the first occurrence of @p str at or after @p from.
                 * @return The index, or `NPos`.
                 */
                constexpr size_t indexOf(StringView str, size_t from = 0) const { return _view.find(str._view, from); }

                /**
                 * @brief Finds the last occurrence of @p ch.
                 * @return The index, or `NPos`.
                 */
                constexpr size_t lastIndexOf(char ch) const { return _view.rfind(ch); }

                /** @brief Returns the view with leading and trailing ASCII whitespace removed. */
                constexpr StringView trimmed() const {
                        size_t first = 0;
                        size_t last = size();
                        while (first < last && isSpace(_view[first])) ++first;
                        while (last > first && isSpace(_view[last - 1])) --last;
                        return slice(first, last - first);
                }

                /** @brief Returns the underlying `std::string_view`. */
                constexpr StdView toStdStringView() const { return _view; }

        private:
                // Like substr(), without its out_of_range throw: @p pos must be
                // <= size(); @p count is clamped. (A throw path, even unreachable,
                // links libstdc++'s exception runtime: ~5.7 KB flash, ~200 B RAM.)
                constexpr StringView slice(size_t pos, size_t count) const {
                        const size_t n = count < size() - pos ? count : size() - pos;
                        return StringView(StdView(_view.data() + pos, n));
                }

                static constexpr bool isSpace(char ch) {
                        return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\v' || ch == '\f';
                }

                StdView _view;
};

BMDS_NAMESPACE_END
