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

BMDS_NAMESPACE_END
