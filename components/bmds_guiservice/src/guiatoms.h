/**
 * @file      guiatoms.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The GUI protocol's atoms (`devplan/guiprograms.md` §4a): one number per name, shared by
 *        every connection, for the service's life. Names are 1 to `MaxName` bytes.
 *
 * @par Thread Safety
 * Thread-safe.
 */
namespace GuiAtoms {

        constexpr size_t MaxName = 255;

        /** @brief Makes the table (`GuiService::init()`, once). @return `Ok` or `NoMem`. */
        Error init();

        /** @brief Returns @p name's atom, made if new; 0 if it can't be one (empty, too long, no memory). */
        uint32_t intern(StringView name);

        /** @brief Returns the atom's name (stable for the service's life), or empty if it isn't one. */
        StringView name(uint32_t atom);

        /** @brief Returns @p name's atom if it has one, else 0 (nothing made). */
        uint32_t find(StringView name);

} // namespace GuiAtoms

BMDS_NAMESPACE_END
