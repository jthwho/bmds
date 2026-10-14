/**
 * @file      sshservice.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>

BMDS_NAMESPACE_BEGIN

class ProgramLauncher;

/**
 * @brief The SSH server as a kernel service (devplan/network.md §24): while
 *        `net/ssh/server` is true, a kernel thread keeps Dropbear (`dropbear`,
 *        found on `shell/path`, a program on the card) running with the
 *        `net/ssh/` settings, its output to the log; it makes the host key the
 *        first time. A change of a setting starts it again; one that ends on its
 *        own is started again after 5 s, doubling to 5 minutes. mDNS advertises
 *        `_ssh._tcp` while it runs.
 *
 * Settings: `net/ssh/server` (bool, false), `net/ssh/port` (int, 22),
 * `net/ssh/password-login` (bool, true), `net/ssh/host-keys` (path,
 * `/cfg/ssh`), `net/ssh/authorized-keys-dir` (a template, `${HOME}/.ssh`).
 *
 * @par Thread Safety
 * `init()` once, at boot.
 */
namespace SshService {

        /**
         * @brief Declares the settings, watches them, and starts the server if it
         *        is on, programs run through @p launcher (kept). After the network
         *        stack and the program loader.
         * @return `Ok`, or the error declaring.
         */
        Error init(ProgramLauncher &launcher);

} // namespace SshService

BMDS_NAMESPACE_END
