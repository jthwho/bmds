/**
 * @file      controlswindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Controls window (devplan/controls.md §7d).
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/fdtable.h>
#include <bmds/filehandle.h>
#include <bmds/chord.h>
#include <bmds/guiapplication.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include "controlswindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        enum Command : uintptr_t {
                SetKey,
                AddKey,
                SetPad,
                AddPad,
                ClearPad,
                Reset,
                ResetAll,
                Close,
        };

        struct ButtonDef {
                        const char *text;
                        Command     command;
        };

        constexpr ButtonDef Buttons[] = {
                {"Set &Key", SetKey},     {"&Add Key", AddKey}, {"Set &Pad", SetPad},     {"A&dd Pad", AddPad},
                {"&Clear Pad", ClearPad}, {"&Reset", Reset},    {"Reset A&ll", ResetAll}, {"Cl&ose", Close},
        };

        constexpr int16_t HalfWay = 16384; // a stick captured past half way

        void say(const BuiltinProgram::Context &context, const char *text) {
                FileHandle *err = context.fds != nullptr ? context.fds->handle(FdTable::Stderr) : nullptr;
                if (err != nullptr)
                        (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)),
                                         WaitForever);
                return;
        }

        void windowClosed(void *arg, GuiWindow &) {
                static_cast<GuiApplication *>(arg)->quit(0);
                return;
        }

        // The words of @p text (spaces and ';' between) as a set: does @p a share one with @p b?
        bool shareWord(StringView a, StringView b) {
                auto next = [](StringView &rest) {
                        size_t i = 0;
                        while (i < rest.size() && (rest[i] == ' ' || rest[i] == ';')) ++i;
                        rest = rest.mid(i);
                        size_t end = 0;
                        while (end < rest.size() && rest[end] != ' ' && rest[end] != ';') ++end;
                        const StringView word = rest.left(end);
                        rest = rest.mid(end);
                        return word;
                };
                for (StringView ra = a, wa = next(ra); !wa.isEmpty(); wa = next(ra))
                        for (StringView rb = b, wb = next(rb); !wb.isEmpty(); wb = next(rb))
                                if (wa == wb) return true;
                return false;
        }

} // namespace

// ---- The model

size_t ControlsWindow::Model::count() const {
        return book != nullptr ? book->current().count() : 0;
}

StringView ControlsWindow::Model::text(size_t row) const {
        return book != nullptr && row < count() ? StringView(book->current().action(row).label) : StringView();
}

StringView ControlsWindow::Model::columnTitle(size_t column) const {
        static constexpr const char *Titles[] = {"Action", "Group", "Keys", "Pad"};
        return column < 4 ? StringView(Titles[column]) : StringView();
}

StringView ControlsWindow::Model::columnText(size_t row, size_t column, Span<char> buffer) const {
        if (book == nullptr || row >= count()) return StringView();
        const Controls::Action &a = book->current().action(row);
        if (column == 0) return StringView(a.label);
        if (column == 1) return StringView(a.group);
        size_t n = 0;
        if (book->current().bindingText(a.id, column == 3, buffer.data(), buffer.size(), n).isError())
                return StringView();
        if (column == 3) { // "Pad." on each is the column's title already
                size_t out = 0;
                for (size_t i = 0; i < n; ++i) {
                        if ((i == 0 || buffer[i - 1] == ' ') && i + 4 <= n && std::memcmp(&buffer[i], "Pad.", 4) == 0)
                                i += 4;
                        if (i < n) buffer[out++] = buffer[i];
                }
                n = out;
        }
        return StringView(buffer.data(), n);
}

uint16_t ControlsWindow::Model::columnWidth(size_t column) const {
        static constexpr uint16_t Widths[] = {180, 92, 240, 256};
        return column < 4 ? Widths[column] : 100;
}

// ---- The window

int ControlsWindow::run(const BuiltinProgram::Context &context, StringView id, StringView home) {
        void *memory = Memory::alloc(MemSpace::Psram, sizeof(GuiApplication));
        if (memory == nullptr) return 1;
        auto *app = ::new (memory) GuiApplication();
        app->setApplicationName("Controls");
        int status = 1;
        if (app->connect(0).isError()) {
                say(context, "controls: the window system isn't running ('win start')\n");
        } else if (void *wmem = Memory::alloc(MemSpace::Psram, sizeof(ControlsWindow)); wmem != nullptr) {
                const Rect        work = app->windowManager().workArea();
                const uint16_t    w = 816 < work.width ? 816 : work.width, h = 480 < work.height ? 480 : work.height;
                GuiWindow::Config config;
                config.rect = Rect{uint16_t((work.x + (work.width - w) / 2) & ~3u),
                                   uint16_t(work.y + (work.height - h) / 2), w, h};
                config.title = "Controls";
                config.kind = "controls";
                auto *window = ::new (wmem) ControlsWindow(*app, config);
                Error err;
                {
                        Mutex::Locker lock(app->lock(), WaitForever);
                        err = window->build(id, home);
                        if (err.isOk()) {
                                window->setClosed(windowClosed, app);
                                err = window->show();
                        }
                }
                if (err.isOk())
                        status = app->exec();
                else if (err == Error::NotExist)
                        say(context, "controls: no application with actions by that id\n");
                else
                        say(context, "controls: can't open its window\n");
                {
                        Mutex::Locker lock(app->lock(), WaitForever);
                        window->~ControlsWindow();
                }
                Memory::free(wmem);
        }
        app->~GuiApplication();
        Memory::free(memory);
        return status;
}

ControlsWindow::~ControlsWindow() {
        ControlsBook::destroy(_book);
}

Error ControlsWindow::build(StringView id, StringView home) {
        if (_home.assign(home).isError()) return Error::NoMem;
        Error err = ControlsBook::apps(_apps);
        if (err.isError()) return err;
        int chosen = _apps.isEmpty() ? -1 : 0;
        for (size_t i = 0; i < _apps.size(); ++i)
                if (_apps[i].id.view() == id) chosen = int(i);
        if (!id.isEmpty() && (chosen < 0 || _apps[size_t(chosen)].id.view() != id)) return Error::NotExist;
        _book = ControlsBook::create();
        if (_book == nullptr) return Error::NoMem;
        _model.book = _book;

        auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (column == nullptr) return Error::NoMem;
        column->setMargins(8, 8, 8, 8);
        column->setSpacing(4);
        setCentral(column);
        auto *top = Widget::create<BoxLayout>(column, BoxLayout::Direction::Horizontal);
        auto *label = top != nullptr ? Widget::create<Label>(top, "A&pplication:") : nullptr;
        _appBox = top != nullptr ? Widget::create<ComboBox>(top) : nullptr;
        _view = Widget::create<DetailsView>(column);
        _status = Widget::create<Label>(column);
        auto *buttons = Widget::create<BoxLayout>(column, BoxLayout::Direction::Horizontal);
        if (label == nullptr || _appBox == nullptr || _view == nullptr || _status == nullptr || buttons == nullptr)
                return Error::NoMem;
        top->setSpacing(4);
        label->setBuddy(_appBox);
        _appBox->setStretch(1);
        for (const ControlsBook::App &a : _apps) (void)_appBox->addItem(a.name.view());
        _appBox->setChanged(appChanged, this);
        _view->setStretch(1);
        _view->setSingleSelection(true);
        _view->setModel(&_model);
        _view->setActivated(activated, this);
        buttons->setSpacing(4);
        for (const ButtonDef &d : Buttons) {
                auto *b = Widget::create<Button>(buttons, d.text);
                if (b == nullptr) return Error::NoMem;
                b->setData(d.command);
                b->setClicked(clicked, this);
        }
        if (chosen >= 0) {
                _appBox->setCurrent(chosen);
                load(_apps[size_t(chosen)].id.view());
        } else {
                stop("No application has actions.");
        }
        setFocusWidget(_view);
        return Error::Ok;
}

void ControlsWindow::load(StringView id) {
        _capture = Capture::None;
        const Error err = _book->load(id, _home.view());
        _view->modelReset();
        if (err.isError()) {
                char text[96];
                std::snprintf(text, sizeof(text), "Its controls can't be read: %s", err.desc());
                stop(text);
                return;
        }
        if (_book->current().count() > 0) _view->selectOnly(0);
        stop("Choose an action; Enter or Set Key changes its key.");
        return;
}

void ControlsWindow::appChanged(void *arg, ComboBox &, int index) {
        auto *self = static_cast<ControlsWindow *>(arg);
        if (index >= 0 && size_t(index) < self->_apps.size()) self->load(self->_apps[size_t(index)].id.view());
        return;
}

void ControlsWindow::activated(void *arg, ItemView &, size_t) {
        static_cast<ControlsWindow *>(arg)->start(Capture::Keys, false);
        return;
}

int ControlsWindow::action() const {
        const int row = _view != nullptr ? _view->current() : -1;
        return row >= 0 && _book != nullptr && size_t(row) < _book->current().count() ? row : -1;
}

void ControlsWindow::clicked(void *arg, Button &button) {
        auto      *self = static_cast<ControlsWindow *>(arg);
        const auto command = Command(button.data());
        if (command == Close) {
                self->application().quit(0);
                return;
        }
        const int a = self->action();
        if (a < 0 && command != ResetAll) {
                self->stop("Choose an action first.");
                return;
        }
        switch (command) {
                case SetKey: self->start(Capture::Keys, false); break;
                case AddKey: self->start(Capture::Keys, true); break;
                case SetPad: self->start(Capture::Pad, false); break;
                case AddPad: self->start(Capture::Pad, true); break;
                case ClearPad:
                        (void)self->_book->current().setBinding(size_t(a), true, StringView());
                        self->save();
                        break;
                case Reset:
                        self->_book->reset(size_t(a));
                        self->save();
                        break;
                case ResetAll:
                        self->_book->resetAll();
                        self->save();
                        break;
                case Close: break;
        }
        return;
}

void ControlsWindow::start(Capture capture, bool adding) {
        _action = action();
        if (_action < 0) return;
        const Controls::Action &a = _book->current().action(size_t(_action));
        _capture = capture;
        _adding = adding;
        _secondHalf = false;
        _modifier = 0;
        _otherKey = false;
        const bool axis = a.kind == Controls::Kind::Axis;
        char       text[160];
        if (capture == Capture::Keys)
                std::snprintf(text, sizeof(text), "Press the key for %s%s (Esc: cancel)", a.label,
                              axis ? ", its low end (left, up)" : "");
        else
                std::snprintf(text, sizeof(text), "Press a pad's button%s for %s%s (Esc: cancel)",
                              axis ? " or move its stick" : " or push a stick", a.label,
                              axis ? " (a button: its low end)" : "");
        _status->setText(text);
        return;
}

void ControlsWindow::stop(StringView status) {
        _capture = Capture::None;
        _status->setText(status);
        return;
}

bool ControlsWindow::keyFirst(const KeyEvent &event) {
        if (_capture == Capture::None) return false;
        const uint8_t u = event.usage;
        const bool    modifierKey = u >= 0xe0 && u <= 0xe7;
        if (event.action == KeyEvent::Action::Press && u == HidKey::Escape && event.modifiers == 0) {
                stop("Cancelled.");
                return true;
        }
        if (_capture != Capture::Keys) return true; // a pad's capture: keys aren't taken
        if (event.action == KeyEvent::Action::Press) {
                if (modifierKey) {
                        _modifier = u;
                        _otherKey = false;
                        return true;
                }
                _otherKey = true;
                Chord c;
                c.usage = 0x00070000u | u;
                c.modifiers = Chord::modifiersOf(event.modifiers);
                char text[Chord::MaxText];
                (void)c.format(text, sizeof(text));
                captured(text, false);
                return true;
        }
        if (event.action == KeyEvent::Action::Release && modifierKey && u == _modifier && !_otherKey) {
                // A modifier alone: written side-less (either Ctrl), the others held as modifiers.
                Chord c;
                c.usage = 0x00070000u | (0xe0u + (u - 0xe0u) % 4u);
                c.eitherSide = true;
                c.modifiers = uint8_t(Chord::modifiersOf(event.modifiers) & ~(1u << ((u - 0xe0u) % 4u)));
                char text[Chord::MaxText];
                (void)c.format(text, sizeof(text));
                _modifier = 0;
                captured(text, false);
        }
        return true;
}

void ControlsWindow::padEvent(const PadEvent &event) {
        if (_capture != Capture::Pad || _action < 0) return;
        const bool axis = _book->current().action(size_t(_action)).kind == Controls::Kind::Axis;
        char       text[48];
        bool       whole = false;
        if (event.type == PadEvent::Type::Button && event.value != 0) {
                std::snprintf(text, sizeof(text), "Pad.%s", PadState::buttonName(PadState::Button(event.index)));
        } else if (event.type == PadEvent::Type::Axis && (event.value > HalfWay || event.value < -HalfWay)) {
                // An axis whole for an axis's single control, a half for a button or a pair's end.
                whole = axis && !_secondHalf;
                std::snprintf(text, sizeof(text), "Pad.%s%s", PadState::axisName(PadState::Axis(event.index)),
                              whole             ? ""
                              : event.value > 0 ? "+"
                                                : "-");
        } else {
                return;
        }
        if (_secondHalf && std::strcmp(text, _first) == 0) return; // the same again: still the first end
        captured(text, whole);
        return;
}

void ControlsWindow::captured(StringView text, bool wholeAxis) {
        const Controls::Action &a = _book->current().action(size_t(_action));
        const bool              axis = a.kind == Controls::Kind::Axis;
        if (axis && !_secondHalf && !wholeAxis) {
                // One end of a pair: the other next.
                const size_t n = text.size() < sizeof(_first) - 1 ? text.size() : sizeof(_first) - 1;
                std::memcpy(_first, text.data(), n);
                _first[n] = '\0';
                _secondHalf = true;
                char status[160];
                std::snprintf(status, sizeof(status), "%s, then the %s for %s's high end (right, down)", _first,
                              _capture == Capture::Keys ? "key" : "pad control", a.label);
                _status->setText(status);
                return;
        }
        char binding[96];
        if (_secondHalf)
                std::snprintf(binding, sizeof(binding), "%s %.*s", _first, int(text.size()), text.data());
        else
                std::snprintf(binding, sizeof(binding), "%.*s", int(text.size()), text.data());
        _capture = Capture::None;
        apply(binding);
        return;
}

void ControlsWindow::apply(StringView text) {
        Controls    &c = _book->current();
        const bool   pad = text.startsWith(StringView("Pad."));
        const size_t index = size_t(_action);
        size_t       n = 0;
        (void)c.bindingText(c.action(index).id, pad, _old, sizeof(_old), n);
        char next[sizeof(_old) + sizeof(_captured) + 4];
        if (_adding && n > 0)
                std::snprintf(next, sizeof(next), "%s; %.*s", _old, int(text.size()), text.data());
        else
                std::snprintf(next, sizeof(next), "%.*s", int(text.size()), text.data());
        if (c.setBinding(index, pad, StringView(next)).isError()) {
                stop(_adding ? "It can't take another (four at most)." : "That can't be bound to it.");
                return;
        }
        const size_t len = text.size() < sizeof(_captured) - 1 ? text.size() : sizeof(_captured) - 1;
        std::memcpy(_captured, text.data(), len);
        _captured[len] = '\0';
        const uint64_t others = c.conflicts(index, pad);
        if (others == 0) {
                save();
                return;
        }
        // Another action in a set it shares has it: offered to move here.
        _asking = others;
        const int first = __builtin_ctzll(others);
        char      question[256];
        std::snprintf(question, sizeof(question), "%s is %s's too.\n\nMove it to %s?", _captured,
                      c.action(size_t(first)).label, c.action(index).label);
        char status[128];
        std::snprintf(status, sizeof(status), "%s is %s's too.", _captured, c.action(size_t(first)).label);
        _status->setText(status);
        if (MessageBox::show(application(), MessageBox::Kind::Question, "Controls", question,
                             MessageBox::Buttons::YesNo, answered, this)
                    .isError()) {
                (void)c.setBinding(index, pad, StringView(_old));
                stop("It's another action's already.");
        }
        return;
}

void ControlsWindow::answered(void *arg, MessageBox::Answer answer) {
        auto      *self = static_cast<ControlsWindow *>(arg);
        Controls  &c = self->_book->current();
        const bool pad = StringView(self->_captured).startsWith(StringView("Pad."));
        if (answer == MessageBox::Answer::Yes) {
                self->steal(self->_asking);
        } else {
                (void)c.setBinding(size_t(self->_action), pad, StringView(self->_old));
                self->stop("Left as it was.");
                self->refresh();
        }
        self->_asking = 0;
        return;
}

void ControlsWindow::steal(uint64_t others) {
        Controls  &c = _book->current();
        const bool pad = StringView(_captured).startsWith(StringView("Pad."));
        for (size_t j = 0; j < c.count(); ++j) {
                if ((others >> j & 1u) == 0) continue;
                char   was[256], kept[256] = {};
                size_t n = 0, used = 0;
                (void)c.bindingText(c.action(j).id, pad, was, sizeof(was), n);
                // Its items without the one taken (a pair goes whole).
                StringView rest(was, n);
                while (!rest.isEmpty()) {
                        size_t semi = rest.indexOf(';');
                        if (semi == StringView::NPos) semi = rest.size();
                        const StringView item = rest.left(semi).trimmed();
                        rest = semi < rest.size() ? rest.mid(semi + 1) : StringView();
                        if (item.isEmpty() || shareWord(item, StringView(_captured))) continue;
                        const int w = std::snprintf(kept + used, sizeof(kept) - used, "%s%.*s", used == 0 ? "" : "; ",
                                                    int(item.size()), item.data());
                        if (w > 0 && size_t(w) < sizeof(kept) - used) used += size_t(w);
                }
                // A key left to it: this one's old one (a swap) if it would have none.
                if (c.setBinding(j, pad, StringView(kept, used)).isError() &&
                    c.setBinding(j, pad, StringView(_old)).isError()) {
                        (void)c.setBinding(size_t(_action), pad, StringView(_old));
                        stop("Not moved: the other action would have no key.");
                        refresh();
                        return;
                }
        }
        save();
        return;
}

void ControlsWindow::save() {
        const Error err = _book->save();
        if (err.isError()) {
                char text[96];
                std::snprintf(text, sizeof(text), "Not saved: %s", err.desc());
                stop(text);
        } else {
                stop("Saved: the application uses it now.");
        }
        refresh();
        return;
}

void ControlsWindow::refresh() {
        const int row = _view->current();
        _view->modelReset();
        if (row >= 0) _view->selectOnly(row);
        return;
}

BMDS_NAMESPACE_END
