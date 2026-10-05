# Roadmap

Phases are ordered so that each one makes the next faster to build. Each phase gets its own devplan document(s) before implementation starts. Check items off as they land.

## Phase 0 — Foundation

Plan: [`foundation.md`](foundation.md). Complete; verified on hardware.

- [x] ESP-IDF project skeleton (`CMakeLists.txt`, `main/`, `sdkconfig.defaults`, `partitions.csv`), builds for `esp32p4`.
- [x] Compiler flags: C++20, `-fno-exceptions`, `-fno-rtti`, warnings-as-errors for BMDS components.
- [x] `bmds_core` skeleton: `config.h`, namespace macros, `Error`, `Result<T>`, `StringView`, `Span`.
- [x] Host test harness (doctest, plain CMake) running `bmds_core` tests.
- [x] `bmds_board`: Olimex ESP32-P4-PC board definition from the schematic; `MemSpace` mapping. Fill in `docs/HARDWARE.md`.
- [x] Logging macros (`bmdsDebug/Info/Warn/Err`, per-module debug registration).
- [x] Boot sequence: `app_main()` → BMDS init stages, with boot timing and memory usage report.
- [x] Baseline size report recorded (RAM/IRAM/flash) for future comparison.

## Phase 1 — Serial Shell

Plan: [`shell.md`](shell.md). Complete; verified on hardware.

- [x] Console abstraction (a terminal endpoint: read/write bytes, size, capabilities).
- [x] Serial console on USB-Serial/JTAG.
- [x] Line editor: editing keys, history. (Completion is deferred to Phase 2, when there are paths to complete.)
- [x] Shell core: command registration (rodata table), argument parsing, exit codes.
- [x] Built-ins: `help`, `version`, `uptime`, `mem` (per-MemSpace usage), `tasks`, `log` (levels/modules), `reboot`.
- [x] Log history (`log show`), since a USB serial port misses the boot messages.

## Phase 1b — Shell Language, Standard I/O, Terminal Model

Plans: [`shell-language.md`](shell-language.md), [`terminal.md`](terminal.md). Done.

- [x] `String`, `List<T>` and an arena allocator in `bmds_core`.
- [x] Standard I/O: `Stream`, refcounted fd tables, pipes, memory/null streams; Ctrl-C interrupts.
- [x] Commands use stdin/stdout/stderr; pipelines (concurrent stages, workers with PSRAM stacks); fd redirection.
- [x] Shell language (bash subset): lexer, parser, executor; variables, expansion, `$(…)`, `$((…))`, `if`/`while`/`until`/`for`/`case`, functions, language built-ins, `printf`/`read`/`type`/`command`, `set -x`, `PS1`/`PS2`, completion. (`source` and path completion need storage: Phase 2.)
- [x] Terminal model: 24-bit color, SGR with depth downgrade, VT parser, UTF-8, `term` built-in, colored prompt and log levels.

## Phase 2 — Storage

Plans: [`storage.md`](storage.md), [`time.md`](time.md). Complete; verified on hardware.

- [x] SD card driver (SDMMC host), card detect, hot-plug handling; `BlockDevice`; `sd` command.
- [x] System clock (Linux-style: software clock, set/slewed/disciplined by sources, survives resets), time zones, `date`, `clock`.
- [x] Filesystem (FatFs with exFAT, vendored) and VFS: numbered block devices and partitions (`/dev/sd0p0`), run-time mount table with automatic mounting (`/sd0/p0`), `mount`/`umount`, `/dev` nodes, files as streams; host tests on a RAM disk.
- [x] Shell: `ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `df`, `hexdump`, `mkfs`.
- [x] Shell: file redirection, globbing, tilde, file tests, `source`/`sh`, `/sd0/p0/autoexec.sh`, path completion.
- [x] File transfer over serial: YMODEM `rz`/`sz` (works with picocom/minicom/tio through `lrzsz`).

## Phase 3 — Programs

- [x] Design: syscall mechanism, executable format, memory layout (`docs/ABI.md` open questions resolved).
- [x] Userspace SDK: C headers, startup code, linker script, host build (CMake toolchain file; `bmds-cc` for other build systems).
- [x] Loader: load from SD, relocate, run, return exit code; clean up on exit.
- [x] Syscall table v1: fd-based I/O (stdin/stdout/stderr, `read/write/close/dup2/isatty`), terminal info/modes, file I/O, memory, time, exit, args, environment.
- [x] ABI lock tests.
- [x] Shell: run programs by name, `PATH`-like search; `#!` and `.sh` scripts launched by content; kill on a second Ctrl-C.
- [x] Push-and-run over serial (host tool sends a program, device runs it — no reflash): `tools/bmds-run`, into the RAM disk (`/ram0/p0`); compatibility corpus `tests/compat/v1` (`tools/bmds-compat`).
- [x] The SDK on static musl (`devplan/programs.md` §12, step 7): vendored musl, the Linux system call layer, file identity, `ftruncate`; BusyBox builds and runs.
- [x] Emulated symbolic links on FAT/exFAT, Cygwin's format (`storage.md` §4: `symlink`, `readlink`, `lstat`); `ln -s`.
- [x] Terminal size learned from a serial terminal (`ESC [ 18 t`, the answer taken out of the input; `terminal.md`), `COLUMNS`/`LINES`, `TIOCGWINSZ`; `clear`, `reset`.
- [x] Distribution structure (`devplan/distribution.md`): `dist/` recipes, `tools/bmds-dist` build and install; BusyBox 1.37 (115 applets as links).
- [x] USB serial console: input bytes lost under two-way traffic. ESP-IDF's ESP32-P4 TX FIFO write is a read-modify-write that pops an RX byte (`shell.md`).
- [x] Storage: a sector cache for directories and the FAT under FatFs (`storage.md` §3): `ls -l` of BusyBox's `bin` 2.36 → 0.30 s.
- [ ] POSIX completion (§12, step 8+): process creation (`vfork`/`posix_spawn`, `waitpid`, `pipe`), signals from outside, `termios`, `poll`. Yardstick: BusyBox no-MMU (shells included) runs. A shared runtime only if measurements ask for it.
  - [x] 8a: processes, `vfork`/`exec`/`posix_spawn`, `waitpid`, `pipe`, close-on-exec (`programs.md` §13); `hush` runs programs and pipelines.
  - [x] 8b: `/bin/sh` (the shell as a process) and `#!` scripts in `exec`; `system`, `popen`.
  - [ ] 8c: BusyBox with `xargs`, `find -exec`, `time`, `nohup`.
- [ ] Example programs: `hello`, `cat`, a benchmark.

## Phase 4 — Display

- [ ] DSI-to-HDMI bridge bring-up; mode selection.
- [ ] Framebuffer in PSRAM (DMA-capable), double buffering policy.
- [ ] Text console: `TerminalScreen` (VT subset, 24-bit color cells) + bitmap font renderer with damage tracking (`terminal.md`).
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
