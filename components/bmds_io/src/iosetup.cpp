/**
 * @file      iosetup.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `io/gpio`'s value, parsed (devplan/io.md §10.1).
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <bmds/iosetup.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Why = char[IoGpioSetup::ReasonBytes];

        [[gnu::format(printf, 3, 4)]] Error refuse(Why &why, Error err, const char *format, ...) {
                va_list args;
                va_start(args, format);
                std::vsnprintf(why, sizeof(why), format, args);
                va_end(args);
                return err;
        }

        bool number(StringView text, unsigned &out) {
                if (text.isEmpty() || text.size() > 4) return false;
                unsigned n = 0;
                for (const char c : text) {
                        if (c < '0' || c > '9') return false;
                        n = n * 10 + unsigned(c - '0');
                }
                out = n;
                return true;
        }

        // `14` or `GPIO14` (any case).
        bool padNumber(StringView text, unsigned &out) {
                if (text.size() > 4 && StringView(text.data(), 4).equalsIgnoreCase("GPIO"))
                        text = StringView(text.data() + 4, text.size() - 4);
                return number(text, out);
        }

        // The next piece of @p text up to @p separator (`text` left after it).
        StringView take(StringView &text, char separator) {
                const size_t     at = text.indexOf(separator);
                const StringView piece = at == StringView::NPos ? text : StringView(text.data(), at);
                text = at == StringView::NPos ? StringView() : StringView(text.data() + at + 1, text.size() - at - 1);
                return piece;
        }

} // namespace

bool IoPinSetup::operator==(const IoPinSetup &other) const {
        if (pad != other.pad || mode != other.mode || high != other.high || pullUp != other.pullUp ||
            pullDown != other.pullDown || force != other.force || drive != other.drive || function != other.function ||
            outSignal != other.outSignal || inputCount != other.inputCount || edges != other.edges ||
            debounceUs != other.debounceUs)
                return false;
        for (size_t i = 0; i < inputCount; ++i)
                if (inputs[i] != other.inputs[i]) return false;
        return true;
}

const IoPinSetup *IoGpioSetup::find(unsigned pad) const {
        for (size_t i = 0; i < _count; ++i)
                if (_pins[i].pad == pad) return &_pins[i];
        return nullptr;
}

Error IoGpioSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        _count = 0;
        while (!text.isEmpty()) {
                StringView entry = take(text, ' ');
                if (entry.isEmpty()) continue; // spaces between entries
                const StringView padText = take(entry, '=');
                unsigned         pad = 0;
                if (!padNumber(padText, pad) || pad >= chip.padCount())
                        return refuse(why, Error::Invalid, "'%.*s' isn't a pin of the chip's", int(padText.size()),
                                      padText.data());
                char name[16];
                std::snprintf(name, sizeof(name), "GPIO%u", pad);
                if (find(pad) != nullptr) return refuse(why, Error::Exists, "%s is set up twice", name);
                if (_count == MaxPins) return refuse(why, Error::Full, "too many pins");
                IoPinSetup &p = _pins[_count];
                p = IoPinSetup();
                p.pad = uint8_t(pad);
                while (!entry.isEmpty()) {
                        const StringView word = take(entry, ',');
                        if (word == "in") {
                                p.mode = IoPinSetup::Mode::Input;
                        } else if (word == "out") {
                                p.mode = IoPinSetup::Mode::Output;
                        } else if (word == "od") {
                                p.mode = IoPinSetup::Mode::OpenDrain;
                        } else if (word == "off") {
                                p.mode = IoPinSetup::Mode::Off;
                        } else if (word == "high" || word == "1") {
                                p.high = true;
                        } else if (word == "low" || word == "0") {
                                p.high = false;
                        } else if (word == "pull-up") {
                                p.pullUp = true;
                        } else if (word == "pull-down") {
                                p.pullDown = true;
                        } else if (word == "force") {
                                p.force = true;
                        } else if (word == "edges") {
                                p.edges = IoPinSetup::Rising | IoPinSetup::Falling;
                        } else if (word == "rising") {
                                p.edges |= IoPinSetup::Rising;
                        } else if (word == "falling") {
                                p.edges |= IoPinSetup::Falling;
                        } else if (word.startsWith("debounce=")) {
                                const StringView v = word.mid(9);
                                // Up to seven digits (number() takes four).
                                uint32_t us = 0;
                                bool     ok = !v.isEmpty() && v.size() <= 7;
                                for (size_t k = 0; ok && k < v.size(); ++k) {
                                        ok = v[k] >= '0' && v[k] <= '9';
                                        us = us * 10 + uint32_t(v[k] - '0');
                                }
                                if (!ok || us > IoPinSetup::MaxDebounceUs)
                                        return refuse(why, Error::Invalid,
                                                      "%s: debounce is 0 to 1000000 us, not '%.*s'", name,
                                                      int(v.size()), v.data());
                                p.debounceUs = us;
                        } else if (word.startsWith("drive=")) {
                                unsigned d = 0;
                                if (!number(word.mid(6), d) || d > 3)
                                        return refuse(why, Error::Invalid, "%s: drive is 0 to 3, not '%.*s'", name,
                                                      int(word.size() - 6), word.data() + 6);
                                p.drive = uint8_t(d);
                        } else if (word.startsWith("func=")) {
                                const StringView     f = word.mid(5);
                                unsigned             index = 0;
                                const IoMuxFunction *found = nullptr;
                                if (number(f, index) && index < 256) {
                                        found = chip.function(pad, uint8_t(index));
                                } else {
                                        for (const IoMuxFunction &fn : chip.padFunctions(pad))
                                                if (f.equalsIgnoreCase(StringView(fn.name))) found = &fn;
                                }
                                if (found == nullptr)
                                        return refuse(why, Error::Invalid,
                                                      "%s has no IO_MUX function '%.*s' ('io pin' lists them)", name,
                                                      int(f.size()), f.data());
                                p.function = found->index == chip.gpioFunction ? -1 : int16_t(found->index);
                        } else if (word.startsWith("out:")) {
                                auto [signal, err] = chip.findOutput(word.mid(4));
                                if (err.isError() || signal == chip.gpioOutputSignal)
                                        return refuse(why, Error::Invalid,
                                                      "%s: no output signal '%.*s' ('io signals' lists them)", name,
                                                      int(word.size() - 4), word.data() + 4);
                                p.outSignal = signal;
                        } else if (word.startsWith("in:")) {
                                auto [signal, err] = chip.findInput(word.mid(3));
                                if (err.isError())
                                        return refuse(why, Error::Invalid,
                                                      "%s: no input signal '%.*s' ('io signals' lists them)", name,
                                                      int(word.size() - 3), word.data() + 3);
                                for (size_t e = 0; e <= _count; ++e) {
                                        const IoPinSetup &q = _pins[e];
                                        for (size_t i = 0; i < q.inputCount; ++i)
                                                if (q.inputs[i] == signal)
                                                        return refuse(why, Error::Exists,
                                                                      "input signal %.*s is read from one pin only",
                                                                      int(word.size() - 3), word.data() + 3);
                                }
                                if (p.inputCount == IoPinSetup::MaxInputs)
                                        return refuse(why, Error::Full, "%s: at most 4 input signals", name);
                                p.inputs[p.inputCount++] = signal;
                        } else {
                                return refuse(why, Error::Invalid, "%s: '%.*s' isn't a word io/gpio knows", name,
                                              int(word.size()), word.data());
                        }
                }
                if (p.function >= 0 && (p.outSignal != IoPinSetup::NoSignal))
                        return refuse(why, Error::Invalid, "%s: an IO_MUX function or a matrix output, not both", name);
                if (p.outSignal != IoPinSetup::NoSignal && !p.isOutput())
                        return refuse(why, Error::Invalid, "%s: a matrix output needs out or od", name);
                if (p.edges != IoPinSetup::NoEdges && !p.readsInput())
                        return refuse(why, Error::Invalid, "%s: edges need its input on (in, od, in:SIGNAL)", name);
                if (p.debounceUs != 0 && p.edges == IoPinSetup::NoEdges)
                        return refuse(why, Error::Invalid, "%s: debounce is for edges (edges, rising, falling)", name);
                ++_count;
        }
        return Error::Ok;
}

BMDS_NAMESPACE_END
