/**
 * @file      pkgsolver.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The dependency walk (devplan/packages.md §5, §20): which packages to install, in what
 *        order, for those asked for; and which automatic packages nothing needs any more.
 *
 * Packages are given by name, version and dependency list (`PkgDepends`); where they come from
 * (files, a folder, an index) is the caller's. A dependency installed in a version that suits
 * stays as it is: nothing is upgraded unasked. Else the newest candidate that suits is taken,
 * marked automatic. The walk keeps its own stack, so a long chain can't exhaust the caller's.
 *
 * @par Thread Safety
 * Pure functions.
 */
namespace PkgSolver {

        /** @brief A package: installed, or a candidate. Views the caller keeps alive. */
        struct Package {
                        StringView name;
                        StringView version;
                        StringView depends;           ///< A `PkgDepends` list.
                        bool       automatic = false; ///< Installed as a dependency (installed packages only).
        };

        /** @brief One package to install. */
        struct Step {
                        size_t candidate = 0;     ///< Its index in the candidates.
                        bool   automatic = false; ///< Brought in as a dependency, not asked for.
        };

        /**
         * @brief Plans installing the candidates @p requested names (indexes into @p candidates)
         *        and what they need, into @p out: each package after its dependencies.
         * @param wanted When not null, set to the dependency nothing provides on `NotFound` (a
         *               view into a candidate's list), so the caller can find candidates for it
         *               and plan again.
         * @return `Ok`; `NotFound` (a dependency neither installed in a version that suits nor
         *         among the candidates); `InvalidState` (a cycle, a package asked for twice, a
         *         dependency's given version too old, or the plan would leave an installed
         *         package's dependency unmet); `CorruptData` (a dependency list that isn't one);
         *         `NoMem`. @p why says which packages, in words.
         */
        Error plan(Span<const Package> installed, Span<const Package> candidates, Span<const size_t> requested,
                   List<Step> &out, String &why, StringView *wanted = nullptr);

        /**
         * @brief The automatic installed packages nothing staying needs, through any chain, once
         *        @p removing are gone: their indexes in @p installed, into @p out.
         * @return `Ok` or `NoMem`.
         */
        Error unneeded(Span<const Package> installed, Span<const StringView> removing, List<size_t> &out);

} // namespace PkgSolver

BMDS_NAMESPACE_END
