# PSRAM at 200 MHz: Verified Before Trusted

Status: **the default since 2026-10-05** (the user's decision, §4): `sdkconfig.defaults` sets
`CONFIG_SPIRAM_SPEED_200M` (with `CONFIG_IDF_EXPERIMENTAL_FEATURES`, which IDF requires for it);
the boot-time test stays off (1.3 s a boot). The cold-boot check was not run.

## 1. Goal

Find out whether the PSRAM (32 MB, AP Memory HEX, on the ESP32-P4NRW32 module, chip revision
1.3) runs **without corrupting data** at 200 MHz, and what that buys: PSRAM bandwidth, the
display's stability under PSRAM traffic (`display.md` §8, the 2D-DMA entry), and the PSRAM
framebuffer (`display.md` §8, the framebuffer table). The user's requirement: be *super* certain
nothing is corrupted before 200 MHz is used for real.

## 2. Facts

- IDF offers 200 MHz on the P4 only behind `CONFIG_IDF_EXPERIMENTAL_FEATURES`
  (`esp_psram/esp32p4/Kconfig.spiram`). The PSRAM's clock then comes from the MPLL at 400 MHz
  (320 MHz at 80 MHz), with longer read and write latencies, and IDF tunes the DQS phase and
  delay line at boot against a 128-byte pattern (`mspi_timing_by_dqs.c`).
- Boot-time tuning is one sample at one temperature. A margin problem shows up as rare bit errors
  under load or warm, not as a failed boot, so the test has to be long, loaded and repeated.
- `CONFIG_SPIRAM_MEMTEST` (IDF's boot test) covers the whole PSRAM once, before the heap.
- Nothing is written to the SD card during the 200 MHz sessions: a corrupted buffer must not get
  onto a disk.

## 3. Plan

1. **`memtest`** (`bmds_drivers`, a bring-up command): tests the free PSRAM, all of it but a
   reserve, without disturbing what's allocated.
   - Patterns, keyed by each word's address so that every word is unique and the chunks are
     independent: the address, its inverse, a checkerboard, and a pseudo-random hash of the
     address and the pass (a new sequence every pass). The pattern code is portable and host
     tested.
   - The CPU path: fill a chunk, write it back and **invalidate** it in the caches, so the verify
     reads the PSRAM itself and not the cache.
   - The DMA path: the AXI GDMA copies one half of each region onto the other, then the CPU
     verifies it (DMA reads and writes, as a framebuffer scan would use it).
   - It reports throughput (MB/s for writes, reads, DMA) and every error's address, expected and
     actual value and their XOR (a fixed bit points at a data line, a fixed address at a cell).
   - Passes: a count, or until Ctrl-C.
2. **Baseline at 80 MHz** (the shipping configuration): the test must report 0 errors, which
   validates the test, and gives the bandwidth to compare against.
3. **The 200 MHz build is separate** (`sdkconfig.psram200`, applied to a copy of the current
   `sdkconfig` in `build-psram200/`), so the normal build never changes. It also turns on IDF's
   boot test.
4. **At 200 MHz:** the boot test, then `memtest` for many passes, idle and with the display
   scanning out, plus the `display stress` cases. Then cold boots: the board off for a while and
   tested immediately on power-up, then warm after a long run.
5. **Decision:** only with zero errors throughout does 200 MHz become a candidate for the
   default, and then still as a separate change the user decides.

(The separate build, `sdkconfig.psram200` in `build-psram200/`, served until the decision below;
both are gone.)

## 4. Results

`memtest` (2026-10-05) tests 30,464 KB, all the free PSRAM but the 2 MB reserve, with the 720p60
test picture scanning out the whole time. Per pass: four CPU patterns, then the DMA copy.

| | 80 MHz | 200 MHz |
|---|---|---|
| IDF's boot test (all 32 MB) | not run (off in the normal build) | OK |
| `memtest`, passes / errors | 3 / 0 | 10 / 0, then a soak of 150 / 0 |
| CPU write / read, address pattern (MB/s) | 32 / 37 | 40 / 41 |
| GDMA copy, PSRAM to PSRAM (MB/s) | 61 | 120 |
| `display stress psram` (copy, MB/s) | 64 | 102 |
| `display stress write` (MB/s) | 89 | 169 |
| Display underruns while `memtest` runs | 636 in 28 s | 60 in 82 s |

- `memtest`'s CPU numbers are bounded by the test itself (computing the patterns, the per-chunk
  write-back and invalidate), not the PSRAM: they move only 25%. The DMA and the plain copies show
  the bus: about twice as fast.
- The display is much better off (a tenth of the underruns per second under `memtest`) but not
  safe: the `display stress` cases still upset it at 200 MHz.
- **The soak:** 150 passes (750 pattern runs over 30,464 KB, CPU and DMA) in 1,244 s, 0 errors,
  with the test picture scanning out throughout. Still to do: cold boots (powered off for a while,
  tested at once). The user keeps the 200 MHz image on the board for daily use
  meanwhile, with no SD card writes of anything that matters.
- **Warm run** (2026-10-05, 200 MHz image): `memtest -p 0` for 45 minutes (155 passes, the board
  up 12 minutes before) under 720p from the RGB565 framebuffer with `display anim start 32`:
  **0 errors**; 86 of 4,881,807 blocks skipped, the animation 0 errors and 0 of 921,600 pixels
  wrong at the end; DMA copies 65-71 MB/s.
- **Decision** (2026-10-05, the user): 200 MHz is the default. The display needs it (surface 0 is
  scanned straight from PSRAM, `display.md` §5c, which 80 MHz couldn't feed), and everything run at
  it found 0 errors: the soak, the warm run, and a 35-minute scan soak under `memtest` and CPU
  stress with 0 underruns. Not run: a cold boot (powered off a while, tested at once). With the
  default image: boot to the picture in 0.45 s, `memtest -p 2` 0 errors with the scan running,
  0 underruns.
