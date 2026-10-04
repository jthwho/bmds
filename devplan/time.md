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

`TimeZone` (in `bmds_core`, pure): parses POSIX `TZ` strings (`UTC0`, `CET-1CEST,M3.5.0,M10.5.0/3`, `EST5EDT,M3.2.0,M11.1.0`) and converts UTC ↔ local, with the DST rules evaluated per year. No tz database; the POSIX form covers fixed offsets and rule-based DST, which is what an embedded system needs. Calendar conversion (days ↔ civil date) is also pure and host-tested.

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

The board has a 32.768 kHz crystal for the always-on RTC domain, whose timer keeps counting through software resets and panics (not through a power cut, unless the optional LiPo keeps the board powered). After every change, the kernel stores `wall − rtcTime` (and the frequency) with a magic number and checksum in `RTC_NOINIT` memory, and a shutdown handler refreshes it before `esp_restart()`; at boot, if that record is valid, the clock resumes from `esp_rtc_get_time_us()` with source `Rtc`. After a power cut the record is gone and the clock starts unset. **Measured:** `reboot` moves the clock by under 1 ms; a reset through the RESET line (the flasher's RTS) also keeps it, since the RTC domain stays powered (2.8 ms moved, without the shutdown handler). The time zone is not carried over: it comes from `CONFIG_BMDS_TIMEZONE` at boot, and `/sd/autoexec.sh` can run `clock zone`.

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
