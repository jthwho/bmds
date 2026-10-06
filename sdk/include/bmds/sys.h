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
#define BMDS_SYS_INTERRUPTED 25u /**< `int32_t interrupted(void)`: 1 while a `SIGINT` is pending. */
#define BMDS_SYS_TERM_GET 26u    /**< `int32_t term_get(int32_t fd, bmds_term_t *term)`. */
#define BMDS_SYS_TERM_SET 27u    /**< `int32_t term_set(int32_t fd, uint32_t flags)`. */
#define BMDS_SYS_MEM_INFO 28u    /**< `int32_t mem_info(uint32_t flags, bmds_mem_info_t *info)`. */
#define BMDS_SYS_RANDOM 29u      /**< `int32_t random(void *buf, uint32_t size)`. */
#define BMDS_SYS_SET_MTIME 30u   /**< `int32_t set_mtime(const char *path, int64_t ns)`. */
#define BMDS_SYS_FTRUNCATE 31u   /**< `int32_t ftruncate(int32_t fd, int64_t size)`. */
#define BMDS_SYS_SYMLINK 32u     /**< `int32_t symlink(const char *target, const char *path)`. */
#define BMDS_SYS_READLINK 33u    /**< `int32_t readlink(const char *path, char *buf, uint32_t size)`. */
#define BMDS_SYS_LSTAT 34u       /**< `int32_t lstat(const char *path, bmds_stat_t *info)`. */
#define BMDS_SYS_PROC_VFORK 35u  /**< `int32_t proc_vfork(void)`: 0, now the child. */
#define BMDS_SYS_PROC_EXEC 36u   /**< `int32_t proc_exec(const char *path, char *const *argv, char *const *envp)`. */
#define BMDS_SYS_PROC_VFORK_EXIT 37u /**< `int32_t proc_vfork_exit(int32_t status)`: the child's pid. */
#define BMDS_SYS_PROC_WAIT 38u       /**< `int32_t proc_wait(int32_t pid, int32_t *status, uint32_t flags)`. */
#define BMDS_SYS_PIPE 39u            /**< `int32_t pipe(int32_t fds[2], uint32_t flags)`. */
#define BMDS_SYS_PROC_ID 40u         /**< `int32_t proc_id(uint32_t which)`. */
#define BMDS_SYS_FD_FLAGS 41u        /**< `int32_t fd_flags(int32_t fd, int32_t flags)`. */
#define BMDS_SYS_SIG_SETUP 42u       /**< `int32_t sig_setup(volatile uint32_t *hint)`. */
#define BMDS_SYS_SIG_ACTION 43u      /**< `int32_t sig_action(int32_t sig, uint32_t action)`: the previous one. */
#define BMDS_SYS_SIG_MASK 44u        /**< `int32_t sig_mask(uint32_t how, uint32_t set)`: the previous mask. */
#define BMDS_SYS_SIG_PENDING 45u     /**< `int32_t sig_pending(void)`: the pending set. */
#define BMDS_SYS_SIG_TAKE 46u        /**< `int32_t sig_take(bmds_siginfo_t *info)`: a due signal, or 0. */
#define BMDS_SYS_SIG_SUSPEND 47u     /**< `int32_t sig_suspend(uint32_t mask)`: `BMDS_EINTR`. */
#define BMDS_SYS_PROC_KILL 48u       /**< `int32_t proc_kill(int32_t pid, int32_t sig)`. */
#define BMDS_SYS_PROC_EXIT_SIGNAL 49u /**< `int32_t proc_exit_signal(int32_t sig)`. */
#define BMDS_SYS_IO_POLL 50u          /**< `int32_t io_poll(fds, count, timeout_ms, mask)`: see `bmds_io_poll`. */
#define BMDS_SYS_PROC_GROUP 51u       /**< `int32_t proc_group(uint32_t op, int32_t pid, int32_t pgid)`. */
#define BMDS_SYS_TERM_FOREGROUND 52u  /**< `int32_t term_foreground(int32_t fd, int32_t pgid)`. */
#define BMDS_SYS_THREAD_START 53u     /**< `int32_t thread_start(const bmds_thread_t *thread)`: its id. */
#define BMDS_SYS_THREAD_EXIT 54u      /**< `void thread_exit(int32_t status, void *release)`. */
#define BMDS_SYS_FUTEX 55u            /**< `int32_t futex(word, op, value, timeout_ns, word2, count2)`. */
#define BMDS_SYS_THREAD_KILL 56u      /**< `int32_t thread_kill(int32_t tid, int32_t sig, uint32_t flags)`. */
#define BMDS_SYS_THREAD_CLEAR 57u     /**< `int32_t thread_clear(volatile int32_t *word)`: its id. */

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
#define BMDS_O_READ 0x01u    /**< Open for reading. */
#define BMDS_O_WRITE 0x02u   /**< Open for writing. */
#define BMDS_O_CREATE 0x04u  /**< Create the file if it doesn't exist (with `BMDS_O_WRITE`). */
#define BMDS_O_TRUNC 0x08u   /**< Empty an existing file (with `BMDS_O_WRITE`). */
#define BMDS_O_APPEND 0x10u  /**< Every write goes to the end. */
#define BMDS_O_EXCL 0x20u    /**< With `BMDS_O_CREATE`: fail with `BMDS_EEXIST` if it exists. */
#define BMDS_O_CLOEXEC 0x40u /**< The new fd is closed by `bmds_proc_exec` (also for `bmds_pipe`). */

/** @brief `fd_flags`: a descriptor's own flags. */
#define BMDS_FD_CLOEXEC 0x1u /**< Closed by `bmds_proc_exec`. */

/** @brief `proc_wait` flags. */
#define BMDS_WAIT_NOHANG 0x1u    /**< Return 0 at once if no child has ended. */
#define BMDS_WAIT_UNTRACED 0x2u  /**< Also a child that stopped: status `0x7f | sig << 8`, once per stop. */
#define BMDS_WAIT_CONTINUED 0x4u /**< Also a child that continued: status `0xffff`, once. */

/** @brief `proc_id`. */
#define BMDS_PROC_SELF 0u   /**< The calling process's id. */
#define BMDS_PROC_PARENT 1u /**< Its parent's (1 for an orphan, as Linux reports it). */
#define BMDS_PROC_JOB 2u    /**< Its job: its process group. */
#define BMDS_PROC_THREAD 3u /**< The calling thread's id (the first thread's is the pid). */

/** @brief `proc_group` operations. */
#define BMDS_GROUP_GET 0u         /**< @p pid's process group (0: the caller's). */
#define BMDS_GROUP_SET 1u         /**< `setpgid(pid, pgid)`: @p pid 0 is the caller, @p pgid 0 is @p pid. */
#define BMDS_GROUP_SESSION 2u     /**< @p pid's session (0: the caller's). */
#define BMDS_GROUP_NEW_SESSION 3u /**< `setsid()`: the caller leads a new session and group. */

/** @brief Signals: Linux's numbers. Signals 1-31 are delivered; a mask has bit `sig - 1`. */
#define BMDS_SIGHUP 1
#define BMDS_SIGINT 2 /**< Ctrl-C. */
#define BMDS_SIGQUIT 3
#define BMDS_SIGILL 4
#define BMDS_SIGTRAP 5
#define BMDS_SIGABRT 6
#define BMDS_SIGBUS 7
#define BMDS_SIGFPE 8
#define BMDS_SIGKILL 9 /**< Ends the process; can't be caught, ignored or blocked. */
#define BMDS_SIGUSR1 10
#define BMDS_SIGSEGV 11
#define BMDS_SIGUSR2 12
#define BMDS_SIGPIPE 13 /**< A write to a pipe nobody reads. */
#define BMDS_SIGALRM 14
#define BMDS_SIGTERM 15
#define BMDS_SIGSTKFLT 16
#define BMDS_SIGCHLD 17 /**< A child ended (dropped by default). */
#define BMDS_SIGCONT 18 /**< Continues a stopped process (whatever its action); then dropped by default. */
#define BMDS_SIGSTOP 19 /**< Stops the process; can't be caught, ignored or blocked. */
#define BMDS_SIGTSTP 20 /**< Ctrl-Z: stops the process by default. */
#define BMDS_SIGTTIN 21 /**< A background read of the terminal: stops the process by default. */
#define BMDS_SIGTTOU 22 /**< Stops the process by default (only `kill` sends it). */
#define BMDS_SIGURG 23  /**< Dropped by default. */
#define BMDS_SIGXCPU 24
#define BMDS_SIGXFSZ 25
#define BMDS_SIGVTALRM 26
#define BMDS_SIGPROF 27
#define BMDS_SIGWINCH 28 /**< The terminal's size changed (dropped by default). */
#define BMDS_SIGIO 29
#define BMDS_SIGPWR 30
#define BMDS_SIGSYS 31
#define BMDS_SIG_COUNT 32 /**< One more than the highest signal delivered. */

/** @brief `sig_action` actions. */
#define BMDS_SIG_DEFAULT 0u        /**< The default: dropped, or ends the process (see the signal). */
#define BMDS_SIG_IGNORE 1u         /**< Dropped (a pending one too). */
#define BMDS_SIG_CATCH 2u          /**< Kept pending until taken with `sig_take` (the SDK runs the handler). */
#define BMDS_SIG_QUERY 0xffffffffu /**< Changes nothing: `sig_action` only returns the action. */

/** @brief `sig_mask` hows (Linux's values). */
#define BMDS_SIG_BLOCK 0u   /**< Add `set` to the mask. */
#define BMDS_SIG_UNBLOCK 1u /**< Remove `set` from the mask. */
#define BMDS_SIG_SETMASK 2u /**< The mask becomes `set`. */

/** @brief `bmds_siginfo_t::code` (Linux's `si_code` values). */
#define BMDS_SI_USER 0      /**< Sent by `proc_kill`. */
#define BMDS_SI_KERNEL 0x80 /**< Sent by the kernel (Ctrl-C, a broken pipe, a child's end, a resize). */

/** @brief `bmds_siginfo_t::flags`. */
#define BMDS_SIG_TERMINATE 0x1u /**< Not a signal to handle: end with `proc_exit_signal(signo)`. */

/**
 * @brief A signal taken with `sig_take`. The caller sets `size`; the kernel
 *        fills the fields that fit in it.
 */
typedef struct bmds_siginfo_t {
                uint32_t size;  /**< Set by the caller: `sizeof(bmds_siginfo_t)`. */
                int32_t  signo; /**< The signal. */
                int32_t  code;  /**< `BMDS_SI_...`. */
                uint32_t flags; /**< `BMDS_SIG_...`. */
} bmds_siginfo_t;

/** @brief `bmds_pollfd_t` events (Linux's values). */
#define BMDS_POLL_IN 0x01u   /**< A read wouldn't wait (data, or the end of input). */
#define BMDS_POLL_PRI 0x02u  /**< Urgent data (never reported). */
#define BMDS_POLL_OUT 0x04u  /**< A write wouldn't wait. */
#define BMDS_POLL_ERR 0x08u  /**< Writes fail: a pipe without a reader (reported unasked). */
#define BMDS_POLL_HUP 0x10u  /**< The other end is gone: a pipe without a writer (reported unasked). */
#define BMDS_POLL_NVAL 0x20u /**< The fd isn't open (reported unasked). */

/**
 * @brief An entry of `io_poll`'s array. The caller sets `size` (the same in
 *        every entry), `fd` and `events`; the kernel sets `revents`.
 */
typedef struct bmds_pollfd_t {
                uint32_t size;    /**< Set by the caller: `sizeof(bmds_pollfd_t)`. */
                int32_t  fd;      /**< The descriptor; a negative one is skipped (`revents` 0). */
                uint32_t events;  /**< `BMDS_POLL_IN` and/or `BMDS_POLL_OUT`. */
                uint32_t revents; /**< Set by the kernel: the events that are true, with ERR, HUP, NVAL. */
} bmds_pollfd_t;

/** @brief The most entries one `io_poll` takes. */
#define BMDS_POLL_MAX 64u

/** @brief `bmds_stat_t::type`. */
#define BMDS_TYPE_FILE 1u      /**< A regular file. */
#define BMDS_TYPE_DIRECTORY 2u /**< A directory (also `/`, `/dev` and mount points). */
#define BMDS_TYPE_DEVICE 3u    /**< A device (`/dev/null`, the console), a pipe. */
#define BMDS_TYPE_LINK 4u      /**< A symbolic link (only from `lstat` and `readdir`). */

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
                uint64_t file_id;  /**< The same for each name and fd of one file, different for others; 0 if unknown. */
                uint32_t device;   /**< The filesystem it is on (non-zero), or 0 (devices, pipes, `/`, `/dev`). */
                uint32_t reserved; /**< Zero. */
} bmds_stat_t;

/** @brief `clock_get` clocks. */
#define BMDS_CLOCK_REALTIME 0u  /**< Wall time, ns since 1970-01-01 UTC (may jump when set). */
#define BMDS_CLOCK_MONOTONIC 1u /**< ns since boot, never goes back. */

/** @brief Terminal modes (`term_get`, `term_set`); a program starts with all set. */
#define BMDS_TERM_SIGNALS 0x01u /**< Ctrl-C sends `SIGINT` to the job instead of being read. */
#define BMDS_TERM_ONLCR 0x02u   /**< Output `\n` is sent as CR LF where the terminal needs it. */
#define BMDS_TERM_COLORS 0x04u  /**< Output colors are fitted to what the terminal shows. */
#define BMDS_TERM_LOGS 0x08u    /**< The kernel's log lines are shown. */
#define BMDS_TERM_LINE 0x10u    /**< Input is read in lines, editable (else raw bytes). */
#define BMDS_TERM_ECHO 0x20u    /**< Input is echoed (line mode: the editor shows it; raw: the bytes read). */
#define BMDS_TERM_ICRNL 0x40u   /**< Raw input: CR is read as LF. */

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

/**
 * @brief A thread to start with `thread_start` (devplan/processes.md §3.10).
 *        The caller sets `size`; the kernel reads the fields that fit in it.
 */
typedef struct bmds_thread_t {
                uint32_t size;               /**< Set by the caller: `sizeof(bmds_thread_t)`. */
                int32_t (*entry)(void *arg); /**< What the thread runs; it ends with what this returns. */
                void    *arg;                /**< `entry`'s argument. */
                void    *tls;                /**< Its thread pointer (`tp`). */
                uint32_t stack_size; /**< Its stack (0: `BMDS_DEFAULT_STACK_SIZE`; at least `BMDS_THREAD_STACK_MIN`). */
                volatile uint32_t *hint;  /**< Its signal hint word (`sig_setup`), or NULL. */
                volatile int32_t  *clear; /**< Its clear word (`thread_clear`), or NULL. */
                int32_t           *tid;   /**< Where its id goes before it runs, or NULL. */
} bmds_thread_t;

/** @brief The smallest stack `thread_start` gives a thread: system calls run on it. */
#define BMDS_THREAD_STACK_MIN 8192u

/** @brief `futex` operations. */
#define BMDS_FUTEX_WAIT 0u    /**< Sleep while `*word == value`: woken, the timeout, or a signal. */
#define BMDS_FUTEX_WAKE 1u    /**< Wake up to `value` waiters on `word`. */
#define BMDS_FUTEX_REQUEUE 2u /**< Wake up to `value`, move up to `count2` more to wait on `word2`. */

/** @brief `thread_kill` flags. */
#define BMDS_THREAD_WAKE 0x1u /**< Also end the call the thread is blocked in, or its next one (`BMDS_EINTR`). */

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
typedef int32_t (*bmds_sys_symlink_fn)(const char *target, const char *path);
typedef int32_t (*bmds_sys_readlink_fn)(const char *path, char *buf, uint32_t size);
typedef int32_t (*bmds_sys_proc_exec_fn)(const char *path, char *const *argv, char *const *envp);
typedef int32_t (*bmds_sys_proc_wait_fn)(int32_t pid, int32_t *status, uint32_t flags);
typedef int32_t (*bmds_sys_pipe_fn)(int32_t *fds, uint32_t flags);
typedef int32_t (*bmds_sys_int_fn)(int32_t value);
typedef int32_t (*bmds_sys_uint_fn)(uint32_t value);
typedef int32_t (*bmds_sys_sig_setup_fn)(volatile uint32_t *hint);
typedef int32_t (*bmds_sys_sig_action_fn)(int32_t sig, uint32_t action);
typedef int32_t (*bmds_sys_sig_mask_fn)(uint32_t how, uint32_t set);
typedef int32_t (*bmds_sys_sig_take_fn)(bmds_siginfo_t *info);
typedef int32_t (*bmds_sys_proc_kill_fn)(int32_t pid, int32_t sig);
typedef int32_t (*bmds_sys_io_poll_fn)(bmds_pollfd_t *fds, uint32_t count, int32_t timeout_ms, const uint32_t *mask);
typedef int32_t (*bmds_sys_proc_group_fn)(uint32_t op, int32_t pid, int32_t pgid);
typedef int32_t (*bmds_sys_term_foreground_fn)(int32_t fd, int32_t pgid);
typedef int32_t (*bmds_sys_thread_start_fn)(const bmds_thread_t *thread);
typedef void (*bmds_sys_thread_exit_fn)(int32_t status, void *release);
typedef int32_t (*bmds_sys_futex_fn)(volatile int32_t *word, uint32_t op, int32_t value, int64_t timeout_ns,
                                     volatile int32_t *word2, int32_t count2);
typedef int32_t (*bmds_sys_thread_kill_fn)(int32_t tid, int32_t sig, uint32_t flags);
typedef int32_t (*bmds_sys_thread_clear_fn)(volatile int32_t *word);

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
 * @return 0, or `BMDS_EINTR` (a signal is due).
 */
static inline int32_t bmds_sleep(int64_t ns) {
        return ((bmds_sys_sleep_fn)bmds_sys->slot[BMDS_SYS_SLEEP])(ns);
}

/**
 * @brief Returns 1 while a `BMDS_SIGINT` is pending (Ctrl-C sends it, with
 *        `BMDS_TERM_SIGNALS`), else 0. By default `SIGINT` ends the program;
 *        one that catches it (`bmds_sig_action`) can poll this, and takes the
 *        signal with `bmds_sig_take` (until then, blocking calls return `BMDS_EINTR`).
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
 *        or grown with zeros. Its position stays.
 * @return 0, or `BMDS_EBADF`, `BMDS_EINVAL` (negative @p size, or not a file),
 *         `BMDS_ENOSPC`, ...
 */
static inline int32_t bmds_ftruncate(int32_t fd, int64_t size) {
        return ((bmds_sys_ftruncate_fn)bmds_sys->slot[BMDS_SYS_FTRUNCATE])(fd, size);
}

/*
 * Symbolic links. FAT has none: they are emulated as Cygwin does
 * (a System file holding "!<symlink>" and the target), so Cygwin on a PC
 * sees them too. open, stat, opendir, chdir, set_mtime and running a
 * program follow them (at most 8 in a row: BMDS_ELOOP); unlink, rename and
 * lstat act on the link itself.
 */

/**
 * @brief Makes @p path a symbolic link to @p target (absolute, or relative
 *        to the link's directory; it needn't exist).
 * @return 0, or `BMDS_EEXIST`, `BMDS_EINVAL` (empty or longer than
 *         `BMDS_PATH_MAX`), `BMDS_ENOENT`, ...
 */
static inline int32_t bmds_symlink(const char *target, const char *path) {
        return ((bmds_sys_symlink_fn)bmds_sys->slot[BMDS_SYS_SYMLINK])(target, path);
}

/**
 * @brief Reads the target of the symbolic link @p path into @p buf,
 *        NUL-terminated, cut to @p size - 1 bytes (at a character boundary).
 * @return The target's full length, or `BMDS_EINVAL` (not a link), `BMDS_ENOENT`, ...
 */
static inline int32_t bmds_readlink(const char *path, char *buf, uint32_t size) {
        return ((bmds_sys_readlink_fn)bmds_sys->slot[BMDS_SYS_READLINK])(path, buf, size);
}

/** @brief As `bmds_stat`, but a link is described itself (`BMDS_TYPE_LINK`, the target's length). */
static inline int32_t bmds_lstat(const char *path, bmds_stat_t *info) {
        return ((bmds_sys_stat_fn)bmds_sys->slot[BMDS_SYS_LSTAT])(path, info);
}

/*
 * Processes. Every program is a process with an id; it starts others with
 * vfork and exec, as on Unix systems without an MMU: the child runs in the
 * parent's memory, on its stack, until it execs or exits, and the parent
 * waits until then. The SDK's vfork(), posix_spawn() and execve() wrap
 * these calls (a C function can't use proc_vfork directly: it returns
 * twice, the second time through the SDK's saved registers).
 */

/**
 * @brief Starts a child process in the calling task: from now until
 *        `bmds_proc_exec` or `bmds_proc_vfork_exit`, system calls act on
 *        the child's descriptors and directory (copies of the caller's).
 *        Memory stays the caller's: what the child allocates is the parent's.
 * @return 0 (now the child), or `BMDS_EAGAIN` (too many processes, or a
 *         child already vforking again), `BMDS_ENOMEM`.
 */
static inline int32_t bmds_proc_vfork(void) {
        return ((bmds_sys_void_fn)bmds_sys->slot[BMDS_SYS_PROC_VFORK])();
}

/**
 * @brief Runs the program at @p path with @p argv and @p envp (both
 *        NULL-terminated; copied). Descriptors flagged `BMDS_FD_CLOEXEC` are
 *        closed, the others stay.
 *
 * @p path may be a BMDS program; a script starting `#!interpreter [arg]`
 * (run as `interpreter [arg] path argv[1]...`: an existing file, `/bin/sh`
 * or `/bin/bash` for the BMDS shell, `/usr/bin/env name` with `name` looked
 * up on `PATH` in @p envp as the shell does; at most 4 nested); a `.sh`
 * file (the shell runs it); or `/bin/sh` itself (the BMDS shell, so
 * `system()` and `popen()` work).
 *
 * In a vfork child: the child becomes that program, running on its own,
 * and the call returns in the parent with the child's id (> 0). Else the
 * calling program is replaced (same id, descriptors and directory) and
 * the call doesn't return.
 * @return The child's id, or `BMDS_ENOENT`, `BMDS_ENOEXEC` (neither a
 *         program nor a script), `BMDS_ELOOP` (interpreters nested too deep),
 *         `BMDS_EACCES` (a directory), `BMDS_ENOMEM`, `BMDS_E2BIG`, ...
 *         (the caller is still what it was).
 */
static inline int32_t bmds_proc_exec(const char *path, char *const *argv, char *const *envp) {
        return ((bmds_sys_proc_exec_fn)bmds_sys->slot[BMDS_SYS_PROC_EXEC])(path, argv, envp);
}

/**
 * @brief Ends a vfork child that didn't exec, with @p status (0-255).
 * @return In the parent again: the child's id; `BMDS_EINVAL` if not a vfork child.
 */
static inline int32_t bmds_proc_vfork_exit(int32_t status) {
        return ((bmds_sys_int_fn)bmds_sys->slot[BMDS_SYS_PROC_VFORK_EXIT])(status);
}

/**
 * @brief Waits for child @p pid (-1: any child) to end and frees it.
 * @param status Gets how it ended (may be NULL), in Linux's layout: the
 *               exit status in bits 8-15, or for a killed child the signal
 *               in bits 0-6 (2, SIGINT, for Ctrl-C).
 * @param flags  `BMDS_WAIT_NOHANG`, `BMDS_WAIT_UNTRACED`, `BMDS_WAIT_CONTINUED`
 *               (a stopped or continued child is reported, not freed).
 * @return The child's id, 0 with `BMDS_WAIT_NOHANG` if none has ended,
 *         `BMDS_ECHILD` (no such child), `BMDS_EINTR` (a signal is due).
 */
static inline int32_t bmds_proc_wait(int32_t pid, int32_t *status, uint32_t flags) {
        return ((bmds_sys_proc_wait_fn)bmds_sys->slot[BMDS_SYS_PROC_WAIT])(pid, status, flags);
}

/**
 * @brief Makes a pipe: @p fds[0] reads what @p fds[1] writes.
 * @param flags `BMDS_O_CLOEXEC` (both ends).
 * @return 0, or `BMDS_EMFILE`, `BMDS_ENOMEM`, `BMDS_EINVAL`.
 */
static inline int32_t bmds_pipe(int32_t *fds, uint32_t flags) {
        return ((bmds_sys_pipe_fn)bmds_sys->slot[BMDS_SYS_PIPE])(fds, flags);
}

/** @brief Returns the calling process's id, its parent's, its process group's or the calling thread's (`BMDS_PROC_...`). */
static inline int32_t bmds_proc_id(uint32_t which) {
        return ((bmds_sys_uint_fn)bmds_sys->slot[BMDS_SYS_PROC_ID])(which);
}

/**
 * @brief Sets @p fd's flags (`BMDS_FD_CLOEXEC`), or with @p flags -1 only reads them.
 * @return The flags (as they are now), or `BMDS_EBADF`, `BMDS_EINVAL`.
 */
static inline int32_t bmds_fd_flags(int32_t fd, int32_t flags) {
        return ((bmds_sys_fd2_fn)bmds_sys->slot[BMDS_SYS_FD_FLAGS])(fd, flags);
}

/*
 * Signals (devplan/programs.md §15). The kernel keeps each process's
 * actions, mask and pending signals; a signal is due when it is pending,
 * not blocked, and either caught or one that ends the process. Signals are
 * looked at between system calls: a due signal that ends the process ends
 * it (at once if it is computing, as its call returns if it is in one); a
 * due caught signal wakes a blocked call (BMDS_EINTR) and waits to be taken.
 */

/**
 * @brief Registers the word the kernel sets non-zero while a signal is due
 *        for the caller (NULL: none). The SDK checks it after each call and
 *        takes due signals with `bmds_sig_take`.
 * @return 0.
 */
static inline int32_t bmds_sig_setup(volatile uint32_t *hint) {
        return ((bmds_sys_sig_setup_fn)bmds_sys->slot[BMDS_SYS_SIG_SETUP])(hint);
}

/**
 * @brief Sets what @p sig does: `BMDS_SIG_DEFAULT`, `BMDS_SIG_IGNORE` or `BMDS_SIG_CATCH`;
 *        `BMDS_SIG_QUERY` only reads it (a new program inherits ignored signals).
 * @return The previous action, or `BMDS_EINVAL` (`BMDS_SIGKILL`, `BMDS_SIGSTOP`
 *         except to query, not 1-31, an unknown action).
 */
static inline int32_t bmds_sig_action(int32_t sig, uint32_t action) {
        return ((bmds_sys_sig_action_fn)bmds_sys->slot[BMDS_SYS_SIG_ACTION])(sig, action);
}

/**
 * @brief Changes the mask (`BMDS_SIG_BLOCK`, `BMDS_SIG_UNBLOCK`, `BMDS_SIG_SETMASK`;
 *        bit `sig - 1`). `BMDS_SIGKILL` and `BMDS_SIGSTOP` are never blocked.
 *        Unblocking a pending signal that ends the process ends it.
 * @return The previous mask, or `BMDS_EINVAL`.
 */
static inline int32_t bmds_sig_mask(uint32_t how, uint32_t set) {
        return ((bmds_sys_sig_mask_fn)bmds_sys->slot[BMDS_SYS_SIG_MASK])(how, set);
}

/** @brief Returns the pending signals (bit `sig - 1`). */
static inline int32_t bmds_sig_pending(void) {
        return ((bmds_sys_void_fn)bmds_sys->slot[BMDS_SYS_SIG_PENDING])();
}

/**
 * @brief Takes the lowest due caught signal off the pending set.
 *
 * With `BMDS_SIG_TERMINATE` in @p info->flags, the caller must end with
 * `bmds_proc_exit_signal(signo)` instead: a vfork child, which only its
 * program can return from.
 *
 * @param info Filled (may be NULL).
 * @return The signal, or 0 if none is due.
 */
static inline int32_t bmds_sig_take(bmds_siginfo_t *info) {
        return ((bmds_sys_sig_take_fn)bmds_sys->slot[BMDS_SYS_SIG_TAKE])(info);
}

/**
 * @brief Sets the mask to @p mask and waits until a signal is due (the mask
 *        stays: put the old one back after taking the signals).
 * @return `BMDS_EINTR`.
 */
static inline int32_t bmds_sig_suspend(uint32_t mask) {
        return ((bmds_sys_uint_fn)bmds_sys->slot[BMDS_SYS_SIG_SUSPEND])(mask);
}

/**
 * @brief Sends @p sig (0: only checks) to @p pid: > 0 that process (one that
 *        ended and hasn't been waited for too); 0 the caller's process group;
 *        -1 every process but the caller; < -1 the group -@p pid.
 *
 * Pid 1 (the serial shell) gets only the signals it has an action for.
 * @return 0, or `BMDS_ESRCH` (no such process), `BMDS_EINVAL` (not 0-31),
 *         `BMDS_EPERM` (`SIGKILL` or `SIGSTOP` to pid 1).
 */
static inline int32_t bmds_proc_kill(int32_t pid, int32_t sig) {
        return ((bmds_sys_proc_kill_fn)bmds_sys->slot[BMDS_SYS_PROC_KILL])(pid, sig);
}

/**
 * @brief Ends the caller as killed by @p sig (its parent sees the signal).
 * @return In a vfork child, in the parent again: the child's id; else doesn't
 *         return (`BMDS_EINVAL` if @p sig isn't 1-31).
 */
static inline int32_t bmds_proc_exit_signal(int32_t sig) {
        return ((bmds_sys_int_fn)bmds_sys->slot[BMDS_SYS_PROC_EXIT_SIGNAL])(sig);
}

/**
 * @brief Waits until one of @p count descriptors is ready for what its entry
 *        asks, @p timeout_ms passes (negative: never; 0: only checks), or a
 *        signal is due.
 *
 * A terminal in line mode is readable once a whole line is typed (polling
 * edits and echoes it as reading would). With @p mask, the signal mask is
 * `*mask` for the wait and stays so when the call returns: the caller runs
 * its handlers under it, then restores its own (`ppoll`, `pselect`).
 *
 * @return How many entries have `revents`; 0 at the timeout; `BMDS_EINTR`,
 *         `BMDS_EINVAL` (more than `BMDS_POLL_MAX`, or a bad `size`),
 *         `BMDS_EFAULT`.
 */
static inline int32_t bmds_io_poll(bmds_pollfd_t *fds, uint32_t count, int32_t timeout_ms, const uint32_t *mask) {
        return ((bmds_sys_io_poll_fn)bmds_sys->slot[BMDS_SYS_IO_POLL])(fds, count, timeout_ms, mask);
}

/**
 * @brief Process groups and sessions (`BMDS_GROUP_...`), as `getpgid`,
 *        `setpgid`, `getsid` and `setsid` (devplan/processes.md §3.6).
 *
 * `BMDS_GROUP_SET` moves the caller or a child of it, in its session and
 * not a session leader, to a group of that session or one of its own.
 * `BMDS_GROUP_NEW_SESSION` fails for a process that leads a group.
 * @return The group or session's id (`GET`, `SESSION`, `NEW_SESSION`), 0
 *         (`SET`); or `BMDS_ESRCH`, `BMDS_EPERM`, `BMDS_EINVAL`.
 */
static inline int32_t bmds_proc_group(uint32_t op, int32_t pid, int32_t pgid) {
        return ((bmds_sys_proc_group_fn)bmds_sys->slot[BMDS_SYS_PROC_GROUP])(op, pid, pgid);
}

/**
 * @brief The foreground process group of the terminal @p fd, the caller's
 *        controlling terminal: sets it to @p pgid (> 0; a group of the
 *        caller's session), or with @p pgid 0 only reads it (`tcsetpgrp`,
 *        `tcgetpgrp`).
 * @return The foreground group; or `BMDS_EBADF`, `BMDS_ENOTTY` (not a
 *         terminal, or not the caller's), `BMDS_EPERM` (no such group in the
 *         session), `BMDS_EINVAL`.
 */
static inline int32_t bmds_term_foreground(int32_t fd, int32_t pgid) {
        return ((bmds_sys_term_foreground_fn)bmds_sys->slot[BMDS_SYS_TERM_FOREGROUND])(fd, pgid);
}

/*
 * Threads (devplan/processes.md §3.10): a process's threads share its
 * memory, descriptors, directory and signal actions; each has its own
 * stack, thread pointer, signal mask and hint word. musl's pthreads are
 * built on these calls; programs use those.
 */

/**
 * @brief Starts a thread in the calling process: `thread->entry(thread->arg)`
 *        with `tp` = `thread->tls`, the caller's signal mask, and a stack of
 *        `thread->stack_size` bytes the kernel allocates. Its id is written to
 *        `*thread->tid` before it runs. When `entry` returns, the thread ends
 *        with that status (`thread_exit`).
 * @return Its id (> 0), or `BMDS_EAGAIN` (the process is ending), `BMDS_ENOMEM`,
 *         `BMDS_EINVAL`.
 */
static inline int32_t bmds_thread_start(const bmds_thread_t *thread) {
        return ((bmds_sys_thread_start_fn)bmds_sys->slot[BMDS_SYS_THREAD_START])(thread);
}

/**
 * @brief Ends the calling thread. @p release (a block from `mem_alloc`, or
 *        NULL) is freed as it ends. Once it is gone, its clear word (if any)
 *        becomes 0 and one `futex` waiter on it is woken. The last thread's
 *        end ends the process with @p status, as `exit`.
 */
static inline void bmds_thread_exit(int32_t status, void *release) {
        ((bmds_sys_thread_exit_fn)bmds_sys->slot[BMDS_SYS_THREAD_EXIT])(status, release);
}

/**
 * @brief Waits on, or wakes, threads waiting on @p word (`BMDS_FUTEX_...`).
 *
 * `BMDS_FUTEX_WAIT` sleeps while `*word == value`, checked atomically
 * against wakes, until woken, @p timeout_ns passes (negative: never), or a
 * signal is due for the thread.
 * @return `WAIT`: 0, or `BMDS_EAGAIN` (the word held another value),
 *         `BMDS_ETIMEDOUT`, `BMDS_EINTR`; `WAKE`, `REQUEUE`: how many were
 *         woken. `BMDS_EINVAL` for a misaligned word or an unknown operation.
 */
static inline int32_t bmds_futex(volatile int32_t *word, uint32_t op, int32_t value, int64_t timeout_ns,
                                 volatile int32_t *word2, int32_t count2) {
        return ((bmds_sys_futex_fn)bmds_sys->slot[BMDS_SYS_FUTEX])(word, op, value, timeout_ns, word2, count2);
}

/**
 * @brief Sends @p sig (0: only a check) to thread @p tid of the calling
 *        process. Caught, it is due for that thread only; otherwise it acts on
 *        the process as any signal. `BMDS_THREAD_WAKE` also ends the call the
 *        thread is blocked in, or its next one (`BMDS_EINTR`).
 * @return 0, or `BMDS_ESRCH`, `BMDS_EINVAL`.
 */
static inline int32_t bmds_thread_kill(int32_t tid, int32_t sig, uint32_t flags) {
        return ((bmds_sys_thread_kill_fn)bmds_sys->slot[BMDS_SYS_THREAD_KILL])(tid, sig, flags);
}

/**
 * @brief Sets the calling thread's clear word (`set_tid_address`): when the
 *        thread has ended it becomes 0 and one `futex` waiter on it is woken.
 * @return The thread's id.
 */
static inline int32_t bmds_thread_clear(volatile int32_t *word) {
        return ((bmds_sys_thread_clear_fn)bmds_sys->slot[BMDS_SYS_THREAD_CLEAR])(word);
}

#ifdef __cplusplus
}
#endif

#endif /* BMDS_SYS_H */
