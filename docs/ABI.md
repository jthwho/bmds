# Userspace ABI Policy

> **We do not break userspace.**
>
> If a program that worked on an earlier BMDS release stops working on a later one, that is a BMDS bug. It doesn't matter whether the program relied on documented behavior, undocumented behavior, or a bug — if it shipped and programs depend on it, it stays.

This document defines what "userspace" means in BMDS, the rules for evolving it, and how compliance is checked. The specifics of the ABI are designed in `devplan/programs.md`; this policy applies from the moment the first user program runs.

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

## Design Decisions

Resolved in [`devplan/programs.md`](../devplan/programs.md) (Phase 3):

- **Call mechanism**: a versioned function table whose address is passed to the program's entry point. Calls cost a few cycles; if a later chip allows user mode, the kernel can hand programs a table of `ecall` stubs without rebuilding them.
- **Executable format**: ELF linked at address 0 with its relocations kept, which the loader patches for wherever it loads the image (`R_RISCV_32`, `R_RISCV_HI20`; DOS-EXE style), identified by a BMDS ELF note (`sdk/include/bmds/program.h`). Commands are found as `name`, `name.elf` or `name.sh` on `$PATH`; files starting with `#!` are launched through their interpreter, and `.sh` files by the built-in `sh`.
- **Where code runs**: from PSRAM through the caches (the internal heap is not executable with IDF's memory protection); measured as fast as flash or internal RAM once cached.
- **C library**: picolibc, linked statically into each program over a small glue layer, so only the syscall table is ABI.
