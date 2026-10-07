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
#include <bmds/errno.h>
#include <bmds/sys.h>
#include <bmds/deadline.h>
#include <bmds/futex.h>
#include <bmds/interrupt.h>
#include <bmds/processtable.h>
#include <bmds/random.h>
#include "running.h"

using bmds::Deadline;
using bmds::Error;
using bmds::Interrupt;
using bmds::KernelCall;
using bmds::ProgramThread;
using bmds::Signals;

extern "C" {

void bmds_k_exit(int32_t status) {
        ProgramThread::self().endProcess(bmds::ProcessTable::exitStatus(status));
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
        bmds::Random::fill(buf, size); // the kernel's pool (devplan/random.md)
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

int32_t bmds_k_file_type(const char *path, uint32_t flags, char *out, uint32_t size) {
        KernelCall call;
        return call->fileType(path, flags, out, size);
}

int32_t bmds_k_proc_vfork(void) {
        KernelCall call;
        return call.thread().vfork();
}

int32_t bmds_k_proc_exec(const char *path, char *const *argv, char *const *envp) {
        KernelCall call;
        return call.thread().exec(path, argv, envp);
}

int32_t bmds_k_proc_vfork_exit(int32_t status) {
        KernelCall call;
        return call.thread().vforkExit(status);
}

int32_t bmds_k_proc_wait(int32_t pid, int32_t *status, uint32_t flags) {
        KernelCall call;
        return call.thread().wait(pid, status, flags);
}

int32_t bmds_k_pipe(int32_t *fds, uint32_t flags) {
        KernelCall call;
        return call->pipe(fds, flags);
}

int32_t bmds_k_proc_id(uint32_t which) {
        KernelCall call;
        return call.thread().id(which);
}

int32_t bmds_k_fd_flags(int32_t fd, int32_t flags) {
        KernelCall call;
        return call->fdFlags(fd, flags);
}

int32_t bmds_k_sig_setup(volatile uint32_t *hint) {
        KernelCall call;
        return call.thread().sigSetup(hint);
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
        KernelCall       call;
        Signals         &s = call->signals();
        Signals::Thread &mine = call.thread().currentSignals();
        const uint32_t   old = Signals::blocked(mine);
        set &= (1u << (Signals::Count - 1)) - 1; // signals 1-31
        switch (how) {
                case BMDS_SIG_BLOCK: s.setBlocked(mine, old | set); break;
                case BMDS_SIG_UNBLOCK: s.setBlocked(mine, old & ~set); break;
                case BMDS_SIG_SETMASK: s.setBlocked(mine, set); break;
                default: return -BMDS_EINVAL;
        }
        return static_cast<int32_t>(old);
}

int32_t bmds_k_sig_pending(void) {
        KernelCall call;
        return static_cast<int32_t>(call->signals().pending(call.thread().currentSignals()));
}

int32_t bmds_k_sig_take(bmds_siginfo_t *info) {
        KernelCall call;
        return call.thread().sigTake(info);
}

int32_t bmds_k_sig_suspend(uint32_t mask) {
        KernelCall call;
        return call.thread().sigSuspend(mask & ((1u << (Signals::Count - 1)) - 1));
}

int32_t bmds_k_proc_kill(int32_t pid, int32_t sig) {
        KernelCall call;
        return call.thread().sendSignal(pid, sig);
}

int32_t bmds_k_proc_exit_signal(int32_t sig) {
        KernelCall call;
        return call.thread().exitSignal(sig);
}

int32_t bmds_k_io_poll(bmds_pollfd_t *fds, uint32_t count, int32_t timeout_ms, const uint32_t *mask) {
        KernelCall call;
        if (mask != nullptr) {
                // Set for the wait and left so: the SDK runs the handlers under
                // it. A signal it unblocks is due now, but nothing raised for it.
                Signals         &s = call->signals();
                Signals::Thread &mine = call.thread().currentSignals();
                s.setBlocked(mine, *mask & ((1u << (Signals::Count - 1)) - 1));
                if (s.isDue(mine)) mine.interrupt.raise();
        }
        return call->poll(fds, count, timeout_ms);
}

int32_t bmds_k_proc_group(uint32_t op, int32_t pid, int32_t pgid) {
        KernelCall call;
        return call.thread().group(op, pid, pgid);
}

int32_t bmds_k_term_foreground(int32_t fd, int32_t pgid) {
        KernelCall call;
        return call.thread().termForeground(fd, pgid);
}

int32_t bmds_k_thread_start(const bmds_thread_t *thread) {
        KernelCall call;
        return call.thread().startThread(thread);
}

void bmds_k_thread_exit(int32_t status, void *release) {
        ProgramThread::self().exitThread(status, release);
}

int32_t bmds_k_futex(volatile int32_t *word, uint32_t op, int32_t value, int64_t timeout_ns, volatile int32_t *word2,
                     int32_t count2) {
        KernelCall call;
        if (word == nullptr || (reinterpret_cast<uintptr_t>(word) & 3u) != 0) return -BMDS_EINVAL;
        bmds::FutexTable &futexes = bmds::programFutexes();
        switch (op) {
                case BMDS_FUTEX_WAIT: {
                        // Whole milliseconds, rounded up: at least what was asked.
                        constexpr int64_t NsPerMs = 1'000'000;
                        constexpr int64_t MaxMs = UINT32_MAX - 1;
                        unsigned int      ms = bmds::WaitForever;
                        if (timeout_ns >= 0) {
                                const int64_t m = timeout_ns / NsPerMs + (timeout_ns % NsPerMs != 0 ? 1 : 0);
                                if (m == 0) return *word != value ? -BMDS_EAGAIN : -BMDS_ETIMEDOUT;
                                ms = static_cast<unsigned int>(m < MaxMs ? m : MaxMs);
                        }
                        const Error err = futexes.wait(word, value, Deadline(ms), Interrupt::current());
                        switch (err.code()) {
                                case Error::Ok: return 0;
                                case Error::WouldBlock: return -BMDS_EAGAIN;
                                case Error::Timeout: return -BMDS_ETIMEDOUT;
                                default: return -BMDS_EINTR;
                        }
                }
                case BMDS_FUTEX_WAKE: return value > 0 ? futexes.wake(word, value) : 0;
                case BMDS_FUTEX_REQUEUE:
                        if (word2 == nullptr || (reinterpret_cast<uintptr_t>(word2) & 3u) != 0) return -BMDS_EINVAL;
                        return futexes.requeue(word, value > 0 ? value : 0, word2, count2 > 0 ? count2 : 0);
                default: return -BMDS_EINVAL;
        }
}

int32_t bmds_k_thread_kill(int32_t tid, int32_t sig, uint32_t flags) {
        KernelCall call;
        return call.thread().killThread(tid, sig, flags);
}

int32_t bmds_k_thread_clear(volatile int32_t *word) {
        KernelCall call;
        return call.thread().setClear(word);
}

} // extern "C"
