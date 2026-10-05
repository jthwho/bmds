# BMDS SDK

Everything needed to build programs for BMDS on a PC: the userspace ABI
headers, the C library (musl, built from source with the program), the
startup code and the Linux system call layer it runs on, the linker script
and CMake helpers. The design is in [`devplan/programs.md`](../devplan/programs.md);
the headers in `include/bmds/` are the ABI and never change incompatibly
([`docs/ABI.md`](../docs/ABI.md)).

| Path | What |
|---|---|
| `include/bmds/sys.h` | The system call table: slot numbers, typed calls (`bmds_open()`, `bmds_term_set()`...), their structs and flags, the startup contract. |
| `include/bmds/program.h` | The executable format and the program note. |
| `include/bmds/errno.h` | The system calls' error numbers (`BMDS_E...`). Programs using musl see Linux's in `errno`. |
| `libc/musl/`, `libc/README.md` | musl 1.2.5, vendored with BMDS's patches (`libc/patches/bmds.diff`). |
| `crt/start.c` | `_start`: the argument and auxiliary vector musl's startup wants, the thread-local template. |
| `crt/linux.c` | The Linux system calls musl makes, carried out on the BMDS table (`__bmds_syscall`). |
| `ld/bmds.ld` | The program layout: one image linked at 0, the note, thread-local storage. |
| `cmake/bmds-toolchain.cmake` | A CMake toolchain file for Espressif's `riscv32-esp-elf-gcc`. |
| `cmake/BmdsProgram.cmake` | `bmds_add_program(name SOURCES ... [STACK_SIZE bytes])`; generates `bmds-cc`. |
| `cmake/BmdsLibc.cmake` | Builds musl's `libc.a` and assembles its headers in the build tree. |
| `cmake/bmds-cc.in` | The compiler wrapper for other build systems (`CC=<build>/bmds-cc make`). |
| `examples/` | `hello`; `cat` (files or stdin, with the raw calls); `ls [-al]` (portable POSIX only). |
| `tests/` | `progtest`, used by the loader's board tests. |

## Building a program

The compiler is the one ESP-IDF installs (`riscv32-esp-elf-gcc`); run IDF's
`export.sh` first, or the toolchain file finds it in `~/.espressif`. Its
own C libraries are never used: programs compile against musl's headers
(`-nostdinc`) and link `-nostdlib` with the SDK's `libc.a`, `libbmds.a` and
libgcc. musl is built once per build directory (about 1,300 files).

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

### Other build systems

Configuring any SDK project (the examples will do) also writes
`<build>/bmds-cc`, a compiler driver with the same flags; build the target
`bmds_libs` once, then use it as `CC`:

```bash
cmake -S <bmds>/sdk/examples -B sdk-build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<bmds>/sdk/cmake/bmds-toolchain.cmake
cd sdk-build && build bmds_libs
cd ~/src/busybox && make CC=<path>/sdk-build/bmds-cc AR=riscv32-esp-elf-ar ...
riscv32-esp-elf-objcopy --strip-unneeded busybox_unstripped busybox
```

`BMDS_STACK=bytes` in the environment sets the stack the linked program
asks for. `-lm`, `-lpthread`, `-lrt`... find empty archives: musl keeps all
of it in `libc.a`. BusyBox 1.37 with `CONFIG_NOMMU` builds this way
unmodified (about 90 applets; `hush` starts programs and runs pipelines
with `vfork`).

## What a program is

- C (C99 or later; C++ without exceptions or RTTI, and without its
  standard library for now: the toolchain's libstdc++ is built for newlib),
  musl for the C library, `main(argc, argv, envp)`, an exit status.
- Built for every ESP32-P4 revision: `rv32imafc` with atomics, single
  float (`ilp32f`), Espressif's hardware loops and PIE SIMD, `medany`.
- Linked at address 0 with its relocations kept (`--emit-relocs`); the
  kernel moves it where it loads it. `strip --strip-unneeded` (what the SDK
  does) is fine; a full `strip` removes what the loader needs.
- `gp` belongs to the kernel: build with relaxation off (the toolchain
  file does) and never set it.

## The C library

musl, linked statically: POSIX as on Linux, from source (devplan/programs.md
§12). Under it, `crt/linux.c` carries out the Linux system calls musl makes
on BMDS's table; programs never see that layer. What differs from Linux:

- **Files:** FAT has no permissions or owners: modes are made up (directories
  0755; files 0644, or 0444 when read-only; execute bits as the shell's
  `test -x` decides: `.elf` and `.sh` by name, a name without an extension
  by its first bytes), every user is root, `chmod`/`chown` do nothing. No
  hard links (`link` is `EPERM`). Symbolic links work, emulated as
  Cygwin does: `symlink`, `readlink`, `lstat`, `O_NOFOLLOW`; `readdir` calls
  a System file a link by its attributes (`lstat` reads it to be sure). `st_ino` and
  `st_dev` tell files apart (a hash of the path, so a rename changes it).
  `ftruncate`, holes (a write past the end leaves zeros), `pread`/`pwrite`,
  64-bit `off_t`. A file open for writing can't be opened again (FatFs), and
  `stat` by name shows its size as of its last flush (`fstat` is current).
  `unlink` of an open file is `EBUSY`; `rename` across filesystems `EXDEV`.
- **Directories:** `readdir` lists `.` and `..` first, as Linux; up to 8 open.
- **Terminal size:** `TIOCGWINSZ` gives the console's size, which a serial
  terminal reports when the shell asks at each prompt (`term size` asks now);
  a resize while a program runs isn't seen until the next prompt (no
  `SIGWINCH` yet).
- **Memory:** `malloc` (musl's) over whole pages from the kernel; private
  file `mmap` is a copy; no shared mappings; everything is freed at exit.
- **Terminal:** `termios` maps `ICANON` (line input, with echo), `ISIG`
  (Ctrl-C) and `OPOST|ONLCR` onto the console's modes; `TIOCGWINSZ` works;
  echo can't be turned off by itself yet. `poll` is a guess (output and
  files are ready; terminal and pipe input is ready only for an endless wait).
- **Signals:** handlers, masks and `raise`/`abort`/`kill(getpid())` work
  within the program. Ctrl-C is not a signal yet: blocking calls fail with
  `EINTR`, and the program ends with status 130 however it exits.
- **Processes:** no `fork` (`ENOSYS`: no MMU). `vfork` + `exec`, as on
  uClinux: the child runs in the parent's memory, on its stack, until it
  execs or exits (`_exit`), while the parent waits; what it changes in
  memory the parent sees, its descriptors and directory are its own.
  `posix_spawn` (with file actions), `waitpid`/`wait4`/`waitid`, `pipe`,
  `O_CLOEXEC`/`FD_CLOEXEC`, `getpid`/`getppid` work. `exec` runs BMDS
  programs only so far: `system`, `popen` and scripts (`/bin/sh`) come with
  step 8b. A second Ctrl-C kills every process the shell's command started.
- **Not yet:** threads, sockets (`EAFNOSUPPORT`), `alarm`, `kill` of
  another process (signals from outside).
- **Size:** a `printf` hello is about 40 KB (`long double` is 128-bit on
  RISC-V, and musl's `printf` formats through it: about 12 KB of libgcc's
  soft quad-float; the system call layer is about 10 KB). Programs are
  PC-relative throughout: a few dozen relocations, so they load fast.

BMDS's own calls (`bmds/sys.h`) still work next to musl, on the same
descriptors, and do what POSIX has no call for: the terminal's modes
(`bmds_term_get`, `bmds_term_set`: raw keys, Ctrl-C as a key, log lines
off; the shell gets its modes back when the program ends), memory
(`bmds_mem_info`, `bmds_mem_alloc` with `BMDS_MEM_FAST`/`BMDS_MEM_DMA`).
Their structs start with `size`; initialize them as
`bmds_term_t t = {.size = sizeof(t)};`. Their errors are `BMDS_E...`
values, the same numbers as `errno`'s (Linux's).
