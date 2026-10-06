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

#define SLOTS 58

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
        },
};

_Static_assert(SLOTS == BMDS_SYS_THREAD_CLEAR + 1, "every slot is in the table");
_Static_assert(sizeof(table) == 16 + SLOTS * sizeof(bmds_sys_fn_t), "table layout");

const bmds_sys_t *bmds_k_sys_table(void) {
        return (const bmds_sys_t *)&table;
}
