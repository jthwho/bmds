/**
 * @file      ioport.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A port's setup, parsed (devplan/io.md §11.1).
 */

#include <cstdarg>
#include <cstdio>
#include <bmds/ioport.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Why = char[IoUartSetup::ReasonBytes];

        [[gnu::format(printf, 3, 4)]] Error refuse(Why &why, Error err, const char *format, ...) {
                va_list args;
                va_start(args, format);
                std::vsnprintf(why, sizeof(why), format, args);
                va_end(args);
                return err;
        }

        bool number(StringView text, uint32_t &out) {
                if (text.isEmpty() || text.size() > 9) return false;
                uint32_t n = 0;
                for (const char c : text) {
                        if (c < '0' || c > '9') return false;
                        n = n * 10 + uint32_t(c - '0');
                }
                out = n;
                return true;
        }

        // `14` or `GPIO14`.
        bool pad(const IoChip &chip, StringView text, int8_t &out) {
                if (text.size() > 4 && StringView(text.data(), 4).equalsIgnoreCase("GPIO")) text = text.mid(4);
                uint32_t n = 0;
                if (text.size() > 3 || !number(text, n) || n >= chip.padCount()) return false;
                out = int8_t(n);
                return true;
        }

        StringView take(StringView &text, char separator) {
                const size_t     at = text.indexOf(separator);
                const StringView piece = at == StringView::NPos ? text : text.left(at);
                text = at == StringView::NPos ? StringView() : text.mid(at + 1);
                return piece;
        }

} // namespace

bool IoUartSetup::operator==(const IoUartSetup &o) const {
        return tx == o.tx && rx == o.rx && rts == o.rts && cts == o.cts && baud == o.baud && dataBits == o.dataBits &&
               parity == o.parity && stopBits == o.stopBits && rtsCts == o.rtsCts && loopback == o.loopback &&
               force == o.force;
}

Error IoUartSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        *this = IoUartSetup();
        while (!text.isEmpty()) {
                StringView word = take(text, ' ');
                if (word.isEmpty()) continue;
                const size_t     eq = word.indexOf('=');
                const StringView key = eq == StringView::NPos ? word : word.left(eq);
                const StringView value = eq == StringView::NPos ? StringView() : word.mid(eq + 1);
                int8_t          *pin = key == "tx"    ? &tx
                                       : key == "rx"  ? &rx
                                       : key == "rts" ? &rts
                                       : key == "cts" ? &cts
                                                      : nullptr;
                if (pin != nullptr) {
                        if (!pad(chip, value, *pin))
                                return refuse(why, Error::Invalid, "%.*s: '%.*s' isn't a pin of the chip's",
                                              int(key.size()), key.data(), int(value.size()), value.data());
                } else if (key == "baud") {
                        uint32_t b = 0;
                        if (!number(value, b) || b < MinBaud || b > MaxBaud)
                                return refuse(why, Error::Invalid, "baud is %u to %u, not '%.*s'", unsigned(MinBaud),
                                              unsigned(MaxBaud), int(value.size()), value.data());
                        baud = b;
                } else if (key == "format") {
                        if (value.size() != 3 || value[0] < '5' || value[0] > '8' ||
                            (value[1] != 'n' && value[1] != 'e' && value[1] != 'o') ||
                            (value[2] != '1' && value[2] != '2'))
                                return refuse(
                                        why, Error::Invalid,
                                        "format is the data bits (5-8), the parity (n, e, o), the stop bits (1, 2): "
                                        "8n1, not '%.*s'",
                                        int(value.size()), value.data());
                        dataBits = uint8_t(value[0] - '0');
                        parity = value[1];
                        stopBits = uint8_t(value[2] - '0');
                } else if (key == "flow") {
                        if (value == "rtscts")
                                rtsCts = true;
                        else if (value == "none")
                                rtsCts = false;
                        else
                                return refuse(why, Error::Invalid, "flow is rtscts or none, not '%.*s'",
                                              int(value.size()), value.data());
                } else if (word == "loopback") {
                        loopback = true;
                } else if (word == "force") {
                        force = true;
                } else {
                        return refuse(why, Error::Invalid, "'%.*s' isn't a word a UART port knows", int(word.size()),
                                      word.data());
                }
        }
        if (tx < 0 && rx < 0) return refuse(why, Error::Invalid, "a UART port needs tx=, rx= or both");
        const int8_t pins[] = {tx, rx, rts, cts};
        for (size_t i = 0; i < 4; ++i)
                for (size_t k = i + 1; k < 4; ++k)
                        if (pins[i] >= 0 && pins[i] == pins[k])
                                return refuse(why, Error::Invalid, "GPIO%d is given twice", int(pins[i]));
        if ((rts >= 0 || cts >= 0) && !rtsCts) return refuse(why, Error::Invalid, "rts= and cts= are for flow=rtscts");
        if (rtsCts && (rts < 0 || cts < 0)) return refuse(why, Error::Invalid, "flow=rtscts needs rts= and cts=");
        return Error::Ok;
}

BMDS_NAMESPACE_END
