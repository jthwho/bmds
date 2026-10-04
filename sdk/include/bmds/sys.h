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
#define BMDS_ABI_VERSION 1u

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

#define BMDS_SYS_EXIT 0u        /**< `void exit(int32_t status)`: does not return. */
#define BMDS_SYS_READ 1u        /**< `int32_t read(int32_t fd, void *buf, uint32_t size)`. */
#define BMDS_SYS_WRITE 2u       /**< `int32_t write(int32_t fd, const void *buf, uint32_t size)`. */
#define BMDS_SYS_CLOSE 3u       /**< `int32_t close(int32_t fd)`. */
#define BMDS_SYS_SEEK 4u        /**< `int32_t seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position)`. */
#define BMDS_SYS_ISATTY 5u      /**< `int32_t isatty(int32_t fd)`: 1 terminal, 0 not, or an error. */
#define BMDS_SYS_MEM_ALLOC 6u   /**< `void *mem_alloc(uint32_t size, uint32_t align, uint32_t flags)`. */
#define BMDS_SYS_MEM_FREE 7u    /**< `void mem_free(void *ptr)`. */
#define BMDS_SYS_MEM_REALLOC 8u /**< `void *mem_realloc(void *ptr, uint32_t size, uint32_t flags)`. */

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

#ifdef __cplusplus
}
#endif

#endif /* BMDS_SYS_H */
