/**
 * @file      sys.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The BMDS system call table: how a program calls the OS.
 *
 * The kernel passes a pointer to a `bmds_sys_t` to the program's entry
 * point; the SDK's startup code keeps it in `bmds_sys`. Each system call
 * is a slot in the table: a plain C function (RISC-V ilp32f calling
 * convention). Slot numbers never change; new calls take new slots.
 *
 * Results: a slot returning `int32_t` gives a value >= 0 on success and a
 * negated `BMDS_E...` error number on failure.
 *
 * Program startup state (the kernel's side of the contract):
 * - `_start(const bmds_sys_t *sys, int32_t argc, char **argv, char **envp)`:
 *   a0-a3 as named; `argv[argc]` and the end of `envp` are NULL.
 * - sp: the top of the program's stack, 16-byte aligned, at least the size
 *   its note asks for (`bmds_program_note_t`).
 * - gp belongs to the kernel: a program must never change it (build with
 *   relaxation off and no global pointer, as the SDK does). tp is the
 *   program's own (for its thread-local storage); the kernel uses none.
 * - The floating-point state is clear; the program's memory image is
 *   loaded, relocated and its bss is zero.
 * - Returning from `_start` ends the program with a0 as its status.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_SYS_H
#define BMDS_SYS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief `bmds_sys_t::magic`: "BMDS" in memory order. */
#define BMDS_SYS_MAGIC 0x53444d42u

/**
 * @brief The ABI version this SDK describes. A kernel implements every
 *        version up to its `bmds_sys_t::abi_version`.
 */
#define BMDS_ABI_VERSION 2u

/** @brief A slot, before it is cast to its function's type. */
typedef void (*bmds_sys_fn_t)(void);

/** @brief The table the kernel passes to `_start`. */
typedef struct bmds_sys {
                uint32_t            magic;       /**< `BMDS_SYS_MAGIC`. */
                uint32_t            abi_version; /**< Highest ABI version the kernel implements. */
                uint32_t            count;       /**< Number of slots. */
                uint32_t            reserved;    /**< Zero. */
                const bmds_sys_fn_t slot[];      /**< The calls; an entry may be NULL (not provided). */
} bmds_sys_t;

/** @brief The table, set by the startup code before anything else runs. */
extern const bmds_sys_t *bmds_sys;

/** @brief Returns nonzero if this kernel provides slot @p n. */
static inline int bmds_sys_has(uint32_t n) {
        return n < bmds_sys->count && bmds_sys->slot[n] != 0;
}

/* --- Slot numbers ---------------------------------------------------- */

#define BMDS_SYS_EXIT 0u         /**< `void exit(int32_t status)`: does not return. */
#define BMDS_SYS_READ 1u         /**< `int32_t read(int32_t fd, void *buf, uint32_t size)`. */
#define BMDS_SYS_WRITE 2u        /**< `int32_t write(int32_t fd, const void *buf, uint32_t size)`. */
#define BMDS_SYS_CLOSE 3u        /**< `int32_t close(int32_t fd)`. */
#define BMDS_SYS_SEEK 4u         /**< `int32_t seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position)`. */
#define BMDS_SYS_ISATTY 5u       /**< `int32_t isatty(int32_t fd)`: 1 terminal, 0 not, or an error. */
#define BMDS_SYS_MEM_ALLOC 6u    /**< `void *mem_alloc(uint32_t size, uint32_t align, uint32_t flags)`. */
#define BMDS_SYS_MEM_FREE 7u     /**< `void mem_free(void *ptr)`. */
#define BMDS_SYS_MEM_REALLOC 8u  /**< `void *mem_realloc(void *ptr, uint32_t size, uint32_t flags)`. */
#define BMDS_SYS_OPEN 9u         /**< `int32_t open(const char *path, uint32_t flags)`: a new fd. */
#define BMDS_SYS_STAT 10u        /**< `int32_t stat(const char *path, bmds_stat_t *info)`. */
#define BMDS_SYS_FSTAT 11u       /**< `int32_t fstat(int32_t fd, bmds_stat_t *info)`. */
#define BMDS_SYS_MKDIR 12u       /**< `int32_t mkdir(const char *path)`. */
#define BMDS_SYS_RMDIR 13u       /**< `int32_t rmdir(const char *path)`. */
#define BMDS_SYS_UNLINK 14u      /**< `int32_t unlink(const char *path)`. */
#define BMDS_SYS_RENAME 15u      /**< `int32_t rename(const char *from, const char *to)`. */
#define BMDS_SYS_OPENDIR 16u     /**< `int32_t opendir(const char *path)`: a directory handle. */
#define BMDS_SYS_READDIR 17u     /**< `int32_t readdir(int32_t dir, bmds_stat_t *info, char *name, uint32_t size)`. */
#define BMDS_SYS_CLOSEDIR 18u    /**< `int32_t closedir(int32_t dir)`. */
#define BMDS_SYS_GETCWD 19u      /**< `int32_t getcwd(char *buf, uint32_t size)`: the length. */
#define BMDS_SYS_CHDIR 20u       /**< `int32_t chdir(const char *path)`. */
#define BMDS_SYS_DUP 21u         /**< `int32_t dup(int32_t fd, int32_t min_fd)`: a new fd. */
#define BMDS_SYS_DUP2 22u        /**< `int32_t dup2(int32_t fd, int32_t to)`. */
#define BMDS_SYS_CLOCK_GET 23u   /**< `int32_t clock_get(uint32_t clock, int64_t *ns)`. */
#define BMDS_SYS_SLEEP 24u       /**< `int32_t sleep(int64_t ns)`. */
#define BMDS_SYS_INTERRUPTED 25u /**< `int32_t interrupted(void)`: 1 after Ctrl-C. */
#define BMDS_SYS_TERM_GET 26u    /**< `int32_t term_get(int32_t fd, bmds_term_t *term)`. */
#define BMDS_SYS_TERM_SET 27u    /**< `int32_t term_set(int32_t fd, uint32_t flags)`. */
#define BMDS_SYS_MEM_INFO 28u    /**< `int32_t mem_info(uint32_t flags, bmds_mem_info_t *info)`. */
#define BMDS_SYS_RANDOM 29u      /**< `int32_t random(void *buf, uint32_t size)`. */
#define BMDS_SYS_SET_MTIME 30u   /**< `int32_t set_mtime(const char *path, int64_t ns)`. */
/* ABI 2 */
#define BMDS_SYS_FTRUNCATE 31u   /**< `int32_t ftruncate(int32_t fd, int64_t size)`. */

/* --- Constants ------------------------------------------------------- */

#define BMDS_STDIN 0  /**< Standard input. */
#define BMDS_STDOUT 1 /**< Standard output. */
#define BMDS_STDERR 2 /**< Standard error. */

#define BMDS_SEEK_SET 0u /**< From the start. */
#define BMDS_SEEK_CUR 1u /**< From the current position. */
#define BMDS_SEEK_END 2u /**< From the end. */

/** @brief `mem_alloc`/`mem_realloc` flags. */
#define BMDS_MEM_FAST 0x1u /**< Internal RAM (fast, scarce) rather than PSRAM. */
#define BMDS_MEM_DMA 0x2u  /**< Memory peripherals can transfer to and from. */

/** @brief Alignment of `mem_alloc` with align 0, and of `mem_realloc` (that of max_align_t). */
#define BMDS_MEM_ALIGN 16u

/** @brief The longest path in bytes (UTF-8), without its terminating NUL. */
#define BMDS_PATH_MAX 1024u

/** @brief The longest file name in bytes (UTF-8), without its terminating NUL. */
#define BMDS_NAME_MAX 765u

/** @brief `open` flags: at least one of `BMDS_O_READ` and `BMDS_O_WRITE`. */
#define BMDS_O_READ 0x01u   /**< Open for reading. */
#define BMDS_O_WRITE 0x02u  /**< Open for writing. */
#define BMDS_O_CREATE 0x04u /**< Create the file if it doesn't exist (with `BMDS_O_WRITE`). */
#define BMDS_O_TRUNC 0x08u  /**< Empty an existing file (with `BMDS_O_WRITE`). */
#define BMDS_O_APPEND 0x10u /**< Every write goes to the end. */
#define BMDS_O_EXCL 0x20u   /**< With `BMDS_O_CREATE`: fail with `BMDS_EEXIST` if it exists. */

/** @brief `bmds_stat_t::type`. */
#define BMDS_TYPE_FILE 1u      /**< A regular file. */
#define BMDS_TYPE_DIRECTORY 2u /**< A directory (also `/`, `/dev` and mount points). */
#define BMDS_TYPE_DEVICE 3u    /**< A device (`/dev/null`, the console), a pipe. */

/** @brief `bmds_stat_t::attributes` (the FAT attributes). */
#define BMDS_ATTR_READONLY 0x01u /**< Read-only. */
#define BMDS_ATTR_HIDDEN 0x02u   /**< Hidden. */
#define BMDS_ATTR_SYSTEM 0x04u   /**< System. */
#define BMDS_ATTR_ARCHIVE 0x20u  /**< Changed since last backed up. */

/**
 * @brief What `stat`, `fstat` and `readdir` tell about a file.
 *
 * The caller sets `size` to `sizeof(bmds_stat_t)`; the kernel fills the
 * fields that fit in it and leaves `size` alone.
 */
typedef struct bmds_stat_t {
                uint32_t size;        /**< Set by the caller: `sizeof(bmds_stat_t)`. */
                uint32_t type;        /**< `BMDS_TYPE_...`. */
                uint32_t attributes;  /**< `BMDS_ATTR_...`. */
                uint32_t open_flags;  /**< `fstat`: the fd's `BMDS_O_READ`/`BMDS_O_WRITE`; else 0. */
                uint64_t file_size;   /**< Bytes (0 for directories and devices). */
                int64_t  modified_ns; /**< Last modified, ns since 1970-01-01 UTC; 0 if unknown. */
                /* ABI 2 (0 from an ABI 1 kernel) */
                uint64_t file_id;  /**< The same for each name and fd of one file, different for others; 0 if unknown. */
                uint32_t device;   /**< The filesystem it is on (non-zero), or 0 (devices, pipes, `/`, `/dev`). */
                uint32_t reserved; /**< Zero. */
} bmds_stat_t;

/** @brief `clock_get` clocks. */
#define BMDS_CLOCK_REALTIME 0u  /**< Wall time, ns since 1970-01-01 UTC (may jump when set). */
#define BMDS_CLOCK_MONOTONIC 1u /**< ns since boot, never goes back. */

/** @brief Terminal modes (`term_get`, `term_set`); a program starts with all set. */
#define BMDS_TERM_SIGNALS 0x01u /**< Ctrl-C interrupts the program instead of being read. */
#define BMDS_TERM_ONLCR 0x02u   /**< Output `\n` is sent as CR LF where the terminal needs it. */
#define BMDS_TERM_COLORS 0x04u  /**< Output colors are fitted to what the terminal shows. */
#define BMDS_TERM_LOGS 0x08u    /**< The kernel's log lines are shown. */
#define BMDS_TERM_LINE 0x10u    /**< Input is read in lines, echoed and editable (else raw bytes). */

/**
 * @brief A terminal (`term_get`). The caller sets `size`, the kernel the rest
 *        (the fields that fit).
 */
typedef struct bmds_term {
                uint32_t size;    /**< Set by the caller: `sizeof(bmds_term_t)`. */
                uint32_t flags;   /**< `BMDS_TERM_...` now in effect. */
                uint16_t columns; /**< Width in character cells. */
                uint16_t rows;    /**< Height. */
                uint32_t colors;  /**< Colors it shows: 0 (none), 16, 256 or 16777216. */
} bmds_term_t;

/**
 * @brief Memory (`mem_info`): for the memory `mem_alloc` with the same flags
 *        uses, and what this program holds. The caller sets `size`.
 */
typedef struct bmds_mem_info_t {
                uint32_t size;           /**< Set by the caller: `sizeof(bmds_mem_info_t)`. */
                uint32_t total_bytes;    /**< The memory's capacity. */
                uint32_t free_bytes;     /**< Free now (for everyone). */
                uint32_t largest_free;   /**< The largest allocation that could succeed now. */
                uint32_t program_bytes;  /**< Allocated by this program and not freed (all memories). */
                uint32_t program_blocks; /**< How many allocations that is. */
} bmds_mem_info_t;

/* --- Calls ----------------------------------------------------------- */

typedef void (*bmds_sys_exit_fn)(int32_t status);
typedef int32_t (*bmds_sys_read_fn)(int32_t fd, void *buf, uint32_t size);
typedef int32_t (*bmds_sys_write_fn)(int32_t fd, const void *buf, uint32_t size);
typedef int32_t (*bmds_sys_close_fn)(int32_t fd);
typedef int32_t (*bmds_sys_seek_fn)(int32_t fd, int64_t offset, uint32_t whence, int64_t *position);
typedef int32_t (*bmds_sys_isatty_fn)(int32_t fd);
typedef void *(*bmds_sys_mem_alloc_fn)(uint32_t size, uint32_t align, uint32_t flags);
typedef void (*bmds_sys_mem_free_fn)(void *ptr);
typedef void *(*bmds_sys_mem_realloc_fn)(void *ptr, uint32_t size, uint32_t flags);
typedef int32_t (*bmds_sys_path_fn)(const char *path);
typedef int32_t (*bmds_sys_open_fn)(const char *path, uint32_t flags);
typedef int32_t (*bmds_sys_stat_fn)(const char *path, bmds_stat_t *info);
typedef int32_t (*bmds_sys_fstat_fn)(int32_t fd, bmds_stat_t *info);
typedef int32_t (*bmds_sys_rename_fn)(const char *from, const char *to);
typedef int32_t (*bmds_sys_readdir_fn)(int32_t dir, bmds_stat_t *info, char *name, uint32_t size);
typedef int32_t (*bmds_sys_getcwd_fn)(char *buf, uint32_t size);
typedef int32_t (*bmds_sys_fd2_fn)(int32_t fd, int32_t other);
typedef int32_t (*bmds_sys_clock_get_fn)(uint32_t clock, int64_t *ns);
typedef int32_t (*bmds_sys_sleep_fn)(int64_t ns);
typedef int32_t (*bmds_sys_void_fn)(void);
typedef int32_t (*bmds_sys_term_get_fn)(int32_t fd, bmds_term_t *term);
typedef int32_t (*bmds_sys_term_set_fn)(int32_t fd, uint32_t flags);
typedef int32_t (*bmds_sys_mem_info_fn)(uint32_t flags, bmds_mem_info_t *info);
typedef int32_t (*bmds_sys_random_fn)(void *buf, uint32_t size);
typedef int32_t (*bmds_sys_set_mtime_fn)(const char *path, int64_t ns);
typedef int32_t (*bmds_sys_ftruncate_fn)(int32_t fd, int64_t size);

/** @brief Ends the program with @p status (0-255; its exit status in the shell). */
static inline void bmds_exit(int32_t status) {
        ((bmds_sys_exit_fn)bmds_sys->slot[BMDS_SYS_EXIT])(status);
        for (;;) {}
}

/** @brief Reads up to @p size bytes; 0 at end of file. Blocks until some data is there. */
static inline int32_t bmds_read(int32_t fd, void *buf, uint32_t size) {
        return ((bmds_sys_read_fn)bmds_sys->slot[BMDS_SYS_READ])(fd, buf, size);
}

/** @brief Writes all of @p size bytes (or fails); returns @p size. */
static inline int32_t bmds_write(int32_t fd, const void *buf, uint32_t size) {
        return ((bmds_sys_write_fn)bmds_sys->slot[BMDS_SYS_WRITE])(fd, buf, size);
}

/** @brief Closes @p fd. */
static inline int32_t bmds_close(int32_t fd) {
        return ((bmds_sys_close_fn)bmds_sys->slot[BMDS_SYS_CLOSE])(fd);
}

/** @brief Moves @p fd's position; the new one goes to @p position (may be NULL). */
static inline int32_t bmds_seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position) {
        return ((bmds_sys_seek_fn)bmds_sys->slot[BMDS_SYS_SEEK])(fd, offset, whence, position);
}

/** @brief Returns 1 if @p fd is a terminal, 0 if not. */
static inline int32_t bmds_isatty(int32_t fd) {
        return ((bmds_sys_isatty_fn)bmds_sys->slot[BMDS_SYS_ISATTY])(fd);
}

/**
 * @brief Allocates @p size bytes aligned to @p align (a power of two; 0 for
 *        `BMDS_MEM_ALIGN`), from PSRAM unless @p flags say otherwise.
 * @return The memory, or NULL (no memory, a bad alignment, unknown flags).
 *         The kernel frees what a program still holds when it ends.
 */
static inline void *bmds_mem_alloc(uint32_t size, uint32_t align, uint32_t flags) {
        return ((bmds_sys_mem_alloc_fn)bmds_sys->slot[BMDS_SYS_MEM_ALLOC])(size, align, flags);
}

/** @brief Frees memory from `bmds_mem_alloc` (NULL is ignored). */
static inline void bmds_mem_free(void *ptr) {
        ((bmds_sys_mem_free_fn)bmds_sys->slot[BMDS_SYS_MEM_FREE])(ptr);
        return;
}

/**
 * @brief Resizes memory from `bmds_mem_alloc` (NULL @p ptr allocates), keeping
 *        its contents up to the smaller size.
 * @return The memory (maybe moved), or NULL with @p ptr untouched.
 */
static inline void *bmds_mem_realloc(void *ptr, uint32_t size, uint32_t flags) {
        return ((bmds_sys_mem_realloc_fn)bmds_sys->slot[BMDS_SYS_MEM_REALLOC])(ptr, size, flags);
}

/*
 * Paths: absolute, or relative to the program's current directory (its
 * own: it starts as the shell's, and `chdir` changes only the program's).
 * A path longer than `BMDS_PATH_MAX` fails with `BMDS_ENAMETOOLONG`.
 */

/**
 * @brief Opens a file (or a device: `/dev/null`, `/dev/console`).
 * @return The new fd (the lowest free one), or an error (`BMDS_EINVAL` for
 *         unknown flags or neither `BMDS_O_READ` nor `BMDS_O_WRITE`).
 */
static inline int32_t bmds_open(const char *path, uint32_t flags) {
        return ((bmds_sys_open_fn)bmds_sys->slot[BMDS_SYS_OPEN])(path, flags);
}

/** @brief Describes the file at @p path (`info->size` set by the caller). */
static inline int32_t bmds_stat(const char *path, bmds_stat_t *info) {
        return ((bmds_sys_stat_fn)bmds_sys->slot[BMDS_SYS_STAT])(path, info);
}

/**
 * @brief Describes what @p fd refers to: a file (with its size now) or a
 *        device (a terminal, a pipe), and the fd's open mode; attributes
 *        and time are 0 (`bmds_stat` has them).
 */
static inline int32_t bmds_fstat(int32_t fd, bmds_stat_t *info) {
        return ((bmds_sys_fstat_fn)bmds_sys->slot[BMDS_SYS_FSTAT])(fd, info);
}

/** @brief Creates a directory (its parent must exist). */
static inline int32_t bmds_mkdir(const char *path) {
        return ((bmds_sys_path_fn)bmds_sys->slot[BMDS_SYS_MKDIR])(path);
}

/** @brief Removes an empty directory. */
static inline int32_t bmds_rmdir(const char *path) {
        return ((bmds_sys_path_fn)bmds_sys->slot[BMDS_SYS_RMDIR])(path);
}

/** @brief Removes a file (not a directory: `BMDS_EISDIR`; not while open: `BMDS_EBUSY`). */
static inline int32_t bmds_unlink(const char *path) {
        return ((bmds_sys_path_fn)bmds_sys->slot[BMDS_SYS_UNLINK])(path);
}

/**
 * @brief Renames or moves a file or directory within one filesystem
 *        (`BMDS_EXDEV` across them; `BMDS_EEXIST` if @p to exists).
 */
static inline int32_t bmds_rename(const char *from, const char *to) {
        return ((bmds_sys_rename_fn)bmds_sys->slot[BMDS_SYS_RENAME])(from, to);
}

/** @brief Opens a directory for `bmds_readdir`. @return A directory handle (>= 0). */
static inline int32_t bmds_opendir(const char *path) {
        return ((bmds_sys_path_fn)bmds_sys->slot[BMDS_SYS_OPENDIR])(path);
}

/**
 * @brief Reads the next entry of a directory (without `.` and `..`).
 * @param info Filled as by `bmds_stat` (may be NULL).
 * @param name Gets the entry's name, NUL-terminated, cut to @p size - 1
 *             bytes (at a character boundary) if longer.
 * @return The name's full length (more than @p size - 1 if it was cut),
 *         0 at the end, or an error.
 */
static inline int32_t bmds_readdir(int32_t dir, bmds_stat_t *info, char *name, uint32_t size) {
        return ((bmds_sys_readdir_fn)bmds_sys->slot[BMDS_SYS_READDIR])(dir, info, name, size);
}

/** @brief Closes a directory handle. */
static inline int32_t bmds_closedir(int32_t dir) {
        return ((bmds_sys_close_fn)bmds_sys->slot[BMDS_SYS_CLOSEDIR])(dir);
}

/**
 * @brief Copies the program's current directory (absolute) to @p buf.
 * @return Its length (without the NUL), or `BMDS_ERANGE` if @p size is too small.
 */
static inline int32_t bmds_getcwd(char *buf, uint32_t size) {
        return ((bmds_sys_getcwd_fn)bmds_sys->slot[BMDS_SYS_GETCWD])(buf, size);
}

/** @brief Changes the program's current directory. */
static inline int32_t bmds_chdir(const char *path) {
        return ((bmds_sys_path_fn)bmds_sys->slot[BMDS_SYS_CHDIR])(path);
}

/** @brief Returns a new fd, the lowest free one >= @p min_fd, for what @p fd refers to. */
static inline int32_t bmds_dup(int32_t fd, int32_t min_fd) {
        return ((bmds_sys_fd2_fn)bmds_sys->slot[BMDS_SYS_DUP])(fd, min_fd);
}

/** @brief Makes @p to refer to what @p fd does (closing @p to first); returns @p to. */
static inline int32_t bmds_dup2(int32_t fd, int32_t to) {
        return ((bmds_sys_fd2_fn)bmds_sys->slot[BMDS_SYS_DUP2])(fd, to);
}

/** @brief Reads clock @p clock (`BMDS_CLOCK_...`) into @p ns. */
static inline int32_t bmds_clock_get(uint32_t clock, int64_t *ns) {
        return ((bmds_sys_clock_get_fn)bmds_sys->slot[BMDS_SYS_CLOCK_GET])(clock, ns);
}

/**
 * @brief Sleeps at least @p ns nanoseconds (rounded up to the kernel's tick).
 * @return 0, or `BMDS_EINTR` (Ctrl-C).
 */
static inline int32_t bmds_sleep(int64_t ns) {
        return ((bmds_sys_sleep_fn)bmds_sys->slot[BMDS_SYS_SLEEP])(ns);
}

/**
 * @brief Returns 1 once Ctrl-C has been pressed (with `BMDS_TERM_SIGNALS`),
 *        else 0. From then until the program ends, blocking calls return
 *        `BMDS_EINTR`; the program should clean up and exit (the SDK makes
 *        its exit status 130).
 */
static inline int32_t bmds_interrupted(void) {
        return ((bmds_sys_void_fn)bmds_sys->slot[BMDS_SYS_INTERRUPTED])();
}

/**
 * @brief Describes the terminal @p fd refers to (`BMDS_ENOTTY` if it isn't one).
 */
static inline int32_t bmds_term_get(int32_t fd, bmds_term_t *term) {
        return ((bmds_sys_term_get_fn)bmds_sys->slot[BMDS_SYS_TERM_GET])(fd, term);
}

/**
 * @brief Sets the modes (`BMDS_TERM_...`) of the terminal @p fd refers to.
 *        The shell gets the modes it had back when the program ends.
 */
static inline int32_t bmds_term_set(int32_t fd, uint32_t flags) {
        return ((bmds_sys_term_set_fn)bmds_sys->slot[BMDS_SYS_TERM_SET])(fd, flags);
}

/** @brief Describes memory: that `bmds_mem_alloc` with @p flags uses, and the program's. */
static inline int32_t bmds_mem_info(uint32_t flags, bmds_mem_info_t *info) {
        return ((bmds_sys_mem_info_fn)bmds_sys->slot[BMDS_SYS_MEM_INFO])(flags, info);
}

/** @brief Fills @p buf with random bytes from the hardware generator. */
static inline int32_t bmds_random(void *buf, uint32_t size) {
        return ((bmds_sys_random_fn)bmds_sys->slot[BMDS_SYS_RANDOM])(buf, size);
}

/** @brief Sets a file's modification time (ns since 1970-01-01 UTC; FAT keeps 2 s). */
static inline int32_t bmds_set_mtime(const char *path, int64_t ns) {
        return ((bmds_sys_set_mtime_fn)bmds_sys->slot[BMDS_SYS_SET_MTIME])(path, ns);
}

/**
 * @brief Sets the size of the file @p fd (open for writing) refers to: cut,
 *        or grown with zeros. Its position stays. ABI 2 (check
 *        `bmds_sys_has(BMDS_SYS_FTRUNCATE)` on older kernels).
 * @return 0, or `BMDS_EBADF`, `BMDS_EINVAL` (negative @p size, or not a file),
 *         `BMDS_ENOSPC`, ...
 */
static inline int32_t bmds_ftruncate(int32_t fd, int64_t size) {
        return ((bmds_sys_ftruncate_fn)bmds_sys->slot[BMDS_SYS_FTRUNCATE])(fd, size);
}

#ifdef __cplusplus
}
#endif

#endif /* BMDS_SYS_H */
