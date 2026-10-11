/**
 * @file      systable.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system call table every program gets (bmds/sys.h), in flash. C,
 * because a C initializer may cast function pointers in a constant (a
 * C++ one would need a global constructor). Slots are appended, never
 * moved: the ABI lock (abilock.c) pins their numbers.
 */

#include <stdint.h>
#include <bmds/sys.h>

void    bmds_k_exit(int32_t status);
int32_t bmds_k_read(int32_t fd, void *buf, uint32_t size);
int32_t bmds_k_write(int32_t fd, const void *buf, uint32_t size);
int32_t bmds_k_close(int32_t fd);
int32_t bmds_k_seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position);
int32_t bmds_k_isatty(int32_t fd);
void   *bmds_k_mem_alloc(uint32_t size, uint32_t align, uint32_t flags);
void    bmds_k_mem_free(void *ptr);
void   *bmds_k_mem_realloc(void *ptr, uint32_t size, uint32_t flags);
int32_t bmds_k_open(const char *path, uint32_t flags);
int32_t bmds_k_stat(const char *path, bmds_stat_t *info);
int32_t bmds_k_fstat(int32_t fd, bmds_stat_t *info);
int32_t bmds_k_mkdir(const char *path);
int32_t bmds_k_rmdir(const char *path);
int32_t bmds_k_unlink(const char *path);
int32_t bmds_k_rename(const char *from, const char *to);
int32_t bmds_k_opendir(const char *path);
int32_t bmds_k_readdir(int32_t dir, bmds_stat_t *info, char *name, uint32_t size);
int32_t bmds_k_closedir(int32_t dir);
int32_t bmds_k_getcwd(char *buf, uint32_t size);
int32_t bmds_k_chdir(const char *path);
int32_t bmds_k_dup(int32_t fd, int32_t min_fd);
int32_t bmds_k_dup2(int32_t fd, int32_t to);
int32_t bmds_k_clock_get(uint32_t clock, int64_t *ns);
int32_t bmds_k_sleep(int64_t ns);
int32_t bmds_k_interrupted(void);
int32_t bmds_k_term_get(int32_t fd, bmds_term_t *term);
int32_t bmds_k_term_set(int32_t fd, uint32_t flags);
int32_t bmds_k_mem_info(uint32_t flags, bmds_mem_info_t *info);
int32_t bmds_k_random(void *buf, uint32_t size);
int32_t bmds_k_set_mtime(const char *path, int64_t ns);
int32_t bmds_k_ftruncate(int32_t fd, int64_t size);
int32_t bmds_k_symlink(const char *target, const char *path);
int32_t bmds_k_readlink(const char *path, char *buf, uint32_t size);
int32_t bmds_k_lstat(const char *path, bmds_stat_t *info);
int32_t bmds_k_proc_vfork(void);
int32_t bmds_k_proc_exec(const char *path, char *const *argv, char *const *envp);
int32_t bmds_k_proc_vfork_exit(int32_t status);
int32_t bmds_k_proc_wait(int32_t pid, int32_t *status, uint32_t flags);
int32_t bmds_k_pipe(int32_t *fds, uint32_t flags);
int32_t bmds_k_proc_id(uint32_t which);
int32_t bmds_k_fd_flags(int32_t fd, int32_t flags);
int32_t bmds_k_sig_setup(volatile uint32_t *hint);
int32_t bmds_k_sig_action(int32_t sig, uint32_t action);
int32_t bmds_k_sig_mask(uint32_t how, uint32_t set);
int32_t bmds_k_sig_pending(void);
int32_t bmds_k_sig_take(bmds_siginfo_t *info);
int32_t bmds_k_sig_suspend(uint32_t mask);
int32_t bmds_k_proc_kill(int32_t pid, int32_t sig);
int32_t bmds_k_proc_exit_signal(int32_t sig);
int32_t bmds_k_io_poll(bmds_pollfd_t *fds, uint32_t count, int32_t timeout_ms, const uint32_t *mask);
int32_t bmds_k_proc_group(uint32_t op, int32_t pid, int32_t pgid);
int32_t bmds_k_term_foreground(int32_t fd, int32_t pgid);
int32_t bmds_k_thread_start(const bmds_thread_t *thread);
void    bmds_k_thread_exit(int32_t status, void *release);
int32_t bmds_k_futex(volatile int32_t *word, uint32_t op, int32_t value, int64_t timeout_ns, volatile int32_t *word2,
                     int32_t count2);
int32_t bmds_k_thread_kill(int32_t tid, int32_t sig, uint32_t flags);
int32_t bmds_k_thread_clear(volatile int32_t *word);
int32_t bmds_k_file_type(const char *path, uint32_t flags, char *out, uint32_t size);
int32_t bmds_k_sysconf_find(const char *key);
int32_t bmds_k_sysconf_get(int32_t id, uint32_t field, char *out, uint32_t size);
int32_t bmds_k_sysconf_info(int32_t id, bmds_sysconf_info_t *info);
int32_t bmds_k_sysconf_set(int32_t id, uint32_t op, const char *value);
int32_t bmds_k_sysconf_next(const char *prefix, int32_t after);
int32_t bmds_k_sysconf_declare(const bmds_sysconf_param_t *param);
int32_t bmds_k_sysconf_watch(const char *prefix, uint32_t flags);
int32_t bmds_k_sync(void);
int32_t bmds_k_audio_config(int32_t fd, const bmds_audio_config_t *set, bmds_audio_config_t *got);
int32_t bmds_k_audio_status(int32_t fd, bmds_audio_status_t *status);
int32_t bmds_k_audio_control(int32_t fd, uint32_t op, int32_t value);
int32_t bmds_k_sound_play(uint32_t sound);
int32_t bmds_k_audio_decode(int32_t fd, const bmds_decode_config_t *config);
int32_t bmds_k_audio_decode_info(int32_t fd, bmds_decode_info_t *info);
int32_t bmds_k_audio_decode_tag(int32_t fd, uint32_t index, char *key, uint32_t key_size, char *value,
                                uint32_t value_size);
int32_t bmds_k_audio_cycle(int32_t fd, bmds_audio_cycle_t *cycle);
int32_t bmds_k_midi_config(int32_t fd, const bmds_midi_config_t *set, bmds_midi_config_t *got);
int32_t bmds_k_midi_status(int32_t fd, bmds_midi_status_t *status);
int32_t bmds_k_midi_control(int32_t fd, uint32_t op, int32_t value);
int32_t bmds_k_audio_synth(const bmds_synth_config_t *config);
int32_t bmds_k_audio_synth_info(int32_t fd, bmds_synth_info_t *info);
int32_t bmds_k_stats_list(uint32_t index, bmds_counter_t *info);
int32_t bmds_k_stats_read(const uint32_t *ids, uint32_t count, int64_t *values, uint8_t *health, int64_t *time_ns);
int32_t bmds_k_stats_history(uint32_t id, int64_t *values, uint32_t max, int64_t *newest_ns);
int32_t bmds_k_stats_problems(uint64_t after, bmds_problem_t *out, uint32_t max);
int32_t bmds_k_stats_control(uint32_t op, uint64_t value);
int32_t bmds_k_stats_status(bmds_stats_status_t *status);
int32_t bmds_k_task_list(bmds_task_info_t *out, uint32_t max);
int32_t bmds_k_proc_info(int32_t pid, uint32_t flags, bmds_proc_info_t *info);
int32_t bmds_k_system_info(bmds_system_info_t *info);
int32_t bmds_k_notify_open(uint32_t flags);
int32_t bmds_k_notify_send(int32_t fd, const bmds_notification_t *n);
int32_t bmds_k_notify_close(uint32_t id);
int32_t bmds_k_expand(const char *text, char *out, uint32_t size);
int32_t bmds_k_image_info(int32_t fd, const void *data, uint32_t length, const bmds_image_decode_t *want,
                          bmds_image_info_t *info);
int32_t bmds_k_image_decode(int32_t fd, const bmds_image_decode_t *d, bmds_image_info_t *info);
int32_t bmds_k_image_encode(int32_t fd, const bmds_image_encode_t *e);
int32_t bmds_k_socket(uint32_t family, uint32_t type, uint32_t protocol, uint32_t flags);
int32_t bmds_k_bind(int32_t fd, const void *addr, uint32_t length);
int32_t bmds_k_listen(int32_t fd, int32_t backlog);
int32_t bmds_k_accept(int32_t fd, void *addr, uint32_t *length, uint32_t flags);
int32_t bmds_k_connect(int32_t fd, const void *addr, uint32_t length, uint32_t flags);
int32_t bmds_k_sock_name(int32_t fd, uint32_t which, void *addr, uint32_t *length);
int32_t bmds_k_sendmsg(int32_t fd, const bmds_msghdr_t *msg, uint32_t flags);
int32_t bmds_k_recvmsg(int32_t fd, bmds_msghdr_t *msg, uint32_t flags);
int32_t bmds_k_setsockopt(int32_t fd, int32_t level, int32_t name, const void *value, uint32_t length);
int32_t bmds_k_getsockopt(int32_t fd, int32_t level, int32_t name, void *value, uint32_t *length);
int32_t bmds_k_shutdown(int32_t fd, uint32_t how);
int32_t bmds_k_net_info(uint32_t index, bmds_net_info_t *info);
int32_t bmds_k_proc_timer(uint32_t which, const bmds_timer_t *set, bmds_timer_t *old);
int32_t bmds_k_net_watch(uint32_t flags);
int32_t bmds_k_term_ctl(int32_t fd, uint32_t op, uint32_t arg);
int32_t bmds_k_fs_info(const char *path, bmds_fs_info_t *info);

#define SLOTS 112

/* bmds_sys_t with its slots spelled out (same layout; checked below). */
static const struct {
                uint32_t      magic;
                uint32_t      abi_version;
                uint32_t      count;
                uint32_t      reserved;
                bmds_sys_fn_t slot[SLOTS];
} table = {
        BMDS_SYS_MAGIC,
        BMDS_ABI_VERSION,
        SLOTS,
        0,
        {
                [BMDS_SYS_EXIT] = (bmds_sys_fn_t)bmds_k_exit,
                [BMDS_SYS_READ] = (bmds_sys_fn_t)bmds_k_read,
                [BMDS_SYS_WRITE] = (bmds_sys_fn_t)bmds_k_write,
                [BMDS_SYS_CLOSE] = (bmds_sys_fn_t)bmds_k_close,
                [BMDS_SYS_SEEK] = (bmds_sys_fn_t)bmds_k_seek,
                [BMDS_SYS_ISATTY] = (bmds_sys_fn_t)bmds_k_isatty,
                [BMDS_SYS_MEM_ALLOC] = (bmds_sys_fn_t)bmds_k_mem_alloc,
                [BMDS_SYS_MEM_FREE] = (bmds_sys_fn_t)bmds_k_mem_free,
                [BMDS_SYS_MEM_REALLOC] = (bmds_sys_fn_t)bmds_k_mem_realloc,
                [BMDS_SYS_OPEN] = (bmds_sys_fn_t)bmds_k_open,
                [BMDS_SYS_STAT] = (bmds_sys_fn_t)bmds_k_stat,
                [BMDS_SYS_FSTAT] = (bmds_sys_fn_t)bmds_k_fstat,
                [BMDS_SYS_MKDIR] = (bmds_sys_fn_t)bmds_k_mkdir,
                [BMDS_SYS_RMDIR] = (bmds_sys_fn_t)bmds_k_rmdir,
                [BMDS_SYS_UNLINK] = (bmds_sys_fn_t)bmds_k_unlink,
                [BMDS_SYS_RENAME] = (bmds_sys_fn_t)bmds_k_rename,
                [BMDS_SYS_OPENDIR] = (bmds_sys_fn_t)bmds_k_opendir,
                [BMDS_SYS_READDIR] = (bmds_sys_fn_t)bmds_k_readdir,
                [BMDS_SYS_CLOSEDIR] = (bmds_sys_fn_t)bmds_k_closedir,
                [BMDS_SYS_GETCWD] = (bmds_sys_fn_t)bmds_k_getcwd,
                [BMDS_SYS_CHDIR] = (bmds_sys_fn_t)bmds_k_chdir,
                [BMDS_SYS_DUP] = (bmds_sys_fn_t)bmds_k_dup,
                [BMDS_SYS_DUP2] = (bmds_sys_fn_t)bmds_k_dup2,
                [BMDS_SYS_CLOCK_GET] = (bmds_sys_fn_t)bmds_k_clock_get,
                [BMDS_SYS_SLEEP] = (bmds_sys_fn_t)bmds_k_sleep,
                [BMDS_SYS_INTERRUPTED] = (bmds_sys_fn_t)bmds_k_interrupted,
                [BMDS_SYS_TERM_GET] = (bmds_sys_fn_t)bmds_k_term_get,
                [BMDS_SYS_TERM_SET] = (bmds_sys_fn_t)bmds_k_term_set,
                [BMDS_SYS_MEM_INFO] = (bmds_sys_fn_t)bmds_k_mem_info,
                [BMDS_SYS_RANDOM] = (bmds_sys_fn_t)bmds_k_random,
                [BMDS_SYS_SET_MTIME] = (bmds_sys_fn_t)bmds_k_set_mtime,
                [BMDS_SYS_FTRUNCATE] = (bmds_sys_fn_t)bmds_k_ftruncate,
                [BMDS_SYS_SYMLINK] = (bmds_sys_fn_t)bmds_k_symlink,
                [BMDS_SYS_READLINK] = (bmds_sys_fn_t)bmds_k_readlink,
                [BMDS_SYS_LSTAT] = (bmds_sys_fn_t)bmds_k_lstat,
                [BMDS_SYS_PROC_VFORK] = (bmds_sys_fn_t)bmds_k_proc_vfork,
                [BMDS_SYS_PROC_EXEC] = (bmds_sys_fn_t)bmds_k_proc_exec,
                [BMDS_SYS_PROC_VFORK_EXIT] = (bmds_sys_fn_t)bmds_k_proc_vfork_exit,
                [BMDS_SYS_PROC_WAIT] = (bmds_sys_fn_t)bmds_k_proc_wait,
                [BMDS_SYS_PIPE] = (bmds_sys_fn_t)bmds_k_pipe,
                [BMDS_SYS_PROC_ID] = (bmds_sys_fn_t)bmds_k_proc_id,
                [BMDS_SYS_FD_FLAGS] = (bmds_sys_fn_t)bmds_k_fd_flags,
                [BMDS_SYS_SIG_SETUP] = (bmds_sys_fn_t)bmds_k_sig_setup,
                [BMDS_SYS_SIG_ACTION] = (bmds_sys_fn_t)bmds_k_sig_action,
                [BMDS_SYS_SIG_MASK] = (bmds_sys_fn_t)bmds_k_sig_mask,
                [BMDS_SYS_SIG_PENDING] = (bmds_sys_fn_t)bmds_k_sig_pending,
                [BMDS_SYS_SIG_TAKE] = (bmds_sys_fn_t)bmds_k_sig_take,
                [BMDS_SYS_SIG_SUSPEND] = (bmds_sys_fn_t)bmds_k_sig_suspend,
                [BMDS_SYS_PROC_KILL] = (bmds_sys_fn_t)bmds_k_proc_kill,
                [BMDS_SYS_PROC_EXIT_SIGNAL] = (bmds_sys_fn_t)bmds_k_proc_exit_signal,
                [BMDS_SYS_IO_POLL] = (bmds_sys_fn_t)bmds_k_io_poll,
                [BMDS_SYS_PROC_GROUP] = (bmds_sys_fn_t)bmds_k_proc_group,
                [BMDS_SYS_TERM_FOREGROUND] = (bmds_sys_fn_t)bmds_k_term_foreground,
                [BMDS_SYS_THREAD_START] = (bmds_sys_fn_t)bmds_k_thread_start,
                [BMDS_SYS_THREAD_EXIT] = (bmds_sys_fn_t)bmds_k_thread_exit,
                [BMDS_SYS_FUTEX] = (bmds_sys_fn_t)bmds_k_futex,
                [BMDS_SYS_THREAD_KILL] = (bmds_sys_fn_t)bmds_k_thread_kill,
                [BMDS_SYS_THREAD_CLEAR] = (bmds_sys_fn_t)bmds_k_thread_clear,
                [BMDS_SYS_FILE_TYPE] = (bmds_sys_fn_t)bmds_k_file_type,
                [BMDS_SYS_SYSCONF_FIND] = (bmds_sys_fn_t)bmds_k_sysconf_find,
                [BMDS_SYS_SYSCONF_GET] = (bmds_sys_fn_t)bmds_k_sysconf_get,
                [BMDS_SYS_SYSCONF_INFO] = (bmds_sys_fn_t)bmds_k_sysconf_info,
                [BMDS_SYS_SYSCONF_SET] = (bmds_sys_fn_t)bmds_k_sysconf_set,
                [BMDS_SYS_SYSCONF_NEXT] = (bmds_sys_fn_t)bmds_k_sysconf_next,
                [BMDS_SYS_SYSCONF_DECLARE] = (bmds_sys_fn_t)bmds_k_sysconf_declare,
                [BMDS_SYS_SYSCONF_WATCH] = (bmds_sys_fn_t)bmds_k_sysconf_watch,
                [BMDS_SYS_SYNC] = (bmds_sys_fn_t)bmds_k_sync,
                [BMDS_SYS_AUDIO_CONFIG] = (bmds_sys_fn_t)bmds_k_audio_config,
                [BMDS_SYS_AUDIO_STATUS] = (bmds_sys_fn_t)bmds_k_audio_status,
                [BMDS_SYS_AUDIO_CONTROL] = (bmds_sys_fn_t)bmds_k_audio_control,
                [BMDS_SYS_SOUND_PLAY] = (bmds_sys_fn_t)bmds_k_sound_play,
                [BMDS_SYS_AUDIO_DECODE] = (bmds_sys_fn_t)bmds_k_audio_decode,
                [BMDS_SYS_AUDIO_DECODE_INFO] = (bmds_sys_fn_t)bmds_k_audio_decode_info,
                [BMDS_SYS_AUDIO_DECODE_TAG] = (bmds_sys_fn_t)bmds_k_audio_decode_tag,
                [BMDS_SYS_AUDIO_CYCLE] = (bmds_sys_fn_t)bmds_k_audio_cycle,
                [BMDS_SYS_MIDI_CONFIG] = (bmds_sys_fn_t)bmds_k_midi_config,
                [BMDS_SYS_MIDI_STATUS] = (bmds_sys_fn_t)bmds_k_midi_status,
                [BMDS_SYS_MIDI_CONTROL] = (bmds_sys_fn_t)bmds_k_midi_control,
                [BMDS_SYS_AUDIO_SYNTH] = (bmds_sys_fn_t)bmds_k_audio_synth,
                [BMDS_SYS_AUDIO_SYNTH_INFO] = (bmds_sys_fn_t)bmds_k_audio_synth_info,
                [BMDS_SYS_STATS_LIST] = (bmds_sys_fn_t)bmds_k_stats_list,
                [BMDS_SYS_STATS_READ] = (bmds_sys_fn_t)bmds_k_stats_read,
                [BMDS_SYS_STATS_HISTORY] = (bmds_sys_fn_t)bmds_k_stats_history,
                [BMDS_SYS_STATS_PROBLEMS] = (bmds_sys_fn_t)bmds_k_stats_problems,
                [BMDS_SYS_STATS_CONTROL] = (bmds_sys_fn_t)bmds_k_stats_control,
                [BMDS_SYS_STATS_STATUS] = (bmds_sys_fn_t)bmds_k_stats_status,
                [BMDS_SYS_TASK_LIST] = (bmds_sys_fn_t)bmds_k_task_list,
                [BMDS_SYS_PROC_INFO] = (bmds_sys_fn_t)bmds_k_proc_info,
                [BMDS_SYS_SYSTEM_INFO] = (bmds_sys_fn_t)bmds_k_system_info,
                [BMDS_SYS_NOTIFY_OPEN] = (bmds_sys_fn_t)bmds_k_notify_open,
                [BMDS_SYS_NOTIFY_SEND] = (bmds_sys_fn_t)bmds_k_notify_send,
                [BMDS_SYS_NOTIFY_CLOSE] = (bmds_sys_fn_t)bmds_k_notify_close,
                [BMDS_SYS_EXPAND] = (bmds_sys_fn_t)bmds_k_expand,
                [BMDS_SYS_IMAGE_INFO] = (bmds_sys_fn_t)bmds_k_image_info,
                [BMDS_SYS_IMAGE_DECODE] = (bmds_sys_fn_t)bmds_k_image_decode,
                [BMDS_SYS_IMAGE_ENCODE] = (bmds_sys_fn_t)bmds_k_image_encode,
                [BMDS_SYS_SOCKET] = (bmds_sys_fn_t)bmds_k_socket,
                [BMDS_SYS_BIND] = (bmds_sys_fn_t)bmds_k_bind,
                [BMDS_SYS_LISTEN] = (bmds_sys_fn_t)bmds_k_listen,
                [BMDS_SYS_ACCEPT] = (bmds_sys_fn_t)bmds_k_accept,
                [BMDS_SYS_CONNECT] = (bmds_sys_fn_t)bmds_k_connect,
                [BMDS_SYS_SOCK_NAME] = (bmds_sys_fn_t)bmds_k_sock_name,
                [BMDS_SYS_SENDMSG] = (bmds_sys_fn_t)bmds_k_sendmsg,
                [BMDS_SYS_RECVMSG] = (bmds_sys_fn_t)bmds_k_recvmsg,
                [BMDS_SYS_SETSOCKOPT] = (bmds_sys_fn_t)bmds_k_setsockopt,
                [BMDS_SYS_GETSOCKOPT] = (bmds_sys_fn_t)bmds_k_getsockopt,
                [BMDS_SYS_SHUTDOWN] = (bmds_sys_fn_t)bmds_k_shutdown,
                [BMDS_SYS_NET_INFO] = (bmds_sys_fn_t)bmds_k_net_info,
                [BMDS_SYS_PROC_TIMER] = (bmds_sys_fn_t)bmds_k_proc_timer,
                [BMDS_SYS_NET_WATCH] = (bmds_sys_fn_t)bmds_k_net_watch,
                [BMDS_SYS_TERM_CTL] = (bmds_sys_fn_t)bmds_k_term_ctl,
                [BMDS_SYS_FS_INFO] = (bmds_sys_fn_t)bmds_k_fs_info,
        },
};

_Static_assert(SLOTS == BMDS_SYS_FS_INFO + 1, "every slot is in the table");
_Static_assert(sizeof(table) == 16 + SLOTS * sizeof(bmds_sys_fn_t), "table layout");

const bmds_sys_t *bmds_k_sys_table(void) {
        return (const bmds_sys_t *)&table;
}
