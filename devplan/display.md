# Display: HDMI through the LT8912B, a PSRAM Framebuffer, Text Console

Status: **step 1 done; the picture is surface 0, an RGB888 surface in PSRAM the DW-GDMA scans directly (§5c, done)** (Phase 4). The first plan (2026-10-04) had no framebuffer: lines rendered into internal RAM as they were scanned out. Measurement changed it twice (2026-10-05, with the user, §8): first an RGB565 double buffer widened by the 2D-DMA into a free-running ring of internal line buffers (§5b), then, with the PSRAM at 200 MHz, no ring at all: the DMA reads an RGB888 surface where it is (§5c, `windows.md` §0). It needs the PSRAM at 200 MHz (`psram.md`).

## 1. Goal

- A picture on an HDMI monitor, from the P4's MIPI-DSI controller through the board's Lontium LT8912B bridge.
- **A framebuffer in PSRAM, double-buffered, RGB565** (§5b): drawing goes to the back buffer (drawn in internal RAM, copied in by DMA), `present()` swaps the buffers at the start of a frame. The DSI controller's DMA streams a small free-running ring of RGB888 line buffers in internal RAM, which the 2D-DMA fills from the front buffer just ahead of the scan. A late fill never stops the stream: the block goes out stale.
- Know the monitor: hot-plug state and its EDID (name, preferred mode).
- A **text console** as the first renderer (`terminal.md` §3): a cell grid with 24-bit colors, glyphs drawn into each line as it goes out; the shell's output on the screen, and with a keyboard (Phase 5) a full local shell.
- **Later** (not this plan's steps): display modes of several depths for programs (8-bit palette, 16-bit, 24-bit), video mode testing beyond 720p, and a fast graphics scheme for a line-based display (tiles, sprites, a palette framebuffer expanded per line).

## 2. Facts that shape the design

- **The bridge** (`docs/HARDWARE.md`): LT8912B on the main I²C bus (GPIO7/8, **100 kHz**: at 400 kHz reads and writes were corrupted), three register pages at 0x48 (main), 0x49 (CEC/DSI), 0x4A (AVI infoframe). Reset is an RC circuit only, so **an ESP reset doesn't reset the bridge**: every setup writes all its registers. No interrupt or hot-plug line: hot-plug is bit 7 of main 0xC1, valid once the bridge's digital clocks are on. 2 DSI data lanes; the DSI PHY is powered by on-chip LDO 3 at 2.5 V. The monitor's DDC (EDID at 0x50) is on the same bus.
- **The bridge takes 24-bit RGB** over DSI and sends HDMI (or DVI). No audio on this board.
- **The DSI link:** the P4's PHY does 80-1500 Mbps per lane, 2 lanes: up to a 125 MHz pixel clock in 24-bit. Standard 1080p60 (148.5 MHz) doesn't fit; 1080p50 with reduced blanking (about 115 MHz) would; 720p60 runs at an 80 MHz pixel clock with 960 Mbps lanes (Espressif's timing: htotal 1800, vtotal 741, 59.98 Hz; the DPI clock divides a PLL, so CEA's 74.25 MHz isn't available). Before chip revision 3.0 the DSI bridge can't convert formats, and the LT8912B never locks onto 16-bit DSI input (§8), so the link carries RGB888.
- **The DSI bridge pulls pixels with the DW-GDMA** (IDF's private `esp_private/dw_gdma.h`): IDF's DPI panel gives it one block, the whole framebuffer in PSRAM, re-armed each frame. The DMA also does **circular linked lists with an interrupt per block**, which is what a ring of line buffers needs. IDF has no line-buffer ("bounce buffer") mode for DSI (its RGB-LCD driver has one), so the scanout is ours, on the DSI HAL and the DW-GDMA, with IDF's DPI panel as the reference for the register setup.
- **The cost moves from memory to rendering.** 720p60 sends 80 Mpixel/s: 160 MB/s of 16-bit pixels, or 240 MB/s of 24-bit, all written by the renderer into internal RAM. Text rows rendered from a glyph bitmap with two colors per cell: about 160 cells per line at 8 pixels each, 720 lines, 60 frames: **6.9 million cell-lines a second**, so a cell-line must cost a few tens of cycles at most. 16-bit output halves the bytes written. The P4's SIMD (`xespv2p1`) and its PPA (which has a palette lookup, L8 with a CLUT, that IDF doesn't drive yet) are for later depths and graphics.
- **The bridge setup** is BMDS's own, from the register meanings in Linux's (GPL) and Espressif's (Apache-2.0, known to work with the P4) drivers; no code is copied. Espressif's analog values are used.

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| V1 | Bridge driver | **Our own `Lt8912b`** over our `I2cBus`. Done. | Espressif's component |
| V2 | Scanout | **Our own, free-running**: the DSI HAL for the link and DPI timing, the DW-GDMA streaming a circular ring of line buffers in internal DMA RAM that never stops (§8), a refill task woken per block | IDF's DPI panel scanning PSRAM directly (no slack: one late read is an underrun; at 80 MHz it couldn't keep up); the ring stopping at a buffer not refilled (the first design; every late refill cost a resync) |
| V3 | I²C | **`I2cBus` over IDF's `i2c_master`**, numbered. Done. | our own on the LL layer |
| V4 | Formats | **RGB565 framebuffer, RGB888 ring and link**: the LT8912B never locks onto 16-bit DSI input (§8), so the 2D-DMA's color converter widens the pixels between framebuffer and ring | RGB888 framebuffer (166 MB/s of PSRAM reads at 720p60, against 110); 8-bit palette (the PPA's lookup was too slow at 720p60) |
| V5 | First mode | **1280×720 at 60 Hz**; other modes later | the EDID's preferred |
| V6 | Hot-plug | **Read 0xC1 every 2 s** in the display service; act on changes (EDID, output on or off) | interrupt (none on this board) |
| V7 | Where | **`bmds_drivers`**: `I2cBus`, `Lt8912b`, `Edid`, `Scanout`, `Framebuffer`; the text renderer with the console | one component per driver |
| V8 | PSRAM clock | **200 MHz** for the framebuffer (its 2D-DMA reads keep up under load; at 80 MHz half the blocks were skipped under load, §8), once verified (`psram.md`) | 80 MHz with the line-rendered scanout |
| V9 | Drawing | **In internal RAM, copied into the back buffer by 2D-DMA rectangle copies**: the CPU never writes the framebuffer, so no PSRAM cache traffic or write-backs (the CPU's PSRAM traffic can't be ranked below the 2D-DMA's: the caches bypass the interconnect, §8) | the CPU drawing into PSRAM (its write-backs held the refill up to 5 ms a frame in the prototype) |
| V10 | Buffering | **Double, swapped at a frame's start** by the refill task (no vsync interrupt on this chip revision); drawing waits while a swap is pending | single buffer (tearing); triple (more PSRAM, more copying) |
| V11 | Ring | **3 buffers of 24 lines** (276 KB of internal RAM at 720p); free-running | 3×16 (184 KB; three times the skips under flat-out CPU PSRAM writes); more buffers (no better: the skips follow the 2D-DMA's pace, not jitter) |

## 4. Step 1: bring-up (done)

- **1a, the bridge on I²C:** `I2cBus`, `Lt8912b` (chip id, hot-plug), `Edid`, the `display` command (status, `edid`, `scan`).
- **1b, a picture:** `display test 720` sets the bridge up and starts IDF's DPI panel showing the DSI controller's own bar pattern: bars on the monitor. Kept until step 2's scanout replaces it.

## 5. Step 2: the line-rendered scanout (done; superseded as the picture source by §5b)

- **`Scanout`** (`bmds_drivers`): powers the PHY (LDO 3), brings up the DSI host and PHY at the mode's lane rate, programs the DPI timing and the DSI bridge (input format, the frame's size, flow control by the DMA), and starts the DW-GDMA on a circular list of `B` buffers of `L` lines each in internal DMA RAM. Each block's completion wakes the **refill task** (pinned to core 1, high priority), which asks the `LineSource` for the next `L` lines into the buffer just sent. A frame counter and the bridge's underrun count tell whether the renderer kept up.
- **`LineSource`:** `void render(uint16_t firstLine, uint16_t lines, uint8_t *dst, size_t stride)`; the mode's format and width are known to it.
- **A test source** first: bars and a gradient computed per line, then a moving element (so tearing or a lost block shows).
- **Measure:** cycles per block for RGB565 and RGB888 at 720p; the refill task's share of core 1; underruns while the other core hammers PSRAM and internal RAM; internal RAM for `B`×`L` (720p, 8 lines: 20 KB per buffer in RGB565, 30 KB in RGB888). Then V4.
- **Then:** `display test` uses it; IDF's DPI panel and its framebuffer go.

## 5b. Step 2b: the framebuffer

```
 CPU draws a strip   2D-DMA copy      PSRAM, 200 MHz         2D-DMA widen      internal ring       DW-GDMA
 in internal RAM ──► (rectangle) ──► back  │ front (565) ──► 565 → 888, ────► 3 × 24 lines ────► (free-running) ──► DSI ──► LT8912B ──► HDMI
 (565)                                     │ swap at a        per 24 lines      RGB888, 276 KB
                                           │ frame's start    (the refill)
```

- **`Framebuffer`** (`bmds_drivers`): two RGB565 buffers in PSRAM (`MemSpace::PsramDma`, 128-byte aligned; 1.84 MB each at 1280×720), and the 2D-DMA jobs that draw into them. One client at a time (the display service, then the console). Sketch:
  - `Error blit(const Rect &to, const uint8_t *src, size_t srcStride, unsigned int timeoutMs)`: copies a 565 rectangle from internal RAM into the back buffer (a 2D descriptor pair: a block of the source picture into a block of the framebuffer). It writes the source back from the cache first, in bounded pieces (a cache operation runs with interrupts off). Blocking; waits while a swap is pending.
  - `Error copyFromFront(const Rect &r, unsigned int timeoutMs)`: brings a rectangle of the back buffer up to the front's (after a swap the back holds the frame from two presents ago). The caller copies only what changed since: the console knows its dirty cells. A full-frame copy moves 3.7 MB of PSRAM traffic (about 15 ms at the measured DMA rate), so it's the exception.
  - `Error present(unsigned int timeoutMs)` / `Error tryPresent()`: asks for the swap (the user's "take"). The refill task swaps when it starts the first block of the next frame and releases the waiter; `present()` waits for that, `tryPresent()` returns at once (`WouldBlock` if a swap is already pending). Until the swap, `blit()` and `copyFromFront()` wait: the back buffer is about to be shown.
  - `size()`, `format()`; later a `Format` beyond 565.
- **`Scanout`** takes the front buffer (`Config::frameSource`, the prototype's) and a swap request, checked at each frame's first block. The `LineSource` path stays as the bring-up and 80 MHz fallback until the framebuffer is the default, then goes with the other bring-up knobs.
- **The 2D-DMA:** 3 TX and 2 RX channels on this revision (TX 0-2 convert colors), so a widen job (1 TX with the converter, 1 RX) and a copy job (1+1) can run at once. Within the 2D-DMA, channel priorities and arbiter weights (`dma2d_ll_rx_set_priority`, token counts) can favor the widen job over copies; against other DMA masters its interconnect QoS is already 15, and other masters can be rate-limited (§8). Against the CPU nothing ranks it (§8): hence V9.
- **Cache rules:** the CPU never writes the framebuffer; it reads it only after invalidating (screenshots, tests). Its strips in internal RAM are written back before each copy.
- **Budgets (720p60):** PSRAM 3.7 MB for the two buffers; reads 110 MB/s for the scan plus the copies. Internal RAM 276 KB for the ring (494 KB was free with the 92 KB 3×8 ring, so about 310 KB remains), 4 KB of descriptors, plus the drawing strips (a 16-line text row is 40 KB). At 1080p30: 4.15 MB a buffer, 124 MB/s, and 5.76 KB a ring line (3×16 lines: 276 KB).
- **When it falls behind:** a skipped block goes out with the lines of three blocks earlier (72 rows up) for one frame, and is counted; the link never loses sync. Measured with the prototype (3×24, 200 MHz): none idle, 3.7% of blocks while the CPU writes PSRAM flat out, about 0.1% under `memtest`.
- **Steps:** (1) `Framebuffer` with the swap in the scanout, the test pattern drawn once; (2) `blit` and `copyFromFront`, a moving test drawn the V9 way, measured idle and under the stresses (the skips, the copy rates, the 2D-DMA priorities); (3) the default, together with 200 MHz PSRAM once `psram.md`'s checks pass; the line-rendered paths and bring-up commands removed.

## 5c. Step 3: surface 0 (decided 2026-10-05; `windows.md` §0)

- **The scan:** the DW-GDMA reads the shown buffer of surface 0 (RGB888 in PSRAM) straight into the
  DSI bridge: a circular list of one descriptor a block, the frame in up to 16 blocks (the most up to
  16 that divide the height: 45 lines at 720p), each block's interrupt making the finished
  descriptors valid again (the DMA's write-back clears them). No ring, no refill: 276 KB of internal
  RAM back. 166 MB/s of PSRAM reads at 720p60. Soaked 35 minutes under load: 0 underruns
  (`windows.md` §4b).
- **Swaps at the vertical** (a double-buffered surface): when a swap is asked for and the DMA is on
  the frame's last block, the interrupt points the other descriptors at the new buffer; at the next
  frame's start (the DMA on the first block) the last one too, and the swap is done. The new picture
  starts at a frame's first line; a late interrupt only delays it a frame.
- **`Surface`** (`bmds_drivers`): RGB888, single (the default) or double buffered. Drawing is the
  same either way and goes to the right buffer: with two, the one not shown, waiting while a swap is
  pending (for now; a mailbox later). `present()` asks for the swap without waiting and returns its
  number; `waitShown(number, timeoutMs)` waits for it; `frames()` counts frames. After a swap, the
  changes drawn since the last present are copied into the other buffer (2D-DMA) before the next
  drawing. Without a scan, a present swaps at once.
- **Drawing with the 2D-DMA** (`BlitChain`): copies from internal RAM (written back first, in 64 KB
  pieces), between buffers, and fills: one line of the color in internal RAM, sent again for every
  row by the transmit chain into one block, so a fill needs no cache work. RGB888 rows into PSRAM
  start and end on 4 bytes: x and width in multiples of 4 pixels.
- **Removed:** the ring and its paths (CPU lines, the PPA palette, the RGB565 framebuffer widened by
  the 2D-DMA, the scan-time compositor), `Framebuffer`, `RectCopier`, `ColorExpander`,
  `PaletteExpander`, and the bring-up commands (`display test` modes, `verify`, `qos`, `check`,
  `resync`, `win proto`). `display` keeps status, `edid`, `scan`, `stats` and `stress`, and gains
  `start`/`off` and a test picture and animation on surface 0.

**As built** (2026-10-05, the 200 MHz image):

- `Surface` (single or double), `Scanout` on it (16 blocks of 45 lines at 720p, a block interrupt
  of 658 cycles at most, 2,244 with a swap's re-pointing), `Display` owning surface 0, `BlitChain`
  with fills (one line, a transmit descriptor a row); the `DisplayBoot` stage
  (`CONFIG_BMDS_DISPLAY_AT_BOOT`, on by default with the 200 MHz PSRAM): 720p and the test picture
  137 ms into the boot. Internal RAM free after boot: 559 KB (482 KB with the ring). The image 34 KB
  smaller. (The stage and its option were removed on 2026-10-05: the display starts off, and
  `/sd0/p0/autoexec.sh` starts it, `windows.md` §8 step 6.)
- **Drawing:** a 64×64 fill about 115 µs (mostly the 2D-DMA job's fixed cost), the test picture
  66 ms (blits of 16-line strips). A full-screen fill wrote PSRAM at about 470 MB/s and **underran the
  scan** once each time, with the 2D-DMA below the scan on the interconnect (QoS 4 against 15): its
  writes are now regulated (the interconnect's token bucket, a burst per 2^6 cycles: about
  340 MB/s, a full fill 8.1 ms), 0 underruns since. Copies (PSRAM to PSRAM) run at about 113 MB/s
  each way and never underran it.
- **Double buffering:** presents are frame-locked (`display present 30`: 17.4 ms each); the changes
  are kept as a bitmap of 32×16 tiles and copied forward as rectangles (a `Region` of exact
  rectangles cost about 280 µs of CPU a fill with many small changes: 41 fps for 16 moving
  squares, against 60 with the tiles). 16 and 32 squares moving: 59.7 and 59.3 fps, every pixel
  right (`display anim stop` checks the whole picture). Under CPU PSRAM stress and `memtest`: 0
  underruns, the animation at 42-44 fps.
- **A heap bug hit the DMA buffers:** the heap (TLSF) writes the next block's link into the last
  word of an allocation, through the cache: the surface's last pixels showed a pointer to its own
  heap block (found by `check` under CPU stress). `Memory` now gives every DMA allocation whole
  cache lines plus one, so no heap write shares a line with a DMA buffer.
- **Modes:** 1920×1080 at 30.7 Hz and 960×540 at 60 Hz (double-buffered, 32 squares at 59.3 fps)
  run too, 0 underruns.

## 5c'. The boot-time underrun (2026-10-05)

The user saw the picture come up shifted (the terminal's bottom wrapped to the top) once the
desktop started from `autoexec.sh`. Found:

- **One underrun on every boot, never later.** `display start 720 double`, `win start` and
  `win term` back to back: the window manager's first round (a full-screen fill) and the new
  shell's start (a 64 KB PSRAM stack, filled by FreeRTOS) at the same moment. With a second
  between the two commands, or the desktop restarted later, nothing. The 2D-DMA filled the
  screen in 8 ms (about 340 MB/s of PSRAM writes) on top of the scan's 166 MB/s.
- **An underrun was counted but not cured** since the framebuffer path: the bridge drops what
  it missed, so everything after lands lower until the stream restarts at a frame's top. The
  ring path's rule is back: an underrun asks the supervisor for a resync (at most one every
  50 ms; what the restart itself raises is ignored), and a resync that can't stop the DMA
  rebuilds the path (a brief link drop).
- **The 2D-DMA's write regulator one step tighter** (level 5 → 6): a full-screen fill 16 ms
  instead of 8, `seq 1 3000` in a terminal unchanged (0.52 s), 0 underruns on 7 boots with the
  three commands back to back.
- Ruled out on the way: the DSI frame acknowledgement, the LT8912B receiver reset (it only
  moved the moment), the time since the display started, and the regulator taking hold late.

## 5d. Screenshots (2026-10-05; `input.md` §7 step 4)

`screenshot PATH` saves what is on the screen as a PNG (truecolor, 8 bits a channel). A
command of its own in `bmds_ui` (it needs the window manager, which `bmds_drivers` can't see).

- **What is read:** the buffer being shown (`Surface::shown()`), copied whole into PSRAM while
  the window manager draws nothing (`WindowManager::capture()`: its lock held, any swap shown
  first), then encoded from the copy with drawing going on. The copy is the screen's size
  (2.7 MB at 720p) for the shot's length; holding the lock only for the copy keeps the desktop
  live while the slow part runs.
- **Deflate from the ROM:** the P4's ROM has miniz's compressor (`tdefl_init`,
  `tdefl_compress`, in both ROM linker scripts we use; the header is ESP-IDF's
  `esp_rom/include/miniz.h`, built with `TDEFL_LESS_MEMORY`). No compressor in flash. Its state,
  `tdefl_compressor`, is 167,744 bytes: in PSRAM, for the shot only.
- **Rows filtered** before compression: each row is tried with the PNG filters None, Sub and Up
  and the one with the smallest sum of absolute values kept (the standard heuristic); a desktop
  of flat colors and text then compresses to a small fraction. A row is read from PSRAM into an
  internal-RAM buffer (the previous row kept for Up), filtered there, and fed to the compressor.
- **CRC-32 from the ROM** (`esp_rom_crc32_le`), Adler-32 from the compressor
  (`TDEFL_WRITE_ZLIB_HEADER`). Output goes through a buffer in internal RAM to the file, one IDAT
  chunk per buffer.
- **As built** (2026-10-05): `screenshot [-0..-9] PATH` (`bmds_ui`, `Png::write()`), the
  compressor's state in internal RAM when it fits (it did), PSRAM otherwise. A 1280×720 desktop
  with a full terminal and a test window:

  | Level | Bytes | Total | Copy | Read | Filter | Deflate |
  |---|---|---|---|---|---|---|
  | 1 | 33,017 | 687 ms | 81 | (in filter) | 251 | 350 |
  | 2 (default) | 32,204 | 679 ms | 81 | 61 | 193 | 340 |
  | 6 | 24,892 | 756 ms | 81 | 61 | 193 | 417 |
  | 9 | 25,077 | 1,202 ms | 81 | | 251 | 867 |
  | 0 (stored) | 2,769,504 | 2,223 ms (a desktop-only screen) | 81 | | 446 | 1,692 |

  Levels 1, 9 and 0 were measured with the first filter (three filterings written, then
  scored); the score-first filter (2 and 6) took 251 → 193 ms plus 61 of reading.

  The file decodes with zlib and every chunk's CRC checks (verified on the host after fetching
  it with YMODEM). On that picture zlib's RLE-only matching beats one probe, but the ROM's
  miniz with `TDEFL_RLE_MATCHES` came out larger (38,259 bytes) and no faster: not used. The
  filter's 193 ms is plain arithmetic (about 25 cycles a byte at `-O2`); a PIE scoring pass
  would cut most of it if screenshots ever need to be quick. The ROM's deflate is the floor.

## 6. Later steps (outline)

Superseded for text by `windows.md` (2026-10-05): a desktop with windows, the terminal a program in one.

- **3, the text renderer:** `TerminalScreen` (host-tested, `terminal.md` §3) holding the cells; changed rows drawn as glyphs with each cell's colors into a strip in internal RAM and blitted into the back buffer, then presented (§5b); an 8×16 bitmap font with a licence that fits (permissive or public domain), in internal RAM.
- **4, the HDMI console:** a `Console` whose output is the screen; a shell instance on it (output only until Phase 5); the boot log on it. `Display` as a service: hot-plug, output off without a monitor.
- **Beyond Phase 4's text console:** modes (720p and up, the EDID's preferred where the link allows), depths for programs (8-bit palette, 16, 24), line-based graphics.

## 7. Open questions

- Answered: the DW-GDMA does run an endless circular list against the DSI bridge (§8), as long as each descriptor is made valid again after its block.
- The 2D-DMA per job: about 13,400 cycles of fixed cost through IDF's job queue (§8). Driving its channels directly (configured once, only descriptors rewritten) would cut most of it; worth it only if the skips stay a problem.
- Whether copies and the widen job, running at once on the 2D-DMA, need its channel priorities set, and what blit sizes keep the scan's share.
- A larger L2 cache (256 or 512 KB, from internal RAM) to cut the CPU's PSRAM misses, against the RAM the ring wants.
- DVI versus HDMI mode: from the EDID (HDMI for this monitor).
- The bridge's analog values: Espressif's, as measured working.

## 8. As built

**1a** (2026-10-04): `bmds_drivers` with `I2cBus` (IDF's `i2c_master`; the board's buses started by a boot stage, `i2c0` and `i2c1`; up to 8 devices per bus made on first use and kept), `Lt8912b` (the three pages, chip id, hot-plug bit), `Edid` (portable, host-tested: the base block, the preferred detailed timing, the name, HDMI from the CEA extension's vendor block) and `display` (status; `display edid` dumps the blocks; `display scan` lists the bus).
- **On the board:** `display scan` shows 0x18 (the codec), 0x48, 0x49, 0x4A and **0x4B** (a fourth address no driver uses); the chip id reads 0x12B2; with no monitor connected the hot-plug bit is clear and nothing answers at 0x50.
- **Measured:** firmware +23.5 KB (445,104 → 468,624 bytes), of which IDF's I²C master driver is 12.6 KB (742 bytes of it in IRAM) and `bmds_drivers` 3.9 KB; 440 bytes of internal RAM (the buses' state). IDF's driver is heavy for register reads: a driver on the LL layer is the alternative (V3) if flash or IRAM gets tight.

**1b** (2026-10-04): `Lt8912b::configure()` (power-up, DSI receiver with the lane count, pixel clock recovery, the video timing, the AVI infoframe, HDMI mode, the output path, output on), `resetReceiver()`, `setOutput()`; `display test 720|1080|off` with IDF's DPI panel and the controller's bar pattern; `display check` (I²C reliability, register read-back, the bridge's measured input). Found on the way:
- **The main I²C bus must run at 100 kHz:** at 400 kHz, 75-93 of 200 reads of the chip id were wrong and writes were corrupted (no acknowledge errors at all); at 100 kHz none in 200. The board definition says 100 kHz now.
- **The bridge keeps its registers across an ESP reset** (RC reset at power-up only): corrupted values from a bad setup stayed until rewritten.
- **The hot-plug bit reads 0 until the digital clocks are on** (main 0x08-0x0C; not 0x02, a reset, which also upset the next reads); `attach()` turns them on.
- **IDF's DSI PHY default clock source is the crystal, which only chip revision 3.0 has:** on v1.3 it aborts. Passing 0 lets IDF choose by revision.
- **On the board:** with an ASUS VE248 (EDID: 1920×1080 at 60 Hz preferred, HDMI), `display test 720` shows vertical color bars; the bridge measures 741 lines per frame from the DSI input (main 0x9E/0x9F).

**2, in progress** (2026-10-04): `Scanout` (the DSI link and DPI set-up on the HAL, a DW-GDMA circular list of line buffers in internal DMA RAM, a refill task on core 1, statistics), `display test 720 [565|888] [lines] [blocks]`, `display stats`, `display stress` (bring-up).
- **It works:** 1280×720 at 59.8 fps from 3 blocks of 8 lines of RGB888 (92 KB); the user saw the bars, the ramp and the moving stripe. IDF's own bridge settings (one DMA block per "frame", the frame interval on) work with a ring: the bridge doesn't count blocks against the frame.
- **Before chip revision 3.0 the DSI bridge can't convert formats:** its input and output format share one register field (IDF's LL writes `raw_type` for both), so RGB565 buffers with an RGB888 link were read as RGB888 (1.5 times the data, a garbled picture). The buffers must be in the link's format. **The LT8912B doesn't lock onto RGB565 over DSI** (it measured 19 lines a frame; the monitor slept), so on this board the lines are RGB888.
- **`dma_flow_multiblk_num` is 4 bits:** 90 became 10. Irrelevant once the format was right, but it cost a detour.
- **Cost, idle:** about 4,000 cycles a line to copy 3,840 bytes into the ring (the budget at 720p60 is about 8,300): 48% of core 1 for a memcpy-only test picture. Writing back from the cache: about 3,500 cycles a block of 8 lines. Writing through the uncached alias instead: 4 times slower.
- **The L1 data cache (64 KB) is shared by both cores** (IDF's cache API has one L1 D-cache, no core index), so the other core's memory traffic evicts the renderer's lines and its misses queue behind the other core's. Measured cycles a line with core 0 copying: internal RAM 6,400-7,000; **PSRAM 14,000-22,000** (3.5-5.5 times idle), whatever the ring size, even one that fits in L1. Program launches and copies into the RAM disk do that much PSRAM traffic: about 5 underruns per launch, constant underruns under a PSRAM hammer. Giving the DW-GDMA the highest read priority on the AXI interconnect (all priorities default to 0) changed nothing: the renderer, not the DMA, is late.
- **An underrun is ugly and isn't recovered:** the bridge sends blue for the rest of the frame, the ring goes on where it was, and from then on the picture is offset (the ramp moved). Revision 1.x has no vsync interrupt (the bridge's is v3.0 only) and neither the host nor the bridge exposes a line counter, so re-aligning to the top of a frame has no direct signal. **The goal is therefore no underruns at all**, which means taking the CPU's writes out of the shared cache's path.
- **LVGL** (v9, `src/draw/espressif/ppa`) offloads only opaque solid fills and unscaled RGB565/888 copies to the PPA, into its draw buffers; text is drawn by the CPU. Not a solution, but a reference for driving IDF's PPA.
- **The PPA has a CLUT** (L4/L8 in the blender's planes, `blend0_clut_data`, present on revision 1.x) that IDF doesn't drive yet. The candidate design: the CPU renders 8-bit indices (a third of the bytes), the PPA expands each block to RGB888 through a palette straight into the ring with its own DMA (2D-DMA, not through the cache). It is also the 8-bit palette mode for programs later. TCM can't serve as the CPU's scratch (not DMA-capable in IDF).
- **The PPA palette experiment** (`PaletteExpander`: the PPA blender with its background plane in L8 through the CLUT, driven on the 2D-DMA as IDF's blend is; `Scanout::Format::Indexed8`, `display test 720 pal`): it works first time (no errors). The CPU's share is small: about 10,000 cycles for 8 lines of indices (a quarter of the RGB888 copy). **The PPA is the limit:** about 7,700 cycles (21 µs) a line plus about 18,000 a job (8 lines: 79,000; 16 lines: 141,000), the same in the blender's bypass mode: about 52 Mpixel/s with 16-line jobs, 60 at best. 720p60 needs 55 Mpixel/s of visible pixels (a line every 23.1 µs on average, 8,300 cycles), so it falls behind. The other core's PSRAM traffic slows it only 16% (it doesn't go through the CPU's cache), so the hardware path does solve the contention; it is too slow for this mode. The PPA and the 2D-DMA run on the system clock, not the CPU's.
- **What fits:** 720p50 gives 27.8 µs a line (10,000 cycles): the PPA at 16-line jobs (8,800) fits with about 12% to spare, with the CPU rendering the next block's indices while it runs. 1024×768 at 60 Hz: shorter lines, about the same margin.
- **The guide's speed advice** (IDF's "Speed Optimization" for the P4): the refill path, its interrupt callbacks and the test renderer moved to IRAM (`BMDS_HOT_ATTR`, `BMDS_ISR_ATTR` in `bmds/placement.h`, the latter the attribute `CODING_STANDARDS.md` names; the DMA descriptor marking inlined from the LL instead of IDF's call in flash). Cycles a line, before and after: idle 4,000 / 3,900; core 0 copying internal RAM 6,400-7,000 / 5,600-5,900; **core 0 copying PSRAM 14,000-22,000 / 14,000-20,000**: instruction fetches weren't the problem, the data path is. Kept: interrupt handlers belong in IRAM anyway. Flash runs at 80 MHz in DIO (the W25Q128JV does quad; QIO needs the board's IO2/IO3 wiring confirmed); PSRAM at 80 MHz.
- **For use** (2026-10-05, the user's request): `CONFIG_BMDS_DISPLAY_TEST_AT_BOOT` (on for now) starts the 720p60 test picture at boot (`DisplayTest::init`, the boot stage "display"): RGB888 on the CPU path, 3 blocks of 8 lines, 92 KB of internal RAM, 48% of core 1. Known: core 0's PSRAM traffic causes underruns (the first ones already during boot), and an underrun leaves the picture offset until the next `display test 720`.
- **Recovery** (2026-10-05, after the user saw the picture stay broken: blue below the first lines, the stripe not moving). On the board: every block stalling, about 60 underruns a second, the renderer idle, the DMA parked at the last word of a block, the bridge's FIFO full and not draining. Its DPI side was wedged, so nothing more ever arrived to act on. What was learned building the cure:
  - The bridge has no reset before revision 3.0 (IDF's LL: "not supported"), and turning its enable bit off while it runs **resets the chip**.
  - A DW-GDMA channel's registers can't be written while it runs (**a store access fault**, the panic seen); a disable waits for the burst under way, which a wedged bridge never takes, and an abort leaves the bridge's DPI side not draining.
  - Turning the DPI output (and the host's video mode) off and on restarts a healthy stream aligned, but doesn't revive a wedged one.
  - **The ladder now:** an underrun or a stall asks for a soft resync (the DPI output off, the DMA disabled and confirmed stopped, the ring refilled from line 0, the DPI output on); if the DMA won't stop, or the progress watchdog sees no block for 50 ms, **a rebuild**: the whole DSI path torn down and set up again in the refill task (the ring, the task and the palette stay), then the bridge's receiver reset (`Config::onRestart`). At most one resync every 50 ms.
  - **Measured** (one serial session; opening the port resets the board, which misled a few readings): 20 BusyBox launches, a copy into the RAM disk and both PSRAM hammers: no reset, the stream back at full rate after each (5,453 blocks a second over a quiet 5 s), about one rebuild per launch, most underruns cured by a soft resync. A rebuild drops the DSI link briefly: how the monitor takes that is for the user to see.
- **Modes and paths for testing** (2026-10-05, the user's request): `display test WIDTHxHEIGHT@HZ [cpu|pal] [lines] [blocks]`. `VideoModes` (portable, host-tested): CEA-861 and VESA DMT timings where a standard exists (1280×720 at 60 and 50, 1024×768, 800×600, 640×480, 1280×1024, 1920×1080 at 30, 25, 24), else CVT reduced blanking (v1; VESA's 1080p60 totals, 2080×1111). **The pixel clock must be a fixed PLL divided** (240 or 160 MHz), so `VideoModes::fitToClock` takes the slowest such clock at least as fast and widens the front porch to keep the rate (CEA 720p60: 80 MHz, 1778×750, 59.99 Hz; 1080p50: 120 MHz); a mode beyond the link's 125 MHz is refused. The host's line timing is now set with the exact lane-to-pixel ratio (IDF's helper rounds the pixel clock to whole MHz).
  - **The APLL** (5.3-125 MHz, any frequency; IDF lists it as a DPI clock source) ran at exactly the asked 74.25 MHz, but the bridge never took a pixel with it selected, and forced on a mode that works from the 240 MHz PLL it made the link fail to start. Not pursued.
  - **Resetting the DSI peripheral** (its only reset, `rst_en_dsi_brg`) resets the host too (19 of 19 tries): no way to restart the bridge alone with the link kept up.
  - **Rebuilds now set the LT8912B up in full** (`onLinkRestart`), not just its receiver: the user saw one rebuild that never recovered until `display test off` and `display test 720`.
  - **On the board, idle:** 1280×720@60, 1024×768@60, 1920×1080@30 and 1920×1080@50 stream without a problem on the CPU path (what the monitor accepts is the user's to report); the PPA path at 1280×720@50 falls behind with 8-line blocks, as measured before (16-line blocks should fit). Soak: 20 launches, three copies into the RAM disk, both PSRAM hammers: no reset, always back.
- **A PSRAM framebuffer** (2026-10-05, the user's suggestion after reading on the 2D-DMA and PPA): `Scanout::Config::framebuffer` (the ring is a whole RGB888 frame in PSRAM in up to 16 blocks, drawn once; the source's `frame()` changes what moves and writes back only that; a write-back of the whole frame each frame would hold up the DMA), `display test MODE fb`. Measured with 80 MHz PSRAM (core 0's PSRAM copy and fill in MB per 2 s; 10 BusyBox launches):

  | Scanout | The scan reads | Idle | Copy / fill | Launches |
  |---|---|---|---|---|
  | none | — | — | 66 / 86 | 2.31 s |
  | CPU path, 720p60 | internal RAM | clean | 65 / 91 | 2.31 s (32 underruns, 10 rebuilds) |
  | framebuffer, 1280×720@60 | 166 MB/s | 124 underruns in 2 s | 62 / 81 | 2.76 s |
  | framebuffer, 1280×720@50 | 138 MB/s | 71 underruns in 2 s | 36 / 63 | 2.86 s |
  | framebuffer, 1024×768@60 | 141 MB/s | 118 underruns in 2 s | 52 / 78 | 2.91 s |
  | framebuffer, 800×600@60 | 86 MB/s | clean, and under load | 33 / 46 | 4.12 s |

  **At 80 MHz the PSRAM can't stream much more than about 90 MB/s to the DSI DMA**, and while it does, everything else that uses PSRAM runs at half speed or worse (launches 78% slower at 800×600). The framebuffer only becomes an option with more PSRAM bandwidth: 200 MHz (2.5 times) is the experiment that decides it, and it bears on the CPU path too (shorter PSRAM waits for the other core).
- **16-bit links, revisited** (2026-10-05, the user's challenge: does revision 1.x really not take RGB565?): `display test MODE 565 [lines] [blocks] [pack=N] [lane24]` sweeps the host's three 16-bit packings (with the bridge's matching arrangement) at the 16-bit lane rate and at the 24-bit one. With RGB888 the LT8912B measures 741 lines a frame; with RGB565 it measures 19 in every one of the six combinations: it never locks. The host sends 16-bit packets as asked; the bridge chip is what doesn't take them. Its datasheet's 16-bit input set-up is the remaining lead.
- **RGB565 widened by the 2D-DMA** (2026-10-05): `ColorExpander`, a memory-to-memory 2D-DMA job whose transmit channel converts RGB565 to RGB888 (`DMA2D_CSC_TX_RGB565_TO_RGB888`, linear descriptors as IDF's test of it uses); `Scanout::Format::Rgb565Expanded`, the source drawing RGB565 into a one-block staging buffer (internal DMA RAM) that the 2D-DMA widens into the ring; `display test MODE dma`. Measured at 720p60, 3 blocks of 8 lines, one serial session:

  | Path | Per block (8 lines), idle | Under PSRAM copy (core 0) | Under internal copy (core 0) |
  |---|---|---|---|
  | CPU (RGB888 copy + write-back) | 31,900 cycles | 122 underruns in 2 s | clean |
  | 2D-DMA (RGB565 draw ~18,800 + widen ~33,900) | 52,700 cycles | 121 underruns in 2 s | 92 underruns in 2 s |
  | PPA palette (indices + blend) | over the budget: does not stream at 720p60 | — | — |

  - **The 2D-DMA widens about 4,200 cycles' worth a line**, about twice the PPA's rate, and needs no CPU for it; it is the first hardware path that keeps up with 720p60.
  - **It is serial for now** (draw, then wait for the widening), so it uses 79% of the block time and has less slack than the CPU path, which is why an internal-RAM copy on the other core upsets it. Drawing the next block while the 2D-DMA widens this one (two staging buffers) would cut that to the larger of the two, about 34,000.
  - **Neither path survives PSRAM traffic, and a deeper ring doesn't help:** with 6 and with 10 blocks (1.8 ms of slack) PSRAM copies still give 55-70 underruns in 2 s, and PSRAM *writes* alone (89 MB/s) do as much harm as copies. Since the CPU path survives a 534 MB/s internal copy but not a 90 MB/s PSRAM write, the PSRAM traffic itself (its cache misses and write-backs) holds up the scan, not the render's speed. The conversion path doesn't decide stability; PSRAM bandwidth (the 200 MHz experiment) or keeping the scan's path clear of it does.
- **AXI arbitration** (2026-10-05, the user's question: have we looked into the interconnect's QoS?). Until now only the QoS values of the scanout's DW-GDMA and the 2D-DMA had been set. The interconnect (`ICM_SYS`, `ICM_SYS_QOS`) also has per-master arbitration priorities (`mst_arb_priority_reg0`), per-slave response priorities (`slv_arb_priority`: L2MEM, the PSRAM's MSPI…) and per-master token-bucket regulators (`axi_icm_ll_set_qos_burstiness`, `_peak_transaction_rate`; nothing in IDF uses them). `display qos` sets them at run time. Measured at 200 MHz PSRAM, 720p60 CPU path, one serial session:
  - **No setting changed anything:** the display DMA's priority 15, L2MEM's response priority over the PSRAM's, both, the inverse (CPU and cache at 15), and the regulator on the cache port (writes, then reads and writes, at 1/128 to 1/1024 of the cycles), and on the CPU port too: every case gave 122 ± 2 underruns in 2 s of `display stress write` and of `display stress psram`, and the stresses ran at the same 170 and 103 MB/s.
  - **The regulator works:** on the AXI-GDMA it cut `memtest`'s DMA copy from 120 to 10 MB/s, and on the display DMA's ports it stopped the picture (183 underruns in 3 s, 2 frames instead of 183).
  - **So the CPU's PSRAM traffic doesn't cross the interconnect:** capping the cache and CPU ports at 1/1024 left PSRAM writes at 170 MB/s. The caches reach the PSRAM controller by their own path, while the display DMA's reads of internal RAM do cross it. The interconnect has nothing to arbitrate between them, and the contention is where both meet: the internal RAM (L2MEM), which also holds the L2 cache's data, and for which no arbitration control has turned up.
  - **The underrun count under stress is saturated** (one per recovery attempt, about 61 a second), so it shows whether the display survives, not by how much. Next levers to try: the L2 cache's size and line size (`CONFIG_CACHE_L2_CACHE_*`), and where the ring sits in L2MEM relative to the L2 cache.
- **The free-running ring** (2026-10-05, the user's design): `Config::freeRunning` (`display test MODE ... free`). The block interrupt marks the buffer just sent valid again at once, so the DMA never meets an invalid descriptor. The refill draws the blocks ahead of the one going out, up to a ring ahead, and skips ahead when it's behind (`skipped` in the stats). A late block goes out stale, from the last pass round the ring, instead of stopping the stream. Measured at 200 MHz PSRAM, 720p60, frames and problems per load (stress: 2 s; `memtest -m 8`: about 2 s):

  | Ring | Idle | stress internal | stress write | stress psram | memtest -m 8 |
  |---|---|---|---|---|---|
  | CPU, 3×8, normal | 123 fr, clean | 124 fr, clean | 7 fr, 121 underruns | 9 fr, 121 underruns | 115 fr, 14 underruns |
  | CPU, 3×8, free | 123 fr, clean | 124 fr, clean | 123 fr, 5,216 skipped, 0 underruns | 127 fr, 7,175 skipped, 0 underruns | 127 fr, 3 skipped, 21 underruns |
  | 2D-DMA, 3×8, normal | clean | 15 fr, 57 underruns | 7 fr, 122 underruns | 7 fr, 122 underruns | 41 fr, 65 underruns |
  | 2D-DMA, 3×8, free | clean | clean | 123 fr, 5,334 skipped, 0 underruns | 127 fr, 6,740 skipped, 0 underruns | 125 fr, 48 skipped, 31 underruns |
  | CPU, 10×8, free | clean | clean | 126 fr, 5,241 skipped, 0 underruns | 124 fr, 7,121 skipped, 0 underruns | 139 fr, 4 underruns |

  - **The stress failures were the refill being late, not the DMA starving:** with the ring always valid there are no underruns under either stress, on both paths, at the full frame rate. (So the arbitration findings above stand, but weren't the problem.)
  - **The cost:** under PSRAM stress the renderer manages only a third to a half of the blocks (5,200-7,200 of about 10,800 in 2 s skipped): core 1's render slows severalfold while core 0 streams PSRAM, through the L1 data cache the cores share. With a 24-line ring a skipped block shows lines from 24 rows up: a visibly wrong band while the load lasts, but no loss of sync and nothing to recover from afterwards.
  - **What's left (`memtest`): the block interrupt itself running late.** `esp_cache_msync` runs in a critical section (interrupts off) under a spinlock both cores share; `memtest` writes back 64 KB at a time, and the refill's own write-back spins behind it with interrupts off. When the re-validation comes more than a ring late, the DMA stalls as before (10 blocks: 4 instead of 21).
  - **Next:** take the interrupt out of the loop (descriptors the hardware never invalidates, or the DW-GDMA's auto-reload of one contiguous ring with the position read from the channel), and render through an uncached alias of internal RAM (no write-back, so no msync and no L1 traffic) to cut the slowdown.
- **Free-running, hardened** (2026-10-05): the default now (`Config::freeRunning`, the boot test, `display test`; `strict` gives the old ring).
  - **The DMA clears each descriptor's valid bit itself** when the block is done (its write-back; nothing in the registers turns that off), so free-running depends on the block interrupt. The list now goes round the buffers several times (63 descriptors over 3 buffers), so the interrupt can be up to 62 blocks (11 ms at 720p60) late.
  - **That alone wasn't enough, and the reason was the count, not the latency:** IDF's DW-GDMA handler reads and clears the status once, so a late interrupt (behind `esp_cache_msync`'s critical section, for one) stands for several blocks. Counting interrupts lost blocks, the count drifted behind the DMA, and the interrupt re-validated the wrong descriptors (and the refill drew into the wrong buffers). The interrupt now reads the DMA's descriptor register and counts the blocks since the last one from it, re-validating every descriptor finished since (and the one before the last, in case the DMA's write-back lands after the interrupt).
  - **Measured** at 200 MHz PSRAM, 720p60, both paths: 0 stalls, 0 underruns, 0 resyncs under `display stress write`, `display stress psram`, `memtest -m 8` and full `memtest` passes (8 full passes in all), at 123-127 frames in 2 s. The strict ring under the same loads: 7-10 frames and 103-122 underruns under the stresses, 7 underruns per `memtest`.
  - `memtest` now invalidates the DMA copy's destination in 64 KB pieces: one call over 15 MB held interrupts off for many milliseconds.
  - **Left:** the skipped blocks under heavy PSRAM traffic (2,400-6,900 of about 10,800 in 2 s; few or none under `memtest`), which show as wrong lines while the load lasts. Next: render through an uncached alias of internal RAM.
- **Prototype: an RGB565 framebuffer in PSRAM, widened by the 2D-DMA into the free-running ring** (2026-10-05, the user's design: a PSRAM framebuffer, 565, the 2D-DMA expanding it on the way out; drawing to be done in internal RAM and copied into PSRAM by DMA). The 2D-DMA can't feed the DSI bridge (its peripherals are the PPA, JPEG and memory-to-memory), so the ring in internal RAM stays: per block, the refill starts a 2D-DMA job that widens the block's lines from the framebuffer into the ring, and the CPU draws nothing. `Config::frameSource`, `display test MODE fb565 [lines] [blocks] still`; `display verify` checks the ring against the framebuffer (every line one of the picture's two kinds, every pixel's top bits right).
  - **Correct:** 0 wrong lines in every check at 80 and 200 MHz. The 2D-DMA fills the low bits (they read 7/3/7, not 0). A rare single mismatched line at 24×3 is most likely the check reading a buffer the 2D-DMA is rewriting with the other kind of line (unproven: the check doesn't skip that buffer yet).
  - **A bug it found:** a scanout stopped during a 2D-DMA job left that job's completion behind, so after a restart every `expand()` returned at once on the previous job's completion (impossible 9,000-cycle timings) while jobs overlapped on the same descriptors. `ColorExpander` and `PaletteExpander` now clear a stale completion at `start()` and wait for a job under way in `stop()` and before reusing the descriptors.
  - **Measured** at 720p60, still picture (a block's time is about 67,000 CPU cycles at 8 lines; the refill task sleeps while the 2D-DMA works, so the CPU is free), skipped blocks per load:

    | PSRAM, ring | 2D-DMA per block (of the block's time) | Idle | stress write (2 s) | stress psram (2 s) | memtest -m 8 | memtest (one pass) |
    |---|---|---|---|---|---|---|
    | 200 MHz, 3×8 | 41,300 (62%) | 0 | 2,507 | 955 | 43 | 159 |
    | 200 MHz, 3×16 | 69,200 (52%) | 0 | 462 | 0 | 14 | 48 |
    | 200 MHz, 4×16 | 69,300 (52%) | 0 | 457 | 0 | 13 | 47 |
    | 200 MHz, 3×24 | 97,200 (48%) | 0 | 139 | 0 | 6 | 24 |
    | 80 MHz, 3×16 | 98,200 (74%) | 0 | 2,786 | 1,950 | 513 | 1,797 |
    | 80 MHz, 3×24 | 140,700 (70%) | 0 | 1,780 | 1,197 | 271 | 948 |

  - **Longer jobs help, a deeper ring doesn't** (4×16 is 3×16): the skips come from the 2D-DMA's average pace under PSRAM contention, not from jitter. 24-line blocks (3 of them: 276 KB of internal RAM) skip 3.7% of blocks while the CPU writes PSRAM flat out, none under PSRAM copies, about 0.1% under `memtest`.
  - **80 MHz works idle but not under load** (half the blocks skipped while the CPU writes PSRAM): the framebuffer wants 200 MHz.
  - Not measured yet: a moving picture drawn the planned way (internal RAM, then DMA into the framebuffer). The test pattern's own moving stripe writes PSRAM from the CPU in the refill task (up to 5 ms a frame), which skipped blocks even idle; that's the approach the plan avoids.
- **§5b step 1, the framebuffer and its swap** (2026-10-05): `Framebuffer` (`bmds_drivers`): two RGB565 buffers in PSRAM (128-byte aligned, cleared and written back in 64 KB pieces), `front()`/`back()`, `present(timeoutMs)`, `tryPresent()`, `isSwapPending()`, `swaps()`. The swap request is three-state (idle, asked, swapping), and whoever moves it from asked to swapping makes the swap, so a present racing a scan that stops swaps once, and `isSwapPending()` stays true until the front has moved. A counter of swaps tells a waiter its swap from an earlier one. `Scanout::start(config, Framebuffer &)` replaces the prototype's `frameSource`: the refill asks the framebuffer for the frame's buffer at each frame's first block filled (after a skip, the first one filled), which is where a pending swap is made; without a running scan a present swaps at once. `display test MODE fb565` draws the test picture into both buffers with the stripe in different places; `display present [N]` swaps N times, timing each.
  - **Measured** (200 MHz PSRAM, 720p60, 3×24): one present waited 9.8 ms; 120 back to back took 16,601 µs on average and 16,677 at most (a frame is 16,672 µs), so swaps land on frame starts. No skipped blocks through 181 swaps; 3 during a 2 s `display stress write`. The 3.7 MB of PSRAM comes back at `display test off`.
  - **`display verify` proven:** the occasional single wrong line was the line the 2D-DMA was writing (part old, part new: a buffer goes from one kind of line to the other three blocks on). Each flagged line is now looked at again 50 µs later: in 20 checks (24- and 8-line blocks, after 61 swaps) every one was right then, 0 stayed wrong.
- **§5b step 2, drawing** (2026-10-05): `RectCopier` (`bmds_drivers`): 2D-DMA memory-to-memory copies of a block of one picture into a block of another (2D descriptors with offsets, as IDF's 2D window test), 16-bit pixels; into PSRAM rows start and end on 4 bytes (IDF's note), so x and width are even. `Framebuffer::blit(to, src, srcStride, timeoutMs)` copies a strip from internal RAM (written back from the cache first, in pieces) into the back buffer; `copyFromFront(rect, timeoutMs)` brings a rectangle of the back buffer up to the front's. Both wait while a swap is pending. `Rect` is a plain x, y, width, height for now. `display anim start [N] [copy]` (a drawing task on core 0) bounces N 64×64 squares over the bars and ramp: each frame, each square's area from two frames ago to now is drawn into a strip in internal RAM and blitted, then the frame is presented; `copy` first copies the whole front buffer to the back. `display anim stop` halts it and checks every pixel of the front buffer against the picture last presented.
  - **Correct:** 0 of 921,600 pixels wrong in each run (8 squares, 32, 8 with full copies), each after about 600 frames and both stresses.
  - **Measured** (200 MHz PSRAM, 720p60, ring 3×24):

    | Run | Frames/s | Drawing per frame | Blits per frame | Skipped blocks: idle 5 s / stress write / stress psram |
    |---|---|---|---|---|
    | 8 squares | 59.7 | 1.9 ms | 79 KB in 0.63 ms | 0 / 0 / 0 |
    | 32 squares | 59.6 | 9.9 ms (the CPU's drawing; a naive loop) | 320 KB in 2.5 ms | 0 / 0 / 0 |
    | 8 squares + a full copy | 59.4 idle, 47.6 overall | 14.8 ms, of which the copy 12.9 (142 MB/s) | 79 KB | 0 / 837 / 477 |

  - **Blits cost the scan nothing measurable**, even at 320 KB a frame under both stresses: drawing in internal RAM and copying by 2D-DMA works as planned (V9). Blits move about 125 MB/s including each job's fixed cost.
  - **A full-frame copy every frame is what hurts:** 12.9 ms of a 16.7 ms frame idle, up to 22 ms under stress (frames missed), and the only case with skips. As planned, copy only what changed. The 2D-DMA's channel priorities weren't needed so far.
  - `display verify` compares the ring with the plain test picture, so it reports wrong lines once squares are drawn (expected).
- **Bulk DMA against the scan** (2026-10-05, the user saw the 32-square animation upset only during `memtest`'s DMA test; `memtest -d` runs that test alone). The AXI-GDMA copying PSRAM to PSRAM isn't ranked above the 2D-DMA (its QoS is 0, the 2D-DMA's 15); it just takes the PSRAM's bandwidth (66-68 MB/s copied, so about 135 MB/s of reads and writes, alongside the scan's 110 MB/s). Measured with `display anim start 32` running:

  | Ring | AXI-GDMA | Copy rate | Skipped blocks in a DMA pass (30 MB) |
  |---|---|---|---|
  | 3×8 (the old `fb565` default) | uncapped | 66 MB/s | 114 (a whole `memtest`: the same 114) |
  | 3×8 | capped at 1/256 of cycles each way | 30 MB/s | 0 |
  | 3×24 | uncapped | 68 MB/s | 0-1 (two 16 MB passes: 1) |
  | 3×24 | `prio.dma2d 15`; caps of 1/128-1/1024 | 58 to 9 MB/s | 0 |

  - The animation kept 60 fps throughout; what showed was the scan's skipped blocks. With the 3×24 ring (V11) the bulk copy does no harm, so no cap by default. The interconnect's regulator on the AXI-GDMA (`display qos limit pdma`) is the lever if a DMA-heavy driver (SD, USB) ever does: 1/256 halves its copy rate and cleared the 3×8 ring entirely. The 2D-DMA's arbitration priority made no difference.
  - `display test MODE fb565` now defaults to 24-line blocks.
- **`display edid` decodes** (2026-10-05; `display edid raw` keeps the bytes): `Edid` also reads the manufacture date, screen size, serial, range limits, all detailed timings (base block and CEA extension), standard and established timings and the CEA video modes (named for the common VICs, native marked); host-tested. The ASUS VE248 here: 50-76 Hz vertical, **30-83 kHz horizontal**, pixel clock up to 170 MHz; preferred 1920×1080 at 60 Hz (148.5 MHz, beyond the 2-lane DSI link's 125 MHz), also 1366×768, 1280×720 and the usual VESA modes. So smaller 16:9 modes made by `VideoModes` (CVT reduced blanking, fitted to the PLL clocks) are in its ranges at 960×540@60 (40 MHz, 33.4 kHz) and 1024×576@60 (48 MHz, 35.6 kHz); 640×360@60 isn't (22.4 kHz, and 20 MHz is under HDMI's 25 MHz minimum). Whether it shows them is the monitor's call: to try.
- **A crash on a mode change, from IDF's DW-GDMA teardown** (2026-10-05): `display test 1024x576@60 fb565` straight after a 960×540 run panicked with a load access fault in IDF's shared-interrupt handler, reading 0x50081030, the DW-GDMA's interrupt status register. IDF's `channel_destroy()` (`esp_hw_support/dma/dw_gdma.c`) releases the DMA group first, which turns the DW-GDMA's bus clock off with the last channel, and only then frees the channel's shared interrupt; an interrupt on that line in between reads the status register with the clock off. The scanout now masks and clears its channel's interrupts before deleting it, and holds an idle DW-GDMA channel for good (no interrupt, never started; one of the 4 channels) so the group and its clock outlive the scanout's channel. 60 mode changes afterwards (720p, 960×540 and 1024×576 framebuffers, the CPU path, `test off`), no fault; the crash itself came once, so that is evidence, not proof. Worth reporting to Espressif.
- `display test` picks the lines a block when they aren't given: the most up to 24 (the framebuffer) or 8 that divide the mode's height (20 for 960×540), at least 4.
- **960×540 at 60 Hz works on the monitor** (2026-10-05, the user's report; ASUS VE248): CVT reduced blanking fitted to 40 MHz (h 960 127 32 80, v 540 3 5 8, 60.002 Hz), from the RGB565 framebuffer with 20-line blocks. The panel's 1920×1080 scales it by exactly 2; it needs 62 MB/s of PSRAM reads against 720p's 110. (1024×576 not reported on.)
