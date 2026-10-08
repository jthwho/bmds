# Hardware: Olimex ESP32-P4-PC

The initial (and currently only) target board. All hardware facts used by code come from the board definition (`components/bmds_board/boards/olimex_esp32p4_pc/bmds/boarddef.h`). This document is the human-readable reference the board definition is built from. Update both together.

> **Verification status.** Facts below come from the **Rev C** KiCad schematic, read as a full netlist export (the PDF has no usable text layer), and were cross-checked against Rev B. Every ESP32-P4 pin assignment is identical between the two revisions; the only difference is the core DC-DC part. Further sources: the BOM, the user manual (rev 2.0), and Olimex's production-test firmware (`SOFTWARE/ESP-IDF/p4_production_test`). Items marked **(unconfirmed)** still need checking on real hardware.

## Links

- Product page: <https://www.olimex.com/Products/IoT/ESP32-P4/ESP32-P4-PC/open-source-hardware>
- Hardware/software repository: <https://github.com/OLIMEX/ESP32-P4-PC>
  - `HARDWARE/ESP32-P4-PC-Rev.C/`: schematic (`.kicad_sch`, PDF) and BOM.
  - `HARDWARE/Hardware-revision-changes.txt`
  - `DOCUMENTS/ESP32-P4-PC-user-manual.pdf`
  - `SOFTWARE/ESP-IDF/p4_production_test`: a known-good sdkconfig, plus a modified copy of Espressif's `esp32_p4_function_ev_board` BSP.
- ESP32-P4 technical reference manual and datasheet: <https://www.espressif.com/en/products/socs/esp32-p4>
- ESP-IDF ESP32-P4 docs: <https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/>

## SoC: ESP32-P4NRW32

U1 is a bare QFN104 ESP32-P4 chip soldered to the board, not a module.

| Item | Value |
|---|---|
| HP cores | 2× 32-bit RISC-V (RV32IMAFC + Espressif extensions). **360 MHz on this silicon**: IDF limits pre-v3 revisions to 360 MHz; 400 MHz needs v3.x. |
| LP core | 1× low-power RISC-V core |
| Internal SRAM | 768 KB of HP L2 memory. **Scarce: reserve it for things that need it.** |
| TCM | 8 KB (the heap reports it as "HP core RAM"), mapped to `MemSpace::Fast` |
| LP SRAM | 32 KB |
| PSRAM | 32 MB in-package, HEX mode (AP Memory, generation 4, confirmed at boot). The production test runs it at 200 MHz; BMDS currently uses 80 MHz, because 200 MHz needs `IDF_EXPERIMENTAL_FEATURES`. |
| Flash | 16 MB Winbond W25Q128JVSIQ (U3), DIO at 80 MHz, powered from LDO VO1 |
| Crystals | 40 MHz main crystal (Q3). A 32.768 kHz crystal (Q1) is on **GPIO0/GPIO1, so those pins are taken**. |
| Core supply | External buck converter driven by the P4's EN_DCDC/FB_DCDC: TLV62569 on Rev C, TPS62A02 on Rev B. |
| Chip revision | **v1.3** (eFuse block v0.3), confirmed by esptool and the boot banner on our board. BMDS builds with `ESP32P4_SELECTS_REV_LESS_V3` (the IDF default, and what Olimex's production test uses). Newer production runs may ship v3.x silicon, which needs that option off. |
| Radio | **None.** The ESP32-P4 has no Wi-Fi or Bluetooth. |
| Accelerators | 2D-DMA, PPA (scale/rotate/blend), JPEG codec, H.264 encoder |

## Board Peripherals

| Function | Implementation | Notes |
|---|---|---|
| **Video out** | HDMI via a **Lontium LT8912B** (U12), 2 DSI lanes | Controlled over the main I²C bus at 0x48/0x49/0x4A. No ESP GPIO for reset (RC only: **an ESP reset leaves the bridge's registers as they were**), interrupt, or HPD; hot-plug state is read from LT8912B registers (main 0xC1 bit 7, valid once its digital clocks are on). Shows a picture at 1280×720 (confirmed 2026-10-04). HDMI carries video only. Up to 1920×1080 at 30 Hz or 1280×800 at 60 Hz. The DSI PHY is powered from LDO VO3 at 2.5 V. |
| **LCD** | 15-pin MIPI-DSI connector | **Shares the DSI lanes with the HDMI bridge, with no mux**, so use one or the other. |
| **Camera** | MIPI-CSI connector, 2 lanes | Camera I²C is on the main bus. No GPIOs for camera reset or strobe. Not planned for early phases. |
| **USB host** | 4× USB-A behind a **Terminus FE1.1s** hub (U7), on the P4's USB 2.0 HS OTG | Hub reset on GPIO21 (active low). VBUS comes from one CH217K switch controlled by the hub. The ESP has no GPIO for VBUS enable or overcurrent. |
| **USB debug** | USB-C carries **USB-Serial/JTAG only** (full-speed PHY, GPIO24/25) | Programming, console, JTAG. There is no separate USB-UART chip. Also the main power input (5.1k CC pull-downs, no PD). |
| **Ethernet** | **IC+ IP101GRR** PHY (U2), RMII, PHY address 1 | The PHY supplies the 50 MHz REF_CLK into GPIO50. PHY reset is GPIO51, and nothing else drives that pin. |
| **PoE** | Passive, through the optional POEv3 add-on module | The module plugs into a 4-pin header and feeds the TPS2116 power mux. No GPIO. |
| **Audio** | **Everest ES8311** mono codec (U8), 3.5 mm jack | I²C address 0x18 on the main bus. No speaker amplifier. Mic input on the jack. Codec supply enable on GPIO6, powered by default. No jack detect. **Left and right are one mono signal in opposite polarity** (see the notes). |
| **Storage** | microSD, native 4-bit SDMMC slot 0 (IOMUX pins) | The IO domain is powered by **on-chip LDO channel 4** (3.3 V). Card VDD is switched by GPIO45 (active low, on by default). Card detect is on GPIO3, but its pull-up is powered by LDO VO4, **so it only reads correctly while LDO 4 is on**. No write protect. |
| **Expansion** | UEXT (10-pin) | Carries UART0, a second I²C bus, and SPI. See the pin map. |
| **GPIO header** | 20-pin EXT1 | **Not fitted** (footprint only). |
| **Power** | USB-C or PoE (U13 mux, USB-C has priority); external 5 V or LiPo (U16 mux); TP4054 charger; MT3608 boost | Battery sense on GPIO20 (VBAT/2) and external 5 V sense on GPIO32 (digital only). Both need **solder jumpers closed** (open by default). No power-good or charger-status GPIO. |
| **Buttons** | BOOT (GPIO35), RESET (CHIP_PU) | BOOT is also RMII TXD1 and a strapping pin. |
| **LEDs** | Green user LED on GPIO2 (active low) | The power, charge, Ethernet and USB LEDs are hardware-driven, not ESP GPIOs. |

## Pin Map

Every ESP32-P4 GPIO with a board function. Netlist names are given where they help.

| GPIO | Function | Peripheral | Notes |
|---|---|---|---|
| 0, 1 | 32.768 kHz crystal | RTC | Not available |
| 2 | User LED (green) | GPIO out | Active low. Also on EXT1 pin 7. |
| 3 | SD card detect (`SD_DET`) | GPIO in | Pull-up powered from LDO VO4. Active low (confirmed on a Rev C board). |
| 4 | UEXT SCK | SPI | UEXT pin 9 |
| 5 | UEXT CS | SPI | UEXT pin 10 |
| 6 | Codec power (`CODEC_PWR_DIS#`) | GPIO out | Drives the codec LDO's CE. Pulled up, so on by default; drive low to switch off. |
| 7 | Main I²C SDA | I²C0 | 10k pull-up |
| 8 | Main I²C SCL | I²C0 | 10k pull-up |
| 9 | I²S DOUT (to codec DSDIN) | I²S | 33R series resistor |
| 10 | I²S WS (codec LRCK) | I²S | |
| 11 | I²S DIN (from codec ASDOUT) | I²S | |
| 12 | I²S BCLK (codec SCLK) | I²S | |
| 13 | I²S MCLK | I²S | |
| 14–19 | Free | — | EXT1 pins 8–13 |
| 20 | Battery sense (VBAT/2) | ADC | Needs the BAT_SENS_E1/E2 jumpers closed. EXT1 pin 14. |
| 21 | USB hub reset (`XRSTJ`) | GPIO out | Active low, 10k pull-up |
| 22 | UEXT SDA | I²C1 | 2.2k pull-up. UEXT pin 6. |
| 23 | UEXT SCL | I²C1 | 2.2k pull-up. UEXT pin 5. |
| 24, 25 | USB-Serial/JTAG D-/D+ | USB FS | USB-C |
| 26, 27 | Free (second FS PHY pins) | — | EXT1 pins 4 and 6 |
| 28 | RMII CRS_DV | EMAC | |
| 29 | RMII RXD0 | EMAC | |
| 30 | RMII RXD1 | EMAC | |
| 31 | MDC | EMAC | 1.5k pull-up |
| 32 | External 5 V sense | GPIO in | Digital only (not an ADC pin). Needs the PWR_SENS_E1 jumper closed. EXT1 pin 15. |
| 33 | Free | — | EXT1 pin 16 |
| 34 | RMII TXD0 | EMAC | Strapping pin |
| 35 | RMII TXD1 / BOOT button | EMAC / GPIO in | Strapping pin. 10k pull-up; the button pulls it to GND. |
| 36 | Free | — | 10k pull-up. EXT1 pin 17. |
| 37 | UART0 TX | UART0 | UEXT pin 3 |
| 38 | UART0 RX | UART0 | 4.7k pull-up. UEXT pin 4. |
| 39–42 | SD D0–D3 | SDMMC slot 0 | 4.7k pull-ups to LDO VO4 |
| 43 | SD CLK | SDMMC slot 0 | |
| 44 | SD CMD | SDMMC slot 0 | 4.7k pull-up to LDO VO4 |
| 45 | SD card power (`SD1_PWR_EN`) | GPIO out | P-FET gate pulled low, so the card is on by default. Drive high to switch it off. |
| 46–48 | Free | — | EXT1 pins 18–20 |
| 49 | RMII TX_EN | EMAC | |
| 50 | RMII REF_CLK (in) | EMAC | 50 MHz from the PHY |
| 51 | PHY reset | GPIO out | Active low. The only driver of the pin; the optional pull-up is not fitted. |
| 52 | MDIO | EMAC | 1.5k pull-up |
| 53 | UEXT MOSI | SPI | UEXT pin 8. The Espressif BSP's "power amp" pin, which this board doesn't have. |
| 54 | UEXT MISO | SPI | UEXT pin 7 |
| DSI | 2 data lanes plus clock | MIPI-DSI | To the LT8912B and the DSI connector in parallel |
| USB HS | DP/DM (dedicated pins 49/50 of the package) | USB 2.0 OTG | To the FE1.1s upstream port |

### Main I²C Bus (GPIO7/8)

**Run it at 100 kHz.** At 400 kHz about 40% of reads from the LT8912B came back wrong and writes were corrupted, with no acknowledge errors (measured 2026-10-04, Rev C board, a monitor connected). The bus carries 10k pull-ups and level shifters to 1.8 V and to the HDMI side's 5 V.

| Device | 7-bit address | Notes |
|---|---|---|
| ES8311 codec | 0x18 | Level-shifted to the 3.3VA domain. **Confirmed** (Rev C board, 2026-10-08): chip ID 0x8311 in 0xFD/0xFE, version 0x01 in 0xFF. Cut off the bus while its supply is off. |
| LT8912B bridge | 0x48, 0x49, 0x4A | Level-shifted to 1.8 V. **Confirmed** (Rev C board, 2026-10-04): all three answer, and 0x4B too (not used by any driver); chip id 0x12B2 in main registers 0x00/0x01. The schematic doesn't state the addresses. |
| HDMI sink DDC | 0x50 (EDID), plus other DDC devices | Level-shifted to 5 V. **Whatever the monitor puts on DDC appears on the system bus.** |
| DSI and CSI connectors | Panel or camera dependent | e.g. OV5647 camera at 0x36 **(unconfirmed)** |

### UEXT

| Pin | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|---|---|---|---|---|---|---|---|---|---|---|
| Signal | 3.3V | GND | GPIO37 TX | GPIO38 RX | GPIO23 SCL | GPIO22 SDA | GPIO54 MISO | GPIO53 MOSI | GPIO4 SCK | GPIO5 CS |

## Gotchas

- **USB-Serial/JTAG: reading `EP1` pops the RX FIFO, so TX bytes must be written with a plain store.** TX and RX share the one data register: a write queues a TX byte, a read takes an RX byte. ESP-IDF's `usb_serial_jtag_ll_write_txfifo()` for the ESP32-P4 writes each byte with `HAL_FORCE_MODIFY_U32_REG_FIELD`, a read-modify-write, so every byte it sends while input waits in the RX FIFO destroys one byte of that input (the S3 and C6 versions store directly). This was long taken for a silicon bug ("a pending RX packet is lost when TX is written"). BMDS writes the FIFO itself (`usbserialconsole.cpp`). See `devplan/shell.md`.
- **The USB-A ports need split transactions.** The FE1.1s is a high-speed hub, and keyboards and mice are low speed, so the host must reach them through the hub's transaction translator. ESP-IDF's USB host library (`espressif/usb`) has no split transactions; forcing the controller to full speed (`HCFG.FSLSSupp`) enumerates the hub, but low-speed devices behind it then fail their first transfer. BMDS uses TinyUSB, whose DWC2 host driver does splits (`devplan/input.md` §2). The hub enumerates as `1a40:0101`.
- **SD card detect depends on LDO 4.** Bring up the SD IO LDO before reading card detect.
- **GPIO35 does three jobs:** BOOT button, RMII TXD1, and a strapping pin. The button is only usable as an input while Ethernet is down.
- **HDMI hot-plug and bridge interrupt are not on a GPIO.** Detecting a display means polling LT8912B registers. The cost should be measured, and the polling kept rare.
- **The HDMI DDC shares the main I²C bus**, so a misbehaving monitor can disturb the codec and bridge buses.
- **HDMI and a DSI panel are mutually exclusive.**
- **The headphone jack's left and right are the codec's two differential outputs.** OUTP goes to left (HP-L) and OUTN to right (HP-R), each through 22 µF to a ground-referenced jack, so the two channels carry the same mono signal in opposite polarity. Headphones sound "phasey", a stereo speaker pair loses bass, and anything that sums left and right to mono cancels to near silence; use one channel, or reverse one speaker. Software can't fix it: the ES8311 has one DAC (`devplan/audio.md` §2).
- **The codec has its own LDO** (VR2, AP1231-3.3, from 5 V), its CE on GPIO6 (`CODEC_PWR_DIS#`, pulled up with 4.7k). It makes +3.3VA, which feeds PVDD and DVDD (through ferrite L5) and AVDD (through 10 Ω): **DVDD is 3.3 V**, so the guide's 3.3 V clock limits apply. Two BSS138s (FET3, FET4) disconnect the codec from the I²C bus while its supply is off. CE pin 20 is pulled to ground (R64 10k), hence 0x18. MCLK and the I²S lines have 33 Ω series resistors, and 27 pF to ground on three of the I²S lines near the codec.
- **The microphone contact (jack pin 1) is biased from ADCVREF** through 200 Ω, 1 µF and 2.2k (R79, C104, R82) into MIC1P. MIC1N is AC-coupled to the same bias through 2.2k and goes nowhere else: the input is pseudo-differential.
- **Olimex's `lan_test` Kconfig defaults are wrong** (MDC 41, MDIO 40, no reset). Its committed sdkconfig values (31, 52, 51) are right.

## Development Connection

The board is programmed and monitored over the USB-C port (USB-Serial/JTAG). On the development machine, udev rules create `/dev/esp-*` symlinks for connected ESP boards, and `esp-select` sets `ESPPORT` to the chosen device (see `docs/BUILDING.md`).
