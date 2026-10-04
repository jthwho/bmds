/**
 * @file      malloc.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The C library's allocator, over the kernel's: every block comes from
 * bmds_mem_alloc() (PSRAM), so the kernel can free what a program leaves
 * behind. These definitions replace picolibc's malloc (and its sbrk).
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/sys.h>

void *malloc(size_t size) {
        void *p = bmds_mem_alloc((uint32_t)size, 0, 0);
        if (p == NULL) errno = ENOMEM;
        return p;
}

void free(void *ptr) {
        bmds_mem_free(ptr);
}

void *calloc(size_t count, size_t size) {
        if (size != 0 && count > SIZE_MAX / size) {
                errno = ENOMEM;
                return NULL;
        }
        void *p = malloc(count * size);
        if (p != NULL) memset(p, 0, count * size);
        return p;
}

void *realloc(void *ptr, size_t size) {
        void *p = bmds_mem_realloc(ptr, (uint32_t)size, 0);
        if (p == NULL && size != 0) errno = ENOMEM;
        return p;
}

void *reallocarray(void *ptr, size_t count, size_t size) {
        if (size != 0 && count > SIZE_MAX / size) {
                errno = ENOMEM;
                return NULL;
        }
        return realloc(ptr, count * size);
}

void *memalign(size_t align, size_t size) {
        void *p = bmds_mem_alloc((uint32_t)size, (uint32_t)align, 0);
        if (p == NULL) errno = ENOMEM;
        return p;
}

void *aligned_alloc(size_t align, size_t size) {
        return memalign(align, size);
}

int posix_memalign(void **out, size_t align, size_t size) {
        if (align < sizeof(void *) || (align & (align - 1)) != 0) return EINVAL;
        void *p = bmds_mem_alloc((uint32_t)size, (uint32_t)align, 0);
        if (p == NULL) return ENOMEM;
        *out = p;
        return 0;
}
