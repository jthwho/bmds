# Time: the System Clock

Status: **done** (Phase 2 step 2a; verified on the board). File time stamps use it from step 2b.

## 1. Goal

Timekeeping as the Linux kernel does it:

- A **monotonic clock** that never jumps (uptime, timeouts, intervals).
- A **software system clock** (wall time, UTC) that is *set* by a source and then *disciplined* by it: stepped once, then slewed and frequency-corrected so it stays right without jumping. Sources come later and plug in: `date -s` by hand now; NTP with networking (Phase 5), PTP over Ethernet, GPS, a host over the serial cable.
- The kernel keeps **UTC only**. Time zones are presentation: a POSIX `TZ` string for display and for the FAT time stamps that need local time.
- A set clock **survives reboots** (software resets, panics), so it doesn't have to be set again after `reboot`.

## 2. Interface (`bmds_kernel`)

```cpp
struct Timestamp { int64_t ns; };      // nanoseconds since 1970-01-01 00:00:00 UTC (bmds_core)

namespace Clock {
        Error     init();              // boot stage: kernel zone, resume after a reset
        int64_t   monotonicNs();       // since boot; never steps
        Timestamp now();               // wall time (UTC)
        bool      isSet();             // false until a source has set it
        ClockStatus status();          // source (None, Manual, Rtc, Ntp, Ptp, Gps, Host), last set, slew left, frequency

        Error   set(Timestamp t, ClockSource source);   // step (settimeofday)
        int64_t adjust(int64_t offsetNs);               // slew at 500 ppm, replacing what was left (adjtime); returns that
        Error   setFrequency(int32_t ppb);              // frequency correction, ±500 ppm (adjtimex, for an NTP/PTP loop)

        Error     setTimeZone(StringView posixTz);      // kernel zone (FAT time stamps, `date`)
        TimeZone  timeZone();
        LocalTime localTime(Timestamp t);               // in the kernel zone
        void      saveResetRecord();                    // also run by a shutdown handler before esp_restart
}
```

In `bmds_core`, all pure and host-tested: `Timestamp`, `Calendar` (days ↔ civil date), `DateTime`, `LocalTime`, `formatTime()` (the `strftime` subset), `TimeZone`, and `ClockModel` (the arithmetic below, as a function of a monotonic time the caller passes).

`TimeZone` (in `bmds_core`, pure): parses POSIX `TZ` strings (`UTC0`, `CET-1CEST,M3.5.0,M10.5.0/3`, `EST5EDT,M3.2.0,M11.1.0`) and converts UTC ↔ local, with the DST rules evaluated per year. No tz database; the POSIX form covers fixed offsets and rule-based DST, which is what an embedded system needs. (Revised for names and history: §9 plans a database on the card; the kernel still runs on POSIX rules.) Calendar conversion (days ↔ civil date) is also pure and host-tested.

## 3. Design

### The clock model

```
monotonic  = esp_timer (64-bit µs from the system timer, 40 MHz crystal)
wall(m)    = anchorWall + (m - anchorMono) * (1 + freq) + slewApplied(m)
```

- An **anchor** pair `(anchorMono, anchorWall)` is re-taken at every change, so the arithmetic stays exact over any uptime.
- **Frequency** (`freq`, in ppb) corrects the crystal's error. A discipline loop (NTP/PTP, later) sets it; nothing else touches it.
- **Slewing:** `adjust(offset)` records an offset to apply; it is applied lazily at 500 ppm (the Linux `adjtime` rate), so a 50 ms correction takes 100 s and time never jumps or runs backwards. When it is used up the anchor is re-taken.
- **Stepping:** `set()` replaces the anchor. Like Linux, a step is the source's decision (e.g. NTP steps beyond 128 ms and slews below).
- **Locking:** the model (48 bytes) and the zone each live in a `SeqLocked<T>`: a sequence lock over 32-bit atomic words. Readers take no lock (they copy, and copy again if a writer was busy); writers are serialized and run with task switching suspended on their core, so a reader can't preempt a writer and spin. Acquire loads and release stores, no standalone fences, so ThreadSanitizer checks it. The price: `now()` is **task context only** (an ISR on the writer's core would spin), a change from the first design; nothing needs wall time in an ISR.
- Arithmetic without 128-bit integers (none on the target): `Δ·ppb/10⁹` is computed as whole seconds and the rest separately, exact for centuries of uptime.

### Across reboots

The board has a 32.768 kHz crystal for the always-on RTC domain, whose timer keeps counting through software resets and panics (not through a power cut, unless the optional LiPo keeps the board powered). After every change, the kernel stores `wall − rtcTime` (and the frequency) with a magic number and checksum in `RTC_NOINIT` memory, and a shutdown handler refreshes it before `esp_restart()`; at boot, if that record is valid, the clock resumes from `esp_rtc_get_time_us()` with source `Rtc`. After a power cut the record is gone and the clock starts unset. **Measured:** `reboot` moves the clock by under 1 ms; a reset through the RESET line (the flasher's RTS) also keeps it, since the RTC domain stays powered (2.8 ms moved, without the shutdown handler). The time zone is not carried over: it comes from `CONFIG_BMDS_TIMEZONE` at boot, and `/sd0/p0/autoexec.sh` can run `clock zone`.

### Unset clock

Until a source sets it, `isSet()` is false and `now()` counts from 2026-01-01 00:00 UTC plus uptime, so time stamps are plausible and ordered rather than 1970. `date` says the clock is unset.

### IDF usage

| Need | Choice | Why |
|---|---|---|
| Monotonic time base | `esp_timer_get_time()` | Already the kernel's (`System::uptimeUs()`, `Deadline`). |
| Time across resets | `esp_rtc_get_time_us()` + an `RTC_NOINIT_ATTR` record | The RTC domain survives resets; no flash writes (none of NVS's wear or its no-PSRAM-stack rule). |
| newlib `time()`/`gettimeofday()` | Kept in step with `Clock` (`settimeofday` on each step) | IDF components that log or stamp with `time()` see the same wall time. BMDS code uses `Clock`. |

## 4. Shell

| Command | Behavior |
|---|---|
| `date` | Local time in `$TZ` (or the kernel zone), `date`'s default format: `Sat Oct  3 14:02:11 UTC 2026`; `date: the clock is not set` on stderr if unset (only with the default format, so scripts using `+FORMAT` stay quiet). |
| `date +FORMAT` | `strftime` subset: `%a %A %b %B %c %C %d %D %e %F %h %H %I %j %k %l %m %M %n %N %p %r %R %s %S %t %T %u %w %y %Y %z %Z %%`; anything else is copied. |
| `date -u` | UTC. |
| `date -s DATE` | Sets the clock (source `Manual`) and prints it: `'YYYY-MM-DD HH:MM[:SS]'` (or `T` between), `YYYY-MM-DD` (midnight), `HH:MM[:SS]` (today), read in the display zone (`-u`: UTC); `@SECONDS[.frac]`. An invalid date: `date: invalid date '...'`, status 1. |
| `clock` | Five lines: `Time:` (with nanoseconds, kernel zone), `Source:` (name and when it was set, or `none (not set; counting from 2026-01-01)`), `Slew:` (`±S.NNNNNNNNN s remaining`), `Frequency:` (`±N ppb`), `Zone:` (the `TZ` string, abbreviation and UTC offset). |
| `clock zone TZ` / `clock adjust ±SECONDS` | Set the kernel time zone; slew by hand (mostly for testing the slew). |

`TZ` (a shell variable; exported or not) overrides the kernel zone for display, as on Unix; one that doesn't parse means UTC, as with glibc.

## 5. Resource budget (estimates)

Estimated ~4 KB flash. **Measured: +11.9 KB** (calendar, `TimeZone` and `formatTime` 5.9 KB in `bmds_core`; `Clock`, `SeqLocked` and the reset record 2.1 KB; `date` and `clock` 2.3 KB; strings); 172 bytes of internal `.data` (the two `SeqLocked` values, initialized at compile time); 24 bytes of `RTC_NOINIT`. Boot stage: 0.8 ms.

## 6. ABI impact

- Phase 3 syscalls: `clock_gettime(REALTIME | MONOTONIC)`, a `settime` for privileged use, and time zone lookup; `Timestamp` as `int64_t` nanoseconds (good to the year 2262).
- `date`, `clock` options and output formats; `TZ` honored as POSIX `TZ` strings.

## 7. Testing

Host (done): calendar round trips 1600-2400 and leap years; `formatTime`; `TimeZone` parsing (malformed forms too) and DST transitions (Europe, US with explicit and default rules, Australia, Julian rules, the ambiguous and missing hours); the clock model with a fake time base (step, slew at 500 ppm never going backwards, replaced adjustments, frequency correction over a century); `SeqLocked` against a concurrent reader (also under ThreadSanitizer); `date` and `clock` through the shell. Board (done): set, `reboot` and RESET line, still set to the millisecond; slew rate. Still to do: drift against a host over a few hours.

## 8. Open questions

- The reset record is refreshed on every change and before `esp_restart()`, but not before a panic or watchdog reset: after a long uptime, those resume with the RTC timer's drift against the main crystal since the last change (typically ±20 ppm: a minute a month). A periodic refresh (say hourly, one timer) would bound it. Measure the drift first.
- Should the RTC timer be calibrated against the 40 MHz crystal (it runs from the 32 kHz crystal, typically ±20 ppm) before the reset record is trusted for long gaps? Measure.
- A serial time source: the host pushes its time when a terminal connects (an escape-sequence handshake, or `rz`-time). Cheap and useful until there's a network.

## 9. A time zone database (planned, 2026-10-06)

**Why.** Today a zone is a POSIX `TZ` string (`CET-1CEST,M3.5.0,M10.5.0/3`). A user knows "Europe/Berlin", not the rules. And a rule string is only today's rules: a date from before they changed (an old file's time, a program's `localtime` of 1990) comes out wrong. This revises §1's "no tz database" for display and for programs. The kernel itself still keeps UTC and needs only today's rules.

**Facts** (measured on the build host's tzdata 2026c, as its Linux ships it):
- 447 zone files (TZif) and 304 aliases (links), 474,824 bytes in all, the largest 3,872.
- Every file ends with its zone's current rules as a POSIX `TZ` string (Europe/Berlin: `CET-1CEST,M3.5.0,M10.5.0/3`). The longest is 44 characters (Pacific/Chatham), within `TimeZone::MaxSpec` (47).
- The longest name is 32 characters (America/Argentina/ComodRivadavia).
- musl reads TZif files itself. A `TZ` that isn't a POSIX string is a name looked up in `/usr/share/zoneinfo/`, `/share/zoneinfo/` and `/etc/zoneinfo/`, or an absolute path; programs can map files (linux.c).

**One file on the card, not 447.** On FAT32 with 32 KB clusters each small file takes a whole cluster (447 files: 14 MB), on exFAT with 128 KB clusters 57 MB. The database is therefore one bundle, `zoneinfo.bin`, consisting of:
- a header (a magic number, the tzdata version);
- a sorted index of names, each with an offset and a length (an alias is one more entry pointing at its zone's data);
- the TZif files.

`tools/bmds-dist` builds the bundle from a pinned IANA release with `zic -b slim` and installs it on the card with BusyBox. The default place is `/sd0/p0/share/zoneinfo.bin`, a setting (Kconfig default, changeable at run time) since no path is a singleton. It lives on the card, not in `/boot` (agreed with the user, 2026-10-06): `/boot` keeps its 4 MB for settings.

**Programs see ordinary files.** The VFS shows the bundle, read-only, as a virtual directory at **`/share/zoneinfo`**, one of musl's own search paths, the way `/bin` shows the programs built in. Opening `/share/zoneinfo/Europe/Berlin` gives a memory stream of that zone's bytes. With no musl patch, `TZ=Europe/Berlin` then works in every program with the full history, and so do BusyBox's `date` and `ls /share/zoneinfo`. Without the bundle the directory is empty and a name means UTC, as musl does with an unknown name.

**The kernel's zone** accepts a name wherever it takes a `TZ` string (`clock zone Europe/Berlin`, `CONFIG_BMDS_TIMEZONE`, the shell's `TZ` for `date` and `ls -l`). It looks the name up in the bundle and uses the file's last line, the current rules, as its `TimeZone`. It keeps the name, so that:
- `clock` shows both (`Zone: Europe/Berlin (CET-1CEST,M3.5.0,M10.5.0/3)`), and a name not found yet as pending (`Zone: Europe/Berlin (pending: no zone database; UTC until it is found)`), so a missing card doesn't pass for UTC on purpose;
- the loader passes the name, not the rules, as a program's `TZ` (so musl uses the history).

**When the lookup happens** (the user's ask: at boot, or when `TZ` changes):
- **On a change:** the shell's `TZ` is looked up when it changes; one name and its rules are kept, so `ls -l` doesn't look up per line.
- **At boot:** the card may mount after `/boot/autoexec.sh` or `/boot/profile` sets the zone. A name that can't be found yet stays pending (UTC meanwhile) and is looked up again when a filesystem is mounted (the VFS's mount waiters).
- **Without a card** (agreed with the user, 2026-10-06): a name means UTC. A zone that must be right without the card is set as a POSIX `TZ` string in `/boot/profile` (or `/boot/autoexec.sh`), as today; nothing is copied into `/boot` behind the user's back.

**Budget (estimate):** firmware a few KB (the bundle reader, the virtual directory); no RAM at rest (the index is searched by reads); a zone opened costs its bytes (at most 3,872 today) while open; on the card, the bundle (about 0.5 MB).

**ABI impact:** append-only. `/share/zoneinfo` is a new path, and `TZ` gains names where it took POSIX strings (both still work). No new slot.

**Testing:**
- **Host:** the bundle's reader (index, aliases, a damaged or truncated bundle refused); the footer into a `TimeZone`; the virtual directory's listing and reads; pending names resolved by a mount.
- **Board:** `TZ=Europe/Berlin date` against the POSIX string; `progtest` `localtime` of a 1990 date in a zone whose rules changed since (history from the file); `clock zone` with a name at boot from `/boot/autoexec.sh` before the card mounts (UTC and `clock` showing it pending, then the zone once it mounts); a POSIX string in `/boot/profile` with no card; the card removed (the directory empties, the kernel keeps the rules it has).

**Open:** updating tzdata (a new bundle copied over the old one: the VFS's change hook drops the kernel's cached lookup).
