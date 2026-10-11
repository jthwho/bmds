/**
 * @file      viewlayout.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where the image viewer's picture goes, pure (`devplan/images.md`
 *        §4b): fitted to the view (never enlarged), or at a zoom with a pan;
 *        the part of the picture that shows, and where in the view.
 *
 * Across, positions and widths are on 4-pixel columns (the painter's); the
 * fitted width is rounded down to one (at most 3 pixels off the aspect).
 *
 * @par Thread Safety
 * Reentrant.
 */
struct ViewLayout {
                /** @brief 100%, in thousandths. */
                static constexpr uint32_t Actual = 1000;

                /** @brief The zoom steps: √2 apart, 1/16 to 8×. */
                static constexpr uint32_t Steps[] = {63,   88,   125,  177,  250,  354,  500, 707,
                                                     1000, 1414, 2000, 2828, 4000, 5657, 8000};

                /** @brief What it is laid out for. */
                struct Input {
                                uint32_t sourceWidth = 0; ///< The picture decoded.
                                uint32_t sourceHeight = 0;
                                uint32_t viewWidth = 0; ///< The room for it.
                                uint32_t viewHeight = 0;
                                bool     fit = true;    ///< Fitted; else at `zoom`.
                                uint32_t zoom = Actual; ///< Thousandths of the picture's pixels.
                                int32_t  panX = 0;      ///< The view's top-left corner in the zoomed picture.
                                int32_t  panY = 0;
                };

                uint32_t zoom = Actual; ///< In effect (fitted: the fit's).
                uint32_t width = 0;     ///< The whole picture at the zoom.
                uint32_t height = 0;
                int32_t  panX = 0; ///< Pan kept in bounds (across: a multiple of 4).
                int32_t  panY = 0;
                uint32_t x = 0; ///< Where the part that shows goes in the view.
                uint32_t y = 0;
                uint32_t shownWidth = 0; ///< The part that shows (a multiple of 4).
                uint32_t shownHeight = 0;
                uint32_t cropX = 0; ///< That part, in the decoded picture.
                uint32_t cropY = 0;
                uint32_t cropWidth = 0;
                uint32_t cropHeight = 0;

                /** @brief Lays @p in out (nothing: an empty picture or view). */
                static ViewLayout make(const Input &in);

                /** @brief Returns the next zoom step past @p zoom, in or out (the last if none). */
                static uint32_t step(uint32_t zoom, bool in);

                /** @brief Returns true if the picture is bigger than the view either way (it pans). */
                bool pans(const Input &in) const { return width > in.viewWidth || height > in.viewHeight; }
};

BMDS_NAMESPACE_END
