/**
 * @file      windowstack.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/compose.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/rect.h>
#include <bmds/region.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A framed window's parts, from its outer rectangle
 *        (`devplan/windows.md` §4a): a title bar a font row high, 4-pixel
 *        sides and bottom, a close box at the title bar's left.
 *
 * Every vertical edge is on a 4-pixel column when the outer rectangle is.
 */
struct WindowFrame {
                static constexpr uint16_t Border = 4;       ///< The sides' and the bottom's thickness.
                static constexpr uint16_t TitleHeight = 20; ///< A 16-pixel font row and 2 pixels above and below.
                static constexpr uint16_t CloseSize = 12;   ///< The close box's side.
                static constexpr uint16_t OutlineWidth = 4; ///< A move's outline.
                static constexpr uint16_t MinWidth = 64;
                static constexpr uint16_t MinHeight = TitleHeight + Border + 8;

                /** @brief Returns the client area: inside the sides, under the title bar. */
                static constexpr Rect client(const Rect &outer) {
                        return Rect{uint16_t(outer.x + Border), uint16_t(outer.y + TitleHeight),
                                    uint16_t(outer.width - 2 * Border), uint16_t(outer.height - TitleHeight - Border)};
                }

                /** @brief Returns the title bar. */
                static constexpr Rect titleBar(const Rect &outer) {
                        return Rect{outer.x, outer.y, outer.width, TitleHeight};
                }

                /** @brief Returns the close box. */
                static constexpr Rect closeBox(const Rect &outer) {
                        return Rect{uint16_t(outer.x + Border), uint16_t(outer.y + (TitleHeight - CloseSize) / 2),
                                    CloseSize, CloseSize};
                }

                /** @brief Returns where the title is written (one font row; may be empty). */
                static constexpr Rect title(const Rect &outer) {
                        const uint16_t left = Border + CloseSize + 8;
                        const uint16_t width = outer.width > left + Border ? outer.width - left - Border : 0;
                        return Rect{uint16_t(outer.x + left), uint16_t(outer.y + 2), width, uint16_t(TitleHeight - 4)};
                }

                /** @brief Writes the frame's rectangles but the title bar (sides, bottom) into @p out. */
                static constexpr void edges(const Rect &outer, Rect (&out)[3]) {
                        const uint16_t sideY = uint16_t(outer.y + TitleHeight);
                        const uint16_t sideH = uint16_t(outer.height - TitleHeight);
                        out[0] = Rect{outer.x, sideY, Border, sideH};
                        out[1] = Rect{uint16_t(outer.right() - Border), sideY, Border, sideH};
                        out[2] = Rect{uint16_t(outer.x + Border), uint16_t(outer.bottom() - Border),
                                      uint16_t(outer.width - 2 * Border), Border};
                        return;
                }

                /** @brief Writes @p outer's outline (`OutlineWidth` thick) into @p out. */
                static constexpr void outline(const Rect &outer, Rect (&out)[4]) {
                        const uint16_t t = OutlineWidth;
                        out[0] = Rect{outer.x, outer.y, outer.width, t};
                        out[1] = Rect{outer.x, uint16_t(outer.bottom() - t), outer.width, t};
                        out[2] = Rect{outer.x, uint16_t(outer.y + t), t, uint16_t(outer.height - 2 * t)};
                        out[3] = Rect{uint16_t(outer.right() - t), uint16_t(outer.y + t), t,
                                      uint16_t(outer.height - 2 * t)};
                        return;
                }
};

/**
 * @brief The windows on a screen, front to back, and the damage their
 *        changes leave (`devplan/windows.md` §4a).
 *
 * Windows are numbered from 1 (the lowest free number is reused) and kept
 * wholly on the screen, their x and width on `Align`-pixel columns. Each
 * change adds to one screen `Region` exactly the pixels whose owner (the
 * topmost window there, or the desktop) changed: what an opened window
 * shows, what a closed or moved one showed and shows, what a raised one
 * had covered. `invalidate()` adds a window's own changes, clipped to what
 * of it is visible. `plan()` then tells which layer draws each piece.
 *
 * Layers are counted from the top: layer `z` is the `z`-th window from the
 * front, layer `count()` the desktop.
 *
 * @par Thread Safety
 * A plain value.
 */
class WindowStack {
        public:
                /** @brief Most windows. */
                static constexpr size_t MaxWindows = 16;

                /** @brief The pixel multiple of window x and widths. */
                static constexpr uint16_t Align = 4;

                /** @brief An empty stack on a screen of @p width × @p height, all damaged. */
                WindowStack(uint16_t width, uint16_t height) { reset(width, height); }

                /** @brief Empties it on a screen of @p width × @p height, all damaged. */
                void reset(uint16_t width, uint16_t height);

                /** @brief Returns the screen. */
                Rect screen() const { return _screen; }

                /** @brief Returns the windows open. */
                size_t count() const { return _count; }

                /** @brief Returns the number of layer @p z's window (0: the top). */
                uint8_t idAt(size_t z) const { return _ids[z]; }

                /** @brief Returns layer @p z's rectangle (`count()`: the screen). */
                Rect rectAt(size_t z) const { return z < _count ? _rects[z] : _screen; }

                /** @brief Returns window @p id's layer, or `NotFound`. */
                Result<size_t> layerOf(uint8_t id) const;

                /** @brief Returns true if @p rect may be a window's: on the screen, aligned. */
                bool isPlaceable(const Rect &rect) const;

                /**
                 * @brief Opens a window at @p rect, on top.
                 * @return Its number, `Invalid` (not placeable) or `Full`.
                 */
                Result<uint8_t> open(const Rect &rect);

                /** @brief Closes window @p id. @return `Ok` or `NotFound`. */
                Error close(uint8_t id);

                /** @brief Moves window @p id to @p rect (any size). @return `Ok`, `NotFound` or `Invalid`. */
                Error move(uint8_t id, const Rect &rect);

                /** @brief Puts window @p id on top. @return `Ok` or `NotFound`. */
                Error raise(uint8_t id);

                /**
                 * @brief Adds what of @p rect (screen coordinates) window @p id
                 *        shows to the damage.
                 * @return `Ok` or `NotFound`.
                 */
                Error invalidate(uint8_t id, const Rect &rect);

                /** @brief Adds what of @p rect the desktop shows to the damage. */
                void invalidateDesktop(const Rect &rect);

                /** @brief Adds @p rect, whoever shows it, to the damage. */
                void invalidateScreen(const Rect &rect);

                /** @brief Returns the damage gathered. */
                const Region &damage() const { return _damage; }

                /** @brief Forgets the damage. */
                void clearDamage() { _damage.clear(); }

                /**
                 * @brief Which layer shows each piece of @p damage: disjoint
                 *        pieces, the desktop's layer `count()`.
                 * @return The pieces written to @p out, or `Full` (`planCompose()`).
                 */
                Result<size_t> plan(const Region &damage, Span<ComposePiece> out) const;

        private:
                void addShown(size_t z, const Rect &rect); // what of rect layer z shows, to the damage
                void layers(Rect (&out)[MaxWindows + 1]) const;

                Rect    _screen;
                Rect    _rects[MaxWindows]; // top first
                uint8_t _ids[MaxWindows] = {};
                uint8_t _count = 0;
                Region  _damage;
};

BMDS_NAMESPACE_END
