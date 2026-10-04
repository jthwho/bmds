# BMDS SDK

Everything needed to build programs for BMDS on a PC: the userspace ABI
headers, the startup code, picolibc glue, the linker script and CMake
helpers. The design is in [`devplan/programs.md`](../devplan/programs.md);
the headers in `include/bmds/` are the ABI and never change incompatibly
([`docs/ABI.md`](../docs/ABI.md)).

| Path | What |
|---|---|
| `include/bmds/sys.h` | The system call table: slot numbers, typed calls (`bmds_open()`, `bmds_term_set()`...), their structs and flags, the startup contract. |
| `include/bmds/program.h` | The executable format and the program note. |
| `include/bmds/errno.h` | Error numbers (`BMDS_E...`; picolibc's values). |
| `crt/crt0.S`, `crt/crt.c` | `_start`, thread-local setup, `main`'s caller, the stdio glue (`read`, `write`, `lseek`, `_exit`). |
| `crt/posix.c` | The rest of picolibc's POSIX glue: `open`, `stat`, `opendir`, `getcwd`, `dup2`, `fcntl`, `clock_gettime`, `nanosleep`... |
| `crt/malloc.c` | `malloc` and friends over the kernel's allocator. |
| `ld/bmds.ld` | The program layout: one image linked at 0, the note, thread-local storage. |
| `cmake/bmds-toolchain.cmake` | A CMake toolchain file for Espressif's `riscv32-esp-elf-gcc`. |
| `cmake/BmdsProgram.cmake` | `bmds_add_program(name SOURCES ... [STACK_SIZE bytes])`. |
| `examples/` | `hello`; `cat` (files or stdin, with the raw calls); `ls [-l]` (portable POSIX only). |
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
as the loader will. To try it without the card, `tools/bmds-run myprog.elf
[args]` sends it to the board's RAM disk and runs it there, with its output
and exit status on the PC (pyserial: run it in ESP-IDF's environment). To
keep it, copy it to the card (`rz` on the board and `sz --ymodem myprog.elf`
on the PC) and run it by path (`./myprog.elf`), or
put it in `/sd0/p0/bin` (the default `$PATH`) and run it by name: `myprog`.
A script can name a program as its interpreter (`#!/sd0/p0/bin/myprog.elf`):
its `argv` is the interpreter, the line's optional argument, the script's path,
then the script's arguments.

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

## The C library

picolibc, over the system calls. What works as on any POSIX system:

- stdio and `malloc` (the kernel frees what a program still holds at exit).
- Files: `open` (`O_RDONLY`/`O_WRONLY`/`O_RDWR`, `O_CREAT`, `O_TRUNC`,
  `O_APPEND`, `O_EXCL`; `O_CLOEXEC`, `O_NOCTTY`, `O_SYNC` accepted and
  meaningless), `stat`/`lstat`/`fstat`, `access`, `mkdir`, `rmdir`,
  `unlink`/`remove` (not while the file is open: `EBUSY`), `rename` (`EXDEV`
  across filesystems), `utime`/`utimes`.
- Directories: `opendir`, `readdir`, `closedir` (up to 8 open), `getcwd`
  (also `getcwd(NULL, 0)`), `chdir` (the program's own current directory).
- Descriptors: `dup`, `dup2`, `fcntl` (`F_DUPFD`, `F_GETFL`; `F_GETFD` and
  `F_SETFD` do nothing: there is no exec).
- Time: `time`, `gettimeofday`, `clock_gettime` (`CLOCK_REALTIME`,
  `CLOCK_MONOTONIC`), `clock`, `localtime` (the kernel's time zone arrives as
  `TZ`), `nanosleep`, `usleep`, `sleep`; `getentropy` (hardware random).
- Ctrl-C: blocking calls fail with `EINTR`, and the program ends with status
  130 however it exits. `bmds_interrupted()` tells it apart from other errors.

BMDS's own calls (`bmds/sys.h`) do what POSIX has no call for: the terminal
(`bmds_term_get`, `bmds_term_set`: line input or raw keys, Ctrl-C as a key,
log lines off; the shell gets its modes back when the program ends), memory
(`bmds_mem_info`, `bmds_mem_alloc` with `BMDS_MEM_FAST`/`BMDS_MEM_DMA`).
Their structs start with `size`; initialize them as
`bmds_term_t t = {.size = sizeof(t)};`.

Limits: one thread (no `pthread`); no `fork`/`exec`, signals or pipes yet;
picolibc's `off_t` is 32 bits, so `stat()` misreports files over 2 GB
(`bmds_stat()` has the full size). The glue functions are linked into every
program as objects, so a program can't define its own `open`, `stat`...
