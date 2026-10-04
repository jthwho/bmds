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
_Static_assert(BMDS_SYS_OPEN == 9, "ABI: slot OPEN");
_Static_assert(BMDS_SYS_STAT == 10, "ABI: slot STAT");
_Static_assert(BMDS_SYS_FSTAT == 11, "ABI: slot FSTAT");
_Static_assert(BMDS_SYS_MKDIR == 12, "ABI: slot MKDIR");
_Static_assert(BMDS_SYS_RMDIR == 13, "ABI: slot RMDIR");
_Static_assert(BMDS_SYS_UNLINK == 14, "ABI: slot UNLINK");
_Static_assert(BMDS_SYS_RENAME == 15, "ABI: slot RENAME");
_Static_assert(BMDS_SYS_OPENDIR == 16, "ABI: slot OPENDIR");
_Static_assert(BMDS_SYS_READDIR == 17, "ABI: slot READDIR");
_Static_assert(BMDS_SYS_CLOSEDIR == 18, "ABI: slot CLOSEDIR");
_Static_assert(BMDS_SYS_GETCWD == 19, "ABI: slot GETCWD");
_Static_assert(BMDS_SYS_CHDIR == 20, "ABI: slot CHDIR");
_Static_assert(BMDS_SYS_DUP == 21, "ABI: slot DUP");
_Static_assert(BMDS_SYS_DUP2 == 22, "ABI: slot DUP2");
_Static_assert(BMDS_SYS_CLOCK_GET == 23, "ABI: slot CLOCK_GET");
_Static_assert(BMDS_SYS_SLEEP == 24, "ABI: slot SLEEP");
_Static_assert(BMDS_SYS_INTERRUPTED == 25, "ABI: slot INTERRUPTED");
_Static_assert(BMDS_SYS_TERM_GET == 26, "ABI: slot TERM_GET");
_Static_assert(BMDS_SYS_TERM_SET == 27, "ABI: slot TERM_SET");
_Static_assert(BMDS_SYS_MEM_INFO == 28, "ABI: slot MEM_INFO");
_Static_assert(BMDS_SYS_RANDOM == 29, "ABI: slot RANDOM");
_Static_assert(BMDS_SYS_SET_MTIME == 30, "ABI: slot SET_MTIME");
_Static_assert(BMDS_SYS_FTRUNCATE == 31, "ABI 2: slot FTRUNCATE");
_Static_assert(BMDS_SYS_SYMLINK == 32, "ABI 3: slot SYMLINK");
_Static_assert(BMDS_SYS_READLINK == 33, "ABI 3: slot READLINK");
_Static_assert(BMDS_SYS_LSTAT == 34, "ABI 3: slot LSTAT");
_Static_assert(BMDS_TYPE_LINK == 4, "ABI 3: file type LINK");
_Static_assert(BMDS_ABI_VERSION == 3, "ABI: version (raise it with every addition)");
_Static_assert(BMDS_STDIN == 0 && BMDS_STDOUT == 1 && BMDS_STDERR == 2, "ABI: standard fds");
_Static_assert(BMDS_SEEK_SET == 0 && BMDS_SEEK_CUR == 1 && BMDS_SEEK_END == 2, "ABI: whence");
_Static_assert(BMDS_MEM_FAST == 1 && BMDS_MEM_DMA == 2 && BMDS_MEM_ALIGN == 16, "ABI: memory flags");

_Static_assert(BMDS_PATH_MAX == 1024 && BMDS_NAME_MAX == 765, "ABI: path and name lengths");
_Static_assert(BMDS_O_READ == 0x01 && BMDS_O_WRITE == 0x02 && BMDS_O_CREATE == 0x04, "ABI: open flags");
_Static_assert(BMDS_O_TRUNC == 0x08 && BMDS_O_APPEND == 0x10 && BMDS_O_EXCL == 0x20, "ABI: open flags");
_Static_assert(BMDS_TYPE_FILE == 1 && BMDS_TYPE_DIRECTORY == 2 && BMDS_TYPE_DEVICE == 3, "ABI: file types");
_Static_assert(BMDS_ATTR_READONLY == 0x01 && BMDS_ATTR_HIDDEN == 0x02, "ABI: attributes");
_Static_assert(BMDS_ATTR_SYSTEM == 0x04 && BMDS_ATTR_ARCHIVE == 0x20, "ABI: attributes");
_Static_assert(BMDS_CLOCK_REALTIME == 0 && BMDS_CLOCK_MONOTONIC == 1, "ABI: clocks");
_Static_assert(BMDS_TERM_SIGNALS == 0x01 && BMDS_TERM_ONLCR == 0x02 && BMDS_TERM_COLORS == 0x04, "ABI: term flags");
_Static_assert(BMDS_TERM_LOGS == 0x08 && BMDS_TERM_LINE == 0x10, "ABI: term flags");

// ABI 1's bmds_stat_t was 32 bytes; ABI 2 appended to it (rule 4).
_Static_assert(offsetof(bmds_stat_t, file_id) == 32, "ABI: bmds_stat_t ABI 1 size");
_Static_assert(offsetof(bmds_stat_t, size) == 0, "ABI: stat.size");
_Static_assert(offsetof(bmds_stat_t, type) == 4, "ABI: stat.type");
_Static_assert(offsetof(bmds_stat_t, attributes) == 8, "ABI: stat.attributes");
_Static_assert(offsetof(bmds_stat_t, open_flags) == 12, "ABI: stat.open_flags");
_Static_assert(offsetof(bmds_stat_t, file_size) == 16, "ABI: stat.file_size");
_Static_assert(offsetof(bmds_stat_t, modified_ns) == 24, "ABI: stat.modified_ns");
_Static_assert(offsetof(bmds_stat_t, device) == 40, "ABI 2: stat.device");
_Static_assert(offsetof(bmds_stat_t, reserved) == 44, "ABI 2: stat.reserved");
_Static_assert(sizeof(bmds_stat_t) == 48, "ABI 2: bmds_stat_t size");
_Static_assert(sizeof(bmds_term_t) == 16, "ABI: bmds_term_t size");
_Static_assert(offsetof(bmds_term_t, size) == 0, "ABI: term.size");
_Static_assert(offsetof(bmds_term_t, flags) == 4, "ABI: term.flags");
_Static_assert(offsetof(bmds_term_t, columns) == 8, "ABI: term.columns");
_Static_assert(offsetof(bmds_term_t, rows) == 10, "ABI: term.rows");
_Static_assert(offsetof(bmds_term_t, colors) == 12, "ABI: term.colors");
_Static_assert(sizeof(bmds_mem_info_t) == 24, "ABI: bmds_mem_info_t size");
_Static_assert(offsetof(bmds_mem_info_t, size) == 0, "ABI: mem_info.size");
_Static_assert(offsetof(bmds_mem_info_t, total_bytes) == 4, "ABI: mem_info.total_bytes");
_Static_assert(offsetof(bmds_mem_info_t, free_bytes) == 8, "ABI: mem_info.free_bytes");
_Static_assert(offsetof(bmds_mem_info_t, largest_free) == 12, "ABI: mem_info.largest_free");
_Static_assert(offsetof(bmds_mem_info_t, program_bytes) == 16, "ABI: mem_info.program_bytes");
_Static_assert(offsetof(bmds_mem_info_t, program_blocks) == 20, "ABI: mem_info.program_blocks");

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
_Static_assert(BMDS_ELOOP == 92, "ABI 3: errno ELOOP");
