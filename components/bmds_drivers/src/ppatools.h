/**
 * @file      ppatools.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What `display ppa`'s checks and the display's benchmarks share (displayppa.cpp,
 * benchdisplay.cpp): the scratch pictures in PSRAM, the cache's write-back and invalidation, and
 * the 565 doubling kernels.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <esp_cache.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memory.h>
#include <bmds/random.h>
#include <bmds/simd.h>

BMDS_NAMESPACE_BEGIN

namespace PpaTools {

        inline constexpr uint16_t     OutWidth = 1280;
        inline constexpr uint16_t     OutHeight = 720;
        inline constexpr unsigned int JobTimeoutMs = 200;

        // The pictures the cases use, PSRAM, freed together.
        struct Pictures {
                        uint8_t *out = nullptr; // OutWidth × OutHeight RGB888
                        uint8_t *src = nullptr; // up to 640 × 480 × 3, or 1280 × 720 × 3 for the copy
                        uint8_t *mid = nullptr; // 320 × 200 RGB888 (expanded, then scaled)
                        size_t   outBytes = size_t(OutWidth) * OutHeight * 3;
                        size_t   srcBytes = size_t(OutWidth) * OutHeight * 3;
                        size_t   midBytes = 320 * 200 * 3;

                        Error allocate() {
                                out = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, outBytes));
                                src = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, srcBytes));
                                mid = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, midBytes));
                                return out != nullptr && src != nullptr && mid != nullptr ? Error(Error::Ok)
                                                                                          : Error(Error::NoMem);
                        }

                        ~Pictures() {
                                Memory::free(out);
                                Memory::free(src);
                                Memory::free(mid);
                        }
        };

        inline void writeBack(const void *data, size_t bytes) {
                (void)esp_cache_msync(const_cast<void *>(data), (bytes + 63) & ~size_t(63),
                                      ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
                return;
        }

        inline void invalidate(const void *data, size_t bytes) {
                (void)esp_cache_msync(const_cast<void *>(data), bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                return;
        }

        inline void fillRandom(uint8_t *data, size_t bytes) {
                Random::fill(data, bytes);
                writeBack(data, bytes);
                return;
        }

        // Each RGB565 pixel of @p in twice into @p out (@p n pixels in).
        inline void double565Scalar(const uint16_t *in, uint16_t *out, size_t n) {
                for (size_t i = 0; i < n; ++i) {
                        const uint32_t v = in[i];
                        reinterpret_cast<uint32_t *>(out)[i] = v | (v << 16);
                }
                return;
        }

#if BMDS_HAS_PIE
        // 8 pixels a step: loaded twice, zipped by 16 bits, stored (both 16-byte aligned, n a multiple of 8).
        inline void double565Pie(const uint16_t *in, uint16_t *out, size_t n) {
                const uint16_t *a = in;
                const uint16_t *b = in;
                uint16_t       *o = out;
                for (size_t i = 0; i < n; i += 8) {
                        asm volatile("esp.vld.128.ip q0, %[a], 16\n"
                                     "esp.vld.128.ip q1, %[b], 16\n"
                                     "esp.vzip.16 q0, q1\n"
                                     "esp.vst.128.ip q0, %[o], 16\n"
                                     "esp.vst.128.ip q1, %[o], 16\n"
                                     : [a] "+cr"(a), [b] "+cr"(b), [o] "+cr"(o)
                                     :
                                     : "memory");
                }
                return;
        }
#endif

} // namespace PpaTools

BMDS_NAMESPACE_END
