/**
 * @file      syscalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system call slots (target only): plain C functions that find the
 * calling program's Process and forward to it. systable.c lists them.
 */

#include <cstdint>
#include <esp_random.h>
#include <bmds/errno.h>
#include <bmds/sys.h>
#include "running.h"

using bmds::Running;

extern "C" {

void bmds_k_exit(int32_t status) {
        Running::current().finish(status);
}

int32_t bmds_k_read(int32_t fd, void *buf, uint32_t size) {
        return Running::current().process->read(fd, buf, size);
}

int32_t bmds_k_write(int32_t fd, const void *buf, uint32_t size) {
        return Running::current().process->write(fd, buf, size);
}

int32_t bmds_k_close(int32_t fd) {
        return Running::current().process->close(fd);
}

int32_t bmds_k_seek(int32_t fd, int64_t offset, uint32_t whence, int64_t *position) {
        return Running::current().process->seek(fd, offset, whence, position);
}

int32_t bmds_k_isatty(int32_t fd) {
        return Running::current().process->isatty(fd);
}

void *bmds_k_mem_alloc(uint32_t size, uint32_t align, uint32_t flags) {
        return Running::current().process->memAlloc(size, align, flags);
}

void bmds_k_mem_free(void *ptr) {
        Running::current().process->memFree(ptr);
        return;
}

void *bmds_k_mem_realloc(void *ptr, uint32_t size, uint32_t flags) {
        return Running::current().process->memRealloc(ptr, size, flags);
}

int32_t bmds_k_open(const char *path, uint32_t flags) {
        return Running::current().process->open(path, flags);
}

int32_t bmds_k_stat(const char *path, bmds_stat_t *info) {
        return Running::current().process->stat(path, info);
}

int32_t bmds_k_fstat(int32_t fd, bmds_stat_t *info) {
        return Running::current().process->fstat(fd, info);
}

int32_t bmds_k_mkdir(const char *path) {
        return Running::current().process->mkdir(path);
}

int32_t bmds_k_rmdir(const char *path) {
        return Running::current().process->rmdir(path);
}

int32_t bmds_k_unlink(const char *path) {
        return Running::current().process->unlink(path);
}

int32_t bmds_k_rename(const char *from, const char *to) {
        return Running::current().process->rename(from, to);
}

int32_t bmds_k_opendir(const char *path) {
        return Running::current().process->opendir(path);
}

int32_t bmds_k_readdir(int32_t dir, bmds_stat_t *info, char *name, uint32_t size) {
        return Running::current().process->readdir(dir, info, name, size);
}

int32_t bmds_k_closedir(int32_t dir) {
        return Running::current().process->closedir(dir);
}

int32_t bmds_k_getcwd(char *buf, uint32_t size) {
        return Running::current().process->getcwd(buf, size);
}

int32_t bmds_k_chdir(const char *path) {
        return Running::current().process->chdir(path);
}

int32_t bmds_k_dup(int32_t fd, int32_t min_fd) {
        return Running::current().process->dup(fd, min_fd);
}

int32_t bmds_k_dup2(int32_t fd, int32_t to) {
        return Running::current().process->dup2(fd, to);
}

int32_t bmds_k_clock_get(uint32_t clock, int64_t *ns) {
        return Running::current().process->clockGet(clock, ns);
}

int32_t bmds_k_sleep(int64_t ns) {
        return Running::current().process->sleep(ns);
}

int32_t bmds_k_interrupted(void) {
        return Running::current().process->interrupted();
}

int32_t bmds_k_term_get(int32_t fd, bmds_term_t *term) {
        return Running::current().process->termGet(fd, term);
}

int32_t bmds_k_term_set(int32_t fd, uint32_t flags) {
        return Running::current().process->termSet(fd, flags);
}

int32_t bmds_k_mem_info(uint32_t flags, bmds_mem_info_t *info) {
        return Running::current().process->memInfo(flags, info);
}

int32_t bmds_k_random(void *buf, uint32_t size) {
        if (buf == nullptr && size != 0) return -BMDS_EFAULT;
        esp_fill_random(buf, size);
        return 0;
}

int32_t bmds_k_set_mtime(const char *path, int64_t ns) {
        return Running::current().process->setMtime(path, ns);
}

} // extern "C"
