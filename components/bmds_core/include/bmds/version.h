/**
 * @file      version.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>

/** @brief BMDS major version. */
#define BMDS_VERSION_MAJOR 0
/** @brief BMDS minor version. */
#define BMDS_VERSION_MINOR 2
/** @brief BMDS patch version. */
#define BMDS_VERSION_PATCH 0

BMDS_NAMESPACE_BEGIN

/** @brief The BMDS version as a string, e.g. `"0.1.0"`. */
inline constexpr const char *VersionString = BMDS_STRINGIFY(BMDS_VERSION_MAJOR) "." BMDS_STRINGIFY(
        BMDS_VERSION_MINOR) "." BMDS_STRINGIFY(BMDS_VERSION_PATCH);

BMDS_NAMESPACE_END
