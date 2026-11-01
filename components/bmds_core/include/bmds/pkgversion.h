/**
 * @file      pkgversion.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Package versions (devplan/packages.md §4), Debian's: `[EPOCH:]UPSTREAM[-REVISION]`, as
 *        `1.37.0-1`, compared as `dpkg --compare-versions` does.
 *
 * The epoch is a number; the upstream version starts with a digit and holds letters, digits and
 * `. + ~ -` (`-` only with a revision, the text after the last one); the revision holds letters,
 * digits and `. + ~`. Parts compare in turn: the epoch as a number (none is 0); the upstream
 * version and the revision (none is "0") alternately as runs of non-digits (letters sort before
 * everything else, `~` before anything, even the end) and runs of digits (as numbers).
 *
 * @par Thread Safety
 * Pure functions.
 */
namespace PkgVersion {

        /** @brief The longest version: a package header's field. */
        constexpr size_t MaxBytes = 32;

        /** @brief Returns true if @p version is a valid version of at most `MaxBytes`. */
        bool isValid(StringView version);

        /**
         * @brief Compares two valid versions. @return Less than 0 if @p a is older than @p b, 0 if
         *        they are the same version, more than 0 if @p a is newer.
         */
        int compare(StringView a, StringView b);

} // namespace PkgVersion

BMDS_NAMESPACE_END
