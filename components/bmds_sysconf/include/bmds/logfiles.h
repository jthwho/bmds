/**
 * @file      logfiles.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system log kept in files, and `/dev/log` (devplan/logging.md
 *        §3.3, §3.5).
 *
 * A thread writes the kernel's log lines to `system.log` in `log/dir`
 * (default `/sd0/p0/logs`), the boot's lines from before the card included:
 * batched a second (a warning or an error at once), each line's uptime stamp
 * made the local time, rotated at `log/file-bytes` into `system.log.1` and up,
 * `log/files` kept. While the directory can't be written (no card), the lines
 * wait in the log's history and the thread waits for a mount. The previous
 * boot's lines that weren't written before its reset (a panic's report among
 * them) come first, with that boot's times (§9); `--- boot ---` marks where this
 * boot's begin. Each write tells `Log::setSaved()` how far the files go.
 */
namespace LogFiles {

        /**
         * @brief Adds `/dev/log` and starts the thread. After the VFS and the
         *        registry (the settings are declared with the others).
         * @return `Ok`, or the error adding the device or starting the thread.
         */
        Error init();

} // namespace LogFiles

BMDS_NAMESPACE_END
