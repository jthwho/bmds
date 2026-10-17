/**
 * @file      colordialog.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/dialog.h>
#include <bmds/formmodels.h>
#include <bmds/span.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

class LineEdit;

namespace detail {
        class ColorPlane;
        class ColorBar;
} // namespace detail

/**
 * @brief A color shown in a sunken box, clicked to change it (`devplan/gui.md`
 *        §4u): a palette role's or a terminal color's editor.
 *
 * A click, Space or Enter is told to `Clicked` (the owner opens a
 * `ColorDialog`). It takes the focus, shown by a ring; its tooltip is the
 * color as `#rrggbb`.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class ColorSwatch : public Widget {
        public:
                /** @brief Told of a click (on the loop's thread). */
                using Clicked = void (*)(void *arg, ColorSwatch &swatch);

                explicit ColorSwatch(Widget *parent, uint32_t rgb = 0);

                /** @brief Sets the color (0xRRGGBB). */
                void setColor(uint32_t rgb);

                /** @brief Returns the color. */
                uint32_t color() const { return _rgb; }

                /** @brief Sets the size it asks for (40 × 22 by default). */
                void setHint(uint16_t width, uint16_t height);

                /** @brief Has @p fn told of clicks. */
                void setClicked(Clicked fn, void *arg) {
                        _clicked = fn;
                        _clickedArg = arg;
                        return;
                }

                /** @brief Returns a value of the owner's. */
                uintptr_t data() const { return _data; }

                /** @brief Sets a value of the owner's (the role it shows). */
                void setData(uintptr_t data) { _data = data; }

                GuiSize    sizeHint() const override;
                void       paint(Painter &painter) override;
                void       mousePressed(const WindowManager::MouseEvent &event) override;
                void       mouseReleased(const WindowManager::MouseEvent &event) override;
                bool       keyPressed(const KeyEvent &event) override;
                StringView toolTip() const override { return StringView(_tip); }

        private:
                void click();

                uint32_t  _rgb = 0;
                Clicked   _clicked = nullptr;
                void     *_clickedArg = nullptr;
                uintptr_t _data = 0;
                char      _tip[8] = {};
                GuiSize   _hint{40, GuiStyle::ButtonHeight};
                bool      _down = false;
};

/**
 * @brief Chooses a color (`devplan/gui.md` §4u): a hue and saturation square, a
 *        value bar beside it, the new color over the old, its hex code, and a
 *        row of presets.
 *
 * Pressed or dragged, the square sets the hue (across) and the saturation (up)
 * and the bar the value; with the focus, the arrows move them (Shift: ten at a
 * time). The hex field takes `#rrggbb` at Enter or when it loses the focus;
 * a preset, or the old color, is taken when clicked. Each change is told to
 * `Changed` at once (the owner shows it live), and OK or Cancel to
 * `Finished` (`Accepted`, `Rejected`): on Cancel the owner puts the old color
 * back.
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class ColorDialog : public Dialog {
        public:
                /** @brief Told each color the user chooses (on the loop's thread). */
                using Changed = void (*)(void *arg, ColorDialog &dialog, uint32_t rgb);

                /** @brief `Finished`'s results. */
                enum Choice : int {
                        Rejected = 0,
                        Accepted = 1,
                };

                /** @brief The most presets. */
                static constexpr size_t MaxPresets = 16;

                ColorDialog(GuiApplication &app, StringView title);
                ~ColorDialog() override;

                /** @brief Opens it with @p rgb as the old color and the new. @return `Ok` or the window manager's error. */
                Error open(uint32_t rgb);

                /** @brief Returns the color chosen. */
                uint32_t color() const { return _rgb; }

                /** @brief Returns the color it opened with. */
                uint32_t originalColor() const { return _original; }

                /** @brief Sets the presets (the first `MaxPresets`; 16 standard colors by default). */
                void setPresets(Span<const uint32_t> colors);

                /** @brief Has @p fn told of each color chosen. */
                void setChanged(Changed fn, void *arg) {
                        _changed = fn;
                        _changedArg = arg;
                        return;
                }

        private:
                friend class detail::ColorPlane;
                friend class detail::ColorBar;

                static void hexTaken(void *arg, LineEdit &edit);
                static void hexCancelled(void *arg, LineEdit &edit);
                static void presetClicked(void *arg, ColorSwatch &swatch);

                Error build();
                void  choose(uint32_t rgb);                  // from the hex field, a preset, the old color
                void  chooseHsv(const ColorModel::Hsv &hsv); // from the square or the bar
                void  shown(bool hexToo);                    // every part from the color

                detail::ColorPlane *_plane = nullptr;
                detail::ColorBar   *_bar = nullptr;
                ColorSwatch        *_new = nullptr;
                ColorSwatch        *_old = nullptr;
                LineEdit           *_hex = nullptr;
                ColorSwatch        *_presets[MaxPresets] = {};
                ColorModel::Hsv     _hsv;
                uint32_t            _rgb = 0;
                uint32_t            _original = 0;
                Changed             _changed = nullptr;
                void               *_changedArg = nullptr;
};

BMDS_NAMESPACE_END
