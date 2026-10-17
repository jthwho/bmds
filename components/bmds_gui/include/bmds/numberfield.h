/**
 * @file      numberfield.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/formmodels.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class LineEdit;

/**
 * @brief A whole number in a range, typed or stepped (`devplan/gui.md` §4u):
 *        a field, and up and down arrows beside it.
 *
 * Typed text is taken at Enter or when the field loses the focus: a number
 * past the range is kept at its end, anything else puts the value back; Esc
 * puts it back too. Up and Down step, Page Up and Page Down step a page, and
 * so does the wheel while it has the focus. An arrow pressed steps, and
 * repeats while held (after 400 ms, every 50 ms).
 *
 * `Changed` is told the new value once per change the user makes: each key,
 * each notch, typed text taken, and once when a held arrow is let go (not at
 * each repeat). `setValue()` isn't told.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class NumberField : public Widget {
        public:
                /** @brief Told the value the user chose (on the loop's thread). */
                using Changed = void (*)(void *arg, NumberField &field, int32_t value);

                /** @brief The arrows' width. */
                static constexpr uint16_t ArrowWidth = 16;

                explicit NumberField(Widget *parent);
                ~NumberField() override;

                /** @brief Sets the range (the value kept in it, shown). */
                void setRange(int32_t minimum, int32_t maximum);

                /** @brief Sets an arrow's step and a page's. */
                void setSteps(uint32_t step, uint32_t page);

                /** @brief Returns the value. */
                int32_t value() const { return _model.value(); }

                /** @brief Sets the value (kept in range, shown; not told to `Changed`). */
                void setValue(int32_t value);

                /** @brief Returns the field (a label's buddy), or null (no memory). */
                LineEdit *edit() const { return _edit; }

                /** @brief Has @p fn told of the user's changes. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                void    mouseLeft() override;
                bool    keyBeforeChild(const KeyEvent &event) override;
                bool    wheelTurned(int notches) override;

        protected:
                void resized() override;

        private:
                static void accepted(void *arg, LineEdit &edit);
                static void cancelled(void *arg, LineEdit &edit);
                static void tick(void *arg);

                Rect arrows() const;              // the arrows' strip
                int  arrowAt(int x, int y) const; // +1 up, -1 down, 0 none
                void show();                      // the value as text
                void take();                      // the text as the value, told
                void stepBy(int64_t count, bool page, bool tell);
                void stopRepeat();

                NumberModel _model;
                LineEdit   *_edit = nullptr;
                Changed     _changed = nullptr;
                void       *_changedArg = nullptr;
                int32_t     _pressValue = 0; // before an arrow was pressed: told once at its release
                int         _timer = -1;
                int8_t      _pressed = 0; // the arrow held: +1, -1, or 0
                bool        _repeating = false;
};

BMDS_NAMESPACE_END
