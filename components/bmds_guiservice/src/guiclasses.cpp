/**
 * @file      guiclasses.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The GUI service's classes (devplan/guiprograms.md §4b, §8: step 1's): each a table of named
 * properties, methods and events over a toolkit widget. Names are ABI once shipped: appended
 * only, their meaning never changed.
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

        constexpr GuiStatus Ok = 0;
        constexpr GuiStatus BadValue = GuiStatus(GuiProtocol::ErrorCode::BadValue);
        constexpr GuiStatus BadMatch = GuiStatus(GuiProtocol::ErrorCode::BadMatch);
        constexpr GuiStatus NoMem = GuiStatus(GuiProtocol::ErrorCode::NoMem);

        constexpr uint16_t WindowMargin = 8;
        constexpr uint16_t WindowSpacing = 4;
        constexpr uint16_t DefaultWidth = 320; // a window with nothing in it yet
        constexpr uint16_t DefaultHeight = 200;

        StringView nameOfAtom(const Value &v) {
                return v.tag == Tag::Atom ? GuiAtoms::name(uint32_t(v.number)) : StringView();
        }

        // ---- Widget: every widget's

        GuiStatus setEnabled(GuiObject &o, const Value &v) {
                o.widget->setEnabled(v.number != 0);
                return Ok;
        }
        Value getEnabled(GuiObject &o) {
                return Value::boolean(o.widget->isEnabled());
        }
        GuiStatus setVisible(GuiObject &o, const Value &v) {
                o.widget->setVisible(v.number != 0);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value getVisible(GuiObject &o) {
                return Value::boolean(o.widget->isVisible());
        }
        GuiStatus focus(GuiObject &o, GuiProtocol::Reader &, uint32_t count, Value &) {
                if (count != 0) return BadValue;
                o.widget->setFocus();
                return Ok;
        }

        constexpr GuiProperty WidgetProperties[] = {
                {"enabled", Tag::Bool, setEnabled, getEnabled},
                {"visible", Tag::Bool, setVisible, getVisible},
        };
        constexpr GuiMethod WidgetMethods[] = {{"setFocus", focus}};
        constexpr GuiClass  WidgetClass = {"Widget", nullptr, WidgetProperties, WidgetMethods, {}, nullptr};

        // A widget's parent: a window or a box (their container).
        Widget *containerOf(GuiObject *parent) {
                return parent != nullptr ? parent->container : nullptr;
        }

        // ---- Window

        GuiStatus setTitle(GuiObject &o, const Value &v) {
                o.window->setTitle(v.text);
                return Ok;
        }
        Value getTitle(GuiObject &o) {
                return Value::string(o.window->title());
        }
        GuiStatus setWidth(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 1280) return BadValue;
                o.width = uint16_t(v.number);
                return Ok;
        }
        Value getWidth(GuiObject &o) {
                return Value::integer(o.window->isShown() ? int32_t(o.window->screenRect().width) : o.width);
        }
        GuiStatus setHeight(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 720) return BadValue;
                o.height = uint16_t(v.number);
                return Ok;
        }
        Value getHeight(GuiObject &o) {
                return Value::integer(o.window->isShown() ? int32_t(o.window->screenRect().height) : o.height);
        }

        // Shown at its size (or its children's), in the middle of the work area.
        GuiStatus show(GuiObject &o, GuiProtocol::Reader &, uint32_t count, Value &) {
                if (count != 0) return BadValue;
                if (o.window->isShown()) {
                        o.window->activate();
                        return Ok;
                }
                o.window->column()->relayout();
                const GuiSize hint = o.window->column()->sizeHint();
                uint16_t      w = o.width != 0 ? o.width : (hint.width != 0 ? hint.width : DefaultWidth);
                uint16_t      h = o.height != 0 ? o.height : (hint.height != 0 ? hint.height : DefaultHeight);
                const Rect    work = o.connection->application().windowManager().workArea();
                const uint8_t frame = WindowFrame::Standard;
                uint16_t      ow = WindowFrame::outerWidth(w, frame), oh = WindowFrame::outerHeight(h, frame);
                if (ow > work.width) ow = work.width;
                if (oh > work.height) oh = work.height;
                const Rect r{uint16_t((work.x + (work.width - ow) / 2) & ~3u),
                             uint16_t(work.y + (work.height - oh) / 2), ow, oh};
                if (o.window->show(r).isError()) return NoMem;
                o.connection->touched(o.window);
                return Ok;
        }

        GuiStatus closeWindow(GuiObject &o, GuiProtocol::Reader &, uint32_t count, Value &) {
                if (count != 0) return BadValue;
                o.window->close();
                return Ok;
        }

        GuiStatus createWindow(GuiObject &o, GuiObject *parent) {
                if (parent != nullptr) return BadMatch; // a top-level window (owners: step 2)
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

        constexpr GuiProperty WindowProperties[] = {
                {"title", Tag::String, setTitle, getTitle},
                {"width", Tag::Int, setWidth, getWidth},
                {"height", Tag::Int, setHeight, getHeight},
        };
        constexpr GuiMethod WindowMethods[] = {{"show", show}, {"close", closeWindow}};
        constexpr GuiEvent  WindowEvents[] = {{"closed", true}};
        constexpr GuiClass  WindowClass = {"Window",      nullptr,      WindowProperties,
                                           WindowMethods, WindowEvents, createWindow};

        // ---- Box

        BoxLayout *box(GuiObject &o) {
                return static_cast<BoxLayout *>(o.widget);
        }

        // The toolkit's box keeps its direction for life: `Create` reads it before the box is made
        // (`GuiObject::across`); set again, it must be the same.
        GuiStatus setDirection(GuiObject &o, const Value &v) {
                const StringView d = nameOfAtom(v);
                if (d != "across" && d != "down") return BadValue;
                return (d == "across") == o.across ? Ok : GuiStatus(GuiProtocol::ErrorCode::BadAccess);
        }
        Value getDirection(GuiObject &o) {
                return Value::atom(GuiAtoms::intern(o.across ? StringView("across") : StringView("down")));
        }
        GuiStatus setSpacing(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 256) return BadValue;
                box(o)->setSpacing(uint16_t(v.number));
                o.connection->touched(o.owner);
                return Ok;
        }
        GuiStatus setMargin(GuiObject &o, const Value &v) {
                if (v.number < 0 || v.number > 256) return BadValue;
                const uint16_t m = uint16_t(v.number);
                box(o)->setMargins(m, m, m, m);
                o.connection->touched(o.owner);
                return Ok;
        }

        constexpr GuiProperty BoxProperties[] = {
                {"direction", Tag::Atom, setDirection, getDirection},
                {"spacing", Tag::Int, setSpacing, nullptr},
                {"margin", Tag::Int, setMargin, nullptr},
        };

        // ---- Label, Button, LineEdit

        Label *label(GuiObject &o) {
                return static_cast<Label *>(o.widget);
        }
        GuiStatus setLabelText(GuiObject &o, const Value &v) {
                label(o)->setText(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value getLabelText(GuiObject &o) {
                return Value::string(label(o)->text());
        }
        constexpr GuiProperty LabelProperties[] = {{"text", Tag::String, setLabelText, getLabelText}};

        Button *button(GuiObject &o) {
                return static_cast<Button *>(o.widget);
        }
        GuiStatus setButtonText(GuiObject &o, const Value &v) {
                button(o)->setText(v.text);
                o.connection->touched(o.owner);
                return Ok;
        }
        Value getButtonText(GuiObject &o) {
                return Value::string(button(o)->text());
        }
        GuiStatus clickButton(GuiObject &o, GuiProtocol::Reader &, uint32_t count, Value &) {
                if (count != 0) return BadValue;
                button(o)->click();
                return Ok;
        }
        void clicked(void *arg, Button &) {
                auto *o = static_cast<GuiObject *>(arg);
                o->connection->emit(*o, "clicked", Span<const Value>());
                return;
        }
        constexpr GuiProperty ButtonProperties[] = {{"text", Tag::String, setButtonText, getButtonText}};
        constexpr GuiMethod   ButtonMethods[] = {{"click", clickButton}};
        constexpr GuiEvent    ButtonEvents[] = {{"clicked", true}};

        LineEdit *field(GuiObject &o) {
                return static_cast<LineEdit *>(o.widget);
        }
        GuiStatus setFieldText(GuiObject &o, const Value &v) {
                field(o)->setText(v.text);
                return Ok;
        }
        Value getFieldText(GuiObject &o) {
                return Value::string(field(o)->text());
        }
        GuiStatus setMasked(GuiObject &o, const Value &v) {
                field(o)->setMasked(v.number != 0);
                return Ok;
        }
        GuiStatus setMaxLength(GuiObject &o, const Value &v) {
                if (v.number < 0) return BadValue;
                field(o)->setMaxLength(size_t(v.number));
                return Ok;
        }
        GuiStatus selectAll(GuiObject &o, GuiProtocol::Reader &, uint32_t count, Value &) {
                if (count != 0) return BadValue;
                field(o)->selectAll();
                return Ok;
        }
        void fieldEvent(GuiObject &o, const char *name) {
                const Value text = Value::string(field(o)->text());
                o.connection->emit(o, name, Span<const Value>(&text, 1));
                return;
        }
        void accepted(void *arg, LineEdit &) {
                fieldEvent(*static_cast<GuiObject *>(arg), "accepted");
                return;
        }
        void edited(void *arg, LineEdit &) {
                fieldEvent(*static_cast<GuiObject *>(arg), "edited");
                return;
        }
        constexpr GuiProperty FieldProperties[] = {
                {"text", Tag::String, setFieldText, getFieldText},
                {"masked", Tag::Bool, setMasked, nullptr},
                {"maxLength", Tag::Int, setMaxLength, nullptr},
        };
        constexpr GuiMethod FieldMethods[] = {{"selectAll", selectAll}};
        constexpr GuiEvent  FieldEvents[] = {{"accepted", true}, {"edited", false}};

        // ---- Making widgets

        // The widget made under its parent's container; null parent or one without: BadMatch.
        template <typename T, typename... Args> GuiStatus makeWidget(GuiObject &o, GuiObject *parent, Args &&...args) {
                Widget *in = containerOf(parent);
                if (in == nullptr) return BadMatch;
                T *w = Widget::create<T>(in, static_cast<Args &&>(args)...);
                if (w == nullptr) return NoMem;
                o.widget = w;
                o.owner = parent->owner;
                o.connection->touched(o.owner);
                return Ok;
        }

        GuiStatus createBox(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<BoxLayout>(
                        o, parent, o.across ? BoxLayout::Direction::Horizontal : BoxLayout::Direction::Vertical);
                if (s == Ok) {
                        o.container = o.widget;
                        box(o)->setSpacing(WindowSpacing);
                }
                return s;
        }
        GuiStatus createLabel(GuiObject &o, GuiObject *parent) {
                return makeWidget<Label>(o, parent);
        }
        GuiStatus createButton(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<Button>(o, parent);
                if (s == Ok) button(o)->setClicked(clicked, &o);
                return s;
        }
        GuiStatus createField(GuiObject &o, GuiObject *parent) {
                const GuiStatus s = makeWidget<LineEdit>(o, parent);
                if (s == Ok) {
                        field(o)->setAccepted(accepted, &o);
                        field(o)->setEdited(edited, &o);
                }
                return s;
        }

        constexpr GuiClass BoxClass = {"Box", &WidgetClass, BoxProperties, {}, {}, createBox};
        constexpr GuiClass LabelClass = {"Label", &WidgetClass, LabelProperties, {}, {}, createLabel};
        constexpr GuiClass ButtonClass = {"Button",      &WidgetClass, ButtonProperties,
                                          ButtonMethods, ButtonEvents, createButton};
        constexpr GuiClass FieldClass = {"LineEdit",   &WidgetClass, FieldProperties,
                                         FieldMethods, FieldEvents,  createField};

        constexpr const GuiClass *Classes[] = {&WidgetClass, &WindowClass, &BoxClass,
                                               &LabelClass,  &ButtonClass, &FieldClass};

} // namespace

// ---- ClientWindow

ClientWindow::ClientWindow(GuiApplication &app, const Config &config, GuiObject &object)
    : GuiWindow(app, config), _object(object) {
        _column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (_column != nullptr) {
                _column->setMargins(WindowMargin, WindowMargin, WindowMargin, WindowMargin);
                _column->setSpacing(WindowSpacing);
                setCentral(_column);
        }
}

ClientWindow::~ClientWindow() {
        close();
}

void ClientWindow::closedByManager() {
        GuiWindow::closedByManager();
        _object.connection->emit(_object, "closed", Span<const GuiProtocol::Value>());
        return;
}

// ---- Lookups

const GuiClass *guiClass(StringView name) {
        for (const GuiClass *c : Classes)
                if (StringView(c->name) == name) return c;
        return nullptr;
}

const GuiProperty *guiProperty(const GuiClass *cls, uint32_t atom) {
        const StringView name = GuiAtoms::name(atom);
        for (const GuiClass *c = cls; c != nullptr && !name.isEmpty(); c = c->parent)
                for (const GuiProperty &p : c->properties)
                        if (StringView(p.name) == name) return &p;
        return nullptr;
}

const GuiMethod *guiMethod(const GuiClass *cls, uint32_t atom) {
        const StringView name = GuiAtoms::name(atom);
        for (const GuiClass *c = cls; c != nullptr && !name.isEmpty(); c = c->parent)
                for (const GuiMethod &m : c->methods)
                        if (StringView(m.name) == name) return &m;
        return nullptr;
}

const GuiEvent *guiEvent(const GuiClass *cls, uint32_t atom) {
        const StringView name = GuiAtoms::name(atom);
        for (const GuiClass *c = cls; c != nullptr && !name.isEmpty(); c = c->parent)
                for (const GuiEvent &e : c->events)
                        if (StringView(e.name) == name) return &e;
        return nullptr;
}

BMDS_NAMESPACE_END
