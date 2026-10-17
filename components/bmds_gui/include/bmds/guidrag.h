/**
 * @file      guidrag.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/dragdrop.h>
#include <bmds/guiclipboard.h>
#include <bmds/icon.h>
#include <bmds/list.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

class GuiWindow;
class Widget;

/**
 * @brief A drag to start (`GuiWindow::startDrag()`, `devplan/clipboard.md` §5d):
 *        the types offered on the clipboard's `Drag` selection and what renders
 *        them, the actions allowed, the image, and who is told the end.
 */
struct GuiDrag {
                /** @brief Told what the target did (on the loop's thread): `Move`, the source removes its own. */
                using Ended = void (*)(void *arg, DropAction done);

                Span<const StringView> types;              ///< The best first; copied.
                GuiClipboard::Provider provider = nullptr; ///< Renders a type asked for (on the loop's thread).
                void                  *providerArg = nullptr;
                uint8_t                actions = DropActions::CanCopy;
                DropAction             preferred = DropAction::Copy;
                const uint32_t        *image = nullptr; ///< 0xAARRGGBB (`DragPicture`), copied; null: none.
                uint8_t                width = 0;
                uint8_t                height = 0;
                uint8_t                hotX = 0; ///< The pointer in the image.
                uint8_t                hotY = 0;
                Ended                  ended = nullptr;
                void                  *endedArg = nullptr;
};

/**
 * @brief A drag over a widget, as it is told (`Widget::dragMoved()`,
 *        `Widget::dropped()`, `devplan/clipboard.md` §5d). Positions are the
 *        window's. The widget answers with `accept()` (or `ignore()`); a drop
 *        is read with `read()`, then `finish()`ed once.
 *
 * @par Thread Safety
 * The application's loop.
 */
class GuiDropEvent {
        public:
                using Type = WindowManager::DropEvent::Type;

                GuiDropEvent(GuiWindow &window, const WindowManager::DropEvent &event) : _window(window), _e(event) {}

                Type     type() const { return _e.type; }
                int      x() const { return _e.x; }
                int      y() const { return _e.y; }
                uint8_t  actions() const { return _e.actions; }
                uint8_t  modifiers() const { return _e.modifiers; }
                uint32_t drag() const { return _e.drag; }

                /** @brief Returns true for a right drag's drop: the target asks which action (a menu). */
                bool isAsk() const { return _e.ask; }

                /** @brief Returns true if the drag came from this application. */
                bool isOwn() const { return _e.own; }

                /** @brief Returns the widget that started it, if this application's (else null). */
                Widget *source() const;

                /** @brief Returns true if the drag's data can be read as @p type (offered, or converted). */
                bool hasType(StringView type) const;

                /** @brief The action the keys and @p targetDefault choose (`DropActions::choose()`). */
                DropAction suggested(DropAction targetDefault) const;

                /** @brief Answers: a drop here does @p action (`None`: can't drop here). */
                void accept(DropAction action) { _answer = action; }

                /** @brief Answers that it can't drop here. */
                void ignore() { _answer = DropAction::None; }

                /** @brief Returns the answer given. */
                DropAction answer() const { return _answer; }

                /**
                 * @brief A drop's data as @p type, @p fn told with it later (on the loop's thread).
                 * @return `Ok` (told), or as `GuiClipboard::paste()`.
                 */
                Error read(StringView type, GuiClipboard::Received fn, void *arg) const;

                /** @brief Says what the drop did; the source is told @p done. Once, after `dropped()` took it. */
                void finish(DropAction done) const;

        private:
                GuiWindow               &_window;
                WindowManager::DropEvent _e;
                DropAction               _answer = DropAction::None;
};

/**
 * @brief A drag's image being drawn (`devplan/clipboard.md` §5b D3): a box of
 *        the window (`DragImage::around()`), 0xAARRGGBB, empty at first. Icons,
 *        fills and text go in ghosted (their alpha cut); a count goes in solid.
 *        `crop()` then trims it to what was drawn.
 *
 * @par Thread Safety
 * Not thread-safe.
 */
class DragPicture {
        public:
                /** @brief The ghosted parts' alpha (of 255). */
                static constexpr uint32_t Ghost = 160;

                explicit DragPicture(const DragImage::Box &box);

                /** @brief Returns true if its memory came. */
                bool isValid() const { return !_pixels.isEmpty(); }

                /** @brief Returns its box (window coordinates). */
                const DragImage::Box &box() const { return _box; }

                /** @brief Draws @p icon with its top-left corner at window (@p x, @p y), ghosted. */
                void icon(int x, int y, const Icon &icon);

                /** @brief Fills a rectangle with @p rgb, ghosted. */
                void fill(int x, int y, int w, int h, uint32_t rgb);

                /** @brief Draws @p text's glyphs (unscii-16) in @p rgb, ghosted, cut at @p maxWidth pixels. */
                void text(int x, int y, StringView text, uint32_t rgb, uint16_t maxWidth);

                /** @brief Draws @p count in a solid badge with its top-right corner at (@p right, @p top). */
                void badge(int right, int top, size_t count);

                /**
                 * @brief Trims it to what was drawn, with the pointer at window (@p x, @p y)
                 *        kept inside.
                 * @return False if nothing was drawn.
                 */
                bool crop(int x, int y);

                const uint32_t *pixels() const { return _pixels.data(); }
                uint8_t         width() const { return uint8_t(_box.width); }
                uint8_t         height() const { return uint8_t(_box.height); }
                uint8_t         hotX() const { return _hotX; }
                uint8_t         hotY() const { return _hotY; }

        private:
                void put(int x, int y, uint32_t argb); // window coordinates, over what is there

                DragImage::Box _box;
                List<uint32_t> _pixels{MemSpace::Psram};
                uint8_t        _hotX = 0;
                uint8_t        _hotY = 0;
};

BMDS_NAMESPACE_END
