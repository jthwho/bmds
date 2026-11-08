# Third-Party Software

BMDS is MIT-licensed (`LICENSE`). It builds on the work below, each under its own licence. Every
licence text ships with the binaries: the firmware's in the ROM (`/rom/share/licenses/NAME`), the
card's packages in `share/licenses/PACKAGE` (put there by `tools/bmds-dist`).

Audited for the first public release on 2026-10-30 (`devplan/release.md` §6). When adding
anything, add it here, and give its licence a way onto the board.

## In the firmware

| Software | Where | Licence | Used for |
|---|---|---|---|
| ESP-IDF v6.1-dev (`ff97953b32a`) | toolchain, `IDF_PATH` | Apache-2.0 | drivers, the bootloader, build system |
| FreeRTOS kernel | ESP-IDF | MIT | the kernel |
| Mbed TLS | ESP-IDF | Apache-2.0 | ESP-IDF's crypto (linked by IDF components) |
| newlib | Espressif's GCC toolchain | BSD-style (`COPYING.NEWLIB`) | the firmware's C library |
| GCC runtime (libgcc, libstdc++) | the toolchain | GPL-3.0 with the GCC Runtime Library Exception | no notice needed for binaries |
| lwIP | `thirdparty/lwip` | BSD-3-Clause | the network stack |
| littlefs | `thirdparty/littlefs` | BSD-3-Clause | `/flash` |
| FatFs | `thirdparty/fatfs` | FatFs licence (BSD-1-Clause-like) | the card's FAT and exFAT |
| TinyUSB | `thirdparty/tinyusb` | MIT | USB host |
| Monocypher | `thirdparty/monocypher` | BSD-2-Clause or CC0-1.0 | Ed25519 for update packages, SSH |
| TinySoundFont | `thirdparty/tinysoundfont` | MIT | the SoundFont synth |
| stb_truetype | `thirdparty/stb_truetype` | MIT or public domain | TrueType fonts |
| stb_vorbis | `thirdparty/stb_vorbis` | MIT or public domain | Ogg Vorbis |
| dr_flac | `thirdparty/dr_flac` | public domain or MIT-0 | FLAC |
| minimp3 | `thirdparty/minimp3` | CC0-1.0 | MP3 |
| TJpgDec | `thirdparty/tjpgd` | TJpgDec licence (free use, keep the notice) | JPEG fallback decoder |
| SVOX Pico | `thirdparty/svox-pico` | Apache-2.0 (with `NOTICE`) | text to speech, the en-US voice |
| unscii-16 | `thirdparty/unscii` | public domain | the bitmap font |
| IBM Plex | `thirdparty/plex` | SIL OFL 1.1 | the TrueType fonts, in the ROM |
| Flat Color Icons (icons8) | `thirdparty/flat-color-icons` | MIT (or the Good Boy Licence) | the default icon theme |
| Glyphs (gorango) | `thirdparty/glyphs` | MIT | the second icon theme |
| DMXOPL3 (Sneakernets) | `thirdparty/opl-banks` | MIT (credit asked: "DMXOPL3 by Sneakernets") | the FM synth's bank, in the ROM |
| tz database | `thirdparty/tzdata` | public domain | time zones, leap seconds, in the ROM |
| AngelScript | `thirdparty/angelscript` | zlib | `angel`, in the ROM |
| Dropbear | `dist/dropbear` | MIT | SSH, in the ROM |

## In the SDK (in every program built with it)

| Software | Where | Licence |
|---|---|---|
| musl | `sdk/libc/musl` | MIT (`COPYRIGHT`) |

## On the card (`dist/`)

| Package | Licence | Notes |
|---|---|---|
| BusyBox 1.37.0 | GPL-2.0-only | source offered with each release (`bmds-X.Y.Z-sources.zip`) |
| Doom (doomgeneric `e257a09`) | GPL-2.0-only | with our platform layer (`programs/doom`, MIT); source offered with each release |
| TimGM6mb SoundFont 1.3 | GPL-2.0-only | the SoundFont is its own source; offered with each release all the same |
| Freedoom 0.13.0 | BSD-3-Clause | |
| CoreMark | Apache-2.0 | |
| tzdata | public domain | |
| Icon themes | MIT | |
| md4c 0.5.2 (`thirdparty/md4c`) | MIT | in Preso |
| BMDS's own programs (Lucky Duck, Blocks, angel, Preso, sshfs, ...) | MIT | |
| Newell's teapot (freeglut's data, `thirdparty/teapot`) | MIT | in demo1 as `teapot.bm3d`, its licence installed with it (`LICENSE-freeglut`) |

## The web installer (`installer/`)

| Software | Where | Licence |
|---|---|---|
| esptool-js 0.7.0 | `installer/vendor/esptool-js` | Apache-2.0 (bundles pako: MIT and zlib) |

## Test data only (not shipped)

| Software | Where | Licence |
|---|---|---|
| doctest | `thirdparty/doctest` | MIT |
| JSONTestSuite | `thirdparty/jsontestsuite` | MIT |
