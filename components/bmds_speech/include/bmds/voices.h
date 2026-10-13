/**
 * @file      voices.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/speaker.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The voices speakers speak with (`devplan/speech.md` §3): found in
 *        `speech/path`'s directories, shared while used.
 *
 * A voice named `LANG` is a directory's `LANG_ta.bin` and the first
 * `LANG_*_sg.bin` beside it; the first directory holding it wins. One on a
 * filesystem that maps (`/rom`) is read where it lies; another is read into
 * PSRAM once and freed with its last user.
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace Voices {

        /** @brief Voices in use at once. */
        static constexpr size_t MaxInUse = 4;

        /** @brief The voice used when none is named, without `speech/voice`. */
        static constexpr const char *DefaultVoice = "en-US";

        /** @brief The directories voices are looked for in, without `speech/path`. */
        static constexpr const char *DefaultPath = "/rom/share/speech:/sd0/p0/share/speech";

        /** @brief A voice found. */
        struct Found {
                        String name;     ///< `en-US`.
                        String analysis; ///< Its text analysis file's path.
                        String signal;   ///< Its signal generation file's path.
        };

        /** @brief A voice in use (`acquire()`). */
        struct Voice;

        /** @brief Boot stage: the (empty) table of voices in use. @return `Ok`. */
        Error init();

        /** @brief Sets @p out to the voice used when none is named (`speech/voice`). */
        Error defaultVoice(String &out);

        /**
         * @brief Calls @p each for every voice found, a name once (the first
         *        directory's), in the path's order and each directory's.
         * @return `Ok`, or @p each's error (which stops the list).
         * @blocking Reads the directories.
         */
        Error list(Error (*each)(void *arg, const Found &found), void *arg);

        /**
         * @brief Finds voice @p name.
         * @return `Ok`, `NotExist`, or `Invalid` (not a voice's name: empty, or
         *         with `/` or `_`).
         * @blocking
         */
        Error find(StringView name, Found &found);

        /**
         * @brief Takes voice @p name (empty: the default), read where it lies or
         *        into PSRAM if no one has it.
         * @return The voice; `NotExist`, `Invalid`, `NoMem`, `Busy` (`MaxInUse`
         *         voices in use), or a file's error.
         * @blocking While its files are read (not from `/rom`).
         */
        Result<Voice *> acquire(StringView name);

        /** @brief Gives @p voice back (null: nothing). */
        void release(Voice *voice);

        /** @brief Returns @p voice's files, for `Speaker::open()`. */
        Speaker::Voice files(const Voice *voice);

        /** @brief Returns @p voice's name. */
        StringView name(const Voice *voice);

        /** @brief Returns true if @p voice is read where it lies (nothing copied). */
        bool isInPlace(const Voice *voice);

} // namespace Voices

BMDS_NAMESPACE_END
