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
| **Scripts** (`devplan/scripting.md`) | `angel`'s script API (every type, function, method, property and constant registered), its language (the AngelScript version, the engine settings, float literals and top-level statements), its command line, exit statuses and diagnostic format, `--report-fd`'s lines, `--serve`'s protocol (`devplan/editor.md` §9: an editor may use it), and `--debug`'s (§17: its commands, events and answers). Scripts are kept as source; bytecode isn't ABI. |
| **The GUI protocol** (`devplan/guiprograms.md` §4a, §8) | What programs and the GUI service say on a connection (`gui_open`, slot 124; later TCP; a local canvas's surface by `gui_canvas_surface`, slot 125): the framing, `Hello` and its answer (its extensions: `CLIPBOARD`, `CANVAS-SURFACE`, `SERVICES`, and `SHELL` for the programs the setting `gui/shell-clients` names), the requests and their bodies, the value tags (`Bytes`, 10, appended for the clipboard's data), the error codes, the event message; each class's name, parent, properties, methods and events, and what each means. All append-only: a new request, tag, error code, class or name is added, never changed. `bmds/gui.h` and `libbmdsgui` speak it, and `bmds/tray.h` gives the tray's messages on the service bus (the system's `TrayProtocol`, checked equal at build); the C++ codec (`bmds/guiprotocol.h`) is the system's side. The classes are declared once, in `sdk/gui/classes.def` (§10): the service binds them from it, `tools/guigen.py` writes the C wrappers (`bmds/guiclasses.h`) from it, and `tests/scripts/gui.lock` locks it (the `gui_lock` test; lines appended, never changed). |

Kernel internals — C++ classes, driver interfaces, internal headers under `components/` — are **not** userspace and may change freely.

## Rules

1. **Append only.** New syscalls take the next free slot. Slots are never reused, reordered, or removed.
2. **Semantics are frozen.** To change behavior, add a new syscall or a new flag whose default preserves the old behavior.
3. **Deprecated means "discouraged", never "removed".** A deprecated call keeps working; it may be reimplemented in terms of newer calls.
4. **Extensible structs.** Every struct crossing the boundary starts with `uint32_t size`, filled by the caller with `sizeof` as they saw it. The kernel reads/writes only the first `size` bytes; fields beyond what an old program knows about are treated as zero/default. New fields are only appended. Appending one changes the struct's size, so its lock (`abilock.c`) gains the new field's offset and the new size, with the old size noted beside it: the one edit to a lock that is allowed (the user, 2026-10-10; first `bmds_proc_info_t`, 104 to 112 bytes).
5. **Unknown flags are rejected.** Syscalls fail with an error on unknown flag bits, so new bits can be defined later without changing what old programs get.
6. **Plain C ABI.** Fixed-width integer types only, no bitfields, no `bool`/`enum`/`int`-sized fields of implementation-defined width, standard RISC-V ilp32f calling convention (to be confirmed against the toolchain's default ABI). No C++ crosses the boundary.
7. **Errors are values.** Syscalls return error codes; a bad argument from a program must produce an error, never a kernel assert or crash (as far as the hardware lets us defend against it).
8. **Versioned discovery.** Programs can query the ABI version and probe for the presence of individual syscalls, so new programs can degrade gracefully on older kernels.

## Process

- Any change touching `sdk/` or the syscall table must state **"ABI:"** in the commit message with a one-line description of the change (e.g. `ABI: add bmds_gfx_blit (slot 42)`).
- **ABI lock tests** verify syscall slot numbers, struct sizes, and field offsets. These tests are append-only: a failing ABI test means the change is wrong, not the test.
- **The script API's lock** (`tests/scripts/api.lock`, checked against `angel --api` by the host tests) holds every declaration scripts see. Lines are never changed or removed; new API is appended to it. Upgrading AngelScript is an ABI change, taken only when the script corpus (`tests/scripts`) and the examples still compile and behave the same.
- A small corpus of **compatibility test programs** built against older SDK versions is kept and run against new kernels (`tests/compat/v1/`, kept exactly as built; `tools/bmds-compat` runs it on a board, the host tests check it loads).

## Design Decisions

Resolved in [`devplan/programs.md`](../devplan/programs.md) (Phase 3):

- **Call mechanism**: a versioned function table whose address is passed to the program's entry point. Calls cost a few cycles; if a later chip allows user mode, the kernel can hand programs a table of `ecall` stubs without rebuilding them.
- **Executable format**: ELF linked at address 0 with its relocations kept, which the loader patches for wherever it loads the image (`R_RISCV_32`, `R_RISCV_HI20`; DOS-EXE style), identified by a BMDS ELF note (`sdk/include/bmds/program.h`). Commands are found as `name`, `name.elf` or `name.sh` on `$PATH`; files starting with `#!` are launched through their interpreter, and `.sh` files by the built-in `sh`.
- **Where code runs**: from PSRAM through the caches (the internal heap is not executable with IDF's memory protection); measured as fast as flash or internal RAM once cached.
- **C library**: musl, linked statically into each program, over a layer in the program that carries out musl's Linux system calls on the table, so only the system call table is ABI. Error numbers are Linux's.
