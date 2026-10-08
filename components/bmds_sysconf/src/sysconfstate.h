/**
 * @file      sysconfstate.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Private to bmds_sysconf: what the portable registry tells the boot stage
 * (sysconfinit.cpp, the target's), which logs it.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stringview.h>
#include <bmds/sysconf.h>

BMDS_NAMESPACE_BEGIN

namespace SysConf::detail {

        /** @brief How the registry came up. */
        struct LoadReport {
                        Error       error;               // setting up (NoMem)
                        Error       storeError;          // reading the store (not NotExist)
                        size_t      parameters = 0;      // declared
                        size_t      orphans = 0;         // saved lines not applied
                        size_t      rejected = 0;        // of those: values their type refused
                        size_t      malformed = 0;       // store lines not understood
                        size_t      badDeclarations = 0; // built-in declarations refused
                        const char *firstBad = nullptr;  // the first of those
        };

        /** @brief Sets the registry up with @p builtins and the store at @p storePath. */
        LoadReport start(Span<const Param> builtins, StringView storePath);

} // namespace SysConf::detail

BMDS_NAMESPACE_END
