/**
 * @file      playersettings.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

#include "playlist.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief `mplayer.conf`: what the media player keeps between runs
 *        (`devplan/mediaplayer.md` §6f), read and written as INI.
 *
 * ```
 * [Playback]
 * Volume=-12        dB, -60 to 0
 * Muted=false
 * Repeat=off        off, all or one
 * Shuffle=false
 * Entry=3           the entry playing at the end, from 1 (none: no key)
 * Position=61500    where in it, in milliseconds
 *
 * [Window]
 * Width=680         its size, the frame's included
 * Height=480
 * Tab=0             the tab shown, from 0
 *
 * [Folders]
 * Last=/home/Music  where the file dialogs open
 * ```
 *
 * Its keys are an interface once shipped: appended only. A key missing, or
 * a value that can't be read, leaves its default; unknown keys are skipped.
 *
 * @par Thread Safety
 * A value: one thread at a time.
 */
struct PlayerSettings {
                static constexpr int32_t QuietestDb = -60; ///< The volume's range: to 0.
                static constexpr size_t  NoEntry = Playlist::NoEntry;

                int32_t          volumeDb = 0;
                bool             muted = false;
                Playlist::Repeat repeat = Playlist::Repeat::Off;
                bool             shuffled = false;
                size_t           entry = NoEntry; ///< From 0 here (written from 1).
                uint64_t         positionMs = 0;
                uint16_t         width = 0; ///< 0: the default.
                uint16_t         height = 0;
                uint32_t         tab = 0;
                String           lastFolder{MemSpace::Psram}; ///< Empty: the default.

                /** @brief Reads @p text over the defaults. @return `Ok` or `NoMem`. */
                Error parse(StringView text);

                /** @brief Writes them into @p out (replaced). @return `Ok` or `NoMem`. */
                Error write(String &out) const;
};

BMDS_NAMESPACE_END
