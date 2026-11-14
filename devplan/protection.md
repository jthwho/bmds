# Memory protection: programs in U-mode

Designed 2026-11-14 (the user's request: proper userspace protection, without patching ESP-IDF;
working around parts of it is acceptable). Nothing built yet; the hardware facts below were
measured with two spike programs the same day (`docs/platforms/esp32p4.md` §5a).

## 1. Goal

A loaded program can't read or write memory that isn't its own, can't touch the hardware, and
can't hurt the kernel or another program, whether it is buggy or hostile. Its faults end it
(`programs.md` §21), now including the stray store into kernel memory that today corrupts the
system silently. A bad pointer handed to a system call fails with `EFAULT` instead of being
used. The kernel and its tasks keep running in M-mode as now.

**Not in scope:** separate address spaces (the MMU can't: `programs.md` §2), protecting a process
from its own threads (as on Linux), and the kernel itself against its own bugs beyond what
MEMPROT gives today (§4.1 keeps that).

## 2. Facts that shape the design

From reading IDF (v6.1-dev) and measuring on the board (rev 1.3, 2026-11-14):

- **The core has U-mode** (`misa` 0x40901125); `mstatus.MPP` holds U, `MPRV` works (M-mode loads
  and stores with U-mode's PMP checks), `mcounteren` gates the counters.
- **In U-mode the FPU, PIE and the hardware loops work** once enabled. IDF enables them lazily in
  M-mode through an illegal-instruction trap, deciding by `EXT_ILL_CSR` and `mtval`, not `mepc`
  (`vectors.S`), and saves their state in the coprocessor area at the top of the task's stack.
- **Privileged instructions trap in U** (cause 2, `mtval` the instruction): CSRs, `wfi`, the
  ESP counter `mpccr`. `ecall` from U is cause 8.
- **The PMP:** 16 entries, 128-byte granularity, lowest number wins. A locked entry binds M and U
  alike; an unlocked one binds only U; an address no entry matches is allowed for M and denied
  for U. IDF (`cpu_region_protect.c`) locks 14 entries with rights that would let a U-mode
  program read and write all internal RAM (entry 5), the peripherals (15) and the CPU
  subsystem, the CLIC's threshold included (0). **IDF's layout can't isolate anything.**
- **`CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE` decides whether IDF sets the PMP and PMA at all**,
  in the bootloader and in the app (`bootloader_init_mem()`, run on each core by `cpu_start.c`).
  Off, both are left as the ROM set them, for us to configure. On, the bootloader already locks
  PMP 0, 1, 2 and 15 before the app runs.
- **The PMA** (Espressif's, 16 entries, CSRs 0xBC0/0xBD0, lockable) holds attributes for every
  mode: IDF uses all 16 to fault the address space's holes and to make memory cacheable.
- **The APM (HP peripheral PMS)** has a separate allow bit per peripheral for each core in M and in
  U. IDF never touches it on the P4. Clearing a U bit made U-mode loads from that peripheral
  fault (cause 5, measured). Its PSRAM/flash/L2MEM bits gate only uncached access: it can't keep
  a program out of RAM.
- **IDF's trap and switch code assumes every task runs in M-mode:** `rtos_int_exit`
  (`portasm.S`) returns the *interrupted* task's `mstatus` even after switching tasks, so the
  next task would inherit its MPP. The trap entry pushes its frame on whatever SP was current,
  and SP is restored by popping, not from the frame.
- **`mtvec` and `mtvt` are written once at boot** (`cpu_start.c`) and can be replaced later.
  Exceptions enter through `mtvec` (`_panic_handler`); interrupts are hardware-vectored through
  `mtvt` (`_interrupt_handler` for most; the stack guard to `_panic_handler`; the IPC ISR to
  `esp_ipc_isr_handler`). IDF uses `mscratch` only to save it over light sleep.
- **The hardware stack guard watches SP in U-mode too** (measured): U code on a stack outside the
  task's bounds latched its interrupt. Its bounds are reloaded at every switch.
- **On this revision the CLIC threshold is memory-mapped** (`CLIC_INT_THRESH_REG`, 0x20800008,
  per core); `mintthresh` (0x347) only exists from rev 2.
- **A U round trip** (`mret` into U, `ecall` back, 14 registers saved and restored) took 166
  cycles in the spike.
- **No callbacks cross the ABI** except a thread's entry (`thread_start`): signals are taken by
  the SDK (`sig_setup`'s hint word, `sig_take`), audio clients run on their own threads
  (`audio.md`), `game.h`'s callbacks are the SDK's own. Neither the SDK nor any program reads a
  CSR.
- **Where a program's memory is today** (`programs.md` D3, §14.2): the image in the MMU window
  (shared pages plus private ones, contiguous there); the stack in internal RAM when it fits (the
  spike's was at 0x4ff8xxxx), else PSRAM; each `mem_alloc` a separate kernel heap block with the
  kernel's 16-byte header in front; surface buffers are the kernel's (`surface_buffer` hands out
  a pointer). System calls run on the program's stack (12 KB floor, `programs.md` §21).

## 3. Decisions (proposed; the user's to agree)

| # | Question | Proposal | Alternatives |
|---|---|---|---|
| P1 | Who sets the PMP and PMA | **BMDS**: bootloader region protection off, our own setup on both cores at start-up | IDF's layout plus PMP 9/10 (no isolation: §2); patching IDF |
| P2 | Kernel W^X (today's MEMPROT) | **In the PMA**, which binds every mode; the PMP only grants programs | locked PMP entries (would grant U the same); dropping W^X for the kernel |
| P3 | Getting into and out of U | **Our own trap entry in front of IDF's vectors**, every saved task context in M-mode, a trampoline the only way down to U | per-task `mstatus` by wrapping `vTaskSwitchContext` and editing `s11` (depends on `rtos_int_exit`'s registers) |
| P4 | Stacks | **A kernel stack per program thread** (the FreeRTOS stack) plus the program's own stack in its memory | system calls on the user stack (a hostile SP makes the kernel write anywhere) |
| P5 | System calls | **The same table, its slots small stubs that `ecall`** (D1's plan); no binary changes | a new ABI; direct calls kept for "pure" slots (later, by measurement: §10) |
| P6 | A program's memory | **One contiguous range of the MMU window per process** for image, heap and stacks, so it takes few PMP entries (§4.4) | a PMP region per allocation (16 entries can't cover it) |
| P7 | Bad pointers | **Checked at the slot boundary against the process's regions, `EFAULT`** | MPRV copies (§10) |
| P8 | Turning it off | **A setting for measuring** (`programs/protect`, default on): off runs programs in M-mode with the direct table as today | always on |

## 4. Design

### 4.1 Memory map (P1, P2)

- `CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n`. `bmds_board` (hardware truth) gets
  `regionprotect.cpp`, run on each core early (`ESP_SYSTEM_INIT_FN`, core mask both), which:
  - writes the PMA: IDF's hole entries and cacheable range (`esp_cpu_configure_invalid_regions()`,
    Apache-2.0, credited in `docs/THIRD_PARTY.md`), plus kernel W^X: IRAM text RX, the rest of
    internal RAM RW, flash text RX, rodata R. Locked: nothing should change them later.
    **The PMA has 16 entries and IDF's holes use them all**: whether W^X fits, by merging holes
    or giving up the least useful ones, is step 1's first measurement (§8);
  - clears the PMP (all off, unlocked): M-mode is unrestricted by it, U-mode denied everything;
  - sets the APM's U bits for every HP peripheral to 0 on both cores, a second barrier;
  - **refuses U-mode if it finds a locked PMP entry** (an older bootloader that still locks
    0/1/2/15: OTA updates don't replace the bootloader, `update.md`); programs then run in
    M-mode as today, with one log line.
- The PMP's entries, all unlocked, all for U:

  | Entries | What | Rights | Loaded |
  |---|---|---|---|
  | 0 | the stub table (§4.3): one 4 KB-aligned page in flash | R X | once at start-up |
  | 1-3 | the process's window range: base; TOR to the end of its shared code; TOR to the end of the range | R X; R W | when a different process enters U on this core |
  | 4-15 | extra regions: fast internal RAM, surfaces, shared memory (§4.4) | per region | likewise |

  Each process keeps its entries as a small array (`PmpSet`, up to 15 address/config pairs);
  loading one is about 20 CSR writes.

### 4.2 Trap entry, trampoline and kernel stacks (P3, P4)

- **Installed** on each core after IDF's start-up: our `mtvec` entry and our `mtvt` table, both
  in IRAM's code (the CLIC's table load needs execute permission: §13). Every vector goes through
  the same check first: `csrrw t0, mscratch, t0`. `mscratch` is 0 in M-mode and the running
  thread's kernel-side block in U-mode, so a zero means the trap came from M: swap back and jump
  to IDF's handler for that vector (the table is fixed at link time and checked against IDF's
  `_mtvt_table` at start-up). Cost in M-mode: three instructions. **Not an SP swap:** the
  hardware stack guard watches SP every cycle, and SP outside the task's bounds for one
  instruction trips it (§13).
- **From U**, with t0 holding the block: save the user's t0 (from `mscratch`), SP, `mepc`,
  `mcause` and `gp` into the block; stop the guard's monitor; SP = the kernel stack top (kept in
  the block); `mscratch` = 0; `gp` = the kernel's; the guard's bounds to the kernel stack and its
  monitor on. Then:
  - **an interrupt, or an illegal instruction** (it may be IDF's lazy coprocessor enable): point
    `mepc` at the trampoline, set MPP to M, restore the scratch register and jump to IDF's
    handler. IDF saves its frame on the kernel stack, may switch tasks (every saved context is
    now in M-mode, so `rtos_int_exit`'s `mstatus` is right for whichever task resumes) and
    returns to the trampoline. A real illegal instruction reaches IDF's panic path, where the
    §21 wrapper ends the program (it knows the thread is in U from the block);
  - **`ecall`** (cause 8): the system call (§4.3);
  - **a fault** (1, 5, 7, the guard): the thread ends itself by its signal, as §21 does, now
    simply on its kernel stack (no stub, no saved-SP rules).
- **The trampoline** (IRAM, the only way down): interrupts off first; if this core's loaded
  `PmpSet` isn't the thread's process's, load it; stop the guard's monitor; `mscratch` = the
  block; `mepc` = the saved user PC; MPP = U, MPIE = 1; restore `gp` and SP; the guard's bounds
  to the user stack and its monitor on; `mret`. A thread's first entry (program start, `thread_start`) is the trampoline with a
  prepared block.
- **Kernel stacks** are the program thread's FreeRTOS stack (internal RAM, size measured with
  `CONFIG_BMDS_STACK_PROBE`; system calls measured 7.3 KB, so about 8 KB). The coprocessor save
  area and TLS block IDF puts at its top stay kernel memory. The program's own stack moves into
  its window range (§4.4), and its 12 KB floor can go: only program code runs on it.
- **The cache-off window:** a flash write stalls the other core by IPC interrupt. In U-mode that
  interrupt goes through our entry, so the entry, the trampoline and the kernel stacks must be in
  internal RAM, as above.

### 4.3 System calls (P5)

- **The stub table** is one 4 KB flash page (PMP entry 0): the `bmds_sys_t` header and slot array
  as now, each slot pointing at a stub in the same page: `li a7, n; ecall; ret`. A program calls
  `bmds_sys->slot[n](...)` exactly as today. The table the program gets is the stub table (U
  mode) or the direct one (M mode, P8).
- **Dispatch:** the entry checks `a7 < count`, enables interrupts and calls the real slot (the
  direct table) with `a0`-`a7` and `fa0`-`fa7` untouched; `a0`/`a1`/`fa0` come back as the
  result. The stub is a C call to the program, so only callee-saved state must survive, which C
  already guarantees. Arguments passed on the stack (more than eight words, or large structs by
  value) would be read from the wrong stack: the audit (§4.5) lists them; none were found in a
  first pass over `sys.h`'s typedefs.
- **The syscall runs as the thread in ordinary task context** (interrupts on, not in an ISR, may
  block), exactly as today, `KernelCall` and `inKernel` included: the kill handshake stays.
  Leaving is the trampoline, which also takes a pending signal's turn as now.
- **Expected cost:** the spike's 166-cycle round trip less its 14-register save: about 0.3-0.5 µs
  at 360 MHz on top of the slot, next to `kernel.syscall`'s 2.38 µs today. Measured in step 3.

### 4.4 A program's memory (P6)

- **One window range per process**, sized as it grows: the image's shared pages (RX, read-only
  now: a program can no longer write into its shared code, a §14 caveat closed), its private
  pages, then heap and stacks, mapped page by page (64 KB, `psramwindow.md`'s machinery) in one
  contiguous range. Three PMP entries cover all of it.
- **`mem_alloc`/`mmap` allocate inside the range** with the allocation records kept in kernel
  memory, not in a header in front of the program's block (a program could rewrite that header
  today). musl's `malloc` already asks for whole pages.
- **Stacks inside the range:** each user stack's bounds go to the hardware guard while the thread
  is in U (§4.2), so an overflow into the heap below still faults.
- **64 KB pages cost small programs memory** (a BusyBox stage needs image, a stack and a little
  heap). Measured in step 4 against today's allocations; sub-page packing of several small
  stacks and heap in one page is the first remedy.
- **Fast internal RAM** (`BMDS_MEM_FAST`, stacks that fit) can't be in the window: each region
  costs PMP entries (one NAPOT, or two for TOR). Proposed: a process gets at most one fast region,
  sized at its first fast request, else PSRAM.
- **Surfaces and other shared buffers:** either an extra PMP region per buffer (entries 4-15,
  a cap per process), or the buffer's pages mapped into the process's range as an alias (no
  entries, but the alias rules of `programs.md` §2 apply: write back before the compositor or
  DMA reads). Decided in step 4 by measuring both.
- **DMA** isn't checked by the PMP: drivers that DMA into a program's buffer (SD reads) do so
  after the slot's check.

### 4.5 Pointers at the slot boundary (P7)

- A `UserMemory` view of the process's regions answers `contains(ptr, size, access)` in a few
  compares; a slot checks every pointer and length it is given before use (`BMDS_EFAULT`,
  already in `errno.h`). Structs are copied in once (they start with
  `size`), so a second thread can't change them between check and use.
- **Pointers the kernel keeps** (the signal hint word, `thread_clear`'s word, futex words,
  surfaces, audio buffers) are checked when given; a process's memory lives until it ends, so
  they stay valid.
- **The audit** goes slot by slot over all 135: pointer arguments, struct members holding
  pointers, arguments on the stack, writes the kernel makes later, and any place a program's
  code address is called (only `thread_start`'s entry, which now goes through the trampoline).
  Its result is a table in this document.

## 5. Interface

- **Kernel:** `bmds/usermode.h` (`bmds_loader`): `PmpSet`, `UserMemory`, `enterUser()` (a thread's
  first entry), the installer called from `main`. `bmds_board`: the region setup (§4.1).
- **Settings:** `programs/protect` (bool, default true; P8).
- **Userspace:** no new slots. `sys.h`'s startup contract gains: programs run in U-mode, the slots
  are stubs (call them, don't compare them), `cycle`/`instret` are readable (`mcounteren`), other
  CSRs, `wfi` and the hardware fault (`SIGILL`/`SIGSEGV`).

## 6. IDF usage

- **Kept:** FreeRTOS, its trap handlers and context switch (reached through our entry), the lazy
  coprocessor switching, the panic path for kernel faults, the hardware stack guard.
- **Configured:** `CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n` (bootloader and app), so IDF sets
  neither PMP nor PMA. `CONFIG_ESP_SYSTEM_MEMPROT` stays on for its linker alignment of
  `_iram_text_end` (and the heap's refusal of executable allocations), though its PMP setup no
  longer runs.
- **Replaced at run time, not patched:** `mtvec` and `mtvt` on both cores; `mscratch`.
- **Depended on** (to recheck with each IDF update, listed in the platform notes): `vectors.S`
  ignoring `mstatus` and SP in its frame; `_mtvt_table`'s layout; `rtos_int_exit`'s `mstatus`
  rule; the coprocessor save area at the stack's top. Light sleep saves and restores the PMP and
  `mscratch` (not used by BMDS today).

## 7. Resource budget (estimates; measured as steps land)

| What | Cost |
|---|---|
| IRAM | entry, trampoline, PMP load: ~0.6 KB |
| Flash | the stub page: 4 KB (135 stubs × 12 bytes plus the table); the rest ~4 KB |
| Internal RAM | a kernel stack per program thread (~8 KB) — against today's program stacks in internal RAM, which move to PSRAM |
| PSRAM | the window ranges' rounding to 64 KB pages (§4.4) |
| Per process | a `PmpSet` (~128 bytes), the region list |
| CPU | interrupts from M: 3 instructions; from U: ~20 more; system calls ~0.3-0.5 µs more; a switch between processes: ~20 CSR writes |

## 8. Steps

1. **Own the PMP and PMA.** Bootloader region protection off; BMDS's setup on both cores
   (§4.1), still all M-mode. Measure the PMA's rules first: does an `X`-less PMA region stop an
   M-mode fetch, and can W^X fit in 16 entries? Test: boot, the §21 faults (loads at 0, a PMA
   hole), executing from DRAM and writing to IRAM text fault in the kernel, `bench` unchanged.
   The bootloader changes: it travels in update packages, written when the user confirms
   (`update.md` §20), and the firmware runs on either bootloader.
2. **The trap entry in front of IDF**, M-mode only: install, forward, measure the interrupt cost
   (`bench.md` §22's guard measurement as the model), long runs with audio, network and USB
   busy. Nothing in U yet.
3. **Programs in U-mode** with today's memory: kernel stacks, the trampoline, the stub table,
   `ecall` dispatch, faults from U, `PmpSet`s granting a program its image, stack and
   allocations as they are now (as many entries as that takes; programs that need more fall
   back to M-mode with a log line). Measure `kernel.syscall`, `programs.launch`, `kernel.pipe`.
4. **One window range per process** (§4.4): heap, stacks and image in it, records out of the
   program's memory, fast regions, surfaces. Measure memory use against step 3.
5. **The slot audit** (§4.5): checks, `EFAULT`, the table here. A hostile test program passes
   kernel, peripheral and other processes' addresses to every slot.
6. **On by default.** `docs/ABI.md`, `sys.h`'s contract, `docs/ARCHITECTURE.md`, the roadmap.

## 9. Testing

- **Host:** `PmpSet` encoding (NAPOT/TOR, 128-byte rounding, what fits in 16), `UserMemory`
  (edges, overflow, wrap), the slot checks' tables (`bmds_core`, no IDF).
- **Board** (`progtest`, new cases): a store to kernel RAM, a peripheral read, a write into the
  program's own code, a jump into kernel flash, `csrr mstatus`, a stack overflow, a wild SP
  before an interrupt (each ends the program with its signal, the system unharmed); every slot
  with bad pointers (`EFAULT`); FPU, PIE and loops across preemption in U on both cores; a flash
  write while a program computes (the cache-off path); pipelines, Doom and the GUI programs for
  speed against `programs/protect=false`.

## 10. ABI impact

- **No slots, structs or formats change.** Behaviour does: a bad pointer to a slot is `EFAULT`
  instead of corruption or a panic; a store outside the program's memory, a write into its own
  code, a CSR other than the counters, and hardware access end the program with
  `SIGSEGV`/`SIGILL`. None of those were ever part of the ABI (`programs.md` §4: "a bad pointer
  from a program is its own problem"). Programs see the slots as function pointers as before.
- The stack floor (12 KB) can be lowered; `BMDS_THREAD_STACK_MIN` keeps its value.
- Commit with an `ABI:` line (step 3 and step 5).

## 11. Open questions

- **The PMA's budget for kernel W^X** (step 1). If it doesn't fit, which matters more: faulting
  every hole, or W^X for the kernel? Neither affects programs' isolation.
- **Direct calls for pure slots** (text measuring, syntax scanning) to skip the trap: they would
  need kernel flash executable in U (one more entry, and kernel rodata readable). Only if
  measurements ask.
- **MPRV instead of range checks** for copying user buffers: simpler slots, but every access
  through a check-free path pays the mode switch. Range checks first.
- **Surfaces:** PMP regions or aliases (§4.4).
- **Chip revision 3** has the standard CLIC (`mintthresh`, `mintstatus`): the entry and
  trampoline don't use the threshold, but recheck when such a board arrives.
- **Debugging:** IDF's panic output and GDB stub for a U-mode frame (the kernel's frame is what
  they see).

## 12. Step 1: BMDS owns the PMP and PMA (built and tried 2026-11-14)

**Built:**
- `CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n` (`sdkconfig.defaults`, both `sdkconfig`s): IDF
  sets neither PMP nor PMA in the bootloader, the main firmware or recovery. The bootloader
  shrank: 672 bytes free of its 24 KB (48 before).
- `bmds_board/src/regionprotect.cpp`, two start-up hooks kept in every app by a `-u` anchor
  (recovery has no loader to reference them):
  - **core stage, core 0, while core 1 is paused:** an older bootloader's locks (PMA or PMP)
    decide the layout for both cores (`bootloaderLocked`). It runs on core 0 alone, so core 1's
    own entries can't tell;
  - **secondary stage, each core, priority 1 (before the stack guard's 101):** with an older
    bootloader, IDF's own `esp_cpu_configure_region_protection()`, as before
    (`RegionProtection::idfLayout`); otherwise 16 locked PMA entries in address order, each reset,
    its address written and **read back before its rights are set**, then the PMP cleared:

    | # | Range | Rights |
    |---|---|---|
    | 0 | 0 to the CPU subsystem (NAPOT) | none |
    | 1-2 | CPU subsystem's end to the TCM | none |
    | 3-4 | CPU peripherals' end to the flash window | none |
    | 5 | flash text (to `_instruction_reserved_end`, page-rounded) | R X |
    | 6 | the rest of the flash window (rodata, mapped partitions) | R |
    | 7 | the flash window's end to PSRAM | none |
    | 8-9 | ROM's end to internal RAM | none |
    | 10 | IRAM text (to `_iram_text_end`, exactly) | R X |
    | 11 | the rest of internal RAM | R W |
    | 12 | internal RAM's end to the peripherals | none |
    | 13 | flash window to the peripherals, cacheable (PSRAM, the ROM) | R W X |
    | 14-15 | uncached internal RAM's end to 4 GB | none |

    Against IDF's: the four W^X entries (5, 6, 10, 11) are new; two holes no longer fit
    (0x30102000-0x3ff00000 and 0x4c000000-0x4fc00000); the flash window above rodata is read-only
    (IDF's PMP left it unrestricted for M-mode); LP RAM loses IDF's PMP split (text RX, rest RW).
    A split's top that doesn't read back exactly gives the entry after it both sides' rights
    (`flashSplit`, `iramSplit` false, a warning at boot) rather than a boundary in the wrong place.
- `Loader::init` logs one line on an older bootloader, warns on anything else not as designed.
- The bootloader through the Updater (`update.md` §20): packages carry it; written only when the
  user confirms (`update apply --bootloader`, the Updater's question).
- `progtest faults` gains the kernel's W^X: `dramexec`, `iramwrite`, `textwrite`, `rodatawrite`,
  and `top` (just under 4 GB).

**The first try failed, and why** (recovery panicked at once on core 1, the update reported "the
recovery firmware it brought didn't start", nothing else changed): core 1 found no locks (the old
bootloader runs on core 0 alone) and took BMDS's layout while core 0 took IDF's; and its PMA probe
found no free entry in the ROM's state, so the IRAM split was rounded up to 4 KB, which made the
start of `.data` read-only: the stack guard's init faulted storing to `periph_spinlock`. Hence the
core-0 decision and the exact, read-back boundaries above (no probe, no rounding).

**Tried on the board** (2026-11-14, through the updater, no cable):
1. The package without its bootloader (`--force`, the same version): the new firmware and recovery
   on the old bootloader; one log line, IDF's layout on both cores (the probe), the seven faults
   `SIGSEGV` as before.
2. `update apply --bootloader`: recovery wrote it, the board came back, `update check` says the
   same. Both cores: all 16 PMP entries clear and unlocked; the PMA as the table, every boundary
   exact (IRAM text to 0x4ff10600, flash text to 0x40270000; the granularity is 128 bytes, which
   the linker's alignment already meets). No boot warnings. The seven faults `SIGSEGV`, each at its
   address (executing at 0x4ff36c80, writing 0x4ff00100, 0x4011bfcc, 0x402d483c; 0xfffffff0, 0,
   0x46000000).
3. `bench`: `kernel.syscall` getpid 2381 ns (2440 before), `kernel.pipe` 269.8 MB/s (259.3),
   `programs.launch` 5.56 ms on core 0 (5.07 on core 1 before; the benchmark's noisy one).

## 13. Step 2: the trap entry in front of IDF's (built and tried 2026-11-14)

**Built** (`bmds_loader`: `trapentry.S`, `trapentry.cpp`, `CONFIG_BMDS_TRAP_ENTRY`, default on):
- **Stubs**, one per place IDF's table leads (`_panic_handler`, `_interrupt_handler`,
  `esp_ipc_isr_handler`), in IRAM: `csrrw t0, mscratch, t0`; non-zero is a trap from U-mode
  (none yet: restored and passed on the same way); zero, swapped back and `j` to IDF's handler.
- **The table** (`bmds_trap_table`, 48 words) in IRAM's code, fixed at link time from the same
  options as IDF's `vectors_clic.S` (the stack guard's vector, the IPC interrupt's). A core-stage
  hook on core 0 checks it against IDF's `_mtvt_table` vector by vector; a secondary-stage hook on
  each core (priority 2, after the region setup) checks that `mtvec`/`mtvt` are still IDF's, then,
  interrupts off, sets `mscratch` = 0, `mtvt` = the table, `mtvec` = the exception stub with
  IDF's mode bits. Anything unexpected leaves IDF's vectors and logs one line (`Loader::init`).
- IRAM: 288 bytes (four 24-byte stubs, the 192-byte table) plus up to 64 of alignment.

**Two failures first, each ending in recovery (no network; reached over its serial console from
another machine: `bmdsboard.py`'s `rz` and `update apply --force` of a firmware-only package):**
1. **The table in internal data RAM:** core 0 took an instruction access fault at the table's
   entry 1 (`mepc` = `mtval` = 0x4ff21604) at the first interrupt. **The CLIC's load of a
   vector's address is checked as an instruction fetch:** the table needs execute permission,
   which BMDS's PMA gives only IRAM's code (as IDF's own table is placed).
2. **`csrrw sp, mscratch, sp`** (the usual trick): "Stack protection fault" in `ipc0` at its
   first interrupt. For one instruction SP was 0, outside the task's bounds, and the hardware
   stack guard watches SP every cycle. Hence t0, and the U-mode path stops the monitor before SP
   moves (§4.2).

**Tried on the board** (0.2.0+049b8cbb, both cores: `mtvec` 0x4ff03443, `mtvt` 0x4ff034c0,
`mscratch` 0):
- `progtest faults` (13 kinds, on the main thread and a second): signals as before; the stack
  guard's overflow (vector 43) and exceptions both through the stubs; no panic.
- `bmds-compat` (the v1 corpus): all pass. `bmds-netstress` tcp and flood (5.1/6.8 MB/s, the ring
  never dry) while 40 rounds of pipelines and stack-overflow faults ran: pass, no panic.
- `bench` against the same set just before on step 1: `kernel.switch` 11.64/8.68 µs (11.43/8.60;
  the earlier runs span 11.04-11.60), `kernel.syscall`, `kernel.pipe`, `kernel.sync` within 1%,
  `surface.present` within 2%: three instructions a trap don't show.
