/**
 * @file      tsfalloc.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TinySoundFont's allocator (synth.cpp): PSRAM, with the size kept so that
 * realloc can copy (the kernel's allocator has none).
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *bmdsSynthMalloc(size_t bytes);
void *bmdsSynthRealloc(void *ptr, size_t bytes);
void  bmdsSynthFree(void *ptr);

#ifdef __cplusplus
}
#endif
