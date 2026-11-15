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
#include <bmds/programmemory.h>
#include <bmds/stringview.h>

namespace bmds::detail {

        // The calling program's memory when it runs in U-mode (devplan/protection.md §16), else
        // null: its pointers then aren't checked (loader.cpp's).
        ProgramMemory *callerMemory();

        // True if the calling program could read (write) @p bytes at @p p itself: always for an
        // M-mode one; never for null with @p bytes.
        inline bool canRead(const void *p, size_t bytes) {
                if (bytes == 0) return true;
                if (p == nullptr) return false;
                ProgramMemory *m = callerMemory();
                return m == nullptr || m->isAccessible(p, bytes, false);
        }

        inline bool canWrite(void *p, size_t bytes) {
                if (bytes == 0) return true;
                if (p == nullptr) return false;
                ProgramMemory *m = callerMemory();
                return m == nullptr || m->isAccessible(p, bytes, true);
        }

        // The length of the program's string at @p s (at most @p max), or -BMDS_EFAULT, or
        // -BMDS_ENAMETOOLONG past @p max.
        inline int32_t stringLength(const char *s, size_t max) {
                if (s == nullptr) return -BMDS_EFAULT;
                ProgramMemory *m = callerMemory();
                if (m == nullptr) {
                        const size_t n = strnlen(s, max + 1);
                        return n > max ? -BMDS_ENAMETOOLONG : int32_t(n);
                }
                auto [n, err] = m->stringLength(s, max);
                if (err == Error::OutOfRange) return -BMDS_ENAMETOOLONG;
                if (err.isError()) return -BMDS_EFAULT;
                return int32_t(n);
        }

        // Copies the caller's struct (its `size` first) into a zeroed full one.
        template <typename T> int32_t copyIn(const T *in, T &full) {
                full = T{};
                if (in == nullptr || !canRead(in, sizeof(uint32_t))) return -BMDS_EFAULT;
                const uint32_t size = in->size;
                if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                const size_t n = size < sizeof(T) ? size : sizeof(T);
                if (!canRead(in, n)) return -BMDS_EFAULT;
                std::memcpy(&full, in, n);
                full.size = size; // as read once
                return 0;
        }

        // Copies the fields of @p full that fit in the caller's struct, leaving its `size`.
        template <typename T> int32_t copyOut(const T &full, T *out) {
                if (out == nullptr || !canRead(out, sizeof(uint32_t))) return -BMDS_EFAULT;
                const uint32_t size = out->size;
                if (size < sizeof(uint32_t)) return -BMDS_EINVAL;
                const size_t n = size < sizeof(T) ? size : sizeof(T);
                if (!canWrite(out, n)) return -BMDS_EFAULT;
                std::memcpy(reinterpret_cast<uint8_t *>(out) + sizeof(uint32_t),
                            reinterpret_cast<const uint8_t *>(&full) + sizeof(uint32_t), n - sizeof(uint32_t));
                return 0;
        }

        // Copies @p text into @p out (@p size bytes with the NUL), cut at a UTF-8 character's end.
        // Into the kernel's copy of a struct, or a program's buffer its slot has checked.
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
