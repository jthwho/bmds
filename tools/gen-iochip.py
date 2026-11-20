#!/usr/bin/env python3
# Writes a chip's I/O table (devplan/io.md §3.1): components/bmds_io/chips/<target>/iochipdef.cpp,
# a constexpr bmds::IoChip in rodata, from ESP-IDF's own descriptions of the chip:
#
#   - soc/<target>/include/soc/gpio_sig_map.h       the GPIO matrix's signals, by number
#   - soc/<target>/register/<hw>/soc/io_mux_reg.h   every pad's IO_MUX functions (FUNC_GPIOn_NAME)
#   - soc/<target>/include/soc/soc_caps.h           pad and instance counts (checked against CHIPS)
#   - docs/en/api-reference/peripherals/gpio/<target>.inc
#                                                   every pad's analog functions, LP IO and notes
#
# The output is checked in, so a build needs neither the script nor these files; the host test
# (tests/host/iochip.cpp) checks the table against what the script reads. Porting to another
# ESP32 is an entry in CHIPS below and a run of this script.
#
#   gen-iochip.py [--idf IDF_PATH] TARGET [OUT]
#   gen-iochip.py --check TARGET FILE      exit 1 if FILE isn't what the script writes now

import argparse
import os
import re
import sys

# Per chip: where its headers are, and its resources after the pads (devplan/io.md §3.2), in
# order. Each kind: (IoKind, name, first instance, count, soc_caps macro the count is checked
# against or None). Counts ESP-IDF no longer states in soc_caps.h say where they come from.
CHIPS = {
    "esp32p4": {
        "name": "ESP32-P4",
        # hw_ver1 (v1.x silicon, ESP32P4_SELECTS_REV_LESS_V3) and hw_ver3 have the same IO_MUX
        # functions (checked 2026-11-20); the script reads both and refuses if they ever differ.
        "io_mux": ["register/hw_ver1/soc/io_mux_reg.h", "register/hw_ver3/soc/io_mux_reg.h"],
        "pads": "SOC_GPIO_PIN_COUNT",
        "lp_io": "SOC_RTCIO_PIN_COUNT",
        "kinds": [
            ("Uart", "uart", 0, 5, "SOC_UART_HP_NUM"),
            ("LpUart", "lp-uart", 0, 1, "SOC_UART_LP_NUM"),
            ("I2c", "i2c", 0, 2, "SOC_HP_I2C_NUM"),
            ("LpI2c", "lp-i2c", 0, 1, "SOC_LP_I2C_NUM"),
            # SPI0 and SPI1 are the flash's and PSRAM's: SPI2 and SPI3 are for general use.
            ("Spi", "spi", 2, 2, None),
            # hal/esp32p4/include/hal/i2s_ll.h: I2S_LL_GET_HW(0..2).
            ("I2s", "i2s", 0, 3, None),
            ("Rmt", "rmt", 0, 8, "SOC_RMT_CHANNELS_PER_GROUP"),
            ("Ledc", "ledc", 0, 8, "SOC_LEDC_CHANNEL_NUM"),
            ("LedcTimer", "ledc-timer", 0, 4, "SOC_LEDC_TIMER_NUM"),
            # TRM, PCNT: four units in the one group (pcnt_ll.h: PCNT_LL_GET_HW(0)).
            ("Pcnt", "pcnt", 0, 4, None),
            ("Mcpwm", "mcpwm", 0, 2, "SOC_MCPWM_GROUPS"),
            ("Twai", "twai", 0, 3, "SOC_TWAI_CONTROLLER_NUM"),
            ("Sdmmc", "sdmmc", 0, 2, "SOC_SDMMC_NUM_SLOTS"),
            ("Emac", "emac", 0, 1, None),
            ("Uhci", "uhci", 0, 1, "SOC_UHCI_NUM"),
            ("Parlio", "parlio", 0, 1, "SOC_PARLIO_GROUPS"),
            ("UsbSerialJtag", "usb-serial-jtag", 0, 1, None),
        ],
        # Signal groups whose first word isn't the peripheral's own name.
        "group_names": {"PWM0": "MCPWM0", "PWM1": "MCPWM1", "SD": "SDMMC", "CORE": "DEDIC_GPIO",
                        "SIG": "LOOPBACK", "HP": "PROBE", "MII": "EMAC"},
        "gpio_function": 1,
    },
}


def fail(message):
    sys.exit("gen-iochip: " + message)


def read(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError as e:
        fail("%s: %s" % (path, e.strerror))


def caps_value(caps, macro):
    m = re.search(r"^#define\s+%s\s+\(?\s*(\d+)\s*U?\s*\)?" % re.escape(macro), caps, re.M)
    if not m:
        fail("soc_caps.h has no %s" % macro)
    return int(m.group(1))


def io_mux_functions(text, pads):
    functions = [dict() for _ in range(pads)]
    for m in re.finditer(r"^#define\s+FUNC_GPIO(\d+)_(\w+)\s+(\d+)\s*$", text, re.M):
        pad, name, index = int(m.group(1)), m.group(2), int(m.group(3))
        if pad >= pads:
            continue  # the register file has pads past the GPIOs (the P4's 55 and 56): not ours
        if name == "GPIO%d" % pad:
            name = "GPIO"
        elif name.endswith("_PAD"):
            name = name[:-4]
        if index in functions[pad] and functions[pad][index] != name:
            fail("io_mux_reg.h: pad %d function %d is both %s and %s" % (pad, index, functions[pad][index], name))
        functions[pad][index] = name
    for pad, f in enumerate(functions):
        if not f:
            fail("io_mux_reg.h: pad %d has no functions" % pad)
    return functions


def signal_name(define):
    name = define[:-len("_IDX")]
    m = re.match(r"^(.*)_PAD_(?:IN|OUT)(\d*)$", name)
    if m:
        return m.group(1) + m.group(2)
    return name


def signal_direction(define):
    if re.search(r"_(?:PAD_)?IN\d*_IDX$", define):
        return "in"
    if re.search(r"_(?:PAD_)?OUT\d*_IDX$", define):
        return "out"
    return None


def signals(text):
    inputs, outputs = {}, {}
    undecided = []
    gpio_out = None
    for m in re.finditer(r"^#define\s+(\w+_IDX)\s+(\d+)\s*$", text, re.M):
        define, index = m.group(1), int(m.group(2))
        if define == "SIG_GPIO_OUT_IDX":
            gpio_out = index
            continue
        direction = signal_direction(define)
        if direction is None:
            undecided.append((define, index))
            continue
        table = inputs if direction == "in" else outputs
        if index in table and table[index] != signal_name(define):
            fail("gpio_sig_map.h: %s signal %d is both %s and %s" % (direction, index, table[index], define))
        table[index] = signal_name(define)
    # A name that says neither (DBG_CH0_CLK_IDX, SIG_IN_FUNC250_IDX twice): the side whose slot is
    # free, inputs first, as ESP-IDF lists each number's input before its output.
    for define, index in undecided:
        name = signal_name(define)
        if index not in inputs:
            inputs[index] = name
        elif index not in outputs:
            outputs[index] = name
        else:
            fail("gpio_sig_map.h: %s (%d) has no free slot" % (define, index))
    if gpio_out is None:
        fail("gpio_sig_map.h has no SIG_GPIO_OUT_IDX")
    return inputs, outputs, gpio_out


def doc_pads(text, pads):
    # The pin table: rows of "* - GPIOn", "- analog", "- LP GPIO", "- comments".
    rows = {}
    body = text.split(".. list-table::", 1)
    if len(body) < 2:
        fail("the GPIO doc has no list-table")
    lines = body[1].split("\n")
    i = 0
    while i < len(lines):
        m = re.match(r"^\s*\* - GPIO(\d+)\s*$", lines[i])
        if not m:
            i += 1
            continue
        cells = []
        j = i + 1
        while j < len(lines) and len(cells) < 3:
            c = re.match(r"^\s*- ?(.*)$", lines[j])
            if c and not re.match(r"^\s*\* -", lines[j]):
                cells.append(c.group(1).strip())
                j += 1
            else:
                break
        while len(cells) < 3:
            cells.append("")
        rows[int(m.group(1))] = cells
        i = j
    strapping = set()
    m = re.search(r"Strapping pin:? (.*?) are strapping pins", text)
    if m:
        strapping = {int(n) for n in re.findall(r"GPIO(\d+)", m.group(1))}
    out = []
    for pad in range(pads):
        analog, lp, note = rows.get(pad, ["", "", ""])
        lp_io = -1
        lm = re.match(r"^LP_GPIO(\d+)$", lp)
        if lm:
            lp_io = int(lm.group(1))
        elif lp:
            fail("the GPIO doc: pad %d's LP GPIO is '%s'" % (pad, lp))
        if note.lower() == "strapping pin":
            strapping.add(pad)
            note = ""
        out.append({"analog": analog, "lp": lp_io, "note": note, "strapping": pad in strapping})
    usb = re.search(r"USB-JTAG: (GPIO\d+) and (GPIO\d+) are used by USB-JTAG by default", text)
    if usb:
        for name in usb.groups():
            pad = int(name[4:])
            out[pad]["note"] = (out[pad]["note"] + "; " if out[pad]["note"] else "") + "USB-JTAG by default"
    return out


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def generate(idf, target):
    chip = CHIPS.get(target)
    if chip is None:
        fail("no chip '%s' (known: %s)" % (target, ", ".join(sorted(CHIPS))))
    soc = os.path.join(idf, "components", "soc", target)
    caps = read(os.path.join(soc, "include", "soc", "soc_caps.h"))
    pads = caps_value(caps, chip["pads"])
    lp_count = caps_value(caps, chip["lp_io"])
    for kind, name, first, count, macro in chip["kinds"]:
        if macro is not None and caps_value(caps, macro) != count:
            fail("%s: %d in the script, %s says %d" % (name, count, macro, caps_value(caps, macro)))
    muxes = [io_mux_functions(read(os.path.join(soc, p)), pads) for p in chip["io_mux"]]
    for other, path in zip(muxes[1:], chip["io_mux"][1:]):
        if other != muxes[0]:
            fail("%s's IO_MUX functions differ from %s's: the table must choose by revision" %
                 (path, chip["io_mux"][0]))
    functions = muxes[0]
    for pad, f in enumerate(functions):
        if f.get(chip["gpio_function"]) != "GPIO":
            fail("pad %d: function %d isn't GPIO" % (pad, chip["gpio_function"]))
    inputs, outputs, gpio_out = signals(read(os.path.join(soc, "include", "soc", "gpio_sig_map.h")))
    sig_header = read(os.path.join(soc, "include", "soc", "gpio_pins.h"))
    const0 = int(re.search(r"GPIO_MATRIX_CONST_ZERO_INPUT\s+\((0x[0-9A-Fa-f]+|\d+)\)", sig_header).group(1), 0)
    const1 = int(re.search(r"GPIO_MATRIX_CONST_ONE_INPUT\s+\((0x[0-9A-Fa-f]+|\d+)\)", sig_header).group(1), 0)
    docs = doc_pads(read(os.path.join(idf, "docs", "en", "api-reference", "peripherals", "gpio", target + ".inc")),
                    pads)
    if sum(1 for d in docs if d["lp"] >= 0) != lp_count:
        fail("the GPIO doc names %d LP IOs, %s says %d" %
             (sum(1 for d in docs if d["lp"] >= 0), chip["lp_io"], lp_count))

    groups = []

    def group_of(name):
        word = name.split("_", 1)[0]
        word = chip["group_names"].get(word, word)
        if word not in groups:
            groups.append(word)
        return groups.index(word)

    input_count = max(inputs) + 1
    output_count = max(max(outputs) + 1, gpio_out + 1)
    input_names = [inputs.get(i) for i in range(input_count)]
    output_names = [outputs.get(i) for i in range(output_count)]
    output_names[gpio_out] = "GPIO"
    input_groups = [group_of(n) if n else 255 for n in input_names]
    output_groups = [group_of(n) if n else 255 for n in output_names]
    if len(groups) > 254:
        fail("more than 254 signal groups (255 is none)")

    lines = [
        "/**",
        " * @file      iochipdef.cpp",
        " * @copyright Jason Howard. All rights reserved.",
        " *",
        " * SPDX-License-Identifier: MIT",
        " * See LICENSE file in the project root folder for license information.",
        " *",
        " * The %s's I/O (devplan/io.md §3.1). Generated by tools/gen-iochip.py %s from ESP-IDF's" %
        (chip["name"], target),
        " * gpio_sig_map.h, io_mux_reg.h, soc_caps.h and GPIO documentation: do not edit.",
        " */",
        "",
        "#include <bmds/iochip.h>",
        "",
        "BMDS_NAMESPACE_BEGIN",
        "",
        "namespace {",
        "",
        "        // Every pad's IO_MUX functions, pad by pad.",
        "        constexpr IoMuxFunction Functions[] = {",
    ]
    first = []
    n = 0
    for pad, f in enumerate(functions):
        first.append(n)
        # A peripheral's function, or the pad's own (GPIO, and GPIOn_0: the reset value of pads
        # without a peripheral function 0, which drives nothing; checked on the board 2026-11-20).
        entries = ", ".join("{%d, %s, %s}" % (i, c_string(f[i]), "false" if re.match(r"^GPIO(\d+(_\d+)?)?$", f[i])
                                               else "true") for i in sorted(f))
        lines.append("                // GPIO%d" % pad)
        lines.append("                %s," % entries)
        n += len(f)
    lines += ["        };", "", "        constexpr IoPadInfo Pads[] = {"]
    for pad in range(pads):
        d = docs[pad]
        flags = "IoPadInfo::Strapping" if d["strapping"] else "0"
        lines.append("                {%s, %d, %d, %d, %s, %s}, // GPIO%d" %
                     (flags, d["lp"], first[pad], len(functions[pad]), c_string(d["analog"]),
                      c_string(d["note"]), pad))
    lines += ["        };", ""]

    def name_table(var, names):
        out = ["        constexpr const char *const %s[] = {" % var]
        for i, nm in enumerate(names):
            out.append("                %s, // %d" % (c_string(nm) if nm else "nullptr", i))
        out.append("        };")
        out.append("")
        return out

    def byte_table(var, values):
        out = ["        constexpr uint8_t %s[] = {" % var]
        for i in range(0, len(values), 16):
            out.append("                " + ", ".join(str(v) for v in values[i:i + 16]) + ",")
        out.append("        };")
        out.append("")
        return out

    lines += name_table("InputSignals", input_names)
    lines += name_table("OutputSignals", output_names)
    lines += byte_table("InputGroups", input_groups)
    lines += byte_table("OutputGroups", output_groups)
    lines += ["        constexpr const char *const Groups[] = {"]
    for i in range(0, len(groups), 8):
        lines.append("                " + ", ".join(c_string(g) for g in groups[i:i + 8]) + ",")
    lines += ["        };", "", "        constexpr IoKindInfo Kinds[] = {"]
    for kind, name, first_n, count, macro in chip["kinds"]:
        lines.append("                {IoKind::%s, %s, %d, %d}," % (kind, c_string(name), first_n, count))
    lines += [
        "        };",
        "",
        "        constexpr IoChip Chip = {",
        "                .name = %s," % c_string(chip["name"]),
        "                .gpioFunction = %d," % chip["gpio_function"],
        "                .gpioOutputSignal = %d," % gpio_out,
        "                .constZeroInput = 0x%02x," % const0,
        "                .constOneInput = 0x%02x," % const1,
        "                .pads = Pads,",
        "                .functions = Functions,",
        "                .inputSignals = InputSignals,",
        "                .outputSignals = OutputSignals,",
        "                .inputGroups = InputGroups,",
        "                .outputGroups = OutputGroups,",
        "                .groups = Groups,",
        "                .kinds = Kinds,",
        "        };",
        "",
        "} // namespace",
        "",
        "const IoChip &ioChip() {",
        "        return Chip;",
        "}",
        "",
        "BMDS_NAMESPACE_END",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Write a chip's I/O table from ESP-IDF's headers.")
    parser.add_argument("--idf", default=os.environ.get("IDF_PATH", os.path.expanduser("~/esp/esp-idf")))
    parser.add_argument("--check", action="store_true", help="compare with OUT instead of writing it")
    parser.add_argument("target")
    parser.add_argument("out", nargs="?")
    args = parser.parse_args()
    root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    out = args.out or os.path.join(root, "components", "bmds_io", "chips", args.target, "iochipdef.cpp")
    text = generate(args.idf, args.target)
    if args.check:
        if read(out) != text:
            fail("%s isn't what ESP-IDF's headers give now: run tools/gen-iochip.py %s" % (out, args.target))
        return 0
    if os.path.exists(out) and read(out) == text:
        return 0
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
