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
| `include/sys/soundcard.h` | OSS (`/dev/dsp`, `/dev/mixer`) with Linux's numbers, carried out by `crt/oss.c` on the audio calls. |
| `include/bmds/errno.h` | The system calls' error numbers (`BMDS_E...`). Programs using musl see Linux's in `errno`. |
| `libc/musl/`, `libc/README.md` | musl 1.2.5, vendored with BMDS's patches (`libc/patches/bmds.diff`). |
| `crt/start.c` | `_start`: the argument and auxiliary vector musl's startup wants, the thread-local template. |
| `crt/linux.c` | The Linux system calls musl makes, carried out on the BMDS table (`__bmds_syscall`). |
| `crt/oss.c` | OSS's `SNDCTL_DSP_*` and `SOUND_MIXER_*` ioctls on the audio calls and the `audio/*` settings. |
| `ld/bmds.ld` | The program layout: one image linked at 0, the note, thread-local storage. |
| `cmake/bmds-toolchain.cmake` | A CMake toolchain file for Espressif's `riscv32-esp-elf-gcc`. |
| `cmake/BmdsProgram.cmake` | `bmds_add_program(name SOURCES ... [STACK_SIZE bytes] [ICONS png...])` (ICONS: the program's own icons, 16x16 and 32x32 PNGs, built in as `BMDS_NOTE_ICON` notes: `bmds/program.h`); generates `bmds-cc`. |
| `cmake/BmdsLibc.cmake` | Builds musl's `libc.a` and assembles its headers in the build tree. |
| `cmake/bmds-cc.in` | The compiler wrapper for other build systems (`CC=<build>/bmds-cc make`). |
| `examples/` | `hello`; `cat` (files or stdin, with the raw calls); `ls [-al]` (portable POSIX only); `tone` (a sine on `/dev/dsp`, through OSS or the native audio calls); `player FILE` (a WAV, MP3, Ogg Vorbis, FLAC or MIDI file on `/dev/dsp`, decoded by the firmware: `bmds_audio_decode`; MIDI files play through the synth on `synth/font`); `keytone [-p FRAMES] [-x] [-r RATE] [-m PORT]` (the keyboard as a piano octave, through a real-time audio client: `bmds_audio_cycle`; with `-m`, a MIDI port's notes too); `midimon [PORT]`, `midimon -c NAME` (MIDI events as they arrive, with their times: `bmds_midi_config`; `-c` makes a port that passes them on). |
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
- **Devices:** `/dev/null`, `/dev/zero`, `/dev/urandom` and `/dev/random`
  (the kernel's random pool, as `getrandom`: seeded from the chip's noise
  source at boot; never waits), `/dev/console` (the program's terminal), and
  `/dev/clip0` and `/dev/clip0p`: the clipboard's and the primary selection's
  text as a regular file. Read, it is the text when opened (empty if there is
  none); written, the text becomes what the file holds when it is closed
  (`>` empties it first, `>>` appends). See `devplan/clipboard.md`.
- **Time zones:** `TZ` is a POSIX rule string or a zone name
  (`TZ=Europe/Berlin`); names are read by musl from `/share/zoneinfo`, the
  card's time zone database shown read-only (empty without it: UTC). A
  program started without `TZ` gets the kernel's zone, by name when it has one.
- **Terminal size:** `TIOCGWINSZ` gives the console's size, which a serial
  terminal reports when the shell asks at each prompt (`term size` asks now).
  When the console's size changes while a program runs, its job gets
  `SIGWINCH`; a serial terminal's resize is only learned when something
  asks, so in practice at the next prompt.
- **Memory:** `malloc` (musl's) over whole pages from the kernel; private
  file `mmap` is a copy; a shared writable one is written back to the file
  (what changed) at `msync`, `munmap`, `exec` and exit, so other programs
  see the changes only then, and `SIGKILL` loses them; everything is freed
  at exit.
- **Terminal:** `termios` maps `ICANON` (line input), `ECHO`, `ICRNL`,
  `ISIG` (Ctrl-C) and `OPOST|ONLCR` onto the console's modes; `VMIN` 0
  with `VTIME` (a timed read) works, `VMIN` above 1 reads like 1; the other
  flags and control characters are reported as Linux's defaults and can't
  be changed. `TIOCGWINSZ` works.
- **Waiting:** `poll`, `ppoll`, `select`, `pselect` wait on pipes, the
  terminal and files (always ready), with timeouts and signal masks; the
  terminal in line mode is readable once a whole line is typed. Timeouts
  count whole milliseconds and aren't written back. `O_NONBLOCK` belongs to
  the descriptor number in one program: a `dup`'d descriptor or another
  program doesn't share it.
- **Signals** (signals 1-31): `kill` between processes (`0` the caller's
  process group, `-1` all, `-pgid`), handlers (`SA_SIGINFO`, `SA_RESTART`,
  `SA_RESETHAND`, `SA_NODEFER`), masks, `sigpending`, `sigsuspend`, `pause`;
  the terminal sends its foreground process group `SIGINT` (Ctrl-C),
  `SIGQUIT` (Ctrl-\\) and `SIGWINCH`, and its session `SIGHUP` when it goes;
  a write to a pipe nobody reads `SIGPIPE`, a child's end `SIGCHLD`.
  Handlers run between system calls: a program computing without calls
  takes a caught signal at its next one (one that ends it ends it at once).
  A Ctrl-C while the last one had no effect (ignored, or still pending)
  kills the group. Pid 1 (the serial shell) gets only the signals it
  handles. Nothing stops yet (`SIGSTOP`, `SIGTSTP` are dropped).
- **Process groups and sessions,** as POSIX has them: `setpgid`, `getpgid`,
  `setsid`, `getsid`, `tcsetpgrp`/`tcgetpgrp` (`TIOCSPGRP`, `TIOCGPGRP`,
  `TIOCGSID`). An interactive shell runs each foreground pipeline as a
  group with the terminal's foreground; it goes back to the shell when the
  group ends.
- **Pseudo-terminals** (`devplan/network.md` §24): `posix_openpt`, `grantpt`, `unlockpt`,
  `ptsname`, `openpty` (`/dev/ptmx`, `/dev/pts/N`), `TIOCSWINSZ`; a session leader takes one as
  its terminal with `TIOCSCTTY` or by opening it without `O_NOCTTY`, and `TIOCNOTTY` lets go.
  `/dev/tty` is the caller's terminal. `ttyname` works for terminals; termios on a terminal that
  isn't the caller's own sets and reports its modes, and its line input as a new terminal's.
- **Users:** one, `root` (uid 0): `/etc/passwd`, `/etc/group` and `/etc/shadow` (the password's
  SHA-512 crypt hash, which `crypt()` checks; set with the shell's `passwd`). `setuid`,
  `setgid` and `setgroups` to root succeed.
- **Filesystems:** `statfs`/`statvfs` give a mounted filesystem's size, free space and
  allocation unit; `fstatfs` is `ENOSYS`.
- **Processes:** no `fork` (`ENOSYS`: no per-process address spaces).
  `vfork` + `exec`, as on uClinux: the child runs in the parent's memory,
  on its stack, until it execs or exits (`_exit`), while the parent waits;
  what it changes in memory the parent sees, but for its descriptors,
  directory, signal handlers and environment (`environ`), which are its own.
  `posix_spawn` (with file actions), `waitpid`/`wait4`/`waitid`, `pipe`,
  `O_CLOEXEC`/`FD_CLOEXEC`, `getpid`/`getppid` work (`wait4`'s resource
  usage is zero: CPU time isn't counted). `exec` runs programs,
  `#!` scripts and `.sh` files; `/bin/sh` is the BMDS shell, so `system` and
  `popen` work.
- **Sockets** (`devplan/network.md` §18): IPv4 TCP, UDP and raw ICMP, as Linux has them:
  blocking or not (`O_NONBLOCK`, `MSG_DONTWAIT`), `poll`/`select`, `SO_RCVTIMEO`, multicast
  (`IP_ADD_MEMBERSHIP`), the interface `ioctl`s (`SIOCGIFCONF`, `SIOCGIFADDR`, ...), and
  `getaddrinfo` through `/etc/resolv.conf` and `/etc/hosts` (built from the settings). No
  `AF_INET6` or netlink yet: `socketpair`, `getifaddrs` and `if_nametoindex` fail. `AF_UNIX` is
  only the datagram socket `syslog()` sends to `/dev/log` on: musl's `syslog()` works, its lines
  in the system log (`devplan/logging.md`).
  The interfaces' state is `bmds_net_info`, and `bmds_net_watch` gives a descriptor that is
  readable after it changes (`devplan/network.md` §20).
- **Timers** (`devplan/programs.md` §18): `alarm`, `ualarm`, `setitimer` and `getitimer` with
  `ITIMER_REAL` (`SIGALRM`, never early). `ITIMER_VIRTUAL`/`ITIMER_PROF` are `EINVAL`.
- **Not yet:** threads, POSIX timers (`timer_create`),
  `sigaltstack`, `sigwaitinfo`, realtime signals, the sender's pid in
  `siginfo`.
- **Size:** a `printf` hello is about 40 KB (`long double` is 128-bit on
  RISC-V, and musl's `printf` formats through it: about 12 KB of libgcc's
  soft quad-float; the system call layer is about 10 KB). Programs are
  PC-relative throughout: a few dozen relocations, so they load fast.

BMDS's own calls (`bmds/sys.h`) still work next to musl, on the same
descriptors, and do what POSIX has no call for: the terminal's modes
(`bmds_term_get`, `bmds_term_set`: raw keys, Ctrl-C as a key, log lines
off; the shell gets its modes back when the program ends), memory
(`bmds_mem_info`, `bmds_mem_alloc` with `BMDS_MEM_FAST`/`BMDS_MEM_DMA`),
the chip's AES and SHA engines (`bmds_crypto`: AES-ECB, -CBC and -CTR,
SHA-1, -256 and -512 blocks, about 13 and 35 MB/s; check
`bmds_sys_has(BMDS_SYS_CRYPTO)`; `devplan/network.md` §26).
Their structs start with `size`; initialize them as
`bmds_term_t t = {.size = sizeof(t)};`. Their errors are `BMDS_E...`
values, the same numbers as `errno`'s (Linux's).
