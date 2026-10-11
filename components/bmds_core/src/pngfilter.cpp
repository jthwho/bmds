/**
 * @file      pngfilter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * PNG's row filters for writing (devplan/simd.md step 5): scored, then applied.
 */

#include <cstring>
#include <bmds/pngfilter.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A byte's size as PNG's heuristic reads it: as signed, its absolute value.
        uint32_t magnitude(uint8_t v) {
                const int s = static_cast<int8_t>(v);
                return static_cast<uint32_t>(s < 0 ? -s : s);
        }

        struct Scores {
                        uint32_t none = 0, sub = 0, up = 0;
        };

        void scoreScalar(const uint8_t *cur, const uint8_t *prev, size_t from, size_t bytes, Scores &s) {
                for (size_t i = from; i < bytes; ++i) {
                        const uint8_t c = cur[i];
                        s.none += magnitude(c);
                        s.sub += magnitude(static_cast<uint8_t>(c - cur[int(i) - 3])); // cur[-3..-1] are 0
                        s.up += magnitude(static_cast<uint8_t>(c - prev[i]));
                }
                return;
        }

        enum Filter : uint8_t {
                None = 0,
                Sub = 1,
                Up = 2
        };

        void applyScalar(Filter f, const uint8_t *cur, const uint8_t *prev, size_t from, size_t bytes, uint8_t *row) {
                for (size_t i = from; i < bytes; ++i)
                        row[i] = f == Up ? static_cast<uint8_t>(cur[i] - prev[i])
                                         : static_cast<uint8_t>(cur[i] - cur[int(i) - 3]);
                return;
        }

#if BMDS_HAS_PIE
        // A vector's 16 sizes summed into XACC: |a − b| as max − min, then
        // min(d, 256 − d) (the signed reading's absolute value), × 1 into XACC. q7: all ones.
        // Expects CFG's misaligned loads on.
        uint32_t sumPie(const uint8_t *a, const uint8_t *b, size_t chunks) {
                uint32_t sum;
                asm volatile("esp.zero.xacc\n"
                             "1: esp.vld.128.ip q0, %[a], 16\n"
                             "esp.vld.128.ip q1, %[b], 16\n"
                             "esp.vmax.u8 q2, q0, q1\n"
                             "esp.vmin.u8 q3, q0, q1\n"
                             "esp.vsub.u8 q2, q2, q3\n" // |a − b|: max − min
                             "esp.notq q3, q2\n"
                             "esp.vadd.u8 q3, q3, q7\n" // 256 − d (d = 0: 0, any min keeps it)
                             "esp.vmin.u8 q2, q2, q3\n"
                             "addi %[n], %[n], -1\n"
                             "esp.vmulas.u8.xacc q2, q7\n"
                             "bnez %[n], 1b\n"
                             "esp.movx.r.xacc.l %[sum]\n"
                             : [a] "+cr"(a), [b] "+cr"(b), [n] "+r"(chunks), [sum] "=cr"(sum)
                             :
                             : "memory");
                return sum;
        }

        // cur − ref, 16 bytes a step, wrapping. PIE's vsub saturates, so: d = max − min (exact), kept
        // where cur ≥ ref, 256 − d elsewhere (~d + 1: d ≥ 1 there, no saturation), by a compare's
        // mask. q7: all ones (bytes of 1).
        void subtractPie(const uint8_t *cur, const uint8_t *ref, uint8_t *out, size_t chunks) {
                asm volatile("1: esp.vld.128.ip q0, %[c], 16\n"
                             "esp.vld.128.ip q1, %[r], 16\n"
                             "esp.vmax.u8 q2, q0, q1\n"
                             "esp.vmin.u8 q3, q0, q1\n"
                             "esp.vcmp.eq.u8 q4, q2, q0\n" // cur ≥ ref
                             "esp.vsub.u8 q2, q2, q3\n"    // d
                             "esp.notq q3, q2\n"
                             "esp.vadd.u8 q3, q3, q7\n" // 256 − d
                             "esp.andq q2, q2, q4\n"
                             "esp.notq q4, q4\n"
                             "esp.andq q3, q3, q4\n"
                             "esp.orq q2, q2, q3\n"
                             "addi %[n], %[n], -1\n"
                             "esp.vst.128.ip q2, %[o], 16\n"
                             "bnez %[n], 1b\n"
                             : [c] "+cr"(cur), [r] "+cr"(ref), [o] "+cr"(out), [n] "+r"(chunks)
                             :
                             : "memory");
                return;
        }
#endif

} // namespace

void PngFilter::row(const uint8_t *cur, const uint8_t *prev, size_t bytes, uint8_t *out, Simd::Kernel kernel) {
        uint8_t *row = out + 1;
        if (std::memcmp(cur, prev, bytes) == 0) {
                out[0] = Up;
                std::memset(row, 0, bytes);
                return;
        }
        Scores       s;
        size_t       done = 0;
        const bool   pie = Simd::usable(kernel) == Simd::Kernel::Pie && bytes >= 16;
        const size_t chunks = bytes / 16;
#if BMDS_HAS_PIE
        static const uint8_t Zeros[16] __attribute__((aligned(16))) = {};
        static const uint8_t Ones[16] __attribute__((aligned(16))) = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        uint32_t             cfg = 0;
        if (pie) {
                const uint8_t *op = Ones;
                asm volatile("esp.movx.r.cfg %0" : "=cr"(cfg));
                asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg | 3u)); // misaligned loads and stores
                asm volatile("esp.vld.128.ip q7, %0, 0" ::"cr"(op) : "memory");
                // None: against zeros (the same 16 each step: a zero row of the chunk's length is read).
                uint32_t none = 0;
                for (size_t c = 0; c < chunks; ++c) none += sumPie(cur + 16 * c, Zeros, 1);
                s.none = none;
                s.sub = sumPie(cur, cur - 3, chunks);
                s.up = sumPie(cur, prev, chunks);
                done = chunks * 16;
        }
#endif
        scoreScalar(cur, prev, done, bytes, s);
        const Filter f = s.up <= s.sub && s.up <= s.none ? Up : s.sub < s.none ? Sub : None;
        out[0] = f;
        if (f == None) {
                std::memcpy(row, cur, bytes);
        } else {
#if BMDS_HAS_PIE
                if (pie) subtractPie(cur, f == Up ? prev : cur - 3, row, chunks);
#endif
                applyScalar(f, cur, prev, pie ? chunks * 16 : 0, bytes, row);
        }
#if BMDS_HAS_PIE
        if (pie) asm volatile("esp.movx.w.cfg %0" ::"cr"(cfg));
#endif
        return;
}

BMDS_NAMESPACE_END
