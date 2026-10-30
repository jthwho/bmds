# Kept State: What Survives a Reset

Planned and built 2026-10-29 (the user: keep the window's counters across resets, then the log's last
lines, a record of resets, and the system monitor's lows and highs).

## 1. Goal

A reset of any kind short of a power cut (a reboot, a panic, a watchdog, the USB serial port's
control lines, a hang ended by the RTC watchdog) loses none of what tells why it happened or how
the system ran before it:
1. **the log's last lines** (§3);
2. **a record of the recent boots:** when each started, how long it ran, what ended it, which
   firmware (§4);
3. **the system monitor's lows, highs and totals across boots:** memory's lows, underruns, drops
   (§5).

The window history (`psramwindow.md` §3g, built 2026-10-29) becomes the first user of the common
pieces (§2c).

Not a goal: surviving a power cut (the board has no battery-backed memory, and PSRAM and the
LP RAM both lose their contents); the recovery firmware's view of the main firmware's state.

## 2. Facts and common pieces

### 2a. Measured (`memory.rtc`, 2026-10-29)

| Operation | Cost |
|---|---|
| A 32-bit store to the LP RAM | 495 ns |
| A 100-byte line copied into the LP RAM | 14.5 µs |
| 4 KB written / read in the LP RAM | 518 / 514 µs |
| A 100-byte line in PSRAM written back by address (the ROM's L1 and L2 calls) | 2.8 µs |
| The same through `esp_cache_msync()` (its spinlock: safe beside the other core's) | 3.8 µs |

- **The LP RAM** (RTC memory, 32 KB at `0x50108000`) isn't cached (`docs/platforms/esp32p4.md`
  §3): a store is in it at once, so nothing is lost dirty. But it's slow, reads too: about 500 ns a
  word, two orders of magnitude slower than PSRAM through the cache.
- **What it holds now:** the bootloader's area (72 bytes: the clock record), IDF's RTC data
  (56), the update's restart count (12), the window history (224). IDF gives the rest, about
  31.6 KB, to the heap as a last-resort internal pool (no PIE access, no DMA). Everything here
  comes out of that pool.
- **PSRAM's `.noinit`** keeps its contents across a reset too, but behind write-back caches: a
  hardware reset drops what's dirty in them. The log writes its ring back before a software
  reset and at each panic line, not otherwise (`logging.md` §9).
- **The RTC timer** (`Detail::persistentTimeUs()`) counts across resets: the clock record already
  relies on it. The difference between two boots' start times is the time between them.
- A firmware built with a different layout puts its `RTC_NOINIT` objects elsewhere, so each
  record is checked (magic, size, a check word) and starts again when it doesn't pass. The
  recovery firmware has its own layout: a boot through it starts the records again.

### 2b. So

- **The LP RAM holds small records written now and then, never per event.** Records are kept in
  ordinary RAM and copied into the LP RAM by one thread every 10 s (`sysmon/keep-interval`), and
  at once before a clean reboot. A hardware reset or a panic loses at most the last interval.
- **The log stays in PSRAM**, written back line by line (§3): cheaper than copying lines into the
  LP RAM, and it keeps the whole 64 KB, not just the newest few KB.

### 2c. The common pieces

- **`KeptRecord`** (`bmds_core`, host-tested): a record of N bytes in two copies in memory a reset
  leaves alone. Each copy is a header (magic, size, sequence, check) and the bytes. `save()`
  writes the older copy, its magic cleared first and set last, so a reset partway leaves the
  other. `load()` takes the newer copy that passes: the magic, the size and an FNV-1a check over
  the header and the bytes. Reads copy the whole copy out first (the LP RAM's reads are slow too).
- **`Kept`** (`bmds_kernel`): the keepers and their thread.
  - `Kept::add(keeper, arg)`: up to 8 functions, called each pass on the `kept` thread (4 KB
    stack, priority 1). A keeper saves its record if it changed.
  - `Kept::setInterval(ms)`; 0 stops the passes.
  - `Kept::flush()`: a pass now, on the caller's thread. `System::restart()` calls it, so a
    `reboot` loses nothing.
  - A boot stage, `kept`, after `stats`; keepers may be added before it.
- **The window history moves onto it:** `Memory` marks the history changed (no LP RAM store on
  an allocation, which had cost about 14 µs each), and its keeper saves it.

## 3. The log's last lines

**Problem:** the history ring (64 KB) is in PSRAM's `.noinit`. Before a software reset or a
panic, its caches are written back. On a hardware reset they aren't, so the newest lines (and
the header's `head` and `saved`) can be lost: the lines before a hang, the ones most wanted.

**Design:**
- Each line appended to the ring is written back from the caches as it's written
  (`esp_cache_msync()`, by address; a line that wraps is two ranges): about 4 µs a line, under
  the log's lock. The panic path keeps its own write-backs (the other core is stalled, so the
  ROM's calls are safe).
- **The header's counters go in the LP RAM too:** `head`, `saved`, the boot's clock offset, with a
  magic and the capacity. `head` is stored after its line is written back, so it never runs ahead
  of the bytes: two stores a line (1 µs). `saved` and the offset change when `logd` saves (once a
  second at most).
- **At boot,** when the LP RAM's header passes (magic, capacity, `saved <= head`), its counters
  replace PSRAM's (whose header may have been dropped dirty); else PSRAM's header is used, as
  now. The recovery then runs unchanged. The ring is written back after it's rebuilt.
- **Cost:** about 5 µs a line in all. The log already formats the line and sends it to the
  console; at 100 lines a second it's 0.05 % of one core.

## 4. The record of boots

**`Boots`** (`bmds_kernel`, boot stage `boots` after `clock`): the last 8 boots, newest first.
- **An entry:** the RTC timer at the application's start (the timer's reading minus the uptime),
  the RTC timer when last seen alive, the reset reason that ended it (known at the next boot), and
  the firmware's version (32 characters).
- **At boot:** the previous entry gets this boot's reset reason; a new entry starts. The boot log
  says how the last boot went: `Boots: the last ran 4:49:12, ended by USB (the serial port's
  control lines), last alive 6 s before`.
- **Each pass,** the keeper stores the time last alive. A boot that ended long after it was last
  alive hung (the gap shows how long).
- **`boots`** (shell) lists them: when each started (wall time, from the clock now and the RTC
  timer, `-` while the clock isn't set), how long it ran, the gap before its end, what ended it,
  the firmware. `boots clear` starts the list again.
- A power-on starts it again (the LP RAM's contents are gone), so the first entry is always the
  power-on's.

## 5. The system monitor's lows, highs and totals

**Problem:** the counters (`devplan/sysmon.md`) count from boot, so a reset erases what was
going wrong before it: a heap running low, underruns building up, frames dropped.

**Design** (`bmds_stats`, its keeper added by `Sysmon::init()`):
- **A fixed list** in rodata of counter names (`Stats::find()`), each kept as one of:
  - **low:** the lowest value seen across boots (memory's `min_free`, `largest`);
  - **high:** the highest (worst fill and wait times, the network stack's heap high);
  - **total:** a `Total`'s count summed over boots (underruns, drops, errors).
- **The list** (to start): `mem.internal.min_free`, `mem.internal.largest`,
  `mem.internal_dma.min_free`, `mem.psram.min_free`, `mem.psram.largest` (low);
  `audio.dsp0.fill_us_max`, `display0.scanout.isr_cycles_max`, `display0.surface.wait_us_max`,
  `net.ip.heap_max` (high); `display0.scanout.underruns`, `display0.scanout.resyncs`,
  `display0.scanout.failures`, `audio.dsp0.late`, `audio.dsp0.underruns`,
  `audio.dsp0.client_late`, `audio.dsp0.sounds_dropped`, `net.eth0.rx_dropped`,
  `net.eth0.rx_errors`, `net.eth0.tx_errors`, `net.eth0.rx_missed`, `net.eth0.rx_overflow`,
  `net.ip.heap_failed`, `net.ip.pools_failed`, `console.usbserial.rx_dropped`,
  `usb0.unhandled_halts`, `usb0.hid_rearm_failures`, `midi.port0.dropped` (total).
- **Each pass** reads them (`Stats::read()`; names looked up again when the registry's generation
  changes; a counter not registered is skipped) and keeps for each two numbers: low/high, the
  extreme across boots; total, the earlier boots' sum and this boot's last reading. At boot each
  total's last reading joins the sum.
- **The record** holds the list's hash (a changed list starts it again), the boots counted, and
  when it started. 27 counters: about 450 bytes a copy.
- **`stats kept`** (shell) lists them: the counter, how it's kept, this boot's value, the value
  across boots; `stats kept clear` starts again.
- The pass costs a read of a few groups every 10 s, and an LP RAM write (about 60 µs) only when
  something changed.

## 6. Budget

| Record | LP RAM (two copies) |
|---|---|
| The log's counters | 32 bytes (one copy: its fields are stored one at a time) |
| The window history | about 200 bytes |
| Boots (8 entries) | about 900 bytes |
| The monitor's counters (27) | about 950 bytes |

About 2.1 KB of the 31.6 KB the heap had. CPU: about 5 µs a log line, a pass every 10 s (a few
hundred µs at most); the `kept` thread's 4 KB stack in PSRAM.

## 7. Steps

1. [x] `KeptRecord` with host tests; `Kept` and its thread; the window history moved onto it.
2. [x] The log's write-back and its counters in the LP RAM.
3. [x] `Boots`, its boot line and `boots`.
4. [x] The monitor's kept counters and `stats kept`.
5. [x] On the board through `reboot`, a USB control-line reset and `reboot panic`. [ ] A hang
   ended by a hardware watchdog (no panic handler) is still untested: nothing in the shell makes
   one.

## 8. As built (2026-10-29)

- **Built as designed** (§2c-§5). `KeptRecord` (`bmds_core`, 3 host tests: garbage, a save cut
  short, another magic or size); `Kept` (`bmds_kernel`, the `kept` boot stage after `sysmon`,
  `System::restart()` flushing first); `Boots` (the `boots` stage after `clock`);
  `KeptCounters` (`bmds_stats`, from `Sysmon::init()`); `sysmon/keep-interval` (10 s; in the
  Settings app's table). The LP RAM holds 2,124 bytes of `RTC_NOINIT` objects in all (the log's
  counters 32, the window history 208, the monitor's counters 928, boots 944, the update's
  restart count 12).
- **On the board:**
  - a line logged 30 ms before a USB control-line reset recovered, the marker saying "32 bytes
    past PSRAM's header" (exactly the line, which PSRAM's own header had lost);
  - the window history, the boots and the kept counters through a USB control-line reset after a
    keeper pass: the counts carried, `mem.psram.min_free` kept at 3.1 MB (the fragmentation
    bench's) while the new boot's was 26 MB, `console.usbserial.rx_dropped` (116,544 bytes from a
    flood of the console) moved into the all-boots total;
  - `boots` showed the reset, the boot last alive 8 s before it (a 10 s interval);
  - the boot's time unchanged (215-218 ms; one boot after the recovery firmware took 310 ms, its
    `healthy` stage confirming the image).
- **Found on the way:**
  - The first window history computed its check before setting the magic it covered, so no copy
    ever passed; a log of each copy's fields at boot showed it in the same boot. `KeptRecord` now
    builds the check over the magic as written.
  - `Thread::detach()` asserts unless the thread itself calls it: the first `Kept::init()`
    detached from the boot task and the board restarted three times into recovery (the update's
    crash loop guard did its job). A thread that runs for good is just never joined.
  - A boot through the recovery firmware starts every record again (its layout differs), as
    planned.
