# Logging

Designed 2026-10-11 (the user's request: syslog for programs such as Dropbear, logs on the card
in `/sd0/p0/logs`, one logging API for the whole system). Steps 1-4 built and tried on the board
2026-10-11 (§8).

## 1. Goal

One log for the whole system. The kernel, the built-in programs, programs built separately,
shell scripts and the services the kernel runs all write lines into it, and those lines are
kept in three places: the history in RAM (`log show`), the serial console, and files on the
card that survive a reboot.

## 2. Facts that shape the design

- **The kernel's log exists** (`bmds/log.h`): `bmdsDebug/Info/Warn/Err`, formatted on the
  caller's stack, a lock, an overwrite-oldest history ring in PSRAM (8 KB today), and one sink
  (the serial console). ESP-IDF's own log lines come through it too. Its levels are four:
  debug (per module), info, warn and err; `log/level` picks the least that's written.
- **`LogStream`** turns what a kernel-started program writes to stdout and stderr into log
  lines (the SSH server's, `network.md` §24).
- **Programs are linked with musl**, whose `syslog()` makes an `AF_UNIX` datagram socket,
  connects it to `/dev/log`, and sends each message as one datagram in RFC 3164's form:
  `<PRI>Mmm dd hh:mm:ss ident[pid]: text` (the time in UTC). BMDS has no `AF_UNIX` sockets,
  so `syslog()` silently does nothing, and Dropbear was built with `--disable-syslog`.
- **The card comes later than the first lines:** it mounts at about 0.65 s after reset, and
  more than half of the boot's lines come before then. The card can also be absent, be pulled,
  or come back.
- **Writes to the card cost:** each one updates the file's cluster chain and its directory
  entry (exFAT or FAT through the sector cache, written back within a second: `storage.md` §3).
  So the log's file writes must be batched, never one per line.
- **No hardcoded paths:** where the files go is a setting (`log/dir`), so a board without a
  card, or a user who wants them on `/boot`, can say so.

## 3. Design

### 3.1 One log, several ways in

| Who                        | How they log                                                      |
|----------------------------|-------------------------------------------------------------------|
| The kernel, drivers        | `bmdsInfo/Warn/Err/Debug` (unchanged)                             |
| Built-in programs          | `Log::writeText(level, tag, text)`, `tag` being their name        |
| Programs built separately  | `syslog(3)` (musl's, unchanged): its datagrams go to `/dev/log`   |
| Shell scripts              | `logger [-p PRI] [-t TAG] [MESSAGE...]`                           |
| Services the kernel starts | stdout and stderr through `LogStream` (as now)                    |

Every way ends in the kernel's `Log`, so every line has the same form, is in the same history,
reaches the same sink, and goes to the same files. Nothing else keeps a log of its own.

A line with a tag reads `[  123.456] I dropbear[21]: Pubkey auth succeeded for 'root'`: the
tag (a program's name, with its pid when the program gave one) is followed by `: `, as a debug
module's name already is.

### 3.2 Levels

syslog's eight priorities fold into the log's four: 0-3 (emerg, alert, crit, err) are `E`,
4 (warning) is `W`, 5-6 (notice, info) are `I`, and 7 (debug) is `D`. The facility is ignored.
`log/level` gains `debug` (appended), the only level that lets syslog's debug lines through;
the kernel's own `bmdsDebug` stays gated by its modules (`log/debug`) as now.

### 3.3 `/dev/log`

A device file (`Vfs::addDeviceFile`) whose writes are messages: each `write()` is one message
(a datagram's worth), its lines logged one by one. A message is parsed by the portable
`SyslogMessage` (`bmds_core`, host-tested and fuzzed: it takes what any program sends):

- an optional `<PRI>` (1-3 digits, at most 191) gives the level; without one, the level is
  info;
- an optional RFC 3164 timestamp (`Mmm dd hh:mm:ss `) is skipped: the log has its own time, and
  musl's is UTC, which the files would show wrongly;
- an optional tag, `ident:` or `ident[pid]:` (no spaces, at most 48 characters), becomes the
  line's tag;
- the rest, its control characters shown as `?`, is the text. An empty message logs nothing.

Reading `/dev/log` ends at once (end of file). Its writers are any program; the kernel's lock
keeps each line whole.

**`linux.c`** makes musl's `syslog()` reach it without a change to musl: `socket(AF_UNIX,
SOCK_DGRAM, 0)` opens `/dev/log` (the only `AF_UNIX` datagram socket there is; other `AF_UNIX`
types still fail with `EAFNOSUPPORT`), `connect()` to the address `/dev/log` succeeds (any
other path: `ENOENT`), and `send`, `sendto` and `sendmsg` on a descriptor that isn't a socket
(`ENOTSOCK` from the kernel) write it when it is `/dev/log`. `closelog()` closes it as a file.

### 3.4 The log's history and its readers

- The history grows to 64 KB (PSRAM): the lines from reset until the card mounts must still be
  there when the file writer starts, and they are about 5 KB today.
- `Log::read(position, dst)` copies the complete lines written since `position` (a count of
  every byte ever written, as the ring's `head`), and says how many bytes were lost when the
  ring has already overwritten some of them.
- `Log::waiters()` (a `WaitList`) is released after each line is added, so a reader waits on
  a semaphore and is never woken without a reason (no polling).

### 3.5 Files

`LogFiles` (`bmds_sysconf`, next to the settings it reads) is a kernel thread, `logd`
(priority 1, below every program, a 5 KB internal stack because `log/dir` may be on `/boot`):

- **Where:** `log/dir` (a path, default `/sd0/p0/logs`; empty: no files). The directory is made
  when missing. The file is `system.log`, opened for each write and closed after it: FatFs
  refuses to open a file that is open for writing, even to read it (§8).
- **When:** after a line arrives, the thread waits a second more (`FlushMs`) for others, then
  writes what has come in one write; a warning or an error is written at once. So a crash can
  lose at most the last second of info lines.
- **From when:** the first write after boot starts from the history's first line, so the boot's
  lines from before the card mounted are in the file. A line `--- boot ---` comes before them.
- **Time:** each line's uptime stamp becomes the local time (`clock/zone`) it was written at,
  `2026-10-11 17:07:04.123 I ...`, from the clock now and the uptime now. Until the clock is
  set (no RTC time, no NTP yet) the uptime stamp stays.
- **Size:** when `system.log` passes `log/file-bytes` (default 1 MB), it becomes
  `system.log.1`, the older ones move up one, and the oldest beyond `log/files` (default 4,
  the current one counted) is removed. At most about 4 MB on the card.
- **No card:** while the directory can't be opened (no card, a card pulled), the thread waits on
  `Vfs::mountWaiters()` and tries again after each mount; the history keeps the lines meanwhile,
  and a line says how many were lost if the ring overwrote some.
- **Settings change:** a change to `log/dir`, `log/file-bytes` or `log/files` closes the file;
  the next write uses the new values.

### 3.6 Reading the log

`log show` (the history) as now; the files are text, for `cat`, `grep`, `scp` and `sshfs`.
Later: a log tab in the System Monitor (§6).

## 4. ABI (append only)

- `/dev/log`: a device file; each write is one RFC 3164 message as above.
- `linux.c`: `socket(AF_UNIX, SOCK_DGRAM)`, `connect()` to `/dev/log`, and `send*()` on it.
- sysconf: `log/dir`, `log/file-bytes`, `log/files`; `log/level` gains `debug`.
- The files' form: `system.log` and `system.log.N`, one line each, `YYYY-MM-DD HH:MM:SS.mmm L
  text`.
- Shell: `logger [-p PRI] [-t TAG] [MESSAGE...]` (POSIX's, with `PRI` a number or
  `facility.level` names as BusyBox takes; the standard input's lines without a message).

## 5. Resources

- RAM: the history 8 → 64 KB of PSRAM; the thread's 5 KB internal stack, and 10 KB of PSRAM
  buffers (4 KB read from the history, 6 KB of converted lines).
- CPU: a semaphore release per line (no other cost to a writer); the thread's conversion and
  one card write a second while lines come.
- Card: what the lines take, batched.

No new ESP-IDF use: its own log lines already come through `Log` (`esp_log_set_vprintf`).

**Testing:** `SyslogMessage` and the files' line conversion and rotation names on the host
(`tests/host`), the parser fuzzed (`tests/fuzz/syslog.cpp`); on the board, `progtest syslog`,
Dropbear's lines, `logger`, the files across a reboot, and a card pulled and put back.

## 6. Steps

1. **The parser and the kernel's side:** `SyslogMessage` with host tests and a fuzz target;
   `Log::writeText`, `Log::read`, `Log::waiters()`, the 64 KB history; `/dev/log`; `log/level`'s
   `debug`.
2. **`linux.c`'s `AF_UNIX` datagram socket**, a `progtest syslog` case, and Dropbear built with
   syslog (no `--disable-syslog`): its lines tagged `dropbear[PID]` with their own levels.
3. **Files:** `LogFiles`, its settings, rotation, the card coming and going.
4. **`logger`** in the shell.
5. Later: a log tab in the System Monitor; the console sink's level apart from the files'.

## 7. Not doing

- Remote syslog (UDP 514): not until someone needs it.
- A log daemon in userspace: the kernel's lock and ring already do its work.
- Binary or structured logs: lines of text are what `grep` and the user read.

## 8. As built (2026-10-11)

- **`LogText`** (`bmds_core`): `parseSyslog`, `parsePriority` (`logger -p`), `nextLine`,
  `cleanLine`, `parseStamp`; host tests (7 cases) and `syslog_fuzz` (10.3 million inputs in two
  minutes, nothing found).
- **The kernel:** `Log::writeText`, `Log::read`, `Log::position`, `Log::waiters` (a
  `WaitListOf<4>` released after each line), `Log::levelOfSeverity`; the history 64 KB.
  `LogStream` now ends in `writeText` too (its lines cleaned). `SyslogStream` is `/dev/log`.
- **`LogFiles`** (`bmds_sysconf`, the boot stage `logs` after `sysconf`): as §3.5. While it
  can't write (no directory, a failed write), it leaves the log's waiters, so only a mount or a
  setting wakes it: a write error that logs a line can't make it spin.
- **`linux.c`:** `socket(AF_UNIX, SOCK_DGRAM)` opens `/dev/log` and is remembered (four at most)
  until closed or replaced by `dup2`; `connect()` to `/dev/log` succeeds, elsewhere `ENOENT`;
  `send`, `sendto`, `sendmsg` write the message in one write (2 KB at most, `EMSGSIZE` beyond).
  Other `AF_UNIX` types still fail with `EAFNOSUPPORT`.
- **Dropbear** is built without `--disable-syslog`: its lines are `dropbear[PID]: ...` with
  their own levels, and the `[pid] Mmm dd hh:mm:ss` prefix it wrote on stderr is gone.
- **`logger [-s] [-p PRI] [-t TAG] [MESSAGE...]`** in the shell; the tag defaults to `root`.
- **On the board:** the boot's lines from before the card mounted are in `system.log` after
  `--- boot ---`, with local times; `progtest syslog` (musl's `syslog()` with `LOG_PID`, a
  two-line message, debug dropped at `info`, a socket by hand, the errors); `logger` from
  arguments and from a pipe; `log/level debug`; rotation at `log/file-bytes 16384` and
  `log/files 3` (all 450 lines of a burst in the three files, then the oldest removed); a
  `log/dir` that can't be made, and back. The firmware grew 8 KB (1 729 504 to 1 737 520 bytes).
- **Found on the way:**
  - The first version kept `system.log` open, and `cat` of it failed with `Device or resource
    busy`: FatFs (`FF_FS_LOCK`) refuses any open of a file open for writing. The file is now
    opened for each batch (at most once a second).
  - The milliseconds of the files' times came from the uptime and the seconds from the wall
    clock, so times went backwards across a second; both come from the wall clock now.
  - `logd` used 2.9 KB of stack after rotations on exFAT; its stack went from 4 to 5 KB.

## 9. Crash logs: a panic's text and the last lines, on the card after the reset

**Problem:** The files hold only what `logd` wrote, and only after a second's batching. A panic,
a watchdog or a reboot loses:
- the last second of lines;
- every line from a boot that never had a card;
- the panic's own text (the register dump, the backtrace, the failed assertion). IDF prints that
  only on the USB console, so a board on ssh alone just drops off the network (the mutex race in
  bench.md §21 cost a session that way).

**Design:**
- **The history survives a reset.** The 64 KB ring is a static object in PSRAM's `.noinit`
  section (`EXT_RAM_NOINIT_ATTR`, `CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY`), not a heap
  block. Next to it is a header: a magic word, the capacity, `head` (every byte written) and
  `saved`. `saved` is how far `logd` has written to the files (`Log::setSaved()`), with the
  wall-clock offset of that boot for the times.
  - PSRAM keeps its contents across a reset; only a power cut loses them.
  - The header is valid only when the magic and capacity match and `saved <= head`. Random
    power-on contents pass with odds of about 2^-64. The header has no checksum, so a line is
    never slowed by keeping one.
  - 256 bytes stand unused in front of it: IDF's PSRAM timing tuning writes a test pattern at
    physical 0x80..0xff at every boot. The section may start at physical 0.
- **A panic writes into it.** A wrap of `panic_print_char` (all of IDF's panic output goes
  through it) also writes each character into the ring.
  - Each line becomes `[  sec.mmm] E panic: text`, a log line like any other. The uptime comes
    from the timer register, with no lock.
  - It writes without the log's lock: the other core is stalled, and the holder may be the
    task that panicked.
- **The caches are written back before the reset.** The ring is in write-back caches (L1, then
  the L2 in front of PSRAM), and a reset drops whatever is still dirty in them; IDF's restart
  writes back only the L1. Two places write the whole object back:
  - a wrap of `esp_restart_noos`, through which a panic, a watchdog's abort and `reboot` all
    reset;
  - the panic wrap, at each line's end, in case the handler hangs until the hardware watchdog.
- **The next boot puts the unsaved lines back in front.** `Log::init()` finds a valid header
  with `head > saved`. It copies the lines from `saved` to `head` (the last 64 KB at most, with
  a count of the bytes lost before them) out. It then starts the ring again with:
  - `--- recovered: the previous boot's unsaved lines (reset: panic) ---`;
  - those lines;
  - `--- end of the recovered lines ---`;
  - then this boot's lines, from `Log::bootPosition()`.

  `log show` shows the crash after the reset too. `logd` writes everything from position 0 as
  before, so the recovered lines reach `system.log` once the card mounts. Their uptime stamps
  become the previous boot's times when that boot had set its clock; otherwise they stay uptimes.
  `logd` writes `--- boot ---` where this boot's lines begin, not at its first write.
- **Without a card**, a crash loop keeps carrying the last lines forward, until they have been
  written or the 64 KB ring overwrites them.
- **What it can't catch:**
  - a hardware watchdog's reset (or a power cut), apart from what was written back;
  - a panic in `Log::init()` itself, or before it, which goes into the ring as it is;
  - text another firmware (the recovery firmware) wrote over that PSRAM.

**Cost:** the 64 KB that were a heap block are now static (no change in PSRAM use), plus 256
bytes. Logging a line costs nothing new: `setSaved()` is called once per batch `logd` writes.
On boot, one copy of at most 64 KB.

**Testing:** on the board, a panic forced deliberately; its text then appears in `log show` and
in `system.log` after the reset. Also: a reboot within a second of a line, which must not lose
it; a boot without the card and then a reboot with it, whose lines must reach the files; and a
cold power-up, which must not produce a false "recovered" block.

### 9a. As built (2026-10-19)

- **Built as designed:** the history in `.ext_ram_noinit` (at `0x48000000`, 256 bytes guarded
  in front), the recovered lines put back in front at boot, `Log::setSaved()` from `logd`,
  `Log::bootPosition()` and the previous boot's clock offset for their times.
  - **The panic capture, and why it changed from the design:** `--wrap` reaches only the calls
    from other object files, so wrapping `panic_print_char` caught nothing (IDF inlines it into
    `panic.c`). The register dump and the stack memory come from other files through
    `panic_print_str`, `_dec` and `_hex`, which are wrapped. The headline (`Guru Meditation
    Error: Core N panic'ed (reason)`) and an abort's message (`abort: assert failed: ...`) come
    from `panic.c` itself, so a wrap of `esp_panic_handler` writes those from its
    `panic_info_t` and `g_panic_abort_details`.
  - **Write-backs:** the L1 and L2 caches are written back at each panic line's end, and in a
    wrap of `esp_restart_noos`.
  - **No files kept:** with `log/dir` empty, `logd` sets `Log::NoFiles`, so nothing is
    recovered.
- **`reboot panic`** panics on purpose (`System::panic()`, an `esp_system_abort()`), for testing.
  `System::resetReason()` (moved from the boot code) names the reset in the recovered marker.
- **Found on the way:** the log's stamps ran 0.58 s ahead of `esp_timer`, so every time in the
  files had been 0.58 s late, and the panic lines (stamped from the timer) came before the line
  logged just ahead of them. `esp_log_timestamp()`'s base is a cycle count taken across the
  boot's clock changes. The stamps now come from the systimer (`Detail::monotonicUs()`), the
  clock of `uptime` and `Clock::monotonicNs()`. That also takes the kernel's lock out of every
  line: `xTaskGetTickCount()` took it. Stamps now count from the application's start
  (`[0.004] Reset`), not from the chip's reset.
- **On the board, all written to the files after the reset:**
  - an info line logged just before `reboot`, which is the case that used to be lost;
  - a forced panic: the abort line, the register dump and 32 lines of stack memory, as
    `E panic:` lines after the last line logged and 2 ms later than it;
  - lines logged while `log/dir` couldn't be written, with the previous boot's wall-clock times.
  The first boot after the flash, with no valid header, showed no false recovered block.
- **Not tested:** a real cold power-up, since the board's power can't be cut remotely; the
  header check covers it, as on the first boot after the flash. A hardware watchdog's reset
  (only what was written back survives) was not tested either.
- **Since 2026-10-29 a hardware reset loses nothing either** (`keptstate.md` §3): each line is
  written back from the caches as it's appended, and the header's counters are kept in the LP RAM
  as well, stored after the bytes; at boot they replace PSRAM's header. A line logged 30 ms before
  a USB control-line reset came back, with the marker's "32 bytes past PSRAM's header": PSRAM's
  own header had lost exactly that line.
- **Limits:**
  - The recovered lines keep uptime stamps when the previous boot had no clock at its last
    `setSaved()`.
  - Each `reboot` recovers the last second's lines (a small block). That is correct, only
    visible.
  - RISC-V prints no backtrace, only the stack memory. Decoding it needs that build's ELF; the
    previous boot's banner gives the build. **`tools/bmds-panic`** (2026-10-19) does it:
    - It takes the `E panic:` runs from `system.log` (several files, oldest first, for a
      rotation), `log show`, or stdin, and hands each to ESP-IDF's own decoder
      (`esp_idf_panic_decoder`: gdb unwinding the stack memory, as `idf.py monitor` does).
    - It prints the backtrace with functions, files and lines, and warns when the ELF isn't the
      build the banner names.
    - Checked on `reboot panic`: 11 frames, from `panic_abort` through `System::panic` and
      `runReboot` to the shell's executor.
