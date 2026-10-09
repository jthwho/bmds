# Development Plans

Design notes and implementation plans for BMDS subsystems. Write a plan here **before** implementing a non-trivial subsystem, and keep it up to date as the implementation changes.

- [`roadmap.md`](roadmap.md) — phased plan and progress checklist.
- [`foundation.md`](foundation.md) — Phase 0: project skeleton, core types, board definition, logging, boot.
- [`shell.md`](shell.md) — Phase 1: console abstraction, USB-Serial/JTAG console, line editor, shell, built-ins, log history.
- [`shell-language.md`](shell-language.md) — Phase 1b: stdin/stdout/stderr, pipes, redirection, the bash-subset shell language.
- [`terminal.md`](terminal.md) — Phase 1b/4: ANSI/VT and 24-bit color as first-class, terminal modes, HDMI screen model.
- [`time.md`](time.md) — Phase 2: the system clock (Linux-style: set, slewed and disciplined by sources), time zones, `date`.
- [`storage.md`](storage.md) — Phase 2: SD card driver and hot-plug, FatFs (exFAT) under the BMDS VFS, files in the shell, YMODEM transfer.
- [`programs.md`](programs.md) — Phase 3: executables (relocatable ELF), the loader, the syscall table (ABI v1-3), the SDK (static musl), push-and-run.
- [`distribution.md`](distribution.md) — Phase 3: the programs shipped on the card (`dist/` recipes, `tools/bmds-dist`), BusyBox.
- [`display.md`](display.md), [`windows.md`](windows.md) — Phase 4: HDMI, surface 0, the window manager, the terminal program, screenshots.
- [`gui.md`](gui.md) — Phase 4: the GUI toolkit, context menus, the panel and its launcher.
- [`input.md`](input.md) — Phase 5: USB host (TinyUSB, split transactions through the board's hub), HID boot keyboards and mice, the pointer.
- [`hotkeys.md`](hotkeys.md) — Special keys (volume, mute, media) and global hotkeys: named actions, bindings with defaults, user-configurable later (planned).
- [`random.md`](random.md) — the kernel's random pool: boot noise, `/boot/random-seed`, ChaCha20 for `/dev/urandom` and `getrandom`.
- [`pty.md`](pty.md) — pseudo-terminals: a terminal emulator as an ordinary program (with `/bin`, `programs.md` §17).
- [`filetypes.md`](filetypes.md) — what a file is (`file_type()`, by name then contents), opening files by `mimeapps.list`, every file on the desktop.
- [`files.md`](files.md) — copies' names, the trash (freedesktop.org's layout on every filesystem), `trash`, the desktop's Paste and Delete, the terminal's menu.
- [`clipboard.md`](clipboard.md) — copy and paste: typed offers, conversion, `clip`, `/dev/clip0`, the terminal's selection, OSC 52, vi; drag and drop (later).
- [`sysconf.md`](sysconf.md) — the system configuration: typed parameters, `sysconf`, watchers, the SDK's calls.
- [`filebrowser.md`](filebrowser.md) — the file browser: the toolkit's furniture and item views, `bmds_files`, folder windows in the panel, `files`.
- [`audio.md`](audio.md) — Phase 7: audio devices (ES8311 on I²S), the mixing engine, OSS `/dev/dsp` and native audio fds, real-time clients, decoders, MIDI and the synth.

## Plan Document Template

Each subsystem plan (`devplan/<subsystem>.md`) should cover:

1. **Goal** — what this subsystem provides and why.
2. **Interface** — the public API (kernel-internal and, if any, userspace/ABI surface).
3. **Design** — structure, data flow, execution contexts (ISR / task / hot path), memory spaces used.
4. **IDF usage** — which ESP-IDF layers it builds on, which it replaces, and why.
5. **Resource budget** — expected RAM (per MemSpace), IRAM, flash, CPU; then measured numbers once implemented.
6. **ABI impact** — any userspace-visible surface and how it complies with `docs/ABI.md`.
7. **Testing** — host tests, target tests.
8. **Open questions / future work.**
