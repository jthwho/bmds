# Display: HDMI through the LT8912B, Line-Rendered Scanout, Text Console

Status: **step 1 done, step 2 in progress** (Phase 4, 2026-10-04): the bridge is set up over I²C and shows the DSI controller's test pattern at 1280×720 on a monitor. Revised the same day (with the user): no framebuffer; the picture is rendered line by line into internal RAM as it is scanned out.

## 1. Goal

- A picture on an HDMI monitor, from the P4's MIPI-DSI controller through the board's Lontium LT8912B bridge.
- **No framebuffer.** The DSI controller's DMA streams a small ring of line buffers in internal RAM; a renderer fills each part of the ring just before the scan reaches it. Scanout then costs PSRAM nothing, and the resolution isn't bounded by a framebuffer's memory or bandwidth: the native 1920×1080 of a monitor is reachable later as far as the DSI link allows.
- Know the monitor: hot-plug state and its EDID (name, preferred mode).
- A **text console** as the first renderer (`terminal.md` §3): a cell grid with 24-bit colors, glyphs drawn into each line as it goes out; the shell's output on the screen, and with a keyboard (Phase 5) a full local shell.
- **Later** (not this plan's steps): display modes of several depths for programs (8-bit palette, 16-bit, 24-bit), video mode testing beyond 720p, and a fast graphics scheme for a line-based display (tiles, sprites, a palette framebuffer expanded per line).

## 2. Facts that shape the design

- **The bridge** (`docs/HARDWARE.md`): LT8912B on the main I²C bus (GPIO7/8, **100 kHz**: at 400 kHz reads and writes were corrupted), three register pages at 0x48 (main), 0x49 (CEC/DSI), 0x4A (AVI infoframe). Reset is an RC circuit only, so **an ESP reset doesn't reset the bridge**: every setup writes all its registers. No interrupt or hot-plug line: hot-plug is bit 7 of main 0xC1, valid once the bridge's digital clocks are on. 2 DSI data lanes; the DSI PHY is powered by on-chip LDO 3 at 2.5 V. The monitor's DDC (EDID at 0x50) is on the same bus.
- **The bridge takes 24-bit RGB** over DSI and sends HDMI (or DVI). No audio on this board.
- **The DSI link:** the P4's PHY does 80-1500 Mbps per lane, 2 lanes: up to a 125 MHz pixel clock in 24-bit. Standard 1080p60 (148.5 MHz) doesn't fit; 1080p50 with reduced blanking (about 115 MHz) would; 720p60 runs at an 80 MHz pixel clock with 960 Mbps lanes (Espressif's timing: htotal 1800, vtotal 741, 59.98 Hz; the DPI clock divides a PLL, so CEA's 74.25 MHz isn't available). The DSI bridge converts its input format (RGB565, RGB666, RGB888) to the link's 24 bits, so the line buffers can be 16-bit.
- **The DSI bridge pulls pixels with the DW-GDMA** (IDF's private `esp_private/dw_gdma.h`): IDF's DPI panel gives it one block, the whole framebuffer in PSRAM, re-armed each frame. The DMA also does **circular linked lists with an interrupt per block**, which is what a ring of line buffers needs. IDF has no line-buffer ("bounce buffer") mode for DSI (its RGB-LCD driver has one), so the scanout is ours, on the DSI HAL and the DW-GDMA, with IDF's DPI panel as the reference for the register setup.
- **The cost moves from memory to rendering.** 720p60 sends 80 Mpixel/s: 160 MB/s of 16-bit pixels, or 240 MB/s of 24-bit, all written by the renderer into internal RAM. Text rows rendered from a glyph bitmap with two colors per cell: about 160 cells per line at 8 pixels each, 720 lines, 60 frames: **6.9 million cell-lines a second**, so a cell-line must cost a few tens of cycles at most. 16-bit output halves the bytes written. The P4's SIMD (`xespv2p1`) and its PPA (which has a palette lookup, L8 with a CLUT, that IDF doesn't drive yet) are for later depths and graphics.
- **The bridge setup** is BMDS's own, from the register meanings in Linux's (GPL) and Espressif's (Apache-2.0, known to work with the P4) drivers; no code is copied. Espressif's analog values are used.

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| V1 | Bridge driver | **Our own `Lt8912b`** over our `I2cBus`. Done. | Espressif's component |
| V2 | Scanout | **Our own, line-rendered**: the DSI HAL for the link and DPI timing, the DW-GDMA streaming a circular ring of line buffers in internal DMA RAM, a refill task woken per block | IDF's DPI panel with a PSRAM framebuffer (the step 1 bring-up; rejected: memory, bandwidth, resolution) |
| V3 | I²C | **`I2cBus` over IDF's `i2c_master`**, numbered. Done. | our own on the LL layer |
| V4 | Line buffer format | **Measured in step 2**: RGB565 (the bridge widens it) unless RGB888 renders nearly as fast | RGB888 |
| V5 | First mode | **1280×720 at 60 Hz**; other modes later | the EDID's preferred |
| V6 | Hot-plug | **Read 0xC1 every 2 s** in the display service; act on changes (EDID, output on or off) | interrupt (none on this board) |
| V7 | Where | **`bmds_drivers`**: `I2cBus`, `Lt8912b`, `Edid`, `Scanout`; the text renderer with the console | one component per driver |

## 4. Step 1: bring-up (done)

- **1a, the bridge on I²C:** `I2cBus`, `Lt8912b` (chip id, hot-plug), `Edid`, the `display` command (status, `edid`, `scan`).
- **1b, a picture:** `display test 720` sets the bridge up and starts IDF's DPI panel showing the DSI controller's own bar pattern: bars on the monitor. Kept until step 2's scanout replaces it.

## 5. Step 2: the line-rendered scanout (next)

- **`Scanout`** (`bmds_drivers`): powers the PHY (LDO 3), brings up the DSI host and PHY at the mode's lane rate, programs the DPI timing and the DSI bridge (input format, the frame's size, flow control by the DMA), and starts the DW-GDMA on a circular list of `B` buffers of `L` lines each in internal DMA RAM. Each block's completion wakes the **refill task** (pinned to core 1, high priority), which asks the `LineSource` for the next `L` lines into the buffer just sent. A frame counter and the bridge's underrun count tell whether the renderer kept up.
- **`LineSource`:** `void render(uint16_t firstLine, uint16_t lines, uint8_t *dst, size_t stride)`; the mode's format and width are known to it.
- **A test source** first: bars and a gradient computed per line, then a moving element (so tearing or a lost block shows).
- **Measure:** cycles per block for RGB565 and RGB888 at 720p; the refill task's share of core 1; underruns while the other core hammers PSRAM and internal RAM; internal RAM for `B`×`L` (720p, 8 lines: 20 KB per buffer in RGB565, 30 KB in RGB888). Then V4.
- **Then:** `display test` uses it; IDF's DPI panel and its framebuffer go.

## 6. Later steps (outline)

- **3, the text renderer:** `TerminalScreen` (host-tested, `terminal.md` §3) holding the cells (in PSRAM: a 160×45 grid is about 72 KB; each text row's cells copied to internal RAM once per 16 lines); a `LineSource` drawing glyph rows with each cell's colors; an 8×16 bitmap font with a licence that fits (permissive or public domain), in internal RAM.
- **4, the HDMI console:** a `Console` whose output is the screen; a shell instance on it (output only until Phase 5); the boot log on it. `Display` as a service: hot-plug, output off without a monitor.
- **Beyond Phase 4's text console:** modes (720p and up, the EDID's preferred where the link allows), depths for programs (8-bit palette, 16, 24), line-based graphics.

## 7. Open questions

- The DW-GDMA and the DSI bridge in a never-ending circular list: IDF re-arms one block per frame; whether the bridge's per-frame block count and "last block" markers allow an endless ring, or the ring must be re-armed each frame (a per-frame interrupt, cheap). Step 2 finds out.
- Rendering in the DMA interrupt instead of a task (lower latency, fewer buffers; but long interrupts).
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
