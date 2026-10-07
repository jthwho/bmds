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
 * @brief A line of text to edit (`devplan/gui.md` §4q), in a field's bevel.
 *
 * Characters typed go in at the cursor; Backspace and Delete; Left, Right,
 * Home and End (Shift selects); Ctrl+A selects all; Ctrl+C, Ctrl+X, Ctrl+V
 * copy, cut and paste text (the application's clipboard); Enter accepts, Esc
 * cancels. A press puts the cursor, a drag selects, a double click selects
 * all. It scrolls a character at a time to keep the cursor shown. Make it the
 * window's focus widget (`GuiWindow::setFocusWidget()`) for the keys.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class LineEdit : public Widget {
        public:
                /** @brief Told of Enter or Esc (on the loop's thread). */
                using Done = void (*)(void *arg, LineEdit &edit);

                explicit LineEdit(Widget *parent);
                ~LineEdit() override;

                /** @brief Sets the text (cut at `LineEditModel::MaxBytes`), the cursor at its end. */
                void setText(StringView text);

                /** @brief Returns the text. */
                StringView text() const { return _model.text(); }

                /** @brief Selects all of it. */
                void selectAll();

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

                GuiSize sizeHint() const override;
                bool    keyPressed(const KeyEvent &event) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                void    paint(Painter &painter) override;

        protected:
                void resized() override;

        private:
                static void pasted(void *arg, Error err, StringView type, StringView data);

                Rect   textArea() const;      // inside the bevel, less the padding
                size_t offsetAt(int x) const; // the character boundary nearest @p x
                void   reveal();              // the first character shown, so the cursor is
                void   changed();             // revealed and redrawn

                LineEditModel _model;
                size_t        _first = 0; // the first byte shown
                uint64_t      _pressUs = 0;
                bool          _dragging = false;
                Done          _accepted = nullptr;
                void         *_acceptedArg = nullptr;
                Done          _cancelled = nullptr;
                void         *_cancelledArg = nullptr;
};

BMDS_NAMESPACE_END
