/**
 * @file      chatmodel.h
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
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A `ChatView`'s lines, pure (devplan/chatview.md §2): a line's styling in IRC's
 *        (mIRC's) codes parsed into runs, its links found, its gutter (before its first tab), the
 *        line wrapped at a width in a measure of the caller's (a font's pixels), a point hit in
 *        it, its plain text.
 *
 * The codes: `\x02` bold, `\x1d` italic, `\x1f` underline, `\x1e` strike, `\x11` monospace,
 * `\x16` reverse, `\x03FG[,BG]` one of 99 colours (a digit or two each; none: colours off),
 * `\x04RRGGBB[,RRGGBB]` any colour, `\x0f` all off. Every other control character but the tab is
 * dropped; text is UTF-8, as it came.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace ChatText {

        /** @brief A style's flags. */
        enum Flag : uint8_t {
                Bold = 1u << 0,
                Italic = 1u << 1,
                Underline = 1u << 2,
                Strike = 1u << 3,
                Monospace = 1u << 4,
                Reverse = 1u << 5,
        };

        /** @brief The flags that choose a run's font (the rest are drawn over it). */
        constexpr uint8_t FontFlags = Bold | Italic | Monospace;

        /** @brief A colour: none (the view's), one of the palette's 99, or any RGB. */
        struct Colour {
                        enum class Kind : uint8_t {
                                None,
                                Index,
                                Rgb,
                        };
                        Kind     kind = Kind::None;
                        uint8_t  index = 0; ///< `Index`: 0-98.
                        uint32_t rgb = 0;   ///< `Rgb`: 0xRRGGBB.

                        bool operator==(const Colour &other) const = default;
        };

        /** @brief How a run of text looks. */
        struct Style {
                        uint8_t flags = 0; ///< `Flag` bits.
                        Colour  fg;
                        Colour  bg;

                        bool operator==(const Style &other) const = default;
        };

        /** @brief A run of the visible text in one style (and in one link, or none). */
        struct Run {
                        uint32_t start = 0;
                        uint32_t length = 0;
                        Style    style;
                        int16_t  link = -1; ///< Its link's index, or -1.
        };

        /** @brief A link found in the visible text. */
        struct Link {
                        uint32_t start = 0;
                        uint32_t length = 0;
        };

        /** @brief The most runs a line keeps: past them the style stays as it was. */
        constexpr size_t MaxRuns = 64;

        /** @brief The most links a line keeps. */
        constexpr size_t MaxLinks = 8;

        /** @brief No gutter (`Parsed::gutter`). */
        constexpr uint32_t NoGutter = UINT32_MAX;

        /** @brief The palette's colours: 0-15 mIRC's, 16-98 the extended ones (99 is none). */
        constexpr size_t PaletteSize = 99;

        /** @brief Returns colour @p index's RGB (0xRRGGBB); 0 past the palette. */
        uint32_t paletteColour(uint8_t index);

        /** @brief A line parsed. */
        struct Parsed {
                        String   text;              ///< What shows: the codes and the gutter's tab gone.
                        uint32_t gutter = NoGutter; ///< The bytes of `text` before the first tab: the gutter.
                        Run      runs[MaxRuns];     ///< In order, covering `text`, split at links.
                        size_t   runCount = 0;
                        Link     links[MaxLinks];
                        size_t   linkCount = 0;
        };

        /**
         * @brief Parses @p line: its codes into runs, its first tab into the gutter (later tabs
         *        become spaces), its links (`http://`, `https://`, `irc://`, `ircs://`, `www.` at a
         *        word's start, trailing punctuation and an unmatched closing bracket left out).
         * @return `Ok` or `NoMem`.
         */
        Error parse(StringView line, Parsed &out);

        /** @brief What a copy takes of @p line: its gutter, a tab, its text; the codes gone. */
        Error plain(StringView line, String &out);

        /** @brief Returns the index of the link at byte @p offset of `text`, or -1. */
        int linkAt(const Parsed &line, uint32_t offset);

        /** @brief `find()`'s answer when there is no match. */
        constexpr uint32_t NoMatch = UINT32_MAX;

        /**
         * @brief Finds @p needle in @p text (a line's shown text), ASCII letters' case ignored:
         *        the first match starting at or after @p from, or with @p backward the last one
         *        starting before it (@p from past the end: the end).
         * @return The match's first byte, or `NoMatch` (an empty @p needle matches nothing).
         */
        uint32_t find(StringView text, StringView needle, uint32_t from, bool backward);

        /**
         * @brief How text is measured: a run's style may change its font (bold, italic, monospace;
         *        only `FontFlags` may change a width). Widths, and every x and width `wrap()` and
         *        `hit()` take and give, are in its units: a font's pixels, or finer (`ChatView`
         *        measures in 1/64 pixels, so that pieces measured one by one add up exactly).
         */
        struct Measure {
                        /** @brief @p text's width in @p flags' font. */
                        uint32_t (*width)(void *arg, StringView text, uint8_t flags) = nullptr;
                        /** @brief How many bytes of @p text fit in @p most, whole characters only. */
                        size_t (*fitting)(void *arg, StringView text, uint8_t flags, uint32_t most) = nullptr;
                        void *arg = nullptr;
                        /**
                         * @brief Where a run's font changes on a row, it starts on a multiple of
                         *        this (from the line's left; 0 or 1: anywhere): a painter that starts
                         *        a font's text on a pixel column of its own (`Painter::text()`'s 4).
                         */
                        uint32_t align = 0;
        };

        /** @brief A run's part on a row, at `x` from the line's left. */
        struct Piece {
                        uint32_t start = 0;
                        uint32_t length = 0;
                        uint16_t run = 0; ///< Its run's index.
                        uint32_t x = 0;
        };

        /** @brief A row of a wrapped line: its pieces. */
        struct Row {
                        uint32_t firstPiece = 0;
                        uint32_t pieceCount = 0;
        };

        /** @brief Where `wrap()` writes (either may be null or too small: it still counts the rows). */
        struct Layout {
                        Row   *rows = nullptr;
                        size_t maxRows = 0;
                        Piece *pieces = nullptr;
                        size_t maxPieces = 0;
                        size_t rowCount = 0;   ///< Rows written.
                        size_t pieceCount = 0; ///< Pieces written.
        };

        /**
         * @brief Wraps @p line at @p width: the gutter at 0 on the first row, the text from
         *        @p textX (the view's gutter column, or past a wider gutter) and every row after
         *        it from @p textX too (a hanging indent), unless @p textX is more than two thirds
         *        of @p width (then from 0). Breaks at spaces, the spaces at a break dropped; a word
         *        wider than a row is broken where it reaches the edge. An empty line is a row. A
         *        run whose font differs from the one before it on its row starts on a multiple of
         *        `Measure::align` (@p textX should be one).
         * @return The rows it takes (whatever `out` held).
         */
        size_t wrap(const Parsed &line, const Measure &measure, uint32_t width, uint32_t textX, Layout *out);

        /** @brief Returns @p line's gutter's width (0 for none), as `wrap()` places it. */
        uint32_t gutterWidth(const Parsed &line, const Measure &measure);

        /**
         * @brief Returns the byte of `text` nearest @p x (from the line's left) on row @p row of
         *        @p layout: the row's start before it, its end past it.
         */
        uint32_t hit(const Parsed &line, const Measure &measure, const Layout &layout, size_t row, int32_t x);

} // namespace ChatText

/**
 * @brief A `ChatView`'s lines, kept (the newest last, the oldest dropped past `maxLines()`), with
 *        how many rows each takes at the width they were last wrapped at, so the view finds the
 *        line at a row by a binary search. Lines are numbered from the first ever appended (a
 *        number stays a line's while it is kept: a marker's, a selection's).
 *
 * The lines and their counts are in `MemSpace::Psram`.
 *
 * @par Thread Safety
 * Not thread-safe (the view's loop's thread).
 */
class ChatLines {
        public:
                /** @brief Keeps at most @p maxLines (1 at least). */
                explicit ChatLines(size_t maxLines);
                ~ChatLines();
                ChatLines(const ChatLines &) = delete;
                ChatLines &operator=(const ChatLines &) = delete;

                /** @brief Appends @p line, taking @p rows; the oldest goes past `maxLines()`. @return `Ok`, `NoMem`. */
                Error append(StringView line, uint32_t rows);

                /** @brief Forgets every line (the numbers go on). */
                void clear();

                /** @brief Keeps at most @p maxLines from now on (the oldest dropped now). @return `Ok`, `NoMem`. */
                Error setMaxLines(size_t maxLines);

                /** @brief Returns the most lines kept. */
                size_t maxLines() const { return _capacity; }

                /** @brief Returns how many lines are kept. */
                size_t count() const { return _count; }

                /** @brief Returns the oldest kept line's number. */
                uint64_t first() const { return _first; }

                /** @brief Returns the number the next line appended gets. */
                uint64_t next() const { return _first + _count; }

                /** @brief Returns true if line @p number is kept. */
                bool has(uint64_t number) const { return number >= _first && number < _first + _count; }

                /** @brief Returns line @p number's text (`has()` first). */
                StringView line(uint64_t number) const;

                /** @brief Returns line @p number's rows. */
                uint32_t rows(uint64_t number) const;

                /** @brief Sets line @p number's rows (after a rewrap: call `restack()` once all are set). */
                void setRows(uint64_t number, uint32_t rows);

                /** @brief Adds up the rows again after `setRows()`. */
                void restack();

                /** @brief Returns the rows every kept line takes. */
                uint64_t totalRows() const { return _count == 0 ? 0 : _end - startOf(_first); }

                /** @brief Returns the row line @p number starts at (from the oldest kept line's). */
                uint64_t rowOf(uint64_t number) const { return startOf(number) - startOf(_first); }

                /**
                 * @brief Returns the line at row @p row (from the oldest kept line's), and its row in
                 *        that line in @p within; the last line past the end. Nothing kept: `next()`.
                 */
                uint64_t lineAtRow(uint64_t row, uint32_t &within) const;

        private:
                size_t   slot(uint64_t number) const { return size_t((_head + (number - _first)) % _capacity); }
                uint64_t startOf(uint64_t number) const;
                void     dropOldest();

                String   *_lines = nullptr;
                uint32_t *_rows = nullptr;
                uint64_t *_starts = nullptr; ///< Each line's first row, counted from the first line ever.
                size_t    _capacity = 0;
                size_t    _head = 0; ///< The oldest's slot.
                size_t    _count = 0;
                uint64_t  _first = 0; ///< The oldest's number.
                uint64_t  _end = 0;   ///< The rows counted to the newest's end.
};

BMDS_NAMESPACE_END
