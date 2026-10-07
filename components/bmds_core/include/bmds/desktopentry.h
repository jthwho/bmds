/**
 * @file      desktopentry.h
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
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A launcher entry: the parts of a freedesktop.org Desktop Entry file
 *        (`NAME.desktop`) the panel uses (`devplan/gui.md` G10).
 *
 * The `[Desktop Entry]` group's `Type`, `Name`, `Comment`, `Exec`, `Icon`,
 * `Terminal`, `Categories`, `NoDisplay` and `Hidden`; localized keys, other
 * keys and other groups are ignored. Strings are kept unescaped, in fixed
 * sizes (a longer value makes the file invalid rather than cut a command).
 *
 * @par Thread Safety
 * A plain value.
 */
struct DesktopEntry {
                static constexpr size_t MaxName = 63;
                static constexpr size_t MaxComment = 127;
                static constexpr size_t MaxExec = 255;
                static constexpr size_t MaxIcon = 63;
                static constexpr size_t MaxCategories = 127;

                /** @brief The arguments `splitExec()` writes at most. */
                static constexpr size_t MaxArgs = 32;

                char name[MaxName + 1] = {};
                char comment[MaxComment + 1] = {};
                char exec[MaxExec + 1] = {};             ///< As written (quoting kept): `splitExec()`.
                char icon[MaxIcon + 1] = {};             ///< A name or a path; not drawn yet.
                char categories[MaxCategories + 1] = {}; ///< A `;` list.
                bool terminal = false;                   ///< Run in a terminal.
                bool noDisplay = false;                  ///< Not shown in menus.
                bool hidden = false;                     ///< As if it didn't exist (hides another of its name).

                /** @brief A complaint about one line, for the caller to report. */
                using Warn = void (*)(void *arg, uint32_t line, const char *message);

                /**
                 * @brief Reads @p text, a Desktop Entry file. Lines it can't use are
                 *        passed to @p warn (may be null) and skipped.
                 * @return The entry; `Invalid` (no `[Desktop Entry]` group, no
                 *         `Name`, or no `Exec` in an entry not hidden),
                 *         `NotSupported` (a `Type` other than `Application`), or
                 *         `BufferTooSmall` (a value too long).
                 */
                static Result<DesktopEntry> parse(StringView text, Warn warn = nullptr, void *warnArg = nullptr);

                /**
                 * @brief Splits `Exec` into arguments by the specification's quoting
                 *        (double quotes; inside them `\"`, `` \` ``, `\$`, `\\`),
                 *        dropping field codes (`%f`, `%U`, ...; `%%` is a `%`).
                 *        With @p file (`devplan/filetypes.md` T7), `%f`, `%F`, `%u`
                 *        and `%U` are it instead (a local path for `%u`), and an
                 *        `Exec` without one gets it as its last argument. The
                 *        arguments are NUL-ended copies in @p buffer.
                 * @return Their count; `Invalid` (an unclosed quote, or none),
                 *         or `BufferTooSmall` (@p buffer or @p args).
                 */
                static Result<size_t> splitExec(StringView exec, char *buffer, size_t bufferBytes,
                                                Span<StringView> args, StringView file = StringView());

                /** @brief Returns true if the `;` list @p categories holds @p category. */
                static bool hasCategory(StringView categories, StringView category);
};

BMDS_NAMESPACE_END
