# BMDS Architecture

> **Status:** early draft. This captures the intended structure and the open questions. Sections will be filled in (and moved to detailed `devplan/` documents) as each subsystem is designed.

## Layering

```
+---------------------------------------------------------------+
|  User programs (SD card)  —  built against the C SDK          |
+---------------------------------------------------------------+
|  Userspace ABI: syscall table (C, append-only)  — docs/ABI.md |
+---------------------------------------------------------------+
|  Shell  |  Program loader  |  Console (serial / HDMI text)    |
+---------------------------------------------------------------+
|  Kernel services: VFS, device model, input, display, audio,   |
|  network, memory spaces, logging, tasks/IPC wrappers          |
+---------------------------------------------------------------+
|  BMDS drivers: SD/MMC, USB host + HID, DSI/HDMI, audio, Eth   |
+---------------------------------------------------------------+
|  Board definition (all hardware truth)  |  bmds_core library  |
+---------------------------------------------------------------+
|  ESP-IDF: FreeRTOS kernel, startup, HAL/LL, selected drivers  |
+---------------------------------------------------------------+
|  ESP32-P4 hardware                                            |
+---------------------------------------------------------------+
```

### Relationship to ESP-IDF

FreeRTOS (as shipped in ESP-IDF) is the kernel: scheduling, tasks, and synchronization primitives. ESP-IDF also provides startup, the toolchain/build system, and the hardware abstraction layers.

BMDS owns the **driver model and the drivers** that make up the "PC": storage, filesystem, input, display, audio, network. For each driver we choose deliberately:

- Build on IDF's low-level layers (`hal/`, `soc/`, LL functions) — usually the right default; thin, register-level, well-tested.
- Use a higher-level IDF driver (`esp_driver_*`, `esp_lcd`, `usb_host`, `sdmmc`, `esp_eth`) when it is efficient enough and saves significant effort — wrapped behind a BMDS interface so it can be replaced later.
- Write our own when IDF's version costs too much RAM/CPU, imposes the wrong model, or doesn't exist.

That decision and its rationale is recorded per driver in its devplan document.

### Components (Planned)

| Component | Purpose | Depends on IDF? |
|---|---|---|
| `bmds_core` | Core types: `Error`, `Result`, `String`, `StringView`, `List`, `Map`, `Span`, `SharedPtr`, `UniquePtr`, `Buffer`, geometry types, `ObjectBase`, signals/slots. | **No** — host-testable |
| `bmds_board` | Board definitions, `MemSpace` mapping, chip constants. | Yes (thinly) |
| `bmds_kernel` | Boot sequence, task/IPC wrappers, logging, interrupt dispatch, device model. | Yes |
| `bmds_fs` | Block devices (`BlockDevice`, `RamBlockDevice`), the filesystem (FatFs) and the VFS. | **No** — host-testable |
| `bmds_storage` | The SD card (`SdCard`), the storage service (card detect, hot-plug), and the RAM disk (`/ram0/p0`). | Yes |
| `bmds_drivers` | Hardware drivers (may split per driver). | Yes |
| `bmds_console` | Terminal abstraction, serial console, HDMI text console, ANSI/VT parsing. | Partly |
| `bmds_shell` | Command shell, built-in commands. | Partly |
| `bmds_loader` | Executable loader, syscall table, program lifecycle. | Yes |
| `bmds_ui` | The desktop: surfaces' drawing (`Painter`), the window manager, fonts, menus' geometry and drawing. | Partly |
| `bmds_gui` | The GUI toolkit: an event loop, widgets, menus, over the window manager's public API (`devplan/gui.md`). | Partly |
| `bmds_terminal`, `bmds_panel` | Built-in GUI programs: `/bin/terminal`, `/bin/panel`. | Yes |
| `sdk/` | Userspace C headers, the C library (musl) and its system call layer, linker script, example programs. | No (cross-compiled) |

### Repository Layout (Planned)

```
CMakeLists.txt          ESP-IDF project root
sdkconfig.defaults      Checked-in configuration defaults
partitions.csv          Flash partition table
main/                   Minimal app_main() that hands off to the BMDS boot sequence
components/             BMDS components (bmds_core, bmds_board, ...)
sdk/                    Userspace SDK (C headers, musl and its system call layer, linker script, examples)
tests/host/             Host-side doctest unit tests (plain CMake)
tests/target/           On-target tests
tests/compat/           Compatibility corpus: programs from each SDK version, kept as built
dist/                   The distribution: recipes for third-party programs (BusyBox), built by tools/bmds-dist
tools/                  Host tools (bmds-elfcheck, bmds-run push-and-run, bmds-compat, bmds-dist, ...)
cmake/                  Shared CMake helpers (BMDS compile flags)
thirdparty/             Vendored third-party code (doctest, FatFs, littlefs, minimp3, stb_vorbis, dr_flac, the IANA tzdata
                        sources; musl is in sdk/libc)
docs/                   Project documentation
devplan/                Design notes and plans per subsystem
```

## Key Design Themes

- **Memory spaces.** Internal SRAM (~768 KB) is scarce and fast; PSRAM (32 MB) is plentiful and slower. Every allocation names its space. Bulk data (framebuffers, programs, file buffers) lives in PSRAM. See CODING_STANDARDS.md.
- **Consoles are objects.** A console is a terminal endpoint (serial, HDMI+keyboard, later maybe network). The shell runs against a console interface, so the same shell runs on any of them, and multiple instances can run at once.
- **Programs own the machine (mostly).** Like DOS: a foreground program gets most of RAM and CPU and direct access to its console and, if it asks, the framebuffer. Kernel services keep running underneath (USB, storage, the serial shell for recovery).
- **Recovery path.** The serial shell remains available even when a program has taken over the HDMI console, so a hung program can be killed and the system inspected without a power cycle where possible.

## Open Questions

- **Dual-core usage.** Pin kernel services to one core and give user programs the other? Or SMP with affinity hints?
- **Executable format and loading.** Decided in `devplan/programs.md`: ELF linked at 0 with its relocations kept, and our own minimal loader (Espressif's `elf_loader` supports more relocation types than we want to freeze).
- **Execution from PSRAM.** Measured (`devplan/programs.md` §2): cached code in PSRAM runs as fast as code in flash or internal RAM, and PSRAM fills cache misses 5.3 times faster than flash. Programs run from PSRAM.
- **Memory protection.** On the P4, IDF runs in M-mode only and locks its PMP entries at boot, so programs can't run in U-mode today. The syscall table (`devplan/programs.md`) leaves room for `ecall` stubs if that changes.
- **Filesystem.** FatFs for SD compatibility with PCs. Proposed in `devplan/storage.md`: FatFs vendored with exFAT, under BMDS's own VFS (files as `Stream`s behind `FileHandle`s), not IDF's VFS and newlib.
- **USB host stack.** IDF's `usb_host` + HID class driver vs. a lighter custom stack for keyboard/mouse.
- **Display path.** HDMI via the board's MIPI-DSI-to-HDMI bridge; resolution/timing choices; text rendering via the P4's 2D-DMA / PPA hardware where it helps.
- **Networking stack.** lwIP (IDF default) vs. something lighter.
