/**
 * @file      controls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * An application's actions, their bindings, and matching events against them (devplan/controls.md
 * §7c).
 */

#include <cstdio>
#include <cstring>
#include <bmds/controls.h>
#include <bmds/inireader.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using PadType = Controls::PadSource::Type;

        constexpr int16_t HalfWay = 16384; // a stick's half read as a button: down past this

        char lower(char c) {
                return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
        }

        bool same(StringView a, const char *b) {
                size_t i = 0;
                for (; i < a.size(); ++i)
                        if (b[i] == '\0' || lower(a[i]) != lower(b[i])) return false;
                return b[i] == '\0';
        }

        // [a-z0-9-], 1 to MaxName - 1 characters.
        bool isName(StringView name) {
                if (name.isEmpty() || name.size() >= Controls::MaxName) return false;
                for (char c : name)
                        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
                return true;
        }

        void copy(char *out, size_t size, StringView text) {
                const size_t n = text.size() < size - 1 ? text.size() : size - 1;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

        // The next space-separated word of @p rest.
        StringView nextWord(StringView &rest) {
                rest = rest.trimmed();
                size_t end = 0;
                while (end < rest.size() && rest[end] != ' ' && rest[end] != '\t') ++end;
                const StringView word = rest.left(end);
                rest = rest.mid(end);
                return word;
        }

        // `Pad.South`, `Pad.LeftX`, `Pad.LeftX+`, `Pad.LeftX-`: a button first for a button, an axis
        // first for an axis (`Pad.L2` is both).
        bool parsePad(StringView text, bool wantAxis, Controls::PadSource &out) {
                if (text.size() < 5 || !same(text.left(4), "pad.")) return false;
                StringView name = text.mid(4);
                PadType    half = PadType::None;
                if (name.endsWith('+') || name.endsWith('-')) {
                        half = name.endsWith('+') ? PadType::AxisPlus : PadType::AxisMinus;
                        name = name.left(name.size() - 1);
                }
                auto axis = [&]() {
                        for (uint8_t i = 0; i < PadState::AxisCount; ++i)
                                if (same(name, PadState::axisName(PadState::Axis(i)))) {
                                        out = Controls::PadSource{half != PadType::None ? half : PadType::Axis, i};
                                        return true;
                                }
                        return false;
                };
                auto button = [&]() {
                        if (half != PadType::None) return false;
                        for (uint8_t i = 0; i < PadState::ButtonCount; ++i)
                                if (same(name, PadState::buttonName(PadState::Button(i)))) {
                                        out = Controls::PadSource{PadType::Button, i};
                                        return true;
                                }
                        return false;
                };
                if (half != PadType::None || wantAxis) return axis() || button();
                return button() || axis();
        }

        // A list of key bindings (`Z;Ctrl`; an axis's `Left Right;A D`) into @p low and @p high.
        bool parseKeys(StringView value, bool axis, Chord (&low)[Controls::MaxKeys], Chord (&high)[Controls::MaxKeys]) {
                Chord  l[Controls::MaxKeys], h[Controls::MaxKeys];
                size_t n = 0;
                for (StringView item = IniReader::nextListItem(value); !item.isEmpty();
                     item = IniReader::nextListItem(value)) {
                        if (n == Controls::MaxKeys) return false;
                        if (!axis) {
                                if (Chord::parse(item, l[n]).isError()) return false;
                        } else {
                                StringView rest = item;
                                if (Chord::parse(nextWord(rest), l[n]).isError()) return false;
                                if (Chord::parse(nextWord(rest), h[n]).isError()) return false;
                                if (!rest.trimmed().isEmpty()) return false;
                        }
                        ++n;
                }
                for (size_t i = 0; i < Controls::MaxKeys; ++i) {
                        low[i] = l[i];
                        high[i] = h[i];
                }
                return true;
        }

        // A list of pad bindings: a button's controls; an axis's whole axes or pairs of buttons.
        bool parsePads(StringView value, bool axis, Controls::PadSource (&low)[Controls::MaxPad],
                       Controls::PadSource (&high)[Controls::MaxPad]) {
                Controls::PadSource l[Controls::MaxPad], h[Controls::MaxPad];
                size_t              n = 0;
                for (StringView item = IniReader::nextListItem(value); !item.isEmpty();
                     item = IniReader::nextListItem(value)) {
                        if (n == Controls::MaxPad) return false;
                        StringView       rest = item;
                        const StringView first = nextWord(rest), second = nextWord(rest);
                        if (!rest.trimmed().isEmpty()) return false;
                        if (!parsePad(first, axis && second.isEmpty(), l[n])) return false;
                        if (!second.isEmpty()) {
                                if (!axis || !parsePad(second, false, h[n])) return false;
                                if (l[n].type == PadType::Axis || h[n].type == PadType::Axis) return false;
                        } else if (axis && l[n].type != PadType::Axis) {
                                return false; // an axis's single control is a whole axis
                        } else if (!axis && l[n].type == PadType::Axis) {
                                return false; // a button's is never one
                        }
                        ++n;
                }
                for (size_t i = 0; i < Controls::MaxPad; ++i) {
                        low[i] = l[i];
                        high[i] = h[i];
                }
                return true;
        }

        size_t formatPad(char *out, size_t size, const Controls::PadSource &p) {
                if (p.type == PadType::Button)
                        return size_t(
                                std::snprintf(out, size, "Pad.%s", PadState::buttonName(PadState::Button(p.index))));
                return size_t(std::snprintf(out, size, "Pad.%s%s", PadState::axisName(PadState::Axis(p.index)),
                                            p.type == PadType::AxisPlus    ? "+"
                                            : p.type == PadType::AxisMinus ? "-"
                                                                           : ""));
        }

        bool past(PadType type, int32_t value) {
                return type == PadType::AxisPlus ? value > HalfWay : value < -HalfWay;
        }

} // namespace

// In place: the object is too big for a stack's temporary.
void Controls::clear() {
        for (Action &a : _actions) a = Action();
        for (State &s : _states) s = State();
        for (Held &h : _held) h = Held();
        std::memset(_sets, 0, sizeof(_sets));
        std::memset(_padButtons, 0, sizeof(_padButtons));
        std::memset(_padAxes, 0, sizeof(_padAxes));
        _count = 0;
        _setCount = 0;
        _active = NoSet;
        return;
}

Error Controls::declare(StringView text, uint32_t &badLine) {
        badLine = 0;
        clear(); // nothing held is told: the program asked for the change
        IniReader reader(text);
        Action   *a = nullptr; // the action being read; null in another group
        bool      inControls = false;
        Error     err = Error::Ok;
        for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End && err.isOk();
             item = reader.next()) {
                badLine = item.line;
                if (item.kind == IniReader::Kind::Invalid) {
                        err = Error::Invalid;
                } else if (item.kind == IniReader::Kind::Group) {
                        a = nullptr;
                        inControls = item.name == StringView("Controls");
                        if (!item.name.startsWith(StringView("Action "))) continue;
                        const StringView name = item.name.mid(7).trimmed();
                        if (!isName(name)) {
                                err = Error::Invalid;
                                continue;
                        }
                        for (size_t i = 0; i < _count; ++i)
                                if (name == StringView(_actions[i].name)) err = Error::Invalid;
                        if (_count == MaxActions) err = Error::Full;
                        if (err.isError()) continue;
                        a = &_actions[_count++];
                        copy(a->name, sizeof(a->name), name);
                        copy(a->label, sizeof(a->label), name);
                } else if (inControls && item.key == StringView("Sets")) {
                        StringView list = item.value;
                        _setCount = 0;
                        for (StringView s = IniReader::nextListItem(list); !s.isEmpty() && err.isOk();
                             s = IniReader::nextListItem(list)) {
                                if (!isName(s)) err = Error::Invalid;
                                if (_setCount == MaxSets) err = Error::Full;
                                if (err.isOk()) copy(_sets[_setCount++], MaxName, s);
                        }
                } else if (a != nullptr) {
                        const StringView key = item.key, value = item.value;
                        if (key == StringView("Id")) {
                                uint32_t v = 0;
                                bool     ok = !value.isEmpty() && value.size() <= 5;
                                for (char c : value) {
                                        ok = ok && c >= '0' && c <= '9';
                                        v = v * 10 + uint32_t(c - '0');
                                }
                                if (!ok || v == 0 || v > 65535) err = Error::Invalid;
                                for (size_t i = 0; i + 1 < _count && err.isOk(); ++i)
                                        if (_actions[i].id == v) err = Error::Invalid;
                                a->id = uint16_t(v);
                        } else if (key == StringView("Label")) {
                                copy(a->label, sizeof(a->label), value);
                        } else if (key == StringView("Group")) {
                                copy(a->group, sizeof(a->group), value);
                        } else if (key == StringView("Kind")) {
                                if (value == StringView("button")) {
                                        a->kind = Kind::Button;
                                } else if (value == StringView("axis")) {
                                        a->kind = Kind::Axis;
                                } else {
                                        err = Error::Invalid;
                                }
                        } else if (key == StringView("Sets")) {
                                StringView list = value;
                                a->sets = 0;
                                for (StringView s = IniReader::nextListItem(list); !s.isEmpty() && err.isOk();
                                     s = IniReader::nextListItem(list)) {
                                        bool found = false;
                                        for (uint8_t i = 0; i < _setCount; ++i)
                                                if (s == StringView(_sets[i])) {
                                                        a->sets |= uint8_t(1u << i);
                                                        found = true;
                                                }
                                        if (!found) err = Error::Invalid; // sets come first, in [Controls]
                                }
                        } else if (key == StringView("Keys")) {
                                if (!parseKeys(value, a->kind == Kind::Axis, a->keys, a->keysHigh))
                                        err = Error::Invalid;
                        } else if (key == StringView("Pad")) {
                                if (!parsePads(value, a->kind == Kind::Axis, a->pad, a->padHigh)) err = Error::Invalid;
                        } else if (key == StringView("Repeat")) {
                                if (IniReader::toBool(value, a->repeat).isError()) err = Error::Invalid;
                        }
                }
        }
        // Every action has an Id and a key (the keyboard is first-class), and some action exists.
        for (size_t i = 0; i < _count && err.isOk(); ++i) {
                if (_actions[i].id == 0 || !_actions[i].keys[0].isValid()) {
                        err = Error::Invalid;
                        badLine = 0;
                }
        }
        if (err.isOk() && _count == 0) err = Error::Invalid;
        if (err.isError()) {
                clear();
                return err;
        }
        badLine = 0;
        if (_setCount == 0) copy(_sets[_setCount++], MaxName, "default");
        _active = 0;
        return Error::Ok;
}

void Controls::applyUser(StringView text) {
        IniReader reader(text);
        Action   *a = nullptr;
        for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                if (item.kind == IniReader::Kind::Group) {
                        a = nullptr;
                        if (!item.name.startsWith(StringView("Action "))) continue;
                        const StringView name = item.name.mid(7).trimmed();
                        for (size_t i = 0; i < _count; ++i)
                                if (name == StringView(_actions[i].name)) a = &_actions[i];
                } else if (item.kind == IniReader::Kind::Entry && a != nullptr) {
                        const bool axis = a->kind == Kind::Axis;
                        if (item.key == StringView("Keys")) (void)parseKeys(item.value, axis, a->keys, a->keysHigh);
                        if (item.key == StringView("Pad")) (void)parsePads(item.value, axis, a->pad, a->padHigh);
                }
        }
        return;
}

Error Controls::setSet(StringView name, Emit emit, void *arg) {
        uint8_t set = NoSet;
        if (!name.isEmpty()) {
                for (uint8_t i = 0; i < _setCount; ++i)
                        if (name == StringView(_sets[i])) set = i;
                if (set == NoSet) return Error::NotExist;
        }
        if (set == _active) return Error::Ok;
        releaseAll(emit, arg);
        _active = set;
        return Error::Ok;
}

void Controls::press(size_t index, bool high, uint32_t source, Emit emit, void *arg) {
        State &s = _states[index];
        if (_actions[index].kind == Kind::Button) {
                if (s.down++ == 0) emit(arg, Output{Output::Type::Press, _actions[index].id, 1, source});
        } else {
                ++(high ? s.high : s.down);
                tellAxis(index, source, emit, arg);
        }
        return;
}

void Controls::release(size_t index, bool high, uint32_t source, Emit emit, void *arg) {
        State &s = _states[index];
        if (_actions[index].kind == Kind::Button) {
                if (s.down > 0 && --s.down == 0)
                        emit(arg, Output{Output::Type::Release, _actions[index].id, 0, source});
        } else {
                uint8_t &count = high ? s.high : s.down;
                if (count > 0) --count;
                tellAxis(index, source, emit, arg);
        }
        return;
}

void Controls::tellAxis(size_t index, uint32_t source, Emit emit, void *arg) {
        State        &s = _states[index];
        const int32_t keys = (s.high > 0 ? 32767 : 0) - (s.down > 0 ? 32767 : 0);
        const int32_t v = s.stick != 0 ? s.stick : keys;
        if (v == s.out) return;
        s.out = int16_t(v);
        emit(arg, Output{Output::Type::Axis, _actions[index].id, v, source});
        return;
}

bool Controls::key(uint32_t usage, uint8_t modifiers, KeyEvent::Action action, Emit emit, void *arg) {
        if (_active == NoSet) return false;
        Held *held = nullptr;
        for (Held &h : _held)
                if (h.action != 0 && h.usage == usage) held = &h;
        if (action == KeyEvent::Action::Release) {
                if (held == nullptr) return false;
                release(held->action - 1u, held->high, Keyboard, emit, arg);
                held->action = 0;
                return true;
        }
        if (held != nullptr) { // a repeat (or a press again)
                const Action &a = _actions[held->action - 1u];
                if (a.kind == Kind::Button && a.repeat) emit(arg, Output{Output::Type::Repeat, a.id, 1, Keyboard});
                return true;
        }
        if (action == KeyEvent::Action::Repeat) return false; // its press wasn't ours
        // The binding with the most modifiers wins (Ctrl+Z over Z).
        int  best = -1, bestMods = -1;
        bool bestHigh = false;
        for (size_t i = 0; i < _count; ++i) {
                const Action &a = _actions[i];
                if (!inSet(a)) continue;
                for (size_t k = 0; k < MaxKeys; ++k)
                        for (int half = 0; half < (a.kind == Kind::Axis ? 2 : 1); ++half) {
                                const Chord &c = half == 0 ? a.keys[k] : a.keysHigh[k];
                                if (!c.isValid() || !c.matches(usage, modifiers)) continue;
                                const int mods = __builtin_popcount(c.modifiers);
                                if (mods <= bestMods) continue;
                                best = int(i);
                                bestMods = mods;
                                bestHigh = half == 1;
                        }
        }
        if (best < 0) return false;
        for (Held &h : _held)
                if (h.action == 0) {
                        h = Held{usage, uint8_t(best + 1), bestHigh};
                        press(size_t(best), bestHigh, Keyboard, emit, arg);
                        return true;
                }
        return true; // too many held: swallowed rather than half-matched
}

bool Controls::pad(const PadEvent &event, Emit emit, void *arg) {
        if (_active == NoSet || event.pad >= MaxPads) return false;
        const uint32_t source = Pad | uint32_t(event.pad) << 8;
        if (event.type == PadEvent::Type::Button) {
                if (event.index >= PadState::ButtonCount) return false;
                uint16_t &held = _padButtons[event.pad][event.index];
                if (event.value == 0) {
                        if (held == 0) return false;
                        release((held & 0x7fffu) - 1u, (held & 0x8000u) != 0, source, emit, arg);
                        held = 0;
                        return true;
                }
                if (held != 0) return true;
                for (size_t i = 0; i < _count; ++i) {
                        const Action &a = _actions[i];
                        if (!inSet(a)) continue;
                        for (size_t k = 0; k < MaxPad; ++k)
                                for (int half = 0; half < 2; ++half) {
                                        const PadSource &p = half == 0 ? a.pad[k] : a.padHigh[k];
                                        if (p.type != PadType::Button || p.index != event.index) continue;
                                        held = uint16_t((i + 1) | (half == 1 ? 0x8000u : 0));
                                        press(i, half == 1, source, emit, arg);
                                        return true;
                                }
                }
                return false;
        }
        if (event.type != PadEvent::Type::Axis || event.index >= PadState::AxisCount) return false;
        int16_t      &last = _padAxes[event.pad][event.index];
        const int32_t was = last, now = event.value;
        last = int16_t(now);
        bool matched = false;
        for (size_t i = 0; i < _count; ++i) {
                const Action &a = _actions[i];
                if (!inSet(a)) continue;
                for (size_t k = 0; k < MaxPad; ++k)
                        for (int half = 0; half < 2; ++half) {
                                const PadSource &p = half == 0 ? a.pad[k] : a.padHigh[k];
                                if (p.index != event.index || p.type == PadType::None || p.type == PadType::Button)
                                        continue;
                                matched = true;
                                if (p.type == PadType::Axis) {
                                        _states[i].stick = int16_t(now);
                                        tellAxis(i, source, emit, arg);
                                } else if (!past(p.type, was) && past(p.type, now)) {
                                        press(i, half == 1, source, emit, arg);
                                } else if (past(p.type, was) && !past(p.type, now)) {
                                        release(i, half == 1, source, emit, arg);
                                }
                        }
        }
        return matched;
}

void Controls::releaseAll(Emit emit, void *arg) {
        for (size_t i = 0; i < _count; ++i) {
                State &s = _states[i];
                if (_actions[i].kind == Kind::Button && s.down > 0)
                        emit(arg, Output{Output::Type::Release, _actions[i].id, 0, 0});
                if (_actions[i].kind == Kind::Axis && s.out != 0)
                        emit(arg, Output{Output::Type::Axis, _actions[i].id, 0, 0});
                s = State();
        }
        for (Held &h : _held) h = Held();
        std::memset(_padButtons, 0, sizeof(_padButtons));
        return;
}

Error Controls::bindingText(uint16_t id, bool pad, char *out, size_t size, size_t &length) const {
        const Action *a = nullptr;
        for (size_t i = 0; i < _count; ++i)
                if (_actions[i].id == id) a = &_actions[i];
        if (a == nullptr) return Error::NotExist;
        size_t used = 0;
        if (size != 0) out[0] = '\0';
        auto add = [&](const char *s) {
                if (size == 0) return;
                const int w = std::snprintf(out + used, size - used, "%s", s);
                if (w > 0) used = used + size_t(w) < size ? used + size_t(w) : size - 1;
        };
        const bool axis = a->kind == Kind::Axis;
        for (size_t k = 0; k < (pad ? MaxPad : MaxKeys); ++k) {
                char text[2 * Chord::MaxText + 2];
                if (pad) {
                        if (a->pad[k].type == PadType::None) continue;
                        size_t n = formatPad(text, sizeof(text), a->pad[k]);
                        if (axis && a->padHigh[k].type != PadType::None && n + 1 < sizeof(text)) {
                                text[n++] = ' ';
                                (void)formatPad(text + n, sizeof(text) - n, a->padHigh[k]);
                        }
                } else {
                        if (!a->keys[k].isValid()) continue;
                        size_t n = a->keys[k].format(text, Chord::MaxText);
                        if (axis && a->keysHigh[k].isValid()) {
                                text[n++] = ' ';
                                (void)a->keysHigh[k].format(text + n, Chord::MaxText);
                        }
                }
                if (used != 0) add("; ");
                add(text);
        }
        length = used;
        return Error::Ok;
}

BMDS_NAMESPACE_END
