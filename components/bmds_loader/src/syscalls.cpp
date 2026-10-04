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

} // extern "C"
