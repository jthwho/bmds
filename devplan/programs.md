# Programs: Executables, Loader, Syscall ABI v1, SDK

Status: **steps 1-3 done** (Phase 3): the PSRAM spike; the SDK skeleton, the format check and relocation (`ProgramImage`), `bmds-elfcheck`, the ABI lock; the loader: programs run from the card by path (`./hello.elf`). D1-D7 agreed (2026-10-04; D2 revised in step 2, below). Once the first program ships, everything marked *frozen* is userspace ABI (`docs/ABI.md`): append-only forever.

## 1. Goal

- Programs are built separately from BMDS (cross-compiled on a PC now, on the device later), stored on the SD card, loaded and run by typing their name, and get (nearly) the whole machine while they run, as on DOS.
- A program sees the Unix I/O model the shell already has: fds 0/1/2 (console, pipe, file), `argc`/`argv`, the exported environment, an exit status. It works in pipelines and with redirections like any built-in.
- A small, stable C ABI: a program built against SDK v1 runs on every later BMDS.
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
- **No user mode:** IDF runs in M-mode only on the P4 (ESP-TEE supports C5/C6/H2), and its PMP entries are locked at boot. Memory protection between kernel and program is not available now (`docs/VISION.md` non-goal); the design keeps the door open (§4).
- **Toolchain:** `riscv32-esp-elf-gcc` 15.2. The firmware uses `rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1`, `ilp32f` (no Zc* on chip revisions below 3, erratum DIG-661). The toolchain ships **picolibc** and **newlib** (and newlib-nano) multilibs for `rv32imafc_zicsr_zifencei_zaamo_zalrsc` / `ilp32f`. `-fPIC`/`-fPIE`, `-mno-relax` work.
- **No ELF loader** in IDF or locally; Espressif's registry `elf_loader` exists but loads shared objects with a wider relocation set than we want to freeze.

## 3. Decisions (agreed unless marked)

| # | Question | Choice | Alternatives |
|---|---|---|---|
| D1 | Call mechanism | **A function table**, its address passed to the entry point | `ecall` trap |
| D2 | Executable format | **ELF linked at 0 with its relocations kept** (`ET_EXEC`, one image; the loader patches `R_RISCV_32` and `R_RISCV_HI20`), DOS-EXE style; revised from static PIE in step 2 | static PIE with our own picolibc built `-fPIE`; custom format; relocatable `ET_REL` |
| D3 | Where code runs | **PSRAM** (the whole image), measured as fast as flash or IRAM (§2); fast internal RAM for data on request | internal SRAM (needs MEMPROT off) |
| D4 | C library | **picolibc, linked into each program**, over a small SDK glue layer | newlib-nano in the program; libc provided by the kernel |
| D5 | Instruction set | **Everything every P4 runs: `rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1`, `ilp32f`** (the firmware's own set) | the base set only; Zc* (rev ≥ 3 only) |
| D6 | File naming | **`name` runs `<dir>/name`, `<dir>/name.elf` or `<dir>/name.sh`** (in that order) from `$PATH` (default `/sd0/p0/bin`); what runs is decided by the file's first bytes: a BMDS ELF, or a `#!` script (D7) | `.elf` required; no extension |
| D7 | Scripts | **`#!` is planned into the launch path** from the start (§5), even if the first release only runs ELF | scripts only through `sh file` |

### D1: why a table

- A call through a table is an indirect call: a few cycles. An `ecall` traps into IDF's exception vector, saves the full context, dispatches, and returns: hundreds of cycles, for no protection today (M-mode only, locked PMP).
- **The table keeps traps possible.** Programs only ever call through the table they were given. If a later chip or IDF allows U-mode, the kernel hands such programs a table of small stubs that `ecall`; their binaries don't change. The reverse (traps now, table later) would need nothing either, but costs the trap price from day one.
- The table is versioned: `{ uint32_t magic, abi_version, count; void *slot[count]; }`. A slot number is frozen forever; a program probes a slot as `slot < count && slot[n] != NULL` (ABI rule 8).

### D2: ELF linked at 0, relocations kept (revised in step 2)

- **First choice, static PIE, didn't survive contact with the toolchain:** the picolibc that ships with `riscv32-esp-elf-gcc` is compiled for the `medlow` model (absolute `lui`/`addi` addressing, `R_RISCV_HI20`), which a PIE can't contain (ld refuses, then crashes). Keeping static PIE would mean building picolibc ourselves with `-fPIE`. Agreed instead (2026-10-04): the classic relocatable executable, as DOS did with `.EXE` files.
- **The format** (`sdk/include/bmds/program.h`): `ET_EXEC`, one `PT_LOAD` at virtual address 0 holding the whole image, linked with `--emit-relocs` so the relocation sections and the symbol table stay in the file. The kernel loads it at any 4096-byte-aligned address B and patches only two kinds: `R_RISCV_32` (a data word: add B) and `R_RISCV_HI20` (a `lui`: add B >> 12 to its upper immediate; B's low 12 bits are zero, so the matching `LO12` halves never change). References to absolute or undefined (weak) symbols aren't patched. Everything else in loaded sections is either position-independent (branches, calls, `PCREL_*`, thread-local `TPREL_*`, label differences `ADD/SUB/SET*`, the linker's `RELAX`/`ALIGN` hints) or refused, as is a `PCREL_HI20` to an absolute symbol that the linker didn't turn into a `lui`. Programs' own code is built `medany` (PC-relative, no GOT, slightly faster than PIE); medlow libraries work through `HI20`.
- **Identification:** `EM_RISCV`, `ELFCLASS32`, little endian, float ABI flag `EF_RISCV_FLOAT_ABI_SINGLE`, and a `PT_NOTE` with name `"BMDS"`, type 1, holding a `bmds_program_note_t` (size, ABI version the program was built for, requested stack size, flags). A file without the note is not a BMDS program (`exec format error`).
- **Cost:** the file keeps its relocations and the symbols they use, so it is bigger than its image: `hello.elf` is 34 KB for an 11.5 KB image after the SDK's `objcopy --strip-unneeded` (458 KB with everything, kept as `hello.debug.elf`). A full `strip` breaks a program (the loader says "no section headers (stripped? relocations are needed)").
- **Why it's still a good format:** stock toolchain and libraries; standard tools read it (`readelf`, `objdump`, `addr2line`, GDB); a small self-hosted compiler emits exactly this (non-PIC code plus relocations) without knowing about PIC; the loader is small and refuses whatever it doesn't understand instead of guessing.
- No dynamic linking, no shared libraries in v1 (`PT_DYNAMIC` and `PT_INTERP` are refused).

### D3: memory

- The image (text, rodata, data, bss) is one `Psram` allocation, aligned to 4096 bytes (the relocation scheme needs it); the loader copies the `PT_LOAD` segment, zeroes bss, applies relocations, syncs the caches.
- The program's **stack** is allocated from internal RAM if it fits (stacks are hot), else `Psram`: the note asks for a size (default 32 KB, at most 1 MB).
- **Heap:** the SDK defines `malloc`, `free`, `realloc`, `calloc` and the aligned forms itself (`sdk/crt/malloc.c`), straight over the kernel's allocator: slots `mem_alloc(size, align, flags)`, `mem_free`, `mem_realloc`, with flags for fast internal RAM (`BMDS_MEM_FAST`) and DMA-capable memory (`BMDS_MEM_DMA`). picolibc's own malloc (and its fixed `sbrk` heap) is never linked. The kernel tracks every allocation per program and frees what is left at exit.
- Code always runs from PSRAM through the caches; a program that needs a hot loop in fast memory is a later question (TCM is 8 KB).

### D4: libc

- picolibc is made for this (small `stdio`, a clean OS glue layer: `read`, `write`, `close`, `lseek`, `isatty`, `_exit`) and ships in the toolchain for our multilib. Linked statically into each program: a `printf` hello world is 11.5 KB of image.
- picolibc keeps `errno` (and a few other things) in **thread-local storage**, addressed through `tp`. The firmware has no thread-local variables (its TLS area is empty) and FreeRTOS saves `tp` per task, so `_start` points `tp` at the program's own TLS block in its image (one thread per program) without disturbing the kernel. The loader will refuse to run programs if the firmware ever gains TLS (step 3 checks the linker symbols). `gp` stays the kernel's: programs are linked without relaxation or a global pointer.
- A kernel-provided libc would make thousands of functions and every struct in them ABI. Programs carrying their own libc keep the ABI to the syscall table alone, as on Linux.
- newlib-nano also works; picolibc's smaller `printf` and saner glue win.

### D5: instructions

- Programs may use everything every P4 revision executes, the same set the firmware is built with: the base `rv32imafc` with atomics, hard-float single precision (`ilp32f`, so floats pass in FP registers through the table), plus Espressif's **hardware loops** (`xesploop`) and **PIE SIMD** (`xespv2p1`, 128-bit vector registers). GCC 15 doesn't emit the Espressif instructions on its own (checked: plain loops compile the same either way); they come from intrinsics, inline assembly or libraries like esp-dsp, so allowing them costs programs that don't use them nothing.
- FreeRTOS on the P4 saves FPU, hardware-loop and PIE state per task, lazily, and a program runs in its own task (§5), so programs may use all three freely.
- picolibc's multilib for the base set links with programs built for the full set (checked); only the OS glue (`read`, `write`, `lseek`, `close`...) is missing, which is the SDK's job.
- **Zc\*** (smaller code) only works on chip revisions 3 and up (erratum DIG-661; this board is a v1.3). It can become an opt-in later: the loader reads the program's RISC-V arch attribute and refuses Zc* programs on older chips with a clear message, instead of crashing on them.
- picolibc's own linker script makes a fixed-address executable, not a PIE: the SDK's script (§6) replaces it. A `printf` hello world (floats included) is about 11 KB of code.

## 4. The syscall table (frozen once shipped)

- The entry point is `void _start(const bmds_sys_t *sys, int32_t argc, char **argv, char **envp)`; crt0 points `tp` at the program's TLS block, then C stores `sys` in `bmds_sys`, sets `environ`, runs constructors (`__libc_init_array`) and calls `exit(main(...))`. The full startup contract (registers, stack, `gp`/`tp`, FP state, returning from `_start`) is in `sys.h`.
- The table: `{ magic "BMDS", abi_version, count, reserved, slot[] }`; `bmds_sys_has(n)` probes a slot. The SDK calls slots through typed inline functions (`bmds_write()`...), one indirect call each.
- Slot functions are plain C (`ilp32f`), return `int32_t`: ≥ 0 success, negative `BMDS_E*` error codes, a frozen list in `sdk/include/bmds/errno.h` mapped from the kernel's `Error` (kernel `Error` values stay internal).
- **Defined in step 2** (frozen from the first release): 0 `exit`, 1 `read`, 2 `write`, 3 `close`, 4 `seek` (64-bit offset, result through a pointer), 5 `isatty`, 6 `mem_alloc`, 7 `mem_free`, 8 `mem_realloc`: what picolibc's `stdio` and `malloc` need. The rest of v1 lands in step 4.
- **The rest of v1** (names indicative):
  - Discovery: `abi_version`, `slot_count` (also in the table header).
  - Process: `exit`, `getenv`/`environ` (envp is passed in), `interrupted` (has Ctrl-C been pressed), `sleep_us`.
  - fd I/O: `read`, `write`, `close`, `dup2`, `isatty`, `lseek`, `pipe` (maybe v1.1).
  - Files: `open` (`BMDS_O_*` flags mirroring the VFS's `OpenFlag`), `stat`/`fstat` (`bmds_stat_t`, size-prefixed), `mkdir`, `rmdir`, `unlink`, `rename`, `opendir`/`readdir`/`closedir`, `getcwd`, `chdir` (the program's own cwd, inherited).
  - Memory: `mem_alloc`, `mem_free`, `mem_realloc`, `mem_info`.
  - Time: `clock_gettime` (realtime and monotonic, ns), timezone offset.
  - Terminal: `term_mode` get/set (`signals`, `nlToCrNl`, raw input), `term_size`, `term_colors`.
- Every struct starts with `uint32_t size`; unknown flags fail with `BMDS_EINVAL`; a bad pointer from a program is its own problem (no protection), but the kernel validates lengths and handles.
- **ABI lock** (`components/bmds_loader/src/abilock.c`): `_Static_assert`s on slot numbers, struct sizes and offsets (layouts with pointers checked where pointers are 32-bit), error numbers, flag values and format constants, compiled in every firmware build (`main` requires `bmds_loader`) and the host tests. Append-only.

## 5. Running a program

- **Lookup:** a word with a `/` is a path; otherwise built-ins and functions first (as now), then each directory in `$PATH` for `name`, `name.elf` and `name.sh`, in that order (D6). Not found stays `command not found` (127).
- **Launch by content** (one function, `exec(path, argv, envp, fds)`, used by the shell for every file it runs, so scripts and programs are interchangeable everywhere a command goes): the first bytes decide.
  - `\x7fELF` with the BMDS note: load and run (below).
  - `#!` (D7): the first line, up to 255 bytes, is split as Linux does: the interpreter, then at most one optional argument (the rest of the line, trimmed). The command run is `interpreter [arg] path original-args...` (so `argv[0]` of the interpreter is its own path, and the script's path follows), through the same `exec`, nested at most 4 deep (a script whose interpreter is a script; deeper is `too many levels of interpreters`).
  - Interpreter resolution: an existing file is run as such (a program or another script). Scripts written on a PC say `#!/bin/sh` or `#!/usr/bin/env name`, paths BMDS doesn't have, so: if the path doesn't exist and its last component names a built-in (`sh` and, as an alias, `bash`), the built-in runs (`sh` runs the script in a subshell, as `sh file` does today); `/usr/bin/env name` looks `name` up like a command (built-ins, then `$PATH`). Otherwise `interpreter: No such file or directory` (127, as bash).
  - A file ending in `.sh` without `#!` is a shell script: the built-in `sh` runs it, as if it started with `#!/bin/sh`. FAT has no execute bit, so the name is the only other sign of what a file is; BMDS has no `sh` program to point a `#!` at, so scripts shouldn't need one.
  - Anything else is `exec format error` (126). Unlike bash, a text file without `#!` and without `.sh` is not run as a script (decided 2026-10-04): without an execute bit, BMDS can't tell a script from any other text file.
  - With scripts in the launch path, `test -x` (today: directories and `*.sh`) becomes: directories, BMDS ELF files, `*.sh` files, and files starting with `#!`; and `./script.sh` works with no `sh` in front (`storage.md` §5 promised this for Phase 3).
- **Process** (as built in step 3): the shell hands a command whose name has a `/` to its `ProgramLauncher` (an interface in `bmds_kernel`, so neither the shell nor the loader depends on the other; `Shell::Startup::launcher`, set in `main` to a stateless `constinit Loader`). The `Loader` stats and opens the file, runs `ProgramImage::inspect()`, allocates the image (`Psram`, 4096-aligned, whole pages), `load()`s and relocates it in place, and syncs the caches (`esp_cache_msync(C2M | INVALIDATE)`). A `Process` gets a copy of the command's fd table (fds 0-2 and redirections) and the argument block (`argv`, the exported variables as `envp`, in one allocation). The program's task (FreeRTOS name from the file name, the caller's priority) has the stack its note asks for (default 32 KB), in internal RAM when 64 KB would still be left free, else `Psram`. The caller waits on a semaphore, as for a built-in; in a pipeline a program stage runs like any other (several programs at once work: `hello | cat | cat | wc`).
- **System calls find their program** through FreeRTOS thread-local pointer 1 of the program's task (`CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2`; pointer 0 belongs to IDF's pthreads), set before `_start` runs: a `Running` (its `Process`, entry point, exit status, done semaphore). The slots are C functions (`syscalls.cpp`) in a C table in flash (`systable.c`: a C initializer may cast function pointers; a C++ one would need a global constructor).
- **Exit:** returning from `_start` (`main`'s caller does `exit()`) or `exit(n)` records the status, releases the semaphore and suspends the task; the loader then deletes it with the new `Task::stop()` (suspend, wait until off both cores, `vTaskDelete`, then free stack and TCB, as IDF's `vTaskDeleteWithCaps()`), `release()`s the process (fds closed, every allocation freed) and frees the image; the status (& 0xff) becomes `$?`. Problems are reported on the command's stderr as `path: reason` (`No such file or directory`: 127; `Is a directory`, `cannot execute: not an ELF file` and every other format refusal: 126). A crash (illegal instruction, load fault) is IDF's panic and a reboot for now (no protection, as on DOS; a fault hook is an open question).
- **Ctrl-C:** the console's interrupt is raised as now: blocking calls (`read` on the console or a pipe, `sleep`) return `BMDS_EINTR`, `interrupted()` turns true, and the SDK's default makes `main`'s caller exit with 130. A program stuck computing is **killed** on a second Ctrl-C: the kernel deletes its task, but only while it runs program code, not inside a kernel call (a flag set around every slot), so no kernel lock is ever left held.
- **Resources** are owned by the `Process` (allocations, fds; directories with step 4), so cleanup after a kill is the same as after an exit. Each allocation carries a 16-byte header (list links, size, offset back to the raw block) in front, inside padding that keeps the program's pointer aligned (16 bytes by default, up to 4096 on request).
- **Console input:** a program reading the console gets its bytes as they arrive, without echo or line editing (no line discipline yet): `cat` at the prompt shows nothing until it writes back. Terminal modes (a cooked mode with echo, raw keys) come with the step 4 terminal calls.

## 6. The SDK (`sdk/`)

```
sdk/include/bmds/*.h      the ABI (C99): sys.h (the table, startup contract), program.h (the format, the note), errno.h
sdk/crt/crt0.S, crt.c     _start (tp), main's caller, picolibc's POSIX glue over the table
sdk/crt/malloc.c          malloc and friends over mem_alloc
sdk/ld/bmds.ld            the layout: one PT_LOAD RWX at 0, the note (stack size from --defsym), TLS
sdk/cmake/                bmds-toolchain.cmake, BmdsProgram.cmake (bmds_add_program)
sdk/examples/             hello (cat, a benchmark later)
tools/elfcheck/           bmds-elfcheck (built with the host tests); bmds-run later
```

- Building a program: `cmake -DCMAKE_TOOLCHAIN_FILE=sdk/cmake/bmds-toolchain.cmake` with IDF's `riscv32-esp-elf-gcc` (no IDF needed otherwise); see `sdk/README.md`. (A Makefile template can follow if anyone wants one.)
- Two traps found on the way: the bare-metal GCC driver silently drops `-static-pie`, and CMake's Generic platform ignores `CMAKE_EXECUTABLE_SUFFIX` (the helper sets `.elf` per target).
- `bmds-elfcheck` runs `ProgramImage::inspect()`, the loader's own check, and prints what the loader would do (`hello.elf: BMDS program, ABI 1: image 11520 bytes (11364 in the file), entry +0x0, stack 0 (default), 70 relocations, 4 bytes thread-local`).
- **Push-and-run:** `tools/bmds-run prog [args]` sends the file with YMODEM into a RAM disk (`/ram0`, a `RamBlockDevice` in PSRAM, which this also brings) and runs it, printing its output; the card is not needed.

## 7. Phasing

| Step | Content |
|---|---|
| 1 | **Spike:** code copied into PSRAM runs (cache sync, PMP as analyzed); measure PSRAM vs internal code speed. **Done** (§2): PSRAM confirmed. |
| 2 | SDK skeleton: headers, crt0, linker script, toolchain file; `hello` builds on the PC; `bmds-elfcheck`; ABI lock tests. **Done** (format revised, D2). |
| 3 | Loader (`bmds_loader`): validate, load, relocate, sync, run in a task, exit status, cleanup; run by path from the shell. Host tests of the ELF validation and relocation. **Done.** |
| 4 | Syscall table v1 and picolibc glue: fd I/O, files, directories, memory, time, env, terminal; examples `hello`, `cat`, `ls`-like. |
| 5 | Shell: `$PATH` search, programs in pipelines and redirections, Ctrl-C and kill; `exec` dispatch by content with `#!` scripts (built-in `sh` as the interpreter first; programs as interpreters as soon as they run). |
| 6 | RAM disk and push-and-run (`tools/bmds-run`); compatibility corpus of programs built with SDK v1. |

## 8. Resource budget (estimates)

| Item | Flash | RAM |
|---|---|---|
| Loader (ELF checks, relocation, task) | ~3 KB (`ProgramImage` in step 2; linked in step 3) | per program: image in `Psram`, stack (internal if it fits), ~200 B bookkeeping |
| Syscall table v1 + glue to the VFS | ~6 KB | table in flash (rodata) |
| RAM disk | ~1 KB | its blocks in `Psram`, on demand |
| **Total** | **~10 KB** | **no static internal RAM** |

## 9. Testing

- **Host:** `ProgramImage` on hand-built ELF files (each refusal: not ELF, wrong machine/type/ABI, no note, newer ABI, too much stack, unknown flags, interpreter, dynamic, two segments, not at 0, entry outside, stripped, REL, unknown relocation kind, relocation outside its section, bad symbol, PC-relative to an absolute symbol) and their relocation (word, upper immediate with wrap-around, absolute and undefined symbols left alone, bss zeroed, unaligned base refused); the SDK's `hello.elf`, cross-built as an external project when the compiler is found, loaded at 0 (equal to the file's segment) and at two other bases (exactly the patched words differ, by the distance); 3,000 randomly damaged copies refused or loaded without a crash under ASan/UBSan; `bmds-elfcheck hello.elf` as a ctest.
- **Host (step 3):** `Process` (alignment 0-4096, flags, `realloc` keeping contents, unknown flags and bad alignments refused, `release()` freeing everything, checked against the host allocator; fds, EOF, `EBADF`, `ESPIPE`, closing only the program's copy; the argument block; error numbers); the shell's side with a fake launcher (path, arguments, exported environment only, prefix assignments, cwd, pipelines and redirections, 127/126, no launcher).
- **Board (step 3):** `hello`, `cat`, `progtest` pushed with `rz`: arguments with spaces, `return n` and `exit(n)` from 20 calls deep, the environment (exported and `X=y cmd`), `isatty` on a terminal, a pipe and `/dev/null`, floating point, pipelines of three programs, redirections both ways, relative paths (`bin/hello.elf`), the errors above, Ctrl-C while `cat` reads (status 130). A program that leaks 4 MB, or exhausts PSRAM (31 MB, then `NULL`), leaves free memory exactly as it was. The first leak test found the allocation header storing the wrong offset (the loader freed the program's pointer, not the block): the host tests now catch it.
- **Measured (step 3):** launching (stat, open, check, load, relocate, cache sync, task start; the program's run excluded) `cat` 10 ms, `progtest` (13 KB image, 84 patches) 23 ms: first 110 ms, until the loader read each section header, each relocation chunk and each symbol separately (0.4 ms a read from the card) and the 143 KB symbol table of picolibc's statics was in the file. Now the section headers, each relocation section and the symbol table are read whole, the SDK strips what no relocation needs (`objcopy --strip-unneeded`: `progtest.elf` 518 → 40 KB, `name.debug.elf` keeps everything), and the segment starts on a sector boundary (`-z max-page-size=512`). Checking and loading still walk the relocations twice (about 8 and 11 ms): one pass would halve that. The image grew 9.1 KB (396.8 → 405.8 KB; `bmds_loader` 6.5 KB); internal RAM 8 bytes per task (the second thread-local pointer).

## 10. ABI impact

Everything in §4 and §6's headers, the ELF profile in D2 (note format, the patched and the accepted relocation kinds, the 4096-byte load alignment, entry signature, startup state), the stack and memory guarantees, the `$PATH` lookup and the `.elf`/`.sh` order, the `#!` rules (line length, one optional argument, argument order, nesting depth, the `/bin/sh` and `/usr/bin/env` mapping), `.sh` files run by `sh` without `#!`, other text files refused, `test -x`, exit statuses 126/127/130. Each lands with an `ABI:` line.

## 11. Open questions

- A fault hook that turns a program's crash into a status instead of a reboot (IDF's panic handler is not designed for resuming).
- Whether programs get their own console modes (raw keys) restored automatically at exit (yes, planned) and the framebuffer later (Phase 6).
- Dual-core: run programs on the second core by default to keep the shell and services responsive?
- Zc* opt-in for chip revisions ≥ 3 (D5).
- **Programs bigger than memory.** Demand paging (mapping parts of a program from the SD card as it touches them) needs a fault the CPU can restart, and the P4 has none: its MMU maps external memory in 64 KB pages, but a miss is reported as an asynchronous cache-error interrupt that IDF turns into a panic, not a precise exception on the faulting instruction. What would work, if programs ever outgrow 32 MB of PSRAM: **overlays** (DOS-style; sections loaded on first call through linker-generated stubs), a later, opt-in addition to the format. Not needed now: loading is cheap (the card reads 16 MB/s, so a 1 MB program loads in about 65 ms).
- **Keeping recently run programs loaded** (an image cache in spare PSRAM, checked against the file's size and time) would make repeated runs instant; cheap to add after step 3.
