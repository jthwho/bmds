/**
 * @file      syscalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system call slots (target only): plain C functions that find the
 * calling program's Process and forward to it, each inside a KernelCall
 * (so a kill never stops a program inside one). systable.c lists them.
 */

#include <cstdint>
#include <esp_random.h>
#include <bmds/errno.h>
#include <bmds/sys.h>
#include <bmds/processtable.h>
#include "running.h"

using bmds::KernelCall;
using bmds::Running;
using bmds::Signals;

extern "C" {

void bmds_k_exit(int32_t status) {
        Running::self().finish(bmds::ProcessTable::exitStatus(status));
}

int32_t bmds_k_read(int32_t fd, void *buf, uint32_t size) {
        KernelCall call;
        return call->read(fd, buf, size);
}

int32_t bmds_k_write(int32_t fd, const void *buf, uint32_t size) {
        KernelCall call;
        return call->write(fd, buf, size);
}

int32_t bmds_k_close(int32_t fd) {
        KernelCall call;
        return call->close(fd);
}

int32_t bmds_k_seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position) {
        KernelCall call;
        return call->seek(fd, offset, whence, position);
}

int32_t bmds_k_isatty(int32_t fd) {
        KernelCall call;
        return call->isatty(fd);
}

void *bmds_k_mem_alloc(uint32_t size, uint32_t align, uint32_t flags) {
        KernelCall call;
        return call.owner()->memAlloc(size, align, flags);
}

void bmds_k_mem_free(void *ptr) {
        KernelCall call;
        call.owner()->memFree(ptr);
        return;
}

void *bmds_k_mem_realloc(void *ptr, uint32_t size, uint32_t flags) {
        KernelCall call;
        return call.owner()->memRealloc(ptr, size, flags);
}

int32_t bmds_k_open(const char *path, uint32_t flags) {
        KernelCall call;
        return call->open(path, flags);
}

int32_t bmds_k_stat(const char *path, bmds_stat_t *info) {
        KernelCall call;
        return call->stat(path, info);
}

int32_t bmds_k_fstat(int32_t fd, bmds_stat_t *info) {
        KernelCall call;
        return call->fstat(fd, info);
}

int32_t bmds_k_mkdir(const char *path) {
        KernelCall call;
        return call->mkdir(path);
}

int32_t bmds_k_rmdir(const char *path) {
        KernelCall call;
        return call->rmdir(path);
}

int32_t bmds_k_unlink(const char *path) {
        KernelCall call;
        return call->unlink(path);
}

int32_t bmds_k_rename(const char *from, const char *to) {
        KernelCall call;
        return call->rename(from, to);
}

int32_t bmds_k_opendir(const char *path) {
        KernelCall call;
        return call->opendir(path);
}

int32_t bmds_k_readdir(int32_t dir, bmds_stat_t *info, char *name, uint32_t size) {
        KernelCall call;
        return call->readdir(dir, info, name, size);
}

int32_t bmds_k_closedir(int32_t dir) {
        KernelCall call;
        return call->closedir(dir);
}

int32_t bmds_k_getcwd(char *buf, uint32_t size) {
        KernelCall call;
        return call->getcwd(buf, size);
}

int32_t bmds_k_chdir(const char *path) {
        KernelCall call;
        return call->chdir(path);
}

int32_t bmds_k_dup(int32_t fd, int32_t min_fd) {
        KernelCall call;
        return call->dup(fd, min_fd);
}

int32_t bmds_k_dup2(int32_t fd, int32_t to) {
        KernelCall call;
        return call->dup2(fd, to);
}

int32_t bmds_k_clock_get(uint32_t clock, int64_t *ns) {
        KernelCall call;
        return call->clockGet(clock, ns);
}

int32_t bmds_k_sleep(int64_t ns) {
        KernelCall call;
        return call->sleep(ns);
}

int32_t bmds_k_interrupted(void) {
        KernelCall call;
        return call->interrupted();
}

int32_t bmds_k_term_get(int32_t fd, bmds_term_t *term) {
        KernelCall call;
        return call->termGet(fd, term);
}

int32_t bmds_k_term_set(int32_t fd, uint32_t flags) {
        KernelCall call;
        return call->termSet(fd, flags);
}

int32_t bmds_k_mem_info(uint32_t flags, bmds_mem_info_t *info) {
        KernelCall call;
        return call.owner()->memInfo(flags, info);
}

int32_t bmds_k_random(void *buf, uint32_t size) {
        KernelCall call;
        if (buf == nullptr && size != 0) return -BMDS_EFAULT;
        esp_fill_random(buf, size);
        return 0;
}

int32_t bmds_k_set_mtime(const char *path, int64_t ns) {
        KernelCall call;
        return call->setMtime(path, ns);
}

int32_t bmds_k_ftruncate(int32_t fd, int64_t size) {
        KernelCall call;
        return call->ftruncate(fd, size);
}

int32_t bmds_k_symlink(const char *target, const char *path) {
        KernelCall call;
        return call->symlink(target, path);
}

int32_t bmds_k_readlink(const char *path, char *buf, uint32_t size) {
        KernelCall call;
        return call->readlink(path, buf, size);
}

int32_t bmds_k_lstat(const char *path, bmds_stat_t *info) {
        KernelCall call;
        return call->lstat(path, info);
}

int32_t bmds_k_proc_vfork(void) {
        KernelCall call;
        return call.running().vfork();
}

int32_t bmds_k_proc_exec(const char *path, char *const *argv, char *const *envp) {
        KernelCall call;
        return call.running().exec(path, argv, envp);
}

int32_t bmds_k_proc_vfork_exit(int32_t status) {
        KernelCall call;
        return call.running().vforkExit(status);
}

int32_t bmds_k_proc_wait(int32_t pid, int32_t *status, uint32_t flags) {
        KernelCall call;
        return call.running().wait(pid, status, flags);
}

int32_t bmds_k_pipe(int32_t *fds, uint32_t flags) {
        KernelCall call;
        return call->pipe(fds, flags);
}

int32_t bmds_k_proc_id(uint32_t which) {
        KernelCall call;
        return call.running().id(which);
}

int32_t bmds_k_fd_flags(int32_t fd, int32_t flags) {
        KernelCall call;
        return call->fdFlags(fd, flags);
}

int32_t bmds_k_sig_setup(volatile uint32_t *hint) {
        KernelCall call;
        return call.running().sigSetup(hint);
}

int32_t bmds_k_sig_action(int32_t sig, uint32_t action) {
        KernelCall call;
        if (!Signals::isValid(sig) || (action > BMDS_SIG_CATCH && action != BMDS_SIG_QUERY)) return -BMDS_EINVAL;
        Signals       &s = call->signals();
        const uint32_t previous = static_cast<uint32_t>(s.action(sig));
        if (action != BMDS_SIG_QUERY && !s.setAction(sig, static_cast<Signals::Action>(action))) return -BMDS_EINVAL;
        return static_cast<int32_t>(previous);
}

int32_t bmds_k_sig_mask(uint32_t how, uint32_t set) {
        KernelCall     call;
        Signals       &s = call->signals();
        const uint32_t old = s.blocked();
        set &= (1u << (Signals::Count - 1)) - 1; // signals 1-31
        switch (how) {
                case BMDS_SIG_BLOCK: s.setBlocked(old | set); break;
                case BMDS_SIG_UNBLOCK: s.setBlocked(old & ~set); break;
                case BMDS_SIG_SETMASK: s.setBlocked(set); break;
                default: return -BMDS_EINVAL;
        }
        return static_cast<int32_t>(old);
}

int32_t bmds_k_sig_pending(void) {
        KernelCall call;
        return static_cast<int32_t>(call->signals().pending());
}

int32_t bmds_k_sig_take(bmds_siginfo_t *info) {
        KernelCall call;
        return call.running().sigTake(info);
}

int32_t bmds_k_sig_suspend(uint32_t mask) {
        KernelCall call;
        return call.running().sigSuspend(mask & ((1u << (Signals::Count - 1)) - 1));
}

int32_t bmds_k_proc_kill(int32_t pid, int32_t sig) {
        KernelCall call;
        return call.running().sendSignal(pid, sig);
}

int32_t bmds_k_proc_exit_signal(int32_t sig) {
        KernelCall call;
        return call.running().exitSignal(sig);
}

} // extern "C"
