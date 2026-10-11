# CLAUDE.md

Guidance for Claude Code when working in this repository.

## Project

**BMDS ("Bare Metal Do Stuff")** — a DOS-like operating system for the ESP32-P4, targeting the Olimex ESP32-P4-PC board. FreeRTOS (via ESP-IDF) is the kernel; BMDS provides the drivers, filesystem, consoles, shell, program loader, and a stable C userspace ABI so separately-built programs can be loaded from SD card and run. Long-term goal: self-hosting (compile programs on the device).

Read before working:

- `CODING_STANDARDS.md` — **mandatory** conventions. Follow them exactly.
- `docs/VISION.md` — goals and non-goals.
- `docs/ABI.md` — the userspace ABI policy.
- `docs/ARCHITECTURE.md` — layering, components, open questions.
- `docs/HARDWARE.md` — board facts (verify TBD items against the schematic).
- `docs/platforms/esp32p4.md` — the chip's tricks and pitfalls (PIE, caches and DMA, the accelerators, ESP-IDF quirks). Read before low-level work.
- `devplan/roadmap.md` — current phase and progress; `devplan/<subsystem>.md` for subsystem designs.

## Prime Directives

1. **Do not break userspace.** Anything under `sdk/`, the syscall table, the executable format, shell command interfaces, and on-disk formats are frozen once shipped. Append only; never reorder, remove, or change semantics. Never edit an ABI lock test to make it pass. Commits touching the ABI include an `ABI:` line. If a requested change would break userspace, stop and say so.
2. **Efficiency is correctness.** The OS must use as little RAM/IRAM/flash/CPU as possible. No polling where an interrupt or notification works. No heap in ISRs or hot paths. Static/boot-time allocation in the kernel. `constexpr`/rodata over static initializers; no global constructors. Every allocation names a `MemSpace`. Check `idf.py size` impact for non-trivial changes.

## Key Conventions (summary — CODING_STANDARDS.md is authoritative)

- C++20, `-fno-exceptions`, `-fno-rtti`. Errors via `Error` / `Result<T>`, never `bool` for success/failure.
- Qt-inspired API style: `bmds` namespace (`BMDS_NAMESPACE_BEGIN/END`), PascalCase types, camelCase methods, `_member` privates, getters without `get`, `set`/`is`/`has` prefixes, `BMDS_` macros, `Ptr`/`UPtr` ownership aliases.
- Execution context in names: `fooFromIsr()` = ISR-safe; `tryFoo()` = non-blocking (`Error::WouldBlock`); `foo(unsigned int timeoutMs)` = blocking, **no default timeout**, use `bmds::WaitForever` (== 0) rather than literal `0`.
- Hardware truth (pins, peripherals, memory regions) lives only in the board definition (`components/bmds_board/`).
- `bmds_core` must not include ESP-IDF headers — it is unit tested on the host.
- Userspace SDK headers are C99-compatible C with `bmds_`/`BMDS_` snake_case naming; boundary structs start with `uint32_t size`.
- 8-column indentation with spaces; `.clang-format` is authoritative. Explicit `return;` at the end of void functions. Doxygen on all public API with thread-safety statements and `@isrsafe`/`@nonblocking`/`@blocking`/`@hotpath` tags.
- File header: `@file`, `@copyright Jason Howard. All rights reserved.`, `SPDX-License-Identifier: MIT`, license reference line. License is MIT.

## Environment

- ESP-IDF v6.1-dev at `~/esp/esp-idf`. The user runs `esp-env` (an interactive-shell function) before starting Claude Code. If `idf.py` is not on `PATH` in a Bash tool call, source it explicitly: `. ~/esp/esp-idf/export.sh` (prefix each command that needs it, since shell state doesn't persist between calls).
- The board shows up as a `/dev/esp-*` symlink (udev rules); `ESPPORT` may already be set by `esp-select`. If not, look in `/dev/esp-*`.
- Target: `esp32p4`.

## Commands

```bash
idf.py build                                  # firmware (first configure; then `cd build && build`)
idf.py -p "$ESPPORT" flash                    # flash (ask before flashing if the user hasn't said to)
idf.py size / idf.py size-components          # resource usage
cmake -S tests/host -B build-host -G Ninja; cd build-host && build && ctest --output-on-failure   # host tests
```

`idf.py monitor` is interactive; to capture serial output non-interactively, use a bounded read of the port (e.g. a short Python/pyserial script with a timeout) instead.

## Working Practices

- Write or update a `devplan/` document before implementing a non-trivial subsystem; record IDF-usage decisions and resource budgets there.
- Keep `devplan/roadmap.md` checkboxes current as work lands.
- When a TBD hardware fact is confirmed from the schematic, update `docs/HARDWARE.md` and the board definition together.
- Builds must be warning-free. Host tests must pass and stay quiet.
- When something about the platform costs a debugging session or a measurement surprises, add it to `docs/platforms/<platform>.md` (what is true, how it was found, where it is used).

## Commits

- **Never commit unless the user explicitly says to.** The user verifies changes before committing.
- Instead, write the proposed message to `COMMIT_MSG` in the repo root (git-ignored).
- Format: one-line summary, blank line, then **at most two paragraphs** of detail focused on what changed and any hard-won details.
- Wrap every line of `COMMIT_MSG` at **100 characters** (summary line included).
- **Never** add a `Co-Authored-By` (or any attribution) line.
