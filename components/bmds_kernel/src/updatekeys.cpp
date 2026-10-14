/**
 * @file      updatekeys.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <sdkconfig.h>
#include <bmds/updatekeys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // keys/ by bmds_update_keys() (cmake/bmds.cmake): never empty, or the build fails.
        constexpr UpdatePackage::Key Keys[] = {
#include "updatekeys.inc"
        };

} // namespace

Span<const UpdatePackage::Key> UpdateKeys::accepted() {
        return Keys;
}

bool UpdateKeys::isDevKey(size_t index) {
        return index + 1 > BMDS_UPDATE_RELEASE_KEYS; // ">= 0" is a warning without release keys
}

bool UpdateKeys::isDevMode() {
#ifdef CONFIG_BMDS_UPDATE_KEYS_DEV
        return true;
#else
        return false;
#endif
}

BMDS_NAMESPACE_END
