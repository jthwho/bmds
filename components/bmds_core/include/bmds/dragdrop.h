/**
 * @file      dragdrop.h
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
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What a drop does (`devplan/clipboard.md` §5b D6, D7). Append only.
 */
enum class DropAction : uint8_t {
        None,  ///< Nothing: refused, or cancelled.
        Copy,  ///< The data copied; the source keeps its own.
        Move,  ///< The data copied; the source removes its own.
        Link,  ///< A link to the source's data made.
        Moved, ///< The target moved the data itself (files): the source does nothing.
        Ask,   ///< A right drag's drop: the target asks which.
};

/**
 * @brief The actions a source allows, and the target's choice among them
 *        (`devplan/clipboard.md` §5b D6). Pure.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace DropActions {

        /** @brief The actions' bits in a mask of those offered. */
        enum : uint8_t {
                CanCopy = 1,
                CanMove = 2,
                CanLink = 4,
        };

        /** @brief Returns @p action's bit (0 for `None`, `Moved` and `Ask`). */
        constexpr uint8_t bit(DropAction action) {
                return action == DropAction::Copy   ? CanCopy
                       : action == DropAction::Move ? CanMove
                       : action == DropAction::Link ? CanLink
                                                    : 0;
        }

        /** @brief Returns true if @p offered allows @p action. */
        constexpr bool allows(uint8_t offered, DropAction action) {
                return (offered & bit(action)) != 0;
        }

        /**
         * @brief The action a drop takes: Ctrl copies, Shift moves, both link
         *        (only if @p offered allows it, else none); with neither, the
         *        target's @p targetDefault, then the source's @p preferred,
         *        then the first of Copy, Move and Link offered.
         * @return The action, or `None` (nothing offered fits).
         */
        DropAction choose(uint8_t offered, bool ctrl, bool shift, DropAction targetDefault, DropAction preferred);

} // namespace DropActions

/**
 * @brief Words quoted for a POSIX shell (`devplan/clipboard.md` §5b D12): a
 *        terminal types dropped paths with it. Pure.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace ShellQuote {

        /** @brief Returns true if @p word needs no quoting (letters, digits and `._/+-:@%,=`; not empty). */
        bool isPlain(StringView word);

        /**
         * @brief Appends @p word to @p out as the shell reads it back: as it is
         *        if plain, else in single quotes, each `'` as `'\''`.
         * @return `Ok` or `NoMem`.
         */
        Error append(String &out, StringView word);

} // namespace ShellQuote

/**
 * @brief Where a drag's image is (`devplan/clipboard.md` §5b D3): a box of at
 *        most `Max` × `Max` around the pressed item, in the view's own
 *        coordinates, and which items it shows whole. Pure.
 *
 * @par Thread Safety
 * Reentrant.
 */
namespace DragImage {

        /** @brief The image's largest side, in pixels. */
        inline constexpr int Max = 128;

        /** @brief A rectangle that may start left of or above its view's origin. */
        struct Box {
                        int x = 0;
                        int y = 0;
                        int width = 0;
                        int height = 0;

                        constexpr bool operator==(const Box &) const = default;
        };

        /**
         * @brief The image's box: `Max` × `Max` centred on the pressed item
         *        @p item, moved so that the press (@p pressX, @p pressY) and as
         *        much of the item as fits are inside it.
         */
        Box around(const Box &item, int pressX, int pressY);

        /** @brief Returns true if @p box holds all of @p item (drawn in the image). */
        bool holds(const Box &box, const Box &item);

} // namespace DragImage

BMDS_NAMESPACE_END
