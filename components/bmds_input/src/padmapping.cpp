/**
 * @file      padmapping.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The standard pad, and mappings to it in SDL's format (devplan/controls.md §4a, §7a).
 */

#include <cstdio>
#include <bmds/padmapping.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Kind = PadMapping::Source::Kind;

        constexpr int16_t DeadZone = 32767 / 8;    // a stick nearer the centre reads as centred
        constexpr int16_t ButtonPast = 16384;      // an axis read as a button: down past half
        constexpr int16_t TriggerPast = 32767 / 4; // a trigger's button: down past a quarter

        // A mapping's targets, by SDL's names (sorted: format() writes them in this order).
        struct Target {
                        const char *name;
                        bool        isAxis;
                        uint8_t     index;
        };

        constexpr Target Targets[] = {
                {"a", false, PadState::South},
                {"b", false, PadState::East},
                {"back", false, PadState::Select},
                {"dpdown", false, PadState::Down},
                {"dpleft", false, PadState::Left},
                {"dpright", false, PadState::Right},
                {"dpup", false, PadState::Up},
                {"guide", false, PadState::Guide},
                {"leftshoulder", false, PadState::L1},
                {"leftstick", false, PadState::L3},
                {"lefttrigger", true, PadState::LeftTrigger},
                {"leftx", true, PadState::LeftX},
                {"lefty", true, PadState::LeftY},
                {"rightshoulder", false, PadState::R1},
                {"rightstick", false, PadState::R3},
                {"righttrigger", true, PadState::RightTrigger},
                {"rightx", true, PadState::RightX},
                {"righty", true, PadState::RightY},
                {"start", false, PadState::Start},
                {"x", false, PadState::West},
                {"y", false, PadState::North},
        };

        constexpr const char *ButtonNames[PadState::ButtonCount] = {
                "South", "East", "West", "North", "L1",    "R1",    "L2",     "R2",    "L3",
                "R3",    "Up",   "Down", "Left",  "Right", "Start", "Select", "Guide",
        };

        constexpr const char *AxisNames[PadState::AxisCount] = {"LeftX", "LeftY", "RightX", "RightY", "L2", "R2"};

        // A decimal number of @p text from @p at; false if none.
        bool number(StringView text, size_t &at, uint32_t limit, uint8_t &out) {
                uint32_t v = 0;
                size_t   digits = 0;
                while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 3) {
                        v = v * 10 + uint32_t(text[at++] - '0');
                        ++digits;
                }
                if (digits == 0 || v >= limit) return false;
                out = static_cast<uint8_t>(v);
                return true;
        }

        // `bN`, `aN`, `aN~`, `+aN`, `-aN`, `hN.M`.
        bool parseSource(StringView text, PadMapping::Source &out) {
                PadMapping::Source s;
                size_t             at = 0;
                if (text.startsWith('+') || text.startsWith('-')) {
                        s.kind = text[0] == '+' ? Kind::AxisPlus : Kind::AxisMinus;
                        ++at;
                        if (at >= text.size() || text[at] != 'a') return false;
                }
                if (at >= text.size()) return false;
                const char type = text[at++];
                if (type == 'b') {
                        if (s.kind != Kind::None || !number(text, at, RawPad::MaxButtons, s.index)) return false;
                        s.kind = Kind::Button;
                } else if (type == 'a') {
                        if (!number(text, at, RawPad::MaxAxes, s.index)) return false;
                        if (s.kind == Kind::None) s.kind = Kind::Axis;
                        if (at < text.size() && text[at] == '~') {
                                s.invert = true;
                                ++at;
                        }
                } else if (type == 'h') {
                        if (s.kind != Kind::None || !number(text, at, RawPad::MaxHats, s.index)) return false;
                        if (at >= text.size() || text[at++] != '.') return false;
                        if (!number(text, at, 16, s.bits) || s.bits == 0) return false;
                        s.kind = Kind::Hat;
                } else {
                        return false;
                }
                if (at != text.size()) return false;
                out = s;
                return true;
        }

        int formatSource(char *out, size_t size, const PadMapping::Source &s) {
                switch (s.kind) {
                        case Kind::Button: return std::snprintf(out, size, "b%u", s.index);
                        case Kind::Axis: return std::snprintf(out, size, "a%u%s", s.index, s.invert ? "~" : "");
                        case Kind::AxisPlus:
                        case Kind::AxisMinus:
                                return std::snprintf(out, size, "%ca%u%s", s.kind == Kind::AxisPlus ? '+' : '-',
                                                     s.index, s.invert ? "~" : "");
                        case Kind::Hat: return std::snprintf(out, size, "h%u.%u", s.index, s.bits);
                        case Kind::None: break;
                }
                return 0;
        }

        // A source as a button.
        bool isDown(const RawPad &raw, const PadMapping::Source &s) {
                switch (s.kind) {
                        case Kind::Button: return (raw.buttons >> s.index & 1u) != 0;
                        case Kind::Axis:
                                return (s.invert ? -int32_t(raw.axes[s.index]) - 1 : raw.axes[s.index]) > ButtonPast;
                        case Kind::AxisPlus: return raw.axes[s.index] > ButtonPast;
                        case Kind::AxisMinus: return raw.axes[s.index] < -ButtonPast;
                        case Kind::Hat: return (raw.hats[s.index] & s.bits) == s.bits;
                        case Kind::None: break;
                }
                return false;
        }

        // A source as a stick's axis: −32767 to 32767.
        int32_t stick(const RawPad &raw, const PadMapping::Source &s) {
                int32_t v = 0;
                switch (s.kind) {
                        case Kind::Axis: v = raw.axes[s.index]; break;
                        case Kind::AxisPlus: v = (raw.axes[s.index] > 0 ? raw.axes[s.index] : 0) * 2 - 32767; break;
                        case Kind::AxisMinus:
                                v = (raw.axes[s.index] < 0 ? -int32_t(raw.axes[s.index]) - 1 : 0) * 2 - 32767;
                                break;
                        case Kind::Button:
                        case Kind::Hat: return isDown(raw, s) ? 32767 : 0;
                        case Kind::None: return 0;
                }
                if (s.invert) v = -v - 1;
                if (v < -32767) v = -32767;
                if (v > 32767) v = 32767;
                return v > -DeadZone && v < DeadZone ? 0 : v;
        }

        // A source as a trigger: 0 to 32767.
        int32_t trigger(const RawPad &raw, const PadMapping::Source &s) {
                int32_t v = 0;
                switch (s.kind) {
                        case Kind::Axis: v = (int32_t(raw.axes[s.index]) + 32768) >> 1; break;
                        case Kind::AxisPlus: v = raw.axes[s.index] > 0 ? raw.axes[s.index] : 0; break;
                        case Kind::AxisMinus: v = raw.axes[s.index] < 0 ? -int32_t(raw.axes[s.index]) - 1 : 0; break;
                        case Kind::Button:
                        case Kind::Hat: return isDown(raw, s) ? 32767 : 0;
                        case Kind::None: return 0;
                }
                if (s.invert) v = 32767 - v;
                return v < 0 ? 0 : v > 32767 ? 32767 : v;
        }

        constexpr PadMapping::Source buttonSource(uint8_t index) {
                return PadMapping::Source{Kind::Button, index, 0, false};
        }

} // namespace

const char *PadState::buttonName(Button button) {
        return button < ButtonCount ? ButtonNames[button] : "?";
}

const char *PadState::axisName(Axis axis) {
        return axis < AxisCount ? AxisNames[axis] : "?";
}

PadMapping PadMapping::standard(const PadLayout &layout) {
        PadMapping m;
        // Buttons 1-4 the face, then the shoulders, the triggers, Select, Start, the sticks', Guide.
        constexpr PadState::Button Order[] = {
                PadState::South, PadState::East, PadState::West,  PadState::North,  PadState::L1,
                PadState::R1,    PadState::L2,   PadState::R2,    PadState::Select, PadState::Start,
                PadState::L3,    PadState::R3,   PadState::Guide,
        };
        for (size_t i = 0; i < sizeof(Order) / sizeof(Order[0]) && i < layout.buttonCount(); ++i) {
                if (Order[i] == PadState::L2) {
                        m._axes[PadState::LeftTrigger] = buttonSource(uint8_t(i));
                } else if (Order[i] == PadState::R2) {
                        m._axes[PadState::RightTrigger] = buttonSource(uint8_t(i));
                } else {
                        m._buttons[Order[i]] = buttonSource(uint8_t(i));
                }
        }
        // X and Y the left stick; Z and Rz the right, or Rx and Ry where there is no Rz.
        int x = -1, y = -1, z = -1, rx = -1, ry = -1, rz = -1;
        for (size_t i = 0; i < layout.axisCount(); ++i) {
                int *slot = nullptr;
                switch (layout.axisUsage(i)) {
                        case 0x30: slot = &x; break;
                        case 0x31: slot = &y; break;
                        case 0x32: slot = &z; break;
                        case 0x33: slot = &rx; break;
                        case 0x34: slot = &ry; break;
                        case 0x35: slot = &rz; break;
                        default: break;
                }
                if (slot != nullptr && *slot < 0) *slot = int(i);
        }
        auto axis = [](int index) {
                return index < 0 ? Source() : Source{Kind::Axis, uint8_t(index), 0, false};
        };
        m._axes[PadState::LeftX] = axis(x);
        m._axes[PadState::LeftY] = axis(y);
        m._axes[PadState::RightX] = axis(rz >= 0 ? z : rx);
        m._axes[PadState::RightY] = axis(rz >= 0 ? rz : ry);
        if (layout.hatCount() > 0) {
                m._buttons[PadState::Up] = Source{Kind::Hat, 0, RawPad::HatUp, false};
                m._buttons[PadState::Right] = Source{Kind::Hat, 0, RawPad::HatRight, false};
                m._buttons[PadState::Down] = Source{Kind::Hat, 0, RawPad::HatDown, false};
                m._buttons[PadState::Left] = Source{Kind::Hat, 0, RawPad::HatLeft, false};
        }
        return m;
}

Error PadMapping::parse(StringView fields, PadMapping &out) {
        PadMapping m;
        size_t     at = 0;
        while (at < fields.size()) {
                size_t comma = fields.indexOf(',', at);
                if (comma == StringView::NPos) comma = fields.size();
                const StringView field = fields.mid(at, comma - at).trimmed();
                at = comma + 1;
                const size_t colon = field.indexOf(':');
                if (colon == StringView::NPos) continue;
                const StringView name = field.left(colon), source = field.mid(colon + 1);
                for (const Target &t : Targets) {
                        if (name != StringView(t.name)) continue;
                        Source s;
                        if (!parseSource(source, s)) return Error::Invalid;
                        (t.isAxis ? m._axes : m._buttons)[t.index] = s;
                        break;
                }
        }
        out = m;
        return Error::Ok;
}

bool PadMapping::find(StringView database, uint16_t vendor, uint16_t product, PadMapping &out, StringView *name) {
        auto hex = [](StringView text, size_t at, uint32_t &v) {
                v = 0;
                for (size_t i = at; i < at + 2; ++i) {
                        const char c = text[i];
                        const int  d = c >= '0' && c <= '9'   ? c - '0'
                                       : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                       : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                              : -1;
                        if (d < 0) return false;
                        v = v << 4 | uint32_t(d);
                }
                return true;
        };
        size_t at = 0;
        while (at < database.size()) {
                size_t newline = database.indexOf('\n', at);
                if (newline == StringView::NPos) newline = database.size();
                const StringView line = database.mid(at, newline - at).trimmed();
                at = newline + 1;
                // GUID: bus (2 bytes), CRC (2), vendor (2), 0 (2), product (2), 0 (2), version...
                if (line.size() < 33 || line.startsWith('#') || line[32] != ',') continue;
                uint32_t b[10];
                bool     ok = true;
                for (size_t i = 0; i < 10 && ok; ++i) ok = hex(line, i * 2, b[i]);
                if (!ok || (b[4] | b[5] << 8) != vendor || (b[8] | b[9] << 8) != product) continue;
                const StringView rest = line.mid(33);
                const size_t     comma = rest.indexOf(',');
                if (comma == StringView::NPos) continue;
                const StringView fields = rest.mid(comma + 1);
                const size_t     platform = fields.indexOf("platform:");
                if (platform != StringView::NPos) {
                        StringView   p = fields.mid(platform + 9);
                        const size_t end = p.indexOf(',');
                        if (end != StringView::NPos) p = p.left(end);
                        if (p.trimmed() != StringView("BMDS")) continue;
                }
                if (parse(fields, out).isError()) continue;
                if (name != nullptr) *name = rest.left(comma);
                return true;
        }
        return false;
}

size_t PadMapping::format(char *out, size_t size) const {
        if (size == 0) return 0;
        size_t used = 0;
        out[0] = '\0';
        for (const Target &t : Targets) {
                const Source &s = (t.isAxis ? _axes : _buttons)[t.index];
                if (s.kind == Kind::None || used + 1 >= size) continue;
                char      text[12];
                const int n = formatSource(text, sizeof(text), s);
                const int w = std::snprintf(out + used, size - used, "%s:%.*s,", t.name, n, text);
                if (w < 0) break;
                used = used + size_t(w) < size ? used + size_t(w) : size - 1;
        }
        return used;
}

const PadMapping::Source &PadMapping::button(PadState::Button button) const {
        if (button == PadState::L2) return _axes[PadState::LeftTrigger];
        if (button == PadState::R2) return _axes[PadState::RightTrigger];
        return _buttons[button];
}

void PadMapping::apply(const RawPad &raw, PadState &state) const {
        uint32_t buttons = 0;
        for (size_t i = 0; i < PadState::ButtonCount; ++i)
                if (_buttons[i].kind != Kind::None && isDown(raw, _buttons[i])) buttons |= 1u << i;
        for (size_t i = PadState::LeftX; i <= PadState::RightY; ++i)
                state.axes[i] = static_cast<int16_t>(stick(raw, _axes[i]));
        for (size_t i = PadState::LeftTrigger; i <= PadState::RightTrigger; ++i)
                state.axes[i] = static_cast<int16_t>(trigger(raw, _axes[i]));
        if (state.axes[PadState::LeftTrigger] > TriggerPast) buttons |= 1u << PadState::L2;
        if (state.axes[PadState::RightTrigger] > TriggerPast) buttons |= 1u << PadState::R2;
        state.buttons = buttons;
        return;
}

BMDS_NAMESPACE_END
