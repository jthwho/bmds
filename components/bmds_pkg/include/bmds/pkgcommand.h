/**
 * @file      pkgcommand.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>
#include <bmds/packages.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The `pkg` command (devplan/packages.md §9) over `Packages`, given where things are: the
 *        firmware passes its settings and keys, the host tests a tmpfs.
 *
 * Its output is a userspace interface: the text for people, and with `--json` one JSON object a
 * line for programs (the store, scripts), each with a `"type"`. Records and fields are append
 * only.
 *
 * @par Thread Safety
 * As a command: on the command's task. `Packages` allows one operation at a time.
 */
namespace PkgCommand {

        /** @brief The usage line and the help text. */
        inline constexpr const char *Usage =
                "[--json] [--source SOURCE] update | list [--installed|--upgradable] [PATTERN] | search WORD... | info "
                "NAME|FILE "
                "[--files] | install [-y] [--dry-run] [--overwrite] NAME|FILE... | upgrade [-y] [--dry-run] | "
                "remove [-y] [--dry-run] NAME... | autoremove [-y] [--dry-run] | mark auto|manual NAME... | "
                "verify [NAME...] | icon NAME [--size N] -o FILE | index DIR | clean | key list|add FILE|remove ID";
        inline constexpr const char *Help =
                "Install packages (.bmpk) by name from the sources (pkg/sources; update reads their lists) or from "
                "files, with what they need; upgrade them; remove them, and dependencies nothing needs any more; "
                "list and search what is installed and available; show a package; check installed files against "
                "their hashes; index a folder of packages; manage the keys packages may be signed with; --json: one "
                "JSON object a line, for programs; --source: one source more than pkg/sources, for this command";

        /** @brief Runs `pkg` with @p ctx's arguments. @return The exit status. */
        int run(CommandContext &ctx, const Packages::Config &config);

} // namespace PkgCommand

BMDS_NAMESPACE_END
