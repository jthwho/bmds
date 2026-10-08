/**
 * @file      icon.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An icon's picture (`devplan/gui.md` §4h): @p width × @p height pixels,
 *        0xAARRGGBB, rows top first, kept by its owner while it is used. The
 *        sizes drawn are 16 × 16 (menus, title bars, buttons) and 32 × 32.
 */
struct Icon {
                uint16_t        width = 0;
                uint16_t        height = 0;
                const uint32_t *pixels = nullptr;

                /** @brief Returns true if it has pixels. */
                bool isValid() const { return pixels != nullptr && width != 0 && height != 0; }
};

/** @brief Returns @p argb over the opaque @p rgb (0xRRGGBB): its alpha blended. @hotpath */
constexpr uint32_t blendOver(uint32_t argb, uint32_t rgb) {
        const uint32_t a = argb >> 24;
        if (a == 0xff) return argb & 0xffffff;
        if (a == 0) return rgb & 0xffffff;
        uint32_t out = 0;
        for (int shift = 0; shift < 24; shift += 8) {
                const uint32_t s = argb >> shift & 0xff;
                const uint32_t d = rgb >> shift & 0xff;
                out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
        }
        return out;
}

/** @brief Returns @p argb with its alpha halved: a pixel of an icon drawn dimmed (a cut file's). */
constexpr uint32_t halfAlpha(uint32_t argb) {
        return ((argb >> 25) << 24) | (argb & 0xffffff);
}

BMDS_NAMESPACE_END
