/**
 * @file      systemfonts.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/fontlibrary.h>
#include <bmds/glyphcache.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system's fonts on the board (`devplan/fonts.md` §4h): one
 *        `FontLibrary` and its `GlyphCache` for the desktop and every program,
 *        following the `font/...` settings.
 *
 * Made on first use: unscii added, the faces in `font/paths` found (each
 * folder a text template: `${HOME}/Fonts`), the roles, fallback faces and
 * cache budget set, the library installed as a memory reclaimer. A change to
 * a `font/` setting is applied as it comes; a change to `font/paths` scans
 * again.
 *
 * @par Thread Safety
 * Any task.
 */
namespace SystemFonts {

        /** @brief Returns the system's library (made on first use). @blocking The first scan. */
        FontLibrary &library();

        /** @brief Returns the system's glyph cache. */
        GlyphCache &cache();

        /** @brief Returns `font/smooth`: whether text is smoothed unless a call says otherwise. */
        bool smoothing();

        /** @brief Called after a `font/` setting change is applied (the settings' task). */
        using Notify = void (*)(void *arg);

        /** @brief Most listeners `watch()` keeps. */
        constexpr size_t MaxWatchers = 4;

        /**
         * @brief Calls @p notify with @p arg after each `font/` change is applied (the
         *        roles, smoothing, contrast, the faces found), until `unwatch()`.
         * @return `Ok`, or `Full` (`MaxWatchers` already).
         */
        Error watch(Notify notify, void *arg);

        /** @brief Stops calling @p notify with @p arg. Returns once no call to it is running. */
        void unwatch(Notify notify, void *arg);

        /**
         * @brief Finds the faces in `font/paths` again.
         * @return `scan()`'s result. @blocking
         */
        Error rescan();

} // namespace SystemFonts

BMDS_NAMESPACE_END
