/**
 * @file      abilock.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ABI lock: the userspace ABI's numbers and layouts, checked at
 * compile time in every firmware build (and the host tests). These
 * assertions are append-only: one that fails means the change broke
 * userspace, not that the assertion is wrong (docs/ABI.md).
 */

#include <stddef.h>
#include <stdint.h>
#include <bmds/errno.h>
#include <bmds/program.h>
#include <bmds/sys.h>

// --- bmds/sys.h -----------------------------------------------------------

_Static_assert(BMDS_SYS_MAGIC == 0x53444d42u, "ABI: BMDS_SYS_MAGIC");
_Static_assert(BMDS_SYS_EXIT == 0, "ABI: slot EXIT");
_Static_assert(BMDS_SYS_READ == 1, "ABI: slot READ");
_Static_assert(BMDS_SYS_WRITE == 2, "ABI: slot WRITE");
_Static_assert(BMDS_SYS_CLOSE == 3, "ABI: slot CLOSE");
_Static_assert(BMDS_SYS_SEEK == 4, "ABI: slot SEEK");
_Static_assert(BMDS_SYS_ISATTY == 5, "ABI: slot ISATTY");
_Static_assert(BMDS_SYS_MEM_ALLOC == 6, "ABI: slot MEM_ALLOC");
_Static_assert(BMDS_SYS_MEM_FREE == 7, "ABI: slot MEM_FREE");
_Static_assert(BMDS_SYS_MEM_REALLOC == 8, "ABI: slot MEM_REALLOC");
_Static_assert(BMDS_STDIN == 0 && BMDS_STDOUT == 1 && BMDS_STDERR == 2, "ABI: standard fds");
_Static_assert(BMDS_SEEK_SET == 0 && BMDS_SEEK_CUR == 1 && BMDS_SEEK_END == 2, "ABI: whence");
_Static_assert(BMDS_MEM_FAST == 1 && BMDS_MEM_DMA == 2 && BMDS_MEM_ALIGN == 16, "ABI: memory flags");

_Static_assert(offsetof(bmds_sys_t, magic) == 0, "ABI: bmds_sys_t.magic");
_Static_assert(offsetof(bmds_sys_t, abi_version) == 4, "ABI: bmds_sys_t.abi_version");
_Static_assert(offsetof(bmds_sys_t, count) == 8, "ABI: bmds_sys_t.count");
_Static_assert(offsetof(bmds_sys_t, reserved) == 12, "ABI: bmds_sys_t.reserved");
_Static_assert(offsetof(bmds_sys_t, slot) == 16, "ABI: bmds_sys_t.slot");
#if UINTPTR_MAX == 0xffffffffu
_Static_assert(sizeof(bmds_sys_fn_t) == 4, "ABI: slot size");
#endif

// --- bmds/program.h -------------------------------------------------------

_Static_assert(BMDS_NOTE_PROGRAM == 1, "ABI: note type");
_Static_assert(sizeof(BMDS_NOTE_NAME) == 5, "ABI: note name");
_Static_assert(BMDS_DEFAULT_STACK_SIZE == 32768, "ABI: default stack");
_Static_assert(BMDS_MAX_STACK_SIZE == 1048576, "ABI: largest stack");
_Static_assert(BMDS_LOAD_ALIGN == 4096, "ABI: load alignment");
_Static_assert(sizeof(bmds_program_note_t) == 16, "ABI: bmds_program_note_t size");
_Static_assert(offsetof(bmds_program_note_t, size) == 0, "ABI: note.size");
_Static_assert(offsetof(bmds_program_note_t, abi_version) == 4, "ABI: note.abi_version");
_Static_assert(offsetof(bmds_program_note_t, stack_size) == 8, "ABI: note.stack_size");
_Static_assert(offsetof(bmds_program_note_t, flags) == 12, "ABI: note.flags");

// --- bmds/errno.h ---------------------------------------------------------

_Static_assert(BMDS_EPERM == 1 && BMDS_ENOENT == 2 && BMDS_EINTR == 4 && BMDS_EIO == 5, "ABI: errno 1-5");
_Static_assert(BMDS_ENOEXEC == 8 && BMDS_EBADF == 9 && BMDS_EAGAIN == 11 && BMDS_ENOMEM == 12, "ABI: errno 8-12");
_Static_assert(BMDS_EACCES == 13 && BMDS_EFAULT == 14 && BMDS_EBUSY == 16 && BMDS_EEXIST == 17, "ABI: errno 13-17");
_Static_assert(BMDS_EXDEV == 18 && BMDS_ENODEV == 19 && BMDS_ENOTDIR == 20 && BMDS_EISDIR == 21, "ABI: errno 18-21");
_Static_assert(BMDS_EINVAL == 22 && BMDS_ENFILE == 23 && BMDS_EMFILE == 24 && BMDS_ENOTTY == 25, "ABI: errno 22-25");
_Static_assert(BMDS_EFBIG == 27 && BMDS_ENOSPC == 28 && BMDS_ESPIPE == 29 && BMDS_EROFS == 30, "ABI: errno 27-30");
_Static_assert(BMDS_EPIPE == 32 && BMDS_ERANGE == 34 && BMDS_ENOSYS == 88 && BMDS_ENOTEMPTY == 90, "ABI: errno 32-90");
_Static_assert(BMDS_ENAMETOOLONG == 91 && BMDS_ETIMEDOUT == 116 && BMDS_ENOTSUP == 134, "ABI: errno 91-134");
_Static_assert(BMDS_ENOMEDIUM == 135 && BMDS_EILSEQ == 138 && BMDS_EOVERFLOW == 139, "ABI: errno 135-139");
_Static_assert(BMDS_ECANCELED == 140, "ABI: errno 140");
