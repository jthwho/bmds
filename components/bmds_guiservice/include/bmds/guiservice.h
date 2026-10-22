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

        /** @brief Registers the service with the loader (`GuiOpener`): the boot stage `gui`. */
        Error init();

} // namespace GuiService

BMDS_NAMESPACE_END
