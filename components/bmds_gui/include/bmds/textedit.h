/**
 * @file      textedit.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/scrollarea.h>
#include <bmds/stringview.h>
#include <bmds/textdocument.h>

BMDS_NAMESPACE_BEGIN

struct TextCell;

/**
 * @brief A multi-line text editor (`devplan/editor.md` §4): a `TextDocument` drawn in the
 *        fixed-cell font, with a cursor and a selection, the keys and the mouse of an editor,
 *        the clipboard, undo and redo.
 *
 * It owns a document of its own unless given one (`setDocument()`), which it then only
 * observes: several views may share a document. Lines don't wrap; long ones scroll sideways.
 *
 * **Keys:** arrows, Home (the first non-blank, then the line's start), End, Page Up and Down,
 * Ctrl with them by word and to the text's ends, Shift with any to select; Backspace, Delete,
 * Ctrl+Backspace and Ctrl+Delete by word; Enter (keeping the line's indentation if
 * `setAutoIndent()`); Tab and Shift+Tab (indenting a selection of lines); Insert (overwrite);
 * Ctrl+A, Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z, Ctrl+C, Ctrl+X, Ctrl+V, Shift+Delete, Ctrl+Insert,
 * Shift+Insert. Keys it doesn't use go to its window.
 *
 * **The mouse:** a click places the cursor, a drag selects (scrolling at the edges), a double
 * click selects a word, a triple a line; Shift+click extends; the middle button pastes the
 * primary selection, which a selection becomes (as in the terminal). Text and files (their
 * paths) dropped are inserted where they fall.
 *
 * **For subclasses** (`CodeEdit`): colours for a line's bytes (`styleLine()`), a gutter at the
 * left (`gutterWidth()`, `paintGutter()`), a line's background (`lineBackground()`), keys before
 * the defaults (`editKey()`), and the document's changes (`documentChanged()`).
 *
 * @par Thread Safety
 * As `Widget`: the GUI loop's thread.
 */
class TextEdit : public ScrollArea {
        public:
                /** @brief Told of a change (the text, the cursor, being modified). */
                using Notify = void (*)(void *arg, TextEdit &edit);

                /** @brief The colours it draws with, 0xRRGGBB. */
                struct Colors {
                                uint32_t text = 0x000000;
                                uint32_t background = 0xffffff;
                                uint32_t selectionText = 0xffffff;
                                uint32_t selectionBackground = 0x000080;
                                uint32_t cursor = 0x000000;
                                uint32_t currentLine = 0xffffff; ///< The cursor's line's background (CodeEdit's).
                };

                /** @brief A run of a line's bytes in colours of their own (`styleLine()`). */
                struct StyleRun {
                                uint32_t start = 0;            ///< The first byte.
                                uint32_t end = 0;              ///< Past the last.
                                uint32_t text = 0;             ///< Its colour.
                                uint32_t background = NoColor; ///< Its background, or `NoColor` (the line's).
                                uint8_t  flags = 0;            ///< `TextCell` flags (underline, bold).
                };

                /** @brief "No colour": the default. */
                static constexpr uint32_t NoColor = 0xffffffffu;
                /** @brief The most style runs a line gets. */
                static constexpr size_t MaxRuns = 64;
                /** @brief The pixels between the left edge and the text. */
                static constexpr uint16_t Margin = 4;
                /** @brief The cursor's blink, on and off. */
                static constexpr uint32_t BlinkMs = 500;

                explicit TextEdit(Widget *parent);
                ~TextEdit() override;

                // ---- The document

                /** @brief Returns the document it shows. */
                TextDocument       &document() { return *_document; }
                const TextDocument &document() const { return *_document; }
                /** @brief Shows @p document (not owned; null: its own again), the cursor at its start. */
                void setDocument(TextDocument *document);
                /** @brief Replaces the text (of its document) with @p bytes, unmodified, the cursor at the start. */
                Error setText(StringView bytes);

                // ---- The cursor and the selection

                /** @brief Returns the cursor and the selection. */
                const TextSelection &selection() const { return _cursor.selection(); }
                /** @brief Sets the cursor and the selection, and scrolls to show the cursor. */
                void setSelection(const TextSelection &selection);
                /** @brief Puts the cursor at the start of line @p line (from 0), and shows it near the middle. */
                void goToLine(uint32_t line);
                /** @brief Returns the selected text in @p out. */
                Error selectedText(String &out) const;
                /** @brief Scrolls as little as shows the cursor. */
                void revealCursor();
                /** @brief Returns the cursor's character cell (window coordinates): where a popup opens. */
                Rect cursorRect() const;

                // ---- Settings

                /** @brief Sets the tab width in columns (tabs drawn to its multiples). */
                void     setTabWidth(uint32_t width);
                uint32_t tabWidth() const { return _cursor.tabWidth(); }
                /** @brief Tab inserts spaces to the next stop instead of a tab. */
                void setInsertSpaces(bool spaces) { _insertSpaces = spaces; }
                bool insertsSpaces() const { return _insertSpaces; }
                /** @brief Enter keeps the line's indentation. */
                void setAutoIndent(bool on) { _autoIndent = on; }
                bool autoIndents() const { return _autoIndent; }
                /** @brief Allows no edits (the cursor and the selection still move; copying works). */
                void setReadOnly(bool readOnly);
                bool isReadOnly() const { return _readOnly; }
                /** @brief Typing replaces the character at the cursor (Insert toggles it). */
                void setOverwrite(bool overwrite);
                bool isOverwrite() const { return _overwrite; }
                /** @brief Draws in @p colors instead of the window palette's field colours. */
                void setColors(const Colors &colors);
                /** @brief Returns the colours drawn with. */
                const Colors &colors() const { return _colors; }

                // ---- Actions (a menu's)

                void undo();
                void redo();
                void cut();
                void copy();
                void paste();
                void selectAll();
                /** @brief Deletes the selection. */
                void deleteSelection();
                /** @brief Puts @p text in place of the selection, as one undo step (as a paste). */
                void insertText(StringView text);
                /** @brief Indents (or unindents, @p out) the selected lines, or the cursor's. */
                void indentLines(bool out);

                /**
                 * @brief Selects the next @p needle from the cursor (`TextDocument::FindFlags`;
                 *        backward from the selection's start).
                 * @return True if found.
                 */
                bool find(StringView needle, uint32_t flags);
                /** @brief Replaces the selection with @p replacement if it is @p needle, then finds the next. */
                bool replace(StringView needle, StringView replacement, uint32_t flags);
                /** @brief Replaces every @p needle, one undo step. @return How many. */
                size_t replaceAll(StringView needle, StringView replacement, uint32_t flags);

                // ---- Told

                /** @brief The text changed (any edit, an undo). */
                void setChanged(Notify fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }
                /** @brief The cursor or the selection moved. */
                void setCursorMoved(Notify fn, void *arg) {
                        _cursorMoved = fn;
                        _cursorMovedArg = arg;
                        return;
                }

                // ---- Widget

                GuiSize                sizeHint() const override;
                bool                   keyPressed(const KeyEvent &event) override;
                void                   mousePressed(const WindowManager::MouseEvent &event) override;
                void                   mouseMoved(const WindowManager::MouseEvent &event) override;
                void                   mouseReleased(const WindowManager::MouseEvent &event) override;
                WindowManager::Pointer pointerAt(int x, int y) const override;
                void                   focusIn() override;
                void                   focusOut() override;
                void                   dragMoved(GuiDropEvent &event) override;
                bool                   dropped(GuiDropEvent &event) override;

        protected:
                /**
                 * @brief Colours for line @p line (its bytes @p text): up to @p max runs into
                 *        @p runs, in order, not overlapping; bytes in none are plain.
                 * @return How many.
                 */
                virtual size_t styleLine(uint32_t line, StringView text, StyleRun *runs, size_t max) {
                        (void)line;
                        (void)text;
                        (void)runs;
                        (void)max;
                        return 0;
                }
                /** @brief Returns the gutter's width (0: none). */
                virtual uint16_t gutterWidth() const { return 0; }
                /** @brief Draws the gutter's part for line @p line in @p row (window coordinates). */
                virtual void paintGutter(Painter &painter, uint32_t line, const Rect &row) {
                        (void)painter;
                        (void)line;
                        (void)row;
                }
                /** @brief Returns line @p line's background (default: the current line's colour for the cursor's line). */
                virtual uint32_t lineBackground(uint32_t line) const;
                /** @brief A key before the defaults. @return True if used. */
                virtual bool editKey(const KeyEvent &event) {
                        (void)event;
                        return false;
                }
                /** @brief @p text was typed (a character), the cursor after it. */
                virtual void typed(StringView text) { (void)text; }
                /** @brief The document changed (after the widget's own bookkeeping). */
                virtual void documentChanged(const TextDocument::Change &change) { (void)change; }
                /** @brief A click in the gutter on line @p line (default: selects the line). */
                virtual void gutterClicked(uint32_t line, const WindowManager::MouseEvent &event);
                /** @brief Redraws line @p line. */
                void updateLine(uint32_t line);
                /** @brief Redraws the gutter. */
                void updateGutter();
                /** @brief The gutter's width or the text's changed: the content measured again, all redrawn. */
                void relayoutContent();
                /** @brief Draws over line @p line once its text is drawn at @p y (window coordinates) in @p area. */
                virtual void paintOver(Painter &painter, uint32_t line, int y, const Rect &area) {
                        (void)painter;
                        (void)line;
                        (void)y;
                        (void)area;
                }
                /** @brief The cursor or the selection moved (before the window is told). */
                virtual void cursorMovedHook() {}
                /** @brief Returns the window x of column @p column. */
                int columnX(uint32_t column) const;
                /** @brief Returns a character cell's width in pixels. */
                static uint16_t cellWidth();

                // For subclasses' editing: one undo step of @p kind.
                void        beginEdit(TextDocument::EditKind kind = TextDocument::EditKind::Other,
                                      StringView             typed = StringView());
                void        endEdit();
                TextCursor &cursor() { return _cursor; }
                /** @brief The cursor moved: shown, told, the primary selection offered. */
                void cursorChanged();

                void paintContent(Painter &painter, const Rect &viewport) override;
                void resized() override;
                void scrolled() override;

        private:
                static void  observe(void *arg, const TextDocument::Change &change);
                static void  blink(void *arg);
                static void  pasted(void *arg, Error err, StringView type, StringView data);
                static void  dropRead(void *arg, Error err, StringView type, StringView data);
                static Error providePrimary(void *arg, StringView type, String &out);
                static void  edgeTick(void *arg);

                Rect         textArea() const; // the viewport less the gutter (window coordinates)
                int          textLeft() const; // the window x of column 0
                TextPosition positionAt(int x, int y) const;
                void         measureAll(); // the widest line
                void         updateContentSize();
                void         paintLine(Painter &painter, uint32_t line, int y, const Rect &area);
                void         typeCharacter(StringView text);
                void         backspace(bool word);
                void         deleteForward(bool word);
                void         newline();
                void         tab(bool out);
                void         replaceSelection(StringView text, TextDocument::EditKind kind);
                void         startBlink();
                void         stopBlink();
                void         offerPrimary();
                void         dragTo(int x, int y);

                TextDocument  _own{MemSpace::Psram}; // its own, shown unless given another
                TextDocument *_document = &_own;
                TextCursor    _cursor{_own};
                Colors        _colors;
                bool          _paletteColors = true; // the window's field colours, until setColors()
                bool          _insertSpaces = false;
                bool          _autoIndent = false;
                bool          _readOnly = false;
                bool          _overwrite = false;
                bool          _cursorOn = true;
                bool          _dragging = false;
                uint8_t       _clicks = 0; // 1, 2 (words), 3 (lines)
                uint64_t      _pressUs = 0;
                TextRange     _dragUnit; // the word or line a double or triple click took
                int           _blinkTimer = -1;
                int           _edgeTimer = -1;
                int           _dragX = 0;
                int           _dragY = 0;
                uint32_t      _widest = 0; // columns, of any line seen
                uint32_t      _dropDrag = 0;
                TextPosition  _dropAt;
                bool          _changedInEdit = false;
                Notify        _changed = nullptr;
                void         *_changedArg = nullptr;
                Notify        _cursorMoved = nullptr;
                void         *_cursorMovedArg = nullptr;
                TextCell     *_cells = nullptr; // a row's, made at the first paint
                size_t        _cellCount = 0;
};

BMDS_NAMESPACE_END
