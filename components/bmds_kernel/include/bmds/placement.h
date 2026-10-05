/**
 * @file      placement.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#if defined(ESP_PLATFORM)
#include <esp_attr.h>
#endif

/**
 * @def BMDS_ISR_ATTR
 * @brief Places an interrupt handler (and what it calls) in internal RAM, so it
 *        runs while the flash cache is off (`CODING_STANDARDS.md`).
 *
 * @def BMDS_HOT_ATTR
 * @brief Places a hot path in internal RAM: its instruction fetches never
 *        wait behind other traffic in the L2 cache, which serves flash and
 *        PSRAM for both cores (`devplan/display.md`: a busy core's PSRAM
 *        traffic made the scanout's refills in flash several times slower).
 *        Internal RAM is scarce: only for measured needs.
 */
#if defined(ESP_PLATFORM)
#define BMDS_ISR_ATTR IRAM_ATTR
#define BMDS_HOT_ATTR IRAM_ATTR
#else
#define BMDS_ISR_ATTR
#define BMDS_HOT_ATTR
#endif
