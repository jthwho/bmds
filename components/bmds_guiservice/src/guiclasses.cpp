/**
 * @file      guiclasses.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The GUI service's classes (devplan/guiprograms.md §4b, §10). Their names, types and
 * signatures are sdk/gui/classes.def's, the class ABI's one source; here each entry is bound to
 * the function named after it (set_Label_text, call_Window_show, create_Button), so a missing
 * one doesn't compile.
 */

#include <cstring>
#include <new>
#include <bmds/lineedit.h>
#include <bmds/memory.h>
#include <bmds/windowstack.h>
#include "guiatoms.h"
#include "guiconnection.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using GuiProtocol::Tag;
        using GuiProtocol::Value;
        using Args = Span<const Value>;

        constexpr GuiStatus Ok = 0;
        constexpr GuiStatus BadValue = GuiStatus(GuiProtocol::ErrorCode::BadValue);
        constexpr GuiStatus BadMatch = GuiStatus(GuiProtocol::ErrorCode::BadMatch);
        constexpr GuiStatus BadAccess = GuiStatus(GuiProtocol::ErrorCode::BadAccess);
        constexpr GuiStatus NoMem = GuiStatus(GuiProtocol::ErrorCode::NoMem);

        constexpr uint16_t WindowMargin = 8;
        constexpr uint16_t Spacing = 4;
        constexpr uint16_t DefaultWidth = 320; // a window with nothing in it yet
        constexpr uint16_t DefaultHeight = 200;

        StringView atomName(const Value &v) {
                return v.tag == Tag::Atom ? GuiAtoms::name(uint32_t(v.number)) : StringView();
        }

        // A widget made under its parent's container (a window's column, a box): none, BadMatch.
        template <typename T, typename... A> GuiStatus makeWidget(GuiObject &o, GuiObject *parent, A &&...args) {
                Widget *in = parent != nullptr ? parent->container : nullptr;
                if (in == nullptr) return BadMatch;
                T *w = Widget::create<T>(in, static_cast<A &&>(args)...);
                if (w == nullptr) return NoMem;
                o.widget = w;
                o.owner = parent->owner;
                o.connection->touched(o.owner);
                return Ok;
        }

        void emitNone(GuiObject &o, const char *event) {
                o.connection->emit(o, event, Args());
                return;
        }

        // ---- Widget

        GuiStatus set_Widget_enabled(GuiObject &o, const Value &v) {
                o.widget->setEnabled(v.number != 0);
                return Ok;
        }
        Value get_Widget_enabled(GuiObject &o) {
                return Value::boolean(o.widget->isEnabled());
        }
        GuiStatus set_Widget_visible(GuiObject &o, const Value &v) {
                o.widget->setVisible(v.number != 0);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Widget_visible(GuiObject &o) {
                return Value::boolean(o.widget->isVisible());
        }
        GuiStatus call_Widget_setFocus(GuiObject &o, Args, Value &) {
                o.widget->setFocus();
                return Ok;
        }

        // ---- Window

        GuiStatus set_Window_title(GuiObject &o, const Value &v) {
                o.window->setTitle(v.text);
                return Ok;
        }
        Value get_Window_title(GuiObject &o) {
                return Value::string(o.window->title());
        }
        GuiStatus set_Window_width(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 1280) return BadValue;
                o.width = uint16_t(v.number);
                return Ok;
        }
        Value get_Window_width(GuiObject &o) {
                return Value::integer(o.window->isShown() ? int32_t(o.window->screenRect().width) : o.width);
        }
        GuiStatus set_Window_height(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 720) return BadValue;
                o.height = uint16_t(v.number);
                return Ok;
        }
        Value get_Window_height(GuiObject &o) {
                return Value::integer(o.window->isShown() ? int32_t(o.window->screenRect().height) : o.height);
        }

        // Shown where its placement says, or at its size (or its children's) in the middle of its
        // owner or of the work area.
        GuiStatus call_Window_show(GuiObject &o, Args, Value &) {
                ClientWindow &win = *o.window;
                if (win.isShown()) {
                        win.activate();
                        return Ok;
                }
                Error err;
                if (win.placementWanted.normal.width != 0) {
                        err = win.show(win.placementWanted);
                } else {
                        win.column()->relayout();
                        const GuiSize hint = win.column()->sizeHint();
                        uint16_t      w = o.width != 0 ? o.width : (hint.width != 0 ? hint.width : DefaultWidth);
                        uint16_t      h = o.height != 0 ? o.height : (hint.height != 0 ? hint.height : DefaultHeight);
                        if (w < o.minWidth) w = o.minWidth;
                        if (h < o.minHeight) h = o.minHeight;
                        WindowManager &wm = o.connection->application().windowManager();
                        const Rect     work = wm.workArea();
                        Rect           around = work;
                        GuiObject     *owner = win.ownerId != 0 ? o.connection->find(win.ownerId) : nullptr;
                        if (owner != nullptr && owner->window != nullptr && owner->window->isShown()) {
                                auto [r, rerr] = wm.rect(owner->window->id());
                                if (rerr.isOk()) around = r;
                        }
                        const uint8_t frame = win.decorations();
                        uint16_t      ow = WindowFrame::outerWidth(w, frame), oh = WindowFrame::outerHeight(h, frame);
                        if (ow > work.width) ow = work.width;
                        if (oh > work.height) oh = work.height;
                        int x = around.x + (int(around.width) - ow) / 2, y = around.y + (int(around.height) - oh) / 2;
                        if (x + ow > work.x + work.width) x = work.x + work.width - ow;
                        if (y + oh > work.y + work.height) y = work.y + work.height - oh;
                        if (x < work.x) x = work.x;
                        if (y < work.y) y = work.y;
                        err = win.show(Rect{uint16_t(x & ~3), uint16_t(y), ow, oh});
                }
                if (err.isError()) return NoMem;
                const uint8_t  id = win.id();
                WindowManager &wm = o.connection->application().windowManager();
                if (win.above) (void)wm.setAbove(id, true);
                if (!win.menuItem().isEmpty()) win.setMenuItem(win.menuItem());
                win.updateBlock();
                o.connection->touched(&win);
                return Ok;
        }

        GuiStatus call_Window_close(GuiObject &o, Args, Value &) {
                o.window->close();
                o.window->updateBlock();
                return Ok;
        }

        // ---- Window (2b)

        WindowManager &wmOf(GuiObject &o) {
                return o.connection->application().windowManager();
        }

        GuiStatus set_Window_minWidth(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 1280) return BadValue;
                o.window->setMinimumSize(uint16_t(v.number), o.minHeight);
                o.minWidth = uint16_t(v.number);
                return Ok;
        }
        Value get_Window_minWidth(GuiObject &o) {
                return Value::integer(o.minWidth);
        }
        GuiStatus set_Window_minHeight(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 720) return BadValue;
                o.window->setMinimumSize(o.minWidth, uint16_t(v.number));
                o.minHeight = uint16_t(v.number);
                return Ok;
        }
        Value get_Window_minHeight(GuiObject &o) {
                return Value::integer(o.minHeight);
        }

        // `X Y W H`, then `maximized` (WindowPlacement's text): before it is shown, where it opens;
        // shown, it moves there.
        GuiStatus set_Window_placement(GuiObject &o, const Value &v) {
                WindowPlacement p;
                if (!WindowPlacement::parse(v.text, p)) return BadValue;
                ClientWindow &win = *o.window;
                win.placementWanted = p;
                if (!win.isShown()) return Ok;
                WindowManager &wm = wmOf(o);
                const uint8_t  id = win.id();
                (void)wm.restore(id);
                (void)wm.resize(id, p.normal.width, p.normal.height);
                (void)wm.move(id, p.normal.x, p.normal.y);
                if (p.maximized) (void)wm.maximize(id);
                return Ok;
        }
        Value get_Window_placement(GuiObject &o) {
                const ClientWindow   &win = *o.window;
                const WindowPlacement p =
                        win.isShown() || win.lastRect().width != 0 ? win.placement() : win.placementWanted;
                o.text = p.normal.width != 0 ? p.format() : String(MemSpace::Psram);
                return Value::string(o.text.view());
        }

        GuiStatus set_Window_icon(GuiObject &o, const Value &v) {
                return o.window->setIconNamed(v.text) ? Ok : BadValue;
        }
        Value get_Window_icon(GuiObject &o) {
                return Value::string(o.window->iconName());
        }

        // The pointer's names, in WindowManager::Pointer's order.
        constexpr const char *PointerNames[] = {"arrow",      "horizontal", "vertical", "diagonalDown",
                                                "diagonalUp", "text",       "none",     "starting"};

        GuiStatus set_Window_pointer(GuiObject &o, const Value &v) {
                const StringView name = atomName(v);
                for (size_t i = 0; i < sizeof(PointerNames) / sizeof(PointerNames[0]); ++i) {
                        if (name != PointerNames[i]) continue;
                        o.window->setPointer(WindowManager::Pointer(i));
                        return Ok;
                }
                return BadValue;
        }
        Value get_Window_pointer(GuiObject &o) {
                const size_t i = size_t(o.window->pointer());
                return Value::atom(GuiAtoms::intern(i < sizeof(PointerNames) / sizeof(PointerNames[0]) ? PointerNames[i]
                                                                                                       : "arrow"));
        }

        // The frame's parts as booleans: the buttons need the title bar, resizing the border.
        GuiStatus setFrame(GuiObject &o, uint8_t flags, bool on) {
                uint8_t d = o.frameWanted;
                d = on ? uint8_t(d | flags) : uint8_t(d & ~flags);
                uint8_t shown = d;
                if ((shown & WindowFrame::TitleBar) == 0)
                        shown &= uint8_t(~(WindowFrame::MenuButton | WindowFrame::MinimizeButton |
                                           WindowFrame::MaximizeButton | WindowFrame::CloseButton));
                if ((shown & WindowFrame::Border) == 0) shown &= uint8_t(~WindowFrame::Resizable);
                if (o.window->setDecorations(shown).isError()) return BadValue;
                o.frameWanted = d;
                return Ok;
        }
        constexpr uint8_t Buttons = WindowFrame::MenuButton | WindowFrame::MinimizeButton |
                                    WindowFrame::MaximizeButton | WindowFrame::CloseButton;

        GuiStatus set_Window_frame(GuiObject &o, const Value &v) {
                return setFrame(o, WindowFrame::Border, v.number != 0);
        }
        Value get_Window_frame(GuiObject &o) {
                return Value::boolean((o.frameWanted & WindowFrame::Border) != 0);
        }
        GuiStatus set_Window_titleBar(GuiObject &o, const Value &v) {
                return setFrame(o, WindowFrame::TitleBar, v.number != 0);
        }
        Value get_Window_titleBar(GuiObject &o) {
                return Value::boolean((o.frameWanted & WindowFrame::TitleBar) != 0);
        }
        GuiStatus set_Window_buttons(GuiObject &o, const Value &v) {
                return setFrame(o, Buttons, v.number != 0);
        }
        Value get_Window_buttons(GuiObject &o) {
                return Value::boolean((o.frameWanted & Buttons) != 0);
        }
        GuiStatus set_Window_resizable(GuiObject &o, const Value &v) {
                return setFrame(o, WindowFrame::Resizable, v.number != 0);
        }
        Value get_Window_resizable(GuiObject &o) {
                return Value::boolean((o.frameWanted & WindowFrame::Resizable) != 0);
        }

        GuiStatus set_Window_above(GuiObject &o, const Value &v) {
                o.window->above = v.number != 0;
                if (o.window->isShown()) (void)wmOf(o).setAbove(o.window->id(), o.window->above);
                return Ok;
        }
        Value get_Window_above(GuiObject &o) {
                return Value::boolean(o.window->above);
        }
        GuiStatus set_Window_shaded(GuiObject &o, const Value &v) {
                if (!o.window->isShown()) return BadAccess;
                return wmOf(o).shade(o.window->id(), v.number != 0).isOk() ? Ok : BadMatch;
        }
        Value get_Window_shaded(GuiObject &o) {
                return Value::boolean(o.window->isShown() && wmOf(o).isShaded(o.window->id()));
        }
        Value get_Window_minimized(GuiObject &o) {
                return Value::boolean(o.window->isShown() && wmOf(o).isMinimized(o.window->id()));
        }
        Value get_Window_maximized(GuiObject &o) {
                return Value::boolean(o.window->isShown() && o.window->placement().maximized);
        }
        Value get_Window_active(GuiObject &o) {
                return Value::boolean(o.window->isActive());
        }

        // Its owner: another of the client's windows (0: none). It opens centred on it, and a
        // modal one disables the owner's widgets while it shows.
        GuiStatus set_Window_owner(GuiObject &o, const Value &v) {
                const uint32_t id = uint32_t(v.number);
                if (id != 0) {
                        const GuiObject *owner = o.connection->find(id);
                        if (owner == nullptr) return GuiStatus(GuiProtocol::ErrorCode::BadId);
                        if (owner->window == nullptr || owner == &o) return BadMatch;
                }
                ClientWindow &win = *o.window;
                const bool    was = win.modal;
                win.modal = false;
                win.updateBlock(); // the old owner's let go
                win.ownerId = id;
                win.modal = was;
                win.updateBlock();
                return Ok;
        }
        Value get_Window_owner(GuiObject &o) {
                return Value::id(o.window->ownerId);
        }
        GuiStatus set_Window_modal(GuiObject &o, const Value &v) {
                o.window->modal = v.number != 0;
                o.window->updateBlock();
                return Ok;
        }
        Value get_Window_modal(GuiObject &o) {
                return Value::boolean(o.window->modal);
        }

        GuiStatus set_Window_windowMenuItem(GuiObject &o, const Value &v) {
                o.window->setMenuItem(v.text);
                return Ok;
        }
        Value get_Window_windowMenuItem(GuiObject &o) {
                return Value::string(o.window->menuItem());
        }

        GuiStatus windowOp(GuiObject &o, Error (WindowManager::*op)(uint8_t)) {
                if (!o.window->isShown()) return BadAccess;
                return (wmOf(o).*op)(o.window->id()).isOk() ? Ok : BadMatch;
        }
        GuiStatus call_Window_minimize(GuiObject &o, Args, Value &) {
                return windowOp(o, &WindowManager::minimize);
        }
        GuiStatus call_Window_maximize(GuiObject &o, Args, Value &) {
                return windowOp(o, &WindowManager::maximize);
        }
        GuiStatus call_Window_restore(GuiObject &o, Args, Value &) {
                return windowOp(o, &WindowManager::restore);
        }
        GuiStatus call_Window_raise(GuiObject &o, Args, Value &) {
                return windowOp(o, &WindowManager::raise);
        }

        GuiStatus create_Window(GuiObject &o, GuiObject *parent) {
                if (parent != nullptr) return BadMatch;
                GuiWindow::Config config;
                config.rect = Rect{0, 0, WindowFrame::outerWidth(DefaultWidth, WindowFrame::Standard),
                                   WindowFrame::outerHeight(DefaultHeight, WindowFrame::Standard)};
                config.title = "";
                config.kind = "program";
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ClientWindow));
                if (mem == nullptr) return NoMem;
                auto *w = ::new (mem) ClientWindow(o.connection->application(), config, o);
                if (w->column() == nullptr) {
                        w->~ClientWindow();
                        Memory::free(w);
                        return NoMem;
                }
                o.window = w;
                o.container = w->column();
                o.owner = w;
                return Ok;
        }

        // ---- Box

        BoxLayout *box(GuiObject &o) {
                return static_cast<BoxLayout *>(o.widget);
        }

        GuiStatus set_Box_direction(GuiObject &o, const Value &v) {
                box(o)->setDirection(atomName(v) == "across" ? BoxLayout::Direction::Horizontal
                                                             : BoxLayout::Direction::Vertical);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Box_direction(GuiObject &o) {
                const bool across = box(o)->direction() == BoxLayout::Direction::Horizontal;
                return Value::atom(GuiAtoms::intern(across ? StringView("across") : StringView("down")));
        }
        GuiStatus set_Box_spacing(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 256) return BadValue;
                box(o)->setSpacing(uint16_t(v.number));
                o.connection->touched(o.owner);
                return Ok;
        }
        GuiStatus set_Box_margin(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 256) return BadValue;
                const uint16_t m = uint16_t(v.number);
                box(o)->setMargins(m, m, m, m);
                o.connection->touched(o.owner);
                return Ok;
        }
        GuiStatus create_Box(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<BoxLayout>(o, parent, BoxLayout::Direction::Vertical);
                if (s == Ok) {
                        o.container = o.widget;
                        box(o)->setSpacing(Spacing);
                }
                return s;
        }

        // ---- Label

        Label *label(GuiObject &o) {
                return static_cast<Label *>(o.widget);
        }
        GuiStatus set_Label_text(GuiObject &o, const Value &v) {
                label(o)->setText(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Label_text(GuiObject &o) {
                return Value::string(label(o)->text());
        }
        GuiStatus create_Label(GuiObject &o, GuiObject *parent) {
                return makeWidget<Label>(o, parent);
        }

        // ---- Button

        Button *button(GuiObject &o) {
                return static_cast<Button *>(o.widget);
        }
        GuiStatus set_Button_text(GuiObject &o, const Value &v) {
                button(o)->setText(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Button_text(GuiObject &o) {
                return Value::string(button(o)->text());
        }
        GuiStatus call_Button_click(GuiObject &o, Args, Value &) {
                button(o)->click();
                return Ok;
        }
        void buttonClicked(void *arg, Button &) {
                emitNone(*static_cast<GuiObject *>(arg), "clicked");
                return;
        }
        GuiStatus create_Button(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<Button>(o, parent);
                if (s == Ok) button(o)->setClicked(buttonClicked, &o);
                return s;
        }

        // ---- LineEdit

        LineEdit *field(GuiObject &o) {
                return static_cast<LineEdit *>(o.widget);
        }
        GuiStatus set_LineEdit_text(GuiObject &o, const Value &v) {
                field(o)->setText(v.text);
                return Ok;
        }
        Value get_LineEdit_text(GuiObject &o) {
                return Value::string(field(o)->text());
        }
        GuiStatus set_LineEdit_masked(GuiObject &o, const Value &v) {
                field(o)->setMasked(v.number != 0);
                return Ok;
        }
        GuiStatus set_LineEdit_maxLength(GuiObject &o, const Value &v) {
                if (v.number < 0) return BadValue;
                field(o)->setMaxLength(size_t(v.number));
                return Ok;
        }
        GuiStatus call_LineEdit_selectAll(GuiObject &o, Args, Value &) {
                field(o)->selectAll();
                return Ok;
        }
        void fieldEvent(GuiObject &o, const char *name) {
                const Value text = Value::string(field(o)->text());
                o.connection->emit(o, name, Args(&text, 1));
                return;
        }
        void fieldAccepted(void *arg, LineEdit &) {
                fieldEvent(*static_cast<GuiObject *>(arg), "accepted");
                return;
        }
        void fieldEdited(void *arg, LineEdit &) {
                fieldEvent(*static_cast<GuiObject *>(arg), "edited");
                return;
        }
        GuiStatus create_LineEdit(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<LineEdit>(o, parent);
                if (s == Ok) {
                        field(o)->setAccepted(fieldAccepted, &o);
                        field(o)->setEdited(fieldEdited, &o);
                }
                return s;
        }

        // ---- The schema's tables, from classes.def

        template <typename... T> constexpr uint8_t countOf(T...) {
                return uint8_t(sizeof...(T));
        }

        using enum GuiProtocol::Tag;

#define GUI_CREATE_Concrete(C) create_##C
#define GUI_CREATE_Abstract(C) nullptr
#define GUI_SET_RW(C, N) set_##C##_##N
#define GUI_SET_WO(C, N) set_##C##_##N
#define GUI_SET_RO(C, N) nullptr
#define GUI_GET_RW(C, N) get_##C##_##N
#define GUI_GET_RO(C, N) get_##C##_##N
#define GUI_GET_WO(C, N) nullptr
#define GUI_DEFAULT_On true
#define GUI_DEFAULT_Off false

        // Classes (a parent named, or "None").
#define GUI_CLASS(C, P, K) {#C, #P, GUI_CREATE_##K(C)},
#define GUI_PROP(C, N, T, A)
#define GUI_VALUES(C, N, ...)
#define GUI_METHOD(C, N, R, ...)
#define GUI_EVENT(C, N, D, ...)
        constexpr GuiClass Classes[] = {
#include "../../../sdk/gui/classes.def"
        };
#undef GUI_CLASS
#undef GUI_PROP

        // Properties.
#define GUI_CLASS(C, P, K)
#define GUI_PROP(C, N, T, A) {#C, #N, T, GUI_SET_##A(C, N), GUI_GET_##A(C, N)},
        constexpr GuiProperty Properties[] = {
#include "../../../sdk/gui/classes.def"
        };
#undef GUI_PROP
#undef GUI_VALUES

        // Atom properties' names.
#define GUI_PROP(C, N, T, A)
#define GUI_VALUES(C, N, ...) {#C, #N, #__VA_ARGS__},
        constexpr GuiValues Values[] = {
#include "../../../sdk/gui/classes.def"
        };
#undef GUI_VALUES
#undef GUI_METHOD

        // Methods.
#define GUI_VALUES(C, N, ...)
#define GUI_METHOD(C, N, R, ...) {#C, #N, R, {__VA_ARGS__}, countOf(__VA_ARGS__), call_##C##_##N},
        constexpr GuiMethod Methods[] = {
#include "../../../sdk/gui/classes.def"
        };
#undef GUI_METHOD
#undef GUI_EVENT

        // Events.
#define GUI_METHOD(C, N, R, ...)
#define GUI_EVENT(C, N, D, ...) {#C, #N, GUI_DEFAULT_##D, {__VA_ARGS__}, countOf(__VA_ARGS__)},
        constexpr GuiEvent Events[] = {
#include "../../../sdk/gui/classes.def"
        };
#undef GUI_CLASS
#undef GUI_PROP
#undef GUI_VALUES
#undef GUI_METHOD
#undef GUI_EVENT

        bool isClass(const char *cls, const GuiClass *c) {
                return c != nullptr && std::strcmp(cls, c->name) == 0;
        }

} // namespace

// ---- ClientWindow

ClientWindow::ClientWindow(GuiApplication &app, const Config &config, GuiObject &object)
    : GuiWindow(app, config), _object(object) {
        _column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (_column != nullptr) {
                _column->setMargins(WindowMargin, WindowMargin, WindowMargin, WindowMargin);
                _column->setSpacing(Spacing);
                setCentral(_column);
        }
}

ClientWindow::~ClientWindow() {
        application().cancelPosts(this);
        close();
        updateBlock();
        Memory::free(_iconPixels);
}

void ClientWindow::closedByManager() {
        GuiWindow::closedByManager();
        updateBlock();
        emitNone(_object, "closed");
        return;
}

// Asked to close: the client decides if it listens (X11's WM_DELETE_WINDOW), else it closes.
void ClientWindow::closeRequested() {
        if (_object.connection->listens(_object, "closeRequested")) {
                emitNone(_object, "closeRequested");
                return;
        }
        GuiWindow::closeRequested();
        return;
}

void ClientWindow::resized() {
        GuiWindow::resized();
        const bmds::Rect g = geometry();
        if (!isShown() || (g.width == _toldWidth && g.height == _toldHeight)) return;
        _toldWidth = g.width;
        _toldHeight = g.height;
        const Value size[] = {Value::integer(g.width), Value::integer(g.height)};
        _object.connection->emit(_object, "resized", Args(size, 2));
        return;
}

void ClientWindow::activeChanged() {
        // Blocked by a modal window: that one comes up and takes the keys (the window manager
        // has no owner relation to keep it above, §7).
        if (isActive() && _object.blocked > 0) {
                if (ClientWindow *over = _object.connection->modalOver(_object.id); over != nullptr) {
                        over->activate();
                        return;
                }
        }
        const Value active = Value::boolean(isActive());
        _object.connection->emit(_object, "activated", Args(&active, 1));
        return;
}

bool ClientWindow::setIconNamed(StringView name) {
        if (name.isEmpty()) {
                setIcon(nullptr);
                Memory::free(_iconPixels);
                _iconPixels = nullptr;
                _iconName.clear();
                return true;
        }
        const Icon *found = application().icons().find(name, 16);
        if (found == nullptr || !found->isValid()) return false;
        const size_t bytes = size_t(found->width) * found->height * sizeof(uint32_t);
        auto        *pixels = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, bytes));
        if (pixels == nullptr || _iconName.assign(name).isError()) {
                Memory::free(pixels);
                return false;
        }
        std::memcpy(pixels, found->pixels, bytes);
        _icon = Icon{found->width, found->height, pixels};
        setIcon(&_icon); // the window manager has the new one before the old goes
        Memory::free(_iconPixels);
        _iconPixels = pixels;
        return true;
}

void ClientWindow::setMenuItem(StringView label) {
        if (label.data() != _menuItem.data()) (void)_menuItem.assign(label);
        if (isShown()) (void)application().windowManager().setWindowItem(id(), _menuItem.view(), menuItemChosen, this);
        return;
}

void ClientWindow::menuItemChosen(void *arg) {
        auto *self = static_cast<ClientWindow *>(arg);
        (void)self->application().post(menuItemPosted, self);
        return;
}

void ClientWindow::menuItemPosted(void *arg) {
        auto *self = static_cast<ClientWindow *>(arg);
        emitNone(self->_object, "windowMenuChosen");
        return;
}

void ClientWindow::updateBlock() {
        const bool block = isShown() && modal && ownerId != 0;
        if (block == _blocking) return;
        _blocking = block;
        GuiObject *owner = _object.connection->find(ownerId);
        if (owner == nullptr || owner->window == nullptr) return;
        if (block) {
                ++owner->blocked;
        } else if (owner->blocked > 0) {
                --owner->blocked;
        }
        if (owner->window->column() != nullptr) owner->window->column()->setEnabled(owner->blocked == 0);
        return;
}

// ---- Lookups

const GuiClass *guiClass(StringView name) {
        for (const GuiClass &c : Classes)
                if (StringView(c.name) == name) return &c;
        return nullptr;
}

const GuiClass *guiParent(const GuiClass *cls) {
        return cls != nullptr && std::strcmp(cls->parent, "None") != 0 ? guiClass(cls->parent) : nullptr;
}

const GuiProperty *guiProperty(const GuiClass *cls, uint32_t atom) {
        const StringView name = GuiAtoms::name(atom);
        for (const GuiClass *c = cls; c != nullptr && !name.isEmpty(); c = guiParent(c))
                for (const GuiProperty &p : Properties)
                        if (isClass(p.cls, c) && StringView(p.name) == name) return &p;
        return nullptr;
}

const GuiMethod *guiMethod(const GuiClass *cls, uint32_t atom) {
        const StringView name = GuiAtoms::name(atom);
        for (const GuiClass *c = cls; c != nullptr && !name.isEmpty(); c = guiParent(c))
                for (const GuiMethod &m : Methods)
                        if (isClass(m.cls, c) && StringView(m.name) == name) return &m;
        return nullptr;
}

const GuiEvent *guiEvent(const GuiClass *cls, uint32_t atom) {
        const StringView name = GuiAtoms::name(atom);
        for (const GuiClass *c = cls; c != nullptr && !name.isEmpty(); c = guiParent(c))
                for (const GuiEvent &e : Events)
                        if (isClass(e.cls, c) && StringView(e.name) == name) return &e;
        return nullptr;
}

bool guiAllowed(const GuiProperty &property, uint32_t atom) {
        const StringView name = GuiAtoms::name(atom);
        for (const GuiValues &v : Values) {
                if (std::strcmp(v.cls, property.cls) != 0 || std::strcmp(v.property, property.name) != 0) continue;
                // "across, down": the names, comma-separated.
                StringView list(v.names);
                while (!list.isEmpty()) {
                        const size_t     comma = list.indexOf(',');
                        const StringView one = (comma == StringView::NPos ? list : list.left(comma)).trimmed();
                        if (one == name) return true;
                        if (comma == StringView::NPos) break;
                        list = list.mid(comma + 1);
                }
                return false;
        }
        return true;
}

Span<const GuiProperty> guiProperties() {
        return Span<const GuiProperty>(Properties);
}

Span<const GuiMethod> guiMethods() {
        return Span<const GuiMethod>(Methods);
}

Span<const GuiEvent> guiEvents() {
        return Span<const GuiEvent>(Events);
}

BMDS_NAMESPACE_END
