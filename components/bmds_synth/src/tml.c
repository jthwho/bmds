/**
 * @file      tml.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TinyMidiLoader's implementation (thirdparty/tinysoundfont/tml.h, as
 * released): Standard MIDI Files into timed messages, with the synth's
 * allocator (PSRAM).
 */

#include "tsfalloc.h"

#define TML_NO_STDIO
#define TML_MALLOC bmdsSynthMalloc
#define TML_REALLOC bmdsSynthRealloc
#define TML_FREE bmdsSynthFree
#define TML_IMPLEMENTATION
#include <tml.h>
