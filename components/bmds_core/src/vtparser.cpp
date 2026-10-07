/**
 * @file      vtparser.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/vtparser.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t Esc = 0x1B;
        constexpr uint8_t Can = 0x18;
        constexpr uint8_t Sub = 0x1A;
        constexpr uint8_t Bel = 0x07;
        constexpr uint8_t Del = 0x7F;

        bool isC0(uint8_t b) {
                return b < 0x20;
        }

} // namespace

void VtParser::clear() {
        _params = VtParams();
        _paramStarted = false;
        _intermediateCount = 0;
        return;
}

void VtParser::reset() {
        if (_state == State::OscStream) endStream(false);
        clear();
        _utf8.reset();
        _oscLength = 0;
        _state = State::Ground;
        return;
}

void VtParser::collect(uint8_t b) {
        if (_intermediateCount < MaxIntermediates) _intermediates[_intermediateCount++] = static_cast<char>(b);
        return;
}

void VtParser::param(uint8_t b) {
        if (!_paramStarted) {
                _paramStarted = true;
                _params.count = 1;
        }
        if (b == ';' || b == ':') {
                if (_params.count < VtParams::MaxParams) {
                        if (b == ':') _params.subparams |= static_cast<uint16_t>(1u << _params.count);
                        ++_params.count;
                }
                return;
        }
        uint16_t      &v = _params.values[_params.count - 1];
        const uint32_t next = v * 10u + (b - '0');
        v = next > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(next);
        return;
}

// A streamed OSC string's last piece, then its end.
void VtParser::endStream(bool complete) {
        if (complete && _oscLength > 0) _handler.oscData(StringView(_osc, _oscLength));
        _oscLength = 0;
        _handler.oscEnd(complete);
        return;
}

void VtParser::dispatchCsi(uint8_t final) {
        _handler.csiDispatch(_params, StringView(_intermediates, _intermediateCount), final);
        _state = State::Ground;
        return;
}

void VtParser::feed(uint8_t b) {
        // Anywhere: CAN and SUB abort; ESC starts over (and ends an OSC string).
        if (b == Can || b == Sub) {
                if (_state != State::Ground) {
                        if (_state == State::OscStream) endStream(false);
                        _handler.execute(b);
                        _state = State::Ground;
                        return;
                }
        }
        if (b == Esc && _state != State::Ground) {
                if (_state == State::OscString) _handler.oscDispatch(StringView(_osc, _oscLength));
                if (_state == State::OscStream) endStream(true); // ESC: its end (ST)
                clear();
                _state = State::Escape;
                return;
        }

        switch (_state) {
                case State::Ground: {
                        if (b == Esc) {
                                if (_utf8.isPending()) {
                                        _utf8.reset();
                                        _handler.print(Utf8::Replacement);
                                }
                                clear();
                                _state = State::Escape;
                                return;
                        }
                        if (isC0(b) && !_utf8.isPending()) {
                                _handler.execute(b);
                                return;
                        }
                        if (b == Del && !_utf8.isPending()) return;
                        uint32_t     out[2];
                        const size_t n = _utf8.feed(b, out);
                        for (size_t i = 0; i < n; ++i) {
                                // A control that cut a sequence short is still a control.
                                if (out[i] < 0x20) {
                                        _handler.execute(static_cast<uint8_t>(out[i]));
                                } else if (out[i] != Del) {
                                        _handler.print(out[i]);
                                }
                        }
                        return;
                }

                case State::Escape:
                        if (isC0(b)) {
                                _handler.execute(b);
                        } else if (b >= 0x20 && b <= 0x2F) {
                                collect(b);
                                _state = State::EscapeIntermediate;
                        } else if (b == '[') {
                                _state = State::CsiEntry;
                        } else if (b == ']') {
                                _oscLength = 0;
                                _oscAsked = false;
                                _state = State::OscString;
                        } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
                                _state = State::IgnoredString;
                        } else if (b >= 0x30 && b <= 0x7E) {
                                _handler.escDispatch(StringView(), b);
                                _state = State::Ground;
                        }
                        return;

                case State::EscapeIntermediate:
                        if (isC0(b)) {
                                _handler.execute(b);
                        } else if (b >= 0x20 && b <= 0x2F) {
                                collect(b);
                        } else if (b >= 0x30 && b <= 0x7E) {
                                _handler.escDispatch(StringView(_intermediates, _intermediateCount), b);
                                _state = State::Ground;
                        }
                        return;

                case State::CsiEntry:
                case State::CsiParam:
                        if (isC0(b)) {
                                _handler.execute(b);
                        } else if ((b >= '0' && b <= '9') || b == ';' || b == ':') {
                                param(b);
                                _state = State::CsiParam;
                        } else if (b >= 0x3C && b <= 0x3F) {
                                if (_state == State::CsiEntry) {
                                        _params.privateMarker = b;
                                        _state = State::CsiParam;
                                } else {
                                        _state = State::CsiIgnore;
                                }
                        } else if (b >= 0x20 && b <= 0x2F) {
                                collect(b);
                                _state = State::CsiIntermediate;
                        } else if (b >= 0x40 && b <= 0x7E) {
                                dispatchCsi(b);
                        }
                        return;

                case State::CsiIntermediate:
                        if (isC0(b)) {
                                _handler.execute(b);
                        } else if (b >= 0x20 && b <= 0x2F) {
                                collect(b);
                        } else if (b >= 0x30 && b <= 0x3F) {
                                _state = State::CsiIgnore;
                        } else if (b >= 0x40 && b <= 0x7E) {
                                dispatchCsi(b);
                        }
                        return;

                case State::CsiIgnore:
                        if (isC0(b)) {
                                _handler.execute(b);
                        } else if (b >= 0x40 && b <= 0x7E) {
                                _state = State::Ground;
                        }
                        return;

                case State::OscString:
                        if (b == Bel) {
                                _handler.oscDispatch(StringView(_osc, _oscLength));
                                _state = State::Ground;
                        } else if (b == ';' && !_oscAsked) {
                                // The number: the handler may take the rest in pieces.
                                _oscAsked = true;
                                uint32_t number = 0;
                                bool     digits = _oscLength > 0 && _oscLength <= 9;
                                for (size_t i = 0; i < _oscLength && digits; ++i) {
                                        digits = _osc[i] >= '0' && _osc[i] <= '9';
                                        number = number * 10 + uint32_t(_osc[i] - '0');
                                }
                                if (digits && _handler.oscStream(number)) {
                                        _oscLength = 0;
                                        _state = State::OscStream;
                                } else if (_oscLength < MaxOsc) {
                                        _osc[_oscLength++] = ';';
                                }
                        } else if (b >= 0x20 && _oscLength < MaxOsc) {
                                _osc[_oscLength++] = static_cast<char>(b);
                        }
                        return;

                case State::OscStream:
                        if (b == Bel) {
                                endStream(true);
                                _state = State::Ground;
                        } else if (b >= 0x20) {
                                _osc[_oscLength++] = static_cast<char>(b);
                                if (_oscLength == MaxOsc) {
                                        _handler.oscData(StringView(_osc, _oscLength));
                                        _oscLength = 0;
                                }
                        }
                        return;

                case State::IgnoredString: return; // until ESC (ST)
        }
        return;
}

// ---------------------------------------------------------------------------
// SGR
// ---------------------------------------------------------------------------

namespace {

        // Parses the color after 38/48 at params[i]; advances i past it.
        bool extendedColor(const VtParams &p, size_t &i, Color &out) {
                const bool colon = p.isSub(i + 1);
                if (i + 1 >= p.count) return false;
                const uint16_t mode = p.values[i + 1];
                if (mode == 5 && i + 2 < p.count) {
                        out = Color::indexed(static_cast<uint8_t>(p.values[i + 2]));
                        i += 2;
                        return true;
                }
                if (mode == 2) {
                        // `38:2:cs:r:g:b` has a color-space slot that `38;2;r;g;b` lacks.
                        size_t first = i + 2;
                        if (colon) {
                                size_t subs = 0;
                                while (i + 2 + subs < p.count && p.isSub(i + 2 + subs)) ++subs;
                                if (subs >= 4) ++first;
                        }
                        if (first + 2 >= p.count) return false;
                        out = Color::rgb(static_cast<uint8_t>(p.values[first]),
                                         static_cast<uint8_t>(p.values[first + 1]),
                                         static_cast<uint8_t>(p.values[first + 2]));
                        i = first + 2;
                        return true;
                }
                return false;
        }

        class Builder {
                public:
                        explicit Builder(char (&out)[Sgr::MaxSequence]) : _out(out) {}

                        void number(unsigned n) {
                                char   digits[6];
                                size_t k = 0;
                                do {
                                        digits[k++] = static_cast<char>('0' + n % 10);
                                        n /= 10;
                                } while (n != 0 && k < sizeof(digits));
                                if (_any) put(';');
                                _any = true;
                                while (k > 0) put(digits[--k]);
                                return;
                        }

                        void color(const Color &c, bool background) {
                                const unsigned base = background ? 40 : 30;
                                switch (c.kind()) {
                                        case Color::Kind::Default: number(base + 9); break;
                                        case Color::Kind::Indexed:
                                                if (c.index() < 8) {
                                                        number(base + c.index());
                                                } else if (c.index() < 16) {
                                                        number(base + 60 + c.index() - 8);
                                                } else {
                                                        number(base + 8);
                                                        number(5);
                                                        number(c.index());
                                                }
                                                break;
                                        case Color::Kind::Rgb:
                                                number(base + 8);
                                                number(2);
                                                number(c.red());
                                                number(c.green());
                                                number(c.blue());
                                                break;
                                }
                                return;
                        }

                        // Wraps the parameters as ESC [ ... m; returns the length.
                        size_t finish(char (&dest)[Sgr::MaxSequence]) {
                                if (!_any) return 0;
                                size_t n = 0;
                                dest[n++] = '\x1b';
                                dest[n++] = '[';
                                for (size_t k = 0; k < _length && n < Sgr::MaxSequence - 1; ++k) dest[n++] = _out[k];
                                dest[n++] = 'm';
                                return n;
                        }

                        size_t length() const { return _any ? _length + 3 : 0; }

                private:
                        void put(char c) {
                                if (_length < Sgr::MaxSequence - 4) _out[_length++] = c;
                                return;
                        }

                        char (&_out)[Sgr::MaxSequence];
                        size_t _length = 0;
                        bool   _any = false;
        };

        constexpr uint8_t OnCodes[8] = {1, 2, 3, 4, 5, 7, 8, 9};
        constexpr uint8_t OffCodes[8] = {22, 22, 23, 24, 25, 27, 28, 29};

} // namespace

void Sgr::apply(const VtParams &p, TextStyle &style) {
        if (p.count == 0) {
                style = TextStyle();
                return;
        }
        for (size_t i = 0; i < p.count; ++i) {
                if (p.isSub(i)) continue; // a sub-parameter this code doesn't use
                const uint16_t v = p.values[i];
                switch (v) {
                        case 0: style = TextStyle(); break;
                        case 1: style.attributes |= TextStyle::Bold; break;
                        case 2: style.attributes |= TextStyle::Dim; break;
                        case 3: style.attributes |= TextStyle::Italic; break;
                        case 4:
                                // 4:0 is "no underline"; other 4:n are underline styles.
                                if (p.isSub(i + 1) && p.values[i + 1] == 0) {
                                        style.attributes &= static_cast<uint8_t>(~TextStyle::Underline);
                                } else {
                                        style.attributes |= TextStyle::Underline;
                                }
                                break;
                        case 5:
                        case 6: style.attributes |= TextStyle::Blink; break;
                        case 7: style.attributes |= TextStyle::Reverse; break;
                        case 8: style.attributes |= TextStyle::Invisible; break;
                        case 9: style.attributes |= TextStyle::Strike; break;
                        case 21: style.attributes |= TextStyle::Underline; break;
                        case 22: style.attributes &= static_cast<uint8_t>(~(TextStyle::Bold | TextStyle::Dim)); break;
                        case 23: style.attributes &= static_cast<uint8_t>(~TextStyle::Italic); break;
                        case 24: style.attributes &= static_cast<uint8_t>(~TextStyle::Underline); break;
                        case 25: style.attributes &= static_cast<uint8_t>(~TextStyle::Blink); break;
                        case 27: style.attributes &= static_cast<uint8_t>(~TextStyle::Reverse); break;
                        case 28: style.attributes &= static_cast<uint8_t>(~TextStyle::Invisible); break;
                        case 29: style.attributes &= static_cast<uint8_t>(~TextStyle::Strike); break;
                        case 38:
                        case 48: {
                                Color c;
                                if (extendedColor(p, i, c)) (v == 38 ? style.foreground : style.background) = c;
                                break;
                        }
                        case 39: style.foreground = Color(); break;
                        case 49: style.background = Color(); break;
                        default:
                                if (v >= 30 && v <= 37) style.foreground = Color::indexed(static_cast<uint8_t>(v - 30));
                                if (v >= 40 && v <= 47) style.background = Color::indexed(static_cast<uint8_t>(v - 40));
                                if (v >= 90 && v <= 97)
                                        style.foreground = Color::indexed(static_cast<uint8_t>(v - 90 + 8));
                                if (v >= 100 && v <= 107)
                                        style.background = Color::indexed(static_cast<uint8_t>(v - 100 + 8));
                                break;
                }
        }
        return;
}

size_t Sgr::transition(const TextStyle &fromStyle, const TextStyle &toStyle, ColorDepth depth,
                       char (&out)[MaxSequence]) {
        TextStyle from = fromStyle, to = toStyle;
        from.foreground = from.foreground.downgrade(depth);
        from.background = from.background.downgrade(depth);
        to.foreground = to.foreground.downgrade(depth);
        to.background = to.background.downgrade(depth);
        if (from == to) return 0;

        // Either change what differs...
        char    a[MaxSequence];
        Builder inc(a);
        uint8_t removed = from.attributes & static_cast<uint8_t>(~to.attributes);
        uint8_t added = to.attributes & static_cast<uint8_t>(~from.attributes);
        if (removed & (TextStyle::Bold | TextStyle::Dim)) {
                // 22 clears both bold and dim; put back the one that stays.
                inc.number(22);
                removed &= static_cast<uint8_t>(~(TextStyle::Bold | TextStyle::Dim));
                added |= to.attributes & (TextStyle::Bold | TextStyle::Dim);
        }
        for (int bit = 0; bit < 8; ++bit) {
                if (removed & (1u << bit)) inc.number(OffCodes[bit]);
        }
        for (int bit = 0; bit < 8; ++bit) {
                if (added & (1u << bit)) inc.number(OnCodes[bit]);
        }
        if (!(from.foreground == to.foreground)) inc.color(to.foreground, false);
        if (!(from.background == to.background)) inc.color(to.background, true);

        // ...or reset and set everything; whichever is shorter.
        char    b[MaxSequence];
        Builder full(b);
        full.number(0);
        for (int bit = 0; bit < 8; ++bit) {
                if (to.attributes & (1u << bit)) full.number(OnCodes[bit]);
        }
        if (!to.foreground.isDefault()) full.color(to.foreground, false);
        if (!to.background.isDefault()) full.color(to.background, true);

        Builder &best = full.length() < inc.length() ? full : inc;
        return best.finish(out);
}

BMDS_NAMESPACE_END
