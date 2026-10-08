/**
 * @file      dialog.h
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
 * @brief A window asking something: content above a row of buttons at the
 *        bottom right (`devplan/filebrowser.md` §4e).
 *
 * The content is a column (`content()`, a `BoxLayout`) the caller fills.
 * `addButton()` adds a button with a result; the first is the default and has
 * the focus. Escape and the close box choose the cancelling result
 * (`setCancelResult()`; the last button's by default). `open()` centres it in
 * the work area, above other windows. A button's result, or the cancelling
 * one, is told to `Finished` once and the window closes; it may be opened
 * again. It doesn't stop the application's other windows.
 *
 * The caller owns it; it is not destroyed in its own `Finished` (post that).
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class Dialog : public GuiWindow {
        public:
                /** @brief Told the result (on the loop's thread). */
                using Finished = void (*)(void *arg, Dialog &dialog, int result);

                static constexpr size_t   MaxButtons = 4;
                static constexpr uint16_t Margin = 12;

                /** @brief A dialog titled @p title whose content area is @p width × @p height. */
                Dialog(GuiApplication &app, StringView title, uint16_t width, uint16_t height);

                /** @brief Returns the content area's column, to fill. */
                BoxLayout *content() const { return _content; }

                /**
                 * @brief Adds a button (`&` marking its key) choosing @p result.
                 * @return The button, or null (`MaxButtons` already, or no memory).
                 */
                Button *addButton(StringView text, int result);

                /** @brief Sets the result Escape and the close box choose. */
                void setCancelResult(int result) {
                        _cancel = result;
                        _cancelSet = true;
                        return;
                }

                /** @brief Has @p fn told the result. */
                void setFinished(Finished fn, void *arg) {
                        _finished = fn;
                        _finishedArg = arg;
                        return;
                }

                /** @brief Opens it, centred in the work area. @return `Ok`, or the window manager's error. */
                Error open();

                /** @brief Closes it, telling @p result (once per opening). */
                void finish(int result);

                bool keyPressed(const KeyEvent &event) override;
                void paint(Painter &painter) override;

        protected:
                void closedByManager() override;
                void resized() override;

        private:
                static void clicked(void *arg, Button &button);
                int         cancelResult() const;

                BoxLayout *_content = nullptr;
                Button    *_buttons[MaxButtons] = {};
                int        _results[MaxButtons] = {};
                size_t     _count = 0;
                uint16_t   _width;
                uint16_t   _height;
                int        _cancel = 0;
                bool       _cancelSet = false;
                bool       _finishedOnce = false;
                Finished   _finished = nullptr;
                void      *_finishedArg = nullptr;
};

BMDS_NAMESPACE_END
