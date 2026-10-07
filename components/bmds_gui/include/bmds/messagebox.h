/**
 * @file      messagebox.h
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
#include <bmds/guiwindow.h>
#include <bmds/stringview.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A standard message for the user: an icon, text, buttons
 *        (`devplan/gui.md` §4q).
 *
 * `show()` opens one, centred in the work area and above other windows; the
 * answer is told on the loop's thread, then it goes by itself. Its buttons
 * have mnemonics (&OK, &Cancel, &Yes, &No); the first is the default and has
 * the focus, Tab, Left and Right move it, Space or Enter clicks the focused
 * one; Esc and the close box choose its cancelling one (Cancel, else No,
 * else OK). It doesn't stop the application's other windows: the caller
 * waits for the answer.
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class MessageBox final : public GuiWindow {
        public:
                /** @brief What it says. */
                enum class Kind : uint8_t {
                        Information,
                        Warning,
                        Error,
                        Question,
                };

                /** @brief Its buttons, left to right, the first the default. */
                enum class Buttons : uint8_t {
                        Ok,
                        OkCancel,
                        YesNo,
                        YesNoCancel,
                };

                /** @brief The button chosen. */
                enum class Answer : uint8_t {
                        Ok,
                        Cancel,
                        Yes,
                        No,
                };

                /** @brief Told the answer (on the loop's thread). */
                using Done = void (*)(void *arg, Answer answer);

                /** @brief The text's lines at most. */
                static constexpr size_t MaxLines = 12;

                /** @brief The text's columns at most. */
                static constexpr uint16_t MaxColumns = 50;

                /** @brief The text kept, in bytes. */
                static constexpr size_t MaxText = 600;

                /**
                 * @brief Opens a message box of @p app (@p text cut at `MaxText` bytes);
                 *        @p fn (may be null) is told the answer.
                 * @return `Ok`, `NoMem`, or the window manager's error.
                 */
                static Error show(GuiApplication &app, Kind kind, StringView title, StringView text, Buttons buttons,
                                  Done fn, void *arg);

                bool keyPressed(const KeyEvent &event) override;
                void paint(Painter &painter) override;

        protected:
                void closedByManager() override;

        private:
                static constexpr size_t MaxButtons = 3;

                MessageBox(GuiApplication &app, const Config &config) : GuiWindow(app, config) {}
                ~MessageBox() override;

                static void clicked(void *arg, Button &button);
                static void destroy(void *arg); // posted: never inside its own handler

                void   answer(Answer answer);
                Answer cancelling() const;

                Kind       _kind = Kind::Information;
                Buttons    _buttons = Buttons::Ok;
                Done       _done = nullptr;
                void      *_doneArg = nullptr;
                bool       _answered = false;
                char       _text[MaxText + 1] = {};
                StringView _lines[MaxLines];
                size_t     _lineCount = 0;
                Button    *_buttonWidgets[MaxButtons] = {};
                Answer     _answers[MaxButtons] = {};
                size_t     _buttonCount = 0;
                uint32_t  *_iconPixels = nullptr; // a copy (the theme's cache may drop its own)
                uint16_t   _iconWidth = 0;
                uint16_t   _iconHeight = 0;
                Rect       _iconRect;
                Rect       _textRect;
};

BMDS_NAMESPACE_END
