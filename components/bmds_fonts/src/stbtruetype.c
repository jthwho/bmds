/**
 * @file      stbtruetype.c
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * stb_truetype's code (thirdparty/stb_truetype, devplan/fonts.md): its outlines and rasterizer,
 * for FontFace, which checks a file and each glyph before handing them over.
 *
 * Single precision throughout (the P4's FPU has no doubles), and its memory from FontFace's
 * allocator (a MemSpace each time). Its asserts check its own state: on in the host tests and the
 * fuzzer (BMDS_STBTT_ASSERTS), off in the firmware.
 */

#include <math.h>
#include <stddef.h>
#include <string.h>

void *bmds_font_alloc(size_t bytes, void *context);
void  bmds_font_free(void *p, void *context);

#define STBTT_ifloor(x) ((int)floorf(x))
#define STBTT_iceil(x) ((int)ceilf(x))
#define STBTT_sqrt(x) sqrtf(x)
#define STBTT_pow(x, y) powf(x, y)
#define STBTT_fmod(x, y) fmodf(x, y)
#define STBTT_cos(x) cosf(x)
#define STBTT_acos(x) acosf(x)
#define STBTT_fabs(x) fabsf(x)
#define STBTT_malloc(x, u) bmds_font_alloc(x, u)
#define STBTT_free(x, u) bmds_font_free(x, u)
#define STBTT_strlen(x) strlen(x)
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#ifdef BMDS_STBTT_ASSERTS
#include <assert.h>
#define STBTT_assert(x) assert(x)
#else
#define STBTT_assert(x) ((void)0)
#endif

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
