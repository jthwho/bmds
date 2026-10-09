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
#include <bmds/list.h>
#include <bmds/rect.h>
#include <bmds/region.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A window's parts, from its outer rectangle and the decorations it
 *        chose (`devplan/windows.md` §4a, §4f).
 *
 * The decorations are flags: a `Border` (4-pixel sides and bottom, and a
 * top when there is no title bar), a `TitleBar` a font row high, and on it
 * a menu button at the left and minimize, maximize and close buttons at the
 * right. Without any, the window is its client area. A `Resizable` window
 * (it needs the border) is sized by handles: the outer 4-pixel ring, each
 * corner's within 16 pixels of it (§4g). Every vertical edge is on a 4-pixel
 * column when the outer rectangle is.
 *
 * @par Thread Safety
 * A plain value.
 */
struct WindowFrame {
                /** @name Decorations (flags; the buttons need `TitleBar`, `Resizable` needs `Border`) */
                ///@{
                static constexpr uint8_t Border = 1u << 0;
                static constexpr uint8_t TitleBar = 1u << 1;
                static constexpr uint8_t MenuButton = 1u << 2;
                static constexpr uint8_t MinimizeButton = 1u << 3;
                static constexpr uint8_t MaximizeButton = 1u << 4;
                static constexpr uint8_t CloseButton = 1u << 5;
                static constexpr uint8_t Resizable = 1u << 6;
                static constexpr uint8_t None = 0;
                static constexpr uint8_t Standard = 0x7f; ///< All of them.
                ///@}

                /** @name The edges a resize handle holds (`edgesOf()`) */
                ///@{
                static constexpr uint8_t LeftEdge = 1u << 0;
                static constexpr uint8_t RightEdge = 1u << 1;
                static constexpr uint8_t TopEdge = 1u << 2;
                static constexpr uint8_t BottomEdge = 1u << 3;
                ///@}

                /** @brief What a point of a window is on. */
                enum class Part : uint8_t {
                        Outside,  ///< Not the window.
                        Client,   ///< The client area.
                        Edge,     ///< The border.
                        TitleBar, ///< The title bar but its buttons.
                        Menu,     ///< The buttons.
                        Minimize,
                        Maximize,
                        Close,
                        ResizeLeft, ///< The resize handles (a resizable window's).
                        ResizeRight,
                        ResizeTop,
                        ResizeBottom,
                        ResizeTopLeft,
                        ResizeTopRight,
                        ResizeBottomLeft,
                        ResizeBottomRight,
                };

                static constexpr uint16_t BorderWidth = 4;   ///< The border's thickness.
                static constexpr uint16_t TitleHeight = 20;  ///< A 16-pixel font row and 2 pixels above and below.
                static constexpr uint16_t ButtonSize = 16;   ///< A button's side.
                static constexpr uint16_t ButtonGap = 4;     ///< Between buttons, and from the title bar's ends.
                static constexpr uint16_t OutlineWidth = 4;  ///< A move's outline.
                static constexpr uint16_t MinClient = 8;     ///< The smallest client side.
                static constexpr uint16_t HandleCorner = 16; ///< How far a corner's handle reaches along the ring.

                Rect    outer;
                uint8_t decorations = Standard;

                constexpr WindowFrame() = default;
                constexpr WindowFrame(const Rect &rect, uint8_t flags) : outer(rect), decorations(flags) {}

                /**
                 * @brief Returns true if @p flags are decorations: known bits,
                 *        buttons only with a title bar, handles only with a border.
                 */
                static constexpr bool isValid(uint8_t flags) {
                        constexpr uint8_t Buttons = MenuButton | MinimizeButton | MaximizeButton | CloseButton;
                        return (flags & ~Standard) == 0 && ((flags & TitleBar) != 0 || (flags & Buttons) == 0) &&
                               ((flags & Border) != 0 || (flags & Resizable) == 0);
                }

                /** @brief Returns the edges resize handle @p part holds (none for other parts). */
                static constexpr uint8_t edgesOf(Part part) {
                        switch (part) {
                                case Part::ResizeLeft: return LeftEdge;
                                case Part::ResizeRight: return RightEdge;
                                case Part::ResizeTop: return TopEdge;
                                case Part::ResizeBottom: return BottomEdge;
                                case Part::ResizeTopLeft: return TopEdge | LeftEdge;
                                case Part::ResizeTopRight: return TopEdge | RightEdge;
                                case Part::ResizeBottomLeft: return BottomEdge | LeftEdge;
                                case Part::ResizeBottomRight: return BottomEdge | RightEdge;
                                default: return 0;
                        }
                }

                /** @brief Returns the frame's width left and right of the client area. */
                static constexpr uint16_t sideWidth(uint8_t flags) { return (flags & Border) != 0 ? BorderWidth : 0; }

                /** @brief Returns the frame's height above the client area. */
                static constexpr uint16_t topHeight(uint8_t flags) {
                        return (flags & TitleBar) != 0 ? TitleHeight : sideWidth(flags);
                }

                /** @brief Returns the outer width around a client area @p width wide. */
                static constexpr uint16_t outerWidth(uint16_t width, uint8_t flags) {
                        return uint16_t(width + 2 * sideWidth(flags));
                }

                /** @brief Returns the outer height around a client area @p height high. */
                static constexpr uint16_t outerHeight(uint16_t height, uint8_t flags) {
                        return uint16_t(height + topHeight(flags) + sideWidth(flags));
                }

                /** @brief Returns the narrowest window: its buttons and some title, or a small client area. */
                static constexpr uint16_t minWidth(uint8_t flags) {
                        uint16_t buttons = 0;
                        for (uint8_t b = MenuButton; b <= CloseButton; b = uint8_t(b << 1))
                                if ((flags & b) != 0) buttons = uint16_t(buttons + ButtonSize + ButtonGap);
                        const uint16_t bar = uint16_t(buttons + ButtonGap + 4 * ButtonGap);
                        const uint16_t client = outerWidth(MinClient, flags);
                        return (flags & TitleBar) != 0 && bar > client ? bar : client;
                }

                /** @brief Returns the shortest window. */
                static constexpr uint16_t minHeight(uint8_t flags) { return outerHeight(MinClient, flags); }

                /** @brief Returns true if it has all of @p flags. */
                constexpr bool has(uint8_t flags) const { return (decorations & flags) == flags; }

                /** @brief Returns the client area: inside the border, under the title bar. */
                constexpr Rect client() const {
                        const uint16_t side = sideWidth(decorations), top = topHeight(decorations);
                        return Rect{uint16_t(outer.x + side), uint16_t(outer.y + top), uint16_t(outer.width - 2 * side),
                                    uint16_t(outer.height - top - side)};
                }

                /** @brief Returns the title bar (empty without one). */
                constexpr Rect titleBar() const {
                        return has(TitleBar) ? Rect{outer.x, outer.y, outer.width, TitleHeight} : Rect();
                }

                /** @brief Returns a button (empty if it has none, or @p part isn't a button). */
                constexpr Rect button(Part part) const {
                        const uint16_t y = uint16_t(outer.y + (TitleHeight - ButtonSize) / 2);
                        if (part == Part::Menu)
                                return has(MenuButton) ? Rect{uint16_t(outer.x + ButtonGap), y, ButtonSize, ButtonSize}
                                                       : Rect();
                        // From the right: close, maximize, minimize, each only if there.
                        uint32_t left = outer.right() - ButtonGap;
                        for (const Part p : {Part::Close, Part::Maximize, Part::Minimize}) {
                                const uint8_t flag = p == Part::Close      ? CloseButton
                                                     : p == Part::Maximize ? MaximizeButton
                                                                           : MinimizeButton;
                                if (!has(flag)) continue;
                                left -= ButtonSize;
                                if (p == part) return Rect{uint16_t(left), y, ButtonSize, ButtonSize};
                                left -= ButtonGap;
                        }
                        return Rect();
                }

                /** @brief Returns where the title is written (one font row; may be empty). */
                constexpr Rect title() const {
                        if (!has(TitleBar)) return Rect();
                        const Rect     menu = button(Part::Menu);
                        const uint32_t left = menu.isEmpty() ? outer.x + 2 * ButtonGap : menu.right() + ButtonGap;
                        uint32_t       right = outer.right() - ButtonGap;
                        for (const Part p : {Part::Close, Part::Maximize, Part::Minimize}) {
                                const Rect b = button(p);
                                if (!b.isEmpty()) right = b.x - ButtonGap;
                        }
                        return right > left ? Rect{uint16_t(left), uint16_t(outer.y + 2), uint16_t(right - left),
                                                   uint16_t(TitleHeight - 4)}
                                            : Rect();
                }

                /** @brief Writes the border's rectangles into @p out. @return How many (0 to 4). */
                constexpr size_t edges(Rect (&out)[4]) const {
                        if (!has(Border)) return 0;
                        const uint16_t b = BorderWidth;
                        const uint16_t top = topHeight(decorations);
                        const uint16_t sideH = uint16_t(outer.height - top);
                        size_t         n = 0;
                        if (!has(TitleBar)) out[n++] = Rect{outer.x, outer.y, outer.width, b};
                        out[n++] = Rect{outer.x, uint16_t(outer.y + top), b, sideH};
                        out[n++] = Rect{uint16_t(outer.right() - b), uint16_t(outer.y + top), b, sideH};
                        out[n++] = Rect{uint16_t(outer.x + b), uint16_t(outer.bottom() - b),
                                        uint16_t(outer.width - 2 * b), b};
                        return n;
                }

                /** @brief Returns the part (@p x, @p y) is on: a button, then a handle, then the rest. */
                constexpr Part partAt(uint16_t x, uint16_t y) const {
                        const Rect at{x, y, 1, 1};
                        if (!outer.contains(at)) return Part::Outside;
                        if (titleBar().contains(at))
                                for (const Part p : {Part::Menu, Part::Minimize, Part::Maximize, Part::Close})
                                        if (button(p).contains(at)) return p;
                        if (has(Resizable)) {
                                const uint32_t b = BorderWidth, c = HandleCorner;
                                const bool     onL = x < outer.x + b, onR = x >= outer.right() - b;
                                const bool     onT = y < outer.y + b, onB = y >= outer.bottom() - b;
                                if (onL || onR || onT || onB) {
                                        const bool nearL = x < outer.x + c, nearR = x >= outer.right() - c;
                                        const bool nearT = y < outer.y + c, nearB = y >= outer.bottom() - c;
                                        const bool l = onL || (nearL && (onT || onB));
                                        const bool r = !l && (onR || (nearR && (onT || onB)));
                                        const bool t = onT || (nearT && (onL || onR));
                                        const bool bo = !t && (onB || (nearB && (onL || onR)));
                                        if (t)
                                                return l   ? Part::ResizeTopLeft
                                                       : r ? Part::ResizeTopRight
                                                           : Part::ResizeTop;
                                        if (bo)
                                                return l   ? Part::ResizeBottomLeft
                                                       : r ? Part::ResizeBottomRight
                                                           : Part::ResizeBottom;
                                        return l ? Part::ResizeLeft : Part::ResizeRight;
                                }
                        }
                        if (client().contains(at)) return Part::Client;
                        if (!titleBar().contains(at)) return Part::Edge;
                        return Part::TitleBar;
                }

                /**
                 * @brief Returns where a resize from @p start puts the window: the
                 *        @p edges held moved by (@p dx, @p dy), the others kept;
                 *        the client area in steps of @p widthStep × @p heightStep
                 *        on the axes sized (the width's rounded up to a multiple
                 *        of 4), no smaller
                 *        than @p decorations allow, on @p screen, x and width on
                 *        4-pixel columns (as @p start is).
                 */
                static constexpr Rect resizedRect(const Rect &start, uint8_t edges, int dx, int dy, uint8_t decorations,
                                                  uint16_t widthStep, uint16_t heightStep, const Rect &screen) {
                        int left = start.x, top = start.y, right = int(start.right()), bottom = int(start.bottom());
                        if ((edges & LeftEdge) != 0) left += dx;
                        if ((edges & RightEdge) != 0) right += dx;
                        if ((edges & TopEdge) != 0) top += dy;
                        if ((edges & BottomEdge) != 0) bottom += dy;
                        if (left < screen.x) left = screen.x;
                        if (top < screen.y) top = screen.y;
                        if (right > int(screen.right())) right = int(screen.right());
                        if (bottom > int(screen.bottom())) bottom = int(screen.bottom());
                        // The client area in whole steps, down; then up to the least.
                        int ws = widthStep < 1 ? 1 : widthStep, hs = heightStep < 1 ? 1 : heightStep;
                        while (ws % 4 != 0) ws += widthStep < 1 ? 1 : widthStep;
                        const int extraW = outerWidth(0, decorations), extraH = outerHeight(0, decorations);
                        int       cw = (right - left - extraW) / ws * ws;
                        int       ch = (bottom - top - extraH) / hs * hs;
                        const int minW = minWidth(decorations) - extraW, minH = minHeight(decorations) - extraH;
                        while (cw < minW) cw += ws;
                        while (ch < minH) ch += hs;
                        // An axis not being sized keeps its size, in steps or not.
                        const bool sizingW = (edges & (LeftEdge | RightEdge)) != 0;
                        const bool sizingH = (edges & (TopEdge | BottomEdge)) != 0;
                        const int  width = sizingW ? cw + extraW : start.width;
                        const int  height = sizingH ? ch + extraH : start.height;
                        if ((edges & LeftEdge) != 0) {
                                left = right - width;
                                if (left < screen.x) left = screen.x;
                        }
                        if ((edges & TopEdge) != 0) {
                                top = bottom - height;
                                if (top < screen.y) top = screen.y;
                        }
                        // Not past the screen's far edges either (only a window at its least could be).
                        if (left + width > int(screen.right())) left = int(screen.right()) - width;
                        if (top + height > int(screen.bottom())) top = int(screen.bottom()) - height;
                        return Rect{uint16_t(left), uint16_t(top), uint16_t(width), uint16_t(height)};
                }

                /** @brief Writes @p rect's outline (`OutlineWidth` thick) into @p out. */
                static constexpr void outline(const Rect &rect, Rect (&out)[4]) {
                        const uint16_t t = OutlineWidth;
                        out[0] = Rect{rect.x, rect.y, rect.width, t};
                        out[1] = Rect{rect.x, uint16_t(rect.bottom() - t), rect.width, t};
                        out[2] = Rect{rect.x, uint16_t(rect.y + t), t, uint16_t(rect.height - 2 * t)};
                        out[3] = Rect{uint16_t(rect.right() - t), uint16_t(rect.y + t), t,
                                      uint16_t(rect.height - 2 * t)};
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
 * front, layer `count()` the desktop. Hidden windows (minimized) keep their
 * number and rectangle but are in no layer: `hide()` damages what one
 * showed, `show()` puts it back on top.
 *
 * Each window is on a level (`devplan/gui.md` G4): a window opened, raised
 * or shown goes to the top of its level, under every window of a higher one
 * (the panel stays above the windows, popups above everything, a desktop
 * window under them all).
 *
 * The windows are kept in PSRAM lists that grow as windows open (none are
 * held while the stack is empty), up to `MaxWindows`: numbers stay a byte.
 *
 * @par Thread Safety
 * Not internally synchronized: share by handoff, or under the owner's lock.
 */
class WindowStack {
        public:
                /** @brief Most windows: every number a byte holds (0 is none). */
                static constexpr size_t MaxWindows = 255;

                /** @brief The levels windows are stacked in, lowest first. Append only. */
                enum class Level : uint8_t {
                        Desktop, ///< Under every other window (the desktop's own).
                        Normal,  ///< Applications' windows.
                        Above,   ///< Applications' windows kept above the others ("always on top").
                        Panel,   ///< Above them: panels and docks.
                        Popup,   ///< Above everything: menus.
                        Tooltip, ///< Above even menus, and the pointer passes through: tooltips.
                };

                /** @brief The pixel multiple of window x and widths. */
                static constexpr uint16_t Align = 4;

                /** @brief An empty stack on a screen of @p width × @p height, all damaged. */
                WindowStack(uint16_t width, uint16_t height) { reset(width, height); }

                /** @brief Empties it on a screen of @p width × @p height, all damaged. */
                void reset(uint16_t width, uint16_t height);

                /** @brief Returns the screen. */
                Rect screen() const { return _screen; }

                /** @brief Returns the windows shown (the layers above the desktop). */
                size_t count() const { return _count; }

                /** @brief Returns the windows hidden. */
                size_t hiddenCount() const { return _hidden; }

                /** @brief Returns the number of hidden window @p i (0: the last hidden). */
                uint8_t hiddenIdAt(size_t i) const { return _entries[_count + i].id; }

                /** @brief Returns the number of layer @p z's window (0: the top). */
                uint8_t idAt(size_t z) const { return _entries[z].id; }

                /** @brief Returns layer @p z's rectangle (`count()`: the screen). */
                Rect rectAt(size_t z) const { return z < _count ? _rects[z] : _screen; }

                /** @brief Returns window @p id's layer, or `NotFound` (none, or hidden). */
                Result<size_t> layerOf(uint8_t id) const;

                /** @brief Returns window @p id's rectangle, shown or hidden, or `NotFound`. */
                Result<Rect> rectOf(uint8_t id) const;

                /** @brief Returns window @p id's level (`Normal` if there is none). */
                Level levelOf(uint8_t id) const;

                /** @brief Returns true for the levels of applications' windows (`Normal`, `Above`). */
                static constexpr bool isApplication(Level level) {
                        return level == Level::Normal || level == Level::Above;
                }

                /**
                 * @brief Moves window @p id (shown or hidden) to @p level, on top of
                 *        it; what changes on the screen is damaged.
                 * @return `Ok` or `NotFound`.
                 */
                Error setLevel(uint8_t id, Level level);

                /** @brief Returns true if window @p id is hidden. */
                bool isHidden(uint8_t id) const;

                /** @brief Returns true if @p rect may be a window's: on the screen, aligned. */
                bool isPlaceable(const Rect &rect) const;

                /**
                 * @brief Opens a window at @p rect, on top of @p level.
                 * @return Its number, `Invalid` (not placeable), `Full`
                 *         (`MaxWindows` open) or `NoMem`.
                 */
                Result<uint8_t> open(const Rect &rect, Level level = Level::Normal);

                /** @brief Closes window @p id, shown or hidden. @return `Ok` or `NotFound`. */
                Error close(uint8_t id);

                /**
                 * @brief Moves window @p id to @p rect (any size); a hidden one
                 *        is moved without damage.
                 * @return `Ok`, `NotFound` or `Invalid`.
                 */
                Error move(uint8_t id, const Rect &rect);

                /** @brief Puts shown window @p id on top of its level. @return `Ok` or `NotFound`. */
                Error raise(uint8_t id);

                /** @brief Hides shown window @p id. @return `Ok` or `NotFound`. */
                Error hide(uint8_t id);

                /** @brief Shows hidden window @p id on top of its level. @return `Ok` or `NotFound`. */
                Error show(uint8_t id);

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
                struct Entry {
                                uint8_t id = 0;
                                Level   level = Level::Normal;
                };

                void   addShown(size_t z, const Rect &rect); // what of rect layer z shows, to the damage
                Result<size_t> planAbove(size_t z, const Rect &rect, Span<ComposePiece> out) const; // layers 0 to z
                int            find(uint8_t id) const;                // the entry, shown or hidden, or -1
                size_t         topOf(Level level, size_t skip) const; // the top of a level, entry skip not counted
                void           take(size_t from, size_t to);          // moves an entry, shifting those between

                Rect        _screen;
                List<Rect>  _rects;   // the shown top first, then the hidden
                List<Entry> _entries; // as _rects
                uint8_t     _count = 0;
                uint8_t     _hidden = 0;
                Region      _damage;
};

/**
 * @brief Where a new window of @p width × @p height goes when it asks to be
 *        placed (`devplan/gui.md` §4f): down a diagonal from the top-left of
 *        @p area, a title bar's height (`CascadeStep`) a step, at the first
 *        place no window in @p taken has its top-left corner. A diagonal that
 *        reaches @p area's bottom starts again at the top, two steps to the
 *        right. When every place is taken, or the window is too big for
 *        the steps, the first place (kept inside @p area where it fits).
 *        x on a 4-pixel column.
 */
Rect cascadeRect(uint16_t width, uint16_t height, const Rect &area, Span<const Rect> taken);

/** @brief The step down and right between cascaded windows: a title bar and its border. */
constexpr uint16_t CascadeStep = 24;

BMDS_NAMESPACE_END
