# BMDS SDK

Everything needed to build programs for BMDS on a PC: the userspace ABI
headers, the startup code, picolibc glue, the linker script and CMake
helpers. The design is in [`devplan/programs.md`](../devplan/programs.md);
the headers in `include/bmds/` are the ABI and never change incompatibly
([`docs/ABI.md`](../docs/ABI.md)).

| Path | What |
|---|---|
| `include/bmds/sys.h` | The system call table: slot numbers, typed calls (`bmds_write()`...), the startup contract. |
| `include/bmds/program.h` | The executable format and the program note. |
| `include/bmds/errno.h` | Error numbers (`BMDS_E...`; picolibc's values). |
| `crt/crt0.S`, `crt/crt.c` | `_start`, thread-local setup, `main`'s caller, picolibc's POSIX glue (`read`, `write`...). |
| `crt/malloc.c` | `malloc` and friends over the kernel's allocator. |
| `ld/bmds.ld` | The program layout: one image linked at 0, the note, thread-local storage. |
| `cmake/bmds-toolchain.cmake` | A CMake toolchain file for Espressif's `riscv32-esp-elf-gcc`. |
| `cmake/BmdsProgram.cmake` | `bmds_add_program(name SOURCES ... [STACK_SIZE bytes])`. |
| `examples/` | `hello`, `cat` (stdin to stdout with the raw calls). |
| `tests/` | `progtest`, used by the loader's board tests. |

## Building a program

The compiler is the one ESP-IDF installs (`riscv32-esp-elf-gcc`, with
picolibc); run IDF's `export.sh` first, or the toolchain file finds it in
`~/.espressif`.

```cmake
cmake_minimum_required(VERSION 3.16)
project(myprog C ASM)
include(<bmds>/sdk/cmake/BmdsProgram.cmake)
bmds_add_program(myprog SOURCES main.c STACK_SIZE 65536)
```

```bash
cmake -S myprog -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<bmds>/sdk/cmake/bmds-toolchain.cmake
cd build && build            # or ninja
```

The result is two files: `myprog.elf`, what goes on the card (symbols no
relocation needs and debug information stripped, a tenth of the size), and
`myprog.debug.elf`, the same program with everything, for `addr2line` and
GDB. `bmds-elfcheck myprog.elf` (built with the host tests) checks it exactly
as the loader will. On the board, copy it to the card (`rz` and
`sz --ymodem myprog.elf` on the PC) and run it by path: `./myprog.elf`.

## What a program is

- C (C99 or later; C++ without exceptions or RTTI), picolibc for the C
  library, `main(argc, argv, envp)`, an exit status.
- Built for every ESP32-P4 revision: `rv32imafc` with atomics, single
  float (`ilp32f`), Espressif's hardware loops and PIE SIMD, `medany`.
- Linked at address 0 with its relocations kept (`--emit-relocs`); the
  kernel moves it where it loads it. `strip --strip-unneeded` (what the SDK
  does) is fine; a full `strip` removes what the loader needs.
- `gp` belongs to the kernel: build with relaxation off (the toolchain
  file does) and never set it.
