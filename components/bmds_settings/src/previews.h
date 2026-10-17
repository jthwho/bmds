/**
 * @file      previews.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The window manager's palette on a small desktop (`devplan/controlpanel.md`
 *        §12): an inactive window behind an active one with its title, buttons, a
 *        button, a field, text, disabled text, a selected item, a tooltip and the
 *        pointer, all in the colors set now (`wm/palette/...`).
 *
 * @par Thread Safety
 * As `Widget`.
 */
class PalettePreview final : public Widget {
        public:
                explicit PalettePreview(Widget *parent);

                /** @brief Reads the colors again (they changed). */
                void reload();

                GuiSize  sizeHint() const override;
                uint16_t heightForWidth(uint16_t width) const override;
                void     paint(Painter &painter) override;

        private:
                static constexpr size_t Roles = 25;

                void window(Painter &painter, const Rect &r, bool active, const char *title);

                uint32_t _c[Roles] = {};
};

/**
 * @brief The terminal's colors on a short session (`devplan/controlpanel.md` §12): a
 *        prompt, a listing, a warning and an error, the 16 colors, and the cursor,
 *        in the colors set now (`terminal/colors/...`).
 *
 * @par Thread Safety
 * As `Widget`.
 */
class TerminalPreview final : public Widget {
        public:
                explicit TerminalPreview(Widget *parent);

                /** @brief Reads the colors again (they changed). */
                void reload();

                GuiSize  sizeHint() const override;
                uint16_t heightForWidth(uint16_t width) const override;
                void     paint(Painter &painter) override;

        private:
                static constexpr size_t Colors = 19; // text, background, cursor, the 16

                uint32_t _c[Colors] = {};
};

BMDS_NAMESPACE_END
