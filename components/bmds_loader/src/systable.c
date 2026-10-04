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

#define SLOTS 9

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
        },
};

_Static_assert(sizeof(table) == 16 + SLOTS * sizeof(bmds_sys_fn_t), "table layout");

const bmds_sys_t *bmds_k_sys_table(void) {
        return (const bmds_sys_t *)&table;
}
