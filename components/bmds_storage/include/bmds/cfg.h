/**
 * @file      cfg.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief `/cfg`, the configuration (`devplan/cfg.md`): the card's (`/sd0/p0/cfg`) when a card is
 *        there at boot, else `/flash`'s (`/flash/cfg`). Everything configurable is in it: the
 *        settings' store, the password, the SSH host keys, `config/`, the applications, `profile`
 *        and `autoexec.sh`, the random seed.
 *
 * Chosen once, at boot, after the card's first check: a card put in or taken out later moves it
 * at the next restart (while the card is gone, nothing below `/cfg` exists). The two copies are
 * never synced; a card without `cfg/` gets a copy of `/flash/cfg` as it is made. The repository's
 * defaults (`flash/cfg/`) are installed in it.
 *
 * @par Thread Safety
 * `init()` once at boot; the rest from any task.
 */
namespace Cfg {

        /** @brief The card's configuration directory. */
        inline constexpr StringView CardPath("/sd0/p0/cfg");

        /** @brief `/flash`'s, used without a card. */
        inline constexpr StringView FlashPath("/flash/cfg");

        /**
         * @brief Boot stage, after `storage` and before `sysconf`: waits for the card's first
         *        check (bounded), chooses, makes the card's directory if it is new, installs the
         *        defaults, and mixes in the random seed.
         * @return `Ok`, or why `/cfg` couldn't be made (the system runs on with the defaults).
         */
        Error init();

        /** @brief Where `/cfg` leads (empty before `init()`). */
        StringView leadsTo();

        /**
         * @brief Writes a new `/cfg/random-seed` from the random pool (devplan/random.md): by the
         *        storage service a few seconds after boot, and at shutdown. Does nothing before
         *        `init()`.
         */
        void refreshRandomSeed();

} // namespace Cfg

BMDS_NAMESPACE_END
