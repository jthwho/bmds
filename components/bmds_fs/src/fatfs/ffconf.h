/**
 * @file      ffconf.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * FatFs configuration for BMDS (thirdparty/fatfs, R0.16). Every option of
 * the release's ffconf.h is here, with why BMDS picks its value. Plain C:
 * ff.c includes it.
 */

#define FFCONF_DEF 80386 /* Must match the release's ffconf.h. */

/*--- Functions -------------------------------------------------------------*/

#define FF_FS_READONLY  0
#define FF_FS_MINIMIZE  0
#define FF_USE_FIND     0 /* The VFS filters names itself (globbing). */
#define FF_USE_MKFS     1 /* `mkfs`, and the host tests format RAM disks. */
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    1 /* f_utime() for `touch`. */
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  0 /* Files are Streams; no f_printf(). */
#define FF_PRINT_LLI    0
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  0

/*--- Names -----------------------------------------------------------------*/

#define FF_CODE_PAGE 437 /* Short names; long names carry everything else. */

/* Long names, with the working buffer from the heap (ff_memalloc: PSRAM),
   since several tasks use the filesystem and stacks are small. */
#define FF_USE_LFN 3
#define FF_MAX_LFN 255

#define FF_LFN_UNICODE 2 /* UTF-8 on the API, as the rest of BMDS. */

/* FILINFO name buffers in bytes: 255 UTF-16 units are at most 765 bytes of
   UTF-8, so any name fits (CVE-2026-6688). */
#define FF_LFN_BUF 765
#define FF_SFN_BUF 12

#define FF_FS_RPATH   0 /* The VFS resolves `.`, `..` and the cwd. */
#define FF_PATH_DEPTH 10

/*--- Volumes ---------------------------------------------------------------*/

#define FF_VOLUMES         4 /* Mounted FAT filesystems at once (Vfs::MaxMounts). */
#define FF_STR_VOLUME_ID   0 /* The VFS maps mounts to drive numbers ("0:"). */
#define FF_VOLUME_STRS     "0", "1", "2", "3"
#define FF_MULTI_PARTITION 0 /* BMDS reads partition tables itself; FatFs mounts a partition device. */
#define FF_MIN_SS          512
#define FF_MAX_SS          512
#define FF_LBA64           1 /* Cards over 2 TB, and GPT. */
#define FF_MIN_GPT         0x10000000
#define FF_USE_TRIM        1 /* Freed clusters are discarded on the card. */

/*--- System ----------------------------------------------------------------*/

/* Each open file has its own sector buffer, so files don't thrash one
   shared window. */
#define FF_FS_TINY 0

#define FF_FS_EXFAT 1 /* SDXC cards come formatted exFAT. */

#define FF_FS_NORTC   0 /* get_fattime() reads the system clock. */
#define FF_NORTC_MON  1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026

#define FF_FS_CRTIME   0
#define FF_FS_NOFSINFO 0

/* At most this many files and directories open at once; FatFs refuses to
   open a file twice for writing or to remove an open one (Error::Busy). */
#define FF_FS_LOCK 16

/* Off: the BMDS FAT layer serializes every FatFs call, including f_mount()
   and f_mkfs(), which FatFs's own locking doesn't cover (a card can be
   unmounted while another task is using it). */
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT   1000

/*--- End of configuration options ------------------------------------------*/
