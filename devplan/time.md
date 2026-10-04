# Time: the System Clock

Status: **design, not started** (Phase 2; needed for file time stamps).

## 1. Goal

Timekeeping as the Linux kernel does it:

- A **monotonic clock** that never jumps (uptime, timeouts, intervals).
- A **software system clock** (wall time, UTC) that is *set* by a source and then *disciplined* by it: stepped once, then slewed and frequency-corrected so it stays right without jumping. Sources come later and plug in: `date -s` by hand now; NTP with networking (Phase 5), PTP over Ethernet, GPS, a host over the serial cable.
- The kernel keeps **UTC only**. Time zones are presentation: a POSIX `TZ` string for display and for the FAT time stamps that need local time.
- A set clock **survives reboots** (software resets, panics), so it doesn't have to be set again after `reboot`.

## 2. Interface (`bmds_kernel`)

```cpp
struct Timestamp { int64_t ns; };      // nanoseconds since 1970-01-01 00:00:00 UTC

namespace Clock {
        int64_t   monotonicNs();       // since boot; never steps; ISR-safe
        Timestamp now();               // wall time (UTC); ISR-safe
        bool      isSet();             // false until a source has set it
        ClockSource source();          // None, Manual, Rtc (carried over a reset), Ntp, Ptp, ...

        Error set(Timestamp t, ClockSource source);   // step (settimeofday)
        Error adjust(int64_t offsetNs);               // slew by offset, at most 500 ppm (adjtime)
        Error setFrequency(int32_t ppb);              // frequency correction, ±500 ppm (adjtimex, for an NTP/PTP loop)
        ClockStatus status();                         // offset still to slew, frequency, source, last set

        Error setTimeZone(StringView posixTz);        // kernel default zone (FAT time stamps, `date`)
}
```

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
- Readers take a `SpinLock` for a few loads (64-bit values on a 32-bit CPU); `now()` is ISR-safe and costs well under a microsecond.

### Across reboots

The board has a 32.768 kHz crystal for the always-on RTC domain, whose timer keeps counting through software resets and panics (not through a power cut, unless the optional LiPo keeps the board powered). When the clock is set, the kernel stores `wall − rtcTime` with a magic number and checksum in `RTC_NOINIT` memory; at boot, if that record is valid, the clock resumes from `esp_rtc_get_time_us()` with source `Rtc`. After a power cut the record is gone and the clock starts unset.

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
| `date` | Local time in `$TZ` (or the kernel zone), `date`'s default format: `Sat Oct  3 14:02:11 UTC 2026`; `(clock not set)` on stderr if unset. |
| `date +FORMAT` | `strftime` subset: `%Y %m %d %H %M %S %y %b %a %j %Z %z %s %N %F %T %%`. |
| `date -u` | UTC. |
| `date -s 'YYYY-MM-DD HH:MM[:SS]'` | Sets the clock (source `Manual`), the string read in local time. Also `date -s @SECONDS`. |
| `clock` | Status: source, set/unset, last set, slew remaining, frequency, time zone. |
| `clock zone TZ` / `clock adjust ±SECONDS` | Set the kernel time zone; slew by hand (mostly for testing the slew). |

`TZ` (shell variable, exported) overrides the kernel zone for display, as on Unix.

## 5. Resource budget (estimates)

~4 KB flash (clock, calendar, `TimeZone`, `date`, `clock`); 32 bytes `RTC_NOINIT`; ~64 bytes of state (internal: read from ISRs).

## 6. ABI impact

- Phase 3 syscalls: `clock_gettime(REALTIME | MONOTONIC)`, a `settime` for privileged use, and time zone lookup; `Timestamp` as `int64_t` nanoseconds (good to the year 2262).
- `date`, `clock` options and output formats; `TZ` honored as POSIX `TZ` strings.

## 7. Testing

Host: calendar round trips over centuries and leap years; `TimeZone` parsing and DST transitions (both hemispheres, the ambiguous and missing hours); the clock model with a fake time base (step, slew completes at 500 ppm and never goes backwards, frequency correction, re-anchoring). Board: set, `reboot`, still set; drift against a host over a few hours.

## 8. Open questions

- Should the RTC timer be calibrated against the 40 MHz crystal (it runs from the 32 kHz crystal, typically ±20 ppm) before the reset record is trusted for long gaps? Measure.
- A serial time source: the host pushes its time when a terminal connects (an escape-sequence handshake, or `rz`-time). Cheap and useful until there's a network.
