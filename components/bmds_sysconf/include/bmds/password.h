/**
 * @file      password.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/configtypes.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The machine's one user's password (devplan/network.md §24):
 *        `system/password`, a `secret` holding SHA-512 crypt or nothing.
 *
 * The plain password is only ever passed through: `set()` hashes it. Programs
 * check it as on Linux, `crypt()` against `/etc/shadow` (Dropbear, BusyBox);
 * every program is root, as the one user, and may read it.
 *
 * Also `/etc/passwd` and `/etc/group` with the one user, `root` (uid 0, home
 * `/home`, shell `/bin/sh`), for programs that look users up; `passwd`'s
 * password field is `x` (in `/etc/shadow`).
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace Password {

        /** @brief The setting. */
        inline constexpr const char *Key = "system/password";

        /** @brief Rounds a new hash gets: about 0.1 s a check on the board (5000 took 265 ms; network.md §25). */
        inline constexpr uint32_t Rounds = 2000;

        /** @brief Characters of salt a new hash gets. */
        inline constexpr size_t SaltChars = 16;

        /**
         * @brief Adds `/etc/passwd`, `/etc/group` and `/etc/shadow`. After `SysConf::init()`.
         * @return `Ok`, or the VFS's error.
         */
        Error init();

        /** @brief The setting's declaration (for a registry made without the linker section: tests). */
        const Config::Param &declaration();

        /** @brief True if a password is set. */
        bool isSet();

        /**
         * @brief Sets the password to @p plain (hashed with a random salt), and
         *        saves it; empty clears it.
         * @return `Ok`, `Invalid` (longer than `ShaCrypt::MaxPassword`), or the
         *         registry's error.
         * @blocking
         */
        Error set(StringView plain);

        /**
         * @brief `/etc/shadow`'s text: `root:HASH:...`, or `root:!:...` with no
         *        password (a login refused). Appended to @p out.
         */
        Error shadowFile(String &out);

} // namespace Password

BMDS_NAMESPACE_END
