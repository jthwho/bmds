/**
 * @file      topview.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief `top`'s frame, pure (`devplan/sysmon.md` §3.5, step 6): shares from
 *        two readings, times, sizes and the lines themselves. The batch form
 *        is a userspace interface once shipped: keep the columns and their
 *        order, and add new ones only at the end.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace TopView {

        /** @brief Text of a time, a size or a share, with its NUL. */
        constexpr size_t TextBytes = 48;

        /** @brief The longest line written, with its NUL. */
        constexpr size_t LineBytes = 256;


        /** @brief A row of the table: a process, or one of its tasks under it. */
        struct Row {
                        static constexpr uint64_t NoMemory = UINT64_MAX; ///< `memory`: not shown.

                        int32_t     pid = 0;      ///< The process's (0: the kernel's).
                        bool        task = false; ///< A task, under its process.
                        const char *name = "";
                        uint32_t    share = 0; ///< ‰ of a core since the last frame.
                        uint64_t    cpuUs = 0; ///< The time it has run.
                        uint64_t    memory = NoMemory;
                        int32_t     threads = -1; ///< -1: not shown.
                        const char *state = "-";
        };

        /**
         * @brief Returns the share of a core a counter of µs run shows: from
         *        @p thenUs to @p nowUs over @p elapsedUs, in ‰ (0 to 1000; 0 if
         *        it went back or no time passed).
         */
        uint32_t share(uint64_t nowUs, uint64_t thenUs, int64_t elapsedUs);

        /** @brief Returns true if @p a goes before @p b: the larger share, the lower pid, then by name. */
        bool isBefore(const Row &a, const Row &b);

        /** @brief Writes @p permille as a percentage with a tenth ("99.9"). @return @p out. */
        const char *formatShare(char (&out)[TextBytes], uint32_t permille);

        /** @brief Writes a time run: "M:SS.hh", from an hour "H:MM:SS". @return @p out. */
        const char *formatTime(char (&out)[TextBytes], uint64_t us);

        /** @brief Writes an uptime: "H:MM:SS", from a day "N days, H:MM:SS". @return @p out. */
        const char *formatUptime(char (&out)[TextBytes], uint64_t us);

        /** @brief Writes a size: "812 B", "173.0 KB", "6.5 MB". @return @p out. */
        const char *formatBytes(char (&out)[TextBytes], uint64_t bytes);

        /** @brief Writes the table's heading, its columns as `rowLine()`'s. @return @p out. */
        const char *headingLine(char (&out)[LineBytes]);

        /** @brief Writes @p row as a line of the table (no newline). @return @p out. */
        const char *rowLine(char (&out)[LineBytes], const Row &row);

        /** @brief Writes the first line: "top - up 1:02:03, 3 processes, 16 tasks, 0 open problems". */
        const char *summaryLine(char (&out)[LineBytes], uint64_t uptimeUs, size_t processes, size_t tasks,
                                size_t openProblems);

        /** @brief Writes the cores' line from their shares (‰): "CPU0   3.1%  CPU1  99.9%". */
        const char *coresLine(char (&out)[LineBytes], const uint32_t *shares, size_t cores);

        /** @brief Writes a memory's line: "psram       6.5 MB of 32.0 MB, largest ..., lowest free ...". */
        const char *memoryLine(char (&out)[LineBytes], StringView space, uint64_t used, uint64_t total,
                               uint64_t largest, uint64_t lowestFree);

} // namespace TopView

BMDS_NAMESPACE_END
