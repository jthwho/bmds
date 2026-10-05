# Display: HDMI through the LT8912B, Framebuffer, Text Console

Status: **step 1a done, 1b next** (Phase 4, 2026-10-04): the bridge answers on I²C; a picture needs a monitor connected.

## 1. Goal

- A picture on an HDMI monitor: the P4's MIPI-DSI controller scans a framebuffer in PSRAM out to the board's Lontium LT8912B bridge, which drives the HDMI connector.
- Know the monitor: hot-plug state and its EDID (name, preferred mode), read over the bridge's and the monitor's I²C.
- A text console on it (Phase 4's `TerminalScreen` and font renderer, `terminal.md` §3): the shell's output on the screen, and with a keyboard (Phase 5) a full local shell.
- Measured first: what scanning out costs the rest of the system (PSRAM bandwidth), which decides the pixel format and the default mode.

## 2. Facts that shape the design

- **The bridge** (`docs/HARDWARE.md`): LT8912B on the main I²C bus (GPIO7/8, 400 kHz, 10k pull-ups), three register pages at 0x48 (main), 0x49 (CEC/DSI), 0x4A (AVI infoframe, audio). Reset is an RC circuit only; no interrupt or hot-plug GPIO: the hot-plug state is bit 7 of main register 0xC1. 2 DSI data lanes; the DSI PHY is powered by on-chip LDO channel 3 at 2.5 V. The monitor's DDC (EDID at 0x50) is level-shifted onto the same bus.
- **The bridge takes 24-bit RGB** over DSI and sends HDMI (or DVI) RGB. HDMI carries no audio on this board.
- **Lane rate:** 2 lanes at up to about 1 Gbps each, so 24-bit pixels at up to about 80 MHz: 1280×720 at 60 Hz, 1280×800 at 60 Hz, 1920×1080 at 30 Hz (`docs/HARDWARE.md`). Espressif's driver runs 720p at an 80 MHz pixel clock (htotal 1800, vtotal 741: 59.98 Hz; the DPI clock divides a 160 MHz PLL) with 960 Mbps lanes, rather than CEA's 74.25 MHz.
- **The register sequence** is not documented by Lontium publicly. Two drivers show it: Linux's (`drivers/gpu/drm/bridge/lontium-lt8912b.c`, GPL) and Espressif's `esp_lcd_lt8912b` (esp-bsp, Apache-2.0), which is the one known to work with the P4. They agree on the structure (digital clocks, TX/CBUS/HDMI-PLL/MIPI analog, MIPI basic set on page 0x49 with the lane count, the DDS (pixel clock recovery) table, the video timing registers, MIPI RX and DDS resets, the AVI infoframe, HDMI mode, the "LVDS bypass" PLL setup) and differ in a few analog values (TX analog 0x31/0x32/0x33). BMDS writes its own driver from the register meanings; no code is copied (Linux's is GPL; Espressif's would bring its licence notice).
- **ESP-IDF 6.1** has the DSI bus and a DPI ("video mode") panel in `esp_lcd` (`esp_lcd_new_dsi_bus`, `esp_lcd_new_panel_dpi`): it allocates the framebuffer(s) in PSRAM and scans them out by DMA continuously, converts the framebuffer format (`in_color_format`: RGB565, RGB666, RGB888) to the panel's (`out_color_format`), and has a built-in test pattern (`esp_lcd_dpi_panel_set_pattern`). The I²C master driver (`esp_driver_i2c`) and the LDO driver (`esp_ldo_acquire_channel`) are IDF's too.
- **Scanout reads PSRAM continuously.** 1280×720 at 60 Hz with an 80 MHz pixel clock reads 80 Mpixel/s: **160 MB/s in RGB565, 240 MB/s in RGB888**. BMDS runs PSRAM at 80 MHz (HEX mode: 16 bits per edge, about 320 MB/s raw; 200 MHz needs `IDF_EXPERIMENTAL_FEATURES`, `docs/HARDWARE.md`), and programs run from PSRAM and keep their data there. Scanout could take half of PSRAM's bandwidth, or most of it. **This must be measured before anything is built on it.**

## 3. Decisions (proposed)

| # | Question | Proposal | Alternatives |
|---|---|---|---|
| V1 | Bridge driver | **Our own `Lt8912b`** over our `I2cBus`, from the register meanings | Espressif's component (Apache-2.0, `esp_lcd` panel model, `calloc` per instance) |
| V2 | DSI and scanout | **IDF's `esp_lcd` DSI bus and DPI panel**, wrapped behind `Display` | our own on the DSI HAL (later, if IDF's costs too much) |
| V3 | I²C | **`I2cBus` over IDF's `i2c_master`**, numbered (`i2c0` main, `i2c1` UEXT), one device handle per address made at init | our own on the LL layer |
| V4 | Pixel format | **Decided by step 1's measurements**: RGB565 (half the bandwidth and memory) unless the cost of RGB888 turns out small | RGB888; RGB666 |
| V5 | Default mode | **1280×720 at 60 Hz** if the measurements allow, else 1024×768 or 800×600; the EDID's preferred mode when the bridge can do it | always the EDID's |
| V6 | Hot-plug | **Read 0xC1 every 2 s** from the display service (cost measured: one I²C register read, about 100 µs) and act on changes: EDID read, mode set, output on or off | interrupt (none on this board) |
| V7 | Where | **New component `bmds_drivers`** (`ARCHITECTURE.md`'s planned one): `I2cBus`, `Lt8912b`, `Display`; split when it grows | one component per driver now |

## 4. Step 1: bring-up and measurements

A `display` shell command drives the work, kept as the display's status command afterwards.

- **1a, the bridge on I²C:** `I2cBus` for the main bus; `display` probes 0x48-0x4A, reads the chip id (main 0x00/0x01, expected 0x12 0xB2), the hot-plug bit, and the monitor's EDID (128 bytes at 0x50, 100 kHz; then the extension block if there is one), and shows the monitor's name, its preferred mode and whether the bridge can do it. Verifiable without looking at a screen.
- **1b, a picture:** LDO 3 at 2.5 V, the DSI bus (2 lanes, 960 Mbps), the bridge set up for 1280×720 at 60 Hz, the DPI panel with one framebuffer; first the DPI test pattern, then bars drawn into the framebuffer. Needs someone looking at the monitor.
- **1c, the cost:** with scanout off, RGB565 and RGB888, at 720p60 and at 800×600 and 1024×768: a PSRAM copy (`memcpy` of 1 MB, cached and cache-bypassing), a PSRAM read loop, and step 1's 88 KB straight-line code benchmark cold (`programs.md` §2), each in cycles; and whether the picture stays clean while the CPU hammers PSRAM (underruns show as tearing or a blank line; the DSI driver may report them).
- **Then:** V4 and V5 decided from the numbers and recorded here; the DSI PHY LDO, the bridge and scanout off when no monitor is connected.

**Budget (estimate, before measuring):** firmware +15 KB (the IDF LCD and I²C drivers, the bridge, EDID parsing); internal RAM: the drivers' state, a few KB; PSRAM: the framebuffer, 1.8 MB (RGB565) or 2.8 MB (RGB888) at 720p.

## 5. Later steps (outline)

- **2:** `Display` as a service: modes (a table of timings the bridge and lane rate allow), hot-plug handling, output off without a monitor; `display mode WxH[@Hz]`.
- **3:** `TerminalScreen` (host-tested, `terminal.md` §3) and the renderer: a bitmap font (licence-compatible: a permissive or public-domain 8×16), damage tracking, glyphs drawn into the framebuffer by the CPU first, 2D-DMA/PPA where measurements say it pays.
- **4:** an HDMI console: a `Console` whose output is the screen; a shell instance on it (output only until Phase 5's keyboard); the boot log on it.

## 6. Open questions

- PSRAM at 200 MHz (`IDF_EXPERIMENTAL_FEATURES`): how much bandwidth it gives back, and whether it is reliable on this board (Olimex's production test runs it).
- Double buffering for programs drawing directly (Phase 6) versus the text console's single buffer with damage tracking.
- DVI versus HDMI mode (main 0xB2): HDMI with an AVI infoframe is what Espressif's driver sends; a DVI monitor ignores it.
- The bridge's undocumented analog values: Linux's and Espressif's differ for TX analog; step 1b uses Espressif's (proven with the P4) and notes it.

## 7. As built

**1a** (2026-10-04): `bmds_drivers` with `I2cBus` (IDF's `i2c_master`; the board's buses started by a boot stage, `i2c0` and `i2c1`; up to 8 devices per bus made on first use and kept), `Lt8912b` (the three pages, chip id, hot-plug bit), `Edid` (portable, host-tested: the base block, the preferred detailed timing, the name, HDMI from the CEA extension's vendor block) and `display` (status; `display edid` dumps the blocks; `display scan` lists the bus).
- **On the board:** `display scan` shows 0x18 (the codec), 0x48, 0x49, 0x4A and **0x4B** (a fourth address no driver uses); the chip id reads 0x12B2; with no monitor connected the hot-plug bit is clear and nothing answers at 0x50.
- **Measured:** firmware +23.5 KB (445,104 → 468,624 bytes), of which IDF's I²C master driver is 12.6 KB (742 bytes of it in IRAM) and `bmds_drivers` 3.9 KB; 440 bytes of internal RAM (the buses' state). IDF's driver is heavy for register reads: a driver on the LL layer is the alternative (V3) if flash or IRAM gets tight.
