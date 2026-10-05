# musl (vendored)

musl, <https://musl.libc.org/>, the C library BMDS programs link statically
(see `devplan/programs.md` §12). License: `musl/COPYRIGHT` (MIT). Built by
`sdk/cmake/BmdsLibc.cmake` following musl's Makefile.

- **Version:** 1.2.5 (`musl-1.2.5.tar.gz`, sha256
  `a9a118bbe84d8764da0ea0d28b3ab3fae8477fc7e4085d90102b8596fc7c75e4`).
- **Not vendored:** the other architectures (`arch/*` but `riscv32` and
  `generic`, and every `src/*/<arch>` directory but `riscv32`), `crt/` and
  `ldso/` (BMDS has its own startup code and no dynamic linker), `compat/`
  (time32, which riscv32 never had) and `dist/`.
- **Changes** (`patches/bmds.diff`, against the pruned release):
  - `arch/riscv32/syscall_arch.h`: system calls go to `__bmds_syscall()`
    (`sdk/crt/linux.c`), not `ecall`.
  - `src/setjmp/riscv32/setjmp.S`, `longjmp.S`: with single-precision float
    registers (`ilp32f`, `__riscv_flen == 32`) `fs0`-`fs11` are saved with
    `fsw`/`flw`; double with `fsd`/`fld` as before.
  - Removed `src/thread/riscv32/clone.s`, `syscall_cp.s`, `__unmapself.s` and
    `src/signal/riscv32/restore.s`, which issue `ecall` themselves: musl's
    generic C versions take their place (`clone` returns `ENOSYS`).
- **Not built:** `src/process/vfork.c` (generic: `fork`) and
  `src/thread/clone.c` (`ENOSYS`); the SDK's `crt/vfork.S` has `vfork` and
  `__clone` for `CLONE_VM|CLONE_VFORK` (`posix_spawn`, `system`, `popen`).

To update: unpack the new release, prune it the same way, apply
`patches/bmds.diff` (fixing what no longer applies), regenerate the diff
against the pruned release, and compare `arch/riscv32/syscall_arch.h` and
the riscv32 assembly for new `ecall`s.
