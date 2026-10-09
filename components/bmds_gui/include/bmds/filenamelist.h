/**
 * @file      filenamelist.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The names in a file dialog's "File name" field (`devplan/mediaplayer.md`
 *        §6d), pure: one name as it is, or several each in quotes
 *        (`"a.mp3" "b.ogg"`), as the dialog writes a selection of several.
 *
 * FAT and exFAT names never hold a `"`, so a quote always marks a name's ends.
 *
 * @par Thread Safety
 * Stateless; reentrant.
 */
namespace FileNameList {

        /**
         * @brief Splits @p text into @p out (views into it): the quoted names if
         *        it starts with a quote (an unclosed one taken to the end), else
         *        the text trimmed as one name; none if it is blank.
         * @return `Ok` or `NoMem`.
         */
        Error split(StringView text, List<StringView> &out);

        /** @brief Appends @p name in quotes to @p out, a space first unless @p out is empty. */
        Error appendQuoted(StringView name, String &out);

} // namespace FileNameList

BMDS_NAMESPACE_END
