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
| `clock` | Five lines: `Time:` (with nanoseconds, kernel zone), `Source:` (name and when it was set, or `none (not set; counting from 2026-01-01)`), `Slew:` (`±S.NNNNNNNNN s remaining`), `Frequency:` (`±N ppb`), `Zone:` (the `TZ` string, abbreviation and UTC offset; a zone set by name shows the name first, or that it is pending, §9). |
| `clock zone TZ` / `clock adjust ±SECONDS` | Set the kernel time zone (POSIX rules, or a name in the database, §9: `clock: unknown time zone 'X'` if it hasn't that name); slew by hand (mostly for testing the slew). |
| `clock tzdata [PATH]` | The time zone database (§9): `/sd0/p0/share/zoneinfo.bin: tzdata 2026e, 597 zones`, or why it can't be read; with `PATH`, use that file. |

`TZ` (a shell variable; exported or not) overrides the kernel zone for display, as on Unix; one that doesn't parse means UTC, as with glibc. A name (`TZ=Europe/Berlin`) is looked up in the database (§9); not found means UTC, as with musl.

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

## 9. A time zone database (done, 2026-10-06)

**Why.** Today a zone is a POSIX `TZ` string (`CET-1CEST,M3.5.0,M10.5.0/3`). A user knows "Europe/Berlin", not the rules. And a rule string is only today's rules: a date from before they changed (an old file's time, a program's `localtime` of 1990) comes out wrong. This revises §1's "no tz database" for display and for programs. The kernel itself still keeps UTC and needs only today's rules.

**Facts** (IANA tzdata 2026e, built with `zic -b slim`, measured 2026-10-06):
- 597 names: 344 distinct TZif files, the rest aliases (links). 205,747 bytes of distinct data, the largest 2,464. (The build host's own 2026c, built "fat", was 474,824.)
- Every file ends with its zone's current rules as a POSIX `TZ` string (Europe/Berlin: `CET-1CEST,M3.5.0,M10.5.0/3`). The longest is 44 characters (Pacific/Chatham), within `TimeZone::MaxSpec` (47). Some use what `TimeZone` must handle: negative DST (Europe/Dublin `IST-1GMT0,M10.5.0,M3.5.0/1`), a negative transition time (America/Nuuk `.../-1`), a 30-minute DST (Australia/Lord_Howe).
- The longest name is 32 characters (America/Argentina/ComodRivadavia).
- musl reads TZif files itself. A `TZ` not in POSIX form (a name followed by an offset, or `UTC`/`GMT`) is a name, looked up in `/usr/share/zoneinfo/`, `/share/zoneinfo/` and `/etc/zoneinfo/` (`:` first forces a name; names with `.` are refused); programs map the file (`open`, `fstat`, `mmap`: linux.c reads it into memory).

**The data in git** (the user's ask, 2026-10-06): `thirdparty/tzdata/` holds the pinned release's source files, unmodified: the zone files `zic` reads (`africa`, `antarctica`, `asia`, `australasia`, `europe`, `northamerica`, `southamerica`, `etcetera`, `backward`), `zone1970.tab`, `zone.tab`, `iso3166.tab` (for a zone picker later), `version`, `LICENSE` (public domain) and `README`; 0.85 MB of text, so an update is a reviewable diff. `backzone` (pre-1970 history merged into links) and `factory` stay out, as in a default IANA build.

**Updating:** `tools/update-tzdata [VERSION]` (the newest release by default) downloads `tzdataVERSION.tar.gz` and its signature from data.iana.org, checks the signature with `gpgv` against the tzdata maintainer's key kept in `tools/tzdata-key.asc` (Paul Eggert, `7E37 92A9 D8AC F7D6 33BC 1588 ED97 E90E 62AA 7E34`), replaces the files above, builds the bundle once as a check, and prints the old and new version and the release's NEWS entry. It never commits: the diff is reviewed like any other.

**One file on the card, not 597.** On FAT32 with 32 KB clusters each small file takes a whole cluster (344 files: 11 MB), on exFAT with 128 KB clusters 43 MB; and listing 597 FAT files is slow. The database is therefore one bundle, `zoneinfo.bin` (an on-disk format, frozen once shipped; little-endian):
- **Header**, 32 bytes: magic `BMTZ`, format version (u16, 1), header size (u16, 32), the tzdata version (8 bytes, NUL-padded: `2026e`), the entry count (u32), the index offset (u32), the entry size (u32, 48: room for longer names later without a new format), the file size (u32: a truncated copy is refused).
- **Index**, sorted by name (bytes, as `strcmp`): each entry the name (NUL-padded, entry size − 8 bytes: up to 39 characters today), then the data's offset and length (u32 each). An alias is one more entry pointing at its zone's data; identical files are stored once.
- **Data:** the TZif files, each starting `TZif`.
- About 234 KB in all (597 × 48 + 205,747). A lookup is a binary search: about 10 reads of 48 bytes, then the zone's last 64 bytes (its rules) or all of it.

`tools/tzbundle` (Python, standard library) builds it with the host's `zic -b slim`. The distribution builds it as the package **`tzdata`** (`dist/tzdata/`), whose source is the in-tree `thirdparty/tzdata` (a new `source_tree` key in `package.toml`, with no download), so `bmds-dist build tzdata` and `bmds-dist install` put it on the card at `share/zoneinfo.bin`. The default place is `/sd0/p0/share/zoneinfo.bin`, Kconfig `BMDS_ZONEINFO_PATH` (bmds_storage), changeable at run time with `clock tzdata PATH` since no path is a singleton. It lives on the card, not in `/boot` (agreed with the user, 2026-10-06): `/boot` keeps its 4 MB for settings.

**Programs see ordinary files.** The VFS shows the bundle, read-only, as a virtual directory at **`/share/zoneinfo`**, one of musl's own search paths, the way `/bin` shows the programs built in (`/share` lists `zoneinfo`; nothing can be mounted at or below `/share/zoneinfo`). Subdirectories (`America/Argentina`) come from the names. Opening `/share/zoneinfo/Europe/Berlin` reads that zone's bytes into a read-only, seekable stream of its own (Psram, freed when closed), so `fstat` gives its size and musl's `mmap` works. With no musl patch, `TZ=Europe/Berlin` then works in every program with the full history, and so do BusyBox's `date` and `ls /share/zoneinfo`. Without the bundle the directory is empty and a name means UTC, as musl does with an unknown name. Identity: a zone file's id is its path's, device 0.

**The kernel's zone** accepts a name wherever it takes a `TZ` string (`clock zone Europe/Berlin`, `CONFIG_BMDS_TIMEZONE`, the shell's `TZ` for `date` and `ls -l`), told apart as musl does. `Clock` (bmds_kernel) can't read files, so the VFS installs a resolver (`Clock::setZoneResolver()`); a name is looked up there and the file's last line, the current rules, becomes its `TimeZone`. The kernel keeps the name, so that:
- `clock` shows both (`Zone:      Europe/Berlin (CET-1CEST,M3.5.0,M10.5.0/3; CEST, UTC+0200)`), and a name not found yet as pending (`Zone:      Europe/Berlin (pending: no zone database; UTC until it is found)`), so a missing card doesn't pass for UTC on purpose;
- the loader passes the name, not the rules, as a program's `TZ` (so musl uses the history).

A name the database doesn't have is refused (`clock: unknown time zone 'Europe/Berln'`) and the zone stays as it was; with no database to ask, the name is kept pending.

**When the lookup happens** (the user's ask: at boot, or when `TZ` changes):
- **On a change:** the shell's `TZ` is looked up when it changes; one name and its rules are kept (in bmds_fs, shared, under the storage lock), so `ls -l` doesn't look up per line. A mount, an unmount or a change to the bundle drops it (the VFS's change notifications).
- **At boot:** the card may mount after `/boot/autoexec.sh` or `/boot/profile` sets the zone. A name that can't be found yet stays pending (UTC meanwhile) and is looked up again after each mount.
- **Without a card** (agreed with the user, 2026-10-06): a name means UTC. A zone that must be right without the card is set as a POSIX `TZ` string in `/boot/profile` (or `/boot/autoexec.sh`), as today; nothing is copied into `/boot` behind the user's back.
- **A new bundle** copied over the old one is seen by the shell and programs at once; the kernel's own rules change at the next `clock zone` or boot (they are only today's rules, and a tzdata update rarely changes them).

**Shell:** `clock tzdata` shows the database (`/sd0/p0/share/zoneinfo.bin: tzdata 2026e, 597 zones`, or why there is none); `clock tzdata PATH` uses another.

**Budget (estimate):** firmware a few KB (the bundle reader, the virtual directory, the kernel's name); no RAM at rest (the index is searched by reads; the one cached lookup is about 160 bytes of `.bss`); a zone opened costs its bytes (at most 2,464 today) while open; on the card, the bundle (about 234 KB).

**As built (measured 2026-10-06):** the firmware grew 8,938 bytes (718,208 to 727,146) for this and `/dev/zero`, `/dev/urandom` (storage.md §4) together, before the random pool (random.md); `.bss` 144 bytes. The bundle is 234,435 bytes; sent to the card with `tools/bmds-run -n -d /sd0/p0/share` in 3.2 s. On the board: `ls /share/zoneinfo` lists 60 entries; `clock tzdata` says `tzdata 2026e, 597 zones`; `TZ=Europe/Moscow busybox date -d @646916400` gives `15:00:00 MSD 1990` from the file's history where the rules alone (`MSK-3`) give `14:00:00 MSK`; `clock zone America/Winnipeg` with the card unmounted shows it pending and `/share/zoneinfo` empty, and `mount` resolves it (`EST5`, 2026e's new rule); a program started under `clock zone Europe/Berlin` gets `TZ=Europe/Berlin`. Lookups, listings, reads and remounts leave memory where it was. Not yet tried on the board: a name in `CONFIG_BMDS_TIMEZONE` or `/boot/autoexec.sh` at boot (the host tests cover the pending-then-mounted path).

**ABI impact:** append-only. `/share/zoneinfo` is a new path; `TZ` gains names where it took POSIX strings (both still work); the bundle's format; `clock tzdata`. No new slot.

**Testing:**
- **Host:** the bundle's reader on the real 2026e bundle (built by the host test build when `zic` is found) and on made-up ones (aliases, a damaged or truncated bundle refused); every zone's footer parses as a `TimeZone`; the virtual directory's listing and reads; names in the kernel zone, pending until the bundle's filesystem mounts; the shell's `TZ` names.
- **Board:** `TZ=Europe/Berlin date` against the POSIX string; `progtest` `localtime` of a 1990 date in a zone whose rules changed since (history from the file); `clock zone` with a name at boot from `/boot/autoexec.sh` before the card mounts (UTC and `clock` showing it pending, then the zone once it mounts); a POSIX string in `/boot/profile` with no card; the card removed (the directory empties, the kernel keeps the rules it has).
