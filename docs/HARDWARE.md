# Hardware: Olimex ESP32-P4-PC

The initial (and currently only) target board. All hardware facts used by code must come from the board definition in `components/bmds_board/` — this document is the human-readable reference that the board definition is built from.

> **Verification status:** items marked **(TBD)** have not yet been confirmed against the schematic. Check the schematics in the [OLIMEX/ESP32-P4-PC](https://github.com/OLIMEX/ESP32-P4-PC) repository (`HARDWARE/`) before relying on them, and update this file with the confirmed part numbers, GPIOs, and board revision.

## Links

- Product page: <https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-PC/open-source-hardware>
- Hardware/software repository (schematics, demos): <https://github.com/OLIMEX/ESP32-P4-PC>
- ESP32-P4 technical reference manual and datasheet: <https://www.espressif.com/en/products/socs/esp32-p4>
- ESP-IDF ESP32-P4 docs: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/>

## SoC: ESP32-P4 (module ESP32-P4NRW32)

| Item | Value |
|---|---|
| HP cores | 2× 32-bit RISC-V, up to 400 MHz (RV32IMAFC + Espressif extensions) |
| LP core | 1× low-power RISC-V core |
| Internal SRAM | 768 KB (HP L2 memory) — **scarce; reserve for things that need it** |
| Other on-chip memory | TCM (small, fastest), LP SRAM — sizes per TRM (TBD) |
| PSRAM | 32 MB (in-package) |
| Flash | 16 MB external SPI flash |
| Radio | **None.** The ESP32-P4 has no Wi-Fi or Bluetooth. |
| Notable accelerators | 2D-DMA, PPA (pixel processing: scale/rotate/blend), JPEG codec, H.264 encoder (per TRM) |

## Board Peripherals

| Function | Implementation | Notes |
|---|---|---|
| **Video out** | HDMI via MIPI-DSI → HDMI converter | Bridge chip **(TBD)**; requires bridge init over I²C. |
| **LCD** | MIPI-DSI connector | For MIPI panels; shares DSI with HDMI? **(TBD)** |
| **Camera** | MIPI-CSI connector | Not planned for early phases. |
| **USB host** | 4× USB-A host ports | Via USB hub **(TBD hub chip)** on the P4's USB 2.0 HS OTG. Keyboard/mouse/storage. |
| **USB debug** | USB-C: USB-Serial/JTAG | Programming, serial console, JTAG debugging. Also power input. |
| **Ethernet** | 10/100 MAC + PHY, RJ45 | PHY chip **(TBD)**. Optional PoE via extension module. |
| **Audio** | 3.5 mm output jack | Codec/amp **(TBD)**. |
| **Storage** | microSD slot | SDMMC host; bus width and card-detect GPIO **(TBD)**. |
| **Expansion** | UEXT (10-pin), 20-pin 2.54 mm GPIO header | All free GPIOs on the 20-pin header. |
| **Power** | USB-C input, LiPo charger + step-up | Battery and external-power sensing available (UPS capability). GPIOs **(TBD)**. |
| **Buttons** | BOOT, RESET | |

## Development Connection

The board is programmed and monitored over the USB-C port (USB-Serial/JTAG). On the development machine, udev rules create `/dev/esp-*` symlinks for connected ESP boards; `esp-select` sets `ESPPORT` to the chosen device (see `docs/BUILDING.md`).

## Pin Map

**(TBD)** — to be filled in from the schematic when the board definition is written. Record: GPIO, function, peripheral, direction, notes, schematic net name.
