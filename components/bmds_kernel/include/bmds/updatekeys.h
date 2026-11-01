/**
 * @file      updatekeys.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/span.h>
#include <bmds/updatepackage.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The keys this firmware accepts update packages signed with: its acceptance mode
 *        (`CONFIG_BMDS_UPDATE_KEYS_*`, keys/README.md), built in from `keys/`.
 *
 * @par Thread Safety
 * Constant data: callable from any task.
 */
namespace UpdateKeys {

        /** @brief The accepted keys: the trusted keys, and in dev mode the dev key too. */
        Span<const UpdatePackage::Key> accepted();

        /** @brief Returns true if `accepted()[index]` is the dev key, not a trusted key. */
        bool isDevKey(size_t index);

        /** @brief Returns true if this firmware accepts the dev key (`CONFIG_BMDS_UPDATE_KEYS_DEV`). */
        bool isDevMode();

        /** @brief The comment of `accepted()[index]`, from its key file (keys/README.md). */
        const char *comment(size_t index);

        /** @brief keys/dev.pub's key, in either mode: what `pkg key add` refuses (devplan/packages.md §22). */
        const UpdatePackage::Key &devKey();

} // namespace UpdateKeys

BMDS_NAMESPACE_END
