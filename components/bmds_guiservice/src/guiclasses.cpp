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

#include <climits>
#include <cstring>
#include <new>
#include <bmds/codeedit.h>
#include <bmds/colordialog.h>
#include <bmds/combobox.h>
#include <bmds/detailsview.h>
#include <bmds/filedialog.h>
#include <bmds/formlayout.h>
#include <bmds/furniture.h>
#include <bmds/iconview.h>
#include <bmds/itemmodel.h>
#include <bmds/lineedit.h>
#include <bmds/numberfield.h>
#include <bmds/slider.h>
#include <bmds/tabwidget.h>
#include <bmds/terminalwidget.h>
#include <bmds/textedit.h>
#include <bmds/textsyntax.h>
#include <bmds/treeview.h>
#include <bmds/memory.h>
#include <bmds/menu.h>
#include <bmds/menubar.h>
#include <bmds/messagebox.h>
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
                if (parent->adopt != nullptr) {
                        // A container that places its children itself (a splitter's sides, tabs' pages).
                        const GuiStatus s = parent->adopt(*parent, o);
                        if (s != Ok) {
                                w->destroy();
                                o.widget = nullptr;
                                return s;
                        }
                }
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

        GuiStatus set_Window_margin(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 256) return BadValue;
                const uint16_t m = uint16_t(v.number);
                o.window->column()->setMargins(m, m, m, m);
                o.connection->touched(o.window);
                return Ok;
        }
        GuiStatus set_Window_spacing(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 256) return BadValue;
                o.window->column()->setSpacing(uint16_t(v.number));
                o.connection->touched(o.window);
                return Ok;
        }

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
        void boxRelayout(GuiObject &o) {
                box(o)->relayout();
                return;
        }
        GuiStatus create_Box(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<BoxLayout>(o, parent, BoxLayout::Direction::Vertical);
                if (s == Ok) {
                        o.container = o.widget;
                        o.relayout = boxRelayout;
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

        // ---- Step 2c: controls and layouts

        void emitValue(GuiObject &o, const char *event, const Value &v) {
                o.connection->emit(o, event, Args(&v, 1));
                return;
        }

        // A range's ends: either may be set first, the other kept on its side.
        bool rangeValue(const Value &v) {
                return v.number >= INT32_MIN && v.number <= INT32_MAX;
        }

        GuiStatus set_Widget_stretch(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 255) return BadValue;
                o.widget->setStretch(uint8_t(v.number));
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Widget_stretch(GuiObject &o) {
                return Value::integer(o.widget->stretch());
        }

        // ---- CheckBox

        CheckBox *checkBox(GuiObject &o) {
                return static_cast<CheckBox *>(o.widget);
        }
        GuiStatus set_CheckBox_text(GuiObject &o, const Value &v) {
                checkBox(o)->setText(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_CheckBox_text(GuiObject &o) {
                return Value::string(checkBox(o)->text());
        }
        GuiStatus set_CheckBox_checked(GuiObject &o, const Value &v) {
                checkBox(o)->setChecked(v.number != 0);
                return Ok;
        }
        Value get_CheckBox_checked(GuiObject &o) {
                return Value::boolean(checkBox(o)->isChecked());
        }
        void checkToggled(void *arg, CheckBox &box) {
                emitValue(*static_cast<GuiObject *>(arg), "toggled", Value::boolean(box.isChecked()));
                return;
        }
        GuiStatus create_CheckBox(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<CheckBox>(o, parent);
                if (s == Ok) checkBox(o)->setToggled(checkToggled, &o);
                return s;
        }

        // ---- NumberField (its range kept in the object: the toolkit's has no getters)

        NumberField *numberField(GuiObject &o) {
                return static_cast<NumberField *>(o.widget);
        }
        void numberRange(GuiObject &o) {
                numberField(o)->setRange(o.minimum, o.maximum);
                numberField(o)->setSteps(uint32_t(o.step), uint32_t(o.step) * 10);
                return;
        }
        GuiStatus set_NumberField_value(GuiObject &o, const Value &v) {
                if (!rangeValue(v)) return BadValue;
                numberField(o)->setValue(int32_t(v.number));
                return Ok;
        }
        Value get_NumberField_value(GuiObject &o) {
                return Value::integer(numberField(o)->value());
        }
        GuiStatus set_NumberField_minimum(GuiObject &o, const Value &v) {
                o.minimum = int32_t(v.number);
                if (o.maximum < o.minimum) o.maximum = o.minimum;
                numberRange(o);
                return Ok;
        }
        Value get_NumberField_minimum(GuiObject &o) {
                return Value::integer(o.minimum);
        }
        GuiStatus set_NumberField_maximum(GuiObject &o, const Value &v) {
                o.maximum = int32_t(v.number);
                if (o.minimum > o.maximum) o.minimum = o.maximum;
                numberRange(o);
                return Ok;
        }
        Value get_NumberField_maximum(GuiObject &o) {
                return Value::integer(o.maximum);
        }
        GuiStatus set_NumberField_step(GuiObject &o, const Value &v) {
                if (v.number < 1) return BadValue;
                o.step = int32_t(v.number);
                numberRange(o);
                return Ok;
        }
        Value get_NumberField_step(GuiObject &o) {
                return Value::integer(o.step);
        }
        void numberChanged(void *arg, NumberField &, int32_t value) {
                emitValue(*static_cast<GuiObject *>(arg), "changed", Value::integer(value));
                return;
        }
        GuiStatus create_NumberField(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<NumberField>(o, parent);
                if (s != Ok) return s;
                if (numberField(o)->edit() == nullptr) {
                        o.widget->destroy();
                        o.widget = nullptr;
                        return NoMem;
                }
                numberRange(o);
                numberField(o)->setChanged(numberChanged, &o);
                return Ok;
        }

        // ---- Slider

        Slider *slider(GuiObject &o) {
                return static_cast<Slider *>(o.widget);
        }
        void sliderRange(GuiObject &o) {
                slider(o)->setRange(o.minimum, o.maximum);
                slider(o)->setSteps(o.step, o.step * 10);
                return;
        }
        GuiStatus set_Slider_value(GuiObject &o, const Value &v) {
                slider(o)->setValue(v.number);
                return Ok;
        }
        Value get_Slider_value(GuiObject &o) {
                return Value::integer(slider(o)->value());
        }
        GuiStatus set_Slider_minimum(GuiObject &o, const Value &v) {
                o.minimum = int32_t(v.number);
                if (o.maximum < o.minimum) o.maximum = o.minimum;
                sliderRange(o);
                return Ok;
        }
        Value get_Slider_minimum(GuiObject &o) {
                return Value::integer(o.minimum);
        }
        GuiStatus set_Slider_maximum(GuiObject &o, const Value &v) {
                o.maximum = int32_t(v.number);
                if (o.minimum > o.maximum) o.minimum = o.maximum;
                sliderRange(o);
                return Ok;
        }
        Value get_Slider_maximum(GuiObject &o) {
                return Value::integer(o.maximum);
        }
        GuiStatus set_Slider_step(GuiObject &o, const Value &v) {
                if (v.number < 1) return BadValue;
                o.step = int32_t(v.number);
                sliderRange(o);
                return Ok;
        }
        Value get_Slider_step(GuiObject &o) {
                return Value::integer(o.step);
        }
        GuiStatus set_Slider_direction(GuiObject &o, const Value &v) {
                slider(o)->setOrientation(atomName(v) == "down" ? Slider::Orientation::Vertical
                                                                : Slider::Orientation::Horizontal);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Slider_direction(GuiObject &o) {
                const bool down = slider(o)->orientation() == Slider::Orientation::Vertical;
                return Value::atom(GuiAtoms::intern(down ? StringView("down") : StringView("across")));
        }
        void sliderChanged(void *arg, Slider &, int32_t value) {
                emitValue(*static_cast<GuiObject *>(arg), "changed", Value::integer(value));
                return;
        }
        void sliderReleased(void *arg, Slider &, int32_t value) {
                emitValue(*static_cast<GuiObject *>(arg), "released", Value::integer(value));
                return;
        }
        GuiStatus create_Slider(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<Slider>(o, parent, Slider::Orientation::Horizontal);
                if (s != Ok) return s;
                sliderRange(o);
                slider(o)->setChanged(sliderChanged, &o);
                slider(o)->setReleased(sliderReleased, &o);
                return Ok;
        }

        // ---- ComboBox

        ComboBox *combo(GuiObject &o) {
                return static_cast<ComboBox *>(o.widget);
        }
        GuiStatus set_ComboBox_items(GuiObject &o, const Value &v) {
                if (v.elements != Tag::String && v.count != 0) return BadValue;
                if (v.count > ComboBox::MaxItems) return BadValue;
                combo(o)->clear();
                GuiProtocol::Reader items(v.items);
                for (uint32_t i = 0; i < v.count; ++i) {
                        Value item;
                        if (!items.element(Tag::String, item)) return BadValue;
                        (void)combo(o)->addItem(item.text);
                }
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_ComboBox_items(GuiObject &o) {
                o.items.clear();
                GuiProtocol::Writer w(o.items);
                for (size_t i = 0; i < combo(o)->count(); ++i) w.element(Value::string(combo(o)->itemText(i)));
                Value list;
                list.tag = Tag::List;
                list.elements = Tag::String;
                list.count = w.ok() ? uint32_t(combo(o)->count()) : 0;
                list.items = w.ok() ? ByteSpan(o.items.data(), o.items.size()) : ByteSpan();
                return list;
        }
        GuiStatus set_ComboBox_current(GuiObject &o, const Value &v) {
                if (v.number < -1 || v.number >= int64_t(combo(o)->count())) return BadValue;
                combo(o)->setCurrent(int(v.number));
                return Ok;
        }
        Value get_ComboBox_current(GuiObject &o) {
                return Value::integer(combo(o)->current());
        }
        GuiStatus call_ComboBox_addItem(GuiObject &o, Args args, Value &) {
                if (combo(o)->addItem(args[0].text).isError()) return BadAccess;
                o.connection->touched(o.owner);
                return Ok;
        }
        GuiStatus call_ComboBox_clear(GuiObject &o, Args, Value &) {
                combo(o)->clear();
                return Ok;
        }
        void comboChanged(void *arg, ComboBox &, int index) {
                emitValue(*static_cast<GuiObject *>(arg), "changed", Value::integer(index));
                return;
        }
        GuiStatus create_ComboBox(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<ComboBox>(o, parent);
                if (s == Ok) combo(o)->setChanged(comboChanged, &o);
                return s;
        }

        // ---- ProgressBar

        ProgressBar *progress(GuiObject &o) {
                return static_cast<ProgressBar *>(o.widget);
        }
        GuiStatus set_ProgressBar_value(GuiObject &o, const Value &v) {
                if (v.number < 0) return BadValue;
                progress(o)->setValue(uint64_t(v.number));
                return Ok;
        }
        Value get_ProgressBar_value(GuiObject &o) {
                Value r = Value::integer(0);
                r.tag = Tag::Long;
                r.number = int64_t(progress(o)->value());
                return r;
        }
        GuiStatus set_ProgressBar_maximum(GuiObject &o, const Value &v) {
                if (v.number < 0) return BadValue;
                o.longMaximum = v.number;
                progress(o)->setMaximum(uint64_t(v.number));
                return Ok;
        }
        Value get_ProgressBar_maximum(GuiObject &o) {
                Value r = Value::integer(0);
                r.tag = Tag::Long;
                r.number = o.longMaximum;
                return r;
        }
        GuiStatus create_ProgressBar(GuiObject &o, GuiObject *parent) {
                o.longMaximum = 100;
                return makeWidget<ProgressBar>(o, parent);
        }

        // ---- Spacer: room, stretching unless its stretch is set to 0

        class SpacerWidget final : public Widget {
                public:
                        explicit SpacerWidget(Widget *parent) : Widget(parent) { setStretch(1); }

                        GuiSize sizeHint() const override { return GuiSize{size, size}; }

                        uint16_t size = 0;
        };

        GuiStatus set_Spacer_size(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 1280) return BadValue;
                static_cast<SpacerWidget *>(o.widget)->size = uint16_t(v.number);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Spacer_size(GuiObject &o) {
                return Value::integer(static_cast<SpacerWidget *>(o.widget)->size);
        }
        GuiStatus create_Spacer(GuiObject &o, GuiObject *parent) {
                return makeWidget<SpacerWidget>(o, parent);
        }

        // ---- Form: sections and rows (a row's widgets go in its field), or any widget as a row

        bool isClassNamed(const GuiObject *o, const char *name) {
                return o != nullptr && o->cls != nullptr && std::strcmp(o->cls->name, name) == 0;
        }
        void formRelayout(GuiObject &o) {
                static_cast<FormLayout *>(o.widget)->relayout();
                return;
        }
        GuiStatus create_Form(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<FormLayout>(o, parent);
                if (s == Ok) {
                        o.container = o.widget;
                        o.relayout = formRelayout;
                }
                return s;
        }

        GuiStatus set_FormSection_title(GuiObject &o, const Value &v) {
                static_cast<FormSection *>(o.widget)->setTitle(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_FormSection_title(GuiObject &o) {
                return Value::string(static_cast<FormSection *>(o.widget)->title());
        }
        GuiStatus create_FormSection(GuiObject &o, GuiObject *parent) {
                if (!isClassNamed(parent, "Form")) return BadMatch;
                FormSection *section = static_cast<FormLayout *>(parent->widget)->addSection(StringView());
                if (section == nullptr) return NoMem;
                o.widget = section;
                o.owner = parent->owner;
                o.connection->touched(o.owner);
                return Ok;
        }

        FormRow *formRow(GuiObject &o) {
                return static_cast<FormRow *>(o.widget);
        }
        GuiStatus set_FormRow_label(GuiObject &o, const Value &v) {
                formRow(o)->label()->setText(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_FormRow_label(GuiObject &o) {
                return Value::string(formRow(o)->label()->text());
        }
        GuiStatus set_FormRow_help(GuiObject &o, const Value &v) {
                if (formRow(o)->setHelp(v.text).isError()) return NoMem;
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_FormRow_help(GuiObject &o) {
                const TextBlock *help = formRow(o)->help();
                return Value::string(help != nullptr ? help->text() : StringView());
        }
        void rowRelayout(GuiObject &o) {
                formRow(o)->field()->relayout();
                return;
        }
        GuiStatus create_FormRow(GuiObject &o, GuiObject *parent) {
                if (!isClassNamed(parent, "Form")) return BadMatch;
                FormRow *row = static_cast<FormLayout *>(parent->widget)->addRow(StringView());
                if (row == nullptr) return NoMem;
                o.widget = row;
                o.container = row->field();
                o.relayout = rowRelayout;
                o.owner = parent->owner;
                o.connection->touched(o.owner);
                return Ok;
        }

        // ---- Splitter: its first two children are its sides

        Splitter *splitter(GuiObject &o) {
                return static_cast<Splitter *>(o.widget);
        }
        GuiObject *side(GuiObject &o, int which) {
                return o.connection->find(which == 0 ? o.first : o.second);
        }
        void splitterPlace(GuiObject &o) {
                GuiObject *a = side(o, 0), *b = side(o, 1);
                splitter(o)->setWidgets(a != nullptr ? a->widget : nullptr, b != nullptr ? b->widget : nullptr);
                return;
        }
        GuiStatus splitterAdopt(GuiObject &self, GuiObject &child) {
                if (self.first == 0) {
                        self.first = child.id;
                } else if (self.second == 0) {
                        self.second = child.id;
                } else {
                        return BadMatch; // two sides only
                }
                // Placed once it is in the connection's list (find() sees it): after this round.
                self.connection->touched(self.owner);
                return Ok;
        }
        void splitterRelease(GuiObject &self, GuiObject &child) {
                if (self.first == child.id) self.first = 0;
                if (self.second == child.id) self.second = 0;
                GuiObject *a = side(self, 0), *b = side(self, 1);
                splitter(self)->setWidgets(a != nullptr ? a->widget : nullptr, b != nullptr ? b->widget : nullptr);
                return;
        }
        void splitterRelayout(GuiObject &o) {
                splitterPlace(o);
                return;
        }
        GuiStatus set_Splitter_direction(GuiObject &o, const Value &v) {
                splitter(o)->setOrientation(atomName(v) == "down" ? Splitter::Orientation::Vertical
                                                                  : Splitter::Orientation::Horizontal);
                return Ok;
        }
        Value get_Splitter_direction(GuiObject &o) {
                const bool down = splitter(o)->orientation() == Splitter::Orientation::Vertical;
                return Value::atom(GuiAtoms::intern(down ? StringView("down") : StringView("across")));
        }
        GuiStatus set_Splitter_position(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 1280) return BadValue;
                splitter(o)->setPosition(uint16_t(v.number));
                return Ok;
        }
        Value get_Splitter_position(GuiObject &o) {
                return Value::integer(splitter(o)->position());
        }
        GuiStatus set_Splitter_minFirst(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 1280) return BadValue;
                o.minimum = int32_t(v.number);
                splitter(o)->setLeast(uint16_t(o.minimum), uint16_t(o.maximum));
                return Ok;
        }
        Value get_Splitter_minFirst(GuiObject &o) {
                return Value::integer(o.minimum);
        }
        GuiStatus set_Splitter_minSecond(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 1280) return BadValue;
                o.maximum = int32_t(v.number);
                splitter(o)->setLeast(uint16_t(o.minimum), uint16_t(o.maximum));
                return Ok;
        }
        Value get_Splitter_minSecond(GuiObject &o) {
                return Value::integer(o.maximum);
        }
        GuiStatus create_Splitter(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<Splitter>(o, parent, Splitter::Orientation::Horizontal);
                if (s == Ok) {
                        o.container = o.widget;
                        o.adopt = splitterAdopt;
                        o.release = splitterRelease;
                        o.relayout = splitterRelayout;
                        o.minimum = o.maximum = 0;
                        o.widget->setStretch(1);
                }
                return s;
        }

        // ---- Tabs and their pages (boxes)

        TabWidget *tabs(GuiObject &o) {
                return static_cast<TabWidget *>(o.widget);
        }
        int tabIndex(GuiObject &tabsObject, const Widget *page) {
                for (size_t i = 0; i < tabs(tabsObject)->count(); ++i)
                        if (tabs(tabsObject)->page(i) == page) return int(i);
                return -1;
        }
        GuiStatus tabsAdopt(GuiObject &self, GuiObject &child) {
                if (!isClassNamed(&child, "TabPage")) return BadMatch;
                const Error err = tabs(self)->addTab(child.widget, StringView());
                return err.isOk() ? Ok : (err == Error::Full ? BadAccess : NoMem);
        }
        void tabsRelease(GuiObject &self, GuiObject &child) {
                const int at = tabIndex(self, child.widget);
                if (at >= 0) tabs(self)->removeTab(size_t(at));
                return;
        }
        GuiStatus set_Tabs_current(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number >= int64_t(tabs(o)->count())) return BadValue;
                tabs(o)->setCurrent(size_t(v.number));
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_Tabs_current(GuiObject &o) {
                return Value::integer(int32_t(tabs(o)->current()));
        }
        void tabsChanged(void *arg, TabWidget &, size_t index) {
                emitValue(*static_cast<GuiObject *>(arg), "changed", Value::integer(int32_t(index)));
                return;
        }
        GuiStatus create_Tabs(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<TabWidget>(o, parent);
                if (s == Ok) {
                        o.container = o.widget;
                        o.adopt = tabsAdopt;
                        o.release = tabsRelease;
                        o.widget->setStretch(1);
                        tabs(o)->setChanged(tabsChanged, &o);
                }
                return s;
        }

        GuiStatus set_TabPage_title(GuiObject &o, const Value &v) {
                GuiObject *owner = o.connection->find(o.parent);
                if (owner == nullptr) return BadMatch;
                const int at = tabIndex(*owner, o.widget);
                if (at >= 0) tabs(*owner)->setTitle(size_t(at), v.text);
                if (o.text.assign(v.text).isError()) return NoMem;
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_TabPage_title(GuiObject &o) {
                return Value::string(o.text.view());
        }
        GuiStatus create_TabPage(GuiObject &o, GuiObject *parent) {
                if (!isClassNamed(parent, "Tabs")) return BadMatch;
                return create_Box(o, parent); // a box, flush in the tabs' frame
        }

        // ---- ToolBar and its buttons

        GuiStatus call_ToolBar_addSeparator(GuiObject &o, Args, Value &) {
                if (static_cast<ToolBar *>(o.widget)->addSeparator() == nullptr) return NoMem;
                o.connection->touched(o.owner);
                return Ok;
        }
        void toolBarRelayout(GuiObject &o) {
                static_cast<ToolBar *>(o.widget)->relayout();
                return;
        }
        GuiStatus create_ToolBar(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<ToolBar>(o, parent);
                if (s == Ok) o.relayout = toolBarRelayout;
                return s;
        }

        GuiStatus set_ToolButton_text(GuiObject &o, const Value &v) {
                button(o)->setText(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_ToolButton_text(GuiObject &o) {
                return Value::string(button(o)->text());
        }
        GuiStatus set_ToolButton_icon(GuiObject &o, const Value &v) {
                if (!o.icon.set(o.connection->application().icons(), v.text, 16)) return BadValue;
                button(o)->setIcon(o.icon.icon());
                o.icon.dropPrevious();
                o.connection->touched(o.owner);
                return Ok;
        }
        Value get_ToolButton_icon(GuiObject &o) {
                return Value::string(o.icon.name());
        }
        GuiStatus call_ToolButton_click(GuiObject &o, Args, Value &) {
                button(o)->click();
                return Ok;
        }
        GuiStatus create_ToolButton(GuiObject &o, GuiObject *parent) {
                if (!isClassNamed(parent, "ToolBar")) return BadMatch;
                Button *b = static_cast<ToolBar *>(parent->widget)->addButton(nullptr, StringView(), buttonClicked, &o);
                if (b == nullptr) return NoMem;
                o.widget = b;
                o.owner = parent->owner;
                o.connection->touched(o.owner);
                return Ok;
        }

        // ---- StatusBar

        GuiStatus call_StatusBar_addField(GuiObject &o, Args args, Value &result) {
                if (args[0].number < 0 || args[0].number > 1280) return BadValue;
                const int field = static_cast<StatusBar *>(o.widget)->addField(uint16_t(args[0].number));
                if (field < 0) return BadAccess;
                result = Value::integer(field);
                o.connection->touched(o.owner);
                return Ok;
        }
        GuiStatus call_StatusBar_setField(GuiObject &o, Args args, Value &) {
                if (args[0].number < 0 || args[0].number >= int64_t(StatusBar::MaxFields)) return BadValue;
                static_cast<StatusBar *>(o.widget)->setText(size_t(args[0].number), args[1].text);
                return Ok;
        }
        void statusRelayout(GuiObject &o) {
                static_cast<StatusBar *>(o.widget)->relayout();
                return;
        }
        GuiStatus create_StatusBar(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<StatusBar>(o, parent);
                if (s == Ok) o.relayout = statusRelayout;
                return s;
        }

        // ---- Step 2d: views

        // A list value's strings, at most @p max of them, into @p out. False: not a list of strings.
        bool readStrings(const Value &v, StringView *out, size_t max, size_t &count) {
                count = 0;
                if (v.tag != Tag::List || (v.count != 0 && v.elements != Tag::String) || v.count > max) return false;
                GuiProtocol::Reader items(v.items);
                for (uint32_t i = 0; i < v.count; ++i) {
                        Value item;
                        if (!items.element(Tag::String, item)) return false;
                        out[count++] = item.text;
                }
                return true;
        }

        // A list value of @p count strings, made in @p o's `items` (valid until its next).
        template <typename F> Value makeStrings(GuiObject &o, size_t count, F text) {
                o.items.clear();
                GuiProtocol::Writer w(o.items);
                for (size_t i = 0; i < count; ++i) w.element(Value::string(text(i)));
                Value list;
                list.tag = Tag::List;
                list.elements = Tag::String;
                list.count = w.ok() ? uint32_t(count) : 0;
                list.items = w.ok() ? ByteSpan(o.items.data(), o.items.size()) : ByteSpan();
                return list;
        }

        // ---- ItemView: rows in a model of the object's own

        constexpr size_t MaxListItems = 1024; // a list set at once

        ItemView *itemView(GuiObject &o) {
                return static_cast<ItemView *>(o.widget);
        }
        ListItemModel *model(GuiObject &o) {
                return static_cast<ListItemModel *>(o.data);
        }
        void disposeModel(GuiObject &o) {
                if (o.data == nullptr) return;
                model(o)->~ListItemModel();
                Memory::free(o.data);
                o.data = nullptr;
                return;
        }

        // The model changed: the view counts again, its current row kept where it can be.
        void refresh(GuiObject &o) {
                const int cur = itemView(o)->current();
                o.quiet = true;
                itemView(o)->modelReset();
                if (cur >= 0 && size_t(cur) < itemView(o)->count()) itemView(o)->selectOnly(cur);
                o.quiet = false;
                const int now = itemView(o)->current();
                if (now != o.told) {
                        o.told = now;
                        const Value v = Value::integer(now);
                        o.connection->emit(o, "currentChanged", Args(&v, 1));
                }
                o.connection->touched(o.owner);
                return;
        }

        void viewActivated(void *arg, ItemView &, size_t row) {
                const Value v = Value::integer(int32_t(row));
                auto       &o = *static_cast<GuiObject *>(arg);
                o.connection->emit(o, "activated", Args(&v, 1));
                return;
        }
        void viewChanged(void *arg, ItemView &view) {
                auto &o = *static_cast<GuiObject *>(arg);
                if (o.quiet || view.current() == o.told) return;
                o.told = view.current();
                const Value v = Value::integer(o.told);
                o.connection->emit(o, "currentChanged", Args(&v, 1));
                return;
        }

        template <typename T> GuiStatus makeView(GuiObject &o, GuiObject *parent) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ListItemModel));
                if (mem == nullptr) return NoMem;
                o.data = ::new (mem) ListItemModel();
                const GuiStatus s = makeWidget<T>(o, parent);
                if (s != Ok) {
                        disposeModel(o);
                        return s;
                }
                o.dispose = disposeModel;
                o.step = 1; // many rows may be selected (multiSelect), as the toolkit's views
                o.widget->setStretch(1);
                itemView(o)->setModel(model(o));
                itemView(o)->setActivated(viewActivated, &o);
                itemView(o)->setChanged(viewChanged, &o);
                return Ok;
        }

        GuiStatus set_ItemView_items(GuiObject &o, const Value &v) {
                if (v.count > MaxListItems || (v.count != 0 && v.elements != Tag::String)) return BadValue;
                model(o)->clear();
                GuiProtocol::Reader items(v.items);
                for (uint32_t i = 0; i < v.count; ++i) {
                        Value item;
                        if (!items.element(Tag::String, item)) return BadValue;
                        if (model(o)->add(item.text, nullptr, nullptr).isError()) return NoMem;
                }
                refresh(o);
                return Ok;
        }
        Value get_ItemView_items(GuiObject &o) {
                return makeStrings(o, model(o)->count(), [&](size_t i) { return model(o)->text(i); });
        }
        GuiStatus set_ItemView_current(GuiObject &o, const Value &v) {
                if (v.number < -1 || v.number >= int64_t(model(o)->count())) return BadValue;
                o.quiet = true;
                itemView(o)->selectOnly(int(v.number));
                o.quiet = false;
                o.told = itemView(o)->current();
                return Ok;
        }
        Value get_ItemView_current(GuiObject &o) {
                return Value::integer(itemView(o)->current());
        }
        GuiStatus set_ItemView_multiSelect(GuiObject &o, const Value &v) {
                itemView(o)->setSingleSelection(v.number == 0);
                o.step = v.number != 0 ? 1 : 0;
                return Ok;
        }
        Value get_ItemView_multiSelect(GuiObject &o) {
                return Value::boolean(o.step != 0);
        }
        GuiStatus call_ItemView_addItem(GuiObject &o, Args args, Value &) {
                if (model(o)->add(args[0].text, nullptr, nullptr).isError()) return NoMem;
                refresh(o);
                return Ok;
        }
        GuiStatus call_ItemView_removeItem(GuiObject &o, Args args, Value &) {
                if (args[0].number < 0 || args[0].number >= int64_t(model(o)->count())) return BadValue;
                model(o)->removeAt(size_t(args[0].number));
                refresh(o);
                return Ok;
        }
        GuiStatus call_ItemView_clear(GuiObject &o, Args, Value &) {
                model(o)->clear();
                refresh(o);
                return Ok;
        }
        // A row's icon by theme name, small and large (empty: none).
        GuiStatus call_ItemView_setIcon(GuiObject &o, Args args, Value &) {
                if (args[0].number < 0 || args[0].number >= int64_t(model(o)->count())) return BadValue;
                const Icon *small = nullptr, *large = nullptr;
                if (!args[1].text.isEmpty()) {
                        small = o.connection->icon(args[1].text, 16);
                        large = o.connection->icon(args[1].text, 32);
                        if (small == nullptr && large == nullptr) return BadValue;
                }
                model(o)->setIcons(size_t(args[0].number), small, large);
                itemView(o)->update();
                return Ok;
        }

        // ---- ListView: a details view of one column, as wide as it is, without a header

        class ListViewWidget final : public DetailsView {
                public:
                        explicit ListViewWidget(Widget *parent) : DetailsView(parent) {
                                setHeaderVisible(false);
                                setSingleSelection(true);
                        }

                protected:
                        void resized() override {
                                DetailsView::resized();
                                const uint16_t w = uint16_t(viewport().width & ~3u);
                                if (w != 0 && columnWidth(0) != w) setColumnWidth(0, w);
                                return;
                        }
        };

        GuiStatus create_ListView(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeView<ListViewWidget>(o, parent);
                if (s == Ok) o.step = 0; // one row
                return s;
        }

        // ---- DetailsView

        DetailsView *details(GuiObject &o) {
                return static_cast<DetailsView *>(o.widget);
        }
        GuiStatus set_DetailsView_columns(GuiObject &o, const Value &v) {
                StringView titles[ListItemModel::MaxColumns];
                size_t     count = 0;
                if (!readStrings(v, titles, ListItemModel::MaxColumns, count) || count == 0) return BadValue;
                uint16_t         widths[ListItemModel::MaxColumns];
                ItemModel::Align aligns[ListItemModel::MaxColumns];
                for (size_t i = 0; i < count; ++i) {
                        widths[i] = i == 0 ? 200 : 100;
                        aligns[i] = ItemModel::Align::Left;
                }
                model(o)->setColumns(Span<const StringView>(titles, count), Span<const uint16_t>(widths, count),
                                     Span<const ItemModel::Align>(aligns, count));
                refresh(o);
                return Ok;
        }
        Value get_DetailsView_columns(GuiObject &o) {
                return makeStrings(o, model(o)->columns(), [&](size_t i) { return model(o)->columnTitle(i); });
        }
        GuiStatus call_DetailsView_addRow(GuiObject &o, Args args, Value &) {
                StringView texts[ListItemModel::MaxColumns];
                size_t     count = 0;
                if (!readStrings(args[0], texts, ListItemModel::MaxColumns, count) || count == 0) return BadValue;
                if (model(o)->add(texts[0], nullptr, nullptr, Span<const StringView>(texts + 1, count - 1)).isError())
                        return NoMem;
                refresh(o);
                return Ok;
        }
        GuiStatus call_DetailsView_setColumnWidth(GuiObject &o, Args args, Value &) {
                if (args[0].number < 0 || args[0].number >= int64_t(model(o)->columns())) return BadValue;
                if (args[1].number < 0 || args[1].number > 1280) return BadValue;
                details(o)->setColumnWidth(size_t(args[0].number), uint16_t(args[1].number));
                return Ok;
        }
        GuiStatus call_DetailsView_setSortIndicator(GuiObject &o, Args args, Value &) {
                if (args[0].number < -1 || args[0].number >= int64_t(model(o)->columns())) return BadValue;
                details(o)->setSortIndicator(int(args[0].number), args[1].number != 0);
                return Ok;
        }
        void detailsSortAsked(void *arg, DetailsView &, size_t column) {
                auto       &o = *static_cast<GuiObject *>(arg);
                const Value v = Value::integer(int32_t(column));
                o.connection->emit(o, "sortAsked", Args(&v, 1));
                return;
        }
        GuiStatus create_DetailsView(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeView<DetailsView>(o, parent);
                if (s == Ok) details(o)->setSortAsked(detailsSortAsked, &o);
                return s;
        }

        // ---- IconView

        GuiStatus create_IconView(GuiObject &o, GuiObject *parent) {
                return makeView<IconView>(o, parent);
        }

        // ---- TreeView: rows by path, children given when one is expanded

        TreeView *tree(GuiObject &o) {
                return static_cast<TreeView *>(o.widget);
        }
        int treeRow(GuiObject &o, StringView path) {
                return tree(o)->rows().find(path);
        }
        void treeEmitPath(GuiObject &o, const char *event, size_t row) {
                String path(MemSpace::Psram);
                if (tree(o)->rows().pathOf(row, path).isError()) return;
                const Value v = Value::string(path.view());
                o.connection->emit(o, event, Args(&v, 1));
                return;
        }
        void treeExpanding(void *arg, TreeView &, size_t row) {
                treeEmitPath(*static_cast<GuiObject *>(arg), "expanding", row);
                return;
        }
        void treeActivated(void *arg, ItemView &, size_t row) {
                treeEmitPath(*static_cast<GuiObject *>(arg), "activated", row);
                return;
        }
        void treeChanged(void *arg, ItemView &view) {
                auto &o = *static_cast<GuiObject *>(arg);
                if (o.quiet) return;
                String path(MemSpace::Psram);
                if (view.current() >= 0 && tree(o)->rows().pathOf(size_t(view.current()), path).isError()) return;
                if (path.view() == o.path.view()) return;
                (void)o.path.assign(path.view());
                const Value v = Value::string(path.view());
                o.connection->emit(o, "currentChanged", Args(&v, 1));
                return;
        }
        GuiStatus set_TreeView_current(GuiObject &o, const Value &v) {
                o.quiet = true;
                const int row = v.text.isEmpty() ? -1 : tree(o)->selectPath(v.text);
                if (v.text.isEmpty()) tree(o)->selectOnly(-1);
                o.quiet = false;
                if (row < 0 && !v.text.isEmpty()) return BadValue;
                (void)o.path.assign(v.text);
                return Ok;
        }
        Value get_TreeView_current(GuiObject &o) {
                o.text.clear();
                const int cur = tree(o)->current();
                if (cur >= 0) (void)tree(o)->rows().pathOf(size_t(cur), o.text);
                return Value::string(o.text.view());
        }
        GuiStatus call_TreeView_addRoot(GuiObject &o, Args args, Value &) {
                if (args[0].text.isEmpty()) return BadValue;
                if (tree(o)->addRoot(args[0].text).isError()) return NoMem;
                o.connection->touched(o.owner);
                return Ok;
        }
        GuiStatus call_TreeView_setChildren(GuiObject &o, Args args, Value &) {
                const int row = treeRow(o, args[0].text);
                if (row < 0) return BadValue;
                StringView names[256];
                size_t     count = 0;
                if (!readStrings(args[1], names, 256, count)) return BadValue;
                o.quiet = true;
                const Error err = tree(o)->setChildren(size_t(row), Span<const StringView>(names, count));
                o.quiet = false;
                return err.isOk() ? Ok : NoMem;
        }
        GuiStatus call_TreeView_setHasChildren(GuiObject &o, Args args, Value &) {
                const int row = treeRow(o, args[0].text);
                if (row < 0) return BadValue;
                tree(o)->setHasChildren(size_t(row), args[1].number != 0);
                return Ok;
        }
        GuiStatus call_TreeView_expand(GuiObject &o, Args args, Value &) {
                const int row = treeRow(o, args[0].text);
                if (row < 0) return BadValue;
                tree(o)->expand(size_t(row));
                return Ok;
        }
        GuiStatus call_TreeView_collapse(GuiObject &o, Args args, Value &) {
                const int row = treeRow(o, args[0].text);
                if (row < 0) return BadValue;
                tree(o)->collapse(size_t(row));
                return Ok;
        }
        GuiStatus call_TreeView_clear(GuiObject &o, Args, Value &) {
                o.quiet = true;
                tree(o)->clear();
                o.quiet = false;
                o.path.clear();
                return Ok;
        }
        GuiStatus create_TreeView(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<TreeView>(o, parent);
                if (s != Ok) return s;
                TreeView *t = tree(o);
                t->setStretch(1);
                t->setIcons(o.connection->icon("drive-harddisk", 16), o.connection->icon("folder", 16),
                            o.connection->icon("folder-open", 16));
                t->setExpanding(treeExpanding, &o);
                t->setActivated(treeActivated, &o);
                t->setChanged(treeChanged, &o);
                return Ok;
        }

        // ---- TextEdit and CodeEdit

        TextEdit *textEdit(GuiObject &o) {
                return static_cast<TextEdit *>(o.widget);
        }
        void textChanged(void *arg, TextEdit &) {
                auto &o = *static_cast<GuiObject *>(arg);
                if (!o.quiet) o.connection->emit(o, "changed", Args());
                return;
        }
        void textCursorMoved(void *arg, TextEdit &edit) {
                auto       &o = *static_cast<GuiObject *>(arg);
                const auto &c = edit.selection().cursor;
                const Value v[] = {Value::integer(int32_t(c.line)), Value::integer(int32_t(c.byte))};
                o.connection->emit(o, "cursorMoved", Args(v, 2));
                return;
        }
        GuiStatus set_TextEdit_text(GuiObject &o, const Value &v) {
                o.quiet = true;
                const Error err = textEdit(o)->setText(v.text);
                o.quiet = false;
                return err.isOk() ? Ok : NoMem;
        }
        Value get_TextEdit_text(GuiObject &o) {
                o.text.clear();
                (void)textEdit(o)->document().save(o.text);
                return Value::string(o.text.view());
        }
        GuiStatus set_TextEdit_readOnly(GuiObject &o, const Value &v) {
                textEdit(o)->setReadOnly(v.number != 0);
                return Ok;
        }
        Value get_TextEdit_readOnly(GuiObject &o) {
                return Value::boolean(textEdit(o)->isReadOnly());
        }
        Value get_TextEdit_modified(GuiObject &o) {
                return Value::boolean(textEdit(o)->document().isModified());
        }
        Value get_TextEdit_cursorLine(GuiObject &o) {
                return Value::integer(int32_t(textEdit(o)->selection().cursor.line));
        }
        Value get_TextEdit_cursorColumn(GuiObject &o) {
                return Value::integer(int32_t(textEdit(o)->selection().cursor.byte));
        }
        Value get_TextEdit_selectedText(GuiObject &o) {
                o.text.clear();
                (void)textEdit(o)->selectedText(o.text);
                return Value::string(o.text.view());
        }
        GuiStatus call_TextEdit_markSaved(GuiObject &o, Args, Value &) {
                textEdit(o)->document().setSaved();
                return Ok;
        }
        GuiStatus call_TextEdit_insert(GuiObject &o, Args args, Value &) {
                if (textEdit(o)->isReadOnly()) return BadAccess;
                textEdit(o)->insertText(args[0].text);
                return Ok;
        }
        GuiStatus call_TextEdit_goToLine(GuiObject &o, Args args, Value &) {
                if (args[0].number < 0) return BadValue;
                textEdit(o)->goToLine(uint32_t(args[0].number));
                return Ok;
        }
        GuiStatus call_TextEdit_selectAll(GuiObject &o, Args, Value &) {
                textEdit(o)->selectAll();
                return Ok;
        }
        GuiStatus call_TextEdit_undo(GuiObject &o, Args, Value &) {
                textEdit(o)->undo();
                return Ok;
        }
        GuiStatus call_TextEdit_redo(GuiObject &o, Args, Value &) {
                textEdit(o)->redo();
                return Ok;
        }
        GuiStatus call_TextEdit_cut(GuiObject &o, Args, Value &) {
                textEdit(o)->cut();
                return Ok;
        }
        GuiStatus call_TextEdit_copy(GuiObject &o, Args, Value &) {
                textEdit(o)->copy();
                return Ok;
        }
        GuiStatus call_TextEdit_paste(GuiObject &o, Args, Value &) {
                textEdit(o)->paste();
                return Ok;
        }
        template <typename T> GuiStatus makeTextEdit(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<T>(o, parent);
                if (s != Ok) return s;
                o.widget->setStretch(1);
                textEdit(o)->setChanged(textChanged, &o);
                textEdit(o)->setCursorMoved(textCursorMoved, &o);
                return Ok;
        }
        GuiStatus create_TextEdit(GuiObject &o, GuiObject *parent) {
                return makeTextEdit<TextEdit>(o, parent);
        }

        CodeEdit *codeEdit(GuiObject &o) {
                return static_cast<CodeEdit *>(o.widget);
        }
        GuiStatus set_CodeEdit_syntax(GuiObject &o, const Value &v) {
                const Syntax *syntax = v.text.isEmpty() ? nullptr : Syntax::find(v.text);
                if (syntax == nullptr && !v.text.isEmpty()) return BadValue;
                codeEdit(o)->setSyntax(syntax);
                return Ok;
        }
        Value get_CodeEdit_syntax(GuiObject &o) {
                const Syntax *syntax = codeEdit(o)->syntax();
                return Value::string(syntax != nullptr ? StringView(syntax->id) : StringView());
        }
        GuiStatus set_CodeEdit_lineNumbers(GuiObject &o, const Value &v) {
                codeEdit(o)->setLineNumbers(v.number != 0);
                return Ok;
        }
        Value get_CodeEdit_lineNumbers(GuiObject &o) {
                return Value::boolean(codeEdit(o)->hasLineNumbers());
        }
        GuiStatus set_CodeEdit_scheme(GuiObject &o, const Value &v) {
                const CodeEdit::Scheme *scheme = CodeEdit::scheme(v.text);
                if (scheme == nullptr) return BadValue;
                codeEdit(o)->setScheme(*scheme);
                return Ok;
        }
        Value get_CodeEdit_scheme(GuiObject &o) {
                return Value::string(codeEdit(o)->currentScheme().id);
        }
        GuiStatus create_CodeEdit(GuiObject &o, GuiObject *parent) {
                return makeTextEdit<CodeEdit>(o, parent);
        }

        // ---- Terminal: a screen the client feeds (its program's pty is its own), typing told back

        TerminalWidget *terminal(GuiObject &o) {
                return static_cast<TerminalWidget *>(o.widget);
        }
        void terminalTyped(void *arg, TerminalWidget &, StringView bytes) {
                auto       &o = *static_cast<GuiObject *>(arg);
                const Value v = Value::string(bytes);
                o.connection->emit(o, "typed", Args(&v, 1));
                return;
        }
        void terminalSized(void *arg, TerminalWidget &, uint16_t columns, uint16_t rows) {
                auto       &o = *static_cast<GuiObject *>(arg);
                const Value v[] = {Value::integer(columns), Value::integer(rows)};
                o.connection->emit(o, "sized", Args(v, 2));
                return;
        }
        Value get_Terminal_columns(GuiObject &o) {
                return Value::integer(terminal(o)->columns());
        }
        Value get_Terminal_rows(GuiObject &o) {
                return Value::integer(terminal(o)->rows());
        }
        GuiStatus call_Terminal_write(GuiObject &o, Args args, Value &) {
                terminal(o)->show(args[0].text);
                return Ok;
        }
        GuiStatus call_Terminal_clear(GuiObject &o, Args, Value &) {
                terminal(o)->clear();
                return Ok;
        }
        GuiStatus create_Terminal(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<TerminalWidget>(o, parent);
                if (s != Ok) return s;
                o.widget->setStretch(1);
                terminal(o)->setTyped(terminalTyped, &o);
                terminal(o)->setSized(terminalSized, &o);
                return Ok;
        }

        // ---- Step 2e: menus, dialogs, timers

        // A class's own data (`GuiObject::data`), made with the object and freed with it.
        template <typename T> T *dataOf(GuiObject &o) {
                return static_cast<T *>(o.data);
        }
        template <typename T> T *makeData(GuiObject &o) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(T));
                if (mem == nullptr) return nullptr;
                T *d = ::new (mem) T();
                o.data = d;
                return d;
        }
        template <typename T> void freeData(GuiObject &o) {
                if (o.data == nullptr) return;
                dataOf<T>(o)->~T();
                Memory::free(o.data);
                o.data = nullptr;
                return;
        }
        Value colorValue(uint32_t rgb) {
                Value v = Value::integer(0);
                v.tag = Tag::Color;
                v.number = rgb & 0xffffff;
                return v;
        }
        void removeEntry(GuiObject &o, uint32_t id) {
                for (size_t i = o.entries.size(); i-- > 0;)
                        if (o.entries[i] == id) o.entries.removeAt(i);
                return;
        }

        // A shortcut's text ("Ctrl+Shift+S", "F5", "Del") as a HID usage and modifiers (Ctrl 1, Shift 2,
        // Alt 4). False: not one.
        bool parseShortcut(StringView text, uint8_t &usage, uint8_t &mods) {
                usage = 0;
                mods = 0;
                struct Named {
                                const char *name;
                                uint8_t     usage;
                };
                static constexpr Named Keys[] = {
                        {"Enter", 0x28},    {"Return", 0x28}, {"Esc", 0x29},    {"Escape", 0x29}, {"Backspace", 0x2a},
                        {"Tab", 0x2b},      {"Space", 0x2c},  {"Minus", 0x2d},  {"-", 0x2d},      {"=", 0x2e},
                        {"Plus", 0x2e},     {"+", 0x2e},      {"Insert", 0x49}, {"Ins", 0x49},    {"Home", 0x4a},
                        {"PageUp", 0x4b},   {"PgUp", 0x4b},   {"Delete", 0x4c}, {"Del", 0x4c},    {"End", 0x4d},
                        {"PageDown", 0x4e}, {"PgDn", 0x4e},   {"Right", 0x4f},  {"Left", 0x50},   {"Down", 0x51},
                        {"Up", 0x52},
                };
                while (!text.isEmpty()) {
                        size_t plus = text.indexOf('+');
                        if (plus == 0 && text.size() == 1) plus = StringView::NPos; // "+" itself, the last
                        const StringView part = (plus == StringView::NPos ? text : text.left(plus)).trimmed();
                        text = plus == StringView::NPos ? StringView() : text.mid(plus + 1);
                        if (!text.isEmpty()) {
                                // A modifier.
                                if (part == "Ctrl" || part == "Control")
                                        mods |= 1;
                                else if (part == "Shift")
                                        mods |= 2;
                                else if (part == "Alt")
                                        mods |= 4;
                                else
                                        return false;
                                continue;
                        }
                        if (part.size() == 1 && part[0] >= 'A' && part[0] <= 'Z')
                                usage = uint8_t(0x04 + part[0] - 'A');
                        else if (part.size() == 1 && part[0] >= 'a' && part[0] <= 'z')
                                usage = uint8_t(0x04 + part[0] - 'a');
                        else if (part.size() == 1 && part[0] >= '1' && part[0] <= '9')
                                usage = uint8_t(0x1e + part[0] - '1');
                        else if (part == "0")
                                usage = 0x27;
                        else if (part.size() >= 2 && part.size() <= 3 && part[0] == 'F') {
                                int n = 0;
                                for (size_t i = 1; i < part.size(); ++i) {
                                        if (part[i] < '0' || part[i] > '9') return false;
                                        n = n * 10 + (part[i] - '0');
                                }
                                if (n < 1 || n > 12) return false;
                                usage = uint8_t(0x3a + n - 1);
                        } else {
                                for (const Named &k : Keys)
                                        if (part == k.name) usage = k.usage;
                        }
                }
                return usage != 0;
        }

        // ---- Menus: a menu keeps its entries (items, separators, submenus) and builds the toolkit's
        // menu again before it opens; a menu bar builds its titles again after a round of requests.

        struct MenuData {
                        Menu  *menu = nullptr;
                        String title{MemSpace::Psram};
        };
        struct ItemData {
                        String text{MemSpace::Psram};
                        String shortcut{MemSpace::Psram};
                        String icon{MemSpace::Psram};
                        bool   checked = false;
                        bool   enabled = true;
        };

        bool isMenu(const GuiObject *o) {
                return isClassNamed(o, "Menu");
        }
        Menu &toolkitMenu(GuiObject &o) {
                return *dataOf<MenuData>(o)->menu;
        }

        // Menu @p o's toolkit menu built again if it changed, its submenus first.
        void buildMenu(GuiObject &o) {
                for (uint32_t id : o.entries) {
                        GuiObject *child = id != 0 ? o.connection->find(id) : nullptr;
                        if (isMenu(child)) buildMenu(*child);
                }
                if (!o.dirty) return;
                o.dirty = false;
                Menu &menu = toolkitMenu(o);
                menu.clear();
                for (size_t i = 0; i < o.entries.size(); ++i) {
                        const uint32_t id = o.entries[i];
                        GuiObject     *child = id != 0 ? o.connection->find(id) : nullptr;
                        if (id == 0) {
                                (void)menu.addSeparator();
                        } else if (isMenu(child)) {
                                (void)menu.addSubmenu(dataOf<MenuData>(*child)->title.view(), toolkitMenu(*child));
                        } else if (child != nullptr) {
                                const ItemData *item = dataOf<ItemData>(*child);
                                const uint8_t   flags = uint8_t((item->enabled ? 0 : MenuLayout::Disabled) |
                                                                (item->checked ? MenuLayout::Checked : 0));
                                const Icon     *icon =
                                        item->icon.isEmpty() ? nullptr : o.connection->icon(item->icon.view(), 16);
                                (void)menu.addItem(item->text.view(), int(i), flags, item->shortcut.view(), icon);
                        }
                }
                return;
        }

        // Its parent (a menu, a menu bar) built again.
        void parentChanged(GuiObject &o) {
                GuiObject *parent = o.connection->find(o.parent);
                if (parent == nullptr) return;
                parent->dirty = true;
                o.connection->touched(parent->owner);
                return;
        }

        void menuTriggered(void *arg, int item) {
                auto &o = *static_cast<GuiObject *>(arg);
                if (item < 0 || size_t(item) >= o.entries.size()) return;
                GuiObject *child = o.connection->find(o.entries[size_t(item)]);
                if (child != nullptr && child->chosen != nullptr) (void)child->chosen(*child);
                return;
        }
        // A child going: the toolkit's menu lets go of it at once (a submenu is freed next).
        void menuRelease(GuiObject &self, GuiObject &child) {
                removeEntry(self, child.id);
                toolkitMenu(self).clear();
                self.dirty = true;
                return;
        }
        void disposeMenu(GuiObject &o) {
                MenuData *d = dataOf<MenuData>(o);
                if (d != nullptr && d->menu != nullptr) {
                        d->menu->~Menu();
                        Memory::free(d->menu);
                }
                freeData<MenuData>(o);
                return;
        }

        GuiStatus set_Menu_title(GuiObject &o, const Value &v) {
                if (dataOf<MenuData>(o)->title.assign(v.text).isError()) return NoMem;
                parentChanged(o);
                return Ok;
        }
        Value get_Menu_title(GuiObject &o) {
                return Value::string(dataOf<MenuData>(o)->title.view());
        }
        GuiStatus call_Menu_addSeparator(GuiObject &o, Args, Value &) {
                if (o.entries.append(0u).isError()) return NoMem;
                o.dirty = true;
                return Ok;
        }
        // A popup menu at (@p x, @p y) of its window's client area.
        GuiStatus call_Menu_popup(GuiObject &o, Args args, Value &) {
                GuiObject *parent = o.connection->find(o.parent);
                if (parent == nullptr || parent->window == nullptr) return BadMatch;
                if (!parent->window->isShown()) return BadAccess;
                buildMenu(o);
                const Rect r = parent->window->screenRect();
                const int  x = int(r.x) + int(args[0].number), y = int(r.y) + int(args[1].number);
                if (x < 0 || y < 0 || x > 4095 || y > 4095) return BadValue;
                return toolkitMenu(o).popup(Rect{uint16_t(x), uint16_t(y), 1, 1}).isOk() ? Ok : BadAccess;
        }
        GuiStatus create_Menu(GuiObject &o, GuiObject *parent) {
                // Under a window (a popup), a menu bar, or a menu (a submenu).
                if (parent == nullptr) return BadMatch;
                const bool inBar = isClassNamed(parent, "MenuBar");
                if (parent->window == nullptr && !inBar && !isMenu(parent)) return BadMatch;
                MenuData *d = makeData<MenuData>(o);
                if (d == nullptr) return NoMem;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                if (mem == nullptr) {
                        freeData<MenuData>(o);
                        return NoMem;
                }
                d->menu = ::new (mem) Menu(o.connection->application());
                if (parent->window == nullptr && parent->entries.append(o.id).isError()) {
                        disposeMenu(o);
                        return NoMem;
                }
                d->menu->setTriggered(menuTriggered, &o);
                o.dispose = disposeMenu;
                o.release = menuRelease;
                o.owner = parent->window != nullptr ? parent->window : parent->owner;
                if (parent->window == nullptr) {
                        parent->dirty = true;
                        o.connection->touched(o.owner);
                }
                return Ok;
        }

        // ---- MenuItem

        bool itemChosen(GuiObject &o) {
                if (!dataOf<ItemData>(o)->enabled) return false;
                emitNone(o, "chosen");
                return true;
        }
        GuiStatus set_MenuItem_text(GuiObject &o, const Value &v) {
                if (dataOf<ItemData>(o)->text.assign(v.text).isError()) return NoMem;
                parentChanged(o);
                return Ok;
        }
        Value get_MenuItem_text(GuiObject &o) {
                return Value::string(dataOf<ItemData>(o)->text.view());
        }
        GuiStatus set_MenuItem_shortcut(GuiObject &o, const Value &v) {
                uint8_t usage = 0, mods = 0;
                if (!v.text.isEmpty() && !parseShortcut(v.text, usage, mods)) return BadValue;
                if (dataOf<ItemData>(o)->shortcut.assign(v.text).isError()) return NoMem;
                o.keyUsage = usage;
                o.keyMods = mods;
                parentChanged(o);
                return Ok;
        }
        Value get_MenuItem_shortcut(GuiObject &o) {
                return Value::string(dataOf<ItemData>(o)->shortcut.view());
        }
        GuiStatus set_MenuItem_checked(GuiObject &o, const Value &v) {
                dataOf<ItemData>(o)->checked = v.number != 0;
                parentChanged(o);
                return Ok;
        }
        Value get_MenuItem_checked(GuiObject &o) {
                return Value::boolean(dataOf<ItemData>(o)->checked);
        }
        GuiStatus set_MenuItem_enabled(GuiObject &o, const Value &v) {
                dataOf<ItemData>(o)->enabled = v.number != 0;
                parentChanged(o);
                return Ok;
        }
        Value get_MenuItem_enabled(GuiObject &o) {
                return Value::boolean(dataOf<ItemData>(o)->enabled);
        }
        GuiStatus set_MenuItem_icon(GuiObject &o, const Value &v) {
                if (!v.text.isEmpty() && o.connection->icon(v.text, 16) == nullptr) return BadValue;
                if (dataOf<ItemData>(o)->icon.assign(v.text).isError()) return NoMem;
                parentChanged(o);
                return Ok;
        }
        Value get_MenuItem_icon(GuiObject &o) {
                return Value::string(dataOf<ItemData>(o)->icon.view());
        }
        GuiStatus create_MenuItem(GuiObject &o, GuiObject *parent) {
                if (!isMenu(parent)) return BadMatch;
                if (makeData<ItemData>(o) == nullptr) return NoMem;
                if (parent->entries.append(o.id).isError()) {
                        freeData<ItemData>(o);
                        return NoMem;
                }
                o.dispose = freeData<ItemData>;
                o.chosen = itemChosen;
                o.owner = parent->owner;
                parent->dirty = true;
                return Ok;
        }

        // ---- MenuBar: a window's, its menus as titles

        MenuBar *menuBar(GuiObject &o) {
                return static_cast<MenuBar *>(o.widget);
        }
        void barRelayout(GuiObject &o) {
                if (!o.dirty) return;
                o.dirty = false;
                menuBar(o)->clear();
                for (uint32_t id : o.entries) {
                        GuiObject *child = o.connection->find(id);
                        if (isMenu(child))
                                (void)menuBar(o)->addMenu(dataOf<MenuData>(*child)->title.view(), toolkitMenu(*child));
                }
                return;
        }
        void barRelease(GuiObject &self, GuiObject &child) {
                removeEntry(self, child.id);
                menuBar(self)->clear(); // before the menu is freed
                self.dirty = true;
                self.connection->touched(self.owner);
                return;
        }
        void barAboutToShow(void *arg, MenuBar &, size_t index, Menu &) {
                auto &o = *static_cast<GuiObject *>(arg);
                if (index < o.entries.size()) {
                        GuiObject *child = o.connection->find(o.entries[index]);
                        if (isMenu(child)) buildMenu(*child);
                }
                return;
        }
        void disposeBar(GuiObject &o) {
                if (o.owner != nullptr && o.owner->menuBar() == static_cast<MenuBar *>(o.widget))
                        o.owner->setMenuBar(nullptr);
                return;
        }
        GuiStatus create_MenuBar(GuiObject &o, GuiObject *parent) {
                if (parent == nullptr || parent->window == nullptr) return BadMatch; // a window's own
                const GuiStatus s = makeWidget<MenuBar>(o, parent);
                if (s != Ok) return s;
                menuBar(o)->setAboutToShow(barAboutToShow, &o);
                parent->window->setMenuBar(menuBar(o)); // F10 opens it
                o.relayout = barRelayout;
                o.release = barRelease;
                o.dispose = disposeBar;
                return Ok;
        }

        // ---- MessageBox: the toolkit's, opened by `open`; answered or let go with the object

        constexpr const char *KindNames[] = {"information", "warning", "error", "question"};
        constexpr const char *ButtonsNames[] = {"ok", "okCancel", "yesNo", "yesNoCancel"};
        constexpr const char *AnswerNames[] = {"ok", "cancel", "yes", "no"};

        struct BoxData {
                        MessageBox *box = nullptr;
                        uint8_t     kind = 0;
                        uint8_t     buttons = 0;
                        String      title{MemSpace::Psram};
                        String      text{MemSpace::Psram};
        };
        template <size_t N> int nameIndex(const char *const (&names)[N], const Value &v) {
                const StringView name = atomName(v);
                for (size_t i = 0; i < N; ++i)
                        if (name == names[i]) return int(i);
                return -1;
        }
        GuiStatus set_MessageBox_kind(GuiObject &o, const Value &v) {
                const int i = nameIndex(KindNames, v);
                if (i < 0) return BadValue;
                dataOf<BoxData>(o)->kind = uint8_t(i);
                return Ok;
        }
        Value get_MessageBox_kind(GuiObject &o) {
                return Value::atom(GuiAtoms::intern(KindNames[dataOf<BoxData>(o)->kind]));
        }
        GuiStatus set_MessageBox_title(GuiObject &o, const Value &v) {
                return dataOf<BoxData>(o)->title.assign(v.text).isOk() ? Ok : NoMem;
        }
        Value get_MessageBox_title(GuiObject &o) {
                return Value::string(dataOf<BoxData>(o)->title.view());
        }
        GuiStatus set_MessageBox_text(GuiObject &o, const Value &v) {
                return dataOf<BoxData>(o)->text.assign(v.text).isOk() ? Ok : NoMem;
        }
        Value get_MessageBox_text(GuiObject &o) {
                return Value::string(dataOf<BoxData>(o)->text.view());
        }
        GuiStatus set_MessageBox_buttons(GuiObject &o, const Value &v) {
                const int i = nameIndex(ButtonsNames, v);
                if (i < 0) return BadValue;
                dataOf<BoxData>(o)->buttons = uint8_t(i);
                return Ok;
        }
        Value get_MessageBox_buttons(GuiObject &o) {
                return Value::atom(GuiAtoms::intern(ButtonsNames[dataOf<BoxData>(o)->buttons]));
        }
        void boxAnswered(void *arg, MessageBox::Answer answer) {
                auto &o = *static_cast<GuiObject *>(arg);
                dataOf<BoxData>(o)->box = nullptr; // it frees itself
                const size_t i = size_t(answer);
                const Value  v = Value::atom(GuiAtoms::intern(i < 4 ? AnswerNames[i] : "cancel"));
                o.connection->emit(o, "answered", Args(&v, 1));
                return;
        }
        GuiStatus call_MessageBox_open(GuiObject &o, Args, Value &) {
                BoxData *d = dataOf<BoxData>(o);
                if (d->box != nullptr) {
                        d->box->activate();
                        return Ok;
                }
                auto [box, err] =
                        MessageBox::open(o.connection->application(), MessageBox::Kind(d->kind), d->title.view(),
                                         d->text.view(), MessageBox::Buttons(d->buttons), boxAnswered, &o);
                if (err.isError()) return NoMem;
                d->box = box;
                return Ok;
        }
        void disposeBox(GuiObject &o) {
                BoxData *d = dataOf<BoxData>(o);
                if (d != nullptr && d->box != nullptr) d->box->abandon();
                freeData<BoxData>(o);
                return;
        }
        GuiStatus create_MessageBox(GuiObject &o, GuiObject *parent) {
                (void)parent;
                if (makeData<BoxData>(o) == nullptr) return NoMem;
                o.dispose = disposeBox;
                return Ok;
        }

        // ---- FileDialog: made at `open` from what was set

        constexpr const char *ModeNames[] = {"open", "openMany", "save", "folder"};

        struct FileData {
                        FileDialog *dialog = nullptr;
                        uint8_t     mode = 0;
                        String      title{MemSpace::Psram};
                        String      folder{MemSpace::Psram};
                        String      fileName{MemSpace::Psram};
                        String      filters{MemSpace::Psram}; // "name\npatterns\n" each
        };
        void dropFileDialog(FileData *d) {
                if (d->dialog == nullptr) return;
                d->dialog->~FileDialog();
                Memory::free(d->dialog);
                d->dialog = nullptr;
                return;
        }
        GuiStatus set_FileDialog_mode(GuiObject &o, const Value &v) {
                const int i = nameIndex(ModeNames, v);
                if (i < 0) return BadValue;
                dataOf<FileData>(o)->mode = uint8_t(i);
                return Ok;
        }
        Value get_FileDialog_mode(GuiObject &o) {
                return Value::atom(GuiAtoms::intern(ModeNames[dataOf<FileData>(o)->mode]));
        }
        GuiStatus set_FileDialog_title(GuiObject &o, const Value &v) {
                return dataOf<FileData>(o)->title.assign(v.text).isOk() ? Ok : NoMem;
        }
        Value get_FileDialog_title(GuiObject &o) {
                return Value::string(dataOf<FileData>(o)->title.view());
        }
        GuiStatus set_FileDialog_folder(GuiObject &o, const Value &v) {
                return dataOf<FileData>(o)->folder.assign(v.text).isOk() ? Ok : NoMem;
        }
        Value get_FileDialog_folder(GuiObject &o) {
                return Value::string(dataOf<FileData>(o)->folder.view());
        }
        GuiStatus set_FileDialog_fileName(GuiObject &o, const Value &v) {
                return dataOf<FileData>(o)->fileName.assign(v.text).isOk() ? Ok : NoMem;
        }
        Value get_FileDialog_fileName(GuiObject &o) {
                return Value::string(dataOf<FileData>(o)->fileName.view());
        }
        GuiStatus call_FileDialog_addFilter(GuiObject &o, Args args, Value &) {
                if (args[0].text.indexOf('\n') != StringView::NPos || args[1].text.indexOf('\n') != StringView::NPos)
                        return BadValue;
                String &f = dataOf<FileData>(o)->filters;
                if (f.append(args[0].text).isError() || f.append("\n").isError() || f.append(args[1].text).isError() ||
                    f.append("\n").isError())
                        return NoMem;
                return Ok;
        }
        void fileFinished(void *arg, FileDialog &dialog, int result) {
                auto     &o = *static_cast<GuiObject *>(arg);
                FileData *d = dataOf<FileData>(o);
                (void)d->folder.assign(dialog.folder());
                const bool  accepted = result == FileDialog::Accepted;
                const Value v[] = {Value::boolean(accepted), makeStrings(o, accepted ? dialog.count() : 0,
                                                                         [&](size_t i) { return dialog.path(i); })};
                o.connection->emit(o, "finished", Args(v, 2));
                return;
        }
        GuiStatus call_FileDialog_open(GuiObject &o, Args, Value &) {
                FileData *d = dataOf<FileData>(o);
                if (d->dialog != nullptr && d->dialog->isShown()) {
                        d->dialog->activate();
                        return Ok;
                }
                dropFileDialog(d); // made again: its mode and title are its constructor's
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(FileDialog));
                if (mem == nullptr) return NoMem;
                d->dialog =
                        ::new (mem) FileDialog(o.connection->application(), FileDialog::Mode(d->mode), d->title.view());
                StringView rest = d->filters.view();
                while (!rest.isEmpty()) {
                        const size_t     a = rest.indexOf('\n');
                        const StringView name = rest.left(a);
                        rest = rest.mid(a + 1);
                        const size_t     b = rest.indexOf('\n');
                        const StringView patterns = rest.left(b);
                        rest = rest.mid(b + 1);
                        (void)d->dialog->addFilter(name, patterns);
                }
                if (!d->folder.isEmpty()) (void)d->dialog->setFolder(d->folder.view());
                if (!d->fileName.isEmpty()) (void)d->dialog->setFileName(d->fileName.view());
                d->dialog->setFinished(fileFinished, &o);
                if (d->dialog->open().isError()) {
                        dropFileDialog(d);
                        return NoMem;
                }
                return Ok;
        }
        void disposeFileDialog(GuiObject &o) {
                if (o.data != nullptr) dropFileDialog(dataOf<FileData>(o));
                freeData<FileData>(o);
                return;
        }
        GuiStatus create_FileDialog(GuiObject &o, GuiObject *parent) {
                (void)parent;
                if (makeData<FileData>(o) == nullptr) return NoMem;
                o.dispose = disposeFileDialog;
                return Ok;
        }

        // ---- ColorDialog

        struct ColorData {
                        ColorDialog *dialog = nullptr;
                        String       title{MemSpace::Psram};
                        uint32_t     color = 0;
        };
        void dropColorDialog(ColorData *d) {
                if (d->dialog == nullptr) return;
                d->dialog->~ColorDialog();
                Memory::free(d->dialog);
                d->dialog = nullptr;
                return;
        }
        GuiStatus set_ColorDialog_title(GuiObject &o, const Value &v) {
                return dataOf<ColorData>(o)->title.assign(v.text).isOk() ? Ok : NoMem;
        }
        Value get_ColorDialog_title(GuiObject &o) {
                return Value::string(dataOf<ColorData>(o)->title.view());
        }
        GuiStatus set_ColorDialog_color(GuiObject &o, const Value &v) {
                dataOf<ColorData>(o)->color = uint32_t(v.number) & 0xffffff;
                return Ok;
        }
        Value get_ColorDialog_color(GuiObject &o) {
                return colorValue(dataOf<ColorData>(o)->color);
        }
        void colorChanged(void *arg, ColorDialog &, uint32_t rgb) {
                auto       &o = *static_cast<GuiObject *>(arg);
                const Value v = colorValue(rgb);
                o.connection->emit(o, "changed", Args(&v, 1));
                return;
        }
        void colorFinished(void *arg, Dialog &dialog, int result) {
                auto      &o = *static_cast<GuiObject *>(arg);
                ColorData *d = dataOf<ColorData>(o);
                const bool accepted = result == ColorDialog::Accepted;
                if (accepted) d->color = static_cast<ColorDialog &>(dialog).color();
                const Value v[] = {Value::boolean(accepted), colorValue(d->color)};
                o.connection->emit(o, "finished", Args(v, 2));
                return;
        }
        GuiStatus call_ColorDialog_open(GuiObject &o, Args, Value &) {
                ColorData *d = dataOf<ColorData>(o);
                if (d->dialog != nullptr && d->dialog->isShown()) {
                        d->dialog->activate();
                        return Ok;
                }
                dropColorDialog(d);
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ColorDialog));
                if (mem == nullptr) return NoMem;
                d->dialog = ::new (mem) ColorDialog(o.connection->application(), d->title.view());
                d->dialog->setChanged(colorChanged, &o);
                d->dialog->setFinished(colorFinished, &o);
                if (d->dialog->open(d->color).isError()) {
                        dropColorDialog(d);
                        return NoMem;
                }
                return Ok;
        }
        void disposeColorDialog(GuiObject &o) {
                if (o.data != nullptr) dropColorDialog(dataOf<ColorData>(o));
                freeData<ColorData>(o);
                return;
        }
        GuiStatus create_ColorDialog(GuiObject &o, GuiObject *parent) {
                (void)parent;
                if (makeData<ColorData>(o) == nullptr) return NoMem;
                o.dispose = disposeColorDialog;
                return Ok;
        }

        // ---- Timer: the connection's application's timers (shared with its widgets')

        constexpr uint32_t MinIntervalMs = 10;

        struct TimerData {
                        int      id = -1;
                        uint32_t interval = 1000;
                        bool     repeat = true;
        };
        void timerFired(void *arg) {
                auto      &o = *static_cast<GuiObject *>(arg);
                TimerData *d = dataOf<TimerData>(o);
                if (!d->repeat) d->id = -1; // a one-shot is over
                emitNone(o, "timeout");
                return;
        }
        void timerStop(GuiObject &o) {
                TimerData *d = dataOf<TimerData>(o);
                if (d->id >= 0) o.connection->application().stopTimer(d->id);
                d->id = -1;
                return;
        }
        GuiStatus timerStart(GuiObject &o) {
                timerStop(o);
                TimerData *d = dataOf<TimerData>(o);
                auto [id, err] = o.connection->application().startTimer(d->interval, d->repeat, timerFired, &o);
                if (err.isError()) return BadAccess; // the application's timers are all in use
                d->id = id;
                return Ok;
        }
        GuiStatus set_Timer_interval(GuiObject &o, const Value &v) {
                if (v.number < int64_t(MinIntervalMs)) return BadValue;
                dataOf<TimerData>(o)->interval = uint32_t(v.number);
                return dataOf<TimerData>(o)->id >= 0 ? timerStart(o) : Ok;
        }
        Value get_Timer_interval(GuiObject &o) {
                return Value::integer(int32_t(dataOf<TimerData>(o)->interval));
        }
        GuiStatus set_Timer_repeat(GuiObject &o, const Value &v) {
                dataOf<TimerData>(o)->repeat = v.number != 0;
                return dataOf<TimerData>(o)->id >= 0 ? timerStart(o) : Ok;
        }
        Value get_Timer_repeat(GuiObject &o) {
                return Value::boolean(dataOf<TimerData>(o)->repeat);
        }
        Value get_Timer_active(GuiObject &o) {
                return Value::boolean(dataOf<TimerData>(o)->id >= 0);
        }
        GuiStatus call_Timer_start(GuiObject &o, Args, Value &) {
                return timerStart(o);
        }
        GuiStatus call_Timer_stop(GuiObject &o, Args, Value &) {
                timerStop(o);
                return Ok;
        }
        void disposeTimer(GuiObject &o) {
                if (o.data != nullptr) timerStop(o);
                freeData<TimerData>(o);
                return;
        }
        GuiStatus create_Timer(GuiObject &o, GuiObject *parent) {
                (void)parent;
                if (makeData<TimerData>(o) == nullptr) return NoMem;
                o.dispose = disposeTimer;
                return Ok;
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

// ---- GuiIcon

GuiIcon::~GuiIcon() {
        Memory::free(_pixels[0]);
        Memory::free(_pixels[1]);
}

bool GuiIcon::set(IconTheme &theme, StringView name, uint16_t side) {
        if (name.isEmpty()) {
                _name.clear();
                return true;
        }
        const Icon *found = theme.find(name, side);
        if (found == nullptr || !found->isValid()) return false;
        const uint8_t other = uint8_t(1 - _current);
        const size_t  bytes = size_t(found->width) * found->height * sizeof(uint32_t);
        Memory::free(_pixels[other]); // the one before the current: dropped already, or never in use
        _pixels[other] = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, bytes));
        if (_pixels[other] == nullptr || _name.assign(name).isError()) return false;
        std::memcpy(_pixels[other], found->pixels, bytes);
        _icons[other] = Icon{found->width, found->height, _pixels[other]};
        _current = other;
        return true;
}

void GuiIcon::dropPrevious() {
        const uint8_t other = uint8_t(1 - _current);
        Memory::free(_pixels[other]);
        _pixels[other] = nullptr;
        _icons[other] = Icon();
        return;
}

// ---- ClientWindow

ClientWindow::ClientWindow(GuiApplication &app, const Config &config, GuiObject &object)
    : GuiWindow(app, config), _object(object) {
        // Flush with the frame, as the built-in applications' columns: bars at the edges
        // (`margin` and `spacing` pad plain widgets).
        _column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (_column != nullptr) setCentral(_column);
}

ClientWindow::~ClientWindow() {
        application().cancelPosts(this);
        close();
        updateBlock();
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

// A key no widget used: a menu item's shortcut, if one is.
bool ClientWindow::keyUnused(const KeyEvent &event) {
        return _object.connection->shortcut(this, event);
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
        if (!_icon.set(application().icons(), name, 16)) return false;
        setIcon(_icon.icon()); // the window manager has the new one before the old goes
        _icon.dropPrevious();
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
