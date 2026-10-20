/**
 * @file      midi.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/midirouter.h>
#include <bmds/result.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system's MIDI (`devplan/audio.md` §21): its router, the
 *        `/dev/midi` files and the settings under `midi/`.
 *
 * @par Thread Safety
 * Thread-safe once `init()` has run.
 */
namespace Midi {

        /**
         * @brief Boot stage: the router, `/dev/midi*`, and `midi/connect`
         *        followed. Before the USB host, whose MIDI devices are ports.
         * @return `Ok` (a file not added is logged).
         */
        Error init();

        /** @brief Returns the router. Not before `init()`. @nonblocking (but for its lock) */
        MidiRouter &router();

        /**
         * @brief Applies `midi/connect` if it changed since (a watcher may not read
         *        settings, so this is called where ports appear and connections are
         *        shown: opening a MIDI file, a USB device mounted or named, the
         *        `midi` command). Reads the settings: never with a lock held that
         *        they take (the VFS's), so never from `Vfs::DeviceFile::present`.
         */
        void applySettings();

        /** @brief Returns true once `init()` has run. */
        bool isReady();

        /**
         * @brief Returns `system`'s number (`/dev/midi`, `devplan/audio.md` §25): what
         *        programs play, a source only; a write to its file is sent from it.
         *        `MidiRouter::NoPort` if it couldn't be made.
         */
        uint8_t systemPort();

        /** @brief Returns a kind's name: "usb", "serial", "virtual", "net", "usbdev", "system". */
        const char *kindName(MidiRouter::Kind kind);

} // namespace Midi

BMDS_NAMESPACE_END
