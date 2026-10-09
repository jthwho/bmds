/**
 * @file      structcopy.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ABI's structs across the call boundary (docs/ABI.md): each starts
 * with its `uint32_t size`, so programs built against an older SDK pass
 * shorter ones. Private to the loader's system calls.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <bmds/errno.h>
#include <bmds/stringview.h>

namespace bmds::detail {

        // Copies the caller's struct (its `size` first) into a zeroed full one.
        template <typename T> int32_t copyIn(const T *in, T &full) {
                full = T{};
                if (in == nullptr) return -BMDS_EFAULT;
                const uint32_t size = in->size;
                if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                std::memcpy(&full, in, size < sizeof(T) ? size : sizeof(T));
                return 0;
        }

        // Copies the fields of @p full that fit in the caller's struct, leaving its `size`.
        template <typename T> int32_t copyOut(const T &full, T *out) {
                if (out == nullptr) return -BMDS_EFAULT;
                const uint32_t size = out->size;
                if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                const size_t n = size < sizeof(T) ? size : sizeof(T);
                std::memcpy(reinterpret_cast<uint8_t *>(out) + sizeof(uint32_t),
                            reinterpret_cast<const uint8_t *>(&full) + sizeof(uint32_t), n - sizeof(uint32_t));
                return 0;
        }

        // Copies @p text into @p out (@p size bytes with the NUL), cut at a UTF-8 character's end.
        inline void copyText(StringView text, char *out, uint32_t size) {
                if (out == nullptr || size == 0) return;
                size_t n = text.size() < size - 1 ? text.size() : size - 1;
                if (n < text.size())
                        while (n > 0 && (static_cast<uint8_t>(text[n]) & 0xc0) == 0x80) --n;
                std::memcpy(out, text.data(), n);
                out[n] = '\0';
                return;
        }

} // namespace bmds::detail
