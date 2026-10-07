/**
 * @file      clipcommand.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/command.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

namespace ClipCommand {

        /** @brief The usage line `clip` shows. */
        inline constexpr const char *Usage = "[paste|copy|list|clear] [-p] [-t type]... [file]";

        /** @brief Its one-line help. */
        inline constexpr const char *Help = "Copy to, paste from and inspect the clipboard";

        /**
         * @brief `clip` (`devplan/clipboard.md` C8; frozen once shipped), on
         *        clipboard 0:
         *        - `clip [paste] [-p] [-t TYPE]`: the clipboard to stdout, as TYPE
         *          (default text), converted if it must be;
         *        - `clip copy [-p] [-t TYPE]... [FILE]`: stdin or FILE onto it, as
         *          each TYPE (default: by FILE's extension, else text);
         *        - `clip list [-p]`: its owner, types, sizes and what else it can
         *          be read as (both selections; `-p`: `Primary` only);
         *        - `clip clear [-p]`: empties it.
         *
         *        `-p` is the primary selection.
         */
        int run(CommandContext &ctx);

} // namespace ClipCommand

BMDS_NAMESPACE_END
