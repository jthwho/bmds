# Userspace ABI Policy

> **We do not break userspace.**
>
> If a program that worked on an earlier BMDS release stops working on a later one, that is a BMDS bug. It doesn't matter whether the program relied on documented behavior, undocumented behavior, or a bug — if it shipped and programs depend on it, it stays.

This document defines what "userspace" means in BMDS, the rules for evolving it, and how compliance is checked. The specifics of the ABI (calling mechanism, executable format, startup state) are still being designed; this policy applies from the moment the first user program runs.

## What Counts as Userspace

| Surface | Examples |
|---|---|
| **Syscall interface** | Slot numbers, function signatures, argument/return conventions, error codes, semantics. |
| **SDK headers** | Every type, struct layout, constant, flag, and macro in `sdk/include/`. |
| **Executable format** | File format, required/optional sections, entry point signature, relocation types supported, startup register/memory state, argv/env passing. |
| **Runtime environment** | Memory regions a program may use, stack size guarantees, how the console/graphics/input are handed to the program, what happens at exit. |
| **Shell interface** | Command names, options, exit codes, and machine-parsable output used by scripts. |
| **On-disk formats** | Config files, directory conventions (e.g. where programs live), any file format BMDS writes that users or programs read. |

Kernel internals — C++ classes, driver interfaces, internal headers under `components/` — are **not** userspace and may change freely.

## Rules

1. **Append only.** New syscalls take the next free slot. Slots are never reused, reordered, or removed.
2. **Semantics are frozen.** To change behavior, add a new syscall or a new flag whose default preserves the old behavior.
3. **Deprecated means "discouraged", never "removed".** A deprecated call keeps working; it may be reimplemented in terms of newer calls.
4. **Extensible structs.** Every struct crossing the boundary starts with `uint32_t size`, filled by the caller with `sizeof` as they saw it. The kernel reads/writes only the first `size` bytes; fields beyond what an old program knows about are treated as zero/default. New fields are only appended.
5. **Unknown flags are rejected.** Syscalls fail with an error on unknown flag bits, so new bits can be defined later without changing what old programs get.
6. **Plain C ABI.** Fixed-width integer types only, no bitfields, no `bool`/`enum`/`int`-sized fields of implementation-defined width, standard RISC-V ilp32f calling convention (to be confirmed against the toolchain's default ABI). No C++ crosses the boundary.
7. **Errors are values.** Syscalls return error codes; a bad argument from a program must produce an error, never a kernel assert or crash (as far as the hardware lets us defend against it).
8. **Versioned discovery.** Programs can query the ABI version and probe for the presence of individual syscalls, so new programs can degrade gracefully on older kernels.

## Process

- Any change touching `sdk/` or the syscall table must state **"ABI:"** in the commit message with a one-line description of the change (e.g. `ABI: add bmds_gfx_blit (slot 42)`).
- **ABI lock tests** verify syscall slot numbers, struct sizes, and field offsets. These tests are append-only: a failing ABI test means the change is wrong, not the test.
- A small corpus of **compatibility test programs** built against older SDK versions is kept and run against new kernels.

## Open Design Questions

These will be resolved in `devplan/` before the first program runs:

- **Call mechanism**: function-pointer table passed to the program's entry point vs. `ecall`-style trap. (A table is faster; a trap enables future privilege separation via PMP / U-mode.)
- **Executable format**: relocatable ELF vs. position-independent ELF vs. a simple custom format produced from ELF at build time.
- **Where code runs**: executing user code from PSRAM (via cache) vs. internal SRAM, and how much of each a program gets.
- **C library**: what libc a program links against (newlib subset, picolibc, or our own) and whether it lives in the program or is provided by the kernel.
