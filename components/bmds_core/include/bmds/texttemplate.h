/**
 * @file      texttemplate.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Text the system fills in, pure (`devplan/templates.md`): `${NAME}`
 *        and `${NAME:ARG}` replaced, `$$` a `$`, the rest copied.
 *
 * The names: `HOME`, `HOSTNAME`, `SERIAL`, `DATE` (`%Y-%m-%d`) and `TIME`
 * (`%H-%M-%S`), the last two taking a `strftime` format (`formatTime()`) as
 * `${DATE:FORMAT}`. Any other `$`, a `${` without its `}`, a name not known
 * and an argument to a name taking none are errors. The syntax and the names
 * are a userspace interface: names are appended only.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace TextTemplate {

        /** @brief What the names give: the caller's. */
        struct Values {
                        StringView       home;           ///< `HOME`.
                        StringView       hostname;       ///< `HOSTNAME`.
                        StringView       serial;         ///< `SERIAL`.
                        const LocalTime *time = nullptr; ///< `DATE` and `TIME` (null: the epoch, UTC).
        };

        /** @brief `DATE`'s format without an argument. */
        constexpr const char *DefaultDateFormat = "%Y-%m-%d";

        /** @brief `TIME`'s format without an argument (no `:`: FAT can't have one). */
        constexpr const char *DefaultTimeFormat = "%H-%M-%S";

        /** @brief Returns `Ok` if @p text is a template that expands, else `Invalid`. */
        Error check(StringView text);

        /**
         * @brief Sets @p out to @p text expanded with @p values.
         * @return `Ok`, `Invalid` (as `check()`; @p out then undefined), `NoMem`.
         */
        Error expand(StringView text, const Values &values, String &out);

} // namespace TextTemplate

BMDS_NAMESPACE_END
