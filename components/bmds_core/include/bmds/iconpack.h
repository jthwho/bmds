/**
 * @file      iconpack.h
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
 * @brief Reads an icon theme as one file of decoded pixels (`BMIC`,
 *        `devplan/rom.md` §3f; written by `tools/mkrom.py`): the icons by
 *        number (`IconId`, generated from the same list) and by name, each at
 *        the sizes it has, its pixels used where they lie.
 *
 * Little-endian. A 48-byte header (`BMIC`, format 1, the header's size, the
 * file's size, the icon count, up to `MaxSizes` sides, where the table, the
 * sorted index and the names are); a 24-byte row an icon, by id (its name, a
 * pixel offset per size, 0 for none); the ids sorted by name (16 bits each);
 * the names; the pixels, 0xAARRGGBB, rows top first, each icon on a 64-byte
 * boundary. `open()` checks all of it once.
 *
 * @par Thread Safety
 * A value over memory it doesn't own (kept as long as the file is).
 */
class IconPack {
        public:
                /** @brief The header's size in format 1. */
                static constexpr size_t HeaderBytes = 48;

                /** @brief A row's size. */
                static constexpr size_t RowBytes = 24;

                /** @brief The sizes a file can hold. */
                static constexpr size_t MaxSizes = 4;

                /** @brief The longest name. */
                static constexpr size_t MaxName = 63;

                /** @brief The most icons (ids are 16 bits). */
                static constexpr uint32_t MaxIcons = 65535;

                /** @brief No icon (`idOf()`). */
                static constexpr uint32_t NoIcon = UINT32_MAX;

                /** @brief An empty pack (no icons). */
                constexpr IconPack() = default;

                /**
                 * @brief Checks the pack in @p bytes (4-byte aligned, so its pixels can
                 *        be read where they are).
                 * @return The pack, `CorruptData`, `NotSupported` (a later format), or
                 *         `Invalid` (@p bytes not aligned).
                 */
                static Result<IconPack> open(ByteSpan bytes);

                /** @brief Returns the number of icons (ids 0 to `count()` − 1). */
                uint32_t count() const { return _count; }

                /** @brief Returns size @p index's side in pixels (0: none; `MaxSizes` of them, ascending). */
                uint16_t side(size_t index) const { return index < MaxSizes ? _sides[index] : 0; }

                /** @brief Returns icon @p id's name (empty past the last). */
                StringView name(uint32_t id) const;

                /** @brief Returns icon @p id's pixels at @p side × @p side, or null (no such icon or size). */
                const uint32_t *pixels(uint32_t id, uint16_t side) const;

                /** @brief Returns the id of the icon named @p name (a binary search), or `NoIcon`. */
                uint32_t idOf(StringView name) const;

        private:
                const uint8_t *row(uint32_t id) const { return _bytes.data() + _table + id * RowBytes; }

                ByteSpan _bytes;
                uint32_t _count = 0;
                uint32_t _table = 0;
                uint32_t _index = 0;
                uint32_t _names = 0;
                uint16_t _sides[MaxSizes] = {};
};

BMDS_NAMESPACE_END
