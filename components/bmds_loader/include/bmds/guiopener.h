/**
 * @file      guiopener.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/filehandle.h>
#include <bmds/result.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where `gui_open` finds the GUI service (`devplan/guiprograms.md` §8). The service is
 *        above the loader (the toolkit depends on it), so it registers itself here at boot,
 *        and the system call asks through it.
 *
 * @par Thread Safety
 * `set()` once, at boot, before programs run; `open()` from any task.
 */
namespace GuiOpener {

        /** @brief Who is connecting: for the service's records and, later, its permissions. */
        struct Client {
                        int32_t    pid = 0;
                        StringView program; ///< The program's path.
        };

        /**
         * @brief Opens a connection to display @p display for @p client: the client's end.
         * @return The end, `NotSupported` (no such display, or no window system running),
         *         `Full`, or `NoMem`.
         */
        using Open = Result<FileHandle::Ptr> (*)(uint32_t display, const Client &client);

        /** @brief Sets the service's opener. */
        void set(Open open);

        /** @brief Opens through the service's opener (`NotSupported` without one). */
        Result<FileHandle::Ptr> open(uint32_t display, const Client &client);

} // namespace GuiOpener

BMDS_NAMESPACE_END
