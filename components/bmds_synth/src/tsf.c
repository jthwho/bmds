/**
 * @file      tsf.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TinySoundFont's implementation (thirdparty/tinysoundfont, patched: see its
 * README), with the synth's allocator (PSRAM) and single-precision math, and
 * stb_vorbis's declarations for SF3 fonts (its code is bmds_audio's).
 */

#include <stddef.h>
#include "synthmath.h"
#include "tsfalloc.h"

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define TSF_NO_STDIO
#define TSF_MALLOC bmdsSynthMalloc
#define TSF_REALLOC bmdsSynthRealloc
#define TSF_FREE bmdsSynthFree
#define TSF_POW bmdsSynthPow
#define TSF_POWF bmdsSynthPowf
#define TSF_EXPF bmdsSynthExpf
#define TSF_LOG bmdsSynthLog
#define TSF_TAN tan
#define TSF_TANF bmdsSynthTanf
#define TSF_LOG10 bmdsSynthLog10
#define TSF_SQRT sqrt
#define TSF_SQRTF sqrtf
#define TSF_IMPLEMENTATION
#include <tsf.h>

/* What a font holds in memory: its samples, presets and regions (tsfalloc.h's sizes). */
size_t bmdsSynthAllocated(const void *ptr);

size_t bmdsSynthFontBytes(const tsf *f) {
        size_t bytes = bmdsSynthAllocated(f->fontSamples) + bmdsSynthAllocated(f->presets) + sizeof(*f);
        for (int i = 0; i < f->presetNum; ++i) bytes += bmdsSynthAllocated(f->presets[i].regions);
        return bytes;
}

void bmdsSynthPresetId(const tsf *f, int index, unsigned short *bank, unsigned short *number) {
        *bank = index >= 0 && index < f->presetNum ? f->presets[index].bank : 0;
        *number = index >= 0 && index < f->presetNum ? f->presets[index].preset : 0;
}
