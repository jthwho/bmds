# Roadmap

Phases are ordered so that each one makes the next faster to build. Each phase gets its own devplan document(s) before implementation starts. Check items off as they land.

## Phase 0 — Foundation

- [ ] ESP-IDF project skeleton (`CMakeLists.txt`, `main/`, `sdkconfig.defaults`, `partitions.csv`), builds for `esp32p4`.
- [ ] Compiler flags: C++20, `-fno-exceptions`, `-fno-rtti`, warnings-as-errors for BMDS components.
- [ ] `bmds_core` skeleton: `config.h`, namespace macros, `Error`, `Result<T>`, `StringView`, `Span`.
- [ ] Host test harness (doctest, plain CMake) running `bmds_core` tests.
- [ ] `bmds_board`: Olimex ESP32-P4-PC board definition from the schematic; `MemSpace` mapping. Fill in `docs/HARDWARE.md`.
- [ ] Logging macros (`bmdsDebug/Info/Warn/Err`, per-module debug registration).
- [ ] Boot sequence: `app_main()` → BMDS init stages, with boot timing and memory usage report.
- [ ] Baseline size report recorded (RAM/IRAM/flash) for future comparison.

## Phase 1 — Serial Shell

- [ ] Console abstraction (a terminal endpoint: read/write bytes, size, capabilities).
- [ ] Serial console on USB-Serial/JTAG.
- [ ] Line editor: editing keys, history, (later) completion.
- [ ] Shell core: command registration (rodata table), argument parsing, exit codes.
- [ ] Built-ins: `help`, `version`, `uptime`, `mem` (per-MemSpace usage), `tasks`, `log` (levels/modules), `reboot`.

## Phase 2 — Storage

- [ ] SD card driver (SDMMC host), card detect, hot-plug handling.
- [ ] Filesystem (FAT, for PC interchange) and VFS mount points.
- [ ] Shell: `ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `df`, `hexdump`.
- [ ] File transfer over serial (push files from host to SD without removing the card).

## Phase 3 — Programs

- [ ] Design: syscall mechanism, executable format, memory layout (`docs/ABI.md` open questions resolved).
- [ ] Userspace SDK v1: C headers, crt0, linker script, host build (CMake toolchain file / Makefile template).
- [ ] Loader: load from SD, relocate, run, return exit code; clean up on exit.
- [ ] Syscall table v1: console I/O, file I/O, memory, time, exit, args.
- [ ] ABI lock tests.
- [ ] Shell: run programs by name, `PATH`-like search.
- [ ] Push-and-run over serial (host tool sends a program, device runs it — no reflash).
- [ ] Example programs: `hello`, `cat`, a benchmark.

## Phase 4 — Display

- [ ] DSI-to-HDMI bridge bring-up; mode selection.
- [ ] Framebuffer in PSRAM (DMA-capable), double buffering policy.
- [ ] Text console: bitmap font rendering, scrolling, cursor, color attributes, ANSI/VT subset.
- [ ] Shell instance on the HDMI console (output only until Phase 5).

## Phase 5 — Input

- [ ] USB host bring-up, hub support.
- [ ] HID keyboard (boot protocol first), keymaps/layouts.
- [ ] HID mouse.
- [ ] Input event model for the kernel and the SDK.
- [ ] Keyboard + HDMI console = a full local shell.

## Phase 6 — Graphics

- [ ] Graphics modes for programs (resolution / pixel format), direct framebuffer access.
- [ ] Basic 2D API (fill, blit, line, text), PPA / 2D-DMA acceleration where it pays off.
- [ ] Mouse cursor.

## Phase 7 — Sound and Network

- [ ] Audio output (I²S + codec), simple PCM API for programs.
- [ ] Ethernet bring-up, IP stack, basic tools (`ping`, file transfer).

## Phase 8 — Self-Hosting

- [ ] On-device text editor.
- [ ] Compiler evaluation (TinyCC RV32 port vs. alternatives) and port.
- [ ] Assembler/linker producing BMDS executables on the device.
- [ ] Build the SDK examples on the device.
