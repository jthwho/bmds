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
#include "running.h"

using bmds::KernelCall;
using bmds::Running;

extern "C" {

void bmds_k_exit(int32_t status) {
        Running::current().finish(status);
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
        return call->memAlloc(size, align, flags);
}

void bmds_k_mem_free(void *ptr) {
        KernelCall call;
        call->memFree(ptr);
        return;
}

void *bmds_k_mem_realloc(void *ptr, uint32_t size, uint32_t flags) {
        KernelCall call;
        return call->memRealloc(ptr, size, flags);
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
        return call->memInfo(flags, info);
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

} // extern "C"
