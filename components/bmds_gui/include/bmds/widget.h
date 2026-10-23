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
#include <bmds/guidrag.h>
#include <bmds/icon.h>
#include <bmds/inputevent.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/mnemonic.h>
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

        /** @brief Which way `drawArrow()` points. */
        enum class Arrow : uint8_t {
                Up,
                Down,
                Left,
                Right,
        };

        /**
         * @brief Draws a small arrow (a triangle 7 pixels across, 4 deep) in
         *        @p rect's middle, as scroll bars and sorted headers show.
         */
        void drawArrow(Painter &painter, const Rect &rect, Arrow way, uint32_t color);

        /** @brief A clipped painter's pieces kept on the stack (`drawClipped()`). */
        constexpr size_t LocalPieces = 64;

        /**
         * @brief Calls @p draw(painter) with a painter clipped to @p limit (window
         *        coordinates): its storage on the stack for up to `LocalPieces`
         *        pieces, in PSRAM past that (not drawn without it). Nothing is
         *        drawn if none of @p limit shows.
         */
        template <typename Draw> void drawClipped(Painter &painter, const Rect &limit, Draw draw) {
                if (limit.isEmpty() || !painter.isVisible(limit)) return;
                const size_t need = painter.clippedStorage();
                Rect         local[LocalPieces];
                List<Rect>   more(MemSpace::Psram);
                Span<Rect>   storage(local, LocalPieces);
                if (need > LocalPieces) {
                        if (more.reserve(need).isError()) return;
                        for (size_t i = 0; i < need; ++i) (void)more.append(Rect());
                        storage = more.span();
                }
                Painter clipped(painter, limit, storage);
                draw(clipped);
                return;
        }
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

                /**
                 * @brief Returns the height it wants at @p width (wrapped text grows
                 *        as it narrows): its hint's by default. A `ScrollView` asks
                 *        its rows this.
                 */
                virtual uint16_t heightForWidth(uint16_t width) const {
                        (void)width;
                        return sizeHint().height;
                }

                /** @brief Returns its stretch factor in a layout (0: its hint). */
                uint8_t stretch() const { return _stretch; }

                /** @brief Sets its stretch factor in a layout. */
                void setStretch(uint8_t stretch) { _stretch = stretch; }

                /** @brief Returns the widest a layout makes it. */
                uint16_t maximumWidth() const { return _maxWidth; }

                /** @brief Sets the widest a layout makes it. */
                void setMaximumWidth(uint16_t width) { _maxWidth = width; }

                /**
                 * @brief Returns the deepest visible child at (@p x, @p y) (window
                 *        coordinates), or itself; a child is found only inside its
                 *        clip (`childClip()`).
                 */
                Widget *widgetAt(int x, int y);

                /**
                 * @brief Returns true, and in @p clip (window coordinates) where
                 *        @p child and what is in it are drawn and take the mouse, if
                 *        @p child is kept inside a rectangle (a `ScrollView`'s rows, in
                 *        its viewport); false (the default): drawn whole.
                 */
                virtual bool childClip(const Widget &child, Rect &clip) const {
                        (void)child;
                        (void)clip;
                        return false;
                }

                /** @brief Draws it (and the window draws its children after). */
                virtual void paint(Painter &painter) { (void)painter; }

                /** @brief Makes it take drags (`dragMoved()`, `dropped()`), or not (the default). */
                void setAcceptsDrops(bool on) { _acceptsDrops = on; }

                /** @brief Returns true if it takes drags. */
                bool acceptsDrops() const { return _acceptsDrops; }

                /**
                 * @brief Asked first about each drag over it and each drop on it, whatever the
                 *        widget's class (the GUI service's `dropTypes`, `devplan/guiprograms.md`
                 *        §11 3e). It answers a drag (`accept()`, `ignore()`) or takes a drop (it
                 *        finishes it), and returns true; false leaves the drag to the widget's own
                 *        `dragMoved()` and `dropped()`.
                 */
                using DropHook = bool (*)(void *arg, Widget &widget, GuiDropEvent &event);

                /** @brief Sets its drop hook (null: none); with one, it takes drags. */
                void setDropHook(DropHook hook, void *arg) {
                        _dropHook = hook;
                        _dropHookArg = arg;
                        return;
                }

                /** @brief Returns true if it takes drags, by its class or its hook. */
                bool takesDrags() const { return _acceptsDrops || _dropHook != nullptr; }

                /** @brief Asks its hook about @p event. @return True if the hook answered it. */
                bool askDropHook(GuiDropEvent &event) {
                        return _dropHook != nullptr && _dropHook(_dropHookArg, *this, event);
                }

                /** @brief What an input hook is asked about (`setInputHook()`). */
                struct InputHookEvent {
                                enum class Type : uint8_t {
                                        Context, ///< A right press: a context menu asked for.
                                        Wheel,   ///< The wheel turned over it (`notches`, away positive).
                                };
                                Type type = Type::Context;
                                int  x = 0; ///< `Context`: in its window's client area.
                                int  y = 0;
                                int  notches = 0;
                };

                /**
                 * @brief Asked about a right press or the wheel over the widget (or over one of its
                 *        children that doesn't take it), before the widget's own handling and
                 *        whatever its class (`devplan/guiprograms.md` §12 4d).
                 * @return True if taken: the widget isn't told.
                 */
                using InputHook = bool (*)(void *arg, Widget &widget, const InputHookEvent &event);

                /** @brief Sets its input hook (null: none). */
                void setInputHook(InputHook hook, void *arg) {
                        _inputHook = hook;
                        _inputHookArg = arg;
                        return;
                }

                /** @brief Asks its hook about @p event. @return True if taken. */
                bool askInputHook(const InputHookEvent &event) {
                        return _inputHook != nullptr && _inputHook(_inputHookArg, *this, event);
                }

                /**
                 * @brief A drag came over it (`devplan/clipboard.md` §5d): answer with
                 *        `accept()` or `ignore()`. By default as `dragMoved()`.
                 */
                virtual void dragEntered(GuiDropEvent &event) { dragMoved(event); }

                /** @brief A drag moved over it (or a key changed): answer again. Ignored by default. */
                virtual void dragMoved(GuiDropEvent &event) { event.ignore(); }

                /** @brief The drag left it (or ended). */
                virtual void dragLeft() {}

                /**
                 * @brief A drag was let go over it: read the data (`GuiDropEvent::read()`),
                 *        then `finish()`. @return True if it took the drop (it will finish
                 *        it), false to refuse (finished as `None`).
                 */
                virtual bool dropped(GuiDropEvent &event) {
                        (void)event;
                        return false;
                }

                /** @brief A button went down over it. */
                virtual void mousePressed(const WindowManager::MouseEvent &event) { (void)event; }

                /** @brief The button pressed over it came up (anywhere). */
                virtual void mouseReleased(const WindowManager::MouseEvent &event) { (void)event; }

                /** @brief The pointer moved over it (or anywhere while it was pressed). */
                virtual void mouseMoved(const WindowManager::MouseEvent &event) { (void)event; }

                /** @brief The pointer left it (or a popup took the press it held). */
                virtual void mouseLeft() {}

                /**
                 * @brief The wheel turned @p notches (up: positive) with the pointer
                 *        over it; offered to its parents if it returns false.
                 * @return True if used.
                 */
                virtual bool wheelTurned(int notches) {
                        (void)notches;
                        return false;
                }

                /**
                 * @brief Returns the pointer's shape over (@p x, @p y) (window
                 *        coordinates) of it: its parent's by default (the window's
                 *        own, from its config).
                 */
                virtual WindowManager::Pointer pointerAt(int x, int y) const {
                        return _parent != nullptr ? _parent->pointerAt(x, y) : WindowManager::Pointer::Arrow;
                }

                /** @brief A key typed while it has the window's focus. @return True if used. */
                virtual bool keyPressed(const KeyEvent &event) {
                        (void)event;
                        return false;
                }

                /**
                 * @brief A key typed while a widget inside it has the focus, offered
                 *        before that one gets it, innermost first (a tab widget's
                 *        Ctrl+Tab, which a list would take as its own).
                 * @return True if used.
                 */
                virtual bool keyBeforeChild(const KeyEvent &event) {
                        (void)event;
                        return false;
                }

                /** @brief Returns true if Tab stops on it, and a press gives it the focus. */
                bool isFocusable() const { return _focusable; }

                /** @brief Makes Tab stop on it, or not (`devplan/gui.md` §4r; buttons and line edits do). */
                void setFocusable(bool focusable) { _focusable = focusable; }

                /** @brief Returns true if it is its window's focus widget. */
                bool hasFocus() const;

                /** @brief Makes it its window's focus widget. */
                void setFocus();

                /** @brief It became its window's focus widget. */
                virtual void focusIn() {}

                /**
                 * @brief It stopped being its window's focus widget (another has it,
                 *        or none): a field commits what was typed.
                 */
                virtual void focusOut() {}

                /**
                 * @brief @p widget, inside it, became the focus widget: told to each
                 *        of its parents, innermost first (a `ScrollView` shows it).
                 */
                virtual void childFocused(Widget &widget) { (void)widget; }

                /**
                 * @brief Returns its tooltip (`devplan/gui.md` §4s): shown when the
                 *        pointer rests on it; empty (the default) for none. Tell
                 *        `GuiApplication::toolTipChanged()` when it changes.
                 */
                virtual StringView toolTip() const { return StringView(); }

                /** @brief Returns its mnemonic's key (`Mnemonic::keyOf()`'s form), or 0: none. */
                virtual uint32_t mnemonic() const { return 0; }

                /**
                 * @brief Its mnemonic was typed and no other widget of its window has it:
                 *        it acts (a button clicks). It takes the focus by default.
                 */
                virtual void mnemonicActivated() { setFocus(); }

        protected:
                /** @brief Its size changed (lay children out here). */
                virtual void resized() {}

                /** @brief Its window was set (`GuiWindow`'s, for the root). */
                void setWindow(GuiWindow *window) { _window = window; }

                /**
                 * @brief Fills what of @p area its visible children don't cover with
                 *        @p rgb: a container's gaps, without drawing under them.
                 */
                void fillOutsideChildren(Painter &painter, const Rect &area, uint32_t rgb) const;

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
                bool       _focusable = false;
                bool       _acceptsDrops = false;
                DropHook   _dropHook = nullptr;
                void      *_dropHookArg = nullptr;
                InputHook  _inputHook = nullptr;
                void      *_inputHookArg = nullptr;
};

/**
 * @brief Text in one line, in the face's colors. With a buddy, a `&` in it
 *        marks a key that gives the buddy the focus (`Mnemonic`), as Qt's.
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

                /** @brief Sets the widget its mnemonic focuses (null: none, and `&` is shown as it is). */
                void setBuddy(Widget *buddy);

                /** @brief Makes it at least @p width wide in a layout (labels before a column of fields). */
                void setMinimumWidth(uint16_t width) { _minWidth = width; }

                /** @brief Draws its text on the highlight's color, or not (a setting a search found). */
                void setHighlighted(bool highlighted);

                GuiSize  sizeHint() const override;
                void     paint(Painter &painter) override;
                uint32_t mnemonic() const override;
                void     mnemonicActivated() override;

        private:
                Mnemonic::Stripped shown(char (&out)[MaxText + 1]) const; // the text as drawn

                char         _text[MaxText + 1] = {};
                Widget      *_buddy = nullptr;
                uint16_t     _minWidth = 0;
                Align        _align = Align::Left;
                Bevel::Style _frame = Bevel::Style::Raised;
                bool         _framed = false;
                bool         _highlighted = false;
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
 * A `&` in its text marks its key (`Mnemonic`, `devplan/gui.md` §4r): "&OK"
 * shows "OK", the O underlined, and Alt+O clicks it; `&&` is a `&`. It takes
 * the focus (Tab stops on it), shows it with a rectangle while its window is
 * active, and Space or Enter clicks it then; the Menu key and Shift+F10 are a
 * right press. Its window's default button (`GuiWindow::setDefaultButton()`)
 * has a dark outline.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class Button : public Widget {
        public:
                /** @brief Told of a click (on the loop's thread). */
                using Clicked = void (*)(void *arg, Button &button);

                /**
                 * @brief Told of a press (any button, or 0: Space or Enter, without a
                 *        `Clicked`) where it is (screen coordinates).
                 */
                using Pressed = void (*)(void *arg, Button &button, uint8_t which, uint16_t screenX, uint16_t screenY);

                static constexpr size_t MaxText = 63;

                Button(Widget *parent, StringView text = StringView());

                /** @brief Sets the text (`&` marking its key; cut at `MaxText` bytes on a character) and redraws it. */
                void setText(StringView text);

                /** @brief Returns the text shown (its `&` marks taken out). */
                StringView text() const { return StringView(_text); }

                /**
                 * @brief Clicks it as the left button would (told to `Clicked`; without
                 *        one, a `Pressed` with button 0).
                 */
                void click();

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
                void     mouseLeft() override;
                bool     keyPressed(const KeyEvent &event) override;
                uint32_t mnemonic() const override { return _key; }
                void     mnemonicActivated() override;

        private:
                bool inside(int x, int y) const;
                void press(uint8_t which); // told to `Pressed` at its bottom-left corner

                char        _text[MaxText + 1] = {};
                size_t      _underline = Mnemonic::None;
                uint32_t    _key = 0;
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
 * @brief A check box: a sunken white box, ticked or not, and text after it
 *        (`devplan/filebrowser.md` §4j).
 *
 * A `&` in its text marks its key, as a button's. A click, Space or its key
 * (Alt and the letter) toggles it, told to `Toggled`. It takes the focus,
 * shown by a ring around its text while its window is active.
 *
 * @par Thread Safety
 * As `Widget`.
 */
class CheckBox : public Widget {
        public:
                /** @brief Told it was toggled by the user (on the loop's thread). */
                using Toggled = void (*)(void *arg, CheckBox &box);

                static constexpr size_t   MaxText = 63;
                static constexpr uint16_t BoxSide = 13; ///< The box, its bevel included.

                CheckBox(Widget *parent, StringView text = StringView());

                /** @brief Sets the text (`&` marking its key; cut at `MaxText` bytes on a character). */
                void setText(StringView text);

                /** @brief Returns the text shown (its `&` marks taken out). */
                StringView text() const { return StringView(_text); }

                /** @brief Ticks it or not (not told to `Toggled`). */
                void setChecked(bool checked);

                /** @brief Returns true if ticked. */
                bool isChecked() const { return _checked; }

                /** @brief Draws its text on the highlight's color, or not (a setting a search found). */
                void setHighlighted(bool highlighted);

                /** @brief Has @p fn told when the user toggles it. */
                void setToggled(Toggled fn, void *arg) {
                        _toggled = fn;
                        _toggledArg = arg;
                        return;
                }

                GuiSize  sizeHint() const override;
                void     paint(Painter &painter) override;
                void     mousePressed(const WindowManager::MouseEvent &event) override;
                void     mouseReleased(const WindowManager::MouseEvent &event) override;
                bool     keyPressed(const KeyEvent &event) override;
                uint32_t mnemonic() const override { return _key; }
                void     mnemonicActivated() override;

        private:
                void toggle();

                char     _text[MaxText + 1] = {};
                size_t   _underline = Mnemonic::None;
                uint32_t _key = 0;
                Toggled  _toggled = nullptr;
                void    *_toggledArg = nullptr;
                bool     _checked = false;
                bool     _down = false; // the left button pressed on it
                bool     _highlighted = false;
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

                /** @brief Lays its children out in @p direction from now on. */
                void setDirection(Direction direction);

                /** @brief Returns the direction it lays its children out in. */
                Direction direction() const { return _direction; }

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
