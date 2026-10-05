# Display: HDMI through the LT8912B, Line-Rendered Scanout, Text Console

Status: **step 1 done, step 2 next** (Phase 4, 2026-10-04): the bridge is set up over I²C and shows the DSI controller's test pattern at 1280×720 on a monitor. Revised the same day (with the user): no framebuffer; the picture is rendered line by line into internal RAM as it is scanned out.

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
