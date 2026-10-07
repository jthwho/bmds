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
 * It looks like a field, or like a desktop icon's name (§4r): no frame, the
 * desktop's colors, centred and wrapped, as tall as its lines (it grows and
 * shrinks as they change), so the editor looks as the name it replaces.
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class PopupEdit final : public GuiWindow {
        public:
                /** @brief Told how it ended: @p accepted (Enter, a press elsewhere) with @p text, or cancelled. */
                using Done = void (*)(void *arg, bool accepted, StringView text);

                /** @brief How it looks. */
                enum class Look : uint8_t {
                        Field,    ///< A field's bevel and colors, a line.
                        IconName, ///< A desktop icon's name: no frame, wrapped and centred.
                };

                /** @brief The most lines it grows to (`Look::IconName`). */
                static constexpr size_t MaxLines = 8;

                explicit PopupEdit(GuiApplication &app);
                ~PopupEdit() override = default;

                /**
                 * @brief Opens it at @p screen's corner, @p screen's width, with @p text
                 *        all selected; @p fn told how it ends. A field is a button's
                 *        height; an icon's name as tall as its lines.
                 * @return `Ok`, `NoMem`, or the window manager's error.
                 */
                Error open(const Rect &screen, StringView text, Look look, Done fn, void *arg);

                /** @brief Returns its editor (null before the first `open()`): to select part, to limit it. */
                LineEdit *edit() const { return _edit; }

        protected:
                void closedByManager() override;

        private:
                static void accepted(void *arg, LineEdit &edit);
                static void cancelled(void *arg, LineEdit &edit);
                static void linesChanged(void *arg, LineEdit &edit, size_t lines);
                uint16_t    heightFor(size_t lines) const;

                void finish(bool accepted); // closed, then told (once)

                LineEdit *_edit = nullptr;
                Done      _done = nullptr;
                void     *_doneArg = nullptr;
                bool      _open = false;
                Look      _look = Look::Field;
};

BMDS_NAMESPACE_END
