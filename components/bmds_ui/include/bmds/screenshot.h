/**
 * @file      screenshot.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/png.h>
#include <bmds/string.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Display 0's picture saved as a PNG file (`devplan/display.md` §5d,
 *        `devplan/notifications.md` §4): the `screenshot` command's and Print
 *        Screen's.
 *
 * @par Thread Safety
 * Thread-safe (each call its own copy of the screen).
 */
namespace Screenshot {

        /** @brief The setting naming the file: a text template (`devplan/templates.md`). */
        inline constexpr const char *PathKey = "screenshot/path";

        /** @brief `save()`'s flags. */
        enum Flag : uint8_t {
                Unique = 1u << 0,   ///< An existing file kept: `_2`, `_3` ... put before the extension.
                MakeDirs = 1u << 1, ///< The directories made.
        };

        /** @brief What `save()` did. */
        struct Report {
                        const char *failed =
                                nullptr;      ///< What failed: "display 0", "copy", "capture", or null (the file).
                        uint16_t   width = 0; ///< The picture's.
                        uint16_t   height = 0;
                        uint32_t   copyUs = 0;  ///< Copying the screen.
                        uint32_t   totalUs = 0; ///< In all.
                        Png::Stats stats;       ///< Writing the PNG.
        };

        /**
         * @brief Sets @p out to the file `PathKey` names now (expanded: `SysConf::expand()`).
         * @return `Ok`, `NotExist` (no setting), `Invalid`, `NoMem`.
         */
        Error configuredPath(String &out);

        /**
         * @brief Copies display 0's picture (the window manager's last whole round)
         *        and writes it to @p path as a PNG of @p level (`Png`), relative to
         *        @p context; with `Unique` @p path is set to the name used.
         * @return `Ok`, or the error (`Report::failed` says where).
         * @blocking The copy (about 10 ms) and the PNG (a second or so).
         */
        Error save(String &path, int level, uint8_t flags, const Vfs::Context &context, Report &report);

} // namespace Screenshot

BMDS_NAMESPACE_END
