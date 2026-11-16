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

        constexpr const char *Comments[] = {BMDS_UPDATE_KEY_COMMENTS};
        static_assert(sizeof(Comments) / sizeof(Comments[0]) == sizeof(Keys) / sizeof(Keys[0]));

        constexpr UpdatePackage::Key DevKey = BMDS_UPDATE_DEV_KEY;

#if BMDS_PACKAGE_KEY_COUNT > 0
        constexpr UpdatePackage::Key PackageKeys[] = {BMDS_PACKAGE_KEYS};
        constexpr const char        *PackageComments[] = {BMDS_PACKAGE_KEY_COMMENTS};
        static_assert(sizeof(PackageKeys) / sizeof(PackageKeys[0]) == BMDS_PACKAGE_KEY_COUNT);
        static_assert(sizeof(PackageComments) / sizeof(PackageComments[0]) == BMDS_PACKAGE_KEY_COUNT);
#endif

} // namespace

Span<const UpdatePackage::Key> UpdateKeys::accepted() {
        return Keys;
}

bool UpdateKeys::isDevKey(size_t index) {
        return index + 1 > BMDS_UPDATE_TRUSTED_KEYS; // ">= 0" is a warning without trusted keys
}

const char *UpdateKeys::comment(size_t index) {
        return index < sizeof(Comments) / sizeof(Comments[0]) ? Comments[index] : "";
}

const UpdatePackage::Key &UpdateKeys::devKey() {
        return DevKey;
}

Span<const UpdatePackage::Key> UpdateKeys::packageKeys() {
#if BMDS_PACKAGE_KEY_COUNT > 0
        return PackageKeys;
#else
        return {};
#endif
}

const char *UpdateKeys::packageComment(size_t index) {
#if BMDS_PACKAGE_KEY_COUNT > 0
        return index < BMDS_PACKAGE_KEY_COUNT ? PackageComments[index] : "";
#else
        (void)index;
        return "";
#endif
}

bool UpdateKeys::isDevMode() {
#ifdef CONFIG_BMDS_UPDATE_KEYS_DEV
        return true;
#else
        return false;
#endif
}

BMDS_NAMESPACE_END
