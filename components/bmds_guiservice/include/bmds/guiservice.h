/**
 * @file      guiservice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

class ProgramLauncher;

/**
 * @brief The GUI service (`devplan/guiprograms.md`): the toolkit run for programs, which speak
 *        the GUI protocol on a connection (`gui_open`; later TCP). Each connection has its own
 *        `GuiApplication`, on a task of its own, and objects (windows, widgets) made by id.
 *
 * @par Thread Safety
 * `init()` once, at boot.
 */
namespace GuiService {

        /** @brief Connections at once. */
        constexpr size_t MaxConnections = 32;

        /**
         * @brief Registers the service with the loader (`GuiOpener`): the boot stage `gui`.
         *        @p launcher (kept: the loader) is what the `SHELL` extension's launch
         *        feedback asks whether a program still runs.
         */
        Error init(ProgramLauncher &launcher);

        /**
         * @brief The boot stage `guinet` (after the network): remote programs taken on
         *        127.0.0.1:`gui/port` while `gui/listen` is `loopback`, followed live
         *        (`devplan/guiprograms.md` §16).
         */
        Error listen();

} // namespace GuiService

BMDS_NAMESPACE_END
