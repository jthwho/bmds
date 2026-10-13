/**
 * @file      picopool.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What the speaker needs of Pico's internals, in C: their headers aren't C++
 * (parameters named `this`).
 */

#include <stddef.h>
#include "picoapid.h"

/** @brief Returns the bytes of @p system's pool in use now. */
size_t bmds_pico_pool_used(pico_System system) {
        picoos_int32 used = 0;
        picoos_int32 change = 0;
        picoos_int32 most = 0;
        picoos_getMemUsage(pico_sysGetCommon(system)->mm, FALSE, &used, &change, &most);
        return used > 0 ? (size_t)used : 0;
}
