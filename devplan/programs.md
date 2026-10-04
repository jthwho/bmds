# Programs: Executables, Loader, Syscall ABI v1, SDK

Status: **design** (Phase 3); D1-D7 agreed (2026-10-04; D3 confirmed by the step 1 spike, §2). Step 1 done; nothing else implemented. Once the first program ships, everything marked *frozen* is userspace ABI (`docs/ABI.md`): append-only forever.

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
| D2 | Executable format | **ELF, static PIE** (`ET_DYN`, one contiguous image, only `R_RISCV_RELATIVE` relocations) | custom format converted from ELF at build time; relocatable `ET_REL` |
| D3 | Where code runs | **PSRAM** (the whole image), measured as fast as flash or IRAM (§2); fast internal RAM for data on request | internal SRAM (needs MEMPROT off) |
| D4 | C library | **picolibc, linked into each program**, over a small SDK glue layer | newlib-nano in the program; libc provided by the kernel |
| D5 | Instruction set | **Everything every P4 runs: `rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1`, `ilp32f`** (the firmware's own set) | the base set only; Zc* (rev ≥ 3 only) |
| D6 | File naming | **`name` runs `<dir>/name`, `<dir>/name.elf` or `<dir>/name.sh`** (in that order) from `$PATH` (default `/sd0/p0/bin`); what runs is decided by the file's first bytes: a BMDS ELF, or a `#!` script (D7) | `.elf` required; no extension |
| D7 | Scripts | **`#!` is planned into the launch path** from the start (§5), even if the first release only runs ELF | scripts only through `sh file` |

### D1: why a table

- A call through a table is an indirect call: a few cycles. An `ecall` traps into IDF's exception vector, saves the full context, dispatches, and returns: hundreds of cycles, for no protection today (M-mode only, locked PMP).
- **The table keeps traps possible.** Programs only ever call through the table they were given. If a later chip or IDF allows U-mode, the kernel hands such programs a table of small stubs that `ecall`; their binaries don't change. The reverse (traps now, table later) would need nothing either, but costs the trap price from day one.
- The table is versioned: `{ uint32_t magic, abi_version, count; void *slot[count]; }`. A slot number is frozen forever; a program probes a slot as `slot < count && slot[n] != NULL` (ABI rule 8).

### D2: why static-PIE ELF

- Standard tools make it (`-fPIE -static-pie` with our linker script), standard tools read it (`readelf`, `objdump`, `addr2line`, GDB), and a self-hosted toolchain only needs to emit ELF.
- RISC-V position-independent code addresses its own code and data PC-relative (`auipc`), so a contiguous image loaded anywhere needs relocating only where data holds absolute addresses (pointer tables, function pointers, `const char *` arrays): `R_RISCV_RELATIVE` entries, "add the load address". The loader is about 1-2 KB and supports exactly that; anything else (`R_RISCV_32`, `JUMP_SLOT`, TLS, `DT_NEEDED`) is rejected with an error, never guessed at.
- No dynamic linking, no shared libraries in v1. (A shared SDK library could come later as a new, optional mechanism.)
- **Identification:** `EM_RISCV`, `ELFCLASS32`, little endian, float ABI flag `EF_RISCV_FLOAT_ABI_SINGLE`, and a `PT_NOTE` with name `"BMDS"`, type 1, holding a `bmds_program_note_t` (size, ABI version the program was built for, requested stack size, flags). A file without the note is not a BMDS program (`exec format error`).
- A custom format would be smaller to parse but needs a converter on the PC and in a self-hosted toolchain, and loses the tools. `ET_REL` would make the loader a linker (dozens of relocation types, relaxation): too much to freeze.

### D3: memory

- The image (text, rodata, data, bss) is one `Psram` allocation, aligned to a cache line; the loader copies the `PT_LOAD` segments, zeroes bss, applies relocations, syncs the caches.
- The program's **stack** is allocated from internal RAM if it fits (stacks are hot), else `Psram`: the note asks for a size (default 32 KB, at most 1 MB).
- **Heap:** `malloc` in the program is picolibc's over SDK glue that takes chunks from the kernel (`bmds_mem_alloc(size, flags)`), with a flag for fast internal RAM (`BMDS_MEM_FAST`) and one for DMA-capable memory. The kernel tracks every allocation per program and frees what is left at exit.
- Code always runs from PSRAM through the caches; a program that needs a hot loop in fast memory is a later question (TCM is 8 KB).

### D4: libc

- picolibc is made for this (small, `stdio` without `malloc`, a clean OS glue layer: `read`, `write`, `open`, `close`, `lseek`, `_exit`, `sbrk`-free malloc over a callback) and ships in the toolchain for our multilib. Linked statically into each program: a hello world costs a few KB.
- A kernel-provided libc would make thousands of functions and every struct in them ABI. Programs carrying their own libc keep the ABI to the syscall table alone, as on Linux.
- newlib-nano also works; picolibc's smaller `printf` and saner glue win.

### D5: instructions

- Programs may use everything every P4 revision executes, the same set the firmware is built with: the base `rv32imafc` with atomics, hard-float single precision (`ilp32f`, so floats pass in FP registers through the table), plus Espressif's **hardware loops** (`xesploop`) and **PIE SIMD** (`xespv2p1`, 128-bit vector registers). GCC 15 doesn't emit the Espressif instructions on its own (checked: plain loops compile the same either way); they come from intrinsics, inline assembly or libraries like esp-dsp, so allowing them costs programs that don't use them nothing.
- FreeRTOS on the P4 saves FPU, hardware-loop and PIE state per task, lazily, and a program runs in its own task (§5), so programs may use all three freely.
- picolibc's multilib for the base set links with programs built for the full set (checked); only the OS glue (`read`, `write`, `lseek`, `close`...) is missing, which is the SDK's job.
- **Zc\*** (smaller code) only works on chip revisions 3 and up (erratum DIG-661; this board is a v1.3). It can become an opt-in later: the loader reads the program's RISC-V arch attribute and refuses Zc* programs on older chips with a clear message, instead of crashing on them.
- picolibc's own linker script makes a fixed-address executable, not a PIE: the SDK's script (§6) replaces it. A `printf` hello world (floats included) is about 11 KB of code.

## 4. The syscall table (frozen once shipped)

- The entry point is `void _start(const bmds_sys_t *sys, int argc, char **argv, char **envp)`; crt0 stores `sys`, runs constructors, calls `main`, then `exit(main(...))`.
- Slot functions are plain C (`ilp32f`), return `int32_t`: ≥ 0 success, negative `BMDS_E*` error codes, a frozen list in `sdk/include/bmds/errno.h` mapped from the kernel's `Error` (kernel `Error` values stay internal).
- **v1 slots** (names indicative):
  - Discovery: `abi_version`, `slot_count` (also in the table header).
  - Process: `exit`, `getenv`/`environ` (envp is passed in), `interrupted` (has Ctrl-C been pressed), `sleep_us`.
  - fd I/O: `read`, `write`, `close`, `dup2`, `isatty`, `lseek`, `pipe` (maybe v1.1).
  - Files: `open` (`BMDS_O_*` flags mirroring the VFS's `OpenFlag`), `stat`/`fstat` (`bmds_stat_t`, size-prefixed), `mkdir`, `rmdir`, `unlink`, `rename`, `opendir`/`readdir`/`closedir`, `getcwd`, `chdir` (the program's own cwd, inherited).
  - Memory: `mem_alloc`, `mem_free`, `mem_realloc`, `mem_info`.
  - Time: `clock_gettime` (realtime and monotonic, ns), timezone offset.
  - Terminal: `term_mode` get/set (`signals`, `nlToCrNl`, raw input), `term_size`, `term_colors`.
- Every struct starts with `uint32_t size`; unknown flags fail with `BMDS_EINVAL`; a bad pointer from a program is its own problem (no protection), but the kernel validates lengths and handles.
- **ABI lock tests** (host): slot numbers, struct sizes and offsets, error numbers, flag values. Append-only.

## 5. Running a program

- **Lookup:** a word with a `/` is a path; otherwise built-ins and functions first (as now), then each directory in `$PATH` for `name`, `name.elf` and `name.sh`, in that order (D6). Not found stays `command not found` (127).
- **Launch by content** (one function, `exec(path, argv, envp, fds)`, used by the shell for every file it runs, so scripts and programs are interchangeable everywhere a command goes): the first bytes decide.
  - `\x7fELF` with the BMDS note: load and run (below).
  - `#!` (D7): the first line, up to 255 bytes, is split as Linux does: the interpreter, then at most one optional argument (the rest of the line, trimmed). The command run is `interpreter [arg] path original-args...` (so `argv[0]` of the interpreter is its own path, and the script's path follows), through the same `exec`, nested at most 4 deep (a script whose interpreter is a script; deeper is `too many levels of interpreters`).
  - Interpreter resolution: an existing file is run as such (a program or another script). Scripts written on a PC say `#!/bin/sh` or `#!/usr/bin/env name`, paths BMDS doesn't have, so: if the path doesn't exist and its last component names a built-in (`sh` and, as an alias, `bash`), the built-in runs (`sh` runs the script in a subshell, as `sh file` does today); `/usr/bin/env name` looks `name` up like a command (built-ins, then `$PATH`). Otherwise `interpreter: No such file or directory` (127, as bash).
  - A file ending in `.sh` without `#!` is a shell script: the built-in `sh` runs it, as if it started with `#!/bin/sh`. FAT has no execute bit, so the name is the only other sign of what a file is; BMDS has no `sh` program to point a `#!` at, so scripts shouldn't need one.
  - Anything else is `exec format error` (126). Unlike bash, a text file without `#!` and without `.sh` is not run as a script (decided 2026-10-04): without an execute bit, BMDS can't tell a script from any other text file.
  - With scripts in the launch path, `test -x` (today: directories and `*.sh`) becomes: directories, BMDS ELF files, `*.sh` files, and files starting with `#!`; and `./script.sh` works with no `sh` in front (`storage.md` §5 promised this for Phase 3).
- **Process:** the loader validates and loads the image, then starts a task (the program's stack, the shell's priority) and the shell waits for it, as for a built-in; in a pipeline, a program stage runs like any other stage. The program gets the stage's fd table (fds 0-2 and any redirections), `argv`, the exported variables as `envp`, and the shell's cwd.
- **Exit:** returning from `main` or `exit(n)` ends the task; the kernel then closes the program's fds, frees its memory and image, and the status becomes `$?`. A crash (illegal instruction, load fault) is caught by a BMDS fault hook where IDF allows it; otherwise it is IDF's panic and reboot (no protection, as on DOS).
- **Ctrl-C:** the console's interrupt is raised as now: blocking calls (`read` on the console or a pipe, `sleep`) return `BMDS_EINTR`, `interrupted()` turns true, and the SDK's default makes `main`'s caller exit with 130. A program stuck computing is **killed** on a second Ctrl-C: the kernel deletes its task, but only while it runs program code, not inside a kernel call (a flag set around every slot), so no kernel lock is ever left held.
- **Resources** are owned by a `Program` object (allocations, fds, directories), so cleanup after a kill is the same as after an exit.

## 6. The SDK (`sdk/`)

```
sdk/include/bmds/*.h      the ABI: sys.h (the table), errno.h, types and constants (C99)
sdk/crt/crt0.S, glue.c    entry, picolibc's OS glue over the table
sdk/ld/bmds.ld            the static-PIE layout (one PT_LOAD RWX, PT_NOTE, .rela.dyn)
sdk/cmake/bmds-toolchain.cmake, sdk/make/bmds.mk    build a program on a PC
sdk/examples/             hello, cat, a benchmark
tools/                    bmds-run (push over serial and run), bmds-elfcheck
```

- Building a program: `cmake -DCMAKE_TOOLCHAIN_FILE=sdk/cmake/bmds-toolchain.cmake` or `make -f sdk/make/bmds.mk`, with IDF's `riscv32-esp-elf-gcc` (no IDF needed otherwise).
- `bmds-elfcheck` (also run by the loader's host tests) rejects images the loader would.
- **Push-and-run:** `tools/bmds-run prog [args]` sends the file with YMODEM into a RAM disk (`/ram0`, a `RamBlockDevice` in PSRAM, which this also brings) and runs it, printing its output; the card is not needed.

## 7. Phasing

| Step | Content |
|---|---|
| 1 | **Spike:** code copied into PSRAM runs (cache sync, PMP as analyzed); measure PSRAM vs internal code speed. **Done** (§2): PSRAM confirmed. |
| 2 | SDK skeleton: headers, crt0, linker script, toolchain file; `hello` builds on the PC; `bmds-elfcheck`; ABI lock tests. |
| 3 | Loader (`bmds_loader`): validate, load, relocate, sync, run in a task, exit status, cleanup; run by path from the shell. Host tests of the ELF validation and relocation. |
| 4 | Syscall table v1 and picolibc glue: fd I/O, files, directories, memory, time, env, terminal; examples `hello`, `cat`, `ls`-like. |
| 5 | Shell: `$PATH` search, programs in pipelines and redirections, Ctrl-C and kill; `exec` dispatch by content with `#!` scripts (built-in `sh` as the interpreter first; programs as interpreters as soon as they run). |
| 6 | RAM disk and push-and-run (`tools/bmds-run`); compatibility corpus of programs built with SDK v1. |

## 8. Resource budget (estimates)

| Item | Flash | RAM |
|---|---|---|
| Loader (ELF checks, relocation, task) | ~3 KB | per program: image in `Psram`, stack (internal if it fits), ~200 B bookkeeping |
| Syscall table v1 + glue to the VFS | ~6 KB | table in flash (rodata) |
| RAM disk | ~1 KB | its blocks in `Psram`, on demand |
| **Total** | **~10 KB** | **no static internal RAM** |

## 9. ABI impact

Everything in §4 and §6's headers, the ELF profile in D2 (note format, relocation set, entry signature, startup state), the stack and memory guarantees, the `$PATH` lookup and the `.elf`/`.sh` order, the `#!` rules (line length, one optional argument, argument order, nesting depth, the `/bin/sh` and `/usr/bin/env` mapping), `.sh` files run by `sh` without `#!`, other text files refused, `test -x`, exit statuses 126/127/130. Each lands with an `ABI:` line.

## 10. Open questions

- A fault hook that turns a program's crash into a status instead of a reboot (IDF's panic handler is not designed for resuming).
- Whether programs get their own console modes (raw keys) restored automatically at exit (yes, planned) and the framebuffer later (Phase 6).
- Dual-core: run programs on the second core by default to keep the shell and services responsive?
- Zc* opt-in for chip revisions ≥ 3 (D5).
- **Programs bigger than memory.** Demand paging (mapping parts of a program from the SD card as it touches them) needs a fault the CPU can restart, and the P4 has none: its MMU maps external memory in 64 KB pages, but a miss is reported as an asynchronous cache-error interrupt that IDF turns into a panic, not a precise exception on the faulting instruction. What would work, if programs ever outgrow 32 MB of PSRAM: **overlays** (DOS-style; sections loaded on first call through linker-generated stubs), a later, opt-in addition to the format. Not needed now: loading is cheap (the card reads 16 MB/s, so a 1 MB program loads in about 65 ms).
- **Keeping recently run programs loaded** (an image cache in spare PSRAM, checked against the file's size and time) would make repeated runs instant; cheap to add after step 3.
