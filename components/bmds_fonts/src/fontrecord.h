/**
 * @file      fontrecord.h
 * @copyright Jason Howard. All rights reserved.
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A FontLibrary face's record (private to bmds_fonts).
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/fontlibrary.h>

BMDS_NAMESPACE_BEGIN

/** @brief One face: what it is, where it comes from, its bytes while loaded. */
struct FontLibrary::Record {
                char                  path[MaxPath + 1] = {}; // "" for a bitmap font
                char                  family[FontName::MaxFamily + 1] = {};
                char                  style[32] = {};
                const BitmapFont     *bitmap = nullptr;
                FontFace             *face = nullptr;  // TrueType: open (null while not loaded)
                uint8_t              *owned = nullptr; // the file read into PSRAM (not for a mapped one)
                size_t                ownedBytes = 0;
                std::atomic<uint32_t> users{0}; // Fonts using it
                uint16_t              weight = 400;
                uint16_t              glyphs = 0;
                uint8_t               bitmapAscent = 0;
                bool                  italic = false;
                bool                  fixedPitch = false;
                bool                  present = true; // its file there at the last scan
                bool                  seen = false;   // during a scan
                bool                  mapped = false; // read in place, open for good
};

BMDS_NAMESPACE_END
