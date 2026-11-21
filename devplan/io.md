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
that, an application (`/bin/ioman`, the I/O Manager) and a shell command (`io`) that show all of
it and let the user take free pins: drive and read GPIO, change muxing, and stand up a UART, an
I²C bus or an SPI bus on chosen pins in a few clicks, then talk to the device on it from the same
window or from any program through `/dev`.

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
 ┌─── /bin/ioman (the window) ─── io (the command) ─── programs: /dev/gpiochip0, /dev/i2c-N ... ┐
 │                                         Io (bmds_io)                                         │
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
- **Ownership** (`Io`, run time, in `bmds_io`): one byte of owner and a few of state per
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

A watcher in `Io` applies a change at once: checks it (the pins exist, the instance can
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

### 3.10 The window (`ioman`, in bmds-apps)

An ordinary SDK program in the bmds-apps repository (§7, decision 6; its design is bmds-apps'
`devplan/ioman.md`): nothing in the OS knows it, it reaches the kernel through `gpio_ctl` on
`/dev/gpiochip0` (§3.9) and the GUI service's classes, and a second start raises the first
(service `ioman`). Not `io`: the shell finds its own `io` before any program of that name.

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
i2c scan N · i2c read N ADDR REG [COUNT] (its own command: §11.4) · io spi xfer B.C HEX
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
| `gpio_ctl`, `/dev/gpiochip0` | flash | 3.7 KB measured (`ioman` itself is a package on the card) |
| A port's DMA rings | internal RAM | per port, from its setup; none until started |
| A refresh of all pads | CPU, only while shown | < 50 µs, 10 Hz |

## 6. Steps

1. **The model, read-only.** `bmds_io` with `IoChip` for the P4 (generator, tables, host test
   against the headers), `BoardPins` for the Olimex board (host test against the device fields),
   claims taken by every existing driver, `io` / `io pin N`. Check every pad's reading against
   `gpio_dump_io_configuration()` on the board.
2. **The window, read-only.** Pins and Board tabs, the inspector, live levels while shown; as a
   bmds-apps package (§7, 6), so with `/dev/gpiochip0` and `gpio_ctl`'s reads (chip and line
   info, Linux's translated; pads' state and details, connectors), ABI-locked, and an SDK example.
3. **GPIO and muxing.** `io/gpio` and raw routing through sysconf, forced claims with saved and
   restored pad state (§3.3), edges with timestamps; the inspector writing them; `io get|pulse`.
4. **Ports.** UART, I²C, SPI on chosen pins from `io/<instance>`, their `/dev` nodes, `/dev/i2c-0`
   on the system bus; the Ports tab's tools. FIFO data paths first, measured.
5. **Programs.** `gpio_ctl`'s line requests (its reads came with step 2), `i2c_ctl`, `spi_ctl`
   slots, the Linux headers and ioctl translation,
   ABI lock tests, `dist/` recipes for libgpiod's tools and `i2c-tools`; an SDK example.
6. **The RMT** (§3.7): `/dev/rmtN`, `rmt_ctl`, streaming TX and RX with timestamps, DMA on its
   channels; an SDK example sending and capturing an LTC-shaped stream, ready for the user's
   generator and reader.
7. **DMA paths** (§3.8): SPI by GDMA, UART by UHCI above a measured baud, the rings.
8. **Later.** LEDC/PWM, PCNT, PARLIO as port kinds, ADC readings on capable pins (the battery
   sense on 20), a longer logic strip, a second chip (an S3 or C6 board proves the chip layer).

## 7. Decisions (the user's, 2026-11-20)

1. **Built into the firmware:** `/bin/io`, like sysmon. Revised by 6.
2. **Linux's names and API:** `/dev/gpiochip0`, `/dev/i2c-N`, `/dev/spidevB.C`, `/dev/ttySN`, with
   Linux's ioctls: "as compatible as possible with the Linux API so porting applications is
   possible" (§3.6, §3.9).
3. **Like sysconf:** ports and pin setups last the session unless saved; they *are* sysconf
   parameters (§3.6).
4. **System pins need `--force`:** otherwise the user is told it is a system pin and that changing
   it would probably break existing hardware (§3.3).
5. **The right way, no shortcuts:** proper kernel interfaces; DMA-driven paths and the RMT (for
   the user's LTC timecode generator and reader) designed in now (§3.7, §3.8).
6. **The window lives in bmds-apps** (the user, 2026-11-20, after step 2's built-in window: "We
   can make the ioman app live in the bmds-apps repo"), so the read half of `gpio_ctl` (§3.9)
   comes with step 2 rather than step 5 (the user chose that over keeping the window built in
   until step 5, or doing step 3 first).

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
- **Cost:** 14.4 KB of flash (`libbmds_io.a`: 8.3 KB code, 6.2 KB rodata, with `io dump`), 4 B
  of internal RAM, the registry ~3.3 KB of PSRAM.
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


## 9. As built: step 2, the window, read-only (2026-11-20)

The window was first built into the firmware (`/bin/ioman`, a toolkit program like sysmon); the
user then moved it to bmds-apps (§7, 6), so it was rebuilt there on the SDK's GUI classes and
the kernel gained the program interface it reads through. What this repository has:

- **`/dev/gpiochip0` and `gpio_ctl`** (slot 139; `bmds/sys.h`; the kernel's in
  `bmds_loader/src/gpiocalls.cpp`, the device file `GpioChip` in `bmds_io`, added by the boot
  stage `gpio` after `vfs`). Reading takes nothing and changes nothing. Operations:
  - `BMDS_GPIO_CHIP_INFO`: lines (pads), connectors, `gpiochipN`, the chip's and board's names.
  - `BMDS_GPIO_LINE_INFO`: a line as Linux says it (`GPIO_V2_LINE_FLAG_*` bit for bit; the
    board's label as its name, else `GPIO14`; the holder as its consumer; USED for any pad
    that isn't Free).
  - `BMDS_GPIO_PINS`: pads' state from a first one, one read of the chip for all (the matrix's
    256 input selectors once): number, class, level, IO_MUX function, drive, flags (input,
    output, peripheral-driven, open drain, pulls, LP IO, strapping, forced), and the words
    `io` prints (owner, use, route, direction, pull, connector pin). `pin_size` lets the
    record grow.
  - `BMDS_GPIO_PIN_DETAIL`: one pad's every detail as labelled lines (`io pin N`'s), at most
    24, as many as the caller's `size` has room for.
  - `BMDS_GPIO_CONNECTOR`, `BMDS_GPIO_CONNECTOR_PIN`: the board's connectors and their pins.
  - Every struct starts with its `size` (or carries `pin_size`); offsets and sizes are in
    `abilock.c`.
- **Linux's GPIO character device** (`sdk/include/linux/gpio.h`, Linux's layouts and numbers;
  `crt/linux.c`): `GPIO_GET_CHIPINFO_IOCTL`, `GPIO_GET_LINEINFO_IOCTL`,
  `GPIO_V2_GET_LINEINFO_IOCTL` (what `gpiodetect` and `gpioinfo` ask). The rest (line requests,
  watches) fail with `EOPNOTSUPP` until step 3.
- **`sdk/examples/gpioinfo`:** every line through Linux's ioctls, as libgpiod's `gpioinfo`
  prints it; `-b` for `BMDS_GPIO_PINS`' table.
- **Shared with `io`:** a pad's row and details are made in one place, `bmds/iopin.h` (pure,
  host-tested): `IoPinRow` (a table's texts and the pad's state) and `IoPinDetail` (labelled
  lines by `IoPinField`); `Io::boardOf()`, `Io::readPins(first, ...)`, `Io::readPin()` on the
  system. `io`, `io pin N` and `gpio_ctl` all answer from them. `ioPeripheralDriven()` says
  whether a peripheral switches a pad's output (what `periph` means).
- **The window** is `ioman` in bmds-apps (its `devplan/ioman.md`): Pins (the table with each
  pad's class colour and level as its icon, filtered by class or connector, sortable; the chosen
  pin's details beside it) and Board (the connectors as their headers are laid out, then every
  pad; a click opens the pin on Pins), refreshed ten times a second while shown and not
  minimized, sending only what changed. Its package needs a BMDS newer than 0.4.0.
- **Cost:** `gpio_ctl` 3.3 KB of flash (`gpiocalls.cpp`), the device file 0.4 KB and 4 B of
  internal RAM; `libbmds_io.a` is 16.0 KB (the shared rows and details included, 1.6 KB more
  than step 1). A `BMDS_GPIO_PINS` call borrows ~9 KB of PSRAM for its rows while it runs.

## 10. Step 3: GPIO and muxing (design, 2026-11-20)

Built in three parts, each checked on the board before the next: **3a** the kernel (`io/gpio`,
forced claims with the pad saved and restored, `io pulse`), **3b** edges with timestamps on User
inputs, **3c** the I/O Manager's inspector writing `io/gpio`.

### 10.1 `io/gpio`

One `String` parameter (§3.6), entries separated by spaces, `PIN=WORD,WORD...`; `PIN` is `14` or
`GPIO14`. The words, in any order:

| Word | Meaning |
|---|---|
| `in` | GPIO input (the default) |
| `out` | GPIO output, push-pull |
| `od` | GPIO output, open drain (its input on: the line reads back) |
| `off` | neither input nor output (the pad left to its pulls) |
| `high`, `low` | an output's level (`low` by default) |
| `pull-up`, `pull-down` | the weak pull (none by default) |
| `drive=0`..`3` | drive strength (2 by default, the chip's reset value) |
| `func=N`, `func=NAME` | IO_MUX function N, or by its signal's name (`func=UART0_TXD`) |
| `out:SIGNAL` | a matrix output signal drives it (with `out` or `od`: its level then the signal's) |
| `in:SIGNAL` | a matrix input signal reads it (several allowed; with any mode, its input on) |
| `force` | take it from its holder, or a board-wired pad (§3.3) |

`14=out,high 15=in,pull-up 16=od,low 2=func=MTCK 20=out:LEDC_LS_SIG_OUT0 38=in:UART1_RXD,pull-up`.
Signal and function names are the chip's (`io signals`), matched without case.

- **The check** (sysconf's, so `sysconf set` refuses a bad value with the reason): every pad
  exists and appears once; a function the pad has; signals that exist; an input signal read by
  one pad only; and, without `force`, no pad held by another owner or wired by the board (Fixed,
  Device), and no matrix signal whose peripheral instance another owner holds. The refusal names
  the holder (`Io::busyText()`), and how to go on: `... Add force to change it anyway.`
- **Applying** (a watcher, on the task that set it): the entries are compared with those applied;
  a pad that left is released, one that came is claimed for the owner `io` (User; forced when
  its entry says `force`), one that changed is configured again. Every pad's registers are saved
  when it is claimed and written back when it is released: a forced pad goes back to its lender
  as it was (and the lender holds it again), a free pad to what it was before (JTAG's function on
  2, 4, 5). An input signal routed from the pad keeps its old source, put back with the pad.
  The order on the pad: function, level, direction, then the matrix signal (`esp32p4.md`'s
  pitfall). A forced pad logs once, both ways: `io: GPIO35 taken from eth0 (RMII TXD1)`.
- **At boot** the saved value is applied by the stage `iogpio`, after the drivers have claimed
  theirs, so a saved forced setup takes its pads from a running subsystem, as it did when set.
- **Pads 24-27:** choosing a function there switches the USB PHYs off (`gpio_ll_func_sel()`);
  the save and restore include those bits, so forcing the console's pads and letting go brings
  USB-Serial/JTAG back.

### 10.2 Revisions to §3.2 and §3.3

- **ESP-IDF's reserve mask isn't fed from claims.** It is ESP-IDF's drivers' own bookkeeping of
  output paths, and they act on it: Ethernet's MAC refuses a reserved pad
  (`esp_eth_mac_esp_gpio.c`: `ESP_ERR_INVALID_STATE`), I²C and I²S warn and go on, and each revokes
  its pads when it stops, which would take BMDS's marks off. Fed from claims taken before the
  drivers start, it would stop eth0 starting. BMDS's own registry refuses the same conflicts
  before any ESP-IDF driver runs, so the mask is left to ESP-IDF.
- **Owners aren't called when a pad is forced from them** (the `revoked`/`restored` calls): no
  driver yet could do anything with it, so it waits for one that can; the log says it instead.

### 10.3 `io pulse` and the reason a value is refused

- `io pulse PIN [MS] [--force]` (10 ms by default, 1-10000): an output's level inverted for `MS`,
  then put back. A pad `io/gpio` has as an output, or with `--force` any output pad (a reset line a
  driver holds): borrowed for the pulse, its registers saved and restored, the holder's claim
  given back. A pad that isn't an output is refused: it would only float.
- `SysConf::refuse(why)`: a check may leave the reason it refuses a value; `SysConf::set(key,
  value, &why)` gives it to the caller (checks run one at a time, so the reason can't mix with
  another's), and `sysconf set` prints it under its error. Programs' `sysconf_set` doesn't carry
  it yet (3c, for the window).

### 10.4 As built: 3a (2026-11-20)

- `bmds/iosetup.h` (pure, host-tested): `IoGpioSetup::parse()` and `IoPinSetup`, every word of
  §10.1, refusals with the reason (`GPIO14: 'sideways' isn't a word io/gpio knows`).
- `iogpio.cpp`: `io/gpio` (`BMDS_SYSCONF`, its check `Io::checkSetups()`), the watcher applying
  it, the owner `io` (User), the boot stage `iogpio` after `netmidi`; `Io::pulse()`, `io pulse`.
  `IoChipOps::savePad()`, `restorePad()`, `configurePad()`, the input selectors' words.
- `SysConf::refuse()` and `set(key, value, &why)`; `sysconf set` prints the reason.
- **Checked on bmds0:** `2=out,low` (the green LED, in JTAG's `MTCK` at boot) set up, pulsed,
  and let go back to `MTCK` exactly; `io pulse 3` refused (an input); `26=in` refused with eth0
  named, `26=in,pull-down,force` taken ("Forced: from eth0 (PPS out)") and given back, eth0
  holding it again; `14=in:UART1_RXD` routed and its old source (none) put back;
  `15=in:I2S0_I_SD` refused (dsp0 holds i2s0); `2=out,low` saved, applied at boot by `iogpio`;
  `15=out,high` set, not saved, and put back by a `reboot` (the restart hook below).
- **A restored output driven by events** (the PPS, toggled by the ETM) keeps the latch it was
  saved with until its next edge: after the give-back the PPS read high for about a second, then
  ran as before (100 ms high, 1 Hz). A pad's driver is never told (§10.2), so a pad set by
  software in the meantime keeps the saved level until the driver writes it again.
- **A restart keeps the pads.** `esp_restart()` resets the CPUs and a few modules, not IO_MUX or
  the GPIO matrix (`esp_system/port/soc/esp32p4/system_internal.c`), so pins set up by `io/gpio`
  stayed set up into the next boot, where applying the saved value saved *them* as "what it was
  before": letting go after a `reboot` left the LED's pin a GPIO output instead of `MTCK`. A
  restart hook (`System::addRestartHook()`, run by `System::restart()`, which every reboot and
  update goes through) now puts every pad `io/gpio` has back first. Not ESP-IDF's shutdown
  handlers: there are five (`SHUTDOWN_HANDLERS_NO`), all taken, and registering failed. A panic
  runs no hooks: after one, the pads stay as they were until power-on.

### 10.5 3b: edges with timestamps (design, 2026-11-20)

- **Words in `io/gpio`:** `edges` (both ways), `rising`, `falling`; `debounce=US` (0 to
  1,000,000): an edge less than `US` after the last one taken is dropped, so a bouncing contact
  gives one (the level read at the next refresh is the truth either way). Only on a pad whose
  input is on (`in`, `od`, `in:SIGNAL`, an IO_MUX function); `out` and `off` refuse them.
- **The interrupt:** ESP-IDF's per-pad GPIO interrupt service (installed already by the SD card's
  detect; installed here if not), its type set to the edges asked. The handler reads the level
  and `Detail::monotonicUs()` (IRAM, ISR-safe) and writes one record into the pad's ring. The
  service isn't an IRAM one, so an edge during a flash write waits for it and is stamped late;
  noted, and good enough for buttons and signals a person watches.
- **The ring:** 64 records of 16 bytes (time in ns since boot, a sequence number, rising, the
  level), in PSRAM, one per watched pad from a pool of 8 (8 KB at most, none until asked). One
  writer (the interrupt); readers copy what is newer than the sequence they last saw, and a record
  overwritten while it was copied is counted lost, not shown wrong (its sequence is checked
  after the copy). Lost records (more than 64 between reads) are counted too.
- **Readers:**
  - `gpio_ctl`'s `BMDS_GPIO_EDGES` (appended): a pad's records after a sequence, how many were
    lost, the sequence to ask from next; `-BMDS_ENOENT` for a pad not watched. The pin record's
    flags gain `BMDS_GPIO_PIN_EDGES`. A program polls it (the I/O Manager's strip, 3c); waiting
    on an fd comes with line requests (step 5), as Linux's line events.
  - `io edges N [-f]`: the pad's recent edges, the time before now and the way; `-f` follows them
    until interrupted.
  - `io pin N`: an `Edges` line (`both, debounce 2000 µs, 14 seen, 0 lost`) for a watched pad.

### 10.6 As built: 3b (2026-11-20)

- `IoPinSetup::edges`, `debounceUs`, `readsInput()`; the words `edges`, `rising`, `falling`,
  `debounce=US`, refused on a pad whose input is off (host-tested).
- `iogpio.cpp`: a pool of 8 rings (`EdgeRing`, 64 records of 16 bytes, PSRAM, made when first
  wanted and kept), `edgeIsr()` on ESP-IDF's GPIO ISR service (installed by `iogpio` if the SD
  card hasn't), `Io::edgeInfo()`, `Io::readEdges()`; a pad's watch begins with its setup and ends
  with it (the interrupt off and its handler removed before the pad is restored).
- `io edges N [-f]`, `io pin`'s `Edges` line (`IoPinField::Edges`, appended, shown only when
  watched: `ioPinFieldIsOptional()`), the pin record's `BMDS_GPIO_PIN_EDGES`, `gpio_ctl`'s
  `BMDS_GPIO_EDGES` (appended, ABI-locked), `gpioinfo -e N`.
- **Checked on bmds0** with GPIO14 as `od,high,pull-up` (its own pulses are its edges):
  three 50 ms pulses gave six edges, alternating, 50 ms apart, the same through `io edges` and
  `gpioinfo -e` (times since boot as `uptime` says); `debounce=20000` took a 5 ms pulse's falling
  edge only; `rising` took rising edges only; 80 edges with nobody reading kept the last 64,
  16 counted lost; letting the pad go ended the watch. `-f` wasn't run on the board (it stops
  only when interrupted).

### 10.7 3c: the I/O Manager writes `io/gpio` (design, 2026-11-20)

- **The window** (bmds-apps' `ioman`, its `devplan/ioman.md`): the inspector gains an editor over
  the chosen pin's `io/gpio` entry: its mode (not set up, in, out, open drain, off), level, pull,
  edges and debounce, and a line for the rest of its words (`func=`, `out:`, `in:`); Apply,
  Release (its entry taken out), Save (`io/gpio` saved). It writes the whole value through
  `sysconf_set`, its own entry changed and the others kept, as `sysconf set` would. A pin held by
  another owner asks first, in the kernel's words, and Change Anyway adds `force`. A watched pin's
  last edges are drawn in the inspector as a strip and a list, read with `BMDS_GPIO_EDGES`.
- **The reason reaches programs:** `sysconf_get`'s field `BMDS_SYSCONF_FIELD_REFUSAL` (appended)
  gives the calling process's last refused `sysconf_set` of that parameter in its check's words.
  The process keeps the last one (its id and the text, in PSRAM once one is refused); a set taken
  since clears it.
- **One way to set a thing** (`sysconf.md` §13): the window sets `io/gpio`, as `sysconf set` does;
  nothing else changes a pin. `io pulse` stays the shell's: a pulse is an action, not a setting,
  and the window's Toggle is the level changed in `io/gpio`.

### 10.8 As built: 3c (2026-11-20)

- `BMDS_SYSCONF_FIELD_REFUSAL` (appended, ABI-locked): `Process::sysconfSet()` keeps its last
  refused set (the id, the check's words in PSRAM); `sysconfGet()` gives them for that id, ""
  otherwise; a set taken since clears it (host-tested).
- bmds-apps' `ioman`: the editor over the chosen pin's entry (Mode, Level, Pull, Edges, Debounce,
  Other words; Apply, Release, Save), writing `io/gpio` whole with the others kept; a held pin's
  refusal shown in a box in the kernel's words, Yes adding `force`; a watched pin's last 2 s as a
  strip (`▔` high, `▁` low, 32 columns) and its last six edges at the top of the details.
- **Checked on bmds0:** GPIO2 set up as an output from the window (`2=out,low`, its row green);
  GPIO21 (usb0's hub reset) asked "GPIO21 is a system pin: usb0 uses it as hub reset. ...
  Change GPIO21 anyway?", answered No (nothing changed); GPIO14 set up as
  `od,high,pull-up,edges`, three pulses from `io pulse` listed (six edges) and drawn in the
  strip; `io/gpio` put back to its default afterwards, nothing saved.

## 11. Step 4: ports (design, 2026-11-20)

In parts, each checked on the board before the next: **4a** UART (`io/uartN`, `/dev/ttySN`),
**4b** I²C (`io/i2cN`, `/dev/i2c-N`, the system bus as `/dev/i2c-0`), **4c** SPI, **4d** the I/O
Manager's Ports tab.

### 11.1 4a: UART

- **`io/uart0` .. `io/uart4`** (sysconf, empty: off): `tx=37 rx=38 [rts=N] [cts=N]
  [baud=115200] [format=8n1] [flow=rtscts] [loopback] [force]`. `format` is the data bits (5-8),
  the parity (`n`, `e`, `o`) and the stop bits (1, 2); `loopback` ties TX to RX inside the UART
  (a port tried without wires); `force` as `io/gpio`'s. At least one of `tx` and `rx`; a pin once.
  The check refuses, in words, what `io/gpio`'s does (held or board-wired pins without `force`)
  and a UART another owner holds.
- **The port:** the owner `ttySN` (User) holds the pins and `uartN`; each pin's registers are
  saved and put back when the port stops (`IoChipOps`, as `io/gpio`), and its signals go through
  the GPIO matrix (`UARTn_TXD`, `_RXD`, `_RTS`, `_CTS`; a UART's rates are far inside the
  matrix's). A changed value stops the port and starts it again.
- **`/dev/ttySN` is a `Console`** (as the USB serial console): a terminal for `term_get` and
  `term_set`, a shell's if one is started on it, readable for `poll` as input arrives, Ctrl-C a
  signal when its mode says so. It starts **binary** (no signals, no `\n` → CR LF): a serial
  line carries a device's bytes as they are, and a program sets the modes it wants. Present in
  `/dev` while the port runs.
- **Its own interrupt over the UART's LL layer**, not ESP-IDF's driver (revising §4): the USB
  console's design, so input arriving wakes pollers and Ctrl-C signals from the receive path,
  with no task per port. The receive interrupt (FIFO full at 64, or idle after 10 symbols)
  copies into a 2 KB ring, the transmit one refills the 128-byte FIFO from a 1 KB ring; both in
  PSRAM (the interrupt isn't an IRAM one). Bytes lost to a full ring, framing and parity errors
  are counted (`io pin`'s port line; the counters for sysmon come with 4d).
- **Speed and format from programs** (termios' `cfsetspeed`, `CSIZE`, `PARENB`, `CSTOPB`,
  `CRTSCTS`, `TCSBRK`, `TIOCMGET`): with step 5's ioctls, as `term_ctl` operations. Until then
  the port runs as `io/uartN` says and a program's speed is taken as given.

### 11.2 As built: 4a (2026-11-20)

- `bmds/ioport.h` (pure, host-tested): `IoUartSetup::parse()`, every word of §11.1, refusals in
  words. `ioports.cpp`: `io/uart0`..`4` (`BMDS_SYSCONF`, a check each), a watcher on `io`, the
  owner `ttySN` holding the pins and `uartN`, `/dev/ttySN` added when the port first starts
  (present while it runs), a restart hook putting the pins back. `uartconsole.cpp`: the
  `Console`. `iosetups.cpp`: what `io/gpio` and the ports share (who else holds a resource, a
  refusal in words).
- **The order on start:** the inputs (RX, CTS) routed while the UART is off, the UART started
  (its FIFO cleared), then the outputs (TX, RTS) routed, idle high from their first instant.
  Routed the other way round, the switch of RX to its pad read as a byte (`\xff` before the
  first line, seen on the board).
- **Found on the board:** `uart_ll_get_txfifo_len()` on the P4 is the room left in the FIFO,
  not what it holds. Taken the other way, the transmit interrupt wrote nothing, stayed raised,
  and ran until the interrupt watchdog reset the chip (a panic: no restart hooks, so GPIO14 kept
  `UART1_TXD` into the next boots until a power cycle). The receive loop also stops at a FIFO's
  worth a pass now, whatever the count says.
- **`/dev`'s table** (`Vfs::MaxDeviceFiles`) was full at 32 with three ports' nodes: 40 now
  (224 bytes of internal RAM), and a port's node is added only when it first starts.
- **Checked on bmds0** (UART loopback: no wires): `hello-uart` written and read back; 1800
  bytes (more than the 1 KB transmit ring) through and identical; 9600 baud `7e2` and 57600
  after a change, no stray bytes; `tx=26` refused naming eth0; stopped: `/dev/ttyS1` gone, the
  pins back as they were; an unsaved port's pins put back by a `reboot`.
- About 13.7 KB of code (the object files' text), the rings 3 KB of PSRAM per port started.

### 11.3 4b: I²C, `/dev/i2c-N` on the system's buses (design, 2026-11-20)

- **No I²C port on chosen pins for now:** the P4 has two I²C controllers and BMDS runs both, the
  main bus (`i2c0`: display bridge, codec, HDMI DDC) and UEXT's (`i2c1`); the LP I²C reaches
  only pads 0-15. So 4b is §3.6's other half: **the system's buses as Linux's `/dev/i2c-N`**,
  over the existing `I2cBus`, without a port. `io/i2cN` waits for a chip or board with a free
  controller.
- **A file per open:** opening `/dev/i2c-N` makes a client (a small stream in PSRAM, gone with
  its last handle) holding that file's target address and force flag, as Linux's i2c-dev's
  `i2c_client` per open file. `read()` and `write()` are a plain read or write at the target.
- **`i2c_ctl(fd, op, arg)`** (slot 140): `BMDS_I2C_INFO` (the bus, its clock, the file's
  target and force, what it can do), `BMDS_I2C_SET_TARGET` (an address, and force),
  `BMDS_I2C_TRANSFER` (messages, Linux's `i2c_msg` in shape: address, read or write, length,
  buffer; one transaction with repeated starts, at most 42 messages as Linux).
- **Addresses a BMDS driver owns** (an `I2cBus` device: the codec, the bridge, DDC's EDID) are
  `EBUSY` unless the file's force is set (`I2C_SLAVE_FORCE`), for `SET_TARGET` as Linux and for
  `TRANSFER` too (Linux lets `I2C_RDWR` through; here a driver's chip changed behind it is what
  `force` exists for).
- **The bus as ESP-IDF drives it:** one raw device per bus whose transfers are ESP-IDF's
  "defined operations" (START, WRITE of the address byte and data, READ, STOP), so any message
  sequence goes on the wire as given; the bus's own lock keeps transactions whole between the
  drivers and the files.
- **Linux:** `linux/i2c-dev.h` and `linux/i2c.h` (Linux's layouts and numbers): `I2C_SLAVE`,
  `I2C_SLAVE_FORCE`, `I2C_FUNCS`, `I2C_RDWR`, `I2C_SMBUS` (quick, byte, byte data, word data,
  process call, I2C block, block write: built from messages in `linux.c`; block read needs a
  length the device gives, not offered), `I2C_TIMEOUT`, `I2C_RETRIES` (taken, ignored).
- **Shell:** `i2c scan N` (`i2cdetect`'s grid: `UU` for a driver's address), `i2c read N
  ADDR REG [COUNT] [--force]`, `i2c write N ADDR REG BYTE... [--force]`.

### 11.4 As built: 4b (2026-11-20)

- `I2cBus::transfer()` (messages as one transaction), `isOwned()`, `count()`, `addFiles()`
  (boot stage `i2cdev` after `gpio`); `I2cClient` (`bmds/i2cclient.h`), a stream per open in
  PSRAM, freeing itself in `handleClosed()`; `Stream::i2cClient()`.
- **The raw device has no address** (`I2C_DEVICE_ADDRESS_NOT_USED`): ESP-IDF writes a device's
  own address after every start of a transaction it runs, defined operations included, so a
  raw device with an address put that byte ahead of ours and nothing answered
  (`docs/platforms/esp32p4.md`).
- **Commands:** at most 8 (`SOC_I2C_CMD_REG_NUM`): a write is 3, a read 4 (its last byte not
  acknowledged), an address alone 2, plus the stop. A write then a read of any length fits
  (ESP-IDF moves a long read through the 32-byte FIFO in pieces: a 128-byte EDID read works);
  three messages do not, and are `EOPNOTSUPP`, not split.
- **`i2c_ctl`** (slot 140, `bmds/sys.h`, ABI-locked), `BMDS_ENXIO` (6) appended for an address
  or byte not acknowledged, as Linux. `linux.c` translates `I2C_SLAVE`, `_FORCE`, `I2C_FUNCS`,
  `I2C_RDWR` (`I2C_M_RD` only), `I2C_SMBUS` (as §11.3), `I2C_TIMEOUT`, `I2C_RETRIES`;
  `I2C_TENBIT` and `I2C_PEC` take 0 only.
- **Shell:** a command of its own, `i2c` (in `bmds_drivers`: `bmds_io` cannot depend on the
  drivers); bare, it lists the buses and the addresses their drivers own.
- **Size:** about 4.5 KB of code (the bus's transfer 1.1, the command 2.0, `i2c_ctl` 1.0, the
  client 0.4); 16 bytes of names; a client 20 bytes of PSRAM per open file.
- **Checked on bmds0:** `i2c scan 0` finds 0x11, 0x12, 0x18, 0x30, 0x3a, 0x4b, 0x50, 0x54 with
  the bridge's 0x48-0x4a as `UU`; `i2c read 0 0x48 0 2 --force` is its id `12 b2`; the example
  `sdk/examples/i2cread` (Linux's API only) reads EDID at 0x50 by SMBus and by `I2C_RDWR` (128
  bytes), gets `EBUSY` at 0x48, the id with `-f`, `ENXIO` at an empty address; the display
  unaffected.

### 11.5 4c: SPI, `io/spi2`, `io/spi3`, `/dev/spidevB.C` (design, 2026-11-20)

Both GP-SPIs are free on this board (`io resources`), so SPI is a port on chosen pins, as §11.1.

- **`io/spi2`, `io/spi3`** (sysconf, empty: off): `sck=4 mosi=53 miso=54 cs=5[,N...] [mode=0]
  [clock=1m] [bits=8] [lsb] [cs-high] [3wire] [loopback] [force]`. `clock` in Hz, or with `k` or
  `m`, 1k to 80m; `cs` up to 6 pads, in order (`/dev/spidevB.0`, `.1`...), none for one device
  with no chip select (Linux's `SPI_NO_CS`); `3wire` is MOSI both ways (no `miso=`); `loopback`
  reads MISO from MOSI inside the chip (no `miso=`: a port tried without wires); `force` as
  §11.1. `sck=` and one of `mosi=` and `miso=` at least. The owner `spidevN` holds the pads and
  `spiN`; `/dev/spidevN.C` is there while the port runs, added when it first starts.
- **The bus's pins through ESP-IDF's `spi_master`:** it picks IO_MUX when the pads are the
  controller's and the matrix otherwise. The pads and every input
  selector of the controller are saved before `spi_bus_initialize()` and put back after
  `spi_bus_free()` (which only turns the outputs off). `3wire` and `loopback` enable MOSI's
  input and route the controller's D (3-wire) or Q (loopback) from it, as ESP-IDF's master
  doesn't.
- **Chip selects are the port's own GPIO outputs**, not ESP-IDF's: removing an ESP-IDF device
  turns its CS output off (floating: an active-low device selected), and a device's mode or
  speed is fixed when it is added. With CS ours, each node has an ESP-IDF device with no CS pin,
  added again when its settings change, even between two transfers of a message with CS held:
  Linux's per-transfer speed and `cs_change` work as they do there. The order: acquire the bus
  (ESP-IDF sets the device's mode, so the clock idles right before CS falls), CS, transfers, CS
  released, the bus released. Speeds above what full duplex reads through the matrix take are
  lowered to ESP-IDF's limit, as Linux lowers to a controller's maximum.
- **Data: the controller's 64-byte buffer first** (§6: FIFO paths first, measured; DMA is step
  7). ESP-IDF's master with DMA wants each buffer aligned to the cache line (64 bytes here) in
  address *and length*, and otherwise allocates a bounce buffer for that transaction: a 4-byte
  register read would allocate every time. With DMA off it copies to and from the controller's
  buffer and allocates nothing, so a transfer goes in pieces of 64 bytes with CS held; GDMA
  comes with transfers of our own on `spi_hal` (step 7). `spi_device_transmit()` (its interrupt,
  the caller's task waiting), not polling. Words of 8, 16 or 32 bits (Linux's in the CPU's order, sent most significant bit first: their
  bytes swapped into the buffer); other sizes `EINVAL`.
- **Settings per node, as Linux's `spi_device`:** from `io/spiN` when the port starts, changed
  by programs (`SPI_IOC_WR_*`) until it starts again. `CPHA`, `CPOL`, `CS_HIGH`, `LSB_FIRST` can
  be set; `3WIRE`, `LOOP`, `NO_CS` are the port's (set the same, or `EINVAL`); the rest `EINVAL`.
- **`spi_ctl(fd, op, arg)`** (slot 141): `BMDS_SPI_INFO`, `BMDS_SPI_SETUP` (mode, bits, speed),
  `BMDS_SPI_TRANSFER` (an array of `bmds_spi_xfer_t`, Linux's `spi_ioc_transfer` exactly: 32
  bytes, 64-bit buffer fields, so `SPI_IOC_MESSAGE`'s array passes through `linux.c` as it is;
  at most 64 transfers, 64 KB). `tx_nbits` and `rx_nbits` 1 only, `word_delay_usecs` 0 only
  (dual and quad need their pins: later); `delay_usecs` waits after its transfer, spinning below
  100 µs, sleeping above. `read()` and `write()` are Linux's half-duplex ones (MISO ignored;
  zeros sent).
- **Linux:** `linux/spi/spidev.h` and `linux/spi/spi.h` (Linux's numbers): `SPI_IOC_RD_` and
  `WR_MODE`, `MODE32`, `LSB_FIRST`, `BITS_PER_WORD`, `MAX_SPEED_HZ`, `SPI_IOC_MESSAGE(N)`.
- **Shell:** `io spi B.C HEX...`: one full-duplex transfer at a node, the bytes read printed.
- **Checks on the board:** in loopback (no wires): bytes back as sent at a few speeds and modes,
  a transfer longer than the buffers, CS on its pad (`io get`) low only during a transfer;
  `spidev_test`-shaped use from an SDK example.

### 11.6 As built: 4c (2026-11-20)

- `IoSpiSetup` (`bmds/ioport.h`, host-tested): every word of §11.5. `spiports.cpp`: `io/spi2`,
  `io/spi3` (a check each, a watcher on `io`, a restart hook putting the pads back), the owner
  `spidevN`, `/dev/spidevN.C` added when first started; `SpiDevice` (`bmds/spidevice.h`) per chip
  select, made once and kept; `Stream::spiDevice()`. `IoChipOps::enableInput()` for MOSI read
  back. `Vfs::MaxDeviceFiles` 40 → 48 (224 bytes): UART and SPI nodes together reach past 40.
- **FIFO path, measured:** each 64-byte piece costs about 37 µs beyond its bits on the wire
  (`spi_device_transmit()`: its queue, the interrupt, the task woken): 706 KB/s at 10 MHz (the
  wire's 1220), 1.2 MB/s at 40 MHz, 1.4 MB/s asked at 80 MHz; 110 KB/s at 1 MHz (122). GDMA
  (step 7) is for the fast end.
- **`spi_ctl`** (slot 141, `bmds/sys.h`, ABI-locked): `bmds_spi_xfer_t` is Linux's
  `spi_ioc_transfer`, checked by `linux.c` at build time, so `SPI_IOC_MESSAGE(N)` passes its
  array through unchanged.
- **Size:** about 10 KB of code (the port and devices 6.9, the parser 1.6, `spi_ctl` 1.2, `io
  spi` 0.6); a device 40 bytes of PSRAM, the ports' state about 1 KB of PSRAM (the 64-byte
  buffers in it); ESP-IDF's bus and devices from its own heap while a port runs.
- **Checked on bmds0** (UEXT's pads, loopback, no wires): `io spi 2.0 deadbeef0102` back as
  sent; `sdk/examples/spidevtest` (Linux's API only) read back as sent in modes 0-3, 8, 16 and
  32-bit words, 1 to 80 MHz, 32 bytes to 64 KB (two transfers, CS held); CS low during a message
  and high after, on the second of two chip selects too, low idle with `cs-high`; a 12-bit word
  `EINVAL`; SPI3 at the same time on EXT1's pads; `cs=7` refused naming i2c0; stopped, the files
  gone, the pads and `spi2`, `spi3` free again. 3-wire writes and reads run (nothing on the wire
  to answer). Not checked: a real device, and Linux's word order for 16 and 32 bits (a loopback
  swaps both ways).

### 11.7 As built: 4d, the I/O Manager's Ports tab (2026-11-20)

In bmds-apps (its `devplan/ioman.md` §4): the ports listed (`io/uartN`, `io/spiN`, the system's
I²C buses) with their device and state, following sysconf's `io/` watch; a port's value as
fields, set, stopped and saved through sysconf, a held pin asked about in the kernel's words;
a tool each: a `Terminal` on `/dev/ttySN` (its fd in the program's `poll()`), an I²C scan and
register reads and writes (`i2c_ctl`), SPI transfers (`spi_ctl`). Nothing changed in BMDS for
it. Checked on bmds0 with the kernel's loopbacks. Step 4 is done; next, step 5's rest (§6:
`gpio_ctl`'s line requests, `dist/` recipes for libgpiod's tools and `i2c-tools`).

## 12. Step 5: programs' lines (design, 2026-11-20)

In parts, each checked on the board before the next: **5a** line requests (a line fd: Linux's
GPIO v2 `GPIO_V2_GET_LINE_IOCTL` and what it does), **5b** line info watched on the chip fd
(`GPIO_V2_GET_LINEINFO_WATCH_IOCTL`, info-changed events), **5c** `dist/` recipes for libgpiod's
tools and `i2c-tools`, which test both halves.

### 12.1 5a: line requests

- **A request** (`gpio_ctl` on the chip fd, `BMDS_GPIO_LINE_REQUEST`, op 7): up to 64 offsets,
  a consumer name, a config, an event buffer size; it returns a new fd, close-on-exec as Linux's.
  The config is Linux's `gpio_v2_line_config` exactly (flags for every line, then up to ten
  attributes by mask: flags, output values, debounce), so `linux.c` copies it; the request wraps
  it in a struct that starts with `size`. Checked as Linux checks it (`EINVAL`): input and
  output not both; edges only with input; open drain or source only with output; one bias, and
  only with a direction; no unknown flag; padding zero; offsets on the chip and each once.
- **The claim:** every line is claimed for the consumer (`gpioset`, or the program's name when
  none is given), owner kind User, role `line`, all or nothing: a line anybody holds (a driver,
  `io/gpio`, a port, another program's request) is `EBUSY`, as Linux's used lines. No force from
  programs: Linux has none, and the user forces through `io/gpio`. Each pad's registers are
  saved and put back when the fd's last handle closes, so a program that ends (or dies) gives
  its lines back as they were (§3.2).
- **Settings per line**, from the config: direction (input, output, or as the pad is), drive
  (push-pull, open drain, open source), bias (pull-up, pull-down, disabled, or as the pad is),
  active low, edges, debounce, the event clock (monotonic, or `CLOCK_REALTIME`; HTE is
  `EOPNOTSUPP`). The P4 has open drain, not open source: open source is emulated as Linux's
  gpiolib does, driving high and letting go (an input) for low.
- **The line fd** (`gpio_ctl` on it): `BMDS_GPIO_LINE_CONFIG` (op 8, the whole config again:
  directions, edges and all may change), `BMDS_GPIO_LINE_GET_VALUES` (op 9) and
  `BMDS_GPIO_LINE_SET_VALUES` (op 10) by bit mask over the request's lines, active low applied;
  setting an input is `EPERM`. An output's value is what it was set to; an input's is read.
- **Edges:** the pad's interrupt (ESP-IDF's GPIO ISR service, as `io/gpio`'s) writes a record
  into the request's ring (the event buffer size, 16 per line by default as Linux, up to 1024,
  in PSRAM) and wakes the fd's reader and pollers (`releaseFromIsr`, `notifyFromIsr`). Edges are
  logical: active low turns a falling edge into a rising one, as Linux. A full ring drops its
  oldest record, which the sequence numbers show, as Linux's. Debounce as `io/gpio`'s: an edge
  less than the period after the last one taken is dropped.
- **Reading the line fd** gives Linux's `gpio_v2_line_event` records (48 bytes: the time, rising
  or falling, the offset, the request's and the line's sequence numbers): whole records, waiting
  while there are none; `poll()` sees it readable; `O_NONBLOCK` as the SDK does it (a poll
  first). The record is the ABI's `bmds_gpio_line_event_t`, so `linux.c`'s read passes it as is.
- **Linux:** `linux/gpio.h` gains `gpio_v2_line_request`, `_config`, `_config_attribute`,
  `_values`, `_event` and `GPIO_V2_GET_LINE_IOCTL`, `GPIO_V2_LINE_SET_CONFIG_IOCTL`,
  `GPIO_V2_LINE_GET_VALUES_IOCTL`, `GPIO_V2_LINE_SET_VALUES_IOCTL`. The first interface's
  handles and events (`GPIO_GET_LINEHANDLE_IOCTL`) aren't offered: libgpiod 2 uses v2 only.
- **Pure and host-tested** (`bmds/ioline.h`): a config checked and turned into each line's
  settings, Linux's rules; the kernel side (`gpiolines.cpp`) does the pads, the interrupt, the
  ring.
- **`io pin N`** shows a requested line's holder as any claim's (`gpiomon: line`).
- **Checks on the board:** an SDK example on Linux's API only: outputs set and read back, and set
  again through `SET_VALUES`; edges without wires, from an unwired input's pull flipped with
  `SET_CONFIG` (the pad follows its pull); `EBUSY` on a system pad and on a line another program
  holds; lines given back when the program ends, killed or not.

### 12.2 As built: 5a (2026-11-20)

- `bmds/ioline.h` (pure, host-tested: Linux's flag rules, attributes by mask, the refusals);
  `bmds/gpiolines.h`, `gpiolines.cpp`: the request in one PSRAM block (the stream, its state, its
  lines, its ring of 16-byte records, 16 a line by default), the claim, the pads saved and put
  back, the interrupts, `read()` waiting on a semaphore the interrupt releases. Live requests sit
  in 32 lock-free slots for a restart hook that puts their pads back (a 33rd at once is put back
  only when closed). `Stream::gpioLines()`; `gpio_ctl` ops 7-10, their structs, ABI-locked;
  `linux.c` and `linux/gpio.h`; `sdk/examples/gpioline`.
- **A config change keeps a watching line's interrupt on:** stopped and started round the pad's
  new setup, the edge the change itself made (a pull flipped on an unwired pad) fell between
  them and was missed. Now only its type is set again.
- **Not yet:** line info shows what the pad says (direction, drive, bias, the consumer), not a
  request's active low, edges or debounce; those come with 5b, which tracks line changes anyway.
  An empty consumer is `gpio` (the kernel has no process name to give).
- **Size:** about 5.8 KB of code (the request 4.2, the rules 0.7, `gpio_ctl`'s part 0.9); 129
  bytes of RAM (the slots); a request about 250 bytes of PSRAM plus 52 a line and 16 an event.
- **Checked on bmds0** (EXT1's pads, no wires): outputs at 1 and 0 read back, flipped by
  `SET_VALUES`; four pull flips on GPIO47 gave four edges, falling then rising, sequence numbers
  1-4, times matching `uptime`; GPIO7 (`i2c0`) `EBUSY`; GPIO48 `EBUSY` while `gpioline hold`
  had it, shown as `gpioline as line` in `io pin`, free again after, and after the holder was
  killed.

### 12.3 5b: line info watched (design, 2026-11-20)

- **A chip fd of its own per open** (Linux keeps watches per file): `/dev/gpiochip0`'s open makes
  a `GpioChip` in PSRAM, freed with its last handle, as `/dev/i2c-N`'s clients.
- **`BMDS_GPIO_LINE_WATCH`** (op 11): a line's info, as `BMDS_GPIO_LINE_INFO`, and its changes
  told from then on; `BMDS_GPIO_LINE_UNWATCH` (op 12). Watching one twice, or unwatching one not
  watched, is `EBUSY`, as Linux. Reading the chip fd gives Linux's `gpio_v2_line_info_changed`
  records (288 bytes: the line's info as Linux lays it out, the time, requested, released or
  reconfigured), waiting while there are none; `poll()` sees it. 32 waiting at most, as Linux's
  queue; more are dropped.
- **What changes a line:** the registry calls a hook after each claim, release and ending owner,
  its lock let go (a host-tested addition); a holder that changes its pads without a new claim
  (`io/gpio` reapplied, a port's settings, a request's `SET_CONFIG`) says so. Each watching fd
  then compares its lines' info with what it last saw (a hash of the flags, consumer and
  debounce) and queues what moved: free to held is *requested*, held to free *released*,
  anything else *reconfigured*, with the info as it is then.
- **One change, one event:** a request claims its pads, then sets them up and publishes its
  settings; told at the claim, a watcher would see *requested* with the old setup, then
  *reconfigured*. Changes are batched (`GpioChip::Change`, a counter): a request, its config,
  its close, `io/gpio`'s apply, a port's start and stop each report once, at the end. Changes
  elsewhere during a batch are reported at its end.
- **A request's settings in line info:** each pad a request holds publishes its settings
  (direction, drive, bias asked, active low, edges, debounce, the clock) in a small table under
  a spinlock, and line info (`LINE_INFO`, `LINE_WATCH`, the events) reports them as Linux does;
  a pad without them reports what the pad does, as before.
- **Linux:** `GPIO_V2_GET_LINEINFO_WATCH_IOCTL`, `GPIO_GET_LINEINFO_UNWATCH_IOCTL`,
  `gpio_v2_line_info_changed`; the first interface's watch isn't offered (libgpiod 2 uses v2).
- **Checks:** `gpioline watch N` (Linux's API only) beside another program's request of N, its
  `SET_CONFIG`, its end; `io/gpio` taking and letting go of N; a request's active low and edges
  in `gpioinfo`.

### 12.4 As built: 5b (2026-11-20)

- `IoRegistry::setChangeHook()` (host-tested: called after claims, failed claims, releases and
  ended owners, its lock let go, so the hook may read the registry). `GpioChip` per open, its
  watches made at the first (`Watch`: the mask, a hash per line, 32 queued changes, in PSRAM);
  `GpioChip::lineInfo()` (moved from `gpio_ctl`, now with a request's settings) and
  `linesChanged()`; `Change` batches in a request, its config and its close, `io/gpio`'s and
  the ports' watchers (each made before their lock: telling reads the pads, which asks
  `io/gpio` for edge info under its lock). The requests' settings table lives in PSRAM, made by
  the first request (it was 1 KB of internal RAM held for nothing). `gpio_ctl` ops 11 and 12,
  `bmds_gpio_line_info_changed_t` (Linux's), ABI-locked; `linux.c`, `linux/gpio.h`;
  `gpioline watch`.
- **Size:** about 3 KB more code (the chip 3.5, less what moved from `gpio_ctl`); 141 + 184
  bytes of RAM (the slots, the watchers' list); a watching fd 2.8 KB of PSRAM.
- **Checked on bmds0:** `gpioline watch 47` while another program requested 47 with edges and a
  pull-up, flipped its pull twice and closed it, then `io/gpio` took and let go of it: one event
  each, *requested* (`used input rising falling pull-up`, consumer `gpioline`), *reconfigured*
  twice (the pull), *released*, *requested* by `io` as an output, *released*; unwatching twice
  `EBUSY`; `gpioinfo` (a chip per open) as before.

### 12.5 5c: libgpiod's tools and i2c-tools (built, 2026-11-20)

- **`dist/libgpiod`** (2.3.2, kernel.org's tarball, its published hash): the library and the six
  tools (`gpiodetect`, `gpioinfo`, `gpioget`, `gpioset`, `gpiomon`, `gpionotify`) built with
  the SDK's compiler directly (upstream builds with meson); gpioset's interactive mode needs
  libedit and is left out, as upstream does without it. **`dist/i2c-tools`** (4.4): `i2cdetect`,
  `i2cget`, `i2cset`, `i2cdump`, `i2ctransfer` and libi2c.
- **One program each, a link per tool** (as Dropbear's): every tool apart carried the library
  and musl, 200-260 KB each (1.4 MB for libgpiod's six); as one, 326 KB and 257 KB.
- **Patches, each saying why:** libgpiod's chip check asked sysfs whether a character device's
  subsystem is gpio (BMDS has no /sys): it now takes a device named `gpiochipN` that answers
  `GPIO_GET_CHIPINFO_IOCTL`, opening no other (the tools scan /dev, and a device may act on being
  opened). i2c-tools lists buses from /proc/bus/i2c or sysfs: with neither it lists /dev/i2c-N.
- **The SDK, made more like Linux** (what the two needed): `linux/types.h` (`__u8`...
  `__aligned_u64`), `linux/const.h`, `linux/ioctl.h` (Linux's field macros over musl's encoding);
  `linux/i2c.h` and `i2c-dev.h` include `linux/types.h` as Linux's do, and `i2c-dev.h` keeps
  Linux's misspelt `I2C_RDRW_IOCTL_MAX_MSGS`; `linux/spi/spidev.h` uses `linux/ioctl.h`.
- **Checked on bmds0** (run as `gpiodmulti TOOL`, `i2cmulti TOOL` from the card): `gpiodetect`;
  `gpioinfo` with consumers; `gpioset` holding GPIO46 while `gpioget` got `Resource busy` and
  `gpioinfo` named `gpioset`; `gpionotify` saw *requested* and *released* only; `gpioget` with
  pull-up and pull-down, active low; `gpiomon` holding 47 with `edges=both` shown and giving it
  back when killed; `i2cdetect -l` and `-y 0` (`UU` at the bridge's addresses); `i2cget` of
  EDID, `Resource busy` at 0x48 without `-f`, its id with it; `i2ctransfer w1@0x50 0 r8`;
  `i2cdump`; `i2cset` to nothing: `Write failed`. Both package (`bmds-dist package`).
- **Step 5 is done.** Next: step 6, the RMT (§3.7).


## 13. Step 6: the RMT (design, 2026-11-20)

In parts, each checked on the board before the next: **6a** RMT ports streaming through the
channels' own memory, timestamps, and an LTC example (generator and reader); **6b** DMA on the
channels that have it, TX channels started in sync, what a flash write does to a stream
(measured), and the clock trimmed if the hardware allows (§3.7).

### 13.1 6a: RMT ports

- **`io/rmt0` .. `io/rmt7`** (sysconf, empty: off): `pin=46 dir=tx|rx [resolution=1m]
  [idle=low|high] [invert] [continuous] [buffer=SYMBOLS] [min=NS] [max=NS] [monitor] [force]`.
  A port is numbered by its key, not by the hardware channel: ESP-IDF picks a free channel of
  the direction (on the P4, 0-3 send and 4-7 receive), and the port claims the one it got
  (`rmt_get_channel_id()`), so `io resources` shows the truth; `/dev/rmtN` is port N. `idle`
  is the level a sender rests at; `invert` inverts the pad; `buffer` the ring in symbols (1024).
  For a receiver, `min` drops pulses shorter (a glitch filter), `max` ends a run when a level
  lasts longer (the input went quiet); `monitor` reads a pad another port or driver drives
  without taking it (an input buffer on, nothing else): a sender's output measured back, with no
  wires. Owner `rmtN`.
- **A symbol** is the hardware's 32-bit word: bits 0-14 the first duration in ticks, 15 its
  level, 16-30 the second duration, 31 its level. A duration of 0 ends a run.
- **Sending** (`write()` of whole symbols): a ring in PSRAM, drained by ESP-IDF's simple
  encoder, whose callback runs in the RMT interrupt and fills the channel's memory as it empties:
  one transaction that lasts while there are symbols, so frame after frame goes out with no gap
  (ESP-IDF's `rmt_transmit()` per buffer would stop between them). When the ring runs dry, by
  default the transaction ends and the pad rests at `idle` (a burst: IR codes); with
  `continuous` the channel keeps sending `idle` in short pieces, counted (`filler_ticks`,
  `underruns`), so a stream's timing never slips: symbol N goes out at the stream's start plus
  the ticks before it. `write()` waits while the ring is full; `poll()` sees room.
- **Receiving** (`read()`): partial receive into a block of half the channel's memory; each
  block the interrupt hands over goes into a ring in PSRAM. What `read()` gives is a stream of
  words: symbols, and before each run (the first symbols after a start, after a quiet input or
  after a lost block) a header: a word 0 and the 64-bit monotonic time of the run's first edge.
  Words 0 never come from the hardware mid-run (a 0 duration ends a run), so the stream needs no
  other framing. A full ring drops the newest block (`overruns`), and the next block starts a
  run with a header again.
- **Time:** ESP-IDF calls back when a block is handed over, just after it copies the next half
  of the channel's memory out; that copy's last symbol ended about then. So a block's first edge
  is the previous callback's time less the first part's ticks, known once the block arrives; a
  run's first block, which has no previous callback, is held one block and its time worked back
  from the second (or from the run's end: the quiet time, `max`, before the callback). A sender's
  stream start is taken when the transaction is started. The RMT's clock and the monotonic clock
  come from the same crystal, so ticks and nanoseconds keep step; `rmt_ctl` gives the resolution
  and the clock source's rate.
- **`rmt_ctl(fd, op, arg)`** (slot 142): `BMDS_RMT_INFO` (the port, its channel, direction,
  resolution, the source clock, the ring's size; a sender's stream start, symbols and ticks
  sent, filler ticks, underruns; a receiver's symbols received, overruns). More (DMA, sync
  starts, carrier, a clock trim) come as appended ops.
- **The example** (`sdk/examples/ltc`): an LTC generator (SMPTE 12M biphase mark, 24, 25 or 30
  frames a second, the time of day, ticks kept exact with a fractional count) writing a
  `continuous` port, and a reader on a `monitor` port of the same pad decoding the frames back
  and timing each frame's sync word from the headers.

### 13.2 As built: 6a (2026-11-20)

- `IoRmtSetup` (`bmds/ioport.h`, host-tested); `rmtports.cpp`: `io/rmt0`..`7`, a channel per
  port from ESP-IDF, the hardware channel it got claimed (`rmt_get_channel_id()`), the pad saved
  and put back (a monitor's isn't taken, and a pad running an IO_MUX function is refused for it:
  ESP-IDF's receiver selects the GPIO function); `RmtChannel` (`bmds/rmtchannel.h`), the
  stream; `Stream::rmtChannel()`; `rmt_ctl` (slot 142, `BMDS_RMT_INFO`), the symbol macros,
  ABI-locked; `sdk/examples/ltc`. `Vfs::MaxDeviceFiles` 48 → 56.
- **The SDK's new Linux headers step aside on Linux:** compiled on a Linux host (the ABI's
  host test), glibc's own headers include `<linux/ioctl.h>` and `<linux/types.h>` and found the
  SDK's (5c's), missing what Linux's carry; there (`__linux__`, which the SDK's compiler doesn't
  define) they `#include_next` Linux's.
- **Measured on bmds0** (an LTC sender on GPIO46, a monitor receiver of it, no wires):
  `rmt_transmit()` takes about 73 µs, filling the channel's memory through the encoder before
  the hardware starts, so the stream's start is stamped when it returns (stamped before, frame
  0's first edge read back 80 µs late; after, 7 µs, the receiver's interrupt latency and a
  tick). Frames read back 1/25 s and 1/30 s apart within the 1/3 tick of 30 fps's 208 1/3-tick
  half bits; 100 frames at 25 and 120 at 30, no underrun, no overrun; a continuous stream kept
  its start across three programs (counted filler between), a burst port started a stream each
  time. Each burst's last frame isn't read back: its last bit ends at the idle level, with no
  edge to close it, as on any LTC line that stops.
- **Size:** about 10 KB of code (the ports 9.5, `rmt_ctl` 0.6); ESP-IDF's RMT driver about 20
  KB more (the firmware grew 33 KB); 2.5 KB of PSRAM for the ports' state, the rings (4 bytes a
  symbol) while a port runs.

### 13.3 6b: frame marks and a timed pulse (design, 2026-11-20)

The user's use (their words): an LTC frame written at a time, an interrupt at frame rate to
update a HH:MM:SS:FF seven-segment display through shift registers, latched by a GPIO pulse at
bit 0's edge. So 6b adds, before DMA and the rest of §13's list:

- **Marks:** a word 0 written to a sender (never data: a 0 duration ends a transmission) marks
  the stream there; the kernel takes it out. When the hardware has sent up to it, the fd gives a
  record (the mark's number, and the monotonic time of the edge after it: the stream's start plus
  the ticks before it, exact while the stream has no gap) and `poll()` sees it readable: a
  program's wake at frame rate, the frame's time with it.
- **A timed pulse:** `pulse=PIN [pulse-width=US] [pulse-low]` on a sender: at each mark's edge
  the kernel pulses PIN (high for 10 µs unless asked otherwise), from a hardware timer's alarm
  set for the mark's time, not from the program: a few µs from the edge (the interrupt's
  latency), whenever the program wakes. The display's latch, with the program shifting the next
  frame's digits out (an SPI port, `io/spi2`) between marks.
- **The example:** `ltc send` writes a mark before each frame and, given a display, shifts the
  next frame's digits out after each mark (latched by `pulse=`).
- **Steering by duration** (the user's: ticks added to or taken from LTC pulses to keep with the
  timecode): the program writes every duration, so steering is its own; a mark's time already
  counts its adjustments (start plus every tick before it). Two checks: the RMT's ticks against
  the monotonic clock over ten minutes (both from the 40 MHz crystal, so expected to keep step
  exactly: then marks are exact for hours, and only PTP or a foreign timecode needs steering),
  and frames lengthened and shortened by a few ticks read back where the marks said.
- Then DMA, sync starts (for edges tick-exact across channels), a flash write's effect on a
  stream measured, and a clock trim.

### 13.4 As built: 6b's marks and pulse (2026-11-20)

- **Marks:** `write()` takes a word 0 as a mark at that point (`BMDS_RMT_MARK`): its place goes in
  a queue of 32 before the symbols after it; the encoder notes the ticks before it as it hands the
  symbols over (or as the ring runs dry); its time is the stream's start plus those ticks, once the
  start is known (marks handed over inside `rmt_transmit()` are timed when it returns). One
  `gptimer` for every port, at 1 MHz from the crystal, is armed for the soonest mark or pulse end;
  its alarm makes each mark a `bmds_rmt_mark_t` to read (32 kept, the oldest dropped, counted) and
  turns the `pulse=` pin on for `pulse-width=`. A sender's `read()` gives marks, not behind a
  `write()` waiting for room (a lock of its own); `poll()` sees them. Marks are numbered from the
  port's start: `marks_written` (appended to `bmds_rmt_info_t` with `marks`, `marks_lost`,
  `pulse_pin`) tells a program its first, and any below it are an earlier program's.
- **The timer's interrupt at level 3:** at the default level the pulses came 2 to 19 µs after
  their marks, at 3 mostly 2 to 8 (the receiver's 3 µs taken off); the worst seen, 31 µs, in the
  first half minute after a boot. Frame 0's pulse came 50 µs late when the stream started with it
  (its time known only as `rmt_transmit()` returned): the example starts a stream with 1 ms of
  idle, and frame 0 is on time as the rest.
- **Measured on bmds0** (LTC on GPIO46 with marks, `pulse=47`; monitors of both): marks against
  the frames read back, 3 µs (the receiver's latency) in every frame, with `-j 7` moving frames by
  7 ticks and the marks with them; over ten minutes of one continuous stream, frames timed afresh
  at the start, at 5 and at 10 minutes against the times the marks predict: 3, 4 and 3 µs. The
  RMT's ticks and the monotonic clock keep step within a µs in ten minutes (both from the 40 MHz
  crystal): marks are good for hours, and only PTP or a foreign timecode needs steering.
- **Found:** marks were numbered from the port's start but read as if from a program's, so a
  second program on a running continuous port printed every frame 75 off (the first had written
  75): `marks_written` added. A monitor port keeps receiving while nobody reads: a reader starts
  with what its ring holds (a reader wanting only what is new drains it first).
- **Size:** the RMT code 12.6 KB (3 KB more), ESP-IDF's gptimer driver; the firmware 5 KB more.
- **Next (6c):** DMA on the channels that have it, sync starts, a flash write's effect on a stream
  (the RMT's and the timer's interrupts run from flash), and a clock trim.

### 13.5 6c: a flash write's effect, and DMA (built, 2026-11-20)

- **What a flash write does:** the flash's erase and program turn the caches off, and everything
  not in internal RAM waits, the RMT's interrupts and the marks' timer among them (auto-suspend
  faults reads on this board's flash: §13's platform notes). Measured on bmds0, LTC at 25 fps
  from the channel's own memory (24 symbols a half, some 10 ms): 256 KB written to `/flash` 3 s
  into a stream gave 7, 9 and 6 underruns (200 ms of filler) in three runs with a receiving port
  also running, and none in two runs without; a receiving port loses what comes in while it is
  paused (the reader decoded 128 to 138 of 200 frames).
- **Held writes:** a running RMT port holds `/flash`'s writes (`FlashWrites::hold()`), as audio
  playing does: the write queue waits until somebody needs it written (`sync`, a restart) or it
  is half full. A 16 KB write while an LTC stream ran: held, no underrun.
- **`dma[=SYMBOLS]`** (a sender; 1024 unless given, up to 2046; the P4 has one sending channel with
  DMA): ESP-IDF's DMA reads a buffer of that many symbols in internal RAM, refilled half at a time
  by the same encoder, so a stream goes on through a pause up to half the buffer long (1023
  symbols: some 800 ms of LTC). The encoder fills a whole half each time, so the ring holds a whole
  DMA buffer at least (twice it unless `buffer=` says) and a DMA stream starts once its ring has a
  buffer's worth (or is full, or its writer closes): started with less, it filled the rest with
  idle, 400 ms of it at an LTC stream's start. Marks run up to a buffer ahead of the wire: 64
  kept a port (32 lost 56 of 300).
- **Measured:** with DMA, two forced 256 KB writes (`sync`) during each of two streams: no underrun
  (the one counted was each stream's end, filler after its last frame went to the encoder).
- **Not done, and why:**
  - **Marks and their pulse during a forced write:** the timer's interrupt runs from flash, so a
    mark due in the pause fires at its end (a latch late by up to the pause). Its fix is a timer
    interrupt and mark state in IRAM, and the pin set from there: worth it if a latch must be on
    time through `sync`; held writes don't delay it.
  - **Receiving by DMA:** a receiver loses input during a forced write; DMA would carry it as it
    does a sender. Not needed by the uses so far.
  - **Sync starts** (TX channels started on the same tick): no use yet needs edges tick-exact
    across channels (the display's latch is the timed pulse); kept for when one does.
  - **A clock trim:** not needed. The RMT's ticks keep step with the monotonic clock (§13.4), and
    steering by durations is exact to a tick; the RMT's fractional divider would only add jitter.
- **Step 6 is done**: RMT ports, marks, the timed pulse, DMA, the LTC example (§13.1-§13.5).
