/**
 * @file      lineeditmodel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A line of text being edited: its bytes, a cursor and a selection
 *        (`devplan/gui.md` §4q). `LineEdit`'s, pure, so host-tested.
 *
 * Places are byte offsets, always on a UTF-8 character's start: every move
 * and edit takes whole characters. The selection runs between the anchor and
 * the cursor; it is empty when they meet.
 *
 * @par Thread Safety
 * A value.
 */
class LineEditModel {
        public:
                /** @brief The most bytes held. */
                static constexpr size_t MaxBytes = 255;

                /** @brief Returns the text. */
                StringView text() const { return StringView(_text, _length); }

                /** @brief Returns the most bytes inserting goes up to. */
                size_t maxBytes() const { return _max; }

                /** @brief Stops inserting at @p bytes (at most `MaxBytes`); the text already there stays. */
                void setMaxBytes(size_t bytes) {
                        _max = bytes < MaxBytes ? bytes : MaxBytes;
                        return;
                }

                /** @brief Returns the cursor (a byte offset). */
                size_t cursor() const { return _cursor; }

                /** @brief Returns the selection's other end. */
                size_t anchor() const { return _anchor; }

                /** @brief Returns true if something is selected. */
                bool hasSelection() const { return _anchor != _cursor; }

                /** @brief Returns the selection's start. */
                size_t selectionStart() const { return _anchor < _cursor ? _anchor : _cursor; }

                /** @brief Returns the selection's end (past it). */
                size_t selectionEnd() const { return _anchor < _cursor ? _cursor : _anchor; }

                /** @brief Returns the selected text. */
                StringView selected() const { return text().mid(selectionStart(), selectionEnd() - selectionStart()); }

                /** @brief Sets the text (cut at `MaxBytes` on a character), the cursor at its end, nothing selected. */
                void setText(StringView text);

                /** @brief Selects all of it, the cursor at the end. */
                void selectAll();

                /**
                 * @brief Puts @p text where the cursor is, in place of the selection (what
                 *        doesn't fit in `maxBytes()` dropped, a whole character at a time;
                 *        control characters left out).
                 * @return True if the text changed.
                 */
                bool insert(StringView text);

                /** @brief Deletes the selection, else the character before the cursor. @return True if changed. */
                bool backspace();

                /** @brief Deletes the selection, else the character after the cursor. @return True if changed. */
                bool erase();

                /** @brief The cursor a character left (to the selection's start, if not @p select). */
                void left(bool select);

                /** @brief The cursor a character right (to the selection's end, if not @p select). */
                void right(bool select);

                /** @brief The cursor to the start. */
                void home(bool select) { setCursor(0, select); }

                /** @brief The cursor to the end. */
                void end(bool select) { setCursor(_length, select); }

                /** @brief The cursor to @p offset (snapped back to a character's start), selecting if @p select. */
                void setCursor(size_t offset, bool select);

        private:
                size_t before(size_t at) const; // the start of the character before @p at
                size_t after(size_t at) const;  // past the character at @p at
                void   removeSelection();

                char   _text[MaxBytes + 1] = {};
                size_t _length = 0;
                size_t _cursor = 0;
                size_t _anchor = 0;
                size_t _max = MaxBytes;
};

/**
 * @brief Words in lines of a width (`devplan/gui.md` §4q: a message box's
 *        text), pure.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace TextWrap {

        /**
         * @brief Breaks @p text into lines of at most @p columns cells (a wide
         *        character takes two): at spaces, at each LF, and inside a word
         *        too long for a line. Spaces at a break are dropped.
         * @return The lines written into @p lines (views of @p text); text past
         *         `lines.size()` lines is left out.
         */
        size_t wrap(StringView text, uint16_t columns, Span<StringView> lines);

        /** @brief Returns one character's width (@p piece: its bytes) in a measure of the caller's. */
        using Measure = uint32_t (*)(void *arg, StringView piece);

        /**
         * @brief As above in any measure (a font's pixels): lines at most @p width wide, each
         *        character as wide as @p measure says (added up: kerning left out).
         */
        size_t wrap(StringView text, uint32_t width, Measure measure, void *arg, Span<StringView> lines);

} // namespace TextWrap

BMDS_NAMESPACE_END
