/**
 * @file      config.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Build configuration and fundamental macros. Included (directly or
 * indirectly) by every BMDS header.
 *
 * On the target, `BMDS_ENABLE_*` values are derived from Kconfig
 * (`CONFIG_BMDS_*` in the generated `sdkconfig.h`). On the host (unit
 * tests) they take the defaults below. `sdkconfig.h` is generated build
 * configuration, not an ESP-IDF API header, so including it here does not
 * break the "no IDF headers in bmds_core" rule.
 */

#pragma once

#if defined(ESP_PLATFORM)
#include <sdkconfig.h>
#define BMDS_PLATFORM_ESP 1
#define BMDS_PLATFORM_HOST 0
#else
#define BMDS_PLATFORM_ESP 0
#define BMDS_PLATFORM_HOST 1
#endif

/** @def BMDS_NAMESPACE_BEGIN
 *  @brief Opens the `bmds` namespace. */
#define BMDS_NAMESPACE_BEGIN namespace bmds {

/** @def BMDS_NAMESPACE_END
 *  @brief Closes the `bmds` namespace. */
#define BMDS_NAMESPACE_END }

/** @brief Turns the argument into a string literal without expanding it. */
#define BMDS_STRINGIFY_RAW(x) #x

/** @brief Turns the argument into a string literal after macro expansion. */
#define BMDS_STRINGIFY(x) BMDS_STRINGIFY_RAW(x)

/** @brief The current source file name without its directory (shorter rodata strings). */
#if defined(__FILE_NAME__)
#define BMDS_SOURCE_FILE __FILE_NAME__
#else
#define BMDS_SOURCE_FILE __FILE__
#endif

/** @brief Marks a printf-style function so the compiler checks its format string. */
#define BMDS_PRINTF_FORMAT(fmtIndex, argIndex) __attribute__((format(printf, fmtIndex, argIndex)))

// ---------------------------------------------------------------------------
// Feature switches
// ---------------------------------------------------------------------------

/** @def BMDS_ENABLE_ASSERT
 *  @brief 1 if `BMDS_ASSERT` checks are compiled in (Kconfig `BMDS_ASSERT`; always on for host builds). */
#if BMDS_PLATFORM_HOST || defined(CONFIG_BMDS_ASSERT)
#define BMDS_ENABLE_ASSERT 1
#else
#define BMDS_ENABLE_ASSERT 0
#endif

/** @def BMDS_ENABLE_DEBUG_LOG
 *  @brief 1 if `bmdsDebug()` logging is compiled in (Kconfig `BMDS_DEBUG_LOG`; always on for host builds). */
#if BMDS_PLATFORM_HOST || defined(CONFIG_BMDS_DEBUG_LOG)
#define BMDS_ENABLE_DEBUG_LOG 1
#else
#define BMDS_ENABLE_DEBUG_LOG 0
#endif

BMDS_NAMESPACE_BEGIN

/**
 * @brief Timeout value meaning "wait forever".
 *
 * Blocking functions take an explicit `timeoutMs` with no default. Pass
 * `WaitForever` rather than a literal `0` so every unbounded wait is
 * visible and greppable. Unbounded waits are heavily discouraged.
 */
inline constexpr unsigned int WaitForever = 0;

BMDS_NAMESPACE_END
