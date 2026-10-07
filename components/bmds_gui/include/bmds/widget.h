/**
 * @file      widget.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>
#include <bmds/bevel.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/icon.h>
#include <bmds/inputevent.h>
#include <bmds/memory.h>
#include <bmds/painter.h>
#include <bmds/rect.h>
#include <bmds/stringview.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

class GuiWindow;

/** @brief The toolkit's sizes (`devplan/gui.md` §4b, §4e). */
namespace GuiStyle {
        constexpr uint16_t TextHeight = 16;   ///< unscii-16's.
        constexpr uint16_t ButtonPad = 8;     ///< A button's text from its left edge.
        constexpr uint16_t ButtonHeight = 22; ///< A button's height (a panel's row).
        constexpr uint16_t Column = 4;        ///< Text starts on a multiple of this.
        constexpr uint16_t IconSize = 16;     ///< A button's or a menu item's icon.
        constexpr uint16_t IconGap = 4;       ///< Between an icon and its text.
} // namespace GuiStyle

/** @brief A width and a height. */
struct GuiSize {
                uint16_t width = 0;
                uint16_t height = 0;
};

/**
 * @brief Something drawn in a window that may take the mouse and keys
 *        (`devplan/gui.md` §4b).
 *
 * A widget has a rectangle in its window's client area (window coordinates),
 * a parent, and children in the order they were made. A widget made with
 * `create()` belongs to its parent, which destroys it (Qt's model); the
 * rest are the caller's. `update()` asks for its rectangle to be redrawn;
 * `paint()` draws it (on the window manager's task, the application's lock
 * held), and must keep inside its rectangle.
 *
 * @par Thread Safety
 * The application's lock (handlers and drawing hold it).
 */
class Widget {
        public:
                /** @brief Makes a widget in @p parent (null: none; a window). */
                explicit Widget(Widget *parent);
                virtual ~Widget();
                Widget(const Widget &) = delete;
                Widget &operator=(const Widget &) = delete;

                /**
                 * @brief Makes a @p T in PSRAM, owned by @p parent (destroyed with it,
                 *        or by `destroy()`).
                 * @return It, or null (no memory).
                 */
                template <typename T, typename... Args> static T *create(Widget *parent, Args &&...args) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(T));
                        if (mem == nullptr) return nullptr;
                        T *widget = ::new (mem) T(parent, std::forward<Args>(args)...);
                        widget->_owned = true;
                        return widget;
                }

                /** @brief Destroys a widget made by `create()` (its parent's update follows). */
                void destroy();

                /** @brief Returns the window it is in (null: none yet). */
                GuiWindow *window() const;

                /** @brief Returns its parent (null: none). */
                Widget *parent() const { return _parent; }

                /** @brief Returns its first child (then `next()`), or null. */
                Widget *firstChild() const { return _first; }

                /** @brief Returns its next sibling, or null. */
                Widget *next() const { return _next; }

                /** @brief Returns its rectangle in the window. */
                const Rect &geometry() const { return _rect; }

                /** @brief Moves and sizes it; `resized()` follows a change of size. */
                void setGeometry(const Rect &rect);

                /** @brief Returns true unless hidden. */
                bool isVisible() const { return _visible; }

                /** @brief Shows or hides it (a layout then leaves it out). */
                void setVisible(bool visible);

                /** @brief Returns true unless disabled. */
                bool isEnabled() const { return _enabled; }

                /** @brief Enables or disables it (a disabled one takes no input). */
                void setEnabled(bool enabled);

                /** @brief Asks for its rectangle to be redrawn. */
                void update();

                /** @brief Returns the size it wants (a layout's hint). */
                virtual GuiSize sizeHint() const { return GuiSize(); }

                /** @brief Returns its stretch factor in a layout (0: its hint). */
                uint8_t stretch() const { return _stretch; }

                /** @brief Sets its stretch factor in a layout. */
                void setStretch(uint8_t stretch) { _stretch = stretch; }

                /** @brief Returns the widest a layout makes it. */
                uint16_t maximumWidth() const { return _maxWidth; }

                /** @brief Sets the widest a layout makes it. */
                void setMaximumWidth(uint16_t width) { _maxWidth = width; }

                /** @brief Returns the deepest visible child at (@p x, @p y) (window coordinates), or itself. */
                Widget *widgetAt(int x, int y);

                /** @brief Draws it (and the window draws its children after). */
                virtual void paint(Painter &painter) { (void)painter; }

                /** @brief A button went down over it. */
                virtual void mousePressed(const WindowManager::MouseEvent &event) { (void)event; }

                /** @brief The button pressed over it came up (anywhere). */
                virtual void mouseReleased(const WindowManager::MouseEvent &event) { (void)event; }

                /** @brief The pointer moved over it (or anywhere while it was pressed). */
                virtual void mouseMoved(const WindowManager::MouseEvent &event) { (void)event; }

                /** @brief The pointer left it (or a popup took the press it held). */
                virtual void mouseLeft() {}

                /** @brief A key typed while it has the window's focus. @return True if used. */
                virtual bool keyPressed(const KeyEvent &event) {
                        (void)event;
                        return false;
                }

        protected:
                /** @brief Its size changed (lay children out here). */
                virtual void resized() {}

                /** @brief Its window was set (`GuiWindow`'s, for the root). */
                void setWindow(GuiWindow *window) { _window = window; }

        private:
                void add(Widget *child);
                void remove(Widget *child);

                Widget    *_parent = nullptr;
                Widget    *_first = nullptr;
                Widget    *_last = nullptr;
                Widget    *_next = nullptr;
                GuiWindow *_window = nullptr; // the root's; the others ask their parent
                Rect       _rect;
                uint16_t   _maxWidth = UINT16_MAX;
                uint8_t    _stretch = 0;
                bool       _visible = true;
                bool       _enabled = true;
                bool       _owned = false;
};

/**
 * @brief Text in one line, in the face's colors.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class Label : public Widget {
        public:
                /** @brief Where the text goes. */
                enum class Align : uint8_t {
                        Left,
                        Center,
                        Right
                };

                static constexpr size_t MaxText = 63;

                Label(Widget *parent, StringView text = StringView());

                /** @brief Sets the text (cut at `MaxText` bytes on a character) and redraws it. */
                void setText(StringView text);

                /** @brief Returns the text. */
                StringView text() const { return StringView(_text); }

                /** @brief Sets where the text goes. */
                void setAlign(Align align);

                /** @brief Draws a bevel around it (a status field: `Bevel::Style::Status`); none by default. */
                void setFrame(Bevel::Style style);

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;

        private:
                char         _text[MaxText + 1] = {};
                Align        _align = Align::Left;
                Bevel::Style _frame = Bevel::Style::Raised;
                bool         _framed = false;
};

/**
 * @brief A button: text (an icon later), clicked by the left button released
 *        over it; optionally checkable, and told of a right click.
 *
 * A raised button stands out of the face (`Bevel::Style::Raised`) and sinks
 * while pressed with the pointer on it, or while checked (its face lighter,
 * as the classic taskbars showed the active window). A flat
 * one (a toolbar's) shows a thin edge only under the pointer.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class Button : public Widget {
        public:
                /** @brief Told of a click (on the loop's thread). */
                using Clicked = void (*)(void *arg, Button &button);

                /** @brief Told of a press (any button) where it is (screen coordinates). */
                using Pressed = void (*)(void *arg, Button &button, uint8_t which, uint16_t screenX, uint16_t screenY);

                static constexpr size_t MaxText = 63;

                Button(Widget *parent, StringView text = StringView());

                /** @brief Sets the text (cut at `MaxText` bytes on a character) and redraws it. */
                void setText(StringView text);

                /** @brief Returns the text. */
                StringView text() const { return StringView(_text); }

                /**
                 * @brief Shows @p icon before the text (null: none), kept by the
                 *        caller; 16 × 16 (a bigger one isn't drawn).
                 */
                void setIcon(const Icon *icon);

                /** @brief How it stands. */
                enum class Look : uint8_t {
                        Raised,
                        Flat,
                };

                /** @brief Sets how it stands (raised by default). */
                void setLook(Look look);

                /** @brief Centres its text (a dialog's button; without an icon), else from its left. */
                void setCentered(bool centered);

                /** @brief Draws it checked (pressed in) or not. */
                void setChecked(bool checked);

                /** @brief Returns true if drawn checked. */
                bool isChecked() const { return _checked; }

                /** @brief Has @p fn told of clicks. */
                void setClicked(Clicked fn, void *arg) {
                        _clicked = fn;
                        _clickedArg = arg;
                        return;
                }

                /** @brief Has @p fn told of presses at once (a menu opens on the press). */
                void setPressed(Pressed fn, void *arg) {
                        _pressed = fn;
                        _pressedArg = arg;
                        return;
                }

                /** @brief Returns a value of the owner's. */
                uintptr_t data() const { return _data; }

                /** @brief Sets a value of the owner's (a window's number). */
                void setData(uintptr_t data) { _data = data; }

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;
                void    mousePressed(const WindowManager::MouseEvent &event) override;
                void    mouseReleased(const WindowManager::MouseEvent &event) override;
                void    mouseMoved(const WindowManager::MouseEvent &event) override;
                void    mouseLeft() override;

        private:
                bool inside(int x, int y) const;

                char        _text[MaxText + 1] = {};
                const Icon *_icon = nullptr;
                Clicked     _clicked = nullptr;
                void       *_clickedArg = nullptr;
                Pressed     _pressed = nullptr;
                void       *_pressedArg = nullptr;
                uintptr_t   _data = 0;
                Look        _look = Look::Raised;
                bool        _checked = false;
                bool        _centered = false;
                bool        _down = false; // the left button pressed over it
                bool        _over = false; // the pointer on it
};

/**
 * @brief Lays its children out in a row or a column (`LineLayout`): each its
 *        hint, stretching ones sharing the rest; the gaps in the face's color.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class BoxLayout : public Widget {
        public:
                enum class Direction : uint8_t {
                        Horizontal,
                        Vertical
                };

                static constexpr size_t MaxItems = 32;

                BoxLayout(Widget *parent, Direction direction);

                /** @brief Sets the gap between children (a multiple of 4 across). */
                void setSpacing(uint16_t spacing);

                /**
                 * @brief Sets the room kept inside its edges (left and right
                 *        multiples of 4 in a row), where a frame is drawn.
                 */
                void setMargins(uint16_t left, uint16_t top, uint16_t right, uint16_t bottom);

                /** @brief Draws a bevel around it, in its margins (a panel: `Bevel::Style::Window`). */
                void setFrame(Bevel::Style style);

                /** @brief Lays the children out again (after adding or hiding some). */
                void relayout();

                GuiSize sizeHint() const override;
                void    paint(Painter &painter) override;

        protected:
                void resized() override;

        private:
                Direction    _direction;
                uint16_t     _spacing = 0;
                uint16_t     _margins[4] = {}; // left, top, right, bottom
                Bevel::Style _frame = Bevel::Style::Raised;
                bool         _framed = false;
};

BMDS_NAMESPACE_END
