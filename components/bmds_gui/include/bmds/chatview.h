/**
 * @file      chatview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/chatmodel.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/font.h>
#include <bmds/scrollarea.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Lines of chat or a log, appended as they come (`devplan/chatview.md`): styled by IRC's
 *        codes (`ChatText`), a gutter before each line's first tab with the text hanging past
 *        the widest gutter seen, links found and clickable, a marker, a selection to copy, and a
 *        view that follows the end unless scrolled back.
 *
 * Lines wrap at the view's width (`setWrap(false)`: they scroll sideways instead), every line
 * again when the width, the font or the gutter's column changes. Each line is kept as its bytes
 * in PSRAM (`ChatLines`), at most `maxLines()`, the oldest dropped; only what shows is laid out.
 * Its font is `Widget::font()`; bold, italic and monospace runs are drawn in that font's bold
 * and italic and in the `mono` role's at its size.
 *
 * **Scrolling:** at the end, new lines scroll it; scrolled back (the bar, the wheel, the keys),
 * what shows stays put as lines come and go, told to `ScrolledBack`, until the end is reached
 * again or `scrollToEnd()`.
 *
 * **Keys:** Up, Down, Page Up, Page Down, Home and End (Ctrl too) scroll; Ctrl+C and Ctrl+Insert
 * copy; Ctrl+A selects everything. **The mouse:** a drag selects (scrolling at the edges), a
 * double click a word, a triple a line, Shift+click extends; a click on a link (no drag) tells
 * `LinkActivated`. A selection becomes the primary selection.
 *
 * @par Thread Safety
 * As `Widget`: the GUI loop's thread.
 */
class ChatView : public ScrollArea {
        public:
                /** @brief Told a link clicked: its text (`http://...`, `www....`, `irc://...`). */
                using LinkActivated = void (*)(void *arg, ChatView &view, StringView url);

                /** @brief Told the view left the end (@p back) or reached it again. */
                using ScrolledBack = void (*)(void *arg, ChatView &view, bool back);

                /** @brief The lines kept unless `setMaxLines()` says otherwise. */
                static constexpr size_t DefaultMaxLines = 2000;
                /** @brief The most lines `setMaxLines()` takes. */
                static constexpr size_t MostLines = 100000;
                /** @brief The pixels between the left edge and the text. */
                static constexpr uint16_t Margin = 4;
                /** @brief The pixels between the widest gutter and the text. */
                static constexpr uint16_t GutterGap = 8;

                explicit ChatView(Widget *parent);
                ~ChatView() override;

                // ---- The lines

                /** @brief Appends @p line (IRC's codes; a tab ends its gutter). @return `Ok`, `NoMem`. */
                Error append(StringView line);

                /** @brief Appends each of @p lines, laid out once. @return `Ok`, `NoMem` (those before kept). */
                Error appendMany(Span<const StringView> lines);

                /** @brief Forgets every line, the marker and the selection; the view follows the end. */
                void clear();

                /** @brief Returns how many lines are kept. */
                size_t lineCount() const { return _lines.count(); }

                /** @brief Keeps at most @p maxLines (1 to `MostLines`), the oldest dropped now. @return `Ok`, `Invalid`, `NoMem`. */
                Error setMaxLines(size_t maxLines);

                /** @brief Returns the most lines kept. */
                size_t maxLines() const { return _lines.maxLines(); }

                // ---- How it shows them

                /** @brief Wraps lines at the width (the default), or scrolls sideways. */
                void setWrap(bool wrap);

                /** @brief Returns true if lines wrap. */
                bool wraps() const { return _wrap; }

                /** @brief Sets palette colour @p index (0-98) to @p rgb (0xRRGGBB). */
                void setPaletteColour(size_t index, uint32_t rgb);

                /** @brief Returns palette colour @p index's RGB (0 past it). */
                uint32_t paletteColour(size_t index) const {
                        return index < ChatText::PaletteSize ? _palette[index] : 0;
                }

                /** @brief Sets every palette colour back to IRC's (`ChatText::paletteColour()`). */
                void resetPalette();

                /**
                 * @brief Puts the marker under the last line (a rule, @p words on it: "" for none):
                 *        what comes after it is new. A marker's line dropped takes it.
                 * @return `Ok`, `NoMem`.
                 */
                Error setMarker(StringView words);

                /** @brief Takes the marker away. */
                void clearMarker();

                /** @brief Returns true if a marker shows. */
                bool hasMarker() const { return _marker; }

                // ---- Scrolling

                /** @brief Returns true if it follows the end (new lines scroll it). */
                bool isFollowingEnd() const { return _follow; }

                /** @brief Scrolls to the end, and follows it. */
                void scrollToEnd();

                // ---- The selection

                /** @brief Returns true if any text is selected. */
                bool hasSelection() const { return _selAnchor != _selCursor; }

                /** @brief Returns the selection's text (codes gone; a line's gutter, a tab, its text; lines joined by newlines). */
                Error selectedText(String &out) const;

                /** @brief Selects every line. */
                void selectAll();

                /** @brief Puts the selection on the clipboard. */
                void copy();

                // ---- Told

                /** @brief Has @p fn told of a link clicked. */
                void setLinkActivated(LinkActivated fn, void *arg) {
                        _linkActivated = fn;
                        _linkArg = arg;
                        return;
                }

                /** @brief Has @p fn told when the view leaves the end or reaches it. */
                void setScrolledBack(ScrolledBack fn, void *arg) {
                        _scrolledBack = fn;
                        _scrolledArg = arg;
                        return;
                }

                // ---- Widget

                GuiSize                sizeHint() const override;
                bool                   keyPressed(const KeyEvent &event) override;
                void                   mousePressed(const WindowManager::MouseEvent &event) override;
                void                   mouseMoved(const WindowManager::MouseEvent &event) override;
                void                   mouseReleased(const WindowManager::MouseEvent &event) override;
                WindowManager::Pointer pointerAt(int x, int y) const override;

        protected:
                void paintContent(Painter &painter, const Rect &viewport) override;
                void resized() override;
                void scrolled() override;
                void styleChanged() override;

        private:
                // A place in the text: a line's number (`ChatLines`) and a byte of its shown text.
                struct Pos {
                                uint64_t line = 0;
                                uint32_t byte = 0;

                                bool operator==(const Pos &other) const = default;
                                bool operator<(const Pos &other) const {
                                        return line != other.line ? line < other.line : byte < other.byte;
                                }
                };

                // What a row of the view holds.
                enum class RowKind : uint8_t {
                        Line,   // a line's row
                        Marker, // the marker's
                        Past,   // none: past the end
                };

                // A line laid out (the scratch's: one at a time).
                struct Laid {
                                ChatText::Parsed parsed;
                                ChatText::Row    rows[128];
                                ChatText::Piece  pieces[512];
                                ChatText::Layout layout;
                                uint64_t         line = UINT64_MAX; // what it holds (UINT64_MAX: none)
                };

                static uint32_t measureWidth(void *arg, StringView text, uint8_t flags);
                static size_t   measureFitting(void *arg, StringView text, uint8_t flags, uint32_t most);
                static void     edgeTick(void *arg);
                static Error    providePrimary(void *arg, StringView type, String &out);

                ChatText::Measure measure() const;                               // in 1/64 pixels
                uint32_t          unitsOf(StringView text, uint8_t flags) const; // the same

                const Font   &fontFor(uint8_t flags) const { return _fonts[fontIndex(flags)]; }
                static size_t fontIndex(uint8_t flags);
                void          openFonts();
                uint32_t      wrapWidth() const;
                uint32_t      textColumn() const; // from the widest gutter
                const Laid   *layOut(uint64_t line) const;
                uint32_t      rowsOf(const ChatText::Parsed &parsed, uint32_t &width) const;
                void          relayout(bool measureGutters); // every line again
                void          updateContentSize();
                Error         appendOne(StringView line, bool &wider);
                void          linesDropped();
                uint64_t      viewRows() const;
                uint64_t      markerRow() const;
                RowKind       rowAt(uint64_t row, uint64_t &line, uint32_t &within) const;
                uint64_t      rowOfLine(uint64_t line) const; // its first row in the view
                int           lineLeft() const;               // the window x of the lines' left
                void          followOrKeep();                 // after a change: the end, or the anchor
                void          noteAnchor();
                void          internalScroll(int64_t x, int64_t y);
                void          setFollow(bool follow);
                Pos           posAt(int x, int y) const;
                int           linkAt(int x, int y, uint64_t &line) const;
                void          selectUnit(Pos at, uint8_t clicks, Pos &from, Pos &to) const;
                void          dragTo(int x, int y);
                void          selectionChanged();
                void          paintLine(Painter &painter, const Laid &laid, uint32_t row, int y, const Rect &viewport);
                void          paintMarker(Painter &painter, int y, const Rect &viewport);

                ChatLines     _lines{DefaultMaxLines};
                mutable Laid *_laid = nullptr;  // PSRAM, made with the view
                Font          _fonts[8];        // by fontIndex(): bold 1, italic 2, monospace 4
                int16_t       _fontTop[8] = {}; // each font's top in a row (baselines lined up)
                uint16_t      _rowHeight = 16;
                uint16_t      _baseline = 12;    // from a row's top
                uint32_t      _gutterWidest = 0; // 1/64 pixels
                uint32_t      _textX = 0;        // the text's column, from the widest gutter
                uint32_t      _widest = 0;       // unwrapped: the widest line
                uint32_t      _laidWidth = 0;    // the width every line was wrapped at
                uint32_t      _palette[ChatText::PaletteSize] = {};
                bool          _wrap = true;
                bool          _follow = true;
                bool          _internal = false; // a scroll of its own (not the user's)
                bool          _marker = false;
                uint64_t      _markerLine = 0; // the first line after it
                String        _markerWords{MemSpace::Psram};
                uint64_t      _anchorLine = 0; // scrolled back: the line at the top, its row, the pixels into it
                uint32_t      _anchorRow = 0;
                uint32_t      _anchorPixel = 0;
                bool          _anchorMarker = false; // the marker's row at the top
                Pos           _selAnchor;
                Pos           _selCursor;
                Pos           _unitFrom; // a double or triple click's word or line
                Pos           _unitTo;
                bool          _dragging = false;
                bool          _moved = false; // since the press (a click on a link isn't a drag)
                uint8_t       _clicks = 0;
                uint64_t      _pressUs = 0;
                int           _pressX = 0;
                int           _pressY = 0;
                int           _pressLink = -1;
                uint64_t      _pressLinkLine = 0;
                int           _dragX = 0;
                int           _dragY = 0;
                int           _edgeTimer = -1;
                LinkActivated _linkActivated = nullptr;
                void         *_linkArg = nullptr;
                ScrolledBack  _scrolledBack = nullptr;
                void         *_scrolledArg = nullptr;
};

BMDS_NAMESPACE_END
