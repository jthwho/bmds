# Development Plans

Design notes and implementation plans for BMDS subsystems. Write a plan here **before** implementing a non-trivial subsystem, and keep it up to date as the implementation changes.

- [`roadmap.md`](roadmap.md) — phased plan and progress checklist.
- [`foundation.md`](foundation.md) — Phase 0: project skeleton, core types, board definition, logging, boot.
- [`shell.md`](shell.md) — Phase 1: console abstraction, USB-Serial/JTAG console, line editor, shell, built-ins, log history.
- [`shell-language.md`](shell-language.md) — Phase 1b: stdin/stdout/stderr, pipes, redirection, the bash-subset shell language.
- [`terminal.md`](terminal.md) — Phase 1b/4: ANSI/VT and 24-bit color as first-class, terminal modes, HDMI screen model.
- [`time.md`](time.md) — Phase 2: the system clock (Linux-style: set, slewed and disciplined by sources), time zones, `date`.
- [`storage.md`](storage.md) — Phase 2: SD card driver and hot-plug, FatFs (exFAT) under the BMDS VFS, files in the shell, YMODEM transfer.
- [`programs.md`](programs.md) — Phase 3: executables (static-PIE ELF), the loader, the syscall table (ABI v1), the SDK, push-and-run.

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
