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
#include <bmds/errno.h> /* bmds_say's codes (appended 2026-10-12) */

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
#define BMDS_SYS_IMAGE_INFO 93u       /**< `int32_t image_info(fd, data, length, want, info)`. */
#define BMDS_SYS_IMAGE_DECODE 94u     /**< `int32_t image_decode(int32_t fd, const bmds_image_decode_t *d, info)`. */
#define BMDS_SYS_IMAGE_ENCODE 95u     /**< `int32_t image_encode(int32_t fd, const bmds_image_encode_t *e)`. */
#define BMDS_SYS_SOCKET 96u           /**< `int32_t socket(family, type, protocol, flags)`: a new fd. */
#define BMDS_SYS_BIND 97u             /**< `int32_t bind(int32_t fd, const void *addr, uint32_t length)`. */
#define BMDS_SYS_LISTEN 98u           /**< `int32_t listen(int32_t fd, int32_t backlog)`. */
#define BMDS_SYS_ACCEPT 99u           /**< `int32_t accept(fd, addr, length, flags)`: a new fd. */
#define BMDS_SYS_CONNECT 100u         /**< `int32_t connect(fd, addr, length, flags)`. */
#define BMDS_SYS_SOCK_NAME 101u       /**< `int32_t sock_name(fd, which, addr, length)`. */
#define BMDS_SYS_SENDMSG 102u         /**< `int32_t sendmsg(int32_t fd, const bmds_msghdr_t *msg, uint32_t flags)`. */
#define BMDS_SYS_RECVMSG 103u         /**< `int32_t recvmsg(int32_t fd, bmds_msghdr_t *msg, uint32_t flags)`. */
#define BMDS_SYS_SETSOCKOPT 104u      /**< `int32_t setsockopt(fd, level, name, value, length)`. */
#define BMDS_SYS_GETSOCKOPT 105u      /**< `int32_t getsockopt(fd, level, name, value, length)`. */
#define BMDS_SYS_SHUTDOWN 106u        /**< `int32_t shutdown(int32_t fd, uint32_t how)`. */
#define BMDS_SYS_NET_INFO 107u        /**< `int32_t net_info(uint32_t index, bmds_net_info_t *info)`. */
#define BMDS_SYS_PROC_TIMER                                                                                            \
        108u /**< `int32_t proc_timer(uint32_t which, const bmds_timer_t *set, bmds_timer_t *old)`. */
#define BMDS_SYS_NET_WATCH 109u        /**< `int32_t net_watch(uint32_t flags)`: a new fd. */
#define BMDS_SYS_TERM_CTL 110u         /**< `int32_t term_ctl(int32_t fd, uint32_t op, uint32_t arg)`. */
#define BMDS_SYS_FS_INFO 111u          /**< `int32_t fs_info(const char *path, bmds_fs_info_t *info)`. */
#define BMDS_SYS_CRYPTO 112u           /**< `int32_t crypto(const bmds_crypto_op_t *op)`. */
#define BMDS_SYS_SCHED 113u            /**< `int32_t sched(uint32_t op, int32_t tid, bmds_sched_t *param)`. */
#define BMDS_SYS_AUDIO_SPEAK 114u      /**< `int32_t audio_speak(const bmds_speak_config_t *config)`: a new fd. */
#define BMDS_SYS_AUDIO_SPEAK_INFO 115u /**< `int32_t audio_speak_info(int32_t fd, bmds_speak_info_t *info)`. */
#define BMDS_SYS_PTP_CTL 116u          /**< `int32_t ptp_ctl(int32_t fd, uint32_t op, void *arg)`. */
#define BMDS_SYS_SURFACE_OPEN 117u     /**< `int32_t surface_open(const bmds_surface_config_t *config)`: a new fd. */
#define BMDS_SYS_SURFACE_BUFFER 118u   /**< `int32_t surface_buffer(int32_t fd, bmds_surface_buffer_t *info)`. */
#define BMDS_SYS_SURFACE_ACQUIRE 119u  /**< `int32_t surface_acquire(int32_t fd, int32_t timeout_ms)`: an index. */
#define BMDS_SYS_SURFACE_PRESENT 120u  /**< `int32_t surface_present(int32_t fd, const bmds_surface_present_t *p)`. */
#define BMDS_SYS_SURFACE_CONTROL                                                                                       \
        121u /**< `int32_t surface_control(int32_t fd, uint32_t op, void *arg, uint32_t size)`. */
#define BMDS_SYS_CONTROLS 122u /**< `int32_t controls(uint32_t op, void *arg, uint32_t size)`. */
#define BMDS_SYS_FS_SERVE 123u /**< `int32_t fs_serve(const bmds_fs_serve_t *config)`: the server fd. */

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
 * and saved to /cfg only when asked. Values are text in each type's canonical form. A key is
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
/** A string never shown (appended 2026-10-11; `system/password`): its value reads as `(set)` or "". */
#define BMDS_SYSCONF_TYPE_SECRET 7u

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
#define BMDS_POLL_PRI 0x02u  /**< Urgent data: a socket's error queue with `SO_SELECT_ERR_QUEUE`. */
#define BMDS_POLL_OUT 0x04u  /**< A write wouldn't wait. */
#define BMDS_POLL_ERR 0x08u  /**< Writes fail (a pipe without a reader), or an error queue waits (reported unasked). */
#define BMDS_POLL_HUP 0x10u  /**< The other end is gone: a pipe without a writer (reported unasked). */
#define BMDS_POLL_NVAL 0x20u /**< The fd isn't open (reported unasked). */

/**
 * @brief An entry of `io_poll`'s array. The caller sets `size` (the same in
 *        every entry), `fd` and `events`; the kernel sets `revents`.
 */
typedef struct bmds_pollfd_t {
                uint32_t size;    /**< Set by the caller: `sizeof(bmds_pollfd_t)`. */
                int32_t  fd;      /**< The descriptor; a negative one is skipped (`revents` 0). */
                uint32_t events;  /**< `BMDS_POLL_IN`, `BMDS_POLL_OUT`, `BMDS_POLL_PRI`. */
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
                int64_t time_ns; /**< Read: `BMDS_CLOCK_MONOTONIC` when it arrived. Written: 0 (now), or when to send it
                                    (scheduled output: up to `BMDS_MIDI_AHEAD_MAX_NS` ahead, not before the fd's last
                                    scheduled; a time passed sends it now). */
                uint8_t port;    /**< Read: the port whose source produced it (0xff: a write to this port's file). */
                uint8_t length;  /**< The bytes in `data`, 1 to 5. */
                uint8_t flags;   /**< 0. */
                uint8_t data[5]; /**< The message. */
} bmds_midi_event_t;

/** @brief `bmds_midi_status_t::kind`. */
#define BMDS_MIDI_USB 0u     /**< A USB MIDI device's cable. */
#define BMDS_MIDI_SERIAL 1u  /**< A serial MIDI port. */
#define BMDS_MIDI_VIRTUAL 2u /**< A program's (`BMDS_MIDI_CREATE`). */
#define BMDS_MIDI_NET 3u     /**< A network session (RTP-MIDI). */
#define BMDS_MIDI_USB_DEVICE 4u /**< A cable of the board as a USB MIDI device (to a computer). */
#define BMDS_MIDI_SYSTEM 5u     /**< The system's own: `system` (what programs play, `/dev/midi`), `wavetable`. */

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
#define BMDS_MIDI_CANCEL 4u     /**< Throws away what this fd scheduled and isn't sent yet (appended 2026-10-20). */

/**
 * @brief Scheduled output (appended 2026-10-20, `devplan/audio.md` §25): an event written with a
 *        `time_ns` is sent at that time, carrying it as its time, so a synth places it on its
 *        sample. An fd holds this many unsent (past them, `write()` takes what fits, else fails
 *        with `EAGAIN`; `poll()` says writable while one record fits), at most this far ahead.
 */
#define BMDS_MIDI_SCHEDULED_MAX 512u
#define BMDS_MIDI_AHEAD_MAX_NS 60000000000ll

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

/** @brief `bmds_image_info_t::codec`: a picture's file format (devplan/images.md §3f; appended 2026-10-10). */
#define BMDS_IMAGE_PNG 0u
#define BMDS_IMAGE_JPEG 1u
#define BMDS_IMAGE_BMP 2u

/** @brief Pixel formats, named by their bytes in memory. */
#define BMDS_PIXEL_BGR24 0u  /**< Bytes B, G, R (the screen's). */
#define BMDS_PIXEL_RGB24 1u  /**< Bytes R, G, B. */
#define BMDS_PIXEL_BGRA32 2u /**< Bytes B, G, R, A: 0xAARRGGBB as a little-endian word. */
#define BMDS_PIXEL_RGB565 3u /**< A little-endian 16-bit word, red in the top 5 bits. */
#define BMDS_PIXEL_GRAY8 4u  /**< One byte of luma. */

/** @brief `bmds_image_info_t::flags`. */
#define BMDS_IMAGE_ALPHA 0x1u       /**< Some pixels may be transparent. */
#define BMDS_IMAGE_PROGRESSIVE 0x2u /**< A progressive JPEG (not decoded yet), an interlaced PNG. */
#define BMDS_IMAGE_HARDWARE 0x4u    /**< The JPEG codec can decode it. */

/** @brief `bmds_image_info_t::decoder`: what `image_decode` would use. */
#define BMDS_IMAGE_DECODER_PNG 0u
#define BMDS_IMAGE_DECODER_BMP 1u
#define BMDS_IMAGE_DECODER_HARDWARE 2u /**< The JPEG codec. */
#define BMDS_IMAGE_DECODER_SOFTWARE 3u /**< TJpgDec (baseline JPEG, maybe reduced). */

/** @brief `bmds_image_decode_t::flags`. */
#define BMDS_IMAGE_ORIENT 0x1u      /**< Undo the EXIF orientation: the picture comes upright. */
#define BMDS_IMAGE_NO_HARDWARE 0x2u /**< Don't use the JPEG codec. */

/** @brief `bmds_image_encode_t::sampling`: a JPEG's chroma. */
#define BMDS_JPEG_YUV420 0u
#define BMDS_JPEG_YUV422 1u
#define BMDS_JPEG_YUV444 2u
#define BMDS_JPEG_GRAY 3u

/** @brief `bmds_image_encode_t::level`: 0 the default (2), 1-9 that level, this one stored. */
#define BMDS_PNG_STORED 10u

/**
 * @brief How to decode a picture (`image_decode`), or what for (`image_info`). Zeroed but for
 *        `size`, it asks for the whole picture as `BMDS_PIXEL_BGR24`.
 */
typedef struct bmds_image_decode_t {
                uint32_t    size;       /**< Set by the caller: `sizeof(bmds_image_decode_t)`. */
                uint32_t    format;     /**< `BMDS_PIXEL_*`. */
                uint32_t    flags;      /**< `BMDS_IMAGE_ORIENT`, `BMDS_IMAGE_NO_HARDWARE`. */
                uint32_t    background; /**< 0xRRGGBB: alpha is blended over it into formats without. */
                uint32_t    min_width;  /**< The smallest wanted (a JPEG may come reduced); 0: whole. */
                uint32_t    min_height;
                uint32_t    box_width; /**< Shown fitted in this box: no smaller than that fit is wanted. */
                uint32_t    box_height;
                void       *out;      /**< `image_decode`: the pixels' memory. */
                uint32_t    out_size; /**< Its bytes. */
                uint32_t    stride;   /**< Bytes a row; 0: packed. */
                const void *data;     /**< With fd -1: the file in memory. */
                uint32_t    length;   /**< Its bytes. */
} bmds_image_decode_t;

/** @brief What a picture is, and what decoding it as asked gives (`image_info`, `image_decode`). */
typedef struct bmds_image_info_t {
                uint32_t size;  /**< Set by the caller: `sizeof(bmds_image_info_t)`. */
                uint32_t codec; /**< `BMDS_IMAGE_*`. */
                uint32_t width; /**< The file's picture. */
                uint32_t height;
                uint32_t flags;        /**< `BMDS_IMAGE_ALPHA`, `BMDS_IMAGE_PROGRESSIVE`, `BMDS_IMAGE_HARDWARE`. */
                uint32_t orientation;  /**< EXIF's, 1-8 (1: upright). */
                uint32_t decode_width; /**< What decoding as asked gives (reduced, turned). */
                uint32_t decode_height;
                uint32_t decoder; /**< `BMDS_IMAGE_DECODER_*`. */
                uint32_t shift;   /**< A JPEG reduced by 1/2^shift. */
                uint32_t align;   /**< A no-copy buffer: its address and size aligned to this (0: none). */
                uint32_t stride;  /**< ... its bytes a row, */
                uint32_t bytes;   /**< ... and its size. */
} bmds_image_info_t;

/** @brief A picture to write (`image_encode`). */
typedef struct bmds_image_encode_t {
                uint32_t    size;   /**< Set by the caller: `sizeof(bmds_image_encode_t)`. */
                uint32_t    codec;  /**< `BMDS_IMAGE_PNG` or `BMDS_IMAGE_JPEG`. */
                uint32_t    format; /**< `BMDS_PIXEL_*` (alpha is dropped). */
                uint32_t    width;
                uint32_t    height;
                uint32_t    stride; /**< Bytes a row; 0: packed. */
                const void *pixels;
                uint32_t    quality;  /**< JPEG: 1-100; 0: 90. */
                uint32_t    sampling; /**< JPEG: `BMDS_JPEG_*`. */
                uint32_t    level;    /**< PNG: 0 the default, 1-9, `BMDS_PNG_STORED`. */
} bmds_image_encode_t;

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
                uint64_t file_id; /**< The same for each name and fd of one file, different for others; 0 if unknown. */
                uint32_t device;  /**< The filesystem it is on (non-zero), or 0 (devices, pipes, `/`, `/dev`). */
                uint32_t reserved; /**< Zero. */
} bmds_stat_t;

/** @brief `clock_get` clocks. */
#define BMDS_CLOCK_REALTIME 0u  /**< Wall time, ns since 1970-01-01 UTC (may jump when set). */
#define BMDS_CLOCK_MONOTONIC 1u /**< ns since boot, never goes back. */
/**
 * TAI: `BMDS_CLOCK_REALTIME` plus TAI less UTC (a PTP master's, else the leap-second list's,
 * else 0). From 2026-10-13; an older kernel answers `BMDS_EINVAL`.
 */
#define BMDS_CLOCK_TAI 2u

/** @brief Terminal modes (`term_get`, `term_set`); a program starts with all set. */
#define BMDS_TERM_SIGNALS 0x01u /**< Ctrl-C sends `SIGINT` to the job instead of being read. */
#define BMDS_TERM_ONLCR 0x02u   /**< Output `\n` is sent as CR LF where the terminal needs it. */
#define BMDS_TERM_COLORS 0x04u  /**< Output colors are fitted to what the terminal shows. */
#define BMDS_TERM_LOGS 0x08u    /**< The kernel's log lines are shown. */
#define BMDS_TERM_LINE 0x10u    /**< Input is read in lines, editable (else raw bytes). */
#define BMDS_TERM_ECHO 0x20u    /**< Input is echoed (line mode: the editor shows it; raw: the bytes read). */
#define BMDS_TERM_ICRNL 0x40u   /**< Raw input: CR is read as LF. */

/**
 * @brief `term_ctl` operations (appended 2026-10-11, slot 110; devplan/network.md §24): the
 *        pseudo-terminals programs open (`/dev/ptmx` opens a pair: its descriptor is the
 *        emulator's end; `/dev/pts/N` is the program's), and the controlling terminal.
 */
#define BMDS_TERM_PTY_NUMBER 0u /**< @p fd a pair's emulator end: returns N (`/dev/pts/N`). */
#define BMDS_TERM_PTY_UNLOCK 1u /**< @p fd a pair's emulator end: unlocks it (pairs never are locked). */
#define BMDS_TERM_SET_SIZE 2u   /**< @p arg `columns | rows << 16`: either end's size (`SIGWINCH`). */
#define BMDS_TERM_CONTROL 3u    /**< A session leader without one: @p fd's terminal becomes its terminal. */
#define BMDS_TERM_RELEASE 4u    /**< The caller lets go of its terminal (a leader: the session too). */
#define BMDS_TERM_PTS_NUMBER 5u /**< @p fd a pair's program end: returns N (its name: `/dev/pts/N`). */

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

/* --- Sockets (devplan/network.md §18) ------------------------------- */

/*
 * Addresses are Linux's `sockaddr` layouts with their length, so musl's
 * structures go to the kernel as they are; option levels and names, and the
 * `BMDS_MSG_*` flags, are Linux's numbers. IPv4 only for now.
 */

/** @brief Address families. */
#define BMDS_AF_UNSPEC 0u /**< `connect` with it forgets a datagram socket's peer. */
#define BMDS_AF_INET 2u   /**< IPv4: `bmds_sockaddr_in_t`. */

/** @brief Socket types. */
#define BMDS_SOCK_STREAM 1u /**< TCP. */
#define BMDS_SOCK_DGRAM 2u  /**< UDP. */
#define BMDS_SOCK_RAW 3u    /**< Raw IP of one protocol (ICMP, 1): received with the IP header. */

/** @brief `socket` and `accept` flags (the new fd's). */
#define BMDS_SOCK_CLOEXEC BMDS_O_CLOEXEC /**< Closed by `bmds_proc_exec`. */

/** @brief `accept` and `connect` flags. */
#define BMDS_SOCK_DONTWAIT 0x1u /**< Don't wait: `-BMDS_EAGAIN`, or `-BMDS_EINPROGRESS` for a connect. */

/** @brief `sendmsg` and `recvmsg` flags, and `bmds_msghdr_t::flags` (Linux's values). */
#define BMDS_MSG_PEEK 0x2u        /**< Receive without taking. */
#define BMDS_MSG_CTRUNC 0x8u      /**< Out: the control messages were cut. */
#define BMDS_MSG_TRUNC 0x20u      /**< In: return a datagram's whole length; out: it was cut. */
#define BMDS_MSG_DONTWAIT 0x40u   /**< Don't wait: `-BMDS_EAGAIN`. */
#define BMDS_MSG_WAITALL 0x100u   /**< TCP: wait until the buffers are full. */
#define BMDS_MSG_NOSIGNAL 0x4000u /**< No `SIGPIPE` when the connection is gone (`-BMDS_EPIPE` still). */
#define BMDS_MSG_MORE 0x8000u     /**< TCP: more follows; don't push yet. */
#define BMDS_MSG_ERRQUEUE 0x2000u /**< Receive from the error queue (send stamps); out: it was. */

/**
 * @brief BMDS's own socket option level (`setsockopt`, `getsockopt`): "BMDS", so no Linux
 *        level can be the same.
 */
#define BMDS_SOL_BMDS 0x424d4453

/**
 * @brief `BMDS_SOL_BMDS` option: a UDP socket's fast path (devplan/network.md §30f). Its
 *        datagrams bypass the IP stack both ways, ahead of bulk traffic; what the fast path
 *        can't send (larger than a frame, or to an address ARP hasn't answered yet) goes the usual
 *        way. The value is an `int` (0 off; otherwise on, with a 1 ms packet time and a depth of 4)
 *        or a `bmds_fastpath_t`. `-BMDS_ENOBUFS` when the real-time flows or their buffers are
 *        taken, `-BMDS_EADDRINUSE` when another fast path has the port, `-BMDS_ENODEV` without
 *        Ethernet, `-BMDS_ENOPROTOOPT` for a socket that isn't UDP.
 */
#define BMDS_SO_FASTPATH 1

/**
 * @brief `BMDS_SO_FASTPATH`'s value in full: set, the fast path on with these; read, its
 *        settings (`depth` 0: off).
 */
typedef struct bmds_fastpath_t {
                uint32_t size;      /**< Set by the caller: `sizeof(bmds_fastpath_t)`. */
                uint32_t packet_us; /**< The shortest time between its packets either way (µs). */
                uint32_t depth;     /**< Datagrams it holds unread, 1-8 (all fast paths' together at most 8). */
} bmds_fastpath_t;

/** @brief `shutdown` sides. */
#define BMDS_SHUT_RD 0u   /**< Receiving: reads end. */
#define BMDS_SHUT_WR 1u   /**< Sending: TCP sends its FIN. */
#define BMDS_SHUT_RDWR 2u /**< Both. */

/** @brief `sock_name`: which address. */
#define BMDS_SOCK_NAME_LOCAL 0u /**< The socket's own (`getsockname`). */
#define BMDS_SOCK_NAME_PEER 1u  /**< The peer's (`getpeername`). */

/** @brief An IPv4 address and port: Linux's `struct sockaddr_in`. */
typedef struct bmds_sockaddr_in_t {
                uint16_t family;  /**< `BMDS_AF_INET`. */
                uint16_t port;    /**< In network order. */
                uint32_t addr;    /**< In network order. */
                uint8_t  zero[8]; /**< Zero. */
} bmds_sockaddr_in_t;

/** @brief A piece of a message: Linux's `struct iovec` (32-bit). */
typedef struct bmds_iovec_t {
                void    *base;   /**< Where it is. */
                uint32_t length; /**< Its bytes. */
} bmds_iovec_t;

/**
 * @brief A message for `sendmsg` and `recvmsg`: Linux's `struct msghdr` with
 *        `size` first. The caller sets `size`; `recvmsg` writes back
 *        `name_length`, `control_length` and `flags`.
 */
typedef struct bmds_msghdr_t {
                uint32_t            size;           /**< Set by the caller: `sizeof(bmds_msghdr_t)`. */
                void               *name;           /**< The address to send to or received from, or NULL. */
                uint32_t            name_length;    /**< Its bytes (in and out). */
                const bmds_iovec_t *iov;            /**< The pieces. */
                uint32_t            iov_count;      /**< How many (at most `BMDS_IOV_MAX`). */
                void               *control;        /**< Control messages (Linux's `cmsghdr`s), or NULL. */
                uint32_t            control_length; /**< Their bytes (in and out). */
                uint32_t            flags;          /**< Out: `BMDS_MSG_TRUNC`, `BMDS_MSG_CTRUNC`. */
} bmds_msghdr_t;

/** @brief The most pieces one message has. */
#define BMDS_IOV_MAX 64u

/** @brief `bmds_net_info_t::flags`. */
#define BMDS_NET_UP 0x1u          /**< Enabled. */
#define BMDS_NET_LINK 0x2u        /**< The link is up (cable in). */
#define BMDS_NET_FULL_DUPLEX 0x4u /**< The link is full duplex. */
#define BMDS_NET_LOOPBACK 0x8u    /**< The loopback interface (`lo`). */
#define BMDS_NET_BROADCAST 0x10u  /**< Has a broadcast address. */
#define BMDS_NET_MULTICAST 0x20u  /**< Joins multicast groups. */

/** @brief `bmds_net_info_t::mode`: how the IPv4 address is found. */
#define BMDS_NET_MODE_DHCP 0u   /**< DHCP. */
#define BMDS_NET_MODE_STATIC 1u /**< Set (`net/ethN/ipv4/address`). */
#define BMDS_NET_MODE_OFF 2u    /**< None. */

/** @brief `bmds_net_info_t::dhcp`: DHCP's state. */
#define BMDS_NET_DHCP_NONE 0u   /**< Not used. */
#define BMDS_NET_DHCP_ASKING 1u /**< Asking (no lease yet, or renewing one lost). */
#define BMDS_NET_DHCP_BOUND 2u  /**< A lease is held. */

/**
 * @brief One network interface's state (`net_info`). The caller sets `size`;
 *        the kernel fills the fields that fit in it. Addresses are IPv4 in
 *        network order; 0 is none.
 */
typedef struct bmds_net_info_t {
                uint32_t size;         /**< Set by the caller: `sizeof(bmds_net_info_t)`. */
                char     name[16];     /**< "eth0", "lo". */
                uint32_t index;        /**< Its interface index (`if_nametoindex`). */
                uint32_t flags;        /**< `BMDS_NET_*`. */
                uint32_t mtu;          /**< Bytes. */
                uint32_t speed_mbps;   /**< The link's speed; 0 with no link. */
                uint8_t  mac[6];       /**< Its hardware address (zero for `lo`). */
                uint8_t  mode;         /**< `BMDS_NET_MODE_*`. */
                uint8_t  dhcp;         /**< `BMDS_NET_DHCP_*`. */
                uint32_t address;      /**< Its IPv4 address. */
                uint32_t netmask;      /**< Its netmask. */
                uint32_t gateway;      /**< Its gateway. */
                uint32_t dhcp_server;  /**< The DHCP server's address. */
                uint32_t lease_s;      /**< The lease's length in seconds. */
                uint32_t lease_left_s; /**< Seconds left of it. */
                uint32_t dns[2];       /**< The DNS servers DHCP gave. */
                uint32_t reserved;     /**< Zero. */
                uint64_t rx_bytes;     /**< Received since boot. */
                uint64_t tx_bytes;     /**< Sent since boot. */
                uint32_t rx_frames;    /**< Frames received. */
                uint32_t tx_frames;    /**< Frames sent. */
                uint32_t rx_errors;    /**< Frames received damaged. */
                uint32_t tx_errors;    /**< Frames that failed to go. */
                uint32_t rx_dropped;   /**< Frames dropped (no buffer). */
                uint32_t tx_dropped;   /**< Frames dropped (the device was busy). */
} bmds_net_info_t;

/**
 * @brief The filesystem a path is on (`fs_info`, appended 2026-10-11, slot 111): what `statfs`
 *        and `df` show. The caller sets `size`; the kernel fills the fields that fit in it.
 */
typedef struct bmds_fs_info_t {
                uint32_t size;        /**< Set by the caller: `sizeof(bmds_fs_info_t)`. */
                uint32_t block_bytes; /**< Its allocation unit (FAT's cluster, littlefs' block). */
                uint64_t total_bytes; /**< Its data area. */
                uint64_t free_bytes;  /**< Free now. */
                char     type[16];    /**< "FAT12", "FAT16", "FAT32", "exFAT", "littlefs". */
} bmds_fs_info_t;

/** @brief `crypto`'s algorithms (appended 2026-10-11, slot 112; devplan/network.md §26). */
#define BMDS_CRYPTO_AES_ECB_ENCRYPT 1u /**< AES, each block alone. */
#define BMDS_CRYPTO_AES_ECB_DECRYPT 2u /**< AES, each block alone, deciphered. */
#define BMDS_CRYPTO_AES_CBC_ENCRYPT 3u /**< AES-CBC; `state`: the IV, then the last block out. */
#define BMDS_CRYPTO_AES_CBC_DECRYPT 4u /**< AES-CBC deciphered; `state`: the IV, then the last block in. */
#define BMDS_CRYPTO_AES_CTR 5u         /**< AES-CTR; `state`: a 128-bit big-endian counter. */
#define BMDS_CRYPTO_SHA1 16u           /**< SHA-1's compression of whole 64-byte blocks. */
#define BMDS_CRYPTO_SHA256 17u         /**< SHA-256's (so SHA-224's) of whole 64-byte blocks. */
#define BMDS_CRYPTO_SHA512 18u         /**< SHA-512's (so SHA-384's) of whole 128-byte blocks. */

/**
 * @brief One `crypto` call (appended 2026-10-11, slot 112): the chip's AES or SHA engine over
 *        whole blocks. Every call carries its key and state; the kernel keeps neither. The
 *        caller sets `size`.
 */
typedef struct bmds_crypto_op_t {
                uint32_t    size;      /**< Set by the caller: `sizeof(bmds_crypto_op_t)`. */
                uint32_t    algorithm; /**< A `BMDS_CRYPTO_...` algorithm. */
                const void *key;       /**< AES: the key. SHA: unused. */
                uint32_t    key_bytes; /**< AES: 16 or 32 (the engine has no AES-192). */
                void       *state;     /**< AES-CBC: the IV; AES-CTR: the first block's counter;
                                            SHA: the chaining value as the digest is written (20, 32
                                            or 64 bytes, the words big-endian). Replaced by the
                                            value for the next call; AES-ECB: unused. */
                const void *in;        /**< The input: `length` bytes. */
                void       *out;       /**< AES: the output, `length` bytes (may be `in`). SHA: unused. */
                uint32_t    length;    /**< Bytes: a multiple of the block (16; SHA 64, SHA-512 128). */
} bmds_crypto_op_t;

/** @brief Scheduling policies (`sched`, appended 2026-10-12, slot 113; Linux's numbers). */
#define BMDS_SCHED_OTHER 0u /**< The normal priority, time-shared. */
#define BMDS_SCHED_FIFO 1u  /**< Real time at `priority` 1-99. */
#define BMDS_SCHED_RR 2u    /**< Real time at `priority` 1-99, round robin (as `FIFO`: see `sched`). */
#define BMDS_SCHED_BATCH 3u /**< As `OTHER`. */
#define BMDS_SCHED_IDLE 5u  /**< As `OTHER`. */
/** @brief Or'ed into a policy: threads and processes it starts begin as `OTHER`. */
#define BMDS_SCHED_RESET_ON_FORK 0x40000000u

/** @brief Real-time priorities (`FIFO`, `RR`); 0 for the others. */
#define BMDS_SCHED_PRIORITY_MIN 1u
#define BMDS_SCHED_PRIORITY_MAX 99u

/** @brief `sched` operations. */
#define BMDS_SCHED_GET 0u   /**< Fills `param` in. */
#define BMDS_SCHED_SET 1u   /**< Sets `policy` (with `BMDS_SCHED_RESET_ON_FORK`) and `priority`. */
#define BMDS_SCHED_YIELD 2u /**< Lets threads of its priority run (`tid` 0, `param` NULL). */
/**
 * @brief Keeps the calling thread (`tid` 0 or its own) on the cores in `param->cores` (appended
 *        2026-10-15): one core, or all of them. A thread is pinned to one core for good the first
 *        time it uses floating point (or the vector unit); until then it may choose (and is moved
 *        there), after only its own core succeeds (`BMDS_EINVAL` otherwise). All the cores: a
 *        no-op, where it may still run anywhere. `sched_setaffinity` and `pthread_setaffinity_np`.
 */
#define BMDS_SCHED_PIN 3u

/**
 * @brief A thread's scheduling (`sched`, appended 2026-10-12, slot 113). The caller sets
 *        `size`; `BMDS_SCHED_SET` reads `policy` and `priority`, `BMDS_SCHED_GET` fills the
 *        fields that fit in it.
 */
typedef struct bmds_sched_t {
                uint32_t size;            /**< Set by the caller: `sizeof(bmds_sched_t)`. */
                uint32_t policy;          /**< `BMDS_SCHED_...`, with `BMDS_SCHED_RESET_ON_FORK`. */
                uint32_t priority;        /**< 1-99 for `FIFO` and `RR`, else 0. */
                uint32_t cores;           /**< The cores it may run on, a bit each. */
                int32_t  core;            /**< The core it runs on (or last ran on). */
                uint32_t kernel_priority; /**< The kernel priority it runs at now. */
                uint32_t throttled;       /**< Times its real-time priority was held back (95 %), in all. */
                uint32_t flags;           /**< 0. */
} bmds_sched_t;

/** @brief `bmds_speak_config_t::flags` (appended 2026-10-12, slot 114): the text's markup obeyed. */
#define BMDS_SPEAK_MARKUP 0x1000u

/** @brief The longest voice name, its NUL included (`bmds_speak_info_t::voice`). */
#define BMDS_SPEAK_VOICE_MAX 32u

/**
 * @brief Text to speech (`audio_speak`, appended 2026-10-12, slots 114-115, `devplan/speech.md`):
 *        UTF-8 text written to the fd, samples read from it. Speed and pitch are percent of the
 *        voice's, volume of the loudest level that doesn't clip (over about 115, peaks can); Pico's markup in the text (`<speed level='200'>`,
 *        `<break time='300ms'/>`, `<spell>`...) is read as text unless `BMDS_SPEAK_MARKUP`.
 */
typedef struct bmds_speak_config_t {
                uint32_t    size;   /**< Set by the caller: `sizeof(bmds_speak_config_t)`. */
                uint32_t    flags;  /**< `BMDS_O_CLOEXEC`, `BMDS_SPEAK_MARKUP`, or 0. */
                const char *voice;  /**< A voice's name (`"en-US"`; `say -v ?` lists them); NULL: `speech/voice`. */
                uint32_t    speed;  /**< 20 to 500 (0: 100). */
                uint32_t    pitch;  /**< 50 to 200 (0: 100). */
                uint32_t    volume; /**< 1 to 500 (0: 100: as loud as it gets without clipping). */
} bmds_speak_config_t;

/** @brief A speech fd's state (`audio_speak_info`). */
typedef struct bmds_speak_info_t {
                uint32_t size;                        /**< Set by the caller: `sizeof(bmds_speak_info_t)`. */
                uint32_t rate;                        /**< Its samples' rate: 16000. */
                uint32_t channels;                    /**< 1. */
                uint32_t format;                      /**< `BMDS_AUDIO_S16LE`. */
                uint64_t frames;                      /**< Samples read so far. */
                uint32_t speed;                       /**< As configured (100 for 0). */
                uint32_t pitch;                       /**< As configured. */
                uint32_t volume;                      /**< As configured. */
                uint32_t flags;                       /**< `BMDS_SPEAK_MARKUP` if configured. */
                char     voice[BMDS_SPEAK_VOICE_MAX]; /**< The voice's name. */
} bmds_speak_info_t;

/** @brief `ptp_ctl` operations on a PTP clock's fd (`/dev/ptp0`: an Ethernet device's 1588 clock). */
#define BMDS_PTP_TIME 0u     /**< `int64_t *`: reads it, ns since 1970 on PTP's timescale (TAI). */
#define BMDS_PTP_CAPS 1u     /**< `bmds_ptp_caps_t *`: what it can do. */
#define BMDS_PTP_OFFSET 2u   /**< `bmds_ptp_offset_t *`: readings of it between two of a system clock. */
#define BMDS_PTP_SET 3u      /**< `const int64_t *`: sets it. */
#define BMDS_PTP_STEP 4u     /**< `const int64_t *`: adds to it (negative: takes away) in one update. */
#define BMDS_PTP_GET_FREQ 5u /**< `int64_t *`: its frequency correction, parts per trillion. */
#define BMDS_PTP_SET_FREQ 6u /**< `const int64_t *`: sets it (the clock rounds to its own step). */

/** @brief `bmds_ptp_caps_t::flags`. */
#define BMDS_PTP_CAP_PPS 0x1u /**< A pulse on `pps_pin` rises on each of its seconds. */

/** @brief A PTP clock's capabilities (`BMDS_PTP_CAPS`). */
typedef struct bmds_ptp_caps_t {
                uint32_t size;        /**< Set by the caller: `sizeof(bmds_ptp_caps_t)`. */
                uint32_t max_adj_ppb; /**< The largest frequency correction taken, parts per billion. */
                uint32_t flags;       /**< `BMDS_PTP_CAP_...`. */
                int32_t  pps_pin;     /**< The PPS output's GPIO, or -1. */
                uint32_t index;       /**< N of `/dev/ptpN`. */
                uint32_t reserved;    /**< Zero. */
} bmds_ptp_caps_t;

/** @brief The most readings `BMDS_PTP_OFFSET` takes at once (Linux's `PTP_MAX_SAMPLES`). */
#define BMDS_PTP_MAX_SAMPLES 25u

/** @brief Readings of a PTP clock between two of a system clock (`BMDS_PTP_OFFSET`). */
typedef struct bmds_ptp_offset_t {
                uint32_t size;     /**< Set by the caller: `sizeof(bmds_ptp_offset_t)`. */
                uint32_t clock;    /**< The system clock read: `BMDS_CLOCK_REALTIME`, `_MONOTONIC` or `_TAI`. */
                uint32_t count;    /**< In: readings wanted, 1 to `BMDS_PTP_MAX_SAMPLES`. Out: taken. */
                uint32_t reserved; /**< Zero. */
                int64_t samples[BMDS_PTP_MAX_SAMPLES][3]; /**< ns: the system clock, the PTP clock, the system clock. */
} bmds_ptp_offset_t;

/*
 * Surfaces (slots 117-121, appended 2026-10-14, devplan/games.md §11): a picture a program draws
 * with the CPU into its own buffers and presents, in a window or full screen (the user's setting
 * per application, the program's suggestion its default, Alt+Enter between them); its events read
 * from its fd.
 */

/**
 * @brief A palette's 8-bit index (appended to the `BMDS_PIXEL_*` formats 2026-10-14): a surface's
 *        `bmds_surface_config_t::format` may be it, `BMDS_PIXEL_RGB565` or `BMDS_PIXEL_BGR24`.
 */
#define BMDS_PIXEL_L8 5u

/** @brief `bmds_surface_config_t::flags`. */
#define BMDS_SURFACE_FULL_SCREEN 0x1u /**< Suggest full screen (else a window); the user's setting wins. */
#define BMDS_SURFACE_WHOLE 0x2u       /**< Scale by whole multiples only, both ways (sharp pixels). */
#define BMDS_SURFACE_FAST 0x4u        /**< Buffers in internal RAM (small ones only; else `BMDS_ENOMEM`). */
/**
 * @brief Open in a maximized window (appended 2026-10-15, devplan/luckyduck.md §15). With `width`
 *        and `height` both 0 the surface is the maximized window's client area at scale 1 (at
 *        most 1280×720, its width down to a multiple of 4): `BMDS_SURFACE_INFO`'s `picture_width`
 *        and `picture_height` say what it got. A size given is kept (scaled as any window's).
 */
#define BMDS_SURFACE_MAXIMIZED 0x8u

/** @brief A surface to open (`surface_open`). */
typedef struct bmds_surface_config_t {
                uint32_t    size;     /**< Set by the caller: `sizeof(bmds_surface_config_t)`. */
                uint32_t    width;    /**< Pixels: a multiple of 4, at most 1280 (0: see `BMDS_SURFACE_MAXIMIZED`). */
                uint32_t    height;   /**< Pixels: at most 720 (0: see `BMDS_SURFACE_MAXIMIZED`). */
                uint32_t    format;   /**< `BMDS_PIXEL_L8`, `BMDS_PIXEL_RGB565` or `BMDS_PIXEL_BGR24`. */
                uint32_t    buffers;  /**< 2 or 3. */
                uint32_t    flags;    /**< `BMDS_SURFACE_...`, or 0. */
                uint32_t    aspect_x; /**< The picture's shape on the screen, e.g. 4 and 3 for 320x200 shown */
                uint32_t    aspect_y; /**< as 4:3; both 0: square pixels. */
                const char *title;    /**< The window's title (copied), or NULL: the program's name. */
} bmds_surface_config_t;

/** @brief Where a buffer is (`surface_buffer`). */
typedef struct bmds_surface_buffer_t {
                uint32_t size;   /**< Set by the caller: `sizeof(bmds_surface_buffer_t)`. */
                uint32_t index;  /**< Set by the caller: which buffer, 0 up. */
                void    *pixels; /**< Its first pixel, the program's to write while acquired. */
                uint32_t stride; /**< Bytes from a row to the next. */
} bmds_surface_buffer_t;

/** @brief A rectangle of a surface, in its pixels. */
typedef struct bmds_rect_t {
                int32_t  x;
                int32_t  y;
                uint32_t width;
                uint32_t height;
} bmds_rect_t;

/** @brief A buffer to show (`surface_present`). */
typedef struct bmds_surface_present_t {
                uint32_t           size;         /**< Set by the caller: `sizeof(bmds_surface_present_t)`. */
                uint32_t           index;        /**< The buffer, acquired and drawn. */
                const bmds_rect_t *damage;       /**< What differs from the last present (any buffer's), */
                uint32_t           damage_count; /**< `damage_count` rectangles; 0: all of it. */
} bmds_surface_present_t;

/** @brief `surface_control`'s operations. */
#define BMDS_SURFACE_PALETTE 1u /**< `const bmds_surface_palette_t *`: L8's palette, from the next present. */
#define BMDS_SURFACE_TITLE 2u   /**< `const char *` (`size` its bytes, no NUL needed): the window's title. */
#define BMDS_SURFACE_POINTER 3u /**< `const uint32_t *`: `BMDS_POINTER_...` over the surface. */
#define BMDS_SURFACE_WAIT 4u    /**< `const bmds_surface_wait_t *`: waits until a frame has begun. */
#define BMDS_SURFACE_INFO 5u    /**< `bmds_surface_info_t *`: its state. */
#define BMDS_SURFACE_IMAGE 6u /**< `const bmds_surface_image_t *`: a picture BLIT copies from (appended 2026-10-16). */
#define BMDS_SURFACE_BLIT 7u /**< `const bmds_surface_blit_t *`: copies or fills by the 2D-DMA (appended 2026-10-16). */

/** @brief `BMDS_SURFACE_POINTER`'s modes. */
#define BMDS_POINTER_NORMAL 0u   /**< The system's arrow. */
#define BMDS_POINTER_HIDDEN 1u   /**< None over the surface. */
#define BMDS_POINTER_RELATIVE 2u /**< None, held still while focused: motion comes as deltas (mouse look). */

/** @brief `BMDS_SURFACE_PALETTE`'s argument. */
typedef struct bmds_surface_palette_t {
                uint32_t        size;  /**< `sizeof(bmds_surface_palette_t)`. */
                uint32_t        first; /**< The first entry set, 0-255. */
                uint32_t        count; /**< Entries set (`first + count` at most 256). */
                const uint32_t *rgb;   /**< 0xRRGGBB each. */
} bmds_surface_palette_t;

/** @brief `BMDS_SURFACE_WAIT`'s argument. */
typedef struct bmds_surface_wait_t {
                uint32_t size;       /**< `sizeof(bmds_surface_wait_t)`. */
                uint32_t frame;      /**< The frame waited for (`surface_present`'s result, `frames`). */
                int32_t  timeout_ms; /**< Negative: forever; 0: only checks (`BMDS_EAGAIN`). */
} bmds_surface_wait_t;

/** @brief `BMDS_SURFACE_INFO`'s argument: the surface's state. */
typedef struct bmds_surface_info_t {
                uint32_t size;        /**< Set by the caller: `sizeof(bmds_surface_info_t)`. */
                uint32_t full_screen; /**< 1 full screen, 0 a window. */
                uint32_t active;      /**< 1 while it has the keys. */
                uint32_t shown;       /**< 1 while it has a window or the screen (0: closed). */
                uint32_t scale_x16;   /**< Its scale across, in 16ths (32: twice). */
                uint32_t scale_y16;   /**< Down. */
                int32_t  x;           /**< The picture on the screen, in the screen's pixels. */
                int32_t  y;
                uint32_t width;
                uint32_t height;
                uint32_t frames;   /**< Frames the display has begun. */
                uint32_t presents; /**< Presents so far. */
                uint32_t dropped;  /**< Presents replaced before they were shown. */
                uint32_t picture_width; /**< The surface's own size, its buffers' (appended 2026-10-15). */
                uint32_t picture_height;
} bmds_surface_info_t;

/** @brief Image slots a surface has (`BMDS_SURFACE_IMAGE`). */
#define BMDS_SURFACE_IMAGES 4u
/** @brief Pieces a `BMDS_SURFACE_BLIT` takes at most. */
#define BMDS_SURFACE_BLIT_MAX 64u

/**
 * @brief `BMDS_SURFACE_IMAGE`'s argument (appended 2026-10-16, `devplan/games.md` §25): a picture
 *        of the program's, in the surface's format, that `BMDS_SURFACE_BLIT` may copy from. It is
 *        checked and written back from the CPU's cache once, here: change it, and register it
 *        again. Free it only after forgetting it (`pixels` NULL).
 */
typedef struct bmds_surface_image_t {
                uint32_t    size;   /**< `sizeof(bmds_surface_image_t)`. */
                uint32_t    slot;   /**< 0 to `BMDS_SURFACE_IMAGES - 1`. */
                const void *pixels; /**< Inside one allocation of `bmds_mem_alloc()`; NULL: the slot forgotten. */
                uint32_t    width;  /**< Pixels. */
                uint32_t    height;
                uint32_t    stride; /**< Bytes a row (at least `width` pixels). */
} bmds_surface_image_t;

/** @brief One rectangle of a `BMDS_SURFACE_BLIT`. */
typedef struct bmds_surface_piece_t {
                int32_t  x; /**< Where in the buffer written. */
                int32_t  y;
                uint32_t width; /**< Its size. */
                uint32_t height;
                int32_t  from_x; /**< Where in the source (a fill: unused). */
                int32_t  from_y;
} bmds_surface_piece_t;

/** @brief `bmds_surface_blit_t::source`. */
#define BMDS_BLIT_BUFFER 0u /**< Another of the surface's buffers: `from` its index. */
#define BMDS_BLIT_IMAGE 1u  /**< A registered image: `from` its slot. */
#define BMDS_BLIT_FILL 2u   /**< A fill: `from` the pixel (the format's value). */

/**
 * @brief `BMDS_SURFACE_BLIT`'s argument (appended 2026-10-16, `devplan/games.md` §25): pieces of a
 *        buffer the program holds copied or filled by the 2D-DMA, in one job; the call returns
 *        when they are written. Pieces are cut to the buffer and the source. The CPU's cache is
 *        kept right (each piece's rows written back first), so what the program drew elsewhere in
 *        those rows stays. While it runs, no other thread draws in those rows. `-BMDS_EINVAL`: a
 *        buffer not held, a source that is the buffer, an empty slot, 0 or too many pieces.
 */
typedef struct bmds_surface_blit_t {
                uint32_t                    size;   /**< `sizeof(bmds_surface_blit_t)`. */
                uint32_t                    buffer; /**< Acquired and not yet presented. */
                uint32_t                    source; /**< `BMDS_BLIT_...`. */
                uint32_t                    from;   /**< The buffer's index, the image's slot or the fill's pixel. */
                uint32_t                    count;  /**< 1 to `BMDS_SURFACE_BLIT_MAX`. */
                const bmds_surface_piece_t *pieces;
} bmds_surface_blit_t;

/** @brief `bmds_surface_event_t::type`: a program skips types it doesn't know. */
#define BMDS_EVENT_KEY 1u     /**< data: the HID extended usage ((page << 16) | usage), modifiers, character. */
#define BMDS_EVENT_POINTER                                                                                             \
        2u /**< data: x, y (int32, the surface's pixels), buttons, dx | dy << 16 (int16 each: the mouse's motion
                    in the screen's pixels, also when held by `BMDS_POINTER_RELATIVE`). */
#define BMDS_EVENT_WHEEL 3u   /**< data: notches (int32, positive away from the user). */

/**
 * @brief A `BMDS_EVENT_POINTER`'s x and y when the pointer has left the surface's window (appended
 *        2026-10-15): one such event comes as it leaves (no motion, the buttons as they are), so
 *        a program can unlight what was under it. It is outside any surface.
 */
#define BMDS_POINTER_GONE (-32768)
#define BMDS_EVENT_ACTIVE 4u  /**< data: 1 active, 0 not (pause). */
#define BMDS_EVENT_PRESENTED                                                                                           \
        5u                   /**< data: full screen (1) or a window (0), the scale in 16ths (across | down << 16),
                                     the picture's size (width | height << 16). */
#define BMDS_EVENT_CLOSE 6u  /**< The user closed it (its window's close button, Alt+F4): the program decides. */
#define BMDS_EVENT_ACTION                                                                                              \
        7u /**< One of the program's actions (`bmds_controls`): flags `BMDS_ACTION_PRESS`...; data[0] its Id,
                    data[1] its value, data[2] its source (`BMDS_SOURCE_...`, a pad's number << 8). */
#define BMDS_EVENT_PAD                                                                                                 \
        8u /**< A gamepad's control changed, or a pad came or went: flags `BMDS_PAD_BUTTON`... data[0] the pad's
                    number (pad0 is 0), the rest by flags. The focused surface's only. */

/** @brief `BMDS_EVENT_KEY`'s flags. */
#define BMDS_KEY_DOWN 0x1u
#define BMDS_KEY_UP 0x2u
#define BMDS_KEY_REPEAT 0x4u

/**
 * @brief `BMDS_EVENT_PAD`'s flags: what happened (one of them, not bits).
 *
 * Pads are presented as the standard pad (devplan/controls.md §4a), their face buttons by
 * position. On gaining the focus a surface is told of each pad attached and of what is down or
 * off centre on it; on losing it, what was down comes up and sticks go to centre.
 */
#define BMDS_PAD_BUTTON 1u /**< data[1] a `BMDS_PAD_SOUTH`...; data[2] 1 down, 0 up. */
#define BMDS_PAD_AXIS                                                                                                  \
        2u                   /**< data[1] a `BMDS_PAD_LEFT_X`...; data[2] int32: sticks -32767 to 32767
                                  (right and down positive), triggers 0 to 32767. */
#define BMDS_PAD_ATTACHED 3u /**< The pad is there; data[1] its vendor << 16 | product. */
#define BMDS_PAD_DETACHED 4u /**< The pad is gone (what was down came up first). */

/** @brief `BMDS_PAD_BUTTON`'s buttons, by position. */
#define BMDS_PAD_SOUTH 0u /**< Xbox A, PlayStation cross, Nintendo B. */
#define BMDS_PAD_EAST 1u  /**< Xbox B, PlayStation circle, Nintendo A. */
#define BMDS_PAD_WEST 2u  /**< Xbox X, PlayStation square, Nintendo Y. */
#define BMDS_PAD_NORTH 3u /**< Xbox Y, PlayStation triangle, Nintendo X. */
#define BMDS_PAD_L1 4u
#define BMDS_PAD_R1 5u
#define BMDS_PAD_L2 6u /**< The left trigger as a button: down past a quarter. */
#define BMDS_PAD_R2 7u
#define BMDS_PAD_L3 8u /**< The left stick pressed. */
#define BMDS_PAD_R3 9u
#define BMDS_PAD_UP 10u /**< The D-pad. */
#define BMDS_PAD_DOWN 11u
#define BMDS_PAD_LEFT 12u
#define BMDS_PAD_RIGHT 13u
#define BMDS_PAD_START 14u
#define BMDS_PAD_SELECT 15u /**< Back, Share, Minus. */
#define BMDS_PAD_GUIDE 16u  /**< The logo button: the system may keep it for itself. */

/**
 * @brief Actions (`bmds_controls`, slot 122, appended 2026-10-15, devplan/controls.md §7c): what a
 *        program can do, declared with a keyboard binding each (and pad bindings where they
 *        apply), matched by the system against the focused surface's keys and pads. A match comes
 *        as a `BMDS_EVENT_ACTION` and not as the key or pad event it was.
 *
 * The declaration is `controls.conf`'s text (INI): a `[Controls]` group (`Sets=play;menu`, the
 * first active at start; one set if left out) and an `[Action NAME]` group per action (`Id=`,
 * `Label=`, `Group=`, `Kind=button|axis`, `Sets=`, `Keys=` (required: `Z;Ctrl`, an axis's pairs
 * `Left Right;A D`), `Pad=` (`Pad.South;Pad.LeftX-`, an axis's `Pad.LeftX` or pairs), `Repeat=`).
 * The user's changed bindings (`$HOME/.config/bmds/controls/ID.conf`, the same format) are laid
 * over it.
 */
#define BMDS_CONTROLS_DECLARE 1u /**< arg: the declaration (size bytes); NULL: the folder's controls.conf. */
#define BMDS_CONTROLS_SET 2u     /**< arg: the set's name (size bytes); NULL: none (every event raw). */
#define BMDS_CONTROLS_BINDING 3u /**< arg: `bmds_controls_binding_t`: an action's bindings as text. */

/** @brief `BMDS_EVENT_ACTION`'s flags: what happened (one of them, not bits). */
#define BMDS_ACTION_PRESS 1u   /**< A button action went down: data[1] 1. */
#define BMDS_ACTION_RELEASE 2u /**< It came up: data[1] 0. */
#define BMDS_ACTION_REPEAT 3u  /**< A held key repeated (an action with `Repeat=true`). */
#define BMDS_ACTION_AXIS 4u    /**< An axis action's value changed: data[1] int32, -32767 to 32767. */

/** @brief `BMDS_EVENT_ACTION`'s source, data[2]'s low byte (a pad's number above it). */
#define BMDS_SOURCE_KEYBOARD 1u
#define BMDS_SOURCE_PAD 2u

/** @brief `bmds_controls_binding_t::which`. */
#define BMDS_BINDING_KEYS 1u /**< The keyboard's: `Z; Ctrl`. */
#define BMDS_BINDING_PAD 2u  /**< The pads': `Pad.South; Pad.LeftX-`. */

/** @brief An action's bindings as text (`BMDS_CONTROLS_BINDING`), for menus and hints. */
typedef struct bmds_controls_binding_t {
                uint32_t size;      /**< sizeof(bmds_controls_binding_t). */
                uint32_t id;        /**< The action's Id. */
                uint32_t which;     /**< `BMDS_BINDING_KEYS` or `BMDS_BINDING_PAD`. */
                char    *text;      /**< Filled, NUL-ended (empty: none bound). */
                uint32_t text_size; /**< Its bytes. */
} bmds_controls_binding_t;

/** @brief `BMDS_PAD_AXIS`'s axes. */
#define BMDS_PAD_LEFT_X 0u
#define BMDS_PAD_LEFT_Y 1u
#define BMDS_PAD_RIGHT_X 2u
#define BMDS_PAD_RIGHT_Y 3u
#define BMDS_PAD_LEFT_TRIGGER 4u
#define BMDS_PAD_RIGHT_TRIGGER 5u

/** @brief A surface's event: 32 bytes, forever (new kinds take new types). */
typedef struct bmds_surface_event_t {
                uint32_t type;    /**< `BMDS_EVENT_...`. */
                uint32_t flags;   /**< Per type. */
                int64_t  time_ns; /**< `BMDS_CLOCK_MONOTONIC` when it happened. */
                uint32_t data[4]; /**< Per type. */
} bmds_surface_event_t;

/** @brief `proc_timer`'s timers. */
#define BMDS_TIMER_REAL 0u /**< Real time, raising `SIGALRM` (Linux's `ITIMER_REAL`). */

/**
 * @brief A process timer for `proc_timer`. The caller sets `size`; the kernel
 *        fills the fields that fit in it.
 */
typedef struct bmds_timer_t {
                uint32_t size;        /**< Set by the caller: `sizeof(bmds_timer_t)`. */
                uint32_t reserved;    /**< Zero. */
                uint64_t value_us;    /**< Microseconds until it fires; 0: disarmed (disarms). */
                uint64_t interval_us; /**< Then again every so many; 0: once. */
} bmds_timer_t;

/* --- Filesystems served by programs (devplan/netfs.md step 2; slot 123, appended 2026-10-21) --- */

/**
 * @brief A filesystem served by the program (`fs_serve`): mounted at `path`, every operation on
 *        it a request the program reads from the server fd (`bmds_fs_request_t`), answered by a
 *        reply it writes there (`bmds_fs_reply_t`); closing the fd unmounts it.
 */
typedef struct bmds_fs_serve_t {
                uint32_t    size;     /**< Set by the caller: `sizeof(bmds_fs_serve_t)`. */
                uint32_t    flags;    /**< `BMDS_O_CLOEXEC`, or 0. */
                const char *path;     /**< Where to mount it (absolute; not over another mount, nor in `/dev`). */
                const char *type;     /**< Its type's name, for `df` and the file browser ("sshfs"; 1-15 bytes). */
                uint32_t    max_data; /**< The most bytes one READ or WRITE moves, 4096 to 65536 (0: 32768). */
} bmds_fs_serve_t;

/** @brief `max_data`'s range and default. */
#define BMDS_FS_DATA_MIN 4096u
#define BMDS_FS_DATA_MAX 65536u
#define BMDS_FS_DATA_DEFAULT 32768u

/** @brief The longest path in a request, in bytes (no NUL). */
#define BMDS_FS_PATH_MAX 1024u

/** @brief The buffer a server's `read()` needs: one whole request with the most data. */
#define BMDS_FS_REQUEST_MAX(max_data) (sizeof(bmds_fs_request_t) + 2u * BMDS_FS_PATH_MAX + (max_data))

/**
 * @brief A request (`read()` from the server fd gives one), then `path_len` bytes of path (from
 *        the mount's root: `/`, or `/dir/name`; no NUL), `path2_len` bytes of a second path
 *        (`BMDS_FS_RENAME`'s target, `BMDS_FS_SYMLINK`'s), then `BMDS_FS_WRITE`'s data.
 */
typedef struct bmds_fs_request_t {
                uint32_t size;   /**< The record's bytes, all of it. */
                uint32_t op;     /**< `BMDS_FS_STAT`... */
                uint32_t id;     /**< The reply's `id`. */
                uint32_t flags;  /**< `BMDS_FS_OPEN`'s, `BMDS_FS_RENAME`'s. */
                uint64_t handle; /**< A file `BMDS_FS_OPEN` gave (READ, WRITE, CLOSE, TRUNCATE). */
                uint64_t offset; /**< READ, WRITE: where; TRUNCATE: the size; READDIR: entries to skip. */
                /** READ: the bytes wanted; WRITE: the data's; READDIR, READLINK: the most the reply may carry. */
                uint32_t length;
                uint32_t path_len;  /**< The path's bytes. */
                uint32_t path2_len; /**< The second path's bytes. */
                uint32_t reserved;  /**< 0. */
                int64_t  time_ns;   /**< SETTIME: the modification time, ns since the epoch (UTC). */
} bmds_fs_request_t;

/** @brief A reply (`write()` to the server fd, one or more a write), then its payload. */
typedef struct bmds_fs_reply_t {
                uint32_t size;  /**< The record's bytes, all of it. */
                uint32_t id;    /**< The request's. */
                int32_t  error; /**< 0, or a negative `BMDS_E*`. */
                /** READ: the bytes after it (0: the end); WRITE: the bytes written; READDIR: the entries
                 *  after it (0: the end); READLINK: the target's bytes. */
                uint32_t count;
                uint64_t handle;   /**< `BMDS_FS_OPEN`'s. */
                uint64_t reserved; /**< 0. */
} bmds_fs_reply_t;

/** @brief A file's attributes (STAT's payload) or a directory entry (READDIR's), then its name. */
typedef struct bmds_fs_entry_t {
                uint32_t size;        /**< With the name, padded to a multiple of 8. */
                uint32_t type;        /**< `BMDS_TYPE_FILE`, `_DIRECTORY`, `_DEVICE`, `_LINK`. */
                uint64_t bytes;       /**< A file's size. */
                int64_t  modified_ns; /**< Its modification time, ns since the epoch (UTC; 0: unknown). */
                uint32_t mode;        /**< The server's permission bits (shown, not enforced). */
                uint32_t name_len;    /**< The name's bytes (no NUL). */
} bmds_fs_entry_t;

/** @brief `BMDS_FS_STATFS`'s payload. */
typedef struct bmds_fs_space_t {
                uint64_t total; /**< Bytes. */
                uint64_t free;  /**< Bytes. */
} bmds_fs_space_t;

/**
 * @brief `bmds_fs_request_t::op`: what is asked (an op the server doesn't know: `-BMDS_ENOSYS`).
 *
 * A path's last component is never followed if it is a link: `STAT` describes the link, `OPEN`
 * refuses it with `-BMDS_ELOOP`, and the kernel reads it (`READLINK`) and follows it itself.
 * Links on the way to it are the server's to follow, as its own system does.
 */
#define BMDS_FS_STAT 1u      /**< path: an entry (a link itself, not where it leads). */
#define BMDS_FS_OPEN 2u      /**< path, flags `BMDS_FS_O_*`: a `handle` (a link: `-BMDS_ELOOP`). */
#define BMDS_FS_READ 3u      /**< handle, offset, length: `count` bytes. */
#define BMDS_FS_WRITE 4u     /**< handle, offset, data: `count` written. */
#define BMDS_FS_CLOSE 5u     /**< handle. */
#define BMDS_FS_READDIR 6u   /**< path, offset (entries to skip), length: `count` entries. */
#define BMDS_FS_MKDIR 7u     /**< path. */
#define BMDS_FS_RMDIR 8u     /**< path (empty). */
#define BMDS_FS_REMOVE 9u    /**< path (not a directory). */
#define BMDS_FS_RENAME 10u   /**< path to path2, flags `BMDS_FS_REPLACE`. */
#define BMDS_FS_SETTIME 11u  /**< path, time_ns. */
#define BMDS_FS_STATFS 12u   /**< A `bmds_fs_space_t`. */
#define BMDS_FS_TRUNCATE 13u /**< handle, offset (the size). */
#define BMDS_FS_READLINK 14u /**< path, length: `count` bytes, the target. */
#define BMDS_FS_SYMLINK 15u  /**< path (the link), path2 (its target). */

/** @brief `BMDS_FS_OPEN`'s flags. */
#define BMDS_FS_O_READ 0x1u
#define BMDS_FS_O_WRITE 0x2u
#define BMDS_FS_O_CREATE 0x4u    /**< Made if it isn't there. */
#define BMDS_FS_O_EXCLUSIVE 0x8u /**< With CREATE: `-BMDS_EEXIST` if it is. */
#define BMDS_FS_O_TRUNCATE 0x10u /**< Emptied. */

/** @brief `BMDS_FS_RENAME`'s flags. */
#define BMDS_FS_REPLACE 0x1u /**< A file there is replaced (else `-BMDS_EEXIST`). */

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
typedef int32_t (*bmds_sys_image_info_fn)(int32_t fd, const void *data, uint32_t length,
                                          const bmds_image_decode_t *want, bmds_image_info_t *info);
typedef int32_t (*bmds_sys_image_decode_fn)(int32_t fd, const bmds_image_decode_t *d, bmds_image_info_t *info);
typedef int32_t (*bmds_sys_image_encode_fn)(int32_t fd, const bmds_image_encode_t *e);
typedef int32_t (*bmds_sys_socket_fn)(uint32_t family, uint32_t type, uint32_t protocol, uint32_t flags);
typedef int32_t (*bmds_sys_bind_fn)(int32_t fd, const void *addr, uint32_t length);
typedef int32_t (*bmds_sys_listen_fn)(int32_t fd, int32_t backlog);
typedef int32_t (*bmds_sys_accept_fn)(int32_t fd, void *addr, uint32_t *length, uint32_t flags);
typedef int32_t (*bmds_sys_connect_fn)(int32_t fd, const void *addr, uint32_t length, uint32_t flags);
typedef int32_t (*bmds_sys_sock_name_fn)(int32_t fd, uint32_t which, void *addr, uint32_t *length);
typedef int32_t (*bmds_sys_sendmsg_fn)(int32_t fd, const bmds_msghdr_t *msg, uint32_t flags);
typedef int32_t (*bmds_sys_recvmsg_fn)(int32_t fd, bmds_msghdr_t *msg, uint32_t flags);
typedef int32_t (*bmds_sys_setsockopt_fn)(int32_t fd, int32_t level, int32_t name, const void *value, uint32_t length);
typedef int32_t (*bmds_sys_getsockopt_fn)(int32_t fd, int32_t level, int32_t name, void *value, uint32_t *length);
typedef int32_t (*bmds_sys_shutdown_fn)(int32_t fd, uint32_t how);
typedef int32_t (*bmds_sys_net_info_fn)(uint32_t index, bmds_net_info_t *info);
typedef int32_t (*bmds_sys_proc_timer_fn)(uint32_t which, const bmds_timer_t *set, bmds_timer_t *old);
typedef int32_t (*bmds_sys_net_watch_fn)(uint32_t flags);
typedef int32_t (*bmds_sys_term_ctl_fn)(int32_t fd, uint32_t op, uint32_t arg);
typedef int32_t (*bmds_sys_fs_info_fn)(const char *path, bmds_fs_info_t *info);
typedef int32_t (*bmds_sys_crypto_fn)(const bmds_crypto_op_t *op);
typedef int32_t (*bmds_sys_sched_fn)(uint32_t op, int32_t tid, bmds_sched_t *param);
typedef int32_t (*bmds_sys_audio_speak_fn)(const bmds_speak_config_t *config);
typedef int32_t (*bmds_sys_audio_speak_info_fn)(int32_t fd, bmds_speak_info_t *info);
typedef int32_t (*bmds_sys_ptp_ctl_fn)(int32_t fd, uint32_t op, void *arg);
typedef int32_t (*bmds_sys_surface_open_fn)(const bmds_surface_config_t *config);
typedef int32_t (*bmds_sys_surface_buffer_fn)(int32_t fd, bmds_surface_buffer_t *info);
typedef int32_t (*bmds_sys_surface_acquire_fn)(int32_t fd, int32_t timeout_ms);
typedef int32_t (*bmds_sys_surface_present_fn)(int32_t fd, const bmds_surface_present_t *present);
typedef int32_t (*bmds_sys_surface_control_fn)(int32_t fd, uint32_t op, void *arg, uint32_t size);
typedef int32_t (*bmds_sys_controls_fn)(uint32_t op, void *arg, uint32_t size);
typedef int32_t (*bmds_sys_fs_serve_fn)(const bmds_fs_serve_t *config);

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
 * @brief Sleeps at least @p ns nanoseconds: rounded up to the kernel's tick (1 ms), but a
 *        thread with a real-time policy (`sched`, from 2026-10-12) wakes from a hardware timer,
 *        tens of microseconds after the time.
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
 *        otherwise written within a second, and `/flash`'s queue. Before a card is taken out.
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

/**
 * @brief Fills @p info (its `size` set) about the picture in @p fd (read from where it is to
 *        its end) or, with @p fd -1, @p data's @p length bytes: what it is, and what decoding it
 *        as @p want asks (NULL: whole, `BMDS_PIXEL_BGR24`) gives, with the layout of a buffer the
 *        JPEG codec decodes into without a copy. PNG, JPEG (baseline) and BMP.
 *        (Slots 93-95, appended 2026-10-10: check `bmds_sys_has(BMDS_SYS_IMAGE_INFO)`.)
 * @return 0, or `-BMDS_ENOTSUP` (not a picture known; a progressive JPEG: `info` still filled),
 *         `-BMDS_ERANGE` (more pixels than `image/decode-max` allows, even reduced),
 *         `-BMDS_EBADF`, `-BMDS_EFAULT`, `-BMDS_EINVAL`, `-BMDS_ENOMEM`, `-BMDS_EIO`.
 */
static inline int32_t bmds_image_info(int32_t fd, const void *data, uint32_t length, const bmds_image_decode_t *want,
                                      bmds_image_info_t *info) {
        return ((bmds_sys_image_info_fn)bmds_sys->slot[BMDS_SYS_IMAGE_INFO])(fd, data, length, want, info);
}

/**
 * @brief Decodes the picture in @p fd (read to its end) or, with @p fd -1, @p d's `data` into
 *        @p d's `out` as @p d asks; @p info (if not NULL, its `size` set) says what was done. A
 *        buffer laid out as `image_info` says is decoded into by the JPEG codec without a copy.
 * @return 0, or as `image_info`, `-BMDS_EOVERFLOW` (`out` too small for `decode_width` ×
 *         `decode_height`), `-BMDS_EIO` (the file is damaged).
 */
static inline int32_t bmds_image_decode(int32_t fd, const bmds_image_decode_t *d, bmds_image_info_t *info) {
        return ((bmds_sys_image_decode_fn)bmds_sys->slot[BMDS_SYS_IMAGE_DECODE])(fd, d, info);
}

/**
 * @brief Writes @p e's picture to @p fd as a PNG or (by the codec) a baseline JPEG.
 * @return The bytes written, or `-BMDS_EINVAL`, `-BMDS_ENOTSUP` (no JPEG codec), `-BMDS_EBADF`,
 *         `-BMDS_EFAULT`, `-BMDS_ENOMEM`, `-BMDS_EIO`, or the write's error.
 */
static inline int32_t bmds_image_encode(int32_t fd, const bmds_image_encode_t *e) {
        return ((bmds_sys_image_encode_fn)bmds_sys->slot[BMDS_SYS_IMAGE_ENCODE])(fd, e);
}

/**
 * @brief Makes a socket (devplan/network.md §18): @p family `BMDS_AF_INET`, @p type
 *        `BMDS_SOCK_STREAM`, `BMDS_SOCK_DGRAM` or `BMDS_SOCK_RAW`, @p protocol 0 for the
 *        type's own (raw: 1, ICMP); @p flags `BMDS_SOCK_CLOEXEC`. A socket is a file
 *        descriptor: `read`, `write`, `io_poll`, `dup` and `close` work on it. A call that
 *        waits honours the socket's timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`) and ends with
 *        `-BMDS_EINTR` when a signal is due.
 *        (Slots 96-107, appended 2026-10-11: check `bmds_sys_has(BMDS_SYS_SOCKET)`.)
 * @return The new fd, or `-BMDS_EAFNOSUPPORT`, `-BMDS_EPROTONOSUPPORT`, `-BMDS_EINVAL`,
 *         `-BMDS_EMFILE`, `-BMDS_ENOMEM`.
 */
static inline int32_t bmds_socket(uint32_t family, uint32_t type, uint32_t protocol, uint32_t flags) {
        return ((bmds_sys_socket_fn)bmds_sys->slot[BMDS_SYS_SOCKET])(family, type, protocol, flags);
}

/**
 * @brief Gives socket @p fd its address (@p addr, @p length bytes; port 0: one is chosen).
 * @return 0, or `-BMDS_EADDRINUSE`, `-BMDS_EADDRNOTAVAIL`, `-BMDS_EINVAL` (bound already, or
 *         @p length short), `-BMDS_EAFNOSUPPORT`, `-BMDS_ENOTSOCK`, `-BMDS_EBADF`.
 */
static inline int32_t bmds_bind(int32_t fd, const void *addr, uint32_t length) {
        return ((bmds_sys_bind_fn)bmds_sys->slot[BMDS_SYS_BIND])(fd, addr, length);
}

/**
 * @brief Listens on TCP socket @p fd, at most @p backlog connections waiting (at most 8).
 * @return 0, or `-BMDS_ENOTSUP` (not TCP), `-BMDS_EINVAL` (connected).
 */
static inline int32_t bmds_listen(int32_t fd, int32_t backlog) {
        return ((bmds_sys_listen_fn)bmds_sys->slot[BMDS_SYS_LISTEN])(fd, backlog);
}

/**
 * @brief Takes a connection from listening socket @p fd: a new fd (@p flags
 *        `BMDS_SOCK_CLOEXEC`), its peer's address in @p addr (if not NULL: as `sock_name`).
 *        `BMDS_SOCK_DONTWAIT` in @p flags: `-BMDS_EAGAIN` rather than wait.
 * @return The new fd, or `-BMDS_EAGAIN`, `-BMDS_EINVAL` (not listening), `-BMDS_EINTR`,
 *         `-BMDS_EMFILE`, `-BMDS_ENOMEM`.
 */
static inline int32_t bmds_accept(int32_t fd, void *addr, uint32_t *length, uint32_t flags) {
        return ((bmds_sys_accept_fn)bmds_sys->slot[BMDS_SYS_ACCEPT])(fd, addr, length, flags);
}

/**
 * @brief Connects socket @p fd to @p addr (TCP), or sets where its datagrams go and come from
 *        (`BMDS_AF_UNSPEC` forgets it). `BMDS_SOCK_DONTWAIT` in @p flags: TCP answers
 *        `-BMDS_EINPROGRESS` and goes on; `io_poll` for writing and `SO_ERROR` tell the outcome.
 * @return 0, or `-BMDS_EINPROGRESS`, `-BMDS_EALREADY`, `-BMDS_EISCONN`, `-BMDS_ECONNREFUSED`,
 *         `-BMDS_ETIMEDOUT`, `-BMDS_ENETUNREACH`, `-BMDS_EINTR` (it goes on).
 */
static inline int32_t bmds_connect(int32_t fd, const void *addr, uint32_t length, uint32_t flags) {
        return ((bmds_sys_connect_fn)bmds_sys->slot[BMDS_SYS_CONNECT])(fd, addr, length, flags);
}

/**
 * @brief Puts socket @p fd's own address (`BMDS_SOCK_NAME_LOCAL`) or its peer's
 *        (`BMDS_SOCK_NAME_PEER`) in @p addr: as much as `*length` bytes hold; `*length` is set to
 *        the address's whole length.
 * @return 0, or `-BMDS_ENOTCONN` (no peer), `-BMDS_EINVAL`, `-BMDS_EFAULT`.
 */
static inline int32_t bmds_sock_name(int32_t fd, uint32_t which, void *addr, uint32_t *length) {
        return ((bmds_sys_sock_name_fn)bmds_sys->slot[BMDS_SYS_SOCK_NAME])(fd, which, addr, length);
}

/**
 * @brief Sends @p msg's pieces on socket @p fd: TCP bytes, or one datagram (to @p msg's
 *        `name`, or the connected peer). @p flags `BMDS_MSG_DONTWAIT`, `BMDS_MSG_NOSIGNAL`,
 *        `BMDS_MSG_MORE`. A connection that is gone gives `SIGPIPE` unless `BMDS_MSG_NOSIGNAL`.
 * @return The bytes sent (TCP: fewer only when not waiting, or after a signal or the timeout),
 *         or `-BMDS_EAGAIN`, `-BMDS_EPIPE`, `-BMDS_ECONNRESET`, `-BMDS_ENOTCONN`,
 *         `-BMDS_EDESTADDRREQ`, `-BMDS_EMSGSIZE`, `-BMDS_ENETUNREACH`, `-BMDS_EINTR`.
 */
static inline int32_t bmds_sendmsg(int32_t fd, const bmds_msghdr_t *msg, uint32_t flags) {
        return ((bmds_sys_sendmsg_fn)bmds_sys->slot[BMDS_SYS_SENDMSG])(fd, msg, flags);
}

/**
 * @brief Receives on socket @p fd into @p msg's pieces: what TCP has (at least a byte), or
 *        one datagram (cut to fit: `BMDS_MSG_TRUNC` in `flags`), its source in `name`.
 *        @p flags `BMDS_MSG_DONTWAIT`, `BMDS_MSG_PEEK`, `BMDS_MSG_WAITALL`, `BMDS_MSG_TRUNC`,
 *        `BMDS_MSG_ERRQUEUE`. A datagram's control messages are Linux's: `SCM_TIMESTAMPING`,
 *        `SCM_TIMESTAMP`, `SCM_TIMESTAMPNS` as the socket asked (`BMDS_MSG_CTRUNC` when they
 *        don't fit). `BMDS_MSG_ERRQUEUE` takes a send stamp, as Linux gives it: the frame as
 *        sent (none with `SOF_TIMESTAMPING_OPT_TSONLY`), `SCM_TIMESTAMPING`, and `IP_RECVERR`
 *        (`ENOMSG`, origin `SO_EE_ORIGIN_TIMESTAMPING`); it never waits.
 * @return The bytes received (with `BMDS_MSG_TRUNC`, a datagram's whole length), 0 at the end
 *         (the peer finished), or `-BMDS_EAGAIN`, `-BMDS_ECONNRESET`, `-BMDS_ENOTCONN`,
 *         `-BMDS_EINTR`.
 */
static inline int32_t bmds_recvmsg(int32_t fd, bmds_msghdr_t *msg, uint32_t flags) {
        return ((bmds_sys_recvmsg_fn)bmds_sys->slot[BMDS_SYS_RECVMSG])(fd, msg, flags);
}

/**
 * @brief Sets socket @p fd's option @p name at @p level (Linux's numbers): `SO_REUSEADDR`,
 *        `SO_KEEPALIVE`, `SO_RCVBUF`, `SO_SNDBUF`, `SO_RCVTIMEO`, `SO_SNDTIMEO` (both layouts),
 *        `SO_BROADCAST`, `SO_LINGER`, `SO_BINDTODEVICE`, `TCP_NODELAY`, `TCP_KEEPIDLE`,
 *        `TCP_KEEPINTVL`, `TCP_KEEPCNT`, `IP_TTL`, `IP_TOS`, `IP_ADD_MEMBERSHIP`,
 *        `IP_DROP_MEMBERSHIP`, `IP_MULTICAST_IF`, `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`,
 *        `SO_PRIORITY`, `SO_TIMESTAMPING`, `SO_TIMESTAMP`, `SO_TIMESTAMPNS` (old and new numbers),
 *        `SO_SELECT_ERR_QUEUE`; and `BMDS_SOL_BMDS`'s `BMDS_SO_FASTPATH`. Priority 6 or more,
 *        or DSCP EF or CS6, sends a UDP socket's datagrams ahead of bulk (devplan/network.md §30f).
 * @return 0, or `-BMDS_ENOPROTOOPT` (another), `-BMDS_EINVAL`, `-BMDS_EADDRINUSE` (joined),
 *         `-BMDS_EADDRNOTAVAIL`, `-BMDS_ENOBUFS` (groups), `-BMDS_ENODEV`, `-BMDS_ENOMEM`.
 */
static inline int32_t bmds_setsockopt(int32_t fd, int32_t level, int32_t name, const void *value, uint32_t length) {
        return ((bmds_sys_setsockopt_fn)bmds_sys->slot[BMDS_SYS_SETSOCKOPT])(fd, level, name, value, length);
}

/**
 * @brief Reads socket @p fd's option @p name at @p level into @p value (`*length` bytes;
 *        set to the bytes written): those `setsockopt` takes, and `SO_ERROR` (the pending
 *        error, cleared), `SO_TYPE`, `SO_DOMAIN`, `SO_PROTOCOL`, `SO_ACCEPTCONN`, `TCP_MAXSEG`.
 * @return 0, or `-BMDS_ENOPROTOOPT`, `-BMDS_EINVAL`, `-BMDS_EFAULT`.
 */
static inline int32_t bmds_getsockopt(int32_t fd, int32_t level, int32_t name, void *value, uint32_t *length) {
        return ((bmds_sys_getsockopt_fn)bmds_sys->slot[BMDS_SYS_GETSOCKOPT])(fd, level, name, value, length);
}

/**
 * @brief Shuts socket @p fd's receiving side (`BMDS_SHUT_RD`: reads end), sending side
 *        (`BMDS_SHUT_WR`: TCP sends its FIN), or both.
 * @return 0, or `-BMDS_ENOTCONN`, `-BMDS_EINVAL`.
 */
static inline int32_t bmds_shutdown(int32_t fd, uint32_t how) {
        return ((bmds_sys_shutdown_fn)bmds_sys->slot[BMDS_SYS_SHUTDOWN])(fd, how);
}

/**
 * @brief Fills @p info (its `size` set) with network interface @p index's state: 0 up to the
 *        number of interfaces, in the order of their interface indexes (`lo` first).
 * @return 0, or `-BMDS_ENODEV` (past the last), `-BMDS_EFAULT`, `-BMDS_EINVAL`.
 */
static inline int32_t bmds_net_info(uint32_t index, bmds_net_info_t *info) {
        return ((bmds_sys_net_info_fn)bmds_sys->slot[BMDS_SYS_NET_INFO])(index, info);
}

/**
 * @brief Sets the calling process's timer @p which (`BMDS_TIMER_REAL`) to @p set, or with
 *        @p set null only reads it; @p old, if not null, gets what was left of it and its
 *        interval. One timer per process: kept across `proc_exec`, not given to children,
 *        gone when the process ends. On expiry the kernel sends `SIGALRM`, never early;
 *        a repeating timer goes on by whole intervals, so a late one doesn't fire in bursts.
 * @return 0, or `-BMDS_EINVAL` (another timer, a `size` too small), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_proc_timer(uint32_t which, const bmds_timer_t *set, bmds_timer_t *old) {
        return ((bmds_sys_proc_timer_fn)bmds_sys->slot[BMDS_SYS_PROC_TIMER])(which, set, old);
}

/**
 * @brief Opens a watch on the network interfaces' state: a descriptor that is readable
 *        (`io_poll`) after a change to anything `net_info` gives but the counters and the
 *        lease's time left (an interface enabled or not, its link, settings, addresses,
 *        DHCP's state). A read (a multiple of 4 bytes) gives one `uint32_t`, the number of
 *        changes since boot, and waits while nothing changed since the descriptor's last
 *        read or its opening; then read what matters with `net_info`. Closing it ends the
 *        watch. @p flags: 0 or `BMDS_O_CLOEXEC`.
 * @return The descriptor, or `-BMDS_EINVAL`, `-BMDS_ENODEV` (no network stack),
 *         `-BMDS_ENOMEM`, `-BMDS_EMFILE`.
 */
static inline int32_t bmds_net_watch(uint32_t flags) {
        return ((bmds_sys_net_watch_fn)bmds_sys->slot[BMDS_SYS_NET_WATCH])(flags);
}

/**
 * @brief Pseudo-terminals and the controlling terminal: `op` is a `BMDS_TERM_PTY_NUMBER`, ...
 *        (what musl's `posix_openpt`, `ptsname`, `TIOCSWINSZ`, `TIOCSCTTY` and `TIOCNOTTY` use).
 *        `BMDS_TERM_CONTROL` makes @p fd's terminal the caller's (its console: what termios,
 *        `/dev/tty`, Ctrl-C and job control use) if the caller leads its session and has no
 *        terminal of that session, and the terminal belongs to no other session; its children
 *        inherit it.
 * @return 0, N for `BMDS_TERM_PTY_NUMBER`; or `-BMDS_EBADF`, `-BMDS_ENOTTY` (not a terminal,
 *         or not a pair's emulator end where one is needed), `-BMDS_EPERM` (`BMDS_TERM_CONTROL`
 *         refused), `-BMDS_EINVAL`.
 */
static inline int32_t bmds_term_ctl(int32_t fd, uint32_t op, uint32_t arg) {
        return ((bmds_sys_term_ctl_fn)bmds_sys->slot[BMDS_SYS_TERM_CTL])(fd, op, arg);
}

/**
 * @brief Describes the mounted filesystem @p path is on: its size, free space, allocation unit
 *        and type (`statfs`, `statvfs`, `df`).
 * @return 0, or `-BMDS_ENOENT`, `-BMDS_ENOSYS` (not on a mounted filesystem: `/dev`, `/bin`),
 *         `-BMDS_EFAULT`, `-BMDS_EINVAL`.
 */
static inline int32_t bmds_fs_info(const char *path, bmds_fs_info_t *info) {
        return ((bmds_sys_fs_info_fn)bmds_sys->slot[BMDS_SYS_FS_INFO])(path, info);
}

/**
 * @brief Runs AES or a SHA's compression over whole blocks on the chip's engines: about five
 *        times software's speed for AES-CTR and nine for SHA-256 on 16 KB; a call costs about
 *        13 µs, so give it whole packets, not single blocks (devplan/network.md §26-27).
 *        Padding, HMAC and the modes' partial blocks stay the caller's. Check
 *        `bmds_sys_has(BMDS_SYS_CRYPTO)` first; a kernel without the slot has no engines to offer.
 * @return 0, or `-BMDS_EINVAL` (a wrong length, key size or missing pointer), `-BMDS_ENOTSUP`
 *         (an algorithm this kernel doesn't have), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_crypto(const bmds_crypto_op_t *op) {
        return ((bmds_sys_crypto_fn)bmds_sys->slot[BMDS_SYS_CRYPTO])(op);
}

/**
 * @brief Gets or sets thread @p tid's scheduling (0: the caller's; a pid: its process's first
 *        thread), or yields (devplan/processes.md §3.12). `BMDS_SCHED_FIFO` and `BMDS_SCHED_RR`
 *        1-99 map evenly onto a band of kernel priorities above the shells, the desktop and the
 *        IP stack and below the drivers; threads of one priority share it a tick at a time
 *        (`FIFO` acts as `RR`). Real-time threads may take 95 % of each second, then run at
 *        their normal priority for the rest of it (`throttled` counts it). A new thread and a
 *        started process inherit the policy, unless `BMDS_SCHED_RESET_ON_FORK`. Check
 *        `bmds_sys_has(BMDS_SYS_SCHED)` first.
 * @return 0, or `-BMDS_EINVAL` (an op, policy, priority, `size` or flags), `-BMDS_ESRCH` (no
 *         such thread), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_sched(uint32_t op, int32_t tid, bmds_sched_t *param) {
        return ((bmds_sys_sched_fn)bmds_sys->slot[BMDS_SYS_SCHED])(op, tid, param);
}

/**
 * @brief Opens a speech fd (devplan/speech.md §4): write UTF-8 text to it, read 16 kHz mono
 *        signed 16-bit samples (`audio_speak_info` says so) to play (`/dev/dsp`), keep or save.
 *        A write takes what fits (a short count; `-BMDS_EAGAIN` when nothing did: read first).
 *        A read makes whole samples up to its size, and returns 0 once everything written has
 *        been said up to the last NUL byte: end the text with one. Without it the last sentence
 *        waits for more text, even after its full stop. Neither waits: the speech is made in
 *        the reader's thread (about a sixth of a core speaking in real time). A voice opens in
 *        about 0.2 s; each fd takes about 1 MB of memory while open. Check
 *        `bmds_sys_has(BMDS_SYS_AUDIO_SPEAK)` first.
 * @return The fd, or `-BMDS_EINVAL` (a flag, range, name or `size`), `-BMDS_ENOENT` (no such
 *         voice), `-BMDS_ENOMEM`, `-BMDS_EBUSY` (four voices in use), `-BMDS_EIO` (a damaged
 *         voice), `-BMDS_EMFILE`, `-BMDS_EFAULT`.
 */
static inline int32_t bmds_audio_speak(const bmds_speak_config_t *config) {
        return ((bmds_sys_audio_speak_fn)bmds_sys->slot[BMDS_SYS_AUDIO_SPEAK])(config);
}

/**
 * @brief Fills @p info (its `size` set) with speech fd @p fd's state.
 * @return 0, or `-BMDS_ENOTTY` (not a speech fd), `-BMDS_EBADF`, `-BMDS_EINVAL` (`size`),
 *         `-BMDS_EFAULT`.
 */
static inline int32_t bmds_audio_speak_info(int32_t fd, bmds_speak_info_t *info) {
        return ((bmds_sys_audio_speak_info_fn)bmds_sys->slot[BMDS_SYS_AUDIO_SPEAK_INFO])(fd, info);
}

/**
 * @brief Operates on PTP clock fd @p fd (`/dev/ptp0`): `BMDS_PTP_...` with @p arg as each says.
 *        Setting, stepping and the frequency are refused while the PTP service (`net/ptp`)
 *        runs on the clock's device. From 2026-10-13.
 * @return 0, or `-BMDS_ENOTTY` (not a PTP clock), `-BMDS_EBADF`, `-BMDS_EINVAL` (an op, a
 *         `size`, a `count` or `clock`), `-BMDS_EBUSY` (the PTP service has the clock),
 *         `-BMDS_ERANGE` (a frequency past `max_adj_ppb`), `-BMDS_EFAULT`.
 */
static inline int32_t bmds_ptp_ctl(int32_t fd, uint32_t op, void *arg) {
        return ((bmds_sys_ptp_ctl_fn)bmds_sys->slot[BMDS_SYS_PTP_CTL])(fd, op, arg);
}

/**
 * @brief Opens a surface (`bmds_surface_config_t`): its buffers (black) and its window, or the
 *        screen. `read` gives its events (`bmds_surface_event_t`, whole records), `io_poll`
 *        says when there are some, `close` ends it. (Slots 117-121, appended 2026-10-14: check
 *        `bmds_sys_has(BMDS_SYS_SURFACE_OPEN)`.)
 * @return A new fd, or `-BMDS_EINVAL` (the config), `-BMDS_ENOMEM`, `-BMDS_ENODEV` (no desktop
 *         running), `-BMDS_EMFILE`, `-BMDS_EFAULT`.
 */
static inline int32_t bmds_surface_open(const bmds_surface_config_t *config) {
        return ((bmds_sys_surface_open_fn)bmds_sys->slot[BMDS_SYS_SURFACE_OPEN])(config);
}

/**
 * @brief Fills @p info (its `size` and `index` set) with where buffer `index` of surface @p fd is.
 *        The address stays the same while the surface is open.
 * @return 0, or `-BMDS_EINVAL` (the index, `size`), `-BMDS_ENOTTY` (not a surface), `-BMDS_EBADF`.
 */
static inline int32_t bmds_surface_buffer(int32_t fd, bmds_surface_buffer_t *info) {
        return ((bmds_sys_surface_buffer_fn)bmds_sys->slot[BMDS_SYS_SURFACE_BUFFER])(fd, info);
}

/**
 * @brief Takes a free buffer of surface @p fd to draw, waiting up to @p timeout_ms (negative:
 *        forever; 0: only checks). The buffer is the program's until presented.
 * @return Its index, or `-BMDS_EAGAIN` (none free in time), `-BMDS_EINTR`, `-BMDS_ENOTTY`,
 *         `-BMDS_EBADF`.
 */
static inline int32_t bmds_surface_acquire(int32_t fd, int32_t timeout_ms) {
        return ((bmds_sys_surface_acquire_fn)bmds_sys->slot[BMDS_SYS_SURFACE_ACQUIRE])(fd, timeout_ms);
}

/**
 * @brief Shows a drawn buffer of surface @p fd (`bmds_surface_present_t`); the kernel writes it
 *        back from the cache. The newest present is shown at the next frame; one replaced first is
 *        never shown (`bmds_surface_info_t::dropped`). Only the damage is converted or redrawn,
 *        but the buffer must hold the whole picture: an uncovered window is redrawn from it.
 * @return The frame it will be shown in (for `BMDS_SURFACE_WAIT`), or `-BMDS_EINVAL` (not an
 *         acquired buffer, `size`, a damage rectangle), `-BMDS_ENOTTY`, `-BMDS_EBADF`, `-BMDS_EFAULT`.
 */
static inline int32_t bmds_surface_present(int32_t fd, const bmds_surface_present_t *present) {
        return ((bmds_sys_surface_present_fn)bmds_sys->slot[BMDS_SYS_SURFACE_PRESENT])(fd, present);
}

/**
 * @brief Operates on surface @p fd: `BMDS_SURFACE_...` with @p arg (@p size its bytes) as each says.
 * @return 0, or `-BMDS_EINVAL` (the op, the argument), `-BMDS_EAGAIN` (`BMDS_SURFACE_WAIT` timed
 *         out), `-BMDS_EINTR`, `-BMDS_ENOTSUP` (a palette for a surface that isn't L8),
 *         `-BMDS_ENOTTY`, `-BMDS_EBADF`, `-BMDS_EFAULT`.
 */
static inline int32_t bmds_surface_control(int32_t fd, uint32_t op, void *arg, uint32_t size) {
        return ((bmds_sys_surface_control_fn)bmds_sys->slot[BMDS_SYS_SURFACE_CONTROL])(fd, op, arg, size);
}

/**
 * @brief The program's actions: `BMDS_CONTROLS_...` with @p arg (@p size its bytes) as each says.
 * @return 0 (`BMDS_CONTROLS_BINDING`: the text's length), or `-BMDS_EINVAL` (a declaration refused:
 *         the line is logged; an op, an argument), `-BMDS_ENOENT` (no controls.conf, no such set
 *         or action, nothing declared), `-BMDS_E2BIG`, `-BMDS_ENOMEM`, `-BMDS_EFAULT`,
 *         `-BMDS_ENOSYS` (a system without actions).
 */
static inline int32_t bmds_controls(uint32_t op, void *arg, uint32_t size) {
        if (!bmds_sys_has(BMDS_SYS_CONTROLS)) return -BMDS_ENOSYS;
        return ((bmds_sys_controls_fn)bmds_sys->slot[BMDS_SYS_CONTROLS])(op, arg, size);
}

/**
 * @brief Mounts a filesystem the program serves at `config->path` (devplan/netfs.md): read
 *        requests from the fd returned (`bmds_fs_request_t`; a buffer of
 *        `BMDS_FS_REQUEST_MAX(max_data)` bytes), write replies to it (`bmds_fs_reply_t`); `poll()`
 *        says when a request waits. Closing the fd unmounts it; a request unanswered within
 *        `netfs/timeout` fails its caller with `-BMDS_EIO`.
 * @return The server fd, or `-BMDS_EINVAL` (a field), `-BMDS_EEXIST` (something mounted there),
 *         `-BMDS_EBUSY`, `-BMDS_ENOSPC` (no mount free), `-BMDS_ENOMEM`, `-BMDS_EFAULT`,
 *         `-BMDS_ENOSYS` (a system without it).
 */
static inline int32_t bmds_fs_serve(const bmds_fs_serve_t *config) {
        if (!bmds_sys_has(BMDS_SYS_FS_SERVE)) return -BMDS_ENOSYS;
        return ((bmds_sys_fs_serve_fn)bmds_sys->slot[BMDS_SYS_FS_SERVE])(config);
}

/**
 * @brief Says @p text aloud in the default voice (`speech/voice`) on `/dev/dsp`, and returns
 *        once it has been heard: `audio_speak` and an audio fd, as `say` does.
 * @return 0, or a call's `-errno` (`-BMDS_ENOSYS` without speech in the kernel; `-BMDS_EINTR`
 *         on a signal, the rest dropped).
 * @blocking While it speaks.
 */
static inline int32_t bmds_say(const char *text) {
        bmds_speak_config_t config = {sizeof(bmds_speak_config_t), BMDS_O_CLOEXEC, 0, 0, 0, 0};
        /* 2 s queued (a sentence at the slowest rate takes over 0.5 s to start), playing from 0.25 s. */
        bmds_audio_config_t audio = {sizeof(bmds_audio_config_t), BMDS_AUDIO_S16LE, 1, 16000, 32768, 4000, 0, 0};
        int16_t             samples[256];
        const char         *at = text;
        uint32_t            left = 0;
        int32_t             speech, dsp, r = 0;
        if (!bmds_sys_has(BMDS_SYS_AUDIO_SPEAK)) return -BMDS_ENOSYS;
        while (text[left] != 0) left++;
        left++; /* its NUL too: everything is said */
        speech = bmds_audio_speak(&config);
        if (speech < 0) return speech;
        dsp = bmds_open("/dev/dsp", BMDS_O_WRITE | BMDS_O_CLOEXEC);
        if (dsp < 0) {
                bmds_close(speech);
                return dsp;
        }
        r = bmds_audio_config(dsp, &audio, 0);
        while (r >= 0) {
                if (left != 0) {
                        r = bmds_write(speech, at, left);
                        if (r == -BMDS_EAGAIN) r = 0;
                        if (r < 0) break;
                        at += r;
                        left -= (uint32_t)r;
                }
                r = bmds_read(speech, samples, sizeof(samples));
                if (r < 0) break;
                if (r == 0) {
                        if (left == 0) break; /* the NUL written and all before it said */
                        continue;
                }
                {
                        const char *p = (const char *)samples;
                        int32_t     n = r;
                        while (n > 0) {
                                r = bmds_write(dsp, p, (uint32_t)n);
                                if (r < 0) break;
                                p += r;
                                n -= r;
                        }
                }
        }
        bmds_close(speech);
        /* The close waits until what is queued has played (a signal drops it). */
        if (r >= 0)
                r = bmds_close(dsp);
        else
                bmds_close(dsp);
        return r < 0 ? r : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* BMDS_SYS_H */
