/**
 * @file      programlauncher.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/fdtable.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class Interrupt;

/**
 * @brief Runs program files for the shell (bmds_loader implements it; the
 *        interface lives here so neither depends on the other).
 *
 * The shell hands over a command whose name is a path: the file, its
 * arguments (`args[0]` as typed), the exported environment as "NAME=value"
 * strings, the command's file descriptors and current directory. The call
 * returns when the program has ended.
 *
 * @par Thread Safety
 * Called from the task running the command (a shell or a pipeline worker);
 * implementations must allow several at once.
 */
class ProgramLauncher {
        public:
                /** @brief What a launch needs. */
                struct Request {
                                StringView             path;                ///< The file, as typed.
                                Span<const StringView> args;                ///< argv, args[0] included.
                                Span<const StringView> environment;         ///< "NAME=value" each.
                                const FdTable         *fds = nullptr;       ///< The command's descriptors.
                                StringView             cwd;                 ///< Its current directory.
                                Interrupt             *interrupt = nullptr; ///< Ctrl-C (the console's).
                };

                /**
                 * @brief Runs the program in @p request and waits for it to end.
                 *
                 * Problems are reported on the request's stderr, as `path: reason`.
                 *
                 * @return The exit status (0-255), or the error: `NotExist` (no such
                 *         file), `IsDir`, `NotSupported` (not a program it can run),
                 *         `NoMem`, ... (the shell maps them to 127 and 126).
                 * @blocking
                 */
                virtual Result<int> run(const Request &request) = 0;

        protected:
                ~ProgramLauncher() = default;
};

BMDS_NAMESPACE_END
