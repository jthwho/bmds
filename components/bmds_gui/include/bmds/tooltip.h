/**
 * @file      tooltip.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/guiwindow.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An application's tooltip (`devplan/gui.md` §4s): a widget's
 *        `toolTip()`, shown in a small window by the pointer after it rests
 *        on the widget for `DelayMs`, a line at each `\n`. Made by
 *        `GuiApplication` the first time a widget with a tooltip is hovered.
 *
 * Its window is on the window manager's `Tooltip` level: the pointer passes
 * through it, it never takes the keys, and it closes no menus.
 *
 * @par Thread Safety
 * The application's lock (its loop's thread).
 */
class ToolTip final : public GuiWindow {
        public:
                /** @brief The pointer's rest before it shows. */
                static constexpr uint32_t DelayMs = 600;

                /** @brief The longest text shown, in bytes (a tray icon's longest). */
                static constexpr size_t MaxText = 127;

                /** @brief The most lines shown. */
                static constexpr size_t MaxLines = 4;

                explicit ToolTip(GuiApplication &app);
                ~ToolTip() override;

                /**
                 * @brief The pointer is over @p widget (null: none) at (@p screenX,
                 *        @p screenY): a new widget's tooltip shows after the delay.
                 */
                void hover(Widget *widget, int screenX, int screenY);

                /** @brief Hides it (a press, a key, the pointer gone) until the next widget. */
                void hide();

                /** @brief @p widget's text changed: shown again with it (in place if as big), if showing. */
                void changed(Widget *widget);

                /** @brief @p widget (and what is inside it) is being destroyed. */
                void forget(Widget *widget);

                void paint(Painter &painter) override;

        private:
                static void due(void *arg);
                void        open();
                static StringView nextLine(StringView &rest); // the text up to a \n, @p rest after it

                Widget *_target = nullptr; // the widget hovered (shown or waiting)
                int     _x = 0;            // the pointer when it came, on the screen
                int     _y = 0;
                int     _timer = -1;
                bool    _hidden = false; // hidden for this widget: not again until another
                char    _text[MaxText + 1] = {};
};

BMDS_NAMESPACE_END
