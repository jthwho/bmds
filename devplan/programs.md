# Programs: Executables, Loader, Syscall ABI v1, SDK

Status: **steps 1-10 done** (Phase 3), plus emulated symbolic links and the distribution (`distribution.md`): the PSRAM spike; the SDK skeleton, the format check and relocation (`ProgramImage`), `bmds-elfcheck`, the ABI lock; the loader; the system call table (51 slots: files, directories, time, terminal, links, processes, signals, `poll`), examples `hello`, `cat`, `ls`; programs and scripts run by name (`$PATH`, `#!`), Ctrl-C as `SIGINT`; the RAM disk, push-and-run (`tools/bmds-run`) and the compatibility corpus; the SDK on static musl (§12; BusyBox builds with it and runs). **The ABI was reset before the first release** (2026-10-04): the programs of the first SDK (picolibc) dropped with every compatibility path for them, errno values made Linux's, and the additions since folded into ABI 1. Next (agreed 2026-10-04): the drivers; the distribution grows as needed. D1-D7 agreed (2026-10-04; D2 revised in step 2, below). Once the first program ships, everything marked *frozen* is userspace ABI (`docs/ABI.md`): append-only forever.

## 1. Goal

- Programs are built separately from BMDS (cross-compiled on a PC now, on the device later), stored on the SD card, loaded and run by typing their name, and get (nearly) the whole machine while they run, as on DOS.
- A program sees the Unix I/O model the shell already has: fds 0/1/2 (console, pipe, file), `argc`/`argv`, the exported environment, an exit status. It works in pipelines and with redirections like any built-in.
- A small, stable C ABI: a program built against the first release's SDK runs on every later BMDS.
- Fast iteration: push a program over the serial line and run it without reflashing.
- The format and ABI are simple enough that a small self-hosted compiler and linker (TinyCC-class) can produce programs later.

## 2. Facts that shape the design (ESP32-P4, ESP-IDF 6.1)

- **PSRAM is executable at its data address** (0x48000000-0x4C000000, mapped `IBUS1|DBUS1` with `MMU_MEM_CAP_EXEC`, `esp_mm/port/esp32p4/ext_mem_layout.c`). With `SPIRAM_XIP_FROM_PSRAM` off (our config), no PMP entry covers PSRAM, so M-mode may fetch from it (`esp_hw_support/port/esp32p4/cpu_region_protect.c`). Turning PSRAM XIP on later would mark the PSRAM heap no-execute; we must not (the loader should check at boot and refuse to run programs if it is on).
- **Measured** (step 1 spike, 360 MHz, rev 1.3; code copied with `memcpy` into `heap_caps_aligned_alloc(MALLOC_CAP_SPIRAM)`, then `esp_cache_msync(C2M | INVALIDATE)`; results identical to the flash copies):

  | Case | flash (firmware) | internal RAM | TCM | **PSRAM** |
  |---|---|---|---|---|
  | small hot loop (CRC-32, in L1) | 329,715 | 327,832 | 772,111 | **329,708** cycles |
  | 88 KB straight-line code, warm (in L2) | 38,799 | — | — | **38,821** cycles |
  | the same, cold (caches invalidated) | 1,929,748 | — | — | **362,037** cycles |

  Code in PSRAM runs as fast as the firmware's own code from flash, or code in internal RAM, once cached, and fills cache misses **5.3 times faster** than flash (about 87 MB/s of instructions). Instruction fetch from TCM is 2.3 times slower: TCM is for data, not code.
- **Internal SRAM heap is not executable** (`CONFIG_ESP_SYSTEM_MEMPROT`: the I/D split PMP entries make the heap RW). TCM (8 KB) is executable but tiny. So **program code runs from PSRAM**, through the 16 KB L1 I-cache and the 128 KB L2 cache.
- **Loading code:** after writing it, `esp_cache_msync(..., DIR_C2M | INVALIDATE)` writes the D-cache back and invalidates L1 I, L1 D and L2 for the range; `fence.i` alone is not enough on the P4.
- **The MMU maps external memory only:** flash at 0x40000000 and PSRAM at 0x48000000, in 64 KB pages (`CONFIG_MMU_PAGE_SIZE`), 1024 entries each, one table for both cores (`soc/esp32p4/include/soc/ext_mem_defs.h`). An entry holds a physical page, valid and encryption bits: no write protection, no address-space id. Internal RAM is not translated. So it can't give each program its own address space or copy-on-write, and there is no `fork`; it could map scattered PSRAM pages as one range, or a file's pages, later.
- **MMU aliases work** (spike, 2026-10-04, standalone app on the board): one physical PSRAM page can be mapped at several addresses, which is how program instances could share their code (§14).
  - **Addresses:** the heap maps the first 32 MB of the window (0x48000000); 32 MB (512 pages) above it are free. `esp_mmu_map_reserve_block_with_caps()` (private API, permanent) reserved a block at 0x4A000000. IDF's PSRAM driver maps its own region the same way: `mmu_hal_map_region()` on MMU 1. `esp_mmu_map()` doesn't fit: it picks the address itself, one physically contiguous run per call.
  - **Code runs through an alias:** the same page at two addresses, each followed by a different private page; a PC-relative load (`auipc` + `lw`) in it read each instance's own word.
  - **The caches are tagged by address, not by page:** a write through one address isn't seen through another until that one is invalidated; a remapped address returns the old page's lines until invalidated; invalidating drops dirty lines (not written back). Rules: write a page back before mapping it, invalidate an address after mapping, and invalidate before unmapping (a dirty line written back to an unmapped address goes nowhere).
  - **Mapping cost** (360 MHz): IDF's way (caches off, the other core stalled) 15,000 cycles per call; changing only the entries, in a spinlock, plus invalidating 9 pages and unmapping: 19,000 cycles in all, with the other core running. 5000 such remaps while the other core ran PSRAM traffic and code through another alias: no errors.
  - **DMA:** GDMA (async memcpy) both ways and SDMMC reads into an alias work (the P4's SDMMC reaches PSRAM, and its driver syncs with `esp_cache_msync()`, which takes alias addresses); same speed as heap addresses. But `esp_ptr_external_ram()` is false for aliases (it checks the heap's range), so code that tests it (BMDS's SD driver, before DMA) treats them as internal RAM or bounces.
  - **An unmapped address** reads 0x0addbad0, with no exception: a stale pointer into an ended instance isn't caught.
- **No user mode:** IDF runs in M-mode only on the P4 (ESP-TEE supports C5/C6/H2), and its PMP entries are locked at boot. Memory protection between kernel and program is not available now (`docs/VISION.md` non-goal); the design keeps the door open (§4).
- **Toolchain:** `riscv32-esp-elf-gcc` 15.2. The firmware uses `rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1`, `ilp32f` (no Zc* on chip revisions below 3, erratum DIG-661). The toolchain ships **picolibc** and **newlib** (and newlib-nano) multilibs for `rv32imafc_zicsr_zifencei_zaamo_zalrsc` / `ilp32f`. `-fPIC`/`-fPIE`, `-mno-relax` work.
- **No ELF loader** in IDF or locally; Espressif's registry `elf_loader` exists but loads shared objects with a wider relocation set than we want to freeze.

## 3. Decisions (agreed unless marked)

| # | Question | Choice | Alternatives |
|---|---|---|---|
| D1 | Call mechanism | **A function table**, its address passed to the entry point | `ecall` trap |
| D2 | Executable format | **ELF linked at 0 with its relocations kept** (`ET_EXEC`, one image; the loader patches `R_RISCV_32` and `R_RISCV_HI20`), DOS-EXE style; revised from static PIE in step 2 | static PIE with our own picolibc built `-fPIE`; custom format; relocatable `ET_REL` |
| D3 | Where code runs | **PSRAM** (the whole image), measured as fast as flash or IRAM (§2); fast internal RAM for data on request | internal SRAM (needs MEMPROT off) |
| D4 | C library | **musl, linked into each program**, over a Linux system call layer in the program (step 7) | picolibc; newlib-nano; libc provided by the kernel |
| D5 | Instruction set | **Everything every P4 runs: `rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1`, `ilp32f`** (the firmware's own set) | the base set only; Zc* (rev ≥ 3 only) |
| D6 | File naming | **`name` runs `<dir>/name`, `<dir>/name.elf` or `<dir>/name.sh`** (in that order) from `$PATH` (default `/sd0/p0/bin`); what runs is decided by the file's first bytes: a BMDS ELF, or a `#!` script (D7) | `.elf` required; no extension |
| D7 | Scripts | **`#!` is planned into the launch path** from the start (§5), even if the first release only runs ELF | scripts only through `sh file` |

### D1: why a table

- A call through a table is an indirect call: a few cycles. An `ecall` traps into IDF's exception vector, saves the full context, dispatches, and returns: hundreds of cycles, for no protection today (M-mode only, locked PMP).
- **The table keeps traps possible.** Programs only ever call through the table they were given. If a later chip or IDF allows U-mode, the kernel hands such programs a table of small stubs that `ecall`; their binaries don't change. The reverse (traps now, table later) would need nothing either, but costs the trap price from day one.
- The table is versioned: `{ uint32_t magic, abi_version, count; void *slot[count]; }`. A slot number is frozen forever; a program probes a slot as `slot < count && slot[n] != NULL` (ABI rule 8).

### D2: ELF linked at 0, relocations kept (revised in step 2)

- **First choice, static PIE, didn't survive contact with the toolchain:** the C library that shipped with `riscv32-esp-elf-gcc` (used until step 7) is compiled for the `medlow` model (absolute `lui`/`addi` addressing, `R_RISCV_HI20`), which a PIE can't contain (ld refuses, then crashes). Keeping static PIE would have meant building the C library ourselves with `-fPIE`. Agreed instead (2026-10-04): the classic relocatable executable, as DOS did with `.EXE` files.
- **The format** (`sdk/include/bmds/program.h`): `ET_EXEC`, one `PT_LOAD` at virtual address 0 holding the whole image, linked with `--emit-relocs` so the relocation sections and the symbol table stay in the file. The kernel loads it at any 4096-byte-aligned address B and patches only two kinds: `R_RISCV_32` (a data word: add B) and `R_RISCV_HI20` (a `lui`: add B >> 12 to its upper immediate; B's low 12 bits are zero, so the matching `LO12` halves never change). References to absolute or undefined (weak) symbols aren't patched. Everything else in loaded sections is either position-independent (branches, calls, `PCREL_*`, thread-local `TPREL_*`, label differences `ADD/SUB/SET*`, the linker's `RELAX`/`ALIGN` hints) or refused, as is a `PCREL_HI20` to an absolute symbol that the linker didn't turn into a `lui`. Programs' own code is built `medany` (PC-relative, no GOT, slightly faster than PIE); medlow libraries work through `HI20`.
- **Identification:** `EM_RISCV`, `ELFCLASS32`, little endian, float ABI flag `EF_RISCV_FLOAT_ABI_SINGLE`, and a `PT_NOTE` with name `"BMDS"`, type 1, holding a `bmds_program_note_t` (size, ABI version the program was built for, requested stack size, flags). A file without the note is not a BMDS program (`exec format error`).
- **Cost:** the file keeps its relocations and the symbols they use, so it is bigger than its image: `hello.elf` is 34 KB for an 11.5 KB image after the SDK's `objcopy --strip-unneeded` (458 KB with everything, kept as `hello.debug.elf`). A full `strip` breaks a program (the loader says "no section headers (stripped? relocations are needed)").
- **Why it's still a good format:** stock toolchain and libraries; standard tools read it (`readelf`, `objdump`, `addr2line`, GDB); a small self-hosted compiler emits exactly this (non-PIC code plus relocations) without knowing about PIC; the loader is small and refuses whatever it doesn't understand instead of guessing.
- No dynamic linking, no shared libraries in v1 (`PT_DYNAMIC` and `PT_INTERP` are refused).

### D3: memory

- The image (text, rodata, data, bss) is one `Psram` allocation, aligned to 4096 bytes (the relocation scheme needs it); the loader copies the `PT_LOAD` segment, zeroes bss, applies relocations, syncs the caches.
- The program's **stack** is allocated from internal RAM if it fits (stacks are hot), else `Psram`: the note asks for a size (default 32 KB, at most 1 MB).
- **Heap:** musl's `malloc` over whole pages from the kernel's allocator (`mmap` in the system call layer, slots `mem_alloc(size, align, flags)`, `mem_free`, `mem_realloc`); programs can ask for fast internal RAM (`BMDS_MEM_FAST`) or DMA-capable memory (`BMDS_MEM_DMA`) with `bmds_mem_alloc` directly. The kernel tracks every allocation per program and frees what is left at exit.
- Code always runs from PSRAM through the caches; a program that needs a hot loop in fast memory is a later question (TCM is 8 KB).

### D4: libc

- **musl, static** (step 7, §12): the most complete POSIX libc, its Linux system calls carried out on the BMDS table by a layer in the program (`sdk/crt/linux.c`). Programs see POSIX and Linux's errno values; the ABI stays the system call table alone, as on Linux.
- musl sets up the thread pointer and TLS itself, from the `PT_TLS` entry the startup code hands it. The firmware has no thread-local variables (its TLS area is empty) and FreeRTOS saves `tp` per task, so a program's TLS doesn't disturb the kernel; the loader refuses to run programs if the firmware ever gains TLS. `gp` stays the kernel's: programs are linked without relaxation or a global pointer.
- A kernel-provided libc would make thousands of functions and every struct in them ABI; §11 and §12 keep it as a later option, if measurements ask for it.

### D5: instructions

- Programs may use everything every P4 revision executes, the same set the firmware is built with: the base `rv32imafc` with atomics, hard-float single precision (`ilp32f`, so floats pass in FP registers through the table), plus Espressif's **hardware loops** (`xesploop`) and **PIE SIMD** (`xespv2p1`, 128-bit vector registers). GCC 15 doesn't emit the Espressif instructions on its own (checked: plain loops compile the same either way); they come from intrinsics, inline assembly or libraries like esp-dsp, so allowing them costs programs that don't use them nothing.
- FreeRTOS on the P4 saves FPU, hardware-loop and PIE state per task, lazily, and a program runs in its own task (§5), so programs may use all three freely.
- **Zc\*** (smaller code) only works on chip revisions 3 and up (erratum DIG-661; this board is a v1.3). It can become an opt-in later: the loader reads the program's RISC-V arch attribute and refuses Zc* programs on older chips with a clear message, instead of crashing on them.
- The SDK builds musl with these flags itself (`sdk/cmake/BmdsLibc.cmake`): the toolchain's own libraries (newlib, picolibc, built for `medlow`) are never linked.

## 4. The syscall table (frozen once shipped)

- The entry point is `void _start(const bmds_sys_t *sys, int32_t argc, char **argv, char **envp)`; it stores `sys` in `bmds_sys` and hands musl's `__libc_start_main` the argv/envp/auxv array it expects (§12), and musl sets up `tp` and TLS, stdio, constructors, and calls `exit(main(...))`. The full startup contract (registers, stack, `gp`/`tp`, FP state, returning from `_start`) is in `sys.h`.
- The table: `{ magic "BMDS", abi_version, count, reserved, slot[] }`; `bmds_sys_has(n)` probes a slot. The SDK calls slots through typed inline functions (`bmds_write()`...), one indirect call each.
- Slot functions are plain C (`ilp32f`), return `int32_t`: ≥ 0 success, negative `BMDS_E*` error codes, a frozen list in `sdk/include/bmds/errno.h` mapped from the kernel's `Error` (kernel `Error` values stay internal).
- **Defined in step 2** (frozen from the first release): 0 `exit`, 1 `read`, 2 `write`, 3 `close`, 4 `seek` (64-bit offset, result through a pointer), 5 `isatty`, 6 `mem_alloc`, 7 `mem_free`, 8 `mem_realloc`: what a C library's `stdio` and `malloc` need.
- **Defined in step 4** (the rest of v1; `sys.h` documents each):
  - Files: 9 `open` (`BMDS_O_READ/WRITE/CREATE/TRUNC/APPEND/EXCL`, the VFS's `OpenFlag`s), 10 `stat`, 11 `fstat` (`bmds_stat_t`: type, FAT attributes, the fd's open mode, 64-bit size, mtime in ns), 12 `mkdir`, 13 `rmdir`, 14 `unlink` (an open file can't be removed: `EBUSY`, FAT's lock), 15 `rename` (`EXDEV` across filesystems, so a `mv` knows to copy), 30 `set_mtime`.
  - Directories: 16 `opendir` (a handle, up to 8 open), 17 `readdir` (the entry's `bmds_stat_t` and its name in the caller's buffer, cut at a character boundary, returning the full length as `snprintf` does), 18 `closedir`.
  - The current directory: 19 `getcwd`, 20 `chdir`: the program's own (it starts as the shell's; the shell's doesn't change). Paths are at most `BMDS_PATH_MAX` (1024) bytes (`ENAMETOOLONG`), names `BMDS_NAME_MAX` (765, FAT's 255 UTF-16 units in UTF-8).
  - Descriptors: 21 `dup(fd, min_fd)` (also `fcntl(F_DUPFD)`), 22 `dup2`. No `pipe` in v1: with no way to start another program it would only deadlock a program on itself (it comes with spawning).
  - Time: 23 `clock_get` (`REALTIME`, `MONOTONIC`, ns), 24 `sleep` (ns; to a deadline on the monotonic clock, so never short: ticks count from mid-tick, and 50 ms first measured 49.1), 25 `interrupted`. The time zone reaches the C library's `localtime` as `TZ` in the environment: the loader adds the kernel's zone when the shell's environment has no `TZ` (no slot).
  - Terminal: 26 `term_get` (`bmds_term_t`: modes, columns, rows, colors), 27 `term_set` (modes): `BMDS_TERM_SIGNALS` (Ctrl-C interrupts), `ONLCR`, `COLORS` (fitting SGR colors), `LOGS` (log lines shown), `LINE` (line input). On a descriptor that isn't the console: `ENOTTY`.
  - Memory and the rest: 28 `mem_info` (the memory `mem_alloc` with the same flags uses, and what the program holds), 29 `random` (the hardware generator; `getentropy`).
- **Added with links** (`storage.md` §4): 32 `symlink(target, path)`, 33 `readlink(path, buf, size)` (NUL-terminated, cut at a character boundary, the full length returned, as `readdir`'s name), 34 `lstat`; `BMDS_TYPE_LINK` 4 (from `lstat` and `readdir`); `BMDS_ELOOP` 40. `open`, `stat`, `opendir`, `chdir` (to the physical path), `set_mtime` and running programs follow links; `unlink`, `rename`, `rmdir` act on the link. The SDK's layer maps `symlinkat`, `readlinkat` (POSIX: no NUL, as much as fits), `statx(AT_SYMLINK_NOFOLLOW)` (`S_IFLNK | 0777`), `O_NOFOLLOW` (`ELOOP`) and `DT_LNK`.
- **Added in step 7:** `bmds_stat_t`'s `file_id` and `device` (48 bytes), 31 `ftruncate(fd, size)`. `file_id` is FNV-1a of the case-folded normalized path (the VFS computes it in `stat`, `readdir` and `open`, which stores it in the `FileHandle`), or for devices, pipes and the console the stream's identity, so every handle of one stream agrees; `device` is the mount slot + 1 (0 outside filesystems). A rename changes the id (POSIX keeps an inode across renames; tools compare ids within a run). `ftruncate` cuts or grows with zeros, keeping the position; FatFs grew files on a seek past the end with whatever its new clusters held, so a position past the end is now a hole that the next write fills with zeros (and a read there is the end), as POSIX has it.
- **Line input** (`BMDS_TERM_LINE`, on by default): reading the console gets whole lines, echoed and editable, from the shell's own line editor (`LineEditor`, allocated in PSRAM on the first such read, with its own history): what was typed after the line in the same read is kept for the next one, so a CR LF pair or fast typing loses nothing; Ctrl-D on an empty line is end of input; with `LINE` off the program gets bytes as they arrive (after anything left over). The loader saves the console's modes before a program starts and restores them when it ends; `LINE` is the program's own.
- **Ctrl-C** (as in §5): blocking calls return `EINTR` from then on; the SDK's `_exit` makes the status 130 if `interrupted()`, however the program ends (as for a command killed by `SIGINT`).
- Every struct starts with `uint32_t size`; unknown flags fail with `BMDS_EINVAL`; a bad pointer from a program is its own problem (no protection), but the kernel validates lengths and handles.
- **ABI lock** (`components/bmds_loader/src/abilock.c`): `_Static_assert`s on slot numbers, struct sizes and offsets (layouts with pointers checked where pointers are 32-bit), error numbers, flag values and format constants, compiled in every firmware build (`main` requires `bmds_loader`) and the host tests. Append-only.

## 5. Running a program

- **Lookup** (step 5): a word with a `/` is a path; otherwise functions, then built-ins (as before), then each directory in `$PATH` for `name`, `name.elf` and `name.sh`, in that order within a directory (D6); the first regular file wins, and an empty entry is the current directory (POSIX). The default is `PATH=/sd0/p0/bin` (exported). Not found stays `name: command not found` (127). `argv[0]` is the name as typed; the launcher gets the path found. `type` says `hello is /sd0/p0/bin/hello.elf` (`-t`: `file`), `command -v` prints the path, and `command name` runs a built-in or a file, skipping functions. No hash table yet: a lookup is up to three `stat`s per directory, a few milliseconds on the card next to a launch's 20+ ms.
- **Launch by content** (`Executor::runFile`, used for every file a command names, so scripts and programs are interchangeable wherever a command goes): the first 256 bytes decide.
  - `\x7fELF`: the launcher (it does the full check of the BMDS note and reports its own refusals).
  - `#!` (D7): the line must end within the 256 bytes (else `interpreter line too long (at most 255 bytes)`, 126; a file with no newline at all is all line). It is trimmed at both ends, so a CR from a PC's CR LF is dropped (Linux keeps it and fails), then split as Linux does: the interpreter up to the first blank, the rest (trimmed) as at most one argument, kept whole (`#!/x -f  two words` passes `-f  two words`). The command run is `interpreter [arg] path original-args...` (the interpreter's `argv[0]` is the interpreter as written, or the built-in's or command's name when one stands in for a missing path; then the script's path as found), through `runFile` again, at most 4 interpreters deep (`path: too many levels of interpreters`, 126). An empty interpreter is `cannot execute: no interpreter after #!` (126).
  - *Superseded 2026-10-05 (§17): nothing is guessed any more; the interpreter must exist (a file or a built-in in `/bin`, `#!/bin/sh`, `#!/bin/env name`).* Interpreter resolution: an existing file is run as such (a program or another script). Scripts written on a PC say `#!/bin/sh` or `#!/usr/bin/env name`, paths BMDS doesn't have, so if the path doesn't exist: `.../env name` looks `name` up like a command (built-ins, then `$PATH`; not found: `name: command not found`, 127); otherwise its last component names a built-in (`bash` meaning `sh`), which runs (`#!/bin/sh -e` is `sh -e path args...`: `sh` takes `set`'s `-Ceux` options for this). Else `path: interpreter: No such file or directory` (127, as bash 5.2).
  - A file ending in `.sh` without `#!` is a shell script: the built-in `sh` runs it, as if it started with `#!/bin/sh`. FAT has no execute bit, so the name is the only other sign of what a file is; BMDS has no `sh` program to point a `#!` at, so scripts shouldn't need one.
  - Anything else is `path: cannot execute: not a program or script` (126). Unlike bash, a text file without `#!` and without `.sh` is not run as a script (decided 2026-10-04): without an execute bit, BMDS can't tell a script from any other text file.
  - `test -x` is: directories, and files that would run (BMDS ELF, `#!`, `*.sh`); `./script.sh` works with no `sh` in front (`storage.md` §5 promised this for Phase 3).
  - These rules are frozen with the first release, like the other shell interfaces.
- **Process** (as built in step 3): the shell hands a command whose name has a `/` to its `ProgramLauncher` (an interface in `bmds_kernel`, so neither the shell nor the loader depends on the other; `Shell::Startup::launcher`, set in `main` to a stateless `constinit Loader`). The `Loader` stats and opens the file, runs `ProgramImage::inspect()`, allocates the image (`Psram`, 4096-aligned, whole pages), `load()`s and relocates it in place, and syncs the caches (`esp_cache_msync(C2M | INVALIDATE)`). A `Process` gets a copy of the command's fd table (fds 0-2 and redirections) and the argument block (`argv`, the exported variables as `envp`, in one allocation). The program's task (FreeRTOS name from the file name, the caller's priority) has the stack its note asks for (default 32 KB), in internal RAM when 64 KB would still be left free, else `Psram`. The caller waits on a semaphore, as for a built-in; in a pipeline a program stage runs like any other (several programs at once work: `hello | cat | cat | wc`).
- **System calls find their program** through FreeRTOS thread-local pointer 1 of the program's task (`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2`; pointer 0 belongs to IDF's pthreads), set before `_start` runs: a `Running` (its `Process`, entry point, exit status, done semaphore). The slots are C functions (`syscalls.cpp`) in a C table in flash (`systable.c`: a C initializer may cast function pointers; a C++ one would need a global constructor).
- **Exit:** returning from `_start` (`main`'s caller does `exit()`) or `exit(n)` records the status, releases the semaphore and suspends the task; the loader then deletes it with the new `Task::stop()` (suspend, wait until off both cores, `vTaskDelete`, then free stack and TCB, as IDF's `vTaskDeleteWithCaps()`), `release()`s the process (fds closed, every allocation freed) and frees the image; the status (& 0xff) becomes `$?`. Problems are reported on the command's stderr as `path: reason` (`No such file or directory`: 127; `Is a directory`, `cannot execute: not an ELF file` and every other format refusal: 126). A crash (illegal instruction, load fault) is IDF's panic and a reboot for now (no protection, as on DOS; a fault hook is an open question).
- **Ctrl-C:** the console's interrupt is raised as now: blocking calls (`read` on the console or a pipe, `sleep`) return `BMDS_EINTR`, `interrupted()` turns true, and the SDK's default makes `main`'s caller exit with 130. A program that ignores it is **killed** on a second Ctrl-C while it runs (the `Interrupt` counts raises since it was cleared; if it was already raised at launch, the next one kills): status 130. Never inside a kernel call, so no kernel lock is left held: every slot runs inside a `KernelCall` that sets the `Running`'s `inKernel` flag and clears it on the way out. The loader, waiting on the done semaphore or the next raise (no polling), sets `kill`, suspends the task until it is off both cores and reads `inKernel`: clear, the task is in program code and is deleted (`Task::stop`, as after an exit); set, it is resumed and ends itself as it leaves the call. The two flags are a Dekker handshake (sequentially consistent atomics), so one side always sees the other. Measured: 50 ms from the second Ctrl-C to the prompt, for a program computing and for one making system calls in a loop, alone or in a pipeline; free memory afterwards exactly as before.
- **Resources** are owned by the `Process` (allocations, fds; directories with step 4), so cleanup after a kill is the same as after an exit. Each allocation carries a 16-byte header (list links, size, offset back to the raw block) in front, inside padding that keeps the program's pointer aligned (16 bytes by default, up to 4096 on request).
- **Console input:** line input with echo and editing by default; raw bytes with `term_set` (§4).

## 6. The SDK (`sdk/`)

```
sdk/include/bmds/*.h      the ABI (C99): sys.h (the table, startup contract), program.h (the format, the note), errno.h
sdk/libc/musl/            musl 1.2.5 with BMDS's patches (sdk/libc/README.md)
sdk/crt/start.c           _start: argv/envp/auxv for musl, the PT_TLS entry, _DYNAMIC
sdk/crt/linux.c           the Linux system calls musl makes, on the BMDS table
sdk/ld/bmds.ld            the layout: one PT_LOAD RWX at 0, the note (stack size from --defsym), TLS
sdk/cmake/                bmds-toolchain.cmake, BmdsProgram.cmake (bmds_add_program), BmdsLibc.cmake, bmds-cc.in
sdk/examples/             hello, cat (raw calls, files), ls (portable POSIX: opendir, stat, localtime)
tools/elfcheck/           bmds-elfcheck (built with the host tests)
tools/bmds-run            push-and-run (Python, pyserial; bmdsboard.py: the serial side, YMODEM)
tools/bmds-compat         runs the compatibility corpus (tests/compat/v*) on a board
```

- Building a program: `cmake -DCMAKE_TOOLCHAIN_FILE=sdk/cmake/bmds-toolchain.cmake` with IDF's `riscv32-esp-elf-gcc` (no IDF needed otherwise); see `sdk/README.md`. (A Makefile template can follow if anyone wants one.)
- Two traps found on the way: the bare-metal GCC driver silently drops `-static-pie`, and CMake's Generic platform ignores `CMAKE_EXECUTABLE_SUFFIX` (the helper sets `.elf` per target).
- `bmds-elfcheck` runs `ProgramImage::inspect()`, the loader's own check, and prints what the loader would do (`hello.elf: BMDS program, ABI 1: image 11520 bytes (11364 in the file), entry +0x0, stack 0 (default), 70 relocations, 4 bytes thread-local`).
- **The RAM disk** (step 6): `ram0`, mounted at `/ram0/p0` at boot (`RamDisk::init`, a boot stage in `bmds_storage`; `CONFIG_BMDS_RAMDISK_MB`, default 16, 0 for none). `RamBlockDevice` became sparse for it: the sectors live in 32 KB chunks allocated from `Psram` when first written with anything but zeros, and a trim zeroes its sectors and frees any chunk left all zeros (FatFs trims a deleted file's clusters; zeroing the partly covered chunks matters, since a file's first and last chunks are shared with its neighbors: without it, deleting three programs gave back 98 of 231 KB). So the size is a limit, not an allocation: an empty 16 MB RAM disk costs 34 KB (the chunk table and one chunk of filesystem metadata), and memory comes back exactly when files are deleted. Writing more than PSRAM has left is a disk error (`NoMem`). The host tests got faster for it too (64 MB test disks no longer zero-fill).
- **Push-and-run** (step 6): `tools/bmds-run [-p port] [-d dir] [-f file]... [-n] [-t secs] prog [args]` sends the program (and any `-f` files) with YMODEM into `/ram0/p0` and runs it there by path, its output on stdout (CR LF turned to LF when stdout isn't a terminal), and exits with its status (124 on `-t` timeout, 255 if the board can't be reached). On a terminal, keys go to the program raw (Ctrl-C included; Ctrl-] stops it and leaves). The card is not needed and nothing is flashed: about 1 s for `hello` (35 KB), 1.6 s for `progtest` (131 KB), about 100 KB/s (lrzsz managed 47). The tool's own YMODEM sender (`tools/bmdsboard.py`, CRC-16, 1 KB blocks) talks to the shell's `rz`, so lrzsz isn't needed. The shell's echo and prompt are never parsed: each command line is bracketed by `echo` markers written with an empty `""` in the middle, so the echoed line doesn't match; when a Ctrl-C ends the program the shell abandons the rest of the line (as bash) and says `^C`, which then ends the run with 130.
- **Compatibility corpus** (step 6, `docs/ABI.md`): `tests/compat/v1/` keeps `hello.elf`, `cat.elf` (raw calls) and `progtest.elf` (musl over every slot) exactly as built, with `cases.txt` (20 command lines, their console output and status: arguments, statuses, environment, files, directories, cwd, sleep, memory, random, terminal calls on non-terminals, line input from a pipe, pipelines, errors). The host tests check that each still passes the loader's check and relocates; `tools/bmds-compat` runs the cases on a board (5 s; `-u` prints the board's results for writing new cases). Never rebuilt or edited once shipped: a new ABI version adds `v2/`.

## 7. Phasing

| Step | Content |
|---|---|
| 1 | **Spike:** code copied into PSRAM runs (cache sync, PMP as analyzed); measure PSRAM vs internal code speed. **Done** (§2): PSRAM confirmed. |
| 2 | SDK skeleton: headers, crt0, linker script, toolchain file; `hello` builds on the PC; `bmds-elfcheck`; ABI lock tests. **Done** (format revised, D2). |
| 3 | Loader (`bmds_loader`): validate, load, relocate, sync, run in a task, exit status, cleanup; run by path from the shell. Host tests of the ELF validation and relocation. **Done.** |
| 4 | Syscall table v1 and the C library's glue: fd I/O, files, directories, memory, time, env, terminal; examples `hello`, `cat`, `ls`-like. **Done.** |
| 5 | Shell: `$PATH` search, programs in pipelines and redirections, Ctrl-C and kill; `exec` dispatch by content with `#!` scripts (built-in `sh` as the interpreter first; programs as interpreters as soon as they run). **Done.** |
| 6 | RAM disk and push-and-run (`tools/bmds-run`); compatibility corpus. **Done.** |
| 7 | The SDK on static musl (§12): vendored musl, the Linux system call layer, file identity and `ftruncate`; examples and `progtest` on musl. **Done.** |
| 8+ | Process creation (`vfork`/`posix_spawn`, `waitpid`, `pipe`), signals, `termios`, `poll`; BusyBox as the yardstick. A shared runtime only if measurements ask for it. **Done:** 8 (§13): processes, `vfork`/`exec`, `posix_spawn`, `waitpid`, `pipe`; `/bin/sh` and scripts in `exec`; 9 (§15): signals; 10 (§16): `poll`, `termios`. |

## 8. Resource budget (estimates)

| Item | Flash | RAM |
|---|---|---|
| Loader (ELF checks, relocation, task) | ~3 KB (`ProgramImage` in step 2; linked in step 3) | per program: image in `Psram`, stack (internal if it fits), ~200 B bookkeeping |
| Syscall table v1 + glue to the VFS | ~6 KB | table in flash (rodata) |
| RAM disk | 2 KB (measured with step 6) | 144 B static; chunk table 2 KB + 32 KB chunks in `Psram` as written (34 KB empty) |
| **Total** | **~10 KB** | **no static internal RAM** |

## 9. Testing

- **Host:** `ProgramImage` on hand-built ELF files (each refusal: not ELF, wrong machine/type/ABI, no note, newer ABI, too much stack, unknown flags, interpreter, dynamic, two segments, not at 0, entry outside, stripped, REL, unknown relocation kind, relocation outside its section, bad symbol, PC-relative to an absolute symbol) and their relocation (word, upper immediate with wrap-around, absolute and undefined symbols left alone, bss zeroed, unaligned base refused); the SDK's `hello.elf`, cross-built as an external project when the compiler is found, loaded at 0 (equal to the file's segment) and at two other bases (exactly the patched words differ, by the distance); 3,000 randomly damaged copies refused or loaded without a crash under ASan/UBSan; `bmds-elfcheck hello.elf` as a ctest.
- **Host (step 3):** `Process` (alignment 0-4096, flags, `realloc` keeping contents, unknown flags and bad alignments refused, `release()` freeing everything, checked against the host allocator; fds, EOF, `EBADF`, `ESPIPE`, closing only the program's copy; the argument block; error numbers); the shell's side with a fake launcher (path, arguments, exported environment only, prefix assignments, cwd, pipelines and redirections, 127/126, no launcher).
- **Board (step 3):** `hello`, `cat`, `progtest` pushed with `rz`: arguments with spaces, `return n` and `exit(n)` from 20 calls deep, the environment (exported and `X=y cmd`), `isatty` on a terminal, a pipe and `/dev/null`, floating point, pipelines of three programs, redirections both ways, relative paths (`bin/hello.elf`), the errors above, Ctrl-C while `cat` reads (status 130). A program that leaks 4 MB, or exhausts PSRAM (31 MB, then `NULL`), leaves free memory exactly as it was. The first leak test found the allocation header storing the wrong offset (the loader freed the program's pointer, not the block): the host tests now catch it.
- **Host (step 4):** every new call on two RAM disks: refused paths and flags (`EFAULT`, `ENOENT`, `ENAMETOOLONG`, `EINVAL`), lowest-free fds, `fstat` of files and devices, a caller's struct smaller than the kernel's (only what fits is written), `EEXIST`/`ENOTEMPTY`/`EISDIR`/`ENOTDIR`/`EBUSY`/`EXDEV`, relative paths after `chdir`, `getcwd`'s `ERANGE`, `readdir` names cut (also inside a UTF-8 character), the directory limit and `release()` closing them, `set_mtime`, `dup`/`dup2`; console line input on a fake console (echo, backspace, a small read taking a line in pieces, CR LF, Ctrl-D), raw input after typeahead, the terminal calls, `ENOTTY`, Ctrl-C ending reads and sleeps; clocks, sleeping, `mem_info`; the launch request carrying the console. A new check that the SDK's programs point `tp` at their TLS segment (below).
- **Board (step 4):** `progtest files` (create, append, seek, `dup`/`dup2` sharing the position, stdio's `fopen`/`fgets`/`fprintf`, rename, directories and every error above, `/dev/null`, the directory limit), `time` (with the kernel's `TZ=UTC0` and `TZ=EST5EDT,...` giving EDT; a 50 ms sleep measured 50 ms), `term` on a terminal, a pipe and `/dev/null`, `mem`, `random`, `cwd` (the shell's cwd unchanged); `ls -l` of the card and `/dev`; `cat` of files and stdin, its errors, in a pipe; typed line input with backspace and arrow-key edits; raw keys (Ctrl-C and arrows as bytes) and the modes back afterwards; Ctrl-C during a line read and during a program's 5 s sleep (50 ms to the prompt). The first `ls -l` crashed: the linker script put `__tls_base` (where crt0 points `tp`) on a 16-byte boundary, but when a program has no initialized thread-locals the linker drops the empty `.tdata` and the TLS segment starts at `.tbss`, 12 bytes lower; `localtime`'s thread-local buffer then overwrote `bmds_sys`. Three of the four programs had it; `__tls_base` is now the segment's start either way, and the host test reads each program's symbol and `PT_TLS` to check.
- **Host (step 5):** the shell on a RAM disk with stand-in programs (files starting with the ELF magic) and a fake launcher: `$PATH` order (`name` before `name.elf` before `name.sh`, directories in order, built-ins and functions first, an empty entry), `argv[0]` as typed and the path found, `type`/`command -v`/`command name`; `.sh` scripts in a subshell with their arguments; `#!` with `/bin/sh`, `/bin/bash -e`, `/usr/bin/env sh`, `/usr/bin/env program` (with a CR LF line), a program as interpreter with its argument kept whole, a script found on `$PATH`, scripts nested as interpreters, and each refusal (missing interpreter, empty, too long, a loop); `test -x`; the launcher's errors. `Interrupt::raises()` and waiting for the next raise.
- **Board (step 5):** `hello`, `progtest`, `ls.elf -l` by name; `type`; `#!/bin/sh -e` (stops at `false`), `#!/sd0/p0/bin/cat.elf` (prints the script), `#!/usr/bin/env progtest`, `.sh` by path and on `$PATH`, the refusals, `test -x`, scripts in pipes and redirections. The kill: `progtest spin` (computing) and `progtest calls` (system calls in a loop) survive one Ctrl-C and die on the second, alone and as `progtest spin | cat.elf`, with the task gone and free memory unchanged; one Ctrl-C still ends a sleep or a read.
- **Host (step 6):** `RamBlockDevice` sparse: a 512 MB device costing only its table, zeros not allocating, writes across chunk boundaries, trims in pieces and whole, a short last chunk, no leaks; the VFS tests' leak checks now leave out the disk's chunks. The corpus programs pass the loader's check and relocate.
- **Board (step 6):** the RAM disk at boot (`df`: 16 MB), memory back exactly after deleting what `bmds-run` pushed; `bmds-run` with arguments, statuses, `-t` (killed, 124), `-n`, `-f`, output into a pipe, and on a pseudo-terminal (`script`): a typed line to `progtest lines`, Ctrl-C ending a sleep, two killing `progtest spin` (130 both). `bmds-compat`: all 19 cases pass, and changed expectations are reported as failures.
- **Host (step 7):** holes and `truncate` on FAT32 and exFAT (a seek past the end leaves the size; a write there leaves zeros, not old clusters; cut and grown with the position kept; only writable files), file identity (case within a volume, `.` and `..`, relative paths, handles, directory entries, devices), `Process::ftruncate` and the stat fields (a caller's smaller struct untouched); the TLS test checks every program with thread-locals (`progtest` has both kinds).
- **Board (step 7):** the SDK examples and every `progtest` test on musl, the new `posix` test on the RAM disk (FAT), the card (exFAT) and with stdin redirected (`ftruncate`, holes, `pread`, identity by name/fd/`readdir`/case, `access(X_OK)`, `tcsetattr` raw and back, signals raised and blocked, `uname`, thread-locals), `abort` (134), line input, raw keys, Ctrl-C and the second-Ctrl-C kill (no leaks); the corpus; BusyBox built with `bmds-cc`: `ls -la`/`-i`, `awk`, `sort`, `sed`, `tar`+`gzip` round trip, `cp x x` ("the same file"), `test -x`, `du`, `find`, `date`, `uname`.
- **Measured (step 4):** the image grew 5.7 KB (405.8 → 411.6 KB; `bmds_loader` 6.5 → 11.2 KB), no static RAM; a program's line editor is 2.3 KB of PSRAM when it reads the console. Programs got bigger with what they use: `progtest` 45 KB of image (stdio, `localtime`, `strftime`, `scanf`), `ls` 34 KB, `cat` 5 KB. Launch plus run, 10 runs averaged: `progtest` 60 ms, `ls` 45 ms, `cat` 20 ms. Their files are mostly relocations: `progtest.elf` is 130 KB with 4,100 of them, of which the loader patches 617 (`R_RISCV_32`, `HI20`) and checks 181 (PC-relative); the rest (branches, calls, `RELAX`, `TPREL`) are kept only by `--emit-relocs`. Filtering them out at build time (a small post-link step in the SDK) would cut files to about a third and launches with them; with the single relocation pass (step 3), the next launch-time items.
- **Measured (step 3):** launching (stat, open, check, load, relocate, cache sync, task start; the program's run excluded) `cat` 10 ms, `progtest` (13 KB image, 84 patches) 23 ms: first 110 ms, until the loader read each section header, each relocation chunk and each symbol separately (0.4 ms a read from the card) and the 143 KB symbol table of picolibc's statics was in the file. Now the section headers, each relocation section and the symbol table are read whole, the SDK strips what no relocation needs (`objcopy --strip-unneeded`: `progtest.elf` 518 → 40 KB, `name.debug.elf` keeps everything), and the segment starts on a sector boundary (`-z max-page-size=512`). Checking and loading still walk the relocations twice (about 8 and 11 ms): one pass would halve that. The image grew 9.1 KB (396.8 → 405.8 KB; `bmds_loader` 6.5 KB); internal RAM 8 bytes per task (the second thread-local pointer).

## 10. ABI impact

Everything in §4 and §6's headers, the `exec` rules (§13: `#!` as the shell's, `/bin/sh` the shell, `.sh` files), the ELF profile in D2 (note format, the patched and the accepted relocation kinds, the 4096-byte load alignment, entry signature, startup state), the stack and memory guarantees, the `$PATH` lookup and the `.elf`/`.sh` order, the `#!` rules (line length, one optional argument, argument order, nesting depth, the `/bin/sh` and `/usr/bin/env` mapping), `.sh` files run by `sh` without `#!`, other text files refused, `test -x`, exit statuses 126/127/130. Each lands with an `ABI:` line.

## 11. Open questions

- A fault hook that turns a program's crash into a status instead of a reboot (IDF's panic handler is not designed for resuming).
- The framebuffer restored at exit (Phase 6), as the console's modes are now.
- Two programs in one pipeline that both change the console's modes each restore what they saw at their start: the later one to end wins. Rare; a per-job save in the shell would fix it if it matters.
- **`stat` of a file open for writing** shows the size FatFs last wrote to its directory entry (at a flush or close); `fstat` is current. Syncing open files on `stat` would need the VFS to find them by name. FatFs also refuses a second open of a file open for writing.
- **The system call layer's size:** about 10 KB in every program, since musl's one entry point (`__bmds_syscall`) reaches every handler. Linking only the handlers a program uses: `syscall_arch.h` could call a function per constant system call number (`__bmds_sys_` pasted to the name), the switch staying for calls by variable number.
- **Program files carry every relocation** `--emit-relocs` keeps, the PC-relative pairs the loader skips included: `hello.elf` is 90 KB on the card for a 43 KB image. Dropping what the loader doesn't need at link time (a post-link step) halves files and load reads.
- Dual-core: run programs on the second core by default to keep the shell and services responsive?
- Zc* opt-in for chip revisions ≥ 3 (D5).
- **Programs bigger than memory.** Demand paging (mapping parts of a program from the SD card as it touches them) needs a fault the CPU can restart, and the P4 has none: its MMU maps external memory in 64 KB pages, but a miss is reported as an asynchronous cache-error interrupt that IDF turns into a panic, not a precise exception on the faulting instruction. What would work, if programs ever outgrow 32 MB of PSRAM: **overlays** (DOS-style; sections loaded on first call through linker-generated stubs), a later, opt-in addition to the format. Not needed now: loading is cheap (the card reads 16 MB/s, so a 1 MB program loads in about 65 ms).
- **Keeping recently run programs loaded** (an image cache in spare PSRAM, checked against the file's size and time) would make repeated runs instant; cheap to add after step 3.
- **Shared libraries: Amiga-style, not ELF** (agreed 2026-10-04). ELF shared objects would make every exported libc function and struct ABI, need position-independent code (the toolchain's picolibc isn't, the reason static PIE was dropped in step 2), add symbol binding to every launch and complicate thread-local storage across modules; and the RAM they save is small: libraries are linked only for what a program uses (a `printf` hello is 11.5 KB of image, `progtest` 45 KB), code lives in PSRAM, and only pipeline stages run at once (five `progtest`-sized stages duplicate about 200 KB of 32 MB). Code can't be shared between two runs of one program either: its absolute addresses are patched into the code. When a large shared component arrives (a GUI toolkit, TLS, a compiler's runtime), a library is an image loaded and relocated once, opened by name and minimum version through a system call, returning a table of functions as `bmds_sys` does: calls through the table, so programs need no PIC, no GOT and no binding at load; reference-counted, freed when the last user closes it; its ABI is its table, versioned and append-only like the syscall table. Library code must be reentrant and keep per-caller state in a handle (never globals). Kernel services (the line editor, regex, glob) can be served the same way from the firmware at no cost to programs.
- **A C runtime in the firmware** (agreed 2026-10-04; the plan is §12), as such a library: the stateless parts of libc only (string and memory functions, `ctype`, `strtol`/`strtod`, `qsort`/`bsearch`, the formatting core `vsnprintf` to a buffer, calendar conversions, libm): behavior fixed by the C standard, no struct layouts exposed, and most of the bytes (`printf`, float conversion, `pow`/`sin`). Stateful parts stay in the program or move to per-`Process` state behind BMDS's own interface (`FILE` and its buffers, `errno`, `malloc`, `atexit`, `environ`, `rand`, locale): shipping picolibc's `FILE` layout and the `getc`/`putc` macros that read it would freeze picolibc's internals. Gains: a `printf` hello under 1 KB (smaller files, fewer relocations, faster launches), the same code lines in the cache across pipeline stages and repeated runs, and above all self-hosting: an on-device TinyCC-class compiler links against a small stub, not a libc archive. Costs: a fix in the shared code reaches old programs, so changed behavior takes a new table entry, never an edit (the syscall table's rule); 30-60 KB more firmware flash, replacing copies in every program. Placement as on the Amiga: the core runtime in firmware (always there, no load, run from flash through the cache), larger optional libraries loaded from the card on first open and kept while used. No compatibility path for statically linked picolibc programs (decided 2026-10-04): there is no body of them to keep working, so the runtime replaces static picolibc outright. That holds only while nothing has shipped: the switch belongs before the first release, or static programs become userspace like everything else. When: after the library mechanism exists, before the first release.

## 12. Direction: a POSIX C runtime (planned, after step 6)

Agreed 2026-10-04: the C library moves out of programs into a shared runtime library in the firmware (§11), and its target is **POSIX source compatibility**: POSIX utilities compile for BMDS with few or no changes. Not binary compatibility with any other system. Dropping static picolibc (no compatibility path, §11) is what makes this practical: the SDK owns its C headers outright, so struct layouts (`FILE`, `struct stat`, `struct termios`...) are ours to define and freeze, not picolibc's internals.

**Yardstick:** BusyBox and Toybox both build for MMU-less Linux (BusyBox's `BB_MMU=n`: no `fork()`, `vfork()` + `exec` instead). "BusyBox (or Toybox) built for no-MMU runs on BMDS" is the concrete, testable meaning of "POSIX enough", and a corpus of real utilities for the compatibility tests.

**Mapping POSIX onto BMDS:**

| Area | Plan |
|---|---|
| Files, directories, `stat`, `getopt`, regex, time, environment | Mostly there (step 4) or straightforward. FAT has no permissions: modes are synthesized as `stat` does now, every user is root (`getuid() == 0`), `chmod` changes only the read-only attribute. |
| Running programs | No `fork()`: no address-space copy, and a program's absolute addresses are patched into its code. Instead `posix_spawn()`, and `vfork()` + `exec` (the parent waits until the child execs or exits, as on uClinux), with `waitpid` and `pipe` (deferred from v1 in step 4: it only makes sense with spawning). The shell's pipelines can then run on the same calls. |
| Signals | Delivered at system-call boundaries, so a handler never runs in the middle of a kernel call; a program stuck computing is still killed (step 5's second Ctrl-C). `SIGINT`, `SIGTERM`, `SIGPIPE`, `SIGCHLD`, `SIGWINCH`; `sigaction`, masks, `kill` between processes. Replaces the step 4 rule (`EINTR` until exit, status 130). |
| `termios` | `ICANON`, `ECHO`, `ISIG`, `OPOST`/`ONLCR`, `VMIN`/`VTIME` over the terminal modes; echo becomes its own mode (today line input always echoes, raw input never does); window size through `ioctl(TIOCGWINSZ)`. |
| `poll`/`select` | Needs readiness events from consoles and pipes: a kernel addition (wait on several streams), useful beyond programs. |
| `mmap` | Anonymous memory, and private file mappings (copied in). No sharing between processes, no demand paging (§11). |
| Threads | Not at first; `pthread` later if wanted (per-thread `errno` and TLS then matter). |

**Where the code comes from:** a spike compares two bases before committing to either.
- **musl** (MIT): the most complete and cleanest POSIX libc, written against Linux system calls through a small internal layer. Precedent: wasi-libc runs musl's upper half over WASI, a non-Linux system interface. We would write that layer over the BMDS table. Leaning this way for completeness.
- **picolibc** (BSD, in the toolchain): small, but thin on POSIX (no spawn, wait, `poll`, `termios` to speak of): most of the interesting parts would be ours to write.
- The spike: build the libc over a stub layer, count what must be written or replaced, measure the size of the shared part and of a program's stub, build a handful of BusyBox applets against it.

**Spike results (2026-10-04; scratch code, nothing committed but this):**
- **musl 1.2.5 builds** for rv32imafc/ilp32f with IDF's GCC 15.2 (its configure knows the `riscv32-sp` variant): one patch, `setjmp`/`longjmp` saving `fs0-fs11` with `fsw`/`flw` instead of `fsd`/`fld` when `__riscv_flen == 32`.
- **BusyBox 1.37 builds unmodified** against it: about 90 applets with `CONFIG_NOMMU` (coreutils, `sed`, `awk`, `diff`, `patch`, `vi`, `find`, `grep`, `xargs`, `tar`, `gzip`, `bunzip2`, `unzip`, `less`, `hush`), 372 KB of code, static.
- **And runs on the board**, as an ordinary BMDS program pushed with `bmds-run`, over a 400-line shim that turns musl's Linux system calls into BMDS v1 slots (musl's `syscall_arch.h` calls it instead of `ecall`) plus startup glue that builds the argv/envp/auxv array `__libc_start_main` expects (with a `PT_TLS` entry from the linker script). Working: `ls -l`, `find`, `sort`, `tr`, `sed`, `awk` (floating point), `diff -u`, `cmp`, `du`, `md5sum`, `sha256sum`, `date` (musl reads the kernel's `TZ`), `uname`, `printf`, `tar` and `gzip`/`zcat` (round trip checked by `md5sum`), and pipelines of several BusyBox processes. Free memory afterwards as before. `hush` runs its own commands and stops at `waitpid`/`pipe` (not implemented: the expected gap). One applet in a pipeline takes about 60 ms; the 384 KB image has only **217 relocations** (musl and BusyBox are PC-relative throughout; `progtest`, built against the toolchain's picolibc, has 4,100).
- **picolibc**, for the same BusyBox configuration: 14 missing headers (`features.h`, `poll.h`, `sys/ioctl.h`, `sys/mman.h`, `sys/termios.h`, `sys/utsname.h`, ...), `_FILE_OFFSET_BITS=64` unsupported (32-bit `off_t`), and, with the SDK's glue counted, **95 of the 272 libc functions BusyBox calls missing** (signals and `sigsetjmp`, `termios`, `poll`, `pipe`/`vfork`/`waitpid`/`popen`, `getpwnam` and friends, `glob`, `realpath`, `getline`, `mmap`, `uname`, sockets...). musl: none missing.
- **The system calls musl needs for all of BusyBox: 63** (`openat close read write readv writev _llseek statx getdents64 fcntl64 dup dup3 ioctl getcwd chdir mkdirat unlinkat renameat2 faccessat utimensat readlinkat symlinkat linkat mknodat fchmod(at) fchown(at) umask uname clock_gettime clock_settime brk mmap2 munmap mremap mprotect exit exit_group set_tid_address futex sched_yield clone execve waitid pipe2 ppoll kill tkill rt_sigaction rt_sigprocmask rt_sigreturn getpid getppid getuid geteuid getgid getegid getgroups getpgid setpgid setsid socket ftruncate64`); a printf hello needs 10, `ls -l` 17. Most map onto v1 slots or are constants (one user, root; no links). The real work is the planned list: process creation (`clone(CLONE_VM|CLONE_VFORK)`/`execve`/`waitid` for `vfork` and `posix_spawn`), `pipe2`, signals, `ppoll`, the terminal `ioctl`s, `ftruncate`.
- **Size:** a static musl hello is 29.5 KB against picolibc's 11.3 KB: `long double` is 128-bit on RISC-V and musl's `printf` formats floats through it (about 12 KB of soft quad-float from libgcc). `ls` 51.6 vs 33.7 KB. Of BusyBox's 357 KB of code and rodata, **166 KB is libc and libgcc**; all of musl is 535 KB of code. The app partition is 4 MB with 418 KB used.
- **Per-process state is small:** musl's mutable globals reached by BusyBox are 71 objects, 1.6 KB (`malloc`'s context, the three `FILE`s, `environ`, `getopt`, time zone, signal masks...), plus stdio buffers.
- **Found on the way** (any runtime needs these):
  - **File identity:** POSIX tools compare `st_dev`/`st_ino` (`tar` skipped every file as "the archive" until the shim made them up). `bmds_stat_t` should get a file id (volume and first cluster or directory entry): an appended field, which its `size` allows.
  - **Weak undefined symbols:** musl tests `&_DYNAMIC != 0`; undefined, that is a PC-relative reference to absolute 0, which the loader refuses. Either the loader turns such an `auipc` pair into an absolute `lui` pair, or the SDK defines the symbols.
  - **Whole pages:** `mmap` emulation must hand out whole pages; musl's `malloc` keeps metadata at the end of the last page (a shim that allocated the exact length corrupted the kernel heap: no memory protection).
  - **errno numbering:** BMDS's errno values were newlib's above 34; musl uses Linux's. Resolved when the ABI was reset: they are Linux's now.

**What the spike says about the plan:**
1. **musl, not picolibc.** It already is the POSIX layer §12 wants; with picolibc it would be ours to write.
2. **Keep musl's Linux system-call interface internally**, translated in a layer in the runtime (as the spike's shim), rather than rewriting musl's lower half (wasi-libc's route). musl stays close to upstream, and the layer is never visible to programs (they see musl's POSIX headers). Hot paths can call BMDS slots directly later.
3. **Reconsider the firmware-resident runtime.** Static musl works now, and its ABI is just the system call table (frozen already), where a shared runtime freezes a libc table and musl's struct layouts as well. What sharing would save: about 30 KB per small program and 166 KB for BusyBox, in PSRAM (32 MB) and on the card, plus load time (a 384 KB image loads from the card in about 25 ms). Self-hosting doesn't need it either: an on-device compiler can link against a static `libc.a`. Proposal: static musl first (step 7), the shared runtime later only if measurements ask for it (it doesn't change source compatibility, only binaries, which nothing has shipped).

**Decided (2026-10-04): static musl first.** Programs link musl statically; the shared firmware runtime below waits until measurements ask for it.

**Step 7: the SDK on musl.**
- **musl in the SDK:** musl 1.2.5 vendored in `sdk/libc/musl/` (riscv32 and generic parts only; `README.md` there lists the version, checksum and patches), built by the SDK's CMake (`sdk/cmake/BmdsLibc.cmake`, musl's Makefile rules: arch files replace generic ones of the same name, `mallocng`, per-file flags) into `libc.a` with the program, its headers generated into the build tree (`bits/alltypes.h`, `bits/syscall.h`). Programs compile with `-nostdinc` against them and link `-nostdlib` with `libc.a` and libgcc. picolibc, `crt0.S`, `crt.c`, `posix.c` and `malloc.c` go.
- **Patches to musl:** `setjmp`/`longjmp` for single-precision float registers; `syscall_arch.h` calling `__bmds_syscall()` instead of `ecall`; the other riscv32 assembly that issues `ecall` (`clone`, `syscall_cp`, `__unmapself`) replaced by musl's generic C (`clone` is `ENOSYS` until step 8).
- **`sdk/crt/start.c`:** `_start(sys, argc, argv, envp)` as the loader calls it: keeps the table, builds the argv/envp/auxv array `__libc_start_main` wants (`AT_PHDR` pointing at a `PT_TLS` entry made from the linker script's TLS symbols, `AT_PAGESZ`, `AT_RANDOM`), defines `_DYNAMIC` (musl tests the weak symbol; undefined it is a PC-relative reference to absolute 0, which the loader refuses).
- **`sdk/crt/linux.c`:** the Linux system calls musl makes, on BMDS slots: files, directories (`O_DIRECTORY` and `getdents64` over `opendir`/`readdir`, with `.` and `..`), `statx` (modes synthesized: directories 0755, files 0644 or 0444 if read-only, plus execute bits for `.elf`/`.sh`; `access(X_OK)` by content as `test -x`), clocks and sleeps, `mmap` of anonymous memory in whole pages (a mapping table; partial `munmap` keeps the mapping until exit), `brk` refused (musl then uses `mmap`), the terminal `ioctl`s (`TIOCGWINSZ`; `TCGETS`/`TCSETS` mapping `ICANON`/`ECHO`, `ISIG`, `OPOST`/`ONLCR` onto `term_get`/`term_set`), signal masks and handlers kept (delivered only by `raise`/`kill` of the program itself until signals arrive), `uname`, `sysinfo`, `getrandom`; process creation, `pipe2` and sockets fail with `ENOSYS` until step 8. errno values are translated (BMDS's above 34 are newlib's). `exit_group` keeps step 4's rule: status 130 after a Ctrl-C.
- **Kernel:** `bmds_stat_t` grows `file_id` and `device` (appended; `stat`, `fstat`, `readdir`): a hash of the case-folded absolute path, kept in the `FileHandle` from its open, or the stream's identity for pipes and the console. New slot 31 `ftruncate(fd, size)` (growing writes zeros: FatFs leaves extended clusters uninitialized).
- **Kept:** the system call table and `bmds/*.h` (programs may still use raw calls next to musl: they share the descriptors); the format, the loader, the linker script. `off_t` becomes 64 bits. The SDK examples and `progtest` are rebuilt on musl.
- **Not yet:** C++'s standard library (the toolchain's libstdc++ is built for newlib; C++ without it works); process creation (step 8); signals delivered from outside (Ctrl-C) and `termios` in full, `poll` (step 9 on).

**Step 7 as built:** as planned above, plus what building it found:
- **Section order and TLS:** the linker computes thread-local offsets from the first TLS output section it has. A first version described the TLS segment with words in a section placed before `.tdata` that named `.tdata` and `.tbss`; naming them early reordered the output sections, `.tbss` became first, and every thread-local was 16 bytes off (`progtest posix` caught it). The startup code now gets two end symbols defined after both sections, and takes the alignment from the template's address (musl puts the data at that address modulo the alignment, so a larger one would move it).
- **The toolchain's libraries:** `-nostdlib` doesn't stop the linker searching the toolchain's directories, so `-lm` found newlib's `libm.a` (BusyBox links it). The SDK's library directory (`libc.a`, `libbmds.a`, and musl's empty `libm.a`, `libpthread.a`, `librt.a`...) comes first; `--sysroot` doesn't remove the toolchain's own directories.
- **`main` in an archive:** BusyBox keeps `main` in one of its own archives, searched before `libbmds.a` supplied `_start`; `bmds-cc` names `-lbmds` before the program's files as well.
- **Execute bits:** BusyBox's `test -x` (as root) and shells searching `$PATH` look at `st_mode`, not `access()`: files without an extension get execute bits by their first bytes, `.elf`/`.sh` by name, others none, so `ls -l` doesn't read every file.
- **Directories as descriptors:** `open(dir, O_RDONLY)` succeeds, as on Linux (musl's `opendir` is built on it), so `progtest files` changed its expectation; `readdir` lists `.` and `..`, so the example `ls` hides dot names unless `-a`.
- **Measured:** `hello` 39.8 KB of code (picolibc 11.3), `ls` 62, `progtest` 72; 14 to 28 relocations (picolibc's `progtest` 4,100); BusyBox 384 KB with 216. The layer is 9.7 KB of code. The firmware grew 1.9 KB (identity, holes, `ftruncate`). An SDK build directory compiles musl once (about 1,300 files).

**Shape of the runtime** (as planned before the spike; see point 3 above):
- A library in the sense of §11: an image with a versioned, append-only function table, in the firmware (run from flash through the cache), opened by the SDK's startup code. Programs call through the table: no PIC, no GOT, no binding at load. A `printf` hello world drops from 11.5 KB to under 1 KB.
- Per-process state (`errno`, `FILE` buffers, `atexit` handlers, `environ`, `rand`, locale) lives with the `Process`, found the way system calls find it (the task's thread-local pointer); `errno` is `(*__errno())`. Stateless functions (strings, `ctype`, number conversion, formatting, libm) are shared as they are.
- The SDK ships BMDS's own POSIX headers and a small static stub (startup, the table pointer, inline wrappers); `sdk/crt/posix.c` and the static picolibc link from step 4 go away.
- The ABI that freezes at the first release is then the runtime's table and the SDK headers' layouts, in addition to the system calls; the ABI lock (`abilock.c`) grows to cover them.

**Scope and order:** roughly doubles Phase 3's remaining work (the library mechanism, the libc spike and port, spawn/wait/pipe, signals, `termios`, `poll`). Step 5 (`$PATH`, `#!` scripts, kill) doesn't depend on it and goes first; step 6 (RAM disk, push-and-run) also stays ahead, since it speeds up the testing this needs. All of it before the first release.


## 13. Step 8: process creation (design, 2026-10-04)

**Goal:** programs start programs. Yardstick: BusyBox `hush` runs programs and pipelines, `xargs`, `find -exec`, `env CMD`, `time`, `nohup` work, and musl's `posix_spawn`, `system` and `popen` work unchanged. No `fork()` (§12): `vfork()` + `exec`, as on uClinux, and `posix_spawn()` on top of it.

**The model.** A process is a `Process` (its resources, as now), a pid, and a program image running in a task. Every program gets a pid, shell launches included; the shell itself is pid 1 for its programs' `getppid()` (it can't be waited for or signalled).
- **The process table** (`bmds_loader`, portable, host-tested): 32 entries (pid, parent, job, state, wait status, the `Process` while it runs), 1 KB of `Psram` allocated at boot, under a lock. Pids count up from 2 and skip those in use. A full table is `EAGAIN`.
- **Ending:** the program's own task, in kernel code, releases its `Process` at once (fds closed, so a pipe's reader sees EOF when the writer exits, not when someone waits for it), frees its image, records the status (a zombie: the entry only), wakes the parent's waits, and queues its task to be stopped. Stopping (the TCB and the stack) happens in FreeRTOS's timer service task (`xTimerPendFunctionCall`, no new task), and the queue is drained at every start as well, in case the timer queue was full.
- **Waiting:** a zombie stays until its parent waits for it. Children of the shell (parent 1) are waited for by the launcher. When a parent ends, its zombies go and its living children become orphans, freed as soon as they end.
- **Jobs:** a program the shell starts is a job; its descendants inherit the job. A second Ctrl-C kills every process in the job (each with step 5's handshake), not only the first.

**`vfork`, in the same task.** The child runs on the parent's stack, in its memory, until it execs or exits, while the parent waits; this is what makes it work without per-process address spaces (§2).
- `proc_vfork()`: the kernel makes a child `Process`, with a copy of the fds and their flags, the cwd, the console and the interrupt, plus a new pid in the same job. It becomes the task's current process, at most 2 deep. System calls now act on the child's fds and cwd. Memory calls still act on the program's own `Process`: the memory is shared, so a block the child allocates belongs to the parent's heap.
- `proc_exec(path, argv, envp)` in a child:
  1. loads the image (errors are returned, not printed);
  2. closes the child's close-on-exec fds and builds its argument block (a copy: `argv` points into the parent);
  3. starts the child's task with the image's stack size;
  4. returns to the parent's process, giving the child's pid.
- `proc_exec` outside a child replaces the program. The pid and `Process` stay; the new image gets a new task, the old image is freed, and the old task is queued for stopping.
- `proc_vfork_exit(status)` in a child: the child `Process` is released, it becomes a zombie with that status, and the parent's process returns with its pid.
- **SDK** (`sdk/crt/vfork.S`, `linux.c`):
  - `vfork` saves `ra`, `sp`, `s0`-`s11` and `fs0`-`fs11` in a static buffer per depth, calls the slot, and returns 0. `execve` or `_exit` in the child jumps back to that point with the pid, so `vfork` returns a second time, in the parent.
  - `__clone(fn, stack, CLONE_VM|CLONE_VFORK, arg)` does the same with `fn` on the given stack, so musl's `posix_spawn` (its error pipe included), `system` and `popen` work as written. Other `clone` flags and `fork` are `ENOSYS`.
  - What Linux keeps per process but the SDK keeps in program memory is saved at `vfork` and restored on return: signal dispositions, mask, pending signals, `umask`, the directory-fd table. musl's `posix_spawn` child resets dispositions, which must not reach the parent.
  - Since program threads (processes.md §3.10): the registers go in a heap block per `vfork` (with the depth, thread-local: each thread vforks on its own), and the child works on a copy of those tables instead of the parent's being saved and restored, so the parent's other threads never see its changes.
- **Linux layer:** `execve`, `waitid` (`P_PID`, `P_ALL`; `P_PGID` as `P_ALL`; musl builds `wait4`/`waitpid` on it), `pipe2`, `getpid`/`getppid`, `F_GETFD`/`F_SETFD`, `O_CLOEXEC`, `F_DUPFD_CLOEXEC`.

**New slots** (appended to ABI 1, which hasn't shipped):

| Slot | Call |
|---|---|
| 35 | `int32_t proc_vfork(void)`: 0, now in the child; or `EAGAIN` (table full, too deep), `ENOMEM`. |
| 36 | `int32_t proc_exec(const char *path, char *const argv[], char *const envp[])`: in a child, the child's pid (back in the parent); else does not return. `ENOENT`, `ENOEXEC` (not a BMDS program), `ENOMEM`, `E2BIG`, ... |
| 37 | `int32_t proc_vfork_exit(int32_t status)`: in a child, its pid (back in the parent); else `EINVAL`. |
| 38 | `int32_t proc_wait(int32_t pid, int32_t *status, uint32_t flags)`: the pid of an ended child (`pid` -1: any), 0 with `BMDS_WAIT_NOHANG` if none has ended, `ECHILD`, `EINTR`. `*status` in Linux's layout: the exit status in bits 8-15, or a signal number in bits 0-6 for a killed child (Ctrl-C: `SIGINT`). `BMDS_WAIT_UNTRACED` and `BMDS_WAIT_CONTINUED` (appended, `processes.md` §3.7.1) also report a child that stopped (`0x7f \| sig << 8`) or continued (`0xffff`), once each, without freeing it. |
| 39 | `int32_t pipe(int32_t fds[2], uint32_t flags)`: read end, write end; flags `BMDS_O_CLOEXEC`. |
| 40 | `int32_t proc_id(uint32_t which)`: `BMDS_PROC_SELF` or `BMDS_PROC_PARENT`. |
| 41 | `int32_t fd_flags(int32_t fd, int32_t flags)`: sets the fd's flags (`BMDS_FD_CLOEXEC`), or with -1 only returns them. |

Also `BMDS_O_CLOEXEC` (0x40) for `open`. Kernels that predate these refuse the flag, as they refuse any unknown one.

**Ctrl-C until signals arrive (step 9; replaced by §15):** a job's processes share the console's interrupt. A Ctrl-C makes blocking calls fail with `EINTR` in all of them, `proc_wait` included (step 4's rule), and a second kills the job.

**Not in 8a:** `exec` of anything but a BMDS program is `ENOEXEC` (musl's `execvp` then tries `/bin/sh`). 8b adds `#!` scripts and `/bin/sh` as the BMDS shell running as a process (`system` and `popen` run `/bin/sh -c`). `kill()` between processes waits for signals. The shell's own pipelines keep their worker tasks.

**Sub-steps:** 8a, the kernel, the slots and the SDK, tested with `progtest spawn`; 8b, `/bin/sh` and `#!` in `exec`; 8c, BusyBox with the applets this enables (§6 of `distribution.md`) and `hush` as the measure.

**Budget (estimate):** firmware +5 KB, 1 KB `Psram` (the table), no new task, internal RAM unchanged. Per vfork child, a `Process` (~250 B `Psram`). The SDK adds about 1 KB of code to programs that use `vfork` or `posix_spawn`; what it saves at a vfork goes on the heap while the child runs.

**Testing:**
- **Host:** the table (pids, wait for one or any, `NOHANG`, `ECHILD`, zombies freed by waiting and by the parent ending, orphans, a full table); `Process` copies for a child (fds shared with refcounts, close-on-exec closed at exec, cwd separate); `pipe` and `fd_flags`.
- **Board:** `progtest spawn`:
  - `vfork`+`execve` with the child's `dup2`/`close`/`chdir` not reaching the parent;
  - `posix_spawn` with file actions, and its error for a missing program;
  - `system()` failing cleanly until 8b;
  - `waitpid` for one, for any and with `WNOHANG`, and exit statuses;
  - a pipe between two spawned children, with EOF when the writer exits;
  - close-on-exec;
  - `execve` replacing the program (the shell sees the new program's status);
  - a child that `_exit`s without exec;
  - a parent that exits before its children;
  - Ctrl-C killing a job of three;
  - free memory and tasks unchanged afterwards.

**8a as built** (2026-10-04): as designed above, plus what building it found.
- **Task handles:** `Task::start()` stored the handle when the create call returned. A child that starts on the other core can end, and be stopped, before then: the stop saw no handle and the task stayed suspended, with its stack (32 KB of internal RAM each). A static task's handle is its TCB, now known before the task exists.
- **`exec` never returns** when it replaces a program, so nothing that owns memory may still be alive at that point: its argument `List`s leaked 64 bytes an `exec` until scoped.
- **`setArguments()`** freed the old argument block before copying the new one; an `exec` passing its own `argv` on (`exec "$@"`) needs it kept until the copy is made.
- **The stop queue** must not run while the ending task holds a lock: a debug line in it (the log's lock) hung the console when the drain deleted the task inside it. The ending task now holds nothing from queueing itself to suspending.
- **musl:** its generic `vfork.c` (a `fork`) and `clone.c` (`ENOSYS`) are left out of `libc.a`; the SDK's `vfork.S` has both. Its `posix_spawn`, `system` and `popen` are unchanged. `posix_spawn` keeps a 5 KB child stack in its own frame, well within the 32 KB default.
- **Verified on the board:**
  - `progtest spawn` (vfork + exec, vfork + `_exit`, a failing exec, `posix_spawn` with file actions and its `ENOENT`, `getppid`, a pipe and EOF, two children joined by a pipe and waited for in either order, `WNOHANG`, `ECHILD`, close-on-exec set three ways, a child that replaces itself) passes, with output to the console, a file or a pipe;
  - `exec` replacing the shell's program gives the shell the new program's status (42);
  - an orphan outlives its parent and is freed when it ends;
  - two Ctrl-Cs kill a job of three spinning processes;
  - 40 runs of `progtest spawn` (about 640 processes) and 40 replacing execs leave free memory and the task list exactly as they were;
  - BusyBox rebuilt: `hush -c` runs programs, its own pipelines (`for ... | sort -r | tr`), `$(...)`, `&&`/`||` and exit statuses, and `env VAR=x program` runs it;
  - the corpus passes.
- **Host:** the process table (ids, waiting for one or any, zombies, orphans, a full table, ids skipping those in use); a vfork child's `Process` (copied fds and flags, its own `dup2`/`close`/`chdir`); `pipe`, `fd_flags`, `O_CLOEXEC`, `prepareExec()` (arguments from the old block and from the heap, memory freed, close-on-exec fds closed); `FdTable`'s flags.
- **Measured:** the firmware grew 6.1 KB (428.9 → 435.1 KB); 1 KB of `Psram` for the table, internal RAM unchanged at rest. The SDK's system call layer is 9.3 KB of code (`hello` 41.9 KB); `vfork.S` (0.5 KB) is linked only into programs that start others; the saved registers are 208 B of `bss`, and the state saved at a vfork (1.9 KB) is on the heap only while the child runs. `progtest spawn` (16 processes) takes 0.45 s.

**8b as built** (2026-10-04):
- **`/bin/sh` is the shell as a process:** `main` hands the loader a `BuiltinProgram` (`Shell::programMain`, so the loader doesn't depend on the shell). `exec` of `/bin/sh` or `/bin/bash` (when no such file exists) starts it in a task of its own with a 64 KB `Psram` stack. It is a process like any other, with a `Running`, a pid and a place in the job, but without an image. `Shell::runAsProgram()` builds a `Shell` on the process's descriptors, directory and environment (all exported), runs the built-in `sh` with its arguments, and frees the `Shell`. Programs it starts are its children, in its job: the loader finds the calling process through the task's thread-local pointer. It makes no system calls, so it is always "in the kernel" to a kill, and ends by itself on Ctrl-C, as the shell does.
- **`exec` resolves scripts** with the shell's rules: `Shebang` (`bmds/shebang.h`) is shared between the two. An existing interpreter file, `/bin/sh` or `/bin/bash` (the shell), or `/usr/bin/env name` (`name`, `name.elf`, `name.sh` on the `PATH` of the new environment) goes in front of the script's arguments, at most 4 deep (then `ELOOP`). A `.sh` file goes to the shell; anything else is `ENOEXEC`, and musl's `execvp` then runs it with `/bin/sh` (POSIX). The file is opened once: its head is read, and a program is loaded from the same handle.
- **The child's stack:** musl's `posix_spawn` gives `__clone` 5 KB inside its own frame. That is enough on Linux, where `exec` runs on a kernel stack, but here the kernel's `exec` runs on the caller's: resolving a `#!` interpreter overflowed it into `posix_spawn`'s frame, and the parent hung at its exit. `__clone` now runs the child on the current stack, below the caller's frames, as `vfork` does; the parent is suspended until the child execs or exits. Kernel calls run on the program's stack, so they need room there.
- **Ctrl-C:** a command's first process that ends after a Ctrl-C now takes the rest of its job with it (`killJob`). Without that, BusyBox `hush`, which returns from a `waitpid` that failed with `EINTR`, left a shell process and a spinning program running that nothing could kill. Signals (step 9) replace the rule.
- **Verified on the board:** `progtest scripts`:
  - `system()` (its status, a redirection) and `system(NULL)`;
  - `popen` both ways;
  - `#!/bin/sh` with an argument, a `.sh` file without `#!`, a program as interpreter (`#!progtest env`);
  - a CR LF `#!` line with `-e`;
  - `ENOEXEC`, `ELOOP`, `ENOENT`;
  - `execvp` falling back to `/bin/sh`.

  Also: `hush` `exec`s `/bin/sh -c`, whose programs report their status back; a script run by `hush` through `#!/bin/sh`. Ctrl-C ends a sleeping shell process at once, and a shell process running a spinning program, with the job. 21 runs of `progtest scripts` leave free memory and tasks as they were; `progtest spawn` and the corpus pass.
- **Host:** `Shell::runAsProgram()` (`-c` with the environment, directory and status, `-e`, a missing script, an unknown command, no terminal, no leaks).
- **Measured:** the firmware grew 3.4 KB (435.1 → 438.5 KB); a shell process costs its 64 KB `Psram` stack and its `Shell` object, only while it runs.
- **`$0`** was always `bmds` in the shell. It is now the script (`sh script`, a script run by name or through `#!`), the name after `sh -c command`, else `sh` for `sh -c` and `bmds` at the prompt; functions and `source` keep it, as in bash (a corpus entry checks it against bash).

**8c as built** (2026-10-04): BusyBox's `find -exec` (`;`, `+`, `-ok`), `time`, `nohup` and `xargs -I` are on (`xargs` was built already and now runs its commands); 116 applet links. No kernel change: the Linux layer's `waitid` now zeroes the resource usage musl's `wait4` passes (BMDS doesn't count CPU time, so `time` shows `user` and `sys` as 0 and `real` from the monotonic clock).
- **Verified on the board:** each applet directly and inside `hush` (`find -exec … + | sort`, `ls | xargs`); a missing command (`xargs` 127; `find` prints the error and exits 0, as BusyBox does on Linux); `time` passing on the exit status; `nohup` writing `nohup.out` and sending stderr to a redirected stdout. Ctrl-C (once and twice) ends `xargs`, `find -exec`, `time` and `hush` with their children; tasks and free memory are as before afterwards.
- **Cost of an applet's child:** with `CONFIG_FEATURE_PREFER_APPLETS` and no `fork`, BusyBox runs its own applets by `exec`ing `/sd0/p0/bin/busybox` again: `busybox time -p busybox true` takes 0.27 s, almost all of it loading the 1 MB image from the card.

## 14. Shared program images and the image cache (design, 2026-10-04)

**Why.** Every `exec` reads the program from the card, checks every relocation and copies the whole image, even when the same program is already running. BusyBox makes this the common case: with no `fork`, its applets run their children by `exec`ing `/sd0/p0/bin/busybox` again, and `xargs`, `find -exec` and `hush` do so per command. `busybox time -p busybox true` takes 0.27 s (8c), almost all of it loading; two running BusyBoxes hold two 520 KB copies of the same code. Two parts, each useful alone:
1. **The image cache:** a program's checked, unrelocated bytes and its relocation list stay in `Psram` after it runs; the next `exec` copies and relocates from memory, without the card. Every program gains; no MMU.
2. **Shared pages:** the 64 KB pages of an image that no relocation touches and the program never writes are the same at every load address, so instances share them, each instance mapping them through the MMU next to its private pages (the spike in §2).

**What a page is.** An image is cut into 64 KB pages from its start (the MMU's page; `load()` already takes the address the image will run at separately from where it is written). A page is **shared** when it holds no relocation target and lies entirely below the first writable section (`.data`, `.tdata`, `.bss`...: the lowest address of an `SHF_WRITE` section, found by `inspect()`); every other page, and every page from the first writable one on, is **private**. Code reaches its data PC-relatively, so an instance's pages keep the image's layout: one range of the window per instance, shared pages mapped where they fall, private ones between them.

For BusyBox as built (516 KB image, 9 pages; relocation targets: 9 in page 5, 250 in page 6, 63 in page 7; writable from 0x7F458): pages 0-4 are shared (320 KB), pages 5-8 private (256 KB per instance). Two instances need 832 KB instead of 1040 KB; the rest comes with the SDK changes below. `progtest` (2 pages, relocations in both) has none: it runs as a plain copy, from the cache.

**The cache entry** (`Psram`, owned by the cache, not by a process):
- the key: the program's absolute path (after following emulated links), its size and its modification time;
- `Info` (entry, stack size, ABI version, sizes);
- the shared pages: 64 KB-aligned physical pages, filled from the file once, written back, never written again;
- the private part's file bytes, unrelocated (BusyBox: 190 KB), and the relocation list for the whole image (offset and kind, 4 bytes each; every target lies in a private page by definition; BusyBox: 322);
- a count of the instances using it, and when it was last used.

A program without shared pages keeps its whole file part as "private" bytes.

**Starting an instance** (an `exec` that finds a valid entry, or after loading one):
1. A range of the window (the image's pages, 64 KB-aligned), and one 64 KB-aligned `Psram` page per private page, allocated separately (no contiguous run needed).
2. Through the private pages' heap addresses: copy the private bytes, zero the bss and the rest of the last page, apply the relocations for the range's address, and write the pages back (`esp_cache_msync` C2M).
3. Map the shared and private pages into the range, entry by entry, in a spinlock (the entries are an index/content register pair, `mmu_ll_write_entry()`), then invalidate the range. IDF's own `esp_mmu_map()` path (caches off, the other core stalled) is not used: changing entries nobody uses yet is safe without it (spike: 5000 remaps under load), and it stalls the other core.
4. The program runs at the range's address; `Running` keeps that base (for entry, crash offsets and freeing) apart from the memory it owns.

**Ending an instance:** invalidate the range (dropping its dirty lines, so none is written back to an unmapped address), unmap it, free the private pages and the range, and decrement the entry's count. Nothing is written back: the instance's data dies with it.

**Plain copies** remain for what the window can't take: an image without shared pages, a full window or no 64 KB pages free. The image goes into one 4096-aligned `Psram` block, as now, copied and relocated from the cache entry. Each instance of a small program then costs its size, not whole 64 KB pages.

**The window.** At boot the loader reserves a block of the PSRAM window's free addresses with `esp_mmu_map_reserve_block_with_caps()`, once, for good (a private IDF API: IDF's PSRAM driver uses the same pair with `mmu_hal_map_region()`). Its size is `CONFIG_BMDS_IMAGE_WINDOW_MB`, default 16 MB (256 pages: 28 BusyBox instances; the free window is 32 MB, the rest stays for IDF's own `esp_mmu_map()` users). Ranges come from a bitmap (one bit a page, 32 bytes), first fit, under the loader's lock.

**Keeping entries valid.**
- **Lookup:** `exec` stats the path; an entry whose size or time differs is stale.
- **Writes:** the time on FAT moves in 2 s steps, so the VFS tells the cache when a cached path is opened for writing, truncated, renamed over or removed. A volume going away (card out, unmount) drops its entries.
- **Running instances:** a stale entry is never used again but lives until its last instance ends; a running program keeps running its old code.

**Budget and eviction.** Entries nobody uses stay until `CONFIG_BMDS_IMAGE_CACHE_KB` (default 4096) is exceeded, then go least recently used first. When a `Psram` allocation fails, the allocator asks the cache to drop unused entries and tries again, so the cache never denies a program memory. `mem` shows the cache's size.

**DMA.** IDF's `esp_ptr_external_ram()` is false for window addresses (it checks the heap's range). BMDS's SD driver tests it before DMA straight into a buffer; it gets a BMDS check that knows the window (`Memory`), so program buffers in static data still go straight to DMA. The SDMMC driver itself already syncs window addresses correctly (spike).

**SDK changes that make more pages shared** (later, measured one by one; old programs keep working, as plain copies or with fewer shared pages):
- **libgcc's `lui`s:** all 12 `R_RISCV_HI20` targets in BusyBox's code are libgcc's (`__clz_tab` from the division and `clz` helpers, medlow). Building those few helpers `medany` (or replacing them) frees page 5: 384 KB shared, 192 KB private.
- **Pointers in `.rodata`** (293 `R_RISCV_32` targets in BusyBox: tables of pointers) mark their pages private. Building with `-fPIE`-style data placement puts relocated constants in `.data.rel.ro`, which the link script could put with `.data` (whether the stock toolchain does this without a GOT is to be measured).
- **Writable data on its own page:** a link script that starts `.data` on a 64 KB boundary in the image (address space only: the gap isn't loaded) leaves one private page for BusyBox's 7 KB of data and bss. With all three: 512 KB shared, 64 KB per instance.

**Not done:** a stale pointer into an ended instance reads 0x0ADDBAD0 rather than faulting (§2), so this adds no protection, and a program that writes into its shared code corrupts the other instances (no write protection in the MMU; programs can already crash the machine, §2). Scripts (`#!`) aren't cached; their interpreter is.

**ABI impact:** none. The file format, the slots and what a program sees are unchanged; a program can't tell whether its pages are shared. The SDK changes above produce programs that every kernel loads.

**Budget (estimate):** firmware +4 KB; 32 bytes for the bitmap; per entry about 100 bytes plus its pages and bytes (`Psram`, within the cache budget); per instance a range and its private pages instead of a whole copy. Starting a cached instance: the private bytes' copy and a few hundred patches, about a millisecond for BusyBox (to measure), instead of 0.27 s.

**Testing:**
- **Host:** the page classification (shared/private from relocations and writable sections; images smaller than a page, an image ending exactly on a page, bss spanning pages); the relocation list split from `load()` with the same result as `load()` for any base; the cache's lookup, staleness, counts and eviction over a fake mapper and allocator.
- **Board:** BusyBox and `progtest` instances side by side (`hush` pipelines, `xargs`, `find -exec`), each with its own data; free memory per instance before and after; time of a cached `exec`; a program rewritten (same size, within 2 s) and removed while it runs; card removal with cached entries; Ctrl-C of shared instances; a full window falling back to plain copies; the cache emptied under memory pressure; tasks, free memory and the window bitmap as before afterwards.

**Phasing:** (a) the image cache with plain copies (`exec` without the card), measured; (b) shared pages through the window; (c) the SDK changes, each with its measurement.

### 14.1 Phase (a): the image cache with plain copies (design, 2026-10-06)

What (a) builds of the above, and the details the outline left open.

**The key without a path.** `Vfs::stat()` follows links and gives `FileInfo::id` (a hash of the physical path, case folded: the same for every name of a file) and `device` (the mount slot + 1). The key is `(device, id, size, modified)`: no path is stored, and every BusyBox link finds the one entry. A file with `id` or `device` 0 (outside the filesystems, a device node) isn't cached.

**`ProgramImage` gains two calls.** `read()` checks the file again (as `load()` does), reads its segment unrelocated and records each patch `load()` would apply as one word: the offset (below 16 MB, `MaxImageBytes`) and bit 31 for a `lui`/`auipc` (`HI20`), clear for a word (`R_RISCV_32`). Recording is `load()`'s walk at base 0 with the patches written down instead of applied, so both check exactly the same things. `place()` copies the bytes into an image, zeroes the bss and applies the patches for a base. A host test checks that `read()` + `place()` gives `load()`'s image byte for byte, at several bases.

**One path for every file program.** `exec` stats the file and looks the key up. A hit copies and relocates from the entry, without opening the file. A miss opens it, inspects it, allocates an entry, reads the file into it once, and places the image from it; the entry is then kept (or freed, when the cache can't keep it). A program is thus read from the card once either way. Only when there is no memory for an entry does the loader fall back to `load()` straight into the image. The extra memory is transient: the entry and the image exist side by side, as the image and the old relocation buffers did.

**An entry** is one `Psram` block: its header (list links, key, `Info`, pins, generation), the patches, then the file bytes. BusyBox: 516 KB of bytes, 322 patches. An entry is pinned while an image is placed from it. An entry that is dropped while pinned is freed by its last unpin.

**Staleness.**
- `Vfs::setChangeHook()` installs a hook that the VFS calls, with the storage lock held, with a file's `(device, id)` when the file:
  - is closed after writing: when a handle opened for writing closes, rather than when it opens, so an `exec` while a file is being rewritten runs the cached whole program, not a half-written file;
  - is removed, renamed (its old name) or touched.
- After an unmount it calls the hook with `(0, 0)`, which drops everything: removing a mount shifts the slots after it, so a slot number names a different filesystem afterwards.
- The hook drops matching entries and bumps the cache's **generation**. The loader reads the generation before its `stat`, and an entry created under an older one isn't kept. This closes the gap between `stat` and reading the file: a write that ends in between bumps the generation, and one that ends later drops the entry.

**Locks.** The cache has one `Mutex`, and nothing done under it allocates or calls the VFS: a lookup, a pin, linking an entry, freeing ones dropped. Lock order: the storage lock, then the cache's lock (the hook). The reclaim hook (below) takes the cache's lock, so an allocation made under it would deadlock; none is.

**Memory pressure.** `Memory::setReclaimer()` installs a reclaimer (an interface: `reclaim(space, bytes)`, and `usage()`: bytes and items held, bytes reclaimable now, hits, misses, reclaims); one may be set. When a `Psram` or `PsramDma` allocation fails, `Memory` asks the reclaimer, then tries again once if it freed anything. The cache frees its unpinned entries, least recently used first, until enough is free.

**Monitoring:** `mem` gains one line after its table, `cached  528485  programs 1, hits 1, misses 1, given back 0`: the bytes held, the programs, lookups since boot, and how often a failed allocation emptied the cache. The table is unchanged; `mem -h` prints every size for people (`16.55M`, `432.43k`).

**Budget:** `CONFIG_BMDS_IMAGE_CACHE_KB` (new `bmds_loader` Kconfig), default 4096; 0 turns the cache off. Keeping an entry evicts the least recently used unpinned entries until it fits. An entry bigger than the whole budget isn't kept.

**Testing (a):**
- **Host:** `read()` + `place()` against `load()` (the hand-built files and `hello.elf`, several bases); the cache's hits, misses, key mismatches (size, time, device), LRU eviction, the budget, pins (dropped while pinned), the generation, `changed()` by id and `(0, 0)`, and `reclaim()`; the VFS hook's calls (a write handle's close, remove, rename, touch, unmount), and none for a read-only file.
- **Board:**
  - `busybox time -p busybox true` before and after (0.27 s, 8c);
  - BusyBox applets through links sharing one entry;
  - `progtest` suites;
  - a program rewritten while cached runs the new one;
  - card removal;
  - memory after rounds.

**Phase (a) as built (2026-10-06).** As designed above.
- **Time:**
  - `busybox time -p busybox true` takes 0.01–0.02 s, from 0.34–0.35 s before;
  - `progtest` on a hit: 0.00 s (BusyBox's `time` counts in 10 ms steps), from 0.11–0.14 s on a miss;
  - ten `busybox true` from a hush loop: 0.22 s in all;
  - a miss costs what a load cost before (`progtest`: 0.11 s, three times, as before the cache): the file is read once, into the entry.
- **Memory:**
  - BusyBox's entry is 528,485 bytes and `progtest`'s 131,076;
  - free `Psram` and the task count are the same after rounds of the `threads`, `signals`, `spawn` and `poll` suites;
  - an unmount empties the cache and returns free `Psram` to its figure at boot.
- **Staleness on the board:**
  - one byte of a cached program rewritten in place (same size, within FAT's 2 s) and the next `exec` refuses the file;
  - a removed one's entry goes.
- **Pressure:** a program's 25.4 MB allocation, failing, emptied the cache and still failed: the 0.66 MB given back lay apart from the largest free block, with BusyBox's running image between them (largest block 25.69 MB with the cache, 26.74 MB without it and with no program running). The same request succeeded with the cache empty. Giving memory back doesn't defragment the heap; images in 64 KB pages of the window (b) need no contiguous block.
- **Size:** firmware +3,520 bytes with `mem`'s line and `-h`, `.bss` +136.

## 15. Step 9: signals (design, 2026-10-04)

**Goal:** signals between processes, as Linux has them for one user on one terminal: `kill`, Ctrl-C as `SIGINT` to the job, `SIGPIPE`, `SIGCHLD`, `SIGWINCH`, handlers, masks, `sigsuspend`. Yardstick: BusyBox `kill`, `timeout` and `watch` work, `yes | head` ends `yes` by `SIGPIPE`, `hush` traps and Ctrl-C at its prompt behave as on Linux. It replaces step 4's rule (a Ctrl-C makes blocking calls fail with `EINTR` until the program exits, which then reports 130) and 8b's (an interrupted job ends with its first process).

**Where signals live: in the kernel, per `Process`.** The SDK kept dispositions, the mask and pending signals in program memory (§12); the kernel must now decide, when a signal arrives from another task, whether it is dropped, waits, interrupts a blocking call or ends the process, without running program code. So `Signals` (`bmds_loader`, portable, host-tested; one per `Process`, vfork children included) holds, for signals 1-31 (bit `sig - 1`, Linux's layout):
- the action of each signal: default, ignore, or catch (the handler's address stays in the SDK);
- the mask (blocked) and the pending set (from any task: a spinlock, never held over a FreeRTOS call);
- an `Interrupt`, raised when a signal becomes due: pending, not blocked, and either caught or ending the process.

Realtime signals (32-64) are refused by `kill` (`EINVAL`) and never delivered; `sigaction` on them still works in the SDK. Signals don't queue (Linux's standard signals don't either).

**Default actions** (Linux's): `SIGCHLD`, `SIGURG`, `SIGWINCH`, `SIGCONT` are dropped; the stop signals (`SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU`) are dropped too, since nothing stops a task (job control is out of scope, below; `processes.md` §3.7.1 later made them stop and `SIGCONT` continue); every other signal ends the process, with Linux's wait status for "killed by signal N" (the shell shows 128 + N). `SIGKILL` can't be caught, ignored or blocked. Ignoring a signal discards it if pending; `exec` resets caught signals to default and keeps ignored ones, the mask and pending signals; a vfork child starts with its parent's actions and mask and nothing pending.

**Waking a blocked call: the task's interrupt.** Today a blocking call wakes on the interrupt of the stream it waits in (pipes and the console carry the job's, raised by Ctrl-C). A signal must wake the process it is for, and only when it is due, so the interrupt moves from the stream to the caller:
- `Interrupt::current()`: the interrupt of the calling task (FreeRTOS thread-local pointer 2; `CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS` 3; a `thread_local` on the host), set with `Interrupt::Scope`. Index 0 is IDF's pthread's, 1 the loader's `Running`.
- Pipes and the console wait through `Interrupt::current()`; `Pipe::create()` loses its interrupt argument.
- A program's task has its current `Process`'s interrupt (switched at `vfork`, its exit and `exec`); the kernel shell's tasks, its pipeline workers included, have the console's, so built-in commands see Ctrl-C as now; the shell as `/bin/sh` has its process's (`Shell::interrupt()` replaces `console().interrupt()` in the shell).
- A process's interrupt is cleared at the start of each of its system calls when nothing is due, and checked again after the clear, so a signal arriving in between still wakes the call (the sender sets the pending bit before raising).

**Delivery: at system-call boundaries.** A handler never runs inside a kernel call, and a program computing without system calls isn't interrupted until its next one; ending a process doesn't wait for that (below).
- Leaving each system call, the kernel writes the task's **hint** word, an `uint32_t` in program memory that the SDK registers at startup (`sig_setup`): non-zero while a signal is due for the task's current process. Senders in other tasks set it too.
- The SDK checks the hint after every Linux system call it makes. While it is set: `sig_take()` gives the next due caught signal (removing it from the pending set); the SDK blocks the handler's `sa_mask` and the signal (unless `SA_NODEFER`), runs the handler (`SA_SIGINFO` with `si_signo` and `si_code`), restores the mask, and goes on with the next. `SA_RESETHAND` sets the action back to default.
- `SA_RESTART`: a call that failed with `EINTR` and whose handlers all had it is made again (`read`, `readv`, `write`, `writev`, `pread`, `pwrite`, `ioctl`, `waitid`, `openat`), as Linux restarts them; sleeps, `poll` and `sigsuspend` aren't restarted.
- `sigsuspend(mask)`: `sig_suspend` sets the mask, waits until a signal is due and fails with `EINTR`; the SDK runs the handlers under that mask, then restores the old one. `pause()` (musl: `ppoll` with nothing and no timeout) is `sigsuspend` with the current mask.

**Ending a process by a signal.** When a signal that ends the process becomes due:
- **A program in its own task** (no vfork child running): the kernel ends it with step 5's handshake, now with the signal's status: suspended in program code, it is ended at once; inside a system call, it is resumed, its interrupt wakes the call, and it ends as the call returns. The shell as `/bin/sh` (always "in the kernel") sees its interrupt, unwinds, and ends with the signal's status.
- **A vfork child** runs on its parent's stack, which only the SDK can return to: its interrupt wakes it, the hint makes the SDK call `sig_take()`, which says "end" (`BMDS_SIG_TERMINATE`), and the SDK ends the child with `proc_exit_signal(sig)` and resumes the parent. A vfork child only makes system calls until it execs, so this is prompt.
- **A program whose vfork child is running:** it ends when the child execs or exits, as control comes back to it.
- **Sent by the program to itself** (`raise`, `abort`, an unblocked pending signal, `SIGPIPE`): it ends as the call returns.

**Sources.**
- `kill(pid, sig)` (`proc_kill`): `pid` > 1 one process (a zombie still accepts it, as on Linux); 0 the caller's job; -1 every process but the caller; < -1 the job `-pid`. pid 1, the shell, is `EPERM`. `sig` 0 only checks. One user: always permitted.
- **Ctrl-C:** the console raises its interrupt as now (the kernel shell's commands stop); the launcher waiting for a program job sends `SIGINT` to every process in the job. If the previous Ctrl-C had no effect, because every process ignored it or one still has it pending (a program computing with a handler, or blocking it), this Ctrl-C sends `SIGKILL` to the job instead: a job that doesn't answer can always be stopped, while an interactive program that takes each `SIGINT` (`hush` at its prompt) is never killed by it.
- `SIGPIPE`: a write to a pipe without readers, before `EPIPE` returns.
- `SIGCHLD`: to the parent when a process ends (the shell, pid 1, excepted).
- `SIGWINCH`: to the job the launcher waits for, when the console's size changes (the console releases a semaphore the launcher registers).

**Process groups are jobs.** `getpgid`/`getpgrp` return the job, `TIOCGPGRP` the caller's job (one terminal: its job is in front), `setpgid` and `TIOCSPGRP` succeed without effect. Job control (stopping, `fg`/`bg`) is out of scope: no task is stopped.

**New slots** (appended to ABI 1, which hasn't shipped):

| Slot | Call |
|---|---|
| 42 | `int32_t sig_setup(volatile uint32_t *hint)`: the word the kernel sets while a signal is due (0 to stop). |
| 43 | `int32_t sig_action(int32_t sig, uint32_t action)`: `BMDS_SIG_DEFAULT`, `BMDS_SIG_IGNORE`, `BMDS_SIG_CATCH`, or `BMDS_SIG_QUERY` to only read it; returns the previous one, or `EINVAL` (`SIGKILL`, `SIGSTOP`, out of range). |
| 44 | `int32_t sig_mask(uint32_t how, uint32_t set)`: `BMDS_SIG_BLOCK`, `BMDS_SIG_UNBLOCK`, `BMDS_SIG_SETMASK` (Linux's 0, 1, 2); returns the previous mask. |
| 45 | `int32_t sig_pending(void)`: the pending set. |
| 46 | `int32_t sig_take(bmds_siginfo_t *info)`: the next due caught signal, removed from the pending set; 0 if none. `info` (`size`, `signo`, `code`: `BMDS_SI_USER` from `kill`, `BMDS_SI_KERNEL`; `flags`: `BMDS_SIG_TERMINATE` when the process must end instead). |
| 47 | `int32_t sig_suspend(uint32_t mask)`: sets the mask and waits until a signal is due; `EINTR`. |
| 48 | `int32_t proc_kill(int32_t pid, int32_t sig)`: 0, `ESRCH`, `EINVAL`, `EPERM`. |
| 49 | `int32_t proc_exit_signal(int32_t sig)`: ends the caller as killed by `sig`; in a vfork child, returns its pid in the parent (as `proc_vfork_exit`). |

Also `BMDS_PROC_JOB` (2) for `proc_id`. `interrupted()` (slot 25) now means "a `SIGINT` is pending": a program on BMDS's own calls can catch `SIGINT` and poll it, taking it with `sig_take()`. A program that never calls `sig_setup` gets default actions: Ctrl-C ends it, as on Linux.

**SDK** (`linux.c`): the signal state moves to the kernel; the SDK keeps the handlers (still saved and restored around `vfork`) and the hint. `rt_sigaction`, `rt_sigprocmask`, `rt_sigpending`, `rt_sigsuspend`, `kill`/`tkill`/`tgkill` on the slots; `exit` loses the 130 rule.

**Not in this step:** `alarm`/`setitimer` (a timer per process; with `poll`'s timeouts later), `sigaltstack`, `sigwaitinfo`/`sigtimedwait`, the sender's pid in `siginfo`, realtime signals, stopping and continuing, delivery in the middle of computation (no preemption of program code: delivery waits for a system call; ending doesn't).

**ABI impact:** slots 42-49 and their constants, `BMDS_PROC_JOB`, `bmds_siginfo_t`; `interrupted()` redefined; Ctrl-C is a signal for every program (programs built before this get default actions: Ctrl-C ends them). Recorded with an `ABI:` line.

**Budget (estimate):** firmware +4 KB; per `Process` about 60 bytes (`Psram`); one thread-local pointer per task; no new task. The SDK's check after each system call is one load.

**Testing:**
- **Host:** `Signals` (actions, mask, pending, due and ending rules, ignore discarding, `exec` and vfork inheritance, clear-and-check against a racing sender); `Interrupt::current()` and pipes waking on the caller's interrupt; the shell's built-ins still stopped by Ctrl-C.
- **Board:** `progtest signals`: a handler run by `kill` of itself and of a child, `SA_RESTART` on a pipe read, `EINTR` without it, the mask holding a signal until unblocked, `sigsuspend`, `SIGCHLD`, `SIGPIPE` (default and ignored), `SIGTERM` ending a sleeping child and a computing one, a vfork child killed before its exec, `exec` resetting handlers, `kill(0)` and `kill(-job)`. Ctrl-C: a program with a handler, one ignoring it (second Ctrl-C kills), one computing; `hush` at its prompt and running a program; BusyBox `kill`, `timeout` (with `-k`), `watch`, `yes | head`. Free memory and tasks as before afterwards; the corpus passes.

**Sub-steps:** 9a, the kernel (`Signals`, the task's interrupt, the slots, ending, `kill`, Ctrl-C, `SIGPIPE`, `SIGCHLD`) and the SDK, tested with `progtest signals`; 9b, `SIGWINCH` and BusyBox's `kill`, `timeout`, `watch` with `hush` as the measure.

**9a as built** (2026-10-04): as designed above, plus what building it found.
- **Inherited ignored signals:** a new program's handler table starts empty, so `sigaction` would report `SIG_DFL` for a signal ignored across `exec` (`nohup`, a shell's `trap ''`). `sig_action` takes `BMDS_SIG_QUERY` to only read an action; the SDK reads them all at startup.
- **`waitpid` and `SIGCHLD`:** a child's end releases its parent's wait and then sends `SIGCHLD`; with a handler, the wait woke interrupted and failed with `EINTR` although the child could be reaped. As on Linux, an interrupted wait first reaps an ended child.
- **A launcher that stops waiting:** the shell as `/bin/sh` stops waiting for its program when a signal ends it, but the child's table entry points at a semaphore on the launcher's stack. `ProcessTable::detach()` makes the child an orphan first.
- **The shell after a Ctrl-C:** any Ctrl-C during a program abandoned the rest of the line. Now only a program that `SIGINT` ended does (bash's rule); one that caught or ignored it returns its status and the line goes on.
- **vfork children** now have their task's `Running` in the table, so signals find them. `SIGKILL` to a program with a vfork child running ends both: only the SDK can return to the parent's frame.
- **Verified on the board:** `progtest signals` (the list in Testing, all of it), `progtest spawn` and `scripts`, the corpus. Ctrl-C: a program with default actions (130), one with a handler (its status, 7, and the line goes on), one ignoring it (the second Ctrl-C kills: 137), computing with and without calls, `busybox sleep`, `hush -c` with `trap ... INT`, interactive `hush` at its prompt and running a program; `hush` `trap ... USR1`, `kill`, `kill -9`, `wait` (143); `yes | head`. Free memory and tasks as before afterwards.
- **Host:** `Signals` (defaults, catching, the mask, ignoring, exec and vfork inheritance, a sender racing the clear), `Interrupt::current()`, pipes waking only callers with that interrupt, `Process` (sleep, `interrupted()`, `SIGPIPE`), the shell going on after a caught Ctrl-C.
- **Measured:** the firmware grew 3.3 KB (438,348 → 441,674 bytes).
- **Seen, not new:** interactive `hush` reports `fcntl(0,F_DUPFD,255): Invalid argument` at startup (a process has 16 descriptors) and runs without its own copy of the terminal's fd.

**9b as built** (2026-10-04):
- **`SIGWINCH`:** `Console::setSize()` raises `Console::resized()` when the size changes; the launcher watches it (`Interrupt::Watch`: the semaphore it waits on is released by that interrupt's raises too) and sends `SIGWINCH` to the job. Each wake checks what happened, since `ended` is now released by resizes as well (`ProcessTable::isEnded()`). A serial terminal's size is learned only when something asks (the prompt, `term size`): asking from a program's `TIOCGWINSZ` was left out, because only a reader takes the answer out of the input, and a program that never reads would leave it there as typed text for the shell.
- **BusyBox `kill`, `timeout`, `watch`** are on (119 links). `timeout` didn't time anything out: its no-MMU watcher re-execs itself as `timeout DURATION -pPARENT`, but `getopt32("+...")` stops at the duration, so `-p` was never seen and the watcher died. `dist/busybox/patches/0001-timeout-nommu-reexec.patch` puts `-p` (and the options) first.
- **A race fixed on the way:** the launcher read `run->job` after starting the program; one that ended at once could already have been freed.
- **Verified on the board:** `progtest winch | { sleep 0.5; term size 100x40; }` (the program gets `SIGWINCH` and sees 100x40); `kill -l`, `kill -USR1` from `hush`; `timeout 1 sleep 10` (143 after 1.8 s: whole seconds, plus the watcher's start), `timeout -s INT` to a program catching it (its status), `timeout -s INT -k 1 1` to one ignoring it (137), a program that ends in time (0); `watch -n 1` until Ctrl-C (130). Free memory and tasks as before afterwards.
- **Host:** `Interrupt::Watch`, `Console::resized()`.
- **Measured:** steps 9a and 9b together: firmware +3.6 KB (438,348 → 441,982 bytes).

## 16. Step 10: `poll` and `termios` (design, 2026-10-04)

**Goal:** a program can wait on several descriptors with a timeout, and set the terminal as full-screen and line-reading programs expect. Yardstick: BusyBox `vi` and `less` (raw keys; `poll` with a timeout tells a lone Esc from an arrow key's sequence), `hush`'s `read -t`, `read -n`, `read -s`, interactive `hush` line editing; `select` and `pselect` work. It replaces the SDK's "poll by guess" (files and output ready; terminal or pipe input ready only for an endless wait; no `select` at all).

**Readiness in the kernel.** Each stream says what it is ready for and lets waiters know when that changes:
- `Stream::readiness()`: `Readable`, `Writable`, `HungUp` (a pipe's writer is gone: reads end), `Broken` (a pipe's reader is gone: writes fail). The default is readable and writable: files, directories, `/dev/null` and memory streams never block.
- `Stream::readinessWaiters()`: a `WaitList` released whenever the readiness may have changed, or null when it never does (the default).
- **`WaitList`** (`bmds_kernel`): a fixed set of semaphore slots, released on `notify()` (task) or `notifyFromIsr()`. It is what `Interrupt` already does for its waiters (lock-free slots, a busy count so a leaving waiter never has its semaphore released after it has gone, the scheduler suspended while a task releases), moved out so that `Interrupt` (8 slots), `Pipe` (4) and `Console` (4) share it. `notify()` first looks for a waiter, so a pipe nobody polls pays one load per slot on each read and write.
- **`Pipe`:** readable while it holds data or its writer is gone (with `HungUp`); writable while it has room or its reader is gone (with `Broken`). Reads, writes and closes notify.
- **`Console`:** input arrivals notify, from the receive interrupt. Writes are always ready (a console write waits briefly at most).

**`poll` in the kernel** (`Process::poll`, slot 50): one semaphore on the caller's stack is registered in the wait list of every distinct stream polled (at most `FdTable::MaxFds`), then readiness is checked, then the caller waits on the semaphore through its interrupt (`Interrupt::waitCurrent`) until something is ready, the timeout passes, or a signal is due (`EINTR`); it is unregistered from all of them before returning. Registering before checking means a change in between still releases the semaphore. A stream whose wait list is full (more pollers than slots) is checked every 10 ms instead: correct, and rare enough to cost nothing in practice.
- **The console as the program reads it.** A program's console input goes through its `Process` (the line editor with `BMDS_TERM_LINE`, left-over bytes, the terminal's size answer taken out), so readiness is the process's: `poll` pumps what has arrived (`Console::tryRead()`) into the process's input buffer, through the line editor in line mode, and the console is readable when a read wouldn't wait: a whole line in line mode (as Linux's canonical mode), any byte in raw mode. In line mode, polling echoes and edits what was typed, as reading would.
- `count` above 64 is `EINVAL`; a negative fd is skipped (`revents` 0); a closed one gets `POLLNVAL`. Events use Linux's values (`POLLIN` 1, `POLLPRI` 2, `POLLOUT` 4, `POLLERR` 8, `POLLHUP` 16, `POLLNVAL` 32); `POLLERR`, `POLLHUP` and `POLLNVAL` are reported whether asked for or not.

**New slot** (appended to ABI 1, which hasn't shipped):

| Slot | Call |
|---|---|
| 50 | `int32_t io_poll(bmds_pollfd_t *fds, uint32_t count, int32_t timeout_ms, const uint32_t *mask)`: the number of entries with events; 0 at the timeout (`timeout_ms` < 0: none; 0: only check); `EINTR`, `EINVAL`, `EFAULT`. With `mask`, the signal mask is set to `*mask` for the wait and left set when the call returns, so the caller runs the handlers of what interrupted it under that mask and then restores its own (as `sig_suspend`); that is `ppoll` and `pselect` without a race. |

`bmds_pollfd_t` is `{uint32_t size; int32_t fd; uint32_t events; uint32_t revents;}`, a boundary struct like the others (`CODING_STANDARDS.md`): the kernel steps through the array by the first entry's `size`, and every entry must have the same (else `EINVAL`), so the entry can grow. Linux's `struct pollfd` (8 bytes) differs, so the SDK copies the program's array into one on its stack and back (at most 64 entries, 1 KB; usually one to three). `BMDS_POLL_...` constants.

**termios.** Two flags join `BMDS_TERM_...`, set when a program starts, as the others:
- `BMDS_TERM_ECHO` (0x20): input is echoed. In line mode, without it lines are read unseen: no editor display, but Backspace, Ctrl-U (erase the line), Ctrl-D (end of input at the start of a line) and Enter work (passwords, `read -s`). In raw mode, with it, the bytes read are echoed (`read -n` keeps `ECHO` and clears `ICANON`).
- `BMDS_TERM_ICRNL` (0x40): in raw mode, an input CR is read as LF (Linux's default; `cfmakeraw` clears it). Line mode always ends a line with LF.
The SDK maps `ECHO`, `ICRNL` both ways, keeps `VMIN` and `VTIME` itself (per program; a program starts with 1 and 0): a terminal read with `VMIN` 0 polls first, for `VTIME` tenths of a second (0: just a check), and returns 0 if nothing came. `tcgetattr` reports what is in effect; the other flags and control characters keep reporting the fixed values they report now (`VINTR` is Ctrl-C...), and setting them has no effect.

**SDK** (`linux.c`): `poll`, `ppoll` on `io_poll`; `select`, `pselect6` (and their 64-bit-time forms) converted to poll entries (an fd from 64 up can't be open: `EBADF`, as for any fd in a set that isn't open), the timeout left in place untouched (Linux writes back what remains; programs must not depend on it); `pause()` (musl: `ppoll` with nothing and no timeout) waits in `io_poll` until a signal is due, which is what `sig_suspend` with the current mask did. `poll` and `select` aren't restarted after a handler (`SA_RESTART` doesn't apply to them, as on Linux).

**Not in this step:** `FIONREAD`, `VMIN` above 1 (read as 1) and `VTIME` with it, other control characters (`VERASE`, `VINTR`...) being settable, `POLLPRI`, `epoll`.

**ABI impact:** slot 50, `bmds_pollfd_t`, `BMDS_POLL_...`, `BMDS_TERM_ECHO`, `BMDS_TERM_ICRNL`. Recorded with an `ABI:` line.

**Budget (estimate):** firmware +2.5 KB; per pipe 20 bytes (`Psram`); per console 20 bytes; nothing per process; a `poll` uses about 150 bytes of the caller's stack (the registrations). No new task.

**Testing:**
- **Host:** `WaitList` (notify, full lists, a waiter leaving while notified), `Interrupt` unchanged (its tests), pipe readiness (data, room, both ends closing, notification on each), `Process::poll` (pipes ready and not, the timeout, `EINTR` on a due signal, `POLLNVAL`, the console in raw and line mode with the fake console: ready only on a whole line, echo off reading unseen).
- **Board:** `progtest poll`: a pipe becoming readable from a child, a timeout, `POLLHUP` after the writer exits, `POLLERR` on a pipe without a reader, `ppoll` with a mask woken by a signal, `select` on a pipe and the terminal. By hand: `vi` (arrow keys, Esc, `:q`), `less` on a file and a pipe, `hush` `read -t 2`, `read -n 1`, `read -s`, interactive `hush`. Free memory and tasks as before afterwards; the corpus passes.

**Sub-steps:** 10a, `WaitList`, readiness, `io_poll` and the SDK's `poll`/`select`, tested with `progtest poll`; 10b, `ECHO`, `ICRNL`, `VMIN`/`VTIME`, with `vi`, `less` and `hush` as the measure.

**10a and 10b as built** (2026-10-04): as designed above, plus what building it found.
- **`O_NONBLOCK`** joined the step: BusyBox `less` sets it on its terminal and reads keys until `EAGAIN`, so that it can go back to a pipe that is still filling; without it, it waited for a key first. The SDK keeps it per descriptor number (`fcntl`, `open`, `pipe2`; a `dup` starts without it; saved across a vfork child); a non-blocking read or write polls first (`io_poll` with no wait) and fails with `EAGAIN` if it would wait. A large non-blocking write to a pipe with some room still waits for the rest.
- **`WaitList`** is `Interrupt`'s waiter slots moved out (`bmds_kernel`); `Interrupt::Watch` is a `WaitList::Entry`.
- **`less` showed one line:** the BusyBox config had `FEATURE_LESS_MAXLINES=0` (upstream's default is 9999999), not a kernel matter.
- **A child that writes and exits** may be gone before the reader's `poll` returns, which then reports `POLLIN` with `POLLHUP`, as Linux can.
- **Verified on the board:** `progtest poll` (the list in Testing, plus `O_NONBLOCK` on a pipe and the terminal, `ECHO` and `ICRNL` both ways, `VMIN` 0 with `VTIME` 1 returning 0 after 100 ms), `progtest signals`, `spawn`, `scripts`, the corpus; `hush` `read -t 1` on the terminal and on a pipe (1 s, then status 1; a line typed in time; data already there), `read -n 1` (echoed), `read -s` (unseen); interactive `hush` (editing, Ctrl-C at the prompt and to a program); `vi` (insert, Esc, `x`, arrow keys, `:wq`, the file right); `less` on a file (paging, `G`, `/150`) and on a pipe filling in two parts 2 s apart (`G` reaches the end); step 9's Ctrl-C cases and `SIGWINCH`. Free memory and tasks as before afterwards (the shell keeps up to two idle pipeline workers).
- **Host:** `WaitList` (notify, a full list, waiters leaving while notified), pipe readiness and notification, `Process::poll` (arguments, pipes, a file, `POLLNVAL`, the timeout, woken by a write and by a signal, the console in line and raw mode, both ends closing), raw input with `ICRNL` and echo, lines without echo.
- **Measured:** firmware +2.5 KB (441,982 → 444,528 bytes).

## 17. `/bin`: built-in programs as files (design, 2026-10-05; built)

**Goal** (the user's): every program built into the OS is a file in a read-only `/bin`, so anything
that runs programs (the shell's lookup, `exec`, `#!`, `posix_spawn`, the desktop's menu and the
panel's launcher) finds and runs it as it would a program on the card, with nothing special for
it. A built-in pulled out of the OS becomes a file elsewhere, and what runs it doesn't change.
With the decision in `windows.md` §0 (built-in applications are ordinary applications) and the
pseudo-terminals of `pty.md`, it makes the terminal application `/bin/terminal` (`term` is
already a shell command, the serial terminal's size).

- **What's in it: programs, not the shell's commands.** A built-in program runs as a process (its
  own task, descriptors, signals: a `BuiltinProgram`, as `sh` is today). The shell's built-in
  commands (`ls`, `win`, ...) stay the shell's; they could appear as BusyBox-style applets later,
  if something needs to `exec` them.
- **The registry:** `BMDS_PROGRAM(name, main, stackBytes)` puts an entry in a linker section, as
  `BMDS_SHELL_COMMAND` does, so a program registers itself where it is defined; nothing in `main`
  adds any. Names are an interface once shipped (append only). An alias is a second entry with the
  same `main` (`bash` for `sh`).
- **In the VFS:** `/bin` is a virtual directory, like `/dev`: `ls /bin` lists the registry, `stat`
  gives each an empty regular file (size 0; reading one gives nothing), and nothing in it can be
  made, written, renamed or removed (`ReadOnly`, `EROFS`). `Vfs::builtinProgram(path)` says which
  program a path names, symbolic links followed, so a link on the card to `/bin/terminal` runs it.
- **The loader** asks the VFS for every path it runs: a built-in runs as `sh` does now. The
  special case for `/bin/sh` and `/bin/bash` and `Loader::setShell()` go; `sh` registers itself.
  `#!/bin/sh` scripts written on a PC now name a real file. Nothing is guessed for an interpreter
  path that doesn't exist (the user, 2026-10-05: porting Linux scripts unchanged isn't a goal):
  D7's fallbacks (`/usr/bin/env name`; a missing path's last component as a built-in) are gone;
  `#!/bin/env name` does what `/usr/bin/env` did.
- **`PATH`** defaults to `/bin:/sd0/p0/bin` (the user's choice, 2026-10-05): the built-ins first,
  so a program on the card can't take a built-in's name, and they are found without the card. It is
  set in `/boot/profile` (`storage.md` §13), so the order is one line to change.
- **ABI impact:** the paths `/bin/NAME` and the names; the default `PATH` gains `/bin` (appended).
  Recorded with an `ABI:` line.
- **Testing:** host: the registry, the VFS's `/bin` (listing, `stat`, refusals, a link to a
  built-in); board: `sh` from `/bin` (`exec`, `#!/bin/sh`, `system()`), `ls /bin`, `PATH` order
  with a card program shadowing a built-in, the corpus and `progtest` as before.

**As built** (2026-10-05):
- **The registry** (`bmds_kernel`): `ProgramEntry` (a name and a `BuiltinProgram`),
  `BuiltinPrograms::all()` and `find()` (a binary search: the linker sorts `.bmds_prog.NAME` by
  name, `linker.lf`), `BMDS_PROGRAM(Name, Main, StackBytes)`. The host tests set their own table.
  `sh` and `bash` are registered next to `Shell::programMain` (`shellesp.cpp`); `main` no longer
  sets the shell up (`Loader::setShell()` is gone).
- **The VFS:** `/bin` and `/bin/NAME` are node kinds of their own; `/` lists `bin` before `dev`;
  opening a built-in for reading gives an empty file, for writing `ReadOnly`, as do creating,
  `mkdir`, `rmdir`, `remove`, `rename` (either end), `symlink` and `setModified` in `/bin`;
  `mount` over `/bin` is `Invalid`. `Vfs::builtinProgram(path)` follows links.
- **Running one:** the loader's `loadImage()` (the shell running a command) and `resolveExec()`
  (`exec`) ask the VFS first. The kernel shell looks at a file's first bytes itself before running
  it (`Executor::runFile()`, `isExecutable()`), so it asks too: an empty `/bin` file was refused
  as "not a program or script" until then. A `#!` interpreter path that doesn't exist here runs the
  built-in its last component names (`#!/usr/bin/bash`).
- **Verified on the board:** `ls /` (`bin boot dev ram0 sd0`), `ls -l /bin` (`bash`, `sh`, size 0);
  `bash -c`, `/bin/sh -c '...; exit 3'` (status 3), a `#!/bin/sh` script, a `.sh` file, BusyBox's
  `system()`, a link on the RAM disk to `/bin/sh`; `echo x > /bin/sh`, `rm /bin/sh`, `touch
  /bin/new` refused ("Read-only file system"); `progtest spawn`, `scripts`, `signals` and `poll`
  all pass (pushed with `bmds-run`); no task left behind.
- **Host:** the registry and `/bin` in the VFS (listing, `stat`, reading, every refusal, mounting
  over it, `builtinProgram()` by path and through a link); the listings of `/` updated.
- **Known:** the shell as a program always calls itself `sh` (`$0` in `bash -c 'echo $0'` is
  `sh`, where bash says `bash`): it runs its built-in `sh` by that name.
- **Applets** (the user's, 2026-10-05): every shell command is also `/bin/NAME`, as a
  program of its own, except the ones that only work inside a shell (they change it: `.`, `:`,
  `break`, `cd`, `command`, `continue`, `eval`, `exit`, `export`, `local`, `read`, `readonly`,
  `return`, `set`, `sh`, `shift`, `source`, `type`, `unset`). `BMDS_SHELL_COMMAND` registers both
  (the command, and a `ProgramEntry` running `shellAppletMain`: a shell of its own running the
  command named by the program's name, `BuiltinProgram::Context::program`, which the loader now
  fills); `BMDS_SHELL_BUILTIN` registers a shell-only one. Typed at a prompt a command runs in the
  shell as before (`type ls`: a shell builtin); run by path from the shell (`/bin/ls`, a `#!`) one
  of the shell's own commands also runs in the shell, as if typed; run by another program
  (`exec`) it is a process. Built-ins win names over the card (`PATH=/bin:/sd0/p0/bin`): a program
  that runs `ls` gets this `ls`. An entry is 12 bytes of flash (49 programs at present).
- **`env`** (new, an applet): `env [name=value]... [command [arg]...]` runs the command with the
  variables set and exported, putting them back afterwards; without a command it lists the
  environment. It is what `#!/bin/env name` scripts use.
- **Verified on the board (applets):** `ls /bin` lists 49 (the shell-only ones absent: `ls /bin/cd`
  fails); `/bin/ls`, `/bin/win list` by path; by `exec` from BusyBox (`busybox env /bin/ls /boot`,
  `/bin/date`, `/bin/win list`, `/bin/[ 1 -lt 2 ]` 0 and `3 -lt 2` 1, `/bin/env Q=7 /bin/sh -c
  ...`): each a process of its own, no task left behind; `#!/bin/env sh` runs, `#!/usr/bin/env
  sh` fails ("No such file or directory"); `progtest spawn`, `scripts`, `signals`, `poll` pass.
- **Host (applets):** `#!/bin/sh` (in the shell), `#!/bin/bash` (by the launcher, with its
  argument), `#!/bin/env sh` and `#!/bin/env hello` ($PATH), `#!/usr/bin/env` and
  `#!/usr/bin/bash` refused; `env` (set and put back, unset again, a name twice, the listing
  without unexported variables, a missing command).

## 18. Interval timers: `alarm`, `setitimer` (design, 2026-10-11; built)

Left out of step 9; found missing by the network work (BusyBox `ping` sends its later requests
from `SIGALRM`, and `nc -w`, `wget -T`, `timeout`'s own use all time out with `alarm`).

- **One real-time timer per process** (`ITIMER_REAL`), as Linux's: kept across `exec`, not
  given to a child (a vfork child has its own pid, so its own timer), gone when the process
  ends. It raises `SIGALRM` from the kernel (`BMDS_SI_KERNEL`); a repeating one moves on by
  whole intervals past now, so a late firing never queues a burst (a pending `SIGALRM` is one
  signal anyway).
- **Where it runs:** a table of `ProcessTable::Capacity` entries (pid, deadline, interval) in
  the loader under a lock of its own, run by the existing `sig` task: it waits on its queue
  until the earliest deadline (nothing polls; no timer task, no FreeRTOS timer each), and a
  change posts an empty entry to wake it. Deadlines are on the monotonic clock in
  nanoseconds; the wait rounds up to the tick, so a timer never fires early.
- **Slot 108:** `int32_t proc_timer(uint32_t which, const bmds_timer_t *set, bmds_timer_t
  *old)`. `bmds_timer_t` is `{size, reserved, value_us, interval_us}` (`value_us` 0 disarms);
  `set` null only reads; `old`, if given, gets what was left and the interval. `which` is
  `BMDS_TIMER_REAL` (0, Linux's `ITIMER_REAL`); `ITIMER_VIRTUAL` and `ITIMER_PROF` (CPU time)
  are `EINVAL` for now: they need the process's CPU time charged as it runs.
- **`linux.c`:** `setitimer` and `getitimer` (musl passes 32-bit `timeval`s on rv32; a
  `tv_usec` outside 0-999999 is `EINVAL`, as Linux); `alarm` and `ualarm` are musl's over them.
- **Not yet:** POSIX timers (`timer_create`: several per process, `SIGEV_THREAD`), CPU-time
  timers, overrun counts.
- **Budget:** a `constinit` table of 32 × 24 bytes (768 bytes of `.bss`); no task, no
  allocation.
- **Testing:** `progtest timers`: `alarm` interrupting a blocking pipe read (`EINTR` without
  `SA_RESTART`), `setitimer` repeating (three `SIGALRM`s at about the interval, never early),
  `getitimer` counting down, disarming, the old value returned, kept across `exec`, gone in a
  vfork child, `EINVAL` for bad values; BusyBox `ping -c 3`, `nc -w 1`, `timeout`.

**As built** (2026-10-11): as above. On the board `progtest timers` passes (`alarm(1)` after
1001 ms; three 100 ms periods in 302 ms), and BusyBox `ping -c 3`, `nc -w 1`, `wget -T 2` and
`timeout 1 sleep 5` time out as on Linux. Building it found a race older than it: a child's
end was recorded (waking the parent's `waitpid`) before its `SIGCHLD` was sent, so on the
other core `waitpid` could return before the handler was due (`progtest signals` failed now
and then once the timer's clean-up sat between the two). The end and the `SIGCHLD` now happen
under one lock that a reaping wait passes through before returning, so the signal is pending
by then, as on Linux; `progtest signals` passed five runs in a row after.

**Testing it further** (2026-10-11):
- The table's logic moved to `bmds_core` (`IntervalTimers`, header-only) with a host test:
  deadlines never early, one firing for a late look at a repeating timer, several owners
  and the earliest next, replacing, disarming, dropping, a full table, values cut without
  overflow.
- A dead process's timer is now dropped before its end is recorded: before, the parent could
  reap it and a new process take the pid in between, and get its SIGALRM.
- `progtest timers` grew: 100 children each ending with a timer armed (none leaks an
  entry), a process computing without system calls ended by SIGALRM after 1030 ms, three
  timers at once each on time (50, 120, 300 ms), `SA_RESTART` (an interrupted read gets what
  comes later), a process stopped when its timer fires (handled after `SIGCONT`), threads
  (the signal reaches the one not blocking it), 63-year values read back.
- On the board every non-interactive `progtest` suite, the RAM disk and card `posix`, the
  `tree` Ctrl-C, and the v1 corpus pass; the distribution rebuilt and installed with the new
  `linux.c`.
