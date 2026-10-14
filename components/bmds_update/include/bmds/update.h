/**
 * @file      update.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/updatepackage.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Updates (devplan/update.md): checking a package (`.bmup`) on the card and applying it.
 *
 * The main firmware checks a package, writes its recovery firmware to `ota_1` if that differs,
 * leaves the intent on /boot and starts recovery; the recovery firmware writes `ota_0` and `rom`,
 * checks what it wrote, leaves the result on /boot and starts the main firmware again. A reset at
 * any point leaves the intent, and recovery starts over (sections already written match, and are
 * skipped).
 *
 * Flash is erased and written on a thread of its own with an internal stack
 * (`Thread::runOnInternalStack()`), so these may be called from any task.
 *
 * @par Thread Safety
 * One check or apply at a time (a second waits for the first).
 */
namespace Update {

        /** @brief Where the intent and the result are kept. */
        inline constexpr const char *IntentPath = "/boot/update/intent";
        inline constexpr const char *ResultPath = "/boot/update/result"; ///< See `IntentPath`.

        /** @brief What a section's bytes are, against what is in its partition. */
        enum class Flashed : uint8_t {
                Unknown, ///< Not compared (not a type this firmware writes, or the check stopped first).
                Same,    ///< Its partition holds these bytes already: it isn't written.
                Differs, ///< It would be written.
        };

        /** @brief What an update is doing, for progress. */
        enum class Stage : uint8_t {
                Checking,        ///< Reading the package and hashing it, and what is flashed.
                WritingRecovery, ///< Writing `ota_1` (the main firmware does this).
                WritingFirmware, ///< Writing `ota_0` (recovery).
                WritingRom,      ///< Writing `rom` (recovery).
        };

        /** @brief Returns the stage in English ("Writing the system"). */
        const char *stageText(Stage stage);

        /** @brief Called as work goes on: @p done of @p total bytes of @p stage. */
        using Progress = void (*)(void *arg, Stage stage, uint64_t done, uint64_t total);

        /** @brief What a check found. */
        struct Report {
                        UpdatePackage::Package package;
                        /** The first problem, in English, or null when the package is good. */
                        const char *problem = nullptr;
                        bool        devKey = false; ///< Signed by the dev key (not a release key).
                        /** The package's version against the main firmware's in `ota_0`. */
                        int order = 0;
                        /** `ota_0`'s version text (its app description), "" if none. */
                        char    mainVersion[33] = {};
                        Flashed flashed[UpdatePackage::MaxSections] = {};
                        uint8_t headHash[32] = {}; ///< BLAKE2s of the signed head: what the intent names.
        };

        /**
         * @brief Checks the package at @p path (devplan/update.md §2): its signature against the
         *        accepted keys, its board and chip revision, each section's size and hash and
         *        what its image starts with, and how each compares with what is flashed.
         * @return `Ok` with a good package; `CorruptData` with one that isn't (`out.problem` says
         *         why); or why it couldn't be read (`NotExist`, `IoError`, `NoMem`...).
         * @blocking
         */
        Error check(StringView path, Report &out, Progress progress, void *arg);

        /**
         * @brief Applies the package at @p path. In the main firmware: checks it, writes its recovery
         *        firmware to `ota_1` if that differs, leaves the intent on /boot and makes
         *        recovery the next firmware: the caller restarts. In recovery: applies it at once
         *        (the caller restarts). Unless @p force, a package that isn't newer than `ota_0`'s
         *        firmware is refused (`InvalidState`).
         * @return `Ok`; `Exists` when it is all flashed already (nothing to do, no restart);
         *         `InvalidState`; or as `check()`.
         * @blocking
         */
        Error apply(StringView path, bool force, Report &out, Progress progress, void *arg);

        /**
         * @brief The update left on /boot: the intent's package (empty if none), and the last
         *        result's lines ("" if none).
         */
        struct Status {
                        String pending;
                        String result;
        };

        /**
         * @brief Who watches recovery apply an update (recovery's screen): called from the
         *        update's job, so quickly and without blocking. Any may be null.
         */
        struct Observer {
                        /** The package checked good: writing starts (@p report's package is known). */
                        void (*started)(void *arg, const Report &report) = nullptr;
                        /** As `Progress`. */
                        void (*progress)(void *arg, Stage stage, uint64_t done, uint64_t total) = nullptr;
                        /**
                         * It ended: @p failure is null when it was applied, else why not; @p restarting
                         * is false when recovery stays (the main firmware doesn't check).
                         */
                        void (*finished)(void *arg, const Report &report, const char *failure,
                                         bool restarting) = nullptr;
                        void *arg = nullptr;
        };

        /** @brief Sets the observer (outliving its use; null: none). Once, at boot. */
        void setObserver(const Observer *observer);

        /** @brief Reads the intent and the result from /boot. @blocking */
        Error status(Status &out);

        /**
         * @brief Boot stage. The main firmware logs the result of an update just applied. Recovery
         *        applies the intent, if any, on a thread of its own once the card is mounted, then
         *        restarts.
         */
        Error init();

} // namespace Update

BMDS_NAMESPACE_END
