/**
 * @file      popupedit.h
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
#include <bmds/lineedit.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A line edited in place (`devplan/gui.md` §4q): a `LineEdit` in a
 *        popup window over something's name, as file managers rename.
 *
 * `open()` shows it with the text all selected. Enter, or a press anywhere
 * else, accepts; Esc cancels. Either way it closes, then tells its `Done`
 * (which may open it again: to go on editing after a message, say).
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class PopupEdit final : public GuiWindow {
        public:
                /** @brief Told how it ended: @p accepted (Enter, a press elsewhere) with @p text, or cancelled. */
                using Done = void (*)(void *arg, bool accepted, StringView text);

                explicit PopupEdit(GuiApplication &app);
                ~PopupEdit() override = default;

                /**
                 * @brief Opens it at @p screen (its height a field's) with @p text all
                 *        selected; @p fn told how it ends.
                 * @return `Ok`, `NoMem`, or the window manager's error.
                 */
                Error open(const Rect &screen, StringView text, Done fn, void *arg);

        protected:
                void closedByManager() override;

        private:
                static void accepted(void *arg, LineEdit &edit);
                static void cancelled(void *arg, LineEdit &edit);

                void finish(bool accepted); // closed, then told (once)

                LineEdit *_edit = nullptr;
                Done      _done = nullptr;
                void     *_doneArg = nullptr;
                bool      _open = false;
};

BMDS_NAMESPACE_END
