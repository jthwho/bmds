# I/O: pins, their owners, and prototyping from the desktop

Status: **planned** (2026-11-20, the user's request: "a complete view into what each pin is muxed
to, a way to quickly prototype UART, SPI, I2C, GPIO ... aware of I/O that's system managed ... plan
for this to port to other ESP32 devices and other boards"). Decided the same day (§7): built into the
firmware; Linux's device names and interfaces wherever Linux has one; ports and pin setups held in
sysconf (for the session until saved); System pins changed only with `--force`. The user also: "do
this the *right* way, no shortcuts": proper kernel interfaces, DMA-driven data paths later, and the
RMT (pulses) for an LTC timecode generator and reader to come.

## 1. Goal

One place that knows every pad of the chip: what the board wired it to, which subsystem holds it,
what the IO_MUX and the GPIO matrix route through it right now, and its live level. On top of
that, an application (`/bin/io`, the I/O window) and a shell command (`io`) that show all of it and
let the user take free pins: drive and read GPIO, change muxing, and stand up a UART, an I²C bus or
an SPI bus on chosen pins in a few clicks, then talk to the device on it from the same window or
from any program through `/dev`.

The kernel's side is the larger part and is useful without the window: today no subsystem records
which pins it uses, nothing stops two from taking the same one, and programs (in U-mode since
`protection.md` step 6) can't touch a pin at all.

**Portable by construction.** Three layers, each replaceable on its own (§3.1): the *chip* (pads,
IO_MUX functions, matrix signals, capabilities: generated from ESP-IDF's `soc` headers), the
*board* (what is wired where, which pins are fixed, connectors and their labels: the board
definition), and BMDS's *ownership* (who holds a pin now: run time). A new ESP32 is a generated
table and a register reader; a new board is a pin table in its `boarddef.h`.

**Designed in from the start, built later:** DMA data paths for every port that moves data
(§3.8), and the RMT as a port of its own (§3.7), so an LTC timecode generator and reader can be
written on the public interface, with timing a clock discipline can steer (`audio-goals`: every
timing API allows it).

**Not goals (now):** a logic analyser beyond edge events with timestamps; analog (ADC, DAC, touch)
beyond showing which pins can; LP-core GPIO programming; taking a pin from a running subsystem
without the user's `--force` (§3.3).

## 2. What is there (checked 2026-11-20)

- **The board definition** (`boarddef.h`) names every pin a BMDS driver uses, as fields of each
  device's struct (`.sdCard.d0 = 39`, `.ethernet.txd1 = 35`...). There is no per-pin table, so
  "who has GPIO 35?" means reading every struct; pins nothing in BMDS drives (the 32 kHz crystal
  on 0/1, USB-Serial/JTAG on 24/25, the free EXT1 pins, UEXT's labels) live only in
  `docs/HARDWARE.md`.
- **Board facts:** `.uart` (UART0 on UEXT 3/4) is defined and nothing opens it. `.i2c[Ext]`
  (I²C1 on UEXT 5/6) *is* opened at boot as `i2c1` by `I2cBus::init()`, with nothing on it
  (corrected while building step 1; this said otherwise at first). UEXT's SPI pins (4, 5, 53, 54)
  aren't in the definition at all.
- **Drivers configure pins directly** through ESP-IDF (23 `gpio_*` calls across storage, audio,
  network, input, the I²C buses). ESP-IDF's own `esp_gpio_reserve()` mask is set by some of its
  drivers (I²C, I²S, SPI, LEDC, RMT...) and by nothing of ours; it only warns.
- **ESP-IDF can read a pad back:** `gpio_get_io_config()` (pulls, drive, IE/OE, open drain, the
  IO_MUX function, the matrix output signal) and `gpio_dump_io_configuration()` (adds the matrix
  inputs by scanning the input selectors). Neither knows a signal's name.
- **The chip (ESP32-P4):** 55 GPIOs (0-54), 0-15 also LP IOs; 5 HP UARTs + LP UART, 2 HP I²C + LP
  I²C, SPI2/SPI3 for general use, 8 LEDC channels, RMT, PCNT, MCPWM, PARLIO. `gpio_sig_map.h`
  names 474 matrix signals (input 0-255, output 0-255, `SIG_GPIO_OUT_IDX` 256 the plain GPIO
  output); `io_mux_reg.h` names each pad's IO_MUX functions (`FUNC_GPIO37_UART0_TXD_PAD` 0,
  `..._GPIO37` 1, `..._SPI2_IO7_PAD` 2). Both are identical in `hw_ver1` (our v1.3 silicon) and
  `hw_ver3`, checked by diff.
- **The pattern for devices to programs** is set: a `/dev` node (`Vfs::addDeviceFile()`), a native
  `_ctl` slot taking an fd and an op (`ptp_ctl`, slot 116; OSS on the audio fds), and Linux's
  ioctls translated onto it in `sdk/crt/linux.c` so ported tools work unchanged.
- **The toolkit** has what the window needs: item views with a sortable header, splitters, tabs,
  `TerminalWidget` (a program on a pseudo-terminal, `editor.md` §7) for a UART console.

## 3. Design

### 3.1 Layers

```
 ┌──── /bin/io (the window) ─── io (the command) ─── programs: /dev/gpiochip0, /dev/i2c-N ... ──┐
 │                                     IoService (bmds_io)                                      │
 │  claims & ownership · pad state · ports (UART/I²C/SPI/RMT on chosen pins, DMA) · events      │
 ├──────────────────────────────┬───────────────────────────────────────────────────────────────┤
 │ IoChip (per chip, generated) │ BoardPins (per board, boarddef.h)                             │
 │ pads, IO_MUX functions,      │ every pad's wiring: fixed/board use, connector + pin + label, │
 │ matrix signal names, caps,   │ on-board pulls, voltage domain, notes; connectors' layouts    │
 │ a register reader/writer     │                                                               │
 └──────────────────────────────┴───────────────────────────────────────────────────────────────┘
```

- **`IoChip`** (`components/bmds_io/chips/esp32p4/`): `constexpr` rodata, no RAM.
  - Pads: number, IO_MUX functions (name and the peripheral signal each one is), LP/RTC capable,
    ADC channel, strapping, voltage domain, input-only/output-capable.
  - Matrix signals: index, name, direction, the peripheral instance it belongs to (`UART1`,
    `I2C0`...), so the window can group them.
  - Peripheral instances: `UART0-4`, `I2C0-1`, `SPI2-3`, `LEDC`, `RMT`... with their signals in
    roles (`UART.TX`, `UART.RX`, `I2C.SDA`...) and which pads reach them through IO_MUX directly
    (faster, needed for high-speed SPI) or only through the matrix.
  - Generated by `tools/gen-iochip.py` from ESP-IDF's `gpio_sig_map.h`, `io_mux_reg.h`,
    `soc_caps.h` and the `*_periph.c` tables, checked in (so builds don't depend on the script)
    and checked by a host test against the headers. Estimated 10-14 KB of flash for the P4.
  - A small `IoChipOps` per chip: read a pad's whole state (built on `gpio_ll`/`gpio_get_io_config`
    plus a scan of the input selectors), set direction, pulls, drive, open drain, level, the IO_MUX
    function, connect a matrix signal. Portable code never touches a register.
- **`BoardPins`**: a new `pins` table in `Board`, one row per pad the board connects:
  `{gpio, use, label, connector, connectorPin, notes}` where `use` is `Fixed` (crystal, USB-JTAG,
  anything a change would break the board: changed only with `--force`), `Board` (wired to an on-board device
  whose driver claims it), or `Header` (brought out, free). Connectors (`UEXT`, `EXT1`) get a
  layout (rows × columns, power and ground pins) for the window's drawing. The existing per-device
  fields stay the drivers' source; a host test checks the two agree, so the pin table can't drift
  from what the drivers use.
- **Ownership** (`IoService`, run time, in `bmds_io`): one byte of owner and a few of state per
  pad (55 pads: ~250 B of internal RAM, allocated once at boot).

### 3.2 Claims

Every driver claims what it uses before configuring it: pads, and the peripheral instances and
channels behind them (`UART1`, `I2C0`, `SPI2`, an RMT channel, an LEDC timer, a DMA channel), all
in one registry of *resources* the chip table enumerates. `Io::claim(resources, Owner{"eth0",
"RMII TXD1"})` → `Result<Claim>`, released by its destructor. A peripheral instance in use is
then as visible as a pin, and a port can't start on an instance a driver holds. Pin classes, shown
everywhere in the same colours:

| Class | Meaning | The user may |
|---|---|---|
| **Fixed** | Wired to something that must not change (board table: the crystal, USB-JTAG) | look; change only with `--force` (§3.3) |
| **System** | Claimed by a running subsystem (Ethernet, SD, audio, the main I²C bus) | look; change only with `--force` (§3.3) |
| **User** | Claimed through `io`, the window, a port, or a program's fd | change, release |
| **Free** | Unclaimed | claim |

A claim is all-or-nothing and fails with `Error::Busy` naming the holder. A claim held by a
process (a line fd on `/dev/gpiochip0`, §3.9) is released when the process ends, and its pins return to
their reset state (input, no pulls) — or the board's resting state for pins with one.

`esp_gpio_reserve()` is set from the claims too, so ESP-IDF's own drivers warn about the same
conflicts.

### 3.3 System and Fixed pins: `--force`

Single user, DOS-like: the user may change anything, but not by accident. A change touching a
System or Fixed pin is refused unless forced, with the reason spelled out:

```
io: GPIO 35 is a system pin: Ethernet (eth0) uses it as RMII TXD1.
    Changing it will probably break that hardware. Use --force to change it anyway.
```

- The kernel's claim takes a `Force` flag; without it, a held pin is `Error::Busy` naming the
  holder (the message above is built from the holder's owner and role). The window asks the same
  question in a box, with the owner named, and its Change button is the force.
- A forced claim *borrows* the pin: the pad's whole state is saved first, the owner's claim stays
  on record ("forced from eth0"), and the owner gets an optional `revoked` call so a driver that
  can notice logs it once instead of failing strangely. Releasing the forced claim restores the
  saved pad state and calls the owner's `restored`. Forced pins are marked in every view until
  then.
- In sysconf (§3.6), a setup that uses a held pin carries `force` in its value, so a saved forced
  setup says so where it is written.

### 3.4 Reading state, and what moves

A pad's state is a few register loads: IE/OE, pulls, drive, open drain, IO_MUX function, the
matrix output signal, input level, output latch; the input selectors (256) are scanned once per
refresh to find which inputs read which pad. The window reads all 55 pads per refresh, only while
it is shown (sysmon's rule, `sysmon.md` §3.4); measured in step 1, estimated well under 50 µs.

Live levels: the window refreshes at a modest rate (settings, default 10 Hz) while shown — no
interrupts on pins someone else owns (Ethernet's REF_CLK would storm). For User pins configured as
inputs, edges come by interrupt into a ring per claim with timestamps from the system clock
(Linux's line events), so a button press between refreshes isn't missed and the window can draw a
short strip of the line's history.

### 3.5 Muxing

A pin's routing, set in its `io/gpio` entry (`14=out,high`, `14=func2`, `14=out:ledc.ch0`,
`14=in:uart1.rxd`) or a program's line request, is one of:

- **GPIO** (IO_MUX function GPIO, `SIG_GPIO_OUT_IDX`): direction, pulls, drive, open drain, level;
- **an IO_MUX function** the pad has (listed from `IoChip`);
- **a matrix signal**: output (one signal per pad) and/or inputs (any number of input signals may
  read one pad), chosen from the chip's list grouped by peripheral.

Routing a raw peripheral signal doesn't start its peripheral: that is what ports are for (§3.6).
Raw routing exists for looking and for experiments (route `LEDC` out to an LED, loop a UART's TX
to another pad). The platform pitfall (`esp32p4.md`: set the direction first, then connect the
signal, or `gpio_set_direction()` takes the pad back) lives in `IoChipOps`, once.

### 3.6 Ports: a peripheral on chosen pins

A **port** is a peripheral instance started on pins the user chose, with a `/dev` node:

Device names are Linux's (the user: as compatible with Linux's API as possible, so applications
port), numbered by the hardware instance:

| Port | Instances (P4) | Node | Then |
|---|---|---|---|
| UART | UART0-4 (UART0 is free on this board: UEXT 3/4) | `/dev/ttySN` | termios; `microcom`, the window's console |
| I²C | I2C0, I2C1 | `/dev/i2c-N` | Linux `i2c-dev`: `I2C_RDWR`, `I2C_SLAVE`, SMBus |
| SPI | SPI2, SPI3; one node per chip select | `/dev/spidevB.C` | Linux `spidev`: `SPI_IOC_MESSAGE`, modes, speed |
| RMT | channels (4 TX, 4 RX on the P4) | `/dev/rmtN` (no Linux equivalent; §3.7) | timed pulse trains out and in |
| GPIO | the chip | `/dev/gpiochip0` | Linux's GPIO character device v2: lines requested on it |

**Settings are sysconf** (the user: "work like sysconf: by default only the session, but can be
saved"; and `sysconf.md` §13's rule, one way to set a thing). Each port instance is one parameter
whose value is its setup, empty when off, declared by `bmds_io` at boot from the chip table
(`declareKernel`, as `net/eth0/...` are):

```
io/uart1   = tx=37 rx=38 baud=115200 format=8n1
io/i2c1    = sda=22 scl=23 clock=100k
io/spi2    = sck=4 mosi=53 miso=54 cs=5 mode=0 clock=1m
io/rmt0    = pin=14 dir=tx resolution=1m
io/gpio    = 14=out,high 15=in,pull-up 16=od,low
```

A watcher in `IoService` applies a change at once: checks it (the pins exist, the instance can
reach them, the pins are free or the value says `force`), claims, routes, starts the ESP-IDF
driver, adds the node; an empty value tears it down. A bad value is refused by the parameter's
check, so `sysconf set` reports why. `sysconf save` keeps it, and the boot applies saved values
after the subsystems start, so a board can come up with its UART on UEXT every time. One key per
peripheral instance and one for plain GPIO keeps the registry's count low (~15 keys; `MaxEntries`
is 256 and 117+ are taken), the form `net/mounts` already uses for a list of entries.

`io/gpio` holds pins the user sets up from the window or `sysconf`: owned by the system on the
user's behalf ("io"), not by a process. A program's lines (`/dev/gpiochip0`) are the program's,
released when it closes them, exactly as on Linux.

The window's New Port (pick the peripheral, then click pads; pads that reach it through IO_MUX
suggested first) and its inspector write these parameters through sysconf, as the Settings
application does, and offer Save.

The system buses BMDS already runs (the main I²C bus, I2C0: display bridge, codec, HDMI DDC) get
their node without a port: `/dev/i2c-0` reaches the existing `I2cBus` through its lock, as Linux's
`i2c-dev` does on a shared bus. Addresses a BMDS driver owns are busy (`EBUSY` without
`I2C_SLAVE_FORCE`, Linux's own rule), so the force rule holds at the bus level too.

### 3.7 The RMT: timed pulses (for LTC and the like)

The RMT turns a list of (level, duration) symbols into a waveform and measures one back into
symbols; the P4 has 4 TX and 4 RX channels, DMA on one of each, TX channels started in sync, and
RX carrier demodulation. An LTC generator is biphase-mark symbols streamed out; an LTC reader is
RX symbols whose durations decode to bits at any speed.

- **A channel is a port** (`io/rmtN`, `/dev/rmtN`): `write()` takes symbols, `read()` returns
  received ones; `poll` works both ways. Linux has no RMT device (its nearest, LIRC's `mode2`, is
  for IR only), so `/dev/rmtN` is ours, its ops on `rmt_ctl` (§3.9).
- **A symbol** is the hardware's own 32-bit word (two level + 15-bit-duration halves), so a buffer
  goes to the channel's memory or its DMA without conversion; the resolution is set per channel.
- **Streaming, not one-shot:** TX takes symbols continuously (ping-pong in channel memory, DMA on
  the DMA channel), so a generator writes frame after frame with no gap; an underrun is counted
  and reported, never silent.
- **Time:** every RX buffer and every TX start carries the system clock's time of its first
  symbol's edge, and the channel reports its tick source's true rate. A generator disciplined to
  the system clock or PTP (`audio-goals`, `network.md` §31) steers by adjusting durations against
  those times; a reader timestamps each LTC frame's sync word. Trimming the RMT's clock in
  hardware, where possible, is a further op, not a different interface.
- **Built on ESP-IDF's `esp_driver_rmt`** (channels, DMA, the sync manager) with BMDS's own copy
  encoder for symbols already in the hardware's format. Measured before committing: if its
  callbacks or allocation can't keep a gapless stream, `rmt_ll` underneath instead, recorded here.

### 3.8 DMA data paths (designed now, built per port)

No port moves its data a byte at a time through an interrupt at speed. Each one's fd path is a
ring of DMA-capable buffers (`MemSpace::Dma`, cache-line aligned, `esp32p4.md`'s cache rules)
filled and drained by the peripheral's DMA, `read`/`write` copying in and out (later, mapped rings
for zero copy):

| Port | DMA on the P4 | Through |
|---|---|---|
| SPI | GDMA (both GP-SPIs) | `spi_master` queued transactions |
| UART | UHCI (one, shared by the UARTs) | ESP-IDF's `uhci` driver; FIFO and interrupt below a measured baud, DMA above |
| RMT | one TX and one RX channel | `esp_driver_rmt`'s DMA backend |
| I²C | none (FIFO; transactions are short) | `i2c_master` |
| Later | PARLIO (parallel), I²S as a raw serial port | their drivers |

DMA channels are resources in the claim registry (§3.2), so the window shows which port holds the
UHCI or an RMT DMA channel, and a second port that wants it is told plainly.

### 3.9 Programs (ABI, append-only)

Following `ptp_ctl` (§2): native slots carry the semantics; `sdk/crt/linux.c` translates Linux's
ioctls onto them, so ported code calls Linux's API unchanged.

- `/dev/gpiochip0`: `gpio_ctl(fd, op, arg)`, ops in the shape of Linux's GPIO v2 uAPI (chip info,
  line info, line requests returning a line fd with direction, bias, drive, edges, debounce;
  values get and set by mask; edge events with timestamps read from the line fd; info-changed
  events on the chip fd). What Linux lacks (pad routing, drive strength, the owner and class) are
  extra ops. Translated: every `GPIO_V2_*` ioctl (and v1's, if a port needs them).
- `/dev/i2c-N`: `i2c_ctl` with `I2C_RDWR`'s message array, `I2C_SLAVE`/`_FORCE`, `I2C_FUNCS`, the
  SMBus ops; plain `read`/`write` at the chosen address.
- `/dev/spidevB.C`: `spi_ctl` with `spi_ioc_transfer`'s array (full duplex, per-transfer speed,
  `cs_change`, delays), mode, bits, speed.
- `/dev/ttySN`: a stream with termios (consoles have it), `TIOCMGET`/`TIOCMSET` for modem lines
  when routed, `TCSBRK`.
- `/dev/rmtN`: `rmt_ctl` (§3.7).
- `<linux/gpio.h>`, `<linux/i2c-dev.h>`, `<linux/i2c.h>`, `<linux/spi/spidev.h>` in the SDK's
  `include/linux` (Linux's UAPI headers, licensed for exactly this), so libgpiod's tools,
  `i2c-tools` and BusyBox's `i2cdetect`/`i2cget` build unchanged (`dist/` recipes) and serve as
  the compatibility tests.

The struct sizes and op numbers go into the ABI lock tests as they ship. Commits touching them
carry `ABI:` lines.

### 3.10 The window (`/bin/io`)

An ordinary toolkit program, built into the firmware like sysmon (`sysmon.md` §3.5): nothing in the
OS knows it, it reaches the kernel through `IoService`'s public classes, and a second start raises
the first (service `io`).

```
┌ I/O ──────────────────────────────────────────────────────────────────────────────────────┐
│ File  View  Pin  Port  Help                                                               │
├─[Pins]─[Board]─[Ports]──────────────────────────────────────┬─────────────────────────────┤
│ GPIO│Lvl│Class │Owner        │Function       │Dir│Pull│Where │ GPIO 37   ● User          │
│  0  │ ~ │Fixed │32 kHz xtal  │—              │ — │ —  │      │ UEXT pin 3 · 3.3 V domain   │
│  2  │ ○ │System│status LED   │GPIO           │out│ —  │EXT1 7│ Routing                     │
│ 14  │ ● │Free  │             │GPIO           │in │ up │EXT1 8│ (•) GPIO  ( ) IO_MUX  ( ) M │
│ 35  │ ● │System│eth0 RMII TX1│EMAC TXD1      │out│ —  │BOOT  │ Out: UART0 TXD   [Change…]  │
│ 37  │ ● │User  │uart0 TX     │UART0_TXD (mux)│out│ —  │UEXT 3│ In:  —                      │
│ 38  │ ● │User  │uart0 RX     │UART0_RXD (mux)│in │ up │UEXT 4│ Pull [up ▾] Drive [20mA ▾]  │
│ …                                                           │ Level ● high  [Set 0] [Pulse]│
│                                                             │ ▁▁▁▔▔▔▁▁▔▔▔▔▁▁ edges         │
├─────────────────────────────────────────────────────────────┴─────────────────────────────┤
│ 55 pins · 6 fixed · 27 system · 2 user · 20 free                                           │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Pins**: every pad in a sortable, filterable table (by class, owner, peripheral, connector),
  live level, classes coloured; selecting one fills the inspector, which edits User pins and shows
  System and Fixed pins read-only until the user forces a change (§3.3).
- **Board**: the connectors drawn from the board table (UEXT, EXT1) with live levels on their
  pins, and the chip's pads by number; clicking a pin selects it.
- **Ports**: running and saved ports; New Port (peripheral, pins, settings); per port a tool:
  - UART: a `TerminalWidget` running `microcom /dev/ttySN` (or our own small console), baud
    switchable;
  - I²C: a scan grid (`i2cdetect`'s), register read/write by address, a byte dump;
  - SPI: a transfer line (hex out, hex in), mode and speed;
  - GPIO: a group of lines with toggles, inputs' strips (§3.4);
  - RMT: captured symbols drawn as a waveform with durations; a pattern sent.

### 3.11 `io`, the command

Scriptable, shell interface frozen once shipped (append only). Following `sysconf.md` §13, `io`
*shows* and *acts*, but settings are set only through `sysconf` (`sysconf set io/uart1 "tx=37
rx=38 baud=115200"`): no `io` form duplicates a parameter.

```
io                          the table (as the window's Pins tab)
io pin 14                   one pin: wiring, class, owner, routing, level
io resources                instances and channels: free or held by whom; ports' nodes
io signals [PERIPHERAL]     the chip's matrix signals and the pads each reaches through IO_MUX
io get 14                   reads a level (reading changes nothing)
io pulse 14 [MS] [--force]  one action on a pin set up as an output
io i2c scan N · io i2c read N ADDR REG [COUNT] · io spi xfer B.C HEX
```

Linux's own tools (`gpioget`, `gpioset`, `gpiomon`, `i2cdetect`) from `dist/` cover the rest the
way they do on Linux.

## 4. IDF usage

- `gpio_ll`/`gpio_hal` and `gpio_get_io_config()` for reads; `gpio_*` and `esp_rom_gpio_connect_*`
  for writes, wrapped once in `IoChipOps` (the direction-then-signal order there).
- `esp_gpio_reserve()` fed from claims (§3.2).
- Ports on ESP-IDF's `uart`, `i2c_master` and `spi_master` drivers; edges on `gpio_isr_handler_add`
  (one shared ISR service, already installed by the SD card's detect).
- Signal and function names generated from `soc` headers, not hand-typed (§3.1).

## 5. Budget (estimates; measured as steps land)

| What | Where | Estimate |
|---|---|---|
| `IoChip` tables (P4) | flash rodata | 10-14 KB |
| Board pin table | flash rodata | < 1 KB |
| Ownership and state | internal RAM, boot | ~250 B |
| `bmds_io` code | flash | 8-12 KB |
| Edge rings | PSRAM, per claimed input line | 64 events × 16 B |
| `/bin/io` | flash | 30-45 KB |
| A port's DMA rings | internal RAM | per port, from its setup; none until started |
| A refresh of all pads | CPU, only while shown | < 50 µs, 10 Hz |

## 6. Steps

1. **The model, read-only.** `bmds_io` with `IoChip` for the P4 (generator, tables, host test
   against the headers), `BoardPins` for the Olimex board (host test against the device fields),
   claims taken by every existing driver, `io` / `io pin N`. Check every pad's reading against
   `gpio_dump_io_configuration()` on the board.
2. **The window, read-only.** Pins and Board tabs, the inspector, live levels while shown.
3. **GPIO and muxing.** `io/gpio` and raw routing through sysconf, forced claims with saved and
   restored pad state (§3.3), edges with timestamps; the inspector writing them; `io get|pulse`.
4. **Ports.** UART, I²C, SPI on chosen pins from `io/<instance>`, their `/dev` nodes, `/dev/i2c-0`
   on the system bus; the Ports tab's tools. FIFO data paths first, measured.
5. **Programs.** `gpio_ctl`, `i2c_ctl`, `spi_ctl` slots, the Linux headers and ioctl translation,
   ABI lock tests, `dist/` recipes for libgpiod's tools and `i2c-tools`; an SDK example.
6. **The RMT** (§3.7): `/dev/rmtN`, `rmt_ctl`, streaming TX and RX with timestamps, DMA on its
   channels; an SDK example sending and capturing an LTC-shaped stream, ready for the user's
   generator and reader.
7. **DMA paths** (§3.8): SPI by GDMA, UART by UHCI above a measured baud, the rings.
8. **Later.** LEDC/PWM, PCNT, PARLIO as port kinds, ADC readings on capable pins (the battery
   sense on 20), a longer logic strip, a second chip (an S3 or C6 board proves the chip layer).

## 7. Decisions (the user's, 2026-11-20)

1. **Built into the firmware:** `/bin/io`, like sysmon.
2. **Linux's names and API:** `/dev/gpiochip0`, `/dev/i2c-N`, `/dev/spidevB.C`, `/dev/ttySN`, with
   Linux's ioctls: "as compatible as possible with the Linux API so porting applications is
   possible" (§3.6, §3.9).
3. **Like sysconf:** ports and pin setups last the session unless saved; they *are* sysconf
   parameters (§3.6).
4. **System pins need `--force`:** otherwise the user is told it is a system pin and that changing
   it would probably break existing hardware (§3.3).
5. **The right way, no shortcuts:** proper kernel interfaces; DMA-driven paths and the RMT (for
   the user's LTC timecode generator and reader) designed in now (§3.7, §3.8).

## 8. As built: step 1, the model (2026-11-20)

- **`bmds_io`** (`components/bmds_io`), a pure part host-tested with the P4's table and a target
  part:
  - `IoChip` (`bmds/iochip.h`): pads with their IO_MUX functions, LP IO, analog functions,
    strapping and notes; the matrix's input and output signal names (37 groups: `UART0`, `I2C0`,
    `SPI2`...); resource kinds after the pads (`uart0-4`, `lp-uart0`, `i2c0-1`, `lp-i2c0`,
    `spi2-3`, `i2s0-2`, `rmt0-7`, `ledc0-7`, `ledc-timer0-3`, `pcnt0-3`, `mcpwm0-1`, `twai0-2`,
    `sdmmc0-1`, `emac0`, `uhci0`, `parlio0`, `usb-serial-jtag0`: 104 resources with the 55 pads).
    Lookups by name either way.
  - `tools/gen-iochip.py esp32p4` writes `chips/esp32p4/iochipdef.cpp` from ESP-IDF's
    `gpio_sig_map.h`, both `io_mux_reg.h`s (refusing if they differ), `soc_caps.h` (counts
    checked) and the GPIO documentation table (`docs/platforms/esp32p4.md` says what each gives).
    Checked in; the host test `iochip_table` runs `--check` against the ESP-IDF on the host.
  - `IoRegistry` (`bmds/ioregistry.h`): owners (48, named, system or user) and resources (at most
    192), claims all or nothing (`Busy` naming the first that refused), `IoClaim` releasing on
    destruction, `Force` lending a resource (the lender kept and restored; one borrower at a
    time), a user's claim refused board-wired pads without `Force`, classes Free / Fixed / System
    / User, a generation number for viewers; host-tested (`tests/host/io.cpp`).
  - `IoPadState` and its words (`bmds/iopad.h`): route (`GPIO`, `UART0_TXD (IO_MUX)`, matrix
    signals, `LP IO`), direction (`in`, `out`, `in/out`, `periph`, `,od`), pulls.
  - `Io` (`bmds/io.h`): the boot stage `io` (after `console`, before `i2c`; in the recovery
    firmware too), the system's registry in PSRAM, the board's pads recorded from its table,
    `Io::claim()` for drivers, `Io::busyText()` (`GPIO35 is a system pin: eth0 uses it as RMII
    TXD1.`), `Io::readPad()` and `Io::readInputSources()` on the chip's `IoChipOps`
    (`chips/esp32p4/iochipops.cpp`: `gpio_ll`, the GPIO matrix, the LP IO mux).
  - `io`, `io pin N`, `io get N`, `io resources`, `io signals [GROUP]`, and `io dump [N]`
    (ESP-IDF's own `gpio_dump_io_configuration()`, through an `fmemopen()` stream: an independent
    reading to check `io`'s against) (§3.11).
  - Each IO_MUX function says whether it is a peripheral's (`IoMuxFunction::peripheral`, from the
    generator): a pad left at its reset function (`GPIOn_0`) drives nothing, and shows as idle
    (`-`), not `periph`.
- **The board's table** (`board.h`: `BoardPin`, `BoardConnector`; `boarddef.h`: `BoardPins`,
  UEXT and EXT1 from the Rev C netlist). Three `static_assert`s: the pins sorted and once each,
  every GPIO a device field names listed, every connector GPIO listed.
- **Claims by every driver that drives pins:** `i2c0`/`i2c1` (pads and controller), `sd0`
  (the slot's six pads, detect, power, `sdmmc0`), `dsp0` (I²S pads, codec power, `i2s0`),
  `eth0` (RMII, MDC/MDIO, PHY reset, PPS out, `emac0`), `usb0` (the hub reset). The status LED,
  the BOOT button's GPIO35 (Ethernet's) and the sense lines aren't driven by anything: Free (35
  is Ethernet's).
- **Cost:** 14.4 KB of flash (`libbmds_io.a`: 8.3 KB code, 6.2 KB rodata, with `io dump`), 4 B of internal RAM,
  the registry ~3.3 KB of PSRAM.
- **Checked on the board (bmds0, 2026-11-20):**
  - All 55 pads' readings against `io dump`, field by field (a host script over both outputs):
    pulls, drive, input and output enable (the peripheral's enable included), open drain, IO_MUX
    function, the matrix output signal and the matrix inputs read from each pad: no difference.
    ESP-IDF's own reserve mask has only the I²C pads (7, 8, 22, 23: its I²C driver's); feeding it
    from claims (§3.2) is left to step 3.
  - Levels move: GPIO26 (Ethernet's PPS, 1 Hz, 100 ms high) sampled every ~50 ms read 1 for two
    samples each second.
  - The LP IO mux's registers are readable: their APB clock gate is on from reset and ESP-IDF
    never turns it off; the reader checks the gate anyway.
  - The `io` boot stage takes 103 µs; a whole `io` table under 10 ms (the shell's `time`
    resolution), printing included. The boot claims raise no conflict.
  - What the board's pads are at boot: 2, 4 and 5 sit in JTAG's IO_MUX functions (`MTCK`,
    `MTMS`, `MTDO`), 37 and 38 in UART0's (the ROM's console, with pull-ups), the rest unused
    at function 0; RMII, SD and MDC through IO_MUX; I²C, I²S, MDIO through the matrix.

