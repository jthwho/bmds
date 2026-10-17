/**
 * @file      lineedit.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/lineeditmodel.h>
#include <bmds/stringview.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A line of text to edit (`devplan/gui.md` §4q), in a field's bevel,
 *        or drawn as a label is (§4r: no bevel, its colors, centred, wrapped).
 *
 * Characters typed go in at the cursor; Backspace and Delete; Left, Right,
 * Home and End (Shift selects); Ctrl+A selects all; Ctrl+C, Ctrl+X, Ctrl+V
 * copy, cut and paste text (the application's clipboard); Enter accepts, Esc
 * cancels. A press puts the cursor, a drag selects, a double click selects
 * all; Enter and Esc go on to the window (a dialog's buttons) when nothing
 * is told of them. A drag of text, or of files (their paths, spaces between), dropped on it
 * replaces its text, unless masked (`devplan/clipboard.md` §5d). It scrolls a character at
 * a time to keep the cursor shown; wrapped, it
 * breaks its lines as `TextWrap` does, Up and Down move between them, and it
 * asks to be as tall as they are (`setLinesChanged()`) rather than scroll.
 * Make it the window's focus widget (`GuiWindow::setFocusWidget()`) for the
 * keys.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class LineEdit : public Widget {
        public:
                /** @brief Told of Enter or Esc (on the loop's thread). */
                using Done = void (*)(void *arg, LineEdit &edit);

                /** @brief Told that the wrapped text now takes @p lines lines (`setWrapping()`). */
                using LinesChanged = void (*)(void *arg, LineEdit &edit, size_t lines);

                /** @brief The wrapped lines laid out at most (the rest aren't drawn). */
                static constexpr size_t MaxLines = 16;

                /** @brief Inside its edges, beside the text. */
                static constexpr uint16_t Pad = 4;

                explicit LineEdit(Widget *parent);
                ~LineEdit() override;

                /** @brief Sets the text (cut at `LineEditModel::MaxBytes`), the cursor at its end. */
                void setText(StringView text);

                /** @brief Returns the text. */
                StringView text() const { return _model.text(); }

                /** @brief Selects all of it. */
                void selectAll();

                /** @brief Selects from @p start to @p end (byte offsets), the cursor at @p end. */
                void setSelection(size_t start, size_t end);

                /** @brief Stops typing at @p bytes (at most `LineEditModel::MaxBytes`). */
                void setMaxLength(size_t bytes) { _model.setMaxBytes(bytes); }

                /** @brief Draws it in a field's bevel (the default), or without one. */
                void setFramed(bool framed);

                /** @brief Sets its colors: the background's and the text's (a field's by default). */
                void setColors(WindowPalette::Role background, WindowPalette::Role text);

                /** @brief Shows a star for each byte instead of the text, and doesn't copy it (a password). */
                void setMasked(bool masked);

                /** @brief Centres each line (as a desktop icon's name), else from the left. */
                void setCentered(bool centered);

                /** @brief Wraps the text in lines of its width instead of scrolling it. */
                void setWrapping(bool wrapping);

                /** @brief Returns the lines the text takes (1 unwrapped). */
                size_t lineCount() const;

                /** @brief Has @p fn told when the wrapped text's lines change. */
                void setLinesChanged(LinesChanged fn, void *arg) {
                        _linesChanged = fn;
                        _linesChangedArg = arg;
                        return;
                }

                /** @brief Has @p fn told when Enter is pressed. */
                void setAccepted(Done fn, void *arg) {
                        _accepted = fn;
                        _acceptedArg = arg;
                        return;
                }

                /** @brief Has @p fn told when Esc is pressed. */
                void setCancelled(Done fn, void *arg) {
                        _cancelled = fn;
                        _cancelledArg = arg;
                        return;
                }

                /** @brief Has @p fn told after each change the user makes to the text (typed, deleted, pasted). */
                void setEdited(Done fn, void *arg) {
                        _edited = fn;
                        _editedArg = arg;
                        return;
                }

                /**
                 * @brief Has @p fn told when it stops being the focus widget (a field
                 *        commits what was typed then, as at Enter).
                 */
                void setFocusLost(Done fn, void *arg) {
                        _focusLost = fn;
                        _focusLostArg = arg;
                        return;
                }

                GuiSize sizeHint() const override;
                bool    keyPressed(const KeyEvent &event) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                void    paint(Painter &painter) override;
                void    focusOut() override;
                void    dragMoved(GuiDropEvent &event) override;
                bool    dropped(GuiDropEvent &event) override;

        protected:
                void resized() override;

        private:
                static void pasted(void *arg, Error err, StringView type, StringView data);
                static void dropRead(void *arg, Error err, StringView type, StringView data);

                // A line shown: its bytes [start, end) and where it is drawn from.
                struct Line {
                                size_t   start;
                                size_t   end;
                                uint16_t x;
                };

                StringView drawn() const;          // the text as shown (stars when masked)
                Rect   textArea() const;       // inside the bevel, less the padding
                size_t lines(Line *out) const; // at least one, `MaxLines` at most
                size_t lineOf(const Line *lines, size_t count, size_t offset) const;
                size_t offsetAt(const Line &line, int x) const; // the character boundary nearest @p x
                size_t offsetAt(int x, int y) const;
                void   moveLine(int by, bool select); // Up, Down
                void   reveal();                      // the first character shown, so the cursor is
                void   changed();                     // revealed and redrawn
                void   paintRun(Painter &painter, const Line &line, uint16_t y, size_t from, size_t to, uint32_t &x,
                                uint32_t fg, uint32_t bg) const;

                LineEditModel _model;
                size_t        _first = 0; // the first byte shown
                uint64_t      _pressUs = 0;
                bool          _dragging = false;
                uint32_t            _dropDrag = 0; // a drop being read
                Done          _accepted = nullptr;
                void         *_acceptedArg = nullptr;
                Done          _cancelled = nullptr;
                void         *_cancelledArg = nullptr;
                Done                _edited = nullptr;
                void               *_editedArg = nullptr;
                Done                _focusLost = nullptr;
                void               *_focusLostArg = nullptr;
                LinesChanged        _linesChanged = nullptr;
                void               *_linesChangedArg = nullptr;
                size_t              _lineCount = 1;                                   // told last
                WindowPalette::Role _background = WindowPalette::Role::FaceHighlight; // a field's white
                WindowPalette::Role _foreground = WindowPalette::Role::FaceText;
                bool                _framed = true;
                bool                _centered = false;
                bool                _wrapping = false;
                bool                _masked = false;
};

BMDS_NAMESPACE_END
