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
#define BMDS_SYS_FILE_TYPE 58u        /**< `int32_t file_type(path, flags, out, size)`: see `bmds_file_type`. */
#define BMDS_SYS_SYSCONF_FIND 59u     /**< `int32_t sysconf_find(const char *key)`: an id. */
#define BMDS_SYS_SYSCONF_GET 60u      /**< `int32_t sysconf_get(id, field, out, size)`: see `bmds_sysconf_get`. */
#define BMDS_SYS_SYSCONF_INFO 61u     /**< `int32_t sysconf_info(int32_t id, bmds_sysconf_info_t *info)`. */
#define BMDS_SYS_SYSCONF_SET 62u      /**< `int32_t sysconf_set(int32_t id, uint32_t op, const char *value)`. */
#define BMDS_SYS_SYSCONF_NEXT 63u     /**< `int32_t sysconf_next(const char *prefix, int32_t after)`: an id. */
#define BMDS_SYS_SYSCONF_DECLARE 64u  /**< `int32_t sysconf_declare(const bmds_sysconf_param_t *param)`: an id. */
#define BMDS_SYS_SYSCONF_WATCH 65u    /**< `int32_t sysconf_watch(const char *prefix, uint32_t flags)`: a new fd. */
#define BMDS_SYS_SYNC 66u             /**< `int32_t sync(void)`: what the filesystems hold, written. */
#define BMDS_SYS_AUDIO_CONFIG 67u  /**< `int32_t audio_config(int32_t fd, const bmds_audio_config_t *set, ... *got)`. */
#define BMDS_SYS_AUDIO_STATUS 68u  /**< `int32_t audio_status(int32_t fd, bmds_audio_status_t *status)`. */
#define BMDS_SYS_AUDIO_CONTROL 69u /**< `int32_t audio_control(int32_t fd, uint32_t op, int32_t value)`. */
#define BMDS_SYS_SOUND_PLAY 70u    /**< `int32_t sound_play(uint32_t sound)`. */
#define BMDS_SYS_AUDIO_DECODE                                                                                          \
        71u /**< `int32_t audio_decode(int32_t fd, const bmds_decode_config_t *config)`: a new fd. */
#define BMDS_SYS_AUDIO_DECODE_INFO 72u /**< `int32_t audio_decode_info(int32_t fd, bmds_decode_info_t *info)`. */
#define BMDS_SYS_AUDIO_DECODE_TAG 73u  /**< `int32_t audio_decode_tag(fd, index, key, key_size, value, value_size)`. */
#define BMDS_SYS_AUDIO_CYCLE 74u       /**< `int32_t audio_cycle(int32_t fd, bmds_audio_cycle_t *cycle)`. */
#define BMDS_SYS_MIDI_CONFIG 75u  /**< `int32_t midi_config(int32_t fd, const bmds_midi_config_t *set, ... *got)`. */
#define BMDS_SYS_MIDI_STATUS 76u  /**< `int32_t midi_status(int32_t fd, bmds_midi_status_t *status)`. */
#define BMDS_SYS_MIDI_CONTROL 77u /**< `int32_t midi_control(int32_t fd, uint32_t op, int32_t value)`. */
#define BMDS_SYS_AUDIO_SYNTH 78u  /**< `int32_t audio_synth(const bmds_synth_config_t *config)`: a new fd. */
#define BMDS_SYS_AUDIO_SYNTH_INFO 79u /**< `int32_t audio_synth_info(int32_t fd, bmds_synth_info_t *info)`. */
#define BMDS_SYS_STATS_LIST 80u       /**< `int32_t stats_list(uint32_t index, bmds_counter_t *info)`. */
#define BMDS_SYS_STATS_READ 81u       /**< `int32_t stats_read(ids, count, values, health, time_ns)`. */
#define BMDS_SYS_STATS_HISTORY 82u    /**< `int32_t stats_history(id, values, max, newest_ns)`. */
#define BMDS_SYS_STATS_PROBLEMS 83u   /**< `int32_t stats_problems(uint64_t after, bmds_problem_t *out, max)`. */
#define BMDS_SYS_STATS_CONTROL 84u    /**< `int32_t stats_control(uint32_t op, uint64_t value)`. */
#define BMDS_SYS_STATS_STATUS 85u     /**< `int32_t stats_status(bmds_stats_status_t *status)`. */
#define BMDS_SYS_TASK_LIST 86u        /**< `int32_t task_list(bmds_task_info_t *out, uint32_t max)`. */
#define BMDS_SYS_PROC_INFO 87u        /**< `int32_t proc_info(int32_t pid, uint32_t flags, bmds_proc_info_t *info)`. */
#define BMDS_SYS_SYSTEM_INFO 88u      /**< `int32_t system_info(bmds_system_info_t *info)`. */
#define BMDS_SYS_NOTIFY_OPEN 89u      /**< `int32_t notify_open(uint32_t flags)`: an fd of events. */
#define BMDS_SYS_NOTIFY_SEND 90u      /**< `int32_t notify_send(int32_t fd, const bmds_notification_t *n)`: its id. */
#define BMDS_SYS_NOTIFY_CLOSE 91u     /**< `int32_t notify_close(uint32_t id)`. */
#define BMDS_SYS_EXPAND 92u           /**< `int32_t expand(const char *text, char *out, uint32_t size)`. */

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

/** @brief `file_type` flags (appended 2026-10-07). */
#define BMDS_FILE_TYPE_NAME 0x1u    /**< By the name only: nothing read (the file needn't exist). */
#define BMDS_FILE_TYPE_CONTENT 0x2u /**< By the contents only, whatever the name says. */

/** @brief The longest type `file_type` gives, in bytes, without its terminating NUL. */
#define BMDS_FILE_TYPE_MAX 127u

/*
 * The system configuration (appended 2026-10-07, slots 59-65; devplan/sysconf.md §12): typed
 * parameters with defaults, named by keys (`wm/palette/desktop`), changed for the running system
 * and saved to /boot only when asked. Values are text in each type's canonical form. A key is
 * found once (`sysconf_find`); its id names it for the rest of the boot: never keep an id across
 * boots or write it anywhere.
 */

/** @brief The longest key, and the longest value, in bytes (without the terminating NUL). */
#define BMDS_SYSCONF_KEY_MAX 63u
#define BMDS_SYSCONF_VALUE_MAX 1024u

/** @brief Parameter types (`bmds_sysconf_info_t::type`, `bmds_sysconf_param_t::type`). */
#define BMDS_SYSCONF_TYPE_BOOL 0u   /**< `true` or `false` (also yes/no, on/off, 1/0 when set). */
#define BMDS_SYSCONF_TYPE_INT 1u    /**< Decimal, within `min` and `max`. */
#define BMDS_SYSCONF_TYPE_STRING 2u /**< UTF-8, at most `max` bytes (0: `BMDS_SYSCONF_VALUE_MAX`). */
#define BMDS_SYSCONF_TYPE_COLOR 3u  /**< `rrggbb` in hex (also `#RRGGBB` when set). */
#define BMDS_SYSCONF_TYPE_ENUM 4u   /**< One of the choices (words separated by `|`). */
#define BMDS_SYSCONF_TYPE_PATH 5u   /**< An absolute path, normalized. */
#define BMDS_SYSCONF_TYPE_PATHS 6u  /**< Absolute paths separated by `;`. */

/** @brief `sysconf_get` fields. */
#define BMDS_SYSCONF_FIELD_VALUE 0u   /**< The current value. */
#define BMDS_SYSCONF_FIELD_DEFAULT 1u /**< The default. */
#define BMDS_SYSCONF_FIELD_SAVED 2u   /**< The saved value (`-BMDS_ENOENT` if none). */
#define BMDS_SYSCONF_FIELD_KEY 3u     /**< The key. */
#define BMDS_SYSCONF_FIELD_HELP 4u    /**< One line describing it. */
#define BMDS_SYSCONF_FIELD_CHOICES 5u /**< An enum's words, separated by `|` ("" for other types). */

/** @brief `sysconf_set` operations. */
#define BMDS_SYSCONF_OP_SET 0u    /**< The current value becomes `value` (checked; not saved). */
#define BMDS_SYSCONF_OP_SETDEF 1u /**< The current value becomes the default (not saved). */
#define BMDS_SYSCONF_OP_REVERT 2u /**< The current value becomes the saved one, else the default. */
#define BMDS_SYSCONF_OP_SAVE 3u   /**< The current value is saved (the default: taken out of the store). */

/** @brief `sysconf_set`'s id with `BMDS_SYSCONF_OP_SAVE`: every changed parameter. */
#define BMDS_SYSCONF_ALL (-1)

/** @brief `bmds_sysconf_info_t::flags`. */
#define BMDS_SYSCONF_IS_DEFAULT 0x1u /**< The current value is the default. */
#define BMDS_SYSCONF_IS_SAVED 0x2u   /**< A value is saved for it. */
#define BMDS_SYSCONF_CHANGED 0x4u    /**< The current value isn't what a reboot would give. */
#define BMDS_SYSCONF_RUNTIME 0x8u    /**< A program declared it (`sysconf_declare`). */

/** @brief A watch descriptor's record after more changed than it kept: read again what matters. */
#define BMDS_SYSCONF_OVERFLOW 0xffffffffu

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

/** @brief What `sysconf_info` tells about a parameter (its texts: `sysconf_get`). */
typedef struct bmds_sysconf_info_t {
                uint32_t size;     /**< Set by the caller: `sizeof(bmds_sysconf_info_t)`. */
                uint32_t type;     /**< `BMDS_SYSCONF_TYPE_...`. */
                int32_t  min;      /**< An int's smallest value. */
                int32_t  max;      /**< An int's largest; a string's or path's longest (0: the most). */
                uint32_t flags;    /**< `BMDS_SYSCONF_IS_DEFAULT`, `_IS_SAVED`, `_CHANGED`, `_RUNTIME`. */
                uint32_t reserved; /**< 0. */
} bmds_sysconf_info_t;

/**
 * @brief A program's own parameter (`sysconf_declare`). The kernel copies
 *        the strings; the parameter stays until reboot.
 */
typedef struct bmds_sysconf_param_t {
                uint32_t    size;          /**< Set by the caller: `sizeof(bmds_sysconf_param_t)`. */
                uint32_t    type;          /**< `BMDS_SYSCONF_TYPE_...`. */
                const char *key;           /**< `owner/name/...`: lowercase letters, digits, `-`. */
                const char *default_value; /**< Its default (NULL: ""); must pass its type. */
                const char *help;          /**< One line describing it, or NULL. */
                const char *choices;       /**< An enum's words separated by `|`; else NULL. */
                int32_t     min;           /**< An int's smallest value. */
                int32_t     max;           /**< An int's largest; a string's or path's longest (0: the most). */
} bmds_sysconf_param_t;

/**
 * @brief `bmds_audio_config_t::format`: a sample's encoding. Little-endian unless named
 *        otherwise; signed integers are two's complement.
 */
#define BMDS_AUDIO_U8 0u      /**< Unsigned 8-bit, 128 the middle. */
#define BMDS_AUDIO_S8 1u      /**< Signed 8-bit. */
#define BMDS_AUDIO_S16LE 2u   /**< Signed 16-bit. */
#define BMDS_AUDIO_S16BE 3u   /**< Signed 16-bit, big-endian. */
#define BMDS_AUDIO_S24LE 4u   /**< Signed 24-bit in the low three bytes of four. */
#define BMDS_AUDIO_S24_3LE 5u /**< Signed 24-bit in three bytes. */
#define BMDS_AUDIO_S32LE 6u   /**< Signed 32-bit. */
#define BMDS_AUDIO_F32LE 7u   /**< IEEE single, full scale +-1.0. */
#define BMDS_AUDIO_MULAW 8u   /**< G.711 mu-law. */
#define BMDS_AUDIO_ALAW 9u    /**< G.711 A-law. */

/**
 * @brief How an audio fd plays (`audio_config`). Opening `/dev/dsp` sets 8 kHz, `BMDS_AUDIO_U8`,
 *        mono (OSS's defaults; `/dev/audio`: `BMDS_AUDIO_MULAW`), the sizes their defaults.
 *        Read back, the sizes are those chosen.
 */
typedef struct bmds_audio_config_t {
                uint32_t size;         /**< Set by the caller: `sizeof(bmds_audio_config_t)`. */
                uint32_t format;       /**< `BMDS_AUDIO_U8`... */
                uint32_t channels;     /**< 1 or 2 (interleaved, left first). */
                uint32_t rate;         /**< Frames a second: any from the status's `min_rate` to `max_rate`. */
                uint32_t ring_frames;  /**< The buffer (0: 100 ms); rounded up to a power of two in bytes. */
                uint32_t start_frames; /**< Plays once this many are queued (0: half the ring). */
                uint32_t wake_frames;  /**< `io_poll` says writable when this many fit (0: a device period).
                                            With `BMDS_AUDIO_REALTIME`: the period asked for (0: the
                                            device's), read back as the device's. */
                uint32_t flags;        /**< `BMDS_AUDIO_REALTIME`, `BMDS_AUDIO_EXCLUSIVE`, or 0. */
} bmds_audio_config_t;

/**
 * @brief `bmds_audio_config_t::flags` (appended 2026-10-08, `devplan/audio.md` §20). A
 *        real-time client's thread makes each period as the device asks (`audio_cycle`); its
 *        configuration is `BMDS_AUDIO_F32LE`, 1 or 2 channels, `ring_frames` and `start_frames`
 *        0, `rate` 0 (the device's) unless exclusive, and the period in `wake_frames`
 *        (`BMDS_AUDIO_MIN_PERIOD` to `BMDS_AUDIO_MAX_PERIOD`, or 0). The device runs at the
 *        shortest period its clients ask for.
 *
 *        Exclusive mode: no mixing and no limiter, only the client's samples, clamped; any
 *        `rate` the device takes (0: its setting's). Refused (`BMDS_EBUSY`) while another
 *        program has the device open; while it lasts, opens of the device fail with `BMDS_EBUSY`
 *        and system sounds are dropped. The device's volume still applies.
 */
#define BMDS_AUDIO_REALTIME 0x1u  /**< A real-time client: played through `audio_cycle`, not `write`. */
#define BMDS_AUDIO_EXCLUSIVE 0x2u /**< With `BMDS_AUDIO_REALTIME`: it owns the device. */

/** @brief A real-time client's period bounds, in frames (`bmds_audio_config_t::wake_frames`). */
#define BMDS_AUDIO_MIN_PERIOD 16u  /**< The shortest. */
#define BMDS_AUDIO_MAX_PERIOD 480u /**< The longest. */

/** @brief `bmds_audio_status_t::state`. */
#define BMDS_AUDIO_IDLE 0u     /**< Nothing queued, not playing (just opened, drained or dropped). */
#define BMDS_AUDIO_WAITING 1u  /**< Queued below `start_frames`: not playing yet. */
#define BMDS_AUDIO_PLAYING 2u  /**< Playing (an empty ring plays silence and counts an underrun). */
#define BMDS_AUDIO_PAUSED 3u   /**< Paused (`BMDS_AUDIO_PAUSE`): its ring is kept. */
#define BMDS_AUDIO_DRAINING 4u /**< Playing what is queued, then idle. */
#define BMDS_AUDIO_MIXER 5u    /**< A mixer (`/dev/mixer`): only the device's fields mean anything. */

/**
 * @brief An audio fd and its device now (`audio_status`). Frame counts are at the stream's rate.
 *        For A/V sync: a frame written now is heard `delay_frames` frames later. Rates are
 *        nominal: a device's clock may be steered to track another (AES67), so its real rate
 *        differs by parts per million; for time, pair `played_frames` with `timestamp_ns`.
 */
typedef struct bmds_audio_status_t {
                uint32_t size;           /**< Set by the caller: `sizeof(bmds_audio_status_t)`. */
                uint32_t state;          /**< `BMDS_AUDIO_IDLE`... */
                uint32_t device;         /**< The device's number (`/dev/dspN`). */
                uint32_t device_rate;    /**< The rate the device runs at (streams are resampled to it). */
                uint32_t period_frames;  /**< The device's period, at its rate. */
                uint32_t formats;        /**< The formats taken: bit N for format N. */
                uint32_t min_rate;       /**< The lowest `rate` taken. */
                uint32_t max_rate;       /**< The highest. */
                uint32_t max_channels;   /**< The most `channels` taken. */
                uint32_t ring_frames;    /**< The ring's size. */
                uint64_t written_frames; /**< Frames written since it was configured. */
                uint64_t played_frames;  /**< Of those, taken from the ring by the device. */
                int64_t  timestamp_ns;   /**< `BMDS_CLOCK_MONOTONIC` when `played_frames` last grew (0: never). */
                uint32_t queued_frames;  /**< In the ring now. */
                uint32_t delay_frames;   /**< Queued, plus what the device holds before it is heard. */
                uint32_t underruns;      /**< Times the ring ran dry while playing. */
                int32_t  gain_cdb;       /**< The stream's gain (`BMDS_AUDIO_GAIN`), centi-dB. */
                uint32_t late_periods;   /**< The device's periods mixed too late (silence) since it started. */
                uint32_t limited_frames; /**< The device's frames its limiter reduced since it started. */
} bmds_audio_status_t;

/**
 * @brief A period for a real-time client to make (`audio_cycle`). Its output is the kernel's
 *        memory: valid until the thread's next `audio_cycle` on the fd, or the fd's close.
 *        `rate` is nominal (the device's clock may be steered); `position` with `play_ns` is
 *        the device's real timeline.
 */
typedef struct bmds_audio_cycle_t {
                uint32_t     size;        /**< Set by the caller: `sizeof(bmds_audio_cycle_t)`. */
                uint32_t     frames;      /**< The period's frames (the device's period). */
                uint32_t     channels;    /**< As configured: `out` holds `frames` × `channels` floats. */
                uint32_t     rate;        /**< The device's rate. */
                float       *out;         /**< Where to write the period, interleaved, full scale +-1.0. */
                const float *in;          /**< The input's period (capture, later): NULL. */
                uint64_t     position;    /**< The device's frame count at `out[0]` since it started. */
                int64_t      deadline_ns; /**< `BMDS_CLOCK_MONOTONIC` by which to call `audio_cycle` again. */
                int64_t      play_ns;     /**< When `out[0]` leaves the device (the codec's filters follow). */
                uint32_t     skipped;     /**< Periods skipped since the last call (it was that late). */
                uint32_t     late;        /**< Periods not ended by their deadline, in all (silence instead). */
                uint32_t     throttled;   /**< Times its thread was put back to its priority, in all. */
                uint32_t     flags;       /**< 0. */
} bmds_audio_cycle_t;

/** @brief `audio_control` ops. */
#define BMDS_AUDIO_START 1u  /**< Plays now, without waiting for `start_frames`. */
#define BMDS_AUDIO_PAUSE 2u  /**< Pauses, keeping what is queued. */
#define BMDS_AUDIO_RESUME 3u /**< Resumes. */
#define BMDS_AUDIO_DRAIN 4u  /**< Waits until what is queued has played. */
#define BMDS_AUDIO_DROP 5u   /**< Throws away what is queued and stops. */
#define BMDS_AUDIO_GAIN 6u   /**< Sets the stream's gain: `value` centi-dB, -9600 (silent) to 0. */

/** @brief The silent end of `BMDS_AUDIO_GAIN`. */
#define BMDS_AUDIO_GAIN_MUTED (-9600)

/** @brief `sound_play`'s sounds. */
#define BMDS_SOUND_BELL 0u   /**< The terminal's bell (`audio/bell`). */
#define BMDS_SOUND_CLICK 1u  /**< A click (`audio/feedback`). */
#define BMDS_SOUND_ERROR 2u  /**< An error (`audio/feedback`). */
#define BMDS_SOUND_NOTIFY 3u /**< A notification (`audio/feedback`). */

/** @brief `bmds_decode_info_t::codec`: a sound file's coding (appended 2026-10-08). */
#define BMDS_CODEC_WAV 0u    /**< RIFF/WAVE. */
#define BMDS_CODEC_MP3 1u    /**< MPEG-1, 2 and 2.5 audio, layers I to III. */
#define BMDS_CODEC_VORBIS 2u /**< Ogg Vorbis. */
#define BMDS_CODEC_FLAC 3u   /**< FLAC. */
#define BMDS_CODEC_MIDI 4u   /**< A Standard MIDI File, played by the synth (`synth/font`; appended 2026-10-08). */

/** @brief `bmds_decode_config_t::format`: the decoder's own format (`bmds_decode_info_t::native_format`). */
#define BMDS_DECODE_NATIVE 0xffffffffu

/** @brief `bmds_decode_info_t::frames` when the length isn't known. */
#define BMDS_DECODE_UNKNOWN_FRAMES 0xffffffffffffffffull

/** @brief `bmds_decode_info_t::flags`. */
#define BMDS_DECODE_ESTIMATED 1u /**< `frames` from the size and bitrate (an MP3 without a Xing frame). */

/**
 * @brief What `audio_decode` decodes and how (appended 2026-10-08, `devplan/audio.md` §19).
 */
typedef struct bmds_decode_config_t {
                uint32_t size; /**< Set by the caller: `sizeof(bmds_decode_config_t)`. */
                uint32_t
                        format; /**< `BMDS_AUDIO_S16LE`, `BMDS_AUDIO_S32LE`, `BMDS_AUDIO_F32LE`, or `BMDS_DECODE_NATIVE`. */
                uint32_t    flags;  /**< `BMDS_O_CLOEXEC`, or 0. */
                uint32_t    length; /**< With fd -1: `data`'s bytes. */
                const void *data;   /**< With fd -1: the file in memory, copied by the call. */
} bmds_decode_config_t;

/**
 * @brief A decoded fd's file and what reads give (`audio_decode_info`).
 */
typedef struct bmds_decode_info_t {
                uint32_t size;          /**< Set by the caller: `sizeof(bmds_decode_info_t)`. */
                uint32_t codec;         /**< `BMDS_CODEC_WAV`... */
                char     name[8];       /**< The codec's name, NUL-terminated: `wav`, `mp3`, `vorbis`, `flac`. */
                uint32_t format;        /**< What reads give: a `BMDS_AUDIO_*` format, interleaved. */
                uint32_t channels;      /**< 1 or 2. */
                uint32_t rate;          /**< Frames a second. */
                uint32_t bitrate;       /**< The file's average, bits a second; 0 unknown. */
                uint64_t frames;        /**< The length, or `BMDS_DECODE_UNKNOWN_FRAMES`. */
                uint32_t flags;         /**< `BMDS_DECODE_ESTIMATED`. */
                uint32_t source_bits;   /**< A lossless file's sample size; 0 lossy. */
                uint32_t tags;          /**< The number of tags (`audio_decode_tag`). */
                uint32_t native_format; /**< The decoder's own format (`BMDS_DECODE_NATIVE` gives it). */
                uint32_t frame_bytes;   /**< The bytes of a frame read. */
} bmds_decode_info_t;

/** @brief `bmds_midi_config_t::flags` (appended 2026-10-08, `devplan/audio.md` §21). */
#define BMDS_MIDI_EVENTS 0x1u      /**< `read()` and `write()` move `bmds_midi_event_t` records, not bytes. */
#define BMDS_MIDI_CREATE 0x2u      /**< On `/dev/midinew`: make a virtual port named `name`. */
#define BMDS_MIDI_SOURCE 0x4u      /**< With `BMDS_MIDI_CREATE`: the port has a source (what the fd writes). */
#define BMDS_MIDI_DESTINATION 0x8u /**< With `BMDS_MIDI_CREATE`: it has a destination (what the fd reads). */

/** @brief The longest port name in bytes, with its NUL. */
#define BMDS_MIDI_NAME_MAX 32u

/**
 * @brief How a MIDI fd reads and writes (`midi_config`). An fd of `/dev/midiN` reads the port's
 *        source (what a keyboard plays) and writes to its destination; one of `/dev/midinew`
 *        configured `BMDS_MIDI_CREATE` owns a new port, whose source is what the fd writes and
 *        whose destination is what the fd reads.
 */
typedef struct bmds_midi_config_t {
                uint32_t size;       /**< Set by the caller: `sizeof(bmds_midi_config_t)`. */
                uint32_t flags;      /**< `BMDS_MIDI_*`. */
                uint32_t event_size; /**< With `BMDS_MIDI_EVENTS`: `sizeof(bmds_midi_event_t)` (16 to 256). */
                uint32_t port;       /**< Got: the port's number (`midiN`); 0xff before `BMDS_MIDI_CREATE`. */
                char     name[BMDS_MIDI_NAME_MAX]; /**< `BMDS_MIDI_CREATE`: the new port's name; got: the port's. */
} bmds_midi_config_t;

/**
 * @brief One MIDI message, or a piece of a SysEx (`BMDS_MIDI_EVENTS`). A message is whole, with
 *        no running status; a SysEx comes in pieces in order, `F0` first in the first and `F7`
 *        last in the last; a real-time byte (`F8`-`FF`) is an event of its own. Its time is when
 *        it arrived: to place it in an audio period, pair it with `audio_cycle`'s `position`
 *        and `play_ns`, never with a rate.
 */
typedef struct bmds_midi_event_t {
                int64_t time_ns; /**< Read: `BMDS_CLOCK_MONOTONIC` when it arrived. Written: 0 (now; others reserved). */
                uint8_t port;    /**< Read: the port whose source produced it (0xff: a write to this port's file). */
                uint8_t length;  /**< The bytes in `data`, 1 to 5. */
                uint8_t flags;   /**< 0. */
                uint8_t data[5]; /**< The message. */
} bmds_midi_event_t;

/** @brief `bmds_midi_status_t::kind`. */
#define BMDS_MIDI_USB 0u     /**< A USB MIDI device's cable. */
#define BMDS_MIDI_SERIAL 1u  /**< A serial MIDI port. */
#define BMDS_MIDI_VIRTUAL 2u /**< A program's (`BMDS_MIDI_CREATE`). */

/** @brief `bmds_midi_status_t::flags`. */
#define BMDS_MIDI_GONE 0x1u /**< The port went away (unplugged): reads end, writes fail with `EPIPE`. */

/**
 * @brief A MIDI fd's port and its counters (`midi_status`).
 */
typedef struct bmds_midi_status_t {
                uint32_t size;                     /**< Set by the caller: `sizeof(bmds_midi_status_t)`. */
                uint32_t port;                     /**< Its number (`midiN`); 0xff: `/dev/midinew` not created yet. */
                uint32_t kind;                     /**< `BMDS_MIDI_USB`... */
                uint32_t sides;                    /**< `BMDS_MIDI_SOURCE` and/or `BMDS_MIDI_DESTINATION`. */
                uint32_t flags;                    /**< `BMDS_MIDI_GONE`. */
                uint32_t dropped;                  /**< Events its destination had no room for. */
                uint64_t events_in;                /**< Events from its source. */
                uint64_t events_out;               /**< Events its destination took. */
                uint32_t parse_errors;             /**< Bytes its writers' streams threw away, SysExes cut. */
                uint32_t device_errors;            /**< Transfer, framing and overrun errors. */
                int64_t  last_ns;                  /**< `BMDS_CLOCK_MONOTONIC` of its last event (0: none yet). */
                uint64_t fd_events;                /**< Events this fd has read. */
                uint32_t fd_dropped;               /**< Events this fd's ring had no room for (it read too slowly). */
                uint32_t reserved;                 /**< 0. */
                char     name[BMDS_MIDI_NAME_MAX]; /**< The port's name, NUL-terminated. */
} bmds_midi_status_t;

/** @brief `midi_control` ops. */
#define BMDS_MIDI_CONNECT 1u    /**< Connects this fd's port's source to port `value`'s destination. */
#define BMDS_MIDI_DISCONNECT 2u /**< Ends that connection. */
#define BMDS_MIDI_FLUSH 3u      /**< Throws away what waits to be read. */

/**
 * @brief A General MIDI synth (`audio_synth`, appended 2026-10-08, `devplan/audio.md` §22): MIDI
 *        events written with their frames, samples read.
 */
typedef struct bmds_synth_config_t {
                uint32_t    size;       /**< Set by the caller: `sizeof(bmds_synth_config_t)`. */
                uint32_t    flags;      /**< `BMDS_O_CLOEXEC`, or 0. */
                uint32_t    rate;       /**< Frames a second, 8000 to 96000 (0: 48000). */
                uint32_t    channels;   /**< 1 or 2, interleaved (0: 2). */
                uint32_t    format;     /**< `BMDS_AUDIO_F32LE` or `BMDS_AUDIO_S16LE`. */
                uint32_t    max_voices; /**< Voices at most, up to 256 (0: 32); past it, voices are stolen. */
                const char *font;       /**< A SoundFont's path (SF2 or SF3); NULL: the `synth/font` setting. */
} bmds_synth_config_t;

/**
 * @brief A MIDI message for a synth fd's frame `frame` (its own count: the frames read so far).
 *        One for a frame already read plays at the next read's start; a later one waits, so a
 *        player writes ahead and each event lands on its sample. Whole messages, no running
 *        status; a SysEx whole in one event (GM System On).
 */
typedef struct bmds_synth_event_t {
                uint64_t frame;   /**< The frame it plays at. */
                uint8_t  length;  /**< The bytes in `data`, 1 to 6. */
                uint8_t  flags;   /**< 0. */
                uint8_t  data[6]; /**< The message. */
} bmds_synth_event_t;

/** @brief The most events a synth fd holds unplayed; past it, `write()` fails with `EAGAIN`. */
#define BMDS_SYNTH_QUEUE 1024u

/**
 * @brief A synth fd's state (`audio_synth_info`).
 */
typedef struct bmds_synth_info_t {
                uint32_t size;             /**< Set by the caller: `sizeof(bmds_synth_info_t)`. */
                uint32_t rate;             /**< As configured. */
                uint32_t channels;         /**< As configured. */
                uint32_t format;           /**< As configured. */
                uint64_t frames;           /**< Frames read so far: the frame the next read starts at. */
                uint32_t voices;           /**< Playing now. */
                uint32_t max_voices;       /**< The limit. */
                uint32_t peak_voices;      /**< The most at once. */
                uint32_t notes_at_limit;   /**< Note-ons with every voice busy (one stolen or the note dropped). */
                uint32_t queued;           /**< Events waiting for their frame. */
                uint32_t queue_full;       /**< Events refused because the queue was full. */
                uint32_t ignored;          /**< Messages it doesn't take. */
                uint32_t presets;          /**< The font's presets. */
                uint32_t font_bytes;       /**< The memory the font holds (shared by every synth on it). */
                uint32_t cpu_permille;     /**< The last read's render time over its frames' time, in 1/1000. */
                uint32_t cpu_permille_max; /**< The most since it opened. */
                uint32_t reserved;         /**< 0. */
                char     font[128];        /**< The font's path, NUL-terminated (cut at a character's end). */
} bmds_synth_info_t;

/* --- The system's counters, tasks and processes (devplan/sysmon.md §3.6) --- */

/** @brief A counter's value not measured now (CPU time off this boot, a device gone). */
#define BMDS_COUNTER_UNAVAILABLE INT64_MIN

/** @brief The longest counter name, without its NUL (`audio.dsp0.client_throttled`). */
#define BMDS_COUNTER_NAME_MAX 47u

/** @brief `bmds_counter_t::unit`. */
#define BMDS_UNIT_COUNT 0u
#define BMDS_UNIT_BYTES 1u
#define BMDS_UNIT_MICROS 2u   /**< µs. */
#define BMDS_UNIT_PERMILLE 3u /**< 1/1000. */
#define BMDS_UNIT_HZ 4u
#define BMDS_UNIT_MILLIHZ 5u
#define BMDS_UNIT_PPM 6u
#define BMDS_UNIT_FRAMES 7u
#define BMDS_UNIT_CYCLES 8u
#define BMDS_UNIT_DECIBELS 9u
#define BMDS_UNIT_NANOS 10u
#define BMDS_UNIT_PPB 11u

/** @brief `bmds_counter_t::kind`. */
#define BMDS_COUNTER_TOTAL 0u /**< Running, only grows (a restart may reset it): a total and a rate. */
#define BMDS_COUNTER_LEVEL 1u /**< A value now. */
#define BMDS_COUNTER_PEAK 2u  /**< A running largest (a restart may reset it). */
#define BMDS_COUNTER_STATE 3u /**< A number with labels (`bmds_counter_t::labels`). */
#define BMDS_COUNTER_INFO 4u  /**< A fact that seldom changes (a rate, a size). */

/** @brief `bmds_counter_t::flags`. */
#define BMDS_COUNTER_HISTORY 0x1u  /**< Its samples are kept while anyone watches (`stats_history`). */
#define BMDS_COUNTER_ADJUSTED 0x2u /**< Its levels are set from what its component knows (a period, a size). */

/** @brief `bmds_counter_t::rule`: what is abnormal. */
#define BMDS_RULE_NONE 0u  /**< Never judged. */
#define BMDS_RULE_ZERO 1u  /**< Any increase is a problem (`health`), for `hold` seconds after the last. */
#define BMDS_RULE_ABOVE 2u /**< Above `warn` or `crit` for `hold` s (a total's rate a second; a peak rising past). */
#define BMDS_RULE_BELOW 3u /**< Below `warn` or `crit` for `hold` s. */
#define BMDS_RULE_DRIFT 4u /**< Its magnitude above `warn` or `crit` for `hold` s. */
#define BMDS_RULE_STATE 5u /**< In a state whose bit is set in `warn` or `crit`. */

/** @brief A counter's or problem's health. */
#define BMDS_HEALTH_NORMAL 0u
#define BMDS_HEALTH_WARNING 1u    /**< Amber. */
#define BMDS_HEALTH_CRITICAL 2u   /**< Red. */
#define BMDS_HEALTH_UNJUDGED 255u /**< `stats_read`: nobody watches, so nothing is judged. */

/** @brief `bmds_counter_t::warn` and `crit`: a level not used. */
#define BMDS_COUNTER_NO_LEVEL INT64_MIN

/**
 * @brief A counter (`stats_list`): what it is and its rule as in force (adjusted, then
 *        overridden by `sysmon/limits`). Its name, unit, kind and labels are an interface.
 */
typedef struct bmds_counter_t {
                uint32_t size;       /**< Set by the caller: `sizeof(bmds_counter_t)`. */
                uint32_t id;         /**< For `stats_read` and `stats_history`; never another counter's. */
                uint32_t generation; /**< The registry's: it changes as counters come and go. */
                uint32_t unit;       /**< `BMDS_UNIT_...`. */
                uint32_t kind;       /**< `BMDS_COUNTER_TOTAL`, ... */
                uint32_t flags;      /**< `BMDS_COUNTER_HISTORY`, `BMDS_COUNTER_ADJUSTED`. */
                uint32_t rule;       /**< `BMDS_RULE_...`. */
                uint32_t health;     /**< `BMDS_RULE_ZERO`: the problem an increase is. */
                uint32_t hold;       /**< Seconds (see `rule`). */
                uint32_t reserved;   /**< 0. */
                int64_t warn; /**< The warning level (`BMDS_RULE_STATE`: a bit per state), or `BMDS_COUNTER_NO_LEVEL`. */
                int64_t crit;       /**< The critical level, likewise. */
                char    name[48];   /**< `audio.dsp0.late`, NUL-terminated. */
                char    labels[64]; /**< `BMDS_COUNTER_STATE`: the states' names, `|` between (`stopped|running`). */
                char    help[128];  /**< One line saying what it counts (cut at a character's end). */
} bmds_counter_t;

/** @brief `bmds_problem_t::flags`. */
#define BMDS_PROBLEM_OPEN 0x1u    /**< Not normal again yet. */
#define BMDS_PROBLEM_EARLIER 0x2u /**< Found at the first look: it happened before anyone watched. */

/**
 * @brief A problem (`stats_problems`): one episode of a counter out of normal, from when it
 *        began until it was normal again.
 */
typedef struct bmds_problem_t {
                uint32_t size;        /**< Set by the caller: `sizeof(bmds_problem_t)`. */
                uint32_t id;          /**< The counter's. */
                uint32_t health;      /**< The worst it reached: `BMDS_HEALTH_WARNING` or `_CRITICAL`. */
                uint32_t flags;       /**< `BMDS_PROBLEM_OPEN`, `BMDS_PROBLEM_EARLIER`. */
                uint64_t changed;     /**< The change that last touched it (pass the last one's as `after`). */
                int64_t  start_ns;    /**< When it began (monotonic). */
                int64_t  end_ns;      /**< When it ended (open: its last update). */
                int64_t  first_value; /**< The counter's value as it began. */
                int64_t  last_value;  /**< At the end (open: now). */
                uint64_t count;       /**< An increase rule's increases; a level rule's samples out of normal. */
                char     name[48];    /**< The counter's name. */
} bmds_problem_t;

/** @brief `stats_control` ops. */
#define BMDS_STATS_WATCH 1u /**< `value` 1: this process watches (the sampler runs); 0: stops. */
#define BMDS_STATS_SEEN 2u  /**< The problems up to change `value` were shown to the user. */
#define BMDS_STATS_CLEAR 3u /**< Forget the problems that have ended. */

/** @brief `bmds_stats_status_t::flags`. */
#define BMDS_STATS_CPU_COUNTED 0x1u /**< Tasks' CPU time is counted this boot (`sysmon/enable`). */
#define BMDS_STATS_WATCHING 0x2u    /**< The calling process watches. */

/** @brief The counters' registry as a whole (`stats_status`). */
typedef struct bmds_stats_status_t {
                uint32_t size;           /**< Set by the caller: `sizeof(bmds_stats_status_t)`. */
                uint32_t generation;     /**< As `bmds_counter_t::generation`. */
                uint32_t count;          /**< The counters. */
                uint32_t watchers;       /**< Watching (processes, the shell's `stats -w`). */
                uint32_t interval_ms;    /**< Between samples (`sysmon/interval`). */
                uint32_t flags;          /**< `BMDS_STATS_CPU_COUNTED`, `BMDS_STATS_WATCHING`. */
                uint32_t unseen;         /**< The worst health of the open problems and those not yet seen. */
                uint32_t cores;          /**< CPU cores. */
                uint64_t last_change;    /**< The newest problem change. */
                int64_t  last_sample_ns; /**< When the last sample was taken (monotonic; 0: none). */
} bmds_stats_status_t;

/** @brief `bmds_task_info_t::state` (FreeRTOS's). */
#define BMDS_TASK_RUNNING 0u
#define BMDS_TASK_READY 1u
#define BMDS_TASK_BLOCKED 2u
#define BMDS_TASK_SUSPENDED 3u
#define BMDS_TASK_DELETED 4u

/** @brief `bmds_task_info_t::flags`. */
#define BMDS_TASK_MAIN 0x1u /**< A process's first thread. */

/** @brief A task with less stack than these never used is in danger (warn, critical). */
#define BMDS_TASK_STACK_WARN 512u
#define BMDS_TASK_STACK_CRIT 256u

/** @brief A task (`task_list`): a process's thread, or the kernel's. */
typedef struct bmds_task_info_t {
                uint32_t size;          /**< Set by the caller: `sizeof(bmds_task_info_t)`. */
                int32_t  pid;           /**< Its process (0: the kernel's). */
                int32_t  tid;           /**< A program thread's id (0: a kernel task or thread). */
                uint32_t state;         /**< `BMDS_TASK_...`. */
                uint32_t priority;      /**< Now (raised while it holds a lock someone waits on). */
                uint32_t base_priority; /**< Its own. */
                int32_t  core;          /**< The core it is pinned to, or -1. */
                uint32_t stack_free;    /**< Bytes of its stack never used. */
                uint64_t cpu_us;        /**< Time it has run (0: not counted, see `stats_status`). */
                uint32_t number;        /**< A number no other task has had this boot. */
                uint32_t flags;         /**< `BMDS_TASK_MAIN`. */
                char     name[16];      /**< NUL-terminated. */
} bmds_task_info_t;

/** @brief `proc_info` flags. */
#define BMDS_PROC_INFO_NEXT 0x1u /**< The first process after `pid` (0: the first). */

/** @brief `bmds_proc_info_t::state`. */
#define BMDS_PROC_RUNNING 0u /**< Its threads run. */
#define BMDS_PROC_VFORK 1u   /**< A vfork child, on its parent's task (its time is the parent's). */
#define BMDS_PROC_ENDING 2u  /**< Its program returned; its threads are ending. */
#define BMDS_PROC_ZOMBIE 3u  /**< Ended; its parent hasn't waited for it. */
#define BMDS_PROC_STOPPED 4u /**< Stopped by a signal, until continued. */

/** @brief A process (`proc_info`). */
typedef struct bmds_proc_info_t {
                uint32_t size;        /**< Set by the caller: `sizeof(bmds_proc_info_t)`. */
                int32_t  pid;         /**< Its id. */
                int32_t  parent;      /**< Its parent's (0: none; -1: the kernel). */
                int32_t  group;       /**< Its process group. */
                int32_t  session;     /**< Its session. */
                uint32_t state;       /**< `BMDS_PROC_RUNNING`, ... */
                uint32_t threads;     /**< Its tasks. */
                int32_t  status;      /**< A zombie's wait status. */
                uint64_t memory;      /**< Bytes it has allocated. */
                uint64_t cpu_us;      /**< Its tasks' time, ended ones' and before an exec included. */
                int64_t  start_ns;    /**< When it started (monotonic). */
                char     command[48]; /**< Its command line, cut. */
                /** Its children's CPU time: those it waited for, with theirs (appended 2026-10-10:
                    0 from an older kernel). */
                uint64_t children_cpu_us;
} bmds_proc_info_t;

/**
 * @brief The system (`system_info`, devplan/notifications.md §2): its serial number, its host
 *        name, the board, BMDS's version, the cores. The caller sets `size`.
 */
typedef struct bmds_system_info_t {
                uint32_t size;          /**< Set by the caller: `sizeof(bmds_system_info_t)`. */
                uint32_t cores;         /**< CPU cores. */
                uint32_t cpu_mhz;       /**< Their clock. */
                uint32_t chip_revision; /**< The chip's revision: major × 100 + minor. */
                char     serial[32];    /**< The board's serial number (this board's: its chip's MAC, 12 hex digits). */
                char     hostname[64];  /**< The host name (`system/hostname`, else `bmds-` and the serial's end). */
                char     board[32];     /**< The board's name. */
                char     version[16];   /**< BMDS's version. */
} bmds_system_info_t;

/** @brief `bmds_notification_t::urgency` (devplan/notifications.md §3.1). */
#define BMDS_NOTIFY_LOW 0u      /**< A balloon, no sound. */
#define BMDS_NOTIFY_NORMAL 1u   /**< A balloon and a sound. */
#define BMDS_NOTIFY_CRITICAL 2u /**< A sound, a red edge; it stays until dismissed. */

/** @brief `bmds_notification_t::flags`. */
#define BMDS_NOTIFY_SILENT 0x1u    /**< No sound. */
#define BMDS_NOTIFY_TRANSIENT 0x2u /**< Not kept in the centre once its balloon is gone. */

/** @brief `bmds_notification_t::target`: what a click does (the system does it). */
#define BMDS_NOTIFY_TARGET_NONE 0u
#define BMDS_NOTIFY_TARGET_OPEN 1u    /**< `target_text`: a path, opened by its type's application. */
#define BMDS_NOTIFY_TARGET_SHOW 2u    /**< `target_text`: a path, shown selected in its folder. */
#define BMDS_NOTIFY_TARGET_LAUNCH 3u  /**< `target_text`: a Desktop Entry's id, started. */
#define BMDS_NOTIFY_TARGET_SERVICE 4u /**< `target_text`: a service's name, sent `message`. */

/** @brief `bmds_notification_t::timeout_ms`'s special values. */
#define BMDS_NOTIFY_DEFAULT_TIMEOUT 0 /**< The setting's (`notify/timeout`). */
#define BMDS_NOTIFY_NO_TIMEOUT (-1)   /**< Until dismissed. */

/** @brief The most action buttons a notification has. */
#define BMDS_NOTIFY_MAX_ACTIONS 3u

/** @brief `bmds_notify_event_t::event`. */
#define BMDS_NOTIFY_ACTIVATED 1u /**< Clicked: `action` 0 the body, 1 to 3 a button. */
#define BMDS_NOTIFY_DISMISSED 2u /**< Closed by the user. */
#define BMDS_NOTIFY_EXPIRED 3u   /**< Its balloon's time passed (it stays in the centre). */

/**
 * @brief A notification to post (`notify_send`, devplan/notifications.md §3). The caller sets
 *        `size`; texts are UTF-8, cut at a character's end where they don't fit.
 */
typedef struct bmds_notification_t {
                uint32_t size;             /**< Set by the caller: `sizeof(bmds_notification_t)`. */
                uint32_t replaces;         /**< One of the caller's to update in place (0: a new one). */
                uint32_t urgency;          /**< `BMDS_NOTIFY_LOW`, `_NORMAL`, `_CRITICAL`. */
                uint32_t flags;            /**< `BMDS_NOTIFY_SILENT`, `_TRANSIENT`. */
                int32_t  timeout_ms;       /**< Its balloon's, `BMDS_NOTIFY_DEFAULT_TIMEOUT` or `_NO_TIMEOUT`. */
                uint32_t target;           /**< `BMDS_NOTIFY_TARGET_...`. */
                uint32_t action_count;     /**< Action buttons, up to `BMDS_NOTIFY_MAX_ACTIONS`. */
                char     app[32];          /**< The application's name (empty: the program's). */
                char     icon[64];         /**< A theme icon's name (empty: the application's). */
                char     title[64];        /**< The title. */
                char     body[256];        /**< The text; may have `\n`. */
                char     target_text[192]; /**< The target's path, Desktop Entry or service. */
                char     message[64];      /**< `BMDS_NOTIFY_TARGET_SERVICE`'s message. */
                char     actions[3][24];   /**< The buttons' labels. */
} bmds_notification_t;

/** @brief An event of a notification, read from its events fd (`notify_open`). */
typedef struct bmds_notify_event_t {
                uint32_t size;   /**< `sizeof(bmds_notify_event_t)`. */
                uint32_t id;     /**< The notification. */
                uint32_t event;  /**< `BMDS_NOTIFY_ACTIVATED`, `_DISMISSED`, `_EXPIRED`. */
                uint32_t action; /**< `BMDS_NOTIFY_ACTIVATED`'s: 0 the body, 1 to 3 a button. */
} bmds_notify_event_t;

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
typedef int32_t (*bmds_sys_file_type_fn)(const char *path, uint32_t flags, char *out, uint32_t size);
typedef int32_t (*bmds_sys_sysconf_find_fn)(const char *key);
typedef int32_t (*bmds_sys_sysconf_get_fn)(int32_t id, uint32_t field, char *out, uint32_t size);
typedef int32_t (*bmds_sys_sysconf_info_fn)(int32_t id, bmds_sysconf_info_t *info);
typedef int32_t (*bmds_sys_sysconf_set_fn)(int32_t id, uint32_t op, const char *value);
typedef int32_t (*bmds_sys_sysconf_next_fn)(const char *prefix, int32_t after);
typedef int32_t (*bmds_sys_sysconf_declare_fn)(const bmds_sysconf_param_t *param);
typedef int32_t (*bmds_sys_sysconf_watch_fn)(const char *prefix, uint32_t flags);
typedef int32_t (*bmds_sys_sync_fn)(void);
typedef int32_t (*bmds_sys_audio_config_fn)(int32_t fd, const bmds_audio_config_t *set, bmds_audio_config_t *got);
typedef int32_t (*bmds_sys_audio_status_fn)(int32_t fd, bmds_audio_status_t *status);
typedef int32_t (*bmds_sys_audio_control_fn)(int32_t fd, uint32_t op, int32_t value);
typedef int32_t (*bmds_sys_sound_play_fn)(uint32_t sound);
typedef int32_t (*bmds_sys_audio_decode_fn)(int32_t fd, const bmds_decode_config_t *config);
typedef int32_t (*bmds_sys_audio_decode_info_fn)(int32_t fd, bmds_decode_info_t *info);
typedef int32_t (*bmds_sys_audio_decode_tag_fn)(int32_t fd, uint32_t index, char *key, uint32_t key_size, char *value,
                                                uint32_t value_size);
typedef int32_t (*bmds_sys_audio_cycle_fn)(int32_t fd, bmds_audio_cycle_t *cycle);
typedef int32_t (*bmds_sys_midi_config_fn)(int32_t fd, const bmds_midi_config_t *set, bmds_midi_config_t *got);
typedef int32_t (*bmds_sys_midi_status_fn)(int32_t fd, bmds_midi_status_t *status);
typedef int32_t (*bmds_sys_midi_control_fn)(int32_t fd, uint32_t op, int32_t value);
typedef int32_t (*bmds_sys_audio_synth_fn)(const bmds_synth_config_t *config);
typedef int32_t (*bmds_sys_audio_synth_info_fn)(int32_t fd, bmds_synth_info_t *info);
typedef int32_t (*bmds_sys_stats_list_fn)(uint32_t index, bmds_counter_t *info);
typedef int32_t (*bmds_sys_stats_read_fn)(const uint32_t *ids, uint32_t count, int64_t *values, uint8_t *health,
                                          int64_t *time_ns);
typedef int32_t (*bmds_sys_stats_history_fn)(uint32_t id, int64_t *values, uint32_t max, int64_t *newest_ns);
typedef int32_t (*bmds_sys_stats_problems_fn)(uint64_t after, bmds_problem_t *out, uint32_t max);
typedef int32_t (*bmds_sys_stats_control_fn)(uint32_t op, uint64_t value);
typedef int32_t (*bmds_sys_stats_status_fn)(bmds_stats_status_t *status);
typedef int32_t (*bmds_sys_task_list_fn)(bmds_task_info_t *out, uint32_t max);
typedef int32_t (*bmds_sys_proc_info_fn)(int32_t pid, uint32_t flags, bmds_proc_info_t *info);
typedef int32_t (*bmds_sys_system_info_fn)(bmds_system_info_t *info);
typedef int32_t (*bmds_sys_notify_open_fn)(uint32_t flags);
typedef int32_t (*bmds_sys_notify_send_fn)(int32_t fd, const bmds_notification_t *n);
typedef int32_t (*bmds_sys_notify_close_fn)(uint32_t id);
typedef int32_t (*bmds_sys_expand_fn)(const char *text, char *out, uint32_t size);

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

/**
 * @brief Tells what the file @p path is, as a MIME type (freedesktop.org's
 *        names: "text/plain", "image/png", "application/x-executable"): by
 *        its name's extension, and when that says nothing, by its first 512
 *        bytes (magic numbers, else text or "application/octet-stream"). A
 *        directory is "inode/directory", a device "inode/chardevice", an empty
 *        file "application/x-zerosize". Symbolic links are followed. With
 *        `BMDS_FILE_TYPE_NAME` nothing is read, and a name not known is
 *        "application/octet-stream"; with `BMDS_FILE_TYPE_CONTENT` the name is
 *        not used. The same file always gets the same answer.
 *        (Slot 58, appended 2026-10-07: check `bmds_sys_has()`.)
 * @param out Gets the type, NUL-ended; `BMDS_FILE_TYPE_MAX` + 1 bytes always fit.
 * @return The type's length, or `-BMDS_ERANGE` (@p size too small),
 *         `-BMDS_ENOENT`, `-BMDS_EINVAL` (an unknown flag), `-BMDS_EIO`...
 */
static inline int32_t bmds_file_type(const char *path, uint32_t flags, char *out, uint32_t size) {
        return ((bmds_sys_file_type_fn)bmds_sys->slot[BMDS_SYS_FILE_TYPE])(path, flags, out, size);
}

/**
 * @brief Finds a system configuration parameter (slots 59-65, appended 2026-10-07: check
 *        `bmds_sys_has(BMDS_SYS_SYSCONF_FIND)`).
 * @return Its id for the rest of the boot (>= 0), or `-BMDS_ENOENT`.
 */
static inline int32_t bmds_sysconf_find(const char *key) {
        return ((bmds_sys_sysconf_find_fn)bmds_sys->slot[BMDS_SYS_SYSCONF_FIND])(key);
}

/**
 * @brief Copies a text of parameter @p id into @p out, NUL-ended: its value, default, saved
 *        value, key, help or choices (`BMDS_SYSCONF_FIELD_...`).
 *        `BMDS_SYSCONF_VALUE_MAX` + 1 bytes always fit.
 * @return The text's length, or `-BMDS_ERANGE` (@p size too small), `-BMDS_ENOENT` (no such
 *         id; no saved value), `-BMDS_EINVAL` (an unknown field).
 */
static inline int32_t bmds_sysconf_get(int32_t id, uint32_t field, char *out, uint32_t size) {
        return ((bmds_sys_sysconf_get_fn)bmds_sys->slot[BMDS_SYS_SYSCONF_GET])(id, field, out, size);
}

/**
 * @brief Fills @p info (its `size` set) about parameter @p id.
 * @return 0, or `-BMDS_ENOENT`, `-BMDS_EINVAL` (`size`).
 */
static inline int32_t bmds_sysconf_info(int32_t id, bmds_sysconf_info_t *info) {
        return ((bmds_sys_sysconf_info_fn)bmds_sys->slot[BMDS_SYS_SYSCONF_INFO])(id, info);
}

/**
 * @brief Changes parameter @p id: `BMDS_SYSCONF_OP_SET` (to @p value, checked against its type
 *        and limits), `_SETDEF`, `_REVERT` (none save), or `_SAVE` (`BMDS_SYSCONF_ALL`: every
 *        changed one). Watchers are told of a changed value.
 * @return 0, or `-BMDS_ENOENT` (no such id; a path that must exist), `-BMDS_EINVAL` (not of the
 *         type; an unknown op), `-BMDS_ERANGE` (outside its limits), `-BMDS_ENOTDIR`, `-BMDS_EIO`
 *         (saving).
 */
static inline int32_t bmds_sysconf_set(int32_t id, uint32_t op, const char *value) {
        return ((bmds_sys_sysconf_set_fn)bmds_sys->slot[BMDS_SYS_SYSCONF_SET])(id, op, value);
}

/**
 * @brief Lists: the first parameter after id @p after (-1: from the start), in key order, whose
 *        key is @p prefix or under it (`wm/palette` gives `wm/palette/...`; "" all).
 * @return Its id, or `-BMDS_ENOENT` at the end.
 */
static inline int32_t bmds_sysconf_next(const char *prefix, int32_t after) {
        return ((bmds_sys_sysconf_next_fn)bmds_sys->slot[BMDS_SYS_SYSCONF_NEXT])(prefix, after);
}

/**
 * @brief Declares the program's own parameter. A saved value for it applies now; declaring it
 *        again (a program started twice) with the same type gives the same id.
 * @return Its id, or `-BMDS_EEXIST` (declared with another type), `-BMDS_EINVAL` (the key, the
 *         type, the default), `-BMDS_ENOSPC` (64 declared already).
 */
static inline int32_t bmds_sysconf_declare(const bmds_sysconf_param_t *param) {
        return ((bmds_sys_sysconf_declare_fn)bmds_sys->slot[BMDS_SYS_SYSCONF_DECLARE])(param);
}

/**
 * @brief Watches the parameters under @p prefix ("" all): a new descriptor whose reads give
 *        whole `uint32_t` records, the id of each parameter changed (once until read), or
 *        `BMDS_SYSCONF_OVERFLOW` when more changed than it kept. A read waits while nothing
 *        changed; `io_poll` reports `BMDS_POLL_IN` when something did. A size that isn't a
 *        multiple of 4 is `-BMDS_EINVAL`. Closing it ends the watch.
 * @param flags `BMDS_O_CLOEXEC` or 0.
 * @return The descriptor, or `-BMDS_EINVAL` (the prefix, a flag), `-BMDS_EMFILE`.
 */
static inline int32_t bmds_sysconf_watch(const char *prefix, uint32_t flags) {
        return ((bmds_sys_sysconf_watch_fn)bmds_sys->slot[BMDS_SYS_SYSCONF_WATCH])(prefix, flags);
}

/**
 * @brief Writes what the filesystems hold to their devices: a card's directories and allocation,
 *        otherwise written within a second, and `/boot`'s queue. Before a card is taken out.
 *        (Slot 66, appended 2026-10-08: check `bmds_sys_has()`.)
 * @return 0, or `-BMDS_EIO` (a device failed: what it held is lost), `-BMDS_ENOMEDIUM`...
 */
static inline int32_t bmds_sync(void) {
        return ((bmds_sys_sync_fn)bmds_sys->slot[BMDS_SYS_SYNC])();
}

/**
 * @brief Configures audio fd @p fd (opened from `/dev/dsp*` for writing) from @p set, if not
 *        NULL, then fills @p got, if not NULL, with what it chose. Queued audio plays out first.
 *        (Slots 67-70, appended 2026-10-08: check `bmds_sys_has(BMDS_SYS_AUDIO_CONFIG)`.)
 * @return 0, or `-BMDS_ENOTTY` (not an audio fd), `-BMDS_ENOTSUP` (setting a mixer's),
 *         `-BMDS_EINVAL` (a format, channel count, rate, ring over 10 s or flag not taken;
 *         `size`), `-BMDS_ENOMEM`, `-BMDS_EINTR` (while playing out).
 */
static inline int32_t bmds_audio_config(int32_t fd, const bmds_audio_config_t *set, bmds_audio_config_t *got) {
        return ((bmds_sys_audio_config_fn)bmds_sys->slot[BMDS_SYS_AUDIO_CONFIG])(fd, set, got);
}

/**
 * @brief Fills @p status (its `size` set) about audio fd @p fd and its device.
 * @return 0, or `-BMDS_ENOTTY` (not an audio or mixer fd), `-BMDS_EINVAL` (`size`).
 */
static inline int32_t bmds_audio_status(int32_t fd, bmds_audio_status_t *status) {
        return ((bmds_sys_audio_status_fn)bmds_sys->slot[BMDS_SYS_AUDIO_STATUS])(fd, status);
}

/**
 * @brief Does @p op (`BMDS_AUDIO_START`...) on audio fd @p fd; @p value is `BMDS_AUDIO_GAIN`'s,
 *        else 0. `BMDS_AUDIO_DRAIN` blocks; a signal ends it (`-BMDS_EINTR`) and it plays on.
 * @return 0, or `-BMDS_ENOTTY`, `-BMDS_ENOTSUP` (a mixer), `-BMDS_EINVAL` (an unknown op; a
 *         value out of range), `-BMDS_EINTR`.
 */
static inline int32_t bmds_audio_control(int32_t fd, uint32_t op, int32_t value) {
        return ((bmds_sys_audio_control_fn)bmds_sys->slot[BMDS_SYS_AUDIO_CONTROL])(fd, op, value);
}

/**
 * @brief Plays system sound @p sound (`BMDS_SOUND_BELL`...) on the default device, if the
 *        settings allow it (`audio/bell`, `audio/feedback`); one already sounding isn't repeated.
 * @return 0 (also when the settings say no, or there is no audio device), or `-BMDS_EINVAL`.
 */
static inline int32_t bmds_sound_play(uint32_t sound) {
        return ((bmds_sys_sound_play_fn)bmds_sys->slot[BMDS_SYS_SOUND_PLAY])(sound);
}

/**
 * @brief Opens a decoder on the sound file @p fd (readable and seekable; or, with @p fd -1,
 *        @p config's `data`): a new fd whose reads give PCM in @p config's `format`, at the
 *        file's rate and channels. `lseek` on it is in bytes of that PCM and exact; `fstat`'s
 *        size is its length when known; it is read-only. Decoding runs in the reading thread.
 *        The file's fd may be closed meanwhile, but not read or sought from another thread.
 *        (Slots 71-73, appended 2026-10-08: check `bmds_sys_has(BMDS_SYS_AUDIO_DECODE)`.)
 * @return The fd, or `-BMDS_EINVAL` (not a sound file known; a bad format, flag or size),
 *         `-BMDS_ENOTSUP` (a known format in a form not played: Ogg Opus, more than two
 *         channels; or @p fd doesn't seek), `-BMDS_EBADF`, `-BMDS_EFAULT`, `-BMDS_ENOMEM`,
 *         `-BMDS_EMFILE`, `-BMDS_EIO` (a read failed). Reads of a damaged file fail with `EIO`.
 */
static inline int32_t bmds_audio_decode(int32_t fd, const bmds_decode_config_t *config) {
        return ((bmds_sys_audio_decode_fn)bmds_sys->slot[BMDS_SYS_AUDIO_DECODE])(fd, config);
}

/**
 * @brief Fills @p info (its `size` set) about decoded fd @p fd.
 * @return 0, or `-BMDS_ENOTTY` (not a decoded fd), `-BMDS_EBADF`, `-BMDS_EFAULT`, `-BMDS_EINVAL`.
 */
static inline int32_t bmds_audio_decode_info(int32_t fd, bmds_decode_info_t *info) {
        return ((bmds_sys_audio_decode_info_fn)bmds_sys->slot[BMDS_SYS_AUDIO_DECODE_INFO])(fd, info);
}

/**
 * @brief Copies tag @p index (0 the first) of decoded fd @p fd: its key, in capitals (Vorbis
 *        comment names: `TITLE`, `ARTIST`, `ALBUM`, `TRACKNUMBER`, `DATE`...), into @p key and
 *        its value, UTF-8, into @p value, each NUL-terminated and cut at a character's end to
 *        fit (either may be NULL with a size of 0).
 * @return The value's whole length in bytes (more than @p value_size - 1: it was cut), or
 *         `-BMDS_ENOENT` (past the last), `-BMDS_ENOTTY`, `-BMDS_EBADF`.
 */
static inline int32_t bmds_audio_decode_tag(int32_t fd, uint32_t index, char *key, uint32_t key_size, char *value,
                                            uint32_t value_size) {
        return ((bmds_sys_audio_decode_tag_fn)bmds_sys->slot[BMDS_SYS_AUDIO_DECODE_TAG])(fd, index, key, key_size,
                                                                                         value, value_size);
}

/**
 * @brief A real-time client's thread (audio fd @p fd configured `BMDS_AUDIO_REALTIME`): ends
 *        the period it was given, then waits for the next and fills @p cycle (its `size` set)
 *        in; with @p cycle NULL it only ends the period. From its first period the thread runs
 *        at the real-time priority, under the device's own task; one that stays out of
 *        `audio_cycle` for 4 periods is put back to its priority until it calls again. A period
 *        not ended by its `deadline_ns` (a period after it was given) plays silence; a sample
 *        written is heard about two periods after the call returned.
 *        (Slot 74, appended 2026-10-08: check `bmds_sys_has(BMDS_SYS_AUDIO_CYCLE)`.)
 * @return 0, or `-BMDS_EINTR` (a signal: the period is late), `-BMDS_EPIPE` (the device stopped
 *         it, `audio stop`: the next call joins again), `-BMDS_EINVAL` (not real-time; `size`),
 *         `-BMDS_ENOTTY`, `-BMDS_EBADF`, `-BMDS_EBUSY` (exclusive mode taken, rejoining).
 */
static inline int32_t bmds_audio_cycle(int32_t fd, bmds_audio_cycle_t *cycle) {
        return ((bmds_sys_audio_cycle_fn)bmds_sys->slot[BMDS_SYS_AUDIO_CYCLE])(fd, cycle);
}

/**
 * @brief Configures MIDI fd @p fd (`/dev/midiN`, `/dev/midi`, `/dev/midinew`; its `size` set in
 *        each struct) from @p set, unless NULL, and fills @p got, unless NULL, with the result.
 *        Fds read bytes until configured `BMDS_MIDI_EVENTS`. `BMDS_MIDI_CREATE` makes a
 *        virtual port, once, on `/dev/midinew` only; it ends with the fd.
 *        (Slots 75-77, appended 2026-10-08: check `bmds_sys_has(BMDS_SYS_MIDI_CONFIG)`.)
 * @return 0, or `-BMDS_EINVAL` (a flag, size or name not taken), `-BMDS_ENOSPC` (no port left),
 *         `-BMDS_ENOTTY` (not a MIDI fd), `-BMDS_EBADF`.
 */
static inline int32_t bmds_midi_config(int32_t fd, const bmds_midi_config_t *set, bmds_midi_config_t *got) {
        return ((bmds_sys_midi_config_fn)bmds_sys->slot[BMDS_SYS_MIDI_CONFIG])(fd, set, got);
}

/**
 * @brief Fills @p status (its `size` set) with MIDI fd @p fd's port and counters.
 * @return 0, or `-BMDS_ENOTTY`, `-BMDS_EBADF`, `-BMDS_EINVAL` (`size`).
 */
static inline int32_t bmds_midi_status(int32_t fd, bmds_midi_status_t *status) {
        return ((bmds_sys_midi_status_fn)bmds_sys->slot[BMDS_SYS_MIDI_STATUS])(fd, status);
}

/**
 * @brief Does @p op (`BMDS_MIDI_CONNECT`...) on MIDI fd @p fd, with @p value a port's number.
 * @return 0, or `-BMDS_ENOENT` (no such port or connection), `-BMDS_EEXIST` (connected),
 *         `-BMDS_EINVAL` (a side missing; an op not known; no port yet), `-BMDS_ENOSPC` (no
 *         connection left), `-BMDS_ENOTTY`, `-BMDS_EBADF`.
 */
static inline int32_t bmds_midi_control(int32_t fd, uint32_t op, int32_t value) {
        return ((bmds_sys_midi_control_fn)bmds_sys->slot[BMDS_SYS_MIDI_CONTROL])(fd, op, value);
}

/**
 * @brief Opens a General MIDI synth on a SoundFont (@p config, its `size` set): a new fd. Write
 *        `bmds_synth_event_t` records (whole ones) and read whole frames; neither waits. Fonts are
 *        loaded once and shared while any synth plays them; the first open of a font waits for it
 *        to load (TimGM6mb: 0.72 s from the card), and stays loaded after its last synth until memory
 *        runs short (`synth/keep-fonts`). Its output is at `synth/gain` (dB, -10 by default: headroom
 *        for many notes at once).
 *        (Slots 78-79, appended 2026-10-08: check `bmds_sys_has(BMDS_SYS_AUDIO_SYNTH)`.)
 * @return The fd, or `-BMDS_EINVAL` (a rate, channel count, format, voice count or flag),
 *         `-BMDS_ENOENT` (no font there), `-BMDS_ENOMEM`, `-BMDS_EBUSY` (four fonts loaded already),
 *         `-BMDS_EIO` (a damaged font), `-BMDS_EMFILE`.
 */
static inline int32_t bmds_audio_synth(const bmds_synth_config_t *config) {
        return ((bmds_sys_audio_synth_fn)bmds_sys->slot[BMDS_SYS_AUDIO_SYNTH])(config);
}

/**
 * @brief Fills @p info (its `size` set) with synth fd @p fd's state.
 * @return 0, or `-BMDS_ENOTTY`, `-BMDS_EBADF`, `-BMDS_EINVAL` (`size`).
 */
static inline int32_t bmds_audio_synth_info(int32_t fd, bmds_synth_info_t *info) {
        return ((bmds_sys_audio_synth_info_fn)bmds_sys->slot[BMDS_SYS_AUDIO_SYNTH_INFO])(fd, info);
}

/**
 * @brief Fills @p info (its `size` set) with the counter at @p index (0 up, in registration
 *        order; no value: see `bmds_stats_read`). List again from 0 when `generation` changes.
 *        (Slots 80-87, appended 2026-10-09: check `bmds_sys_has(BMDS_SYS_STATS_LIST)`.)
 * @return 0, or `-BMDS_ENOENT` (past the last), `-BMDS_EINVAL` (`size`), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_stats_list(uint32_t index, bmds_counter_t *info) {
        return ((bmds_sys_stats_list_fn)bmds_sys->slot[BMDS_SYS_STATS_LIST])(index, info);
}

/**
 * @brief Reads @p count counters (@p ids) at one moment into @p values (`BMDS_COUNTER_UNAVAILABLE`
 *        for one gone or not measured now); @p health (may be NULL) gets each one's health at the
 *        last sample, or `BMDS_HEALTH_UNJUDGED` while nobody watches; @p time_ns (may be NULL) the
 *        moment (monotonic), for rates.
 * @return @p count, or `-BMDS_EFAULT`, `-BMDS_EINVAL` (more than 1024).
 */
static inline int32_t bmds_stats_read(const uint32_t *ids, uint32_t count, int64_t *values, uint8_t *health,
                                      int64_t *time_ns) {
        return ((bmds_sys_stats_read_fn)bmds_sys->slot[BMDS_SYS_STATS_READ])(ids, count, values, health, time_ns);
}

/**
 * @brief Copies counter @p id's samples (one each interval while anyone watches, the last 120;
 *        only counters flagged `BMDS_COUNTER_HISTORY`) into @p values, the oldest first: a total's
 *        increase each interval, else its value. @p newest_ns (may be NULL) gets the newest one's
 *        time (monotonic).
 * @return How many (0: none kept), or `-BMDS_EFAULT`.
 */
static inline int32_t bmds_stats_history(uint32_t id, int64_t *values, uint32_t max, int64_t *newest_ns) {
        return ((bmds_sys_stats_history_fn)bmds_sys->slot[BMDS_SYS_STATS_HISTORY])(id, values, max, newest_ns);
}

/**
 * @brief Copies the problems changed after change @p after (0: all kept) into @p out, in the
 *        order of their changes, as many as @p max. Set the first entry's `size` (the others are
 *        set to it). Pass the last one's `changed` next time to see new problems and the open
 *        ones' updates.
 * @return How many, or `-BMDS_EFAULT`, `-BMDS_EINVAL` (`size`).
 */
static inline int32_t bmds_stats_problems(uint64_t after, bmds_problem_t *out, uint32_t max) {
        return ((bmds_sys_stats_problems_fn)bmds_sys->slot[BMDS_SYS_STATS_PROBLEMS])(after, out, max);
}

/**
 * @brief `BMDS_STATS_WATCH`: this process watches (@p value 1; once, however often asked) or
 *        stops (0); a watch lasts across `exec` and ends with the process. `BMDS_STATS_SEEN`:
 *        the problems up to change @p value were shown. `BMDS_STATS_CLEAR`: forget the ended ones.
 * @return 0, or `-BMDS_EINVAL`, `-BMDS_ENOMEM` (the sampler couldn't start).
 */
static inline int32_t bmds_stats_control(uint32_t op, uint64_t value) {
        return ((bmds_sys_stats_control_fn)bmds_sys->slot[BMDS_SYS_STATS_CONTROL])(op, value);
}

/**
 * @brief Fills @p status (its `size` set) with the registry's state.
 * @return 0, or `-BMDS_EINVAL` (`size`), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_stats_status(bmds_stats_status_t *status) {
        return ((bmds_sys_stats_status_fn)bmds_sys->slot[BMDS_SYS_STATS_STATUS])(status);
}

/**
 * @brief Fills @p out with the tasks, as many as @p max: those BMDS started, then the system's
 *        (idle, timers, ipc). Set the first entry's `size` (the others are set to it). Listing
 *        scans each stack (interrupts on): a few ms; once a second is fine.
 * @return How many there are (more than @p max: the rest left out), or `-BMDS_EFAULT`,
 *         `-BMDS_EINVAL` (`size`), `-BMDS_ENOMEM`.
 */
static inline int32_t bmds_task_list(bmds_task_info_t *out, uint32_t max) {
        return ((bmds_sys_task_list_fn)bmds_sys->slot[BMDS_SYS_TASK_LIST])(out, max);
}

/**
 * @brief Fills @p info (its `size` set) with process @p pid, or with `BMDS_PROC_INFO_NEXT` the
 *        first after it in pid order (0: the first), zombies included.
 * @return Its pid, or `-BMDS_ESRCH` (none), `-BMDS_EINVAL` (`size`, @p flags), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_proc_info(int32_t pid, uint32_t flags, bmds_proc_info_t *info) {
        return ((bmds_sys_proc_info_fn)bmds_sys->slot[BMDS_SYS_PROC_INFO])(pid, flags, info);
}

/**
 * @brief Fills @p info (its `size` set) with the system's serial number, host name, board,
 *        version and cores. (Slot 88, appended 2026-10-10: check `bmds_sys_has()`.)
 * @return 0, or `-BMDS_EINVAL` (`size`), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_system_info(bmds_system_info_t *info) {
        return ((bmds_sys_system_info_fn)bmds_sys->slot[BMDS_SYS_SYSTEM_INFO])(info);
}

/**
 * @brief Opens an fd of the events of the notifications posted with it: `bmds_notify_event_t`
 *        records, readable and pollable. Closing it stops the events; the notifications stay.
 *        @p flags: `BMDS_O_CLOEXEC`. (Slots 89-91, appended 2026-10-10: check `bmds_sys_has()`.)
 * @return The fd, or `-BMDS_EINVAL`, `-BMDS_ENOMEM`, `-BMDS_EMFILE`, `-BMDS_EBUSY` (too many).
 */
static inline int32_t bmds_notify_open(uint32_t flags) {
        return ((bmds_sys_notify_open_fn)bmds_sys->slot[BMDS_SYS_NOTIFY_OPEN])(flags);
}

/**
 * @brief Posts @p n (its `size` set), its events to @p fd (from `bmds_notify_open`; -1: none);
 *        with `replaces`, updates that one of the caller's.
 * @return Its id, or `-BMDS_EBADF`, `-BMDS_ENOTTY` (@p fd isn't an events fd), `-BMDS_EINVAL`,
 *         `-BMDS_EPERM` (`replaces` someone else's), `-BMDS_ENOMEM`, `-BMDS_EFAULT`.
 */
static inline int32_t bmds_notify_send(int32_t fd, const bmds_notification_t *n) {
        return ((bmds_sys_notify_send_fn)bmds_sys->slot[BMDS_SYS_NOTIFY_SEND])(fd, n);
}

/**
 * @brief Closes notification @p id, one of the caller's (its balloon goes, and it leaves the centre).
 * @return 0, or `-BMDS_ENOENT`, `-BMDS_EPERM`.
 */
static inline int32_t bmds_notify_close(uint32_t id) {
        return ((bmds_sys_notify_close_fn)bmds_sys->slot[BMDS_SYS_NOTIFY_CLOSE])(id);
}

/**
 * @brief Expands the text template @p text (devplan/templates.md) into @p out, @p size bytes
 *        (NUL-terminated if @p size isn't 0): `${HOME}`, `${HOSTNAME}`, `${SERIAL}`,
 *        `${DATE}` and `${TIME}` (local; `${DATE:FORMAT}` as `strftime`), `$$` a `$`; any other
 *        `$` is an error. The system's settings that are templates are expanded the same way.
 *        (Slot 92, appended 2026-10-10: check `bmds_sys_has()`.)
 * @return The expansion's length (as `snprintf`: @p size or more means it was cut), or
 *         `-BMDS_EINVAL` (not a template), `-BMDS_EFAULT`, `-BMDS_ENOMEM`.
 */
static inline int32_t bmds_expand(const char *text, char *out, uint32_t size) {
        return ((bmds_sys_expand_fn)bmds_sys->slot[BMDS_SYS_EXPAND])(text, out, size);
}

#ifdef __cplusplus
}
#endif

#endif /* BMDS_SYS_H */
