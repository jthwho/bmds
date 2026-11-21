/**
 * @file      ioport.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A port's setup, parsed (devplan/io.md §11.1, §11.5, §13.1).
 */

#include <cstdarg>
#include <cstdio>
#include <cstring>
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

bool IoSpiSetup::operator==(const IoSpiSetup &o) const {
        return sck == o.sck && mosi == o.mosi && miso == o.miso && csCount == o.csCount &&
               std::memcmp(cs, o.cs, csCount) == 0 && mode == o.mode && clock == o.clock && bits == o.bits &&
               lsbFirst == o.lsbFirst && csHigh == o.csHigh && threeWire == o.threeWire && loopback == o.loopback &&
               dma == o.dma &&
               force == o.force;
}

namespace {

        // `1m`, `500k`, `40000000`.
        bool clockOf(StringView text, uint32_t &out) {
                uint32_t scale = 1;
                if (!text.isEmpty() && (text.back() == 'k' || text.back() == 'K')) scale = 1000;
                if (!text.isEmpty() && (text.back() == 'm' || text.back() == 'M')) scale = 1000000;
                if (scale != 1) text = text.left(text.size() - 1);
                uint32_t n = 0;
                if (!number(text, n) || uint64_t(n) * scale > 0xffffffffu) return false;
                out = n * scale;
                return true;
        }

} // namespace

Error IoSpiSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        *this = IoSpiSetup();
        while (!text.isEmpty()) {
                StringView word = take(text, ' ');
                if (word.isEmpty()) continue;
                const size_t     eq = word.indexOf('=');
                const StringView key = eq == StringView::NPos ? word : word.left(eq);
                const StringView value = eq == StringView::NPos ? StringView() : word.mid(eq + 1);
                int8_t          *pin = key == "sck" ? &sck : key == "mosi" ? &mosi : key == "miso" ? &miso : nullptr;
                if (pin != nullptr) {
                        if (!pad(chip, value, *pin))
                                return refuse(why, Error::Invalid, "%.*s: '%.*s' isn't a pin of the chip's",
                                              int(key.size()), key.data(), int(value.size()), value.data());
                } else if (key == "cs") {
                        StringView list = value;
                        csCount = 0;
                        while (!list.isEmpty()) {
                                const StringView one = take(list, ',');
                                if (csCount == MaxCs)
                                        return refuse(why, Error::Invalid, "cs takes %u pins at most", unsigned(MaxCs));
                                if (!pad(chip, one, cs[csCount]))
                                        return refuse(why, Error::Invalid, "cs: '%.*s' isn't a pin of the chip's",
                                                      int(one.size()), one.data());
                                ++csCount;
                        }
                        if (csCount == 0) return refuse(why, Error::Invalid, "cs needs a pin: cs=5 or cs=5,6");
                } else if (key == "mode") {
                        if (value.size() != 1 || value[0] < '0' || value[0] > '3')
                                return refuse(why, Error::Invalid, "mode is 0 to 3, not '%.*s'", int(value.size()),
                                              value.data());
                        mode = uint8_t(value[0] - '0');
                } else if (key == "clock") {
                        uint32_t c = 0;
                        if (!clockOf(value, c) || c < MinClock || c > MaxClock)
                                return refuse(why, Error::Invalid,
                                              "clock is 1k to 80m (Hz, or with k or m), not '%.*s'", int(value.size()),
                                              value.data());
                        clock = c;
                } else if (key == "bits") {
                        if (value != "8" && value != "16" && value != "32")
                                return refuse(why, Error::Invalid, "bits is 8, 16 or 32, not '%.*s'", int(value.size()),
                                              value.data());
                        bits = uint8_t(value == "8" ? 8 : value == "16" ? 16 : 32);
                } else if (word == "lsb") {
                        lsbFirst = true;
                } else if (word == "cs-high") {
                        csHigh = true;
                } else if (word == "3wire") {
                        threeWire = true;
                } else if (word == "dma") {
                        dma = true;
                } else if (word == "loopback") {
                        loopback = true;
                } else if (word == "force") {
                        force = true;
                } else {
                        return refuse(why, Error::Invalid, "'%.*s' isn't a word an SPI port knows", int(word.size()),
                                      word.data());
                }
        }
        if (sck < 0) return refuse(why, Error::Invalid, "an SPI port needs sck=");
        if (mosi < 0 && miso < 0) return refuse(why, Error::Invalid, "an SPI port needs mosi=, miso= or both");
        if (threeWire && loopback) return refuse(why, Error::Invalid, "3wire and loopback don't go together");
        if ((threeWire || loopback) && miso >= 0)
                return refuse(why, Error::Invalid,
                              "%s uses MOSI for what comes in: no miso=", threeWire ? "3wire" : "loopback");
        if ((threeWire || loopback) && mosi < 0)
                return refuse(why, Error::Invalid, "%s needs mosi=", threeWire ? "3wire" : "loopback");
        int8_t pins[3 + MaxCs] = {sck, mosi, miso};
        size_t n = 3;
        for (size_t i = 0; i < csCount; ++i) pins[n++] = cs[i];
        for (size_t i = 0; i < n; ++i)
                for (size_t k = i + 1; k < n; ++k)
                        if (pins[i] >= 0 && pins[i] == pins[k])
                                return refuse(why, Error::Invalid, "GPIO%d is given twice", int(pins[i]));
        return Error::Ok;
}

bool IoRmtSetup::operator==(const IoRmtSetup &o) const {
        return pin == o.pin && receive == o.receive && resolution == o.resolution && idleHigh == o.idleHigh &&
               invert == o.invert && continuous == o.continuous && buffer == o.buffer && minNs == o.minNs &&
               maxNs == o.maxNs && monitor == o.monitor && force == o.force && pulsePin == o.pulsePin &&
               pulseWidthUs == o.pulseWidthUs && pulseLow == o.pulseLow && dma == o.dma;
}

uint32_t IoRmtSetup::maxTicks() const {
        if (maxNs == 0) return MaxTicks;
        const uint64_t t = uint64_t(maxNs) * resolution / 1000000000u;
        return t == 0 ? 1 : t > MaxTicks ? MaxTicks : uint32_t(t);
}

Error IoRmtSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        *this = IoRmtSetup();
        bool dirGiven = false;
        bool bufferGiven = false;
        bool sendWord = false, receiveWord = false;
        while (!text.isEmpty()) {
                StringView word = take(text, ' ');
                if (word.isEmpty()) continue;
                const size_t     eq = word.indexOf('=');
                const StringView key = eq == StringView::NPos ? word : word.left(eq);
                const StringView value = eq == StringView::NPos ? StringView() : word.mid(eq + 1);
                if (key == "pin") {
                        if (!pad(chip, value, pin))
                                return refuse(why, Error::Invalid, "pin: '%.*s' isn't a pin of the chip's",
                                              int(value.size()), value.data());
                } else if (key == "dir") {
                        if (value != "tx" && value != "rx")
                                return refuse(why, Error::Invalid, "dir is tx or rx, not '%.*s'", int(value.size()),
                                              value.data());
                        receive = value == "rx";
                        dirGiven = true;
                } else if (key == "resolution") {
                        uint32_t r = 0;
                        if (!clockOf(value, r) || r < MinResolution || r > MaxResolution)
                                return refuse(why, Error::Invalid,
                                              "resolution is 10k to 80m (ticks a second, or with k or m), not '%.*s'",
                                              int(value.size()), value.data());
                        resolution = r;
                } else if (key == "idle") {
                        if (value != "low" && value != "high")
                                return refuse(why, Error::Invalid, "idle is low or high, not '%.*s'", int(value.size()),
                                              value.data());
                        idleHigh = value == "high";
                        sendWord = true;
                } else if (key == "buffer") {
                        uint32_t b = 0;
                        if (!number(value, b) || b < MinBuffer || b > MaxBuffer)
                                return refuse(why, Error::Invalid, "buffer is %u to %u symbols, not '%.*s'",
                                              unsigned(MinBuffer), unsigned(MaxBuffer), int(value.size()),
                                              value.data());
                        buffer = b;
                        bufferGiven = true;
                } else if (key == "min") {
                        uint32_t n = 0;
                        if (!number(value, n) || n > MaxGlitchNs)
                                return refuse(why, Error::Invalid,
                                              "min is 0 to %u ns (the hardware's filter), not '%.*s'",
                                              unsigned(MaxGlitchNs), int(value.size()), value.data());
                        minNs = n;
                        receiveWord = true;
                } else if (key == "max") {
                        uint32_t n = 0;
                        if (!number(value, n) || n == 0)
                                return refuse(why, Error::Invalid, "max is a time in ns, not '%.*s'", int(value.size()),
                                              value.data());
                        maxNs = n;
                        receiveWord = true;
                } else if (key == "pulse") {
                        if (!pad(chip, value, pulsePin))
                                return refuse(why, Error::Invalid, "pulse: '%.*s' isn't a pin of the chip's",
                                              int(value.size()), value.data());
                        sendWord = true;
                } else if (key == "pulse-width") {
                        uint32_t w = 0;
                        if (!number(value, w) || w == 0 || w > MaxPulseUs)
                                return refuse(why, Error::Invalid, "pulse-width is 1 to %u us, not '%.*s'",
                                              unsigned(MaxPulseUs), int(value.size()), value.data());
                        pulseWidthUs = w;
                        sendWord = true;
                } else if (key == "dma") {
                        uint32_t n = DefaultDma;
                        if (eq != StringView::NPos && (!number(value, n) || n < MinDma || n > MaxDma || n % 2 != 0))
                                return refuse(why, Error::Invalid, "dma is an even %u to %u symbols, not '%.*s'",
                                              unsigned(MinDma), unsigned(MaxDma), int(value.size()), value.data());
                        dma = n;
                        sendWord = true;
                } else if (word == "pulse-low") {
                        pulseLow = true;
                        sendWord = true;
                } else if (word == "invert") {
                        invert = true;
                } else if (word == "continuous") {
                        continuous = true;
                        sendWord = true;
                } else if (word == "monitor") {
                        monitor = true;
                        receiveWord = true;
                } else if (word == "force") {
                        force = true;
                } else {
                        return refuse(why, Error::Invalid, "'%.*s' isn't a word an RMT port knows", int(word.size()),
                                      word.data());
                }
        }
        if (pin < 0) return refuse(why, Error::Invalid, "an RMT port needs pin=");
        if (!dirGiven) return refuse(why, Error::Invalid, "an RMT port needs dir=tx or dir=rx");
        if (receive && sendWord)
                return refuse(why, Error::Invalid, "idle=, continuous, pulse= and dma are a sender's (dir=tx)");
        if (!receive && receiveWord)
                return refuse(why, Error::Invalid, "min=, max= and monitor are a receiver's (dir=rx)");
        if (pulsePin >= 0 && pulsePin == pin) return refuse(why, Error::Invalid, "GPIO%d is given twice", int(pin));
        // DMA fills half its buffer at a time from the ring: the ring holds a whole DMA buffer at
        // least (twice it unless asked).
        if (dma > 0 && !bufferGiven) buffer = 2 * dma;
        if (dma > 0 && buffer < dma)
                return refuse(why, Error::Invalid, "buffer is at least dma's (%u symbols) for DMA", unsigned(dma));
        if (monitor && force) return refuse(why, Error::Invalid, "monitor takes nothing: force has nothing to take");
        if (maxNs != 0 && uint64_t(maxNs) * resolution / 1000000000u > MaxTicks)
                return refuse(why, Error::Invalid, "max is at most %u ticks: %u ns at this resolution",
                              unsigned(MaxTicks), unsigned(uint64_t(MaxTicks) * 1000000000u / resolution));
        return Error::Ok;
}

namespace {

        // `1.5ms`, `250us`, `20000` (ns), `2s`: whole ns, at most @p limit.
        bool timeOf(StringView text, uint64_t limit, uint64_t &out) {
                uint64_t scale = 1;
                if (text.endsWith(StringView("ns"))) {
                        text = text.left(text.size() - 2);
                } else if (text.endsWith(StringView("us"))) {
                        scale = 1000;
                        text = text.left(text.size() - 2);
                } else if (text.endsWith(StringView("ms"))) {
                        scale = 1000000;
                        text = text.left(text.size() - 2);
                } else if (text.endsWith(StringView("s"))) {
                        scale = 1000000000;
                        text = text.left(text.size() - 1);
                }
                const size_t     dot = text.indexOf('.');
                const StringView whole = dot == StringView::NPos ? text : text.left(dot);
                const StringView part = dot == StringView::NPos ? StringView() : text.mid(dot + 1);
                if (whole.isEmpty() && part.isEmpty()) return false;
                uint32_t w = 0;
                if (!whole.isEmpty() && !number(whole, w)) return false;
                if (dot != StringView::NPos && part.isEmpty()) return false;
                uint64_t ns = uint64_t(w) * scale;
                // The fraction's digits, as far as they mean whole ns.
                uint64_t place = scale;
                for (const char c : part) {
                        if (c < '0' || c > '9') return false;
                        place /= 10;
                        ns += uint64_t(c - '0') * place;
                }
                if (ns > limit) return false;
                out = ns;
                return true;
        }

        // `25%`, `12.5%`, `33.33%`: hundredths of a percent, at most 100 %.
        bool shareOf(StringView text, uint64_t &out) {
                if (text.isEmpty() || text.back() != '%') return false;
                text = text.left(text.size() - 1);
                const size_t     dot = text.indexOf('.');
                const StringView whole = dot == StringView::NPos ? text : text.left(dot);
                const StringView part = dot == StringView::NPos ? StringView() : text.mid(dot + 1);
                uint32_t         w = 0;
                if (whole.isEmpty() || !number(whole, w) || part.size() > 2) return false;
                if (dot != StringView::NPos && part.isEmpty()) return false;
                uint64_t v = uint64_t(w) * 100;
                uint64_t place = 10;
                for (const char c : part) {
                        if (c < '0' || c > '9') return false;
                        v += uint64_t(c - '0') * place;
                        place /= 10;
                }
                if (v > IoPwmSetup::Whole) return false;
                out = v;
                return true;
        }

} // namespace

bool IoPwmSetup::operator==(const IoPwmSetup &o) const {
        return pin == o.pin && periodNs == o.periodNs && duty == o.duty && dutyShare == o.dutyShare &&
               offset == o.offset && offsetShare == o.offsetShare && force == o.force;
}

Error IoPwmSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        *this = IoPwmSetup();
        while (!text.isEmpty()) {
                StringView word = take(text, ' ');
                if (word.isEmpty()) continue;
                const size_t     eq = word.indexOf('=');
                const StringView key = eq == StringView::NPos ? word : word.left(eq);
                const StringView value = eq == StringView::NPos ? StringView() : word.mid(eq + 1);
                if (key == "pin") {
                        if (!pad(chip, value, pin))
                                return refuse(why, Error::Invalid, "pin: '%.*s' isn't a pin of the chip's",
                                              int(value.size()), value.data());
                } else if (key == "period") {
                        if (!timeOf(value, MaxPeriodNs, periodNs))
                                return refuse(why, Error::Invalid,
                                              "period is a time up to 60s (ns, us, ms or s; 0: off), not '%.*s'",
                                              int(value.size()), value.data());
                } else if (key == "duty" || key == "offset") {
                        const bool duty_ = key == "duty";
                        uint64_t   v = 0;
                        bool       share = shareOf(value, v);
                        if (!share && !timeOf(value, MaxPeriodNs, v))
                                return refuse(why, Error::Invalid,
                                              "%s is a time or a share of the period (25%%), not '%.*s'",
                                              duty_ ? "duty" : "offset", int(value.size()), value.data());
                        (duty_ ? duty : offset) = v;
                        (duty_ ? dutyShare : offsetShare) = share;
                } else if (word == "force") {
                        force = true;
                } else {
                        return refuse(why, Error::Invalid, "'%.*s' isn't a word a PWM channel knows", int(word.size()),
                                      word.data());
                }
        }
        if (pin < 0) return refuse(why, Error::Invalid, "a PWM channel needs pin=");
        if (dutyNs() > periodNs) return refuse(why, Error::Invalid, "duty is longer than the period");
        if (periodNs > 0 ? offsetNs() >= periodNs : offsetNs() > 0)
                return refuse(why, Error::Invalid, "offset is within the period (shorter than it)");
        return Error::Ok;
}

namespace {

        constexpr const char *FunctionNames[] = {"increase",        "decrease",        "pulse-direction",
                                                 "quadrature-x1-a", "quadrature-x1-b", "quadrature-x2-a",
                                                 "quadrature-x2-b", "quadrature-x4"};

} // namespace

const char *IoPcntSetup::functionName(Function f) {
        return FunctionNames[size_t(f)];
}

bool IoPcntSetup::operator==(const IoPcntSetup &o) const {
        return a == o.a && b == o.b && function == o.function && action == o.action && filterNs == o.filterNs &&
               pullUp == o.pullUp && pullDown == o.pullDown && monitor == o.monitor && force == o.force;
}

Error IoPcntSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        *this = IoPcntSetup();
        bool actionGiven = false;
        while (!text.isEmpty()) {
                StringView word = take(text, ' ');
                if (word.isEmpty()) continue;
                const size_t     eq = word.indexOf('=');
                const StringView key = eq == StringView::NPos ? word : word.left(eq);
                const StringView value = eq == StringView::NPos ? StringView() : word.mid(eq + 1);
                if (key == "a" || key == "b") {
                        if (!pad(chip, value, key == "a" ? a : b))
                                return refuse(why, Error::Invalid, "%.*s: '%.*s' isn't a pin of the chip's",
                                              int(key.size()), key.data(), int(value.size()), value.data());
                } else if (key == "function") {
                        size_t f = 0;
                        while (f < sizeof(FunctionNames) / sizeof(FunctionNames[0]) && value != FunctionNames[f]) ++f;
                        if (f == sizeof(FunctionNames) / sizeof(FunctionNames[0]))
                                return refuse(why, Error::Invalid,
                                              "function is increase, decrease, pulse-direction or quadrature-x1-a, "
                                              "-x1-b, -x2-a, -x2-b, -x4, not '%.*s'",
                                              int(value.size()), value.data());
                        function = Function(f);
                } else if (key == "action") {
                        if (value == "rising")
                                action = Action::Rising;
                        else if (value == "falling")
                                action = Action::Falling;
                        else if (value == "both")
                                action = Action::Both;
                        else
                                return refuse(why, Error::Invalid, "action is rising, falling or both, not '%.*s'",
                                              int(value.size()), value.data());
                        actionGiven = true;
                } else if (key == "filter") {
                        uint64_t ns = 0;
                        if (!timeOf(value, MaxFilterNs, ns))
                                return refuse(why, Error::Invalid, "filter is a time up to 10us, not '%.*s'",
                                              int(value.size()), value.data());
                        filterNs = uint32_t(ns);
                } else if (word == "pull-up") {
                        pullUp = true;
                } else if (word == "pull-down") {
                        pullDown = true;
                } else if (word == "monitor") {
                        monitor = true;
                } else if (word == "force") {
                        force = true;
                } else {
                        return refuse(why, Error::Invalid, "'%.*s' isn't a word a counter knows", int(word.size()),
                                      word.data());
                }
        }
        if (a < 0) return refuse(why, Error::Invalid, "a counter needs a=");
        if (readsB() && b < 0) return refuse(why, Error::Invalid, "%s needs b=", functionName(function));
        if (!readsB() && b >= 0) return refuse(why, Error::Invalid, "%s reads only a=", functionName(function));
        if (a == b) return refuse(why, Error::Invalid, "GPIO%d is given twice", int(a));
        if (actionGiven && readsB()) return refuse(why, Error::Invalid, "action= is increase's and decrease's");
        if (pullUp && pullDown) return refuse(why, Error::Invalid, "pull-up or pull-down, not both");
        if (monitor && (force || pullUp || pullDown))
                return refuse(why, Error::Invalid, "monitor takes nothing: no force, no pulls");
        return Error::Ok;
}

uint8_t IoParlioSetup::width() const {
        uint8_t w = 1;
        while (w < dataCount) w = uint8_t(w * 2);
        return w;
}

bool IoParlioSetup::operator==(const IoParlioSetup &o) const {
        if (dataCount != o.dataCount) return false;
        for (size_t i = 0; i < dataCount; ++i)
                if (data[i] != o.data[i]) return false;
        return receive == o.receive && clock == o.clock && clk == o.clk && clkIn == o.clkIn && rising == o.rising &&
               msb == o.msb && idle == o.idle && buffer == o.buffer && monitor == o.monitor && drive == o.drive &&
               force == o.force;
}

Error IoParlioSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        *this = IoParlioSetup();
        bool dirGiven = false, sendWord = false, receiveWord = false;
        while (!text.isEmpty()) {
                StringView word = take(text, ' ');
                if (word.isEmpty()) continue;
                const size_t     eq = word.indexOf('=');
                const StringView key = eq == StringView::NPos ? word : word.left(eq);
                const StringView value = eq == StringView::NPos ? StringView() : word.mid(eq + 1);
                if (key == "dir") {
                        if (value != "tx" && value != "rx")
                                return refuse(why, Error::Invalid, "dir is tx or rx, not '%.*s'", int(value.size()),
                                              value.data());
                        receive = value == "rx";
                        dirGiven = true;
                } else if (key == "data") {
                        StringView list = value;
                        dataCount = 0;
                        while (!list.isEmpty()) {
                                const StringView one = take(list, ',');
                                if (dataCount == MaxData) return refuse(why, Error::Invalid, "data is 1 to 16 pins");
                                if (!pad(chip, one, data[dataCount]))
                                        return refuse(why, Error::Invalid, "data: '%.*s' isn't a pin of the chip's",
                                                      int(one.size()), one.data());
                                ++dataCount;
                        }
                } else if (key == "clock") {
                        uint32_t c = 0;
                        if (!clockOf(value, c) || c < MinClock || c > MaxClock)
                                return refuse(why, Error::Invalid,
                                              "clock is 1k to 80m (Hz, or with k or m), not '%.*s'", int(value.size()),
                                              value.data());
                        clock = c;
                } else if (key == "clk" || key == "clk-in") {
                        if (!pad(chip, value, key == "clk" ? clk : clkIn))
                                return refuse(why, Error::Invalid, "%.*s: '%.*s' isn't a pin of the chip's",
                                              int(key.size()), key.data(), int(value.size()), value.data());
                } else if (key == "edge") {
                        if (value != "rising" && value != "falling")
                                return refuse(why, Error::Invalid, "edge is rising or falling, not '%.*s'",
                                              int(value.size()), value.data());
                        rising = value == "rising";
                } else if (key == "idle") {
                        uint32_t v = 0;
                        if (!number(value, v))
                                return refuse(why, Error::Invalid, "idle is the lines' value, not '%.*s'",
                                              int(value.size()), value.data());
                        idle = v;
                        sendWord = true;
                } else if (key == "buffer") {
                        uint32_t b = 0;
                        if (!number(value, b) || b < MinBuffer || b > MaxBuffer || b % 64 != 0)
                                return refuse(why, Error::Invalid,
                                              "buffer is 4096 to 65472 bytes, a multiple of 64, not "
                                              "'%.*s'",
                                              int(value.size()), value.data());
                        buffer = b;
                        receiveWord = true;
                } else if (key == "drive") {
                        uint32_t d = 0;
                        if (!number(value, d) || d > 3)
                                return refuse(why, Error::Invalid, "drive is 0 to 3, not '%.*s'", int(value.size()),
                                              value.data());
                        drive = uint8_t(d);
                } else if (word == "msb") {
                        msb = true;
                } else if (word == "monitor") {
                        monitor = true;
                        receiveWord = true;
                } else if (word == "force") {
                        force = true;
                } else {
                        return refuse(why, Error::Invalid, "'%.*s' isn't a word a PARLIO port knows", int(word.size()),
                                      word.data());
                }
        }
        if (!dirGiven) return refuse(why, Error::Invalid, "a PARLIO port needs dir=tx or dir=rx");
        if (dataCount == 0) return refuse(why, Error::Invalid, "a PARLIO port needs data=");
        if (receive && sendWord) return refuse(why, Error::Invalid, "idle= is a sender's (dir=tx)");
        if (!receive && receiveWord)
                return refuse(why, Error::Invalid, "buffer= and monitor are a receiver's (dir=rx)");
        if (receive && clk >= 0 && clkIn >= 0)
                return refuse(why, Error::Invalid, "a receiver gives its clock out (clk=) only from its own clock");
        if (idle >= (uint64_t(1) << width()))
                return refuse(why, Error::Invalid, "idle is more than %u lines hold", unsigned(width()));
        if (monitor && force) return refuse(why, Error::Invalid, "monitor takes nothing: force has nothing to take");
        // Each pin once.
        int8_t all[MaxData + 2];
        size_t n = 0;
        for (size_t i = 0; i < dataCount; ++i) all[n++] = data[i];
        if (clk >= 0) all[n++] = clk;
        if (clkIn >= 0) all[n++] = clkIn;
        for (size_t i = 0; i < n; ++i)
                for (size_t j = i + 1; j < n; ++j)
                        if (all[i] == all[j]) return refuse(why, Error::Invalid, "GPIO%d is given twice", int(all[i]));
        return Error::Ok;
}

namespace {

        // `0`, `2.5`, `6`, `12` (dB).
        bool attenOf(StringView text, IoAdcSetup::Atten &out) {
                if (text.endsWith(StringView("db"))) text = text.left(text.size() - 2);
                if (text == "0")
                        out = IoAdcSetup::Atten::Db0;
                else if (text == "2.5")
                        out = IoAdcSetup::Atten::Db2_5;
                else if (text == "6")
                        out = IoAdcSetup::Atten::Db6;
                else if (text == "12")
                        out = IoAdcSetup::Atten::Db12;
                else
                        return false;
                return true;
        }

} // namespace

bool IoAdcSetup::channelOf(const IoChip &chip, int pad, uint8_t &unit, uint8_t &channel) {
        if (pad < 0 || size_t(pad) >= chip.pads.size()) return false;
        const char *a = chip.pads[size_t(pad)].analog;
        // `ADC1_CH4`, maybe followed by other functions.
        if (a == nullptr || std::strncmp(a, "ADC", 3) != 0 || a[3] < '1' || a[3] > '2' ||
            std::strncmp(a + 4, "_CH", 3) != 0 || a[7] < '0' || a[7] > '9')
                return false;
        unit = uint8_t(a[3] - '1');
        channel = uint8_t(a[7] - '0');
        return true;
}

uint32_t IoAdcSetup::centiDb(Atten a) {
        switch (a) {
                case Atten::Db0: return 0;
                case Atten::Db2_5: return 250;
                case Atten::Db6: return 600;
                case Atten::Db12: return 1200;
        }
        return 0;
}

int IoAdcSetup::indexOf(int pad) const {
        for (int i = 0; i < pinCount; ++i)
                if (pins[i] == pad) return i;
        return -1;
}

bool IoAdcSetup::operator==(const IoAdcSetup &o) const {
        if (pinCount != o.pinCount) return false;
        for (size_t i = 0; i < pinCount; ++i)
                if (pins[i] != o.pins[i] || atten[i] != o.atten[i]) return false;
        return rate == o.rate && buffer == o.buffer && force == o.force;
}

Error IoAdcSetup::parse(const IoChip &chip, StringView text, char (&why)[ReasonBytes]) {
        why[0] = '\0';
        *this = IoAdcSetup();
        Atten      all = Atten::Db12;
        bool       own[MaxPins] = {};
        StringView list;
        bool       listGiven = false;
        while (!text.isEmpty()) {
                StringView word = take(text, ' ');
                if (word.isEmpty()) continue;
                const size_t     eq = word.indexOf('=');
                const StringView key = eq == StringView::NPos ? word : word.left(eq);
                const StringView value = eq == StringView::NPos ? StringView() : word.mid(eq + 1);
                if (key == "pins") {
                        list = value;
                        listGiven = true;
                } else if (key == "atten") {
                        if (!attenOf(value, all))
                                return refuse(why, Error::Invalid, "atten is 0, 2.5, 6 or 12 (dB), not '%.*s'",
                                              int(value.size()), value.data());
                } else if (key == "rate") {
                        uint32_t r = 0;
                        if (!clockOf(value, r) || r < MinRate || r > MaxRate)
                                return refuse(why, Error::Invalid,
                                              "rate is 611 to 83333 conversions a second (or with k), not '%.*s'",
                                              int(value.size()), value.data());
                        rate = r;
                } else if (key == "buffer") {
                        uint32_t b = 0;
                        if (!number(value, b) || b < MinBuffer || b > MaxBuffer || b % 4 != 0)
                                return refuse(why, Error::Invalid,
                                              "buffer is 1024 to 65536 bytes, a multiple of 4, not '%.*s'",
                                              int(value.size()), value.data());
                        buffer = b;
                } else if (word == "force") {
                        force = true;
                } else {
                        return refuse(why, Error::Invalid, "'%.*s' isn't a word the ADC knows", int(word.size()),
                                      word.data());
                }
        }
        if (!listGiven || list.isEmpty()) return refuse(why, Error::Invalid, "the ADC needs pins=");
        while (!list.isEmpty()) {
                StringView one = take(list, ',');
                if (pinCount == MaxPins) return refuse(why, Error::Invalid, "pins= is 14 at most");
                const size_t     colon = one.indexOf(':');
                const StringView padText = colon == StringView::NPos ? one : one.left(colon);
                int8_t           at = -1;
                if (!pad(chip, padText, at))
                        return refuse(why, Error::Invalid, "pins: '%.*s' isn't a pin of the chip's",
                                      int(padText.size()), padText.data());
                uint8_t u = 0, c = 0;
                if (!channelOf(chip, at, u, c))
                        return refuse(why, Error::Invalid, "GPIO%d has no ADC channel", int(at));
                if (indexOf(at) >= 0) return refuse(why, Error::Invalid, "GPIO%d is given twice", int(at));
                if (colon != StringView::NPos) {
                        if (!attenOf(one.mid(colon + 1), atten[pinCount]))
                                return refuse(why, Error::Invalid, "GPIO%d: atten is 0, 2.5, 6 or 12 (dB)", int(at));
                        own[pinCount] = true;
                }
                pins[pinCount] = at;
                unit[pinCount] = u;
                channel[pinCount] = c;
                ++pinCount;
        }
        for (size_t i = 0; i < pinCount; ++i)
                if (!own[i]) atten[i] = all;
        if (buffer != 0 && rate == 0) return refuse(why, Error::Invalid, "buffer= is a stream's: it needs rate=");
        return Error::Ok;
}

BMDS_NAMESPACE_END
