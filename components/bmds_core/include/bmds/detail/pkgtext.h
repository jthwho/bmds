/**
 * @file      pkgtext.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The fields package manifests and indexes share (pkgmanifest.cpp, pkgindex.cpp): numbers and
 * hashes, written one way only (no signs, no leading zeros, lowercase hex).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

namespace Detail::PkgText {

        inline bool parseOctal(StringView s, uint32_t max, uint32_t &out) {
                if (s.isEmpty() || s.size() > 6) return false;
                uint32_t n = 0;
                for (char c : s) {
                        if (c < '0' || c > '7') return false;
                        n = n * 8 + uint32_t(c - '0');
                }
                if (n > max) return false;
                out = n;
                return true;
        }

        inline bool parseDecimal(StringView s, uint64_t &out) {
                if (s.isEmpty() || s.size() > 19 || (s.size() > 1 && s[0] == '0')) return false;
                uint64_t n = 0;
                for (char c : s) {
                        if (c < '0' || c > '9') return false;
                        n = n * 10 + uint64_t(c - '0');
                }
                out = n;
                return true;
        }

        inline int hexValue(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
        }

        inline bool parseHash(StringView s, uint8_t (&out)[32]) {
                if (s.size() != 64) return false;
                for (size_t i = 0; i < 32; ++i) {
                        const int hi = hexValue(s[2 * i]);
                        const int lo = hexValue(s[2 * i + 1]);
                        if (hi < 0 || lo < 0) return false;
                        out[i] = uint8_t(hi * 16 + lo);
                }
                return true;
        }

} // namespace Detail::PkgText

BMDS_NAMESPACE_END
