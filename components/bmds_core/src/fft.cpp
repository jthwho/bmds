/**
 * @file      fft.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/fft.h>

#include <cmath>

BMDS_NAMESPACE_BEGIN

Error Fft::init(size_t points, MemSpace space) {
        if (points < MinPoints || points > MaxPoints || (points & (points - 1)) != 0) return Error::Invalid;
        List<float>    twiddles(space);
        List<uint16_t> reversed(space);
        const size_t   half = points / 2;
        if (twiddles.reserve(points).isError() || reversed.reserve(half).isError()) return Error::NoMem;
        // Once, so in double: the tables as exact as a float holds.
        for (size_t k = 0; k < half; ++k) {
                const double a = 2.0 * M_PI * static_cast<double>(k) / static_cast<double>(points);
                (void)twiddles.append(static_cast<float>(std::cos(a))); // reserved
                (void)twiddles.append(static_cast<float>(-std::sin(a)));
        }
        unsigned bits = 0;
        while ((size_t{1} << bits) < half) ++bits;
        for (size_t i = 0; i < half; ++i) {
                size_t r = 0;
                for (unsigned b = 0; b < bits; ++b)
                        if ((i >> b) & 1) r |= size_t{1} << (bits - 1 - b);
                (void)reversed.append(static_cast<uint16_t>(r));
        }
        _twiddles = std::move(twiddles);
        _reversed = std::move(reversed);
        _points = points;
        return Error::Ok;
}

void Fft::forward(float *data) const {
        const size_t n = _points;
        const size_t m = n / 2; // complex points
        const float *w = _twiddles.data();
        // The samples as m complex points (even ones real, odd ones imaginary), bit-reversed.
        for (size_t i = 0; i < m; ++i) {
                const size_t j = _reversed[i];
                if (j > i) {
                        float t = data[2 * i];
                        data[2 * i] = data[2 * j];
                        data[2 * j] = t;
                        t = data[2 * i + 1];
                        data[2 * i + 1] = data[2 * j + 1];
                        data[2 * j + 1] = t;
                }
        }
        for (size_t len = 2; len <= m; len <<= 1) {
                const size_t half = len / 2;
                const size_t step = n / len; // e^(−2πik/len) is twiddle k·n/len
                for (size_t i = 0; i < m; i += len) {
                        for (size_t k = 0; k < half; ++k) {
                                const float wr = w[2 * k * step];
                                const float wi = w[2 * k * step + 1];
                                float      *a = data + 2 * (i + k);
                                float      *b = data + 2 * (i + k + half);
                                const float tr = b[0] * wr - b[1] * wi;
                                const float ti = b[0] * wi + b[1] * wr;
                                b[0] = a[0] - tr;
                                b[1] = a[1] - ti;
                                a[0] += tr;
                                a[1] += ti;
                        }
                }
        }
        // The split: X[k] = E + wᵏ·O and X[m − k] = conj(E − wᵏ·O), where
        // E = (Z[k] + conj Z[m − k]) / 2 and O = (Z[k] − conj Z[m − k]) / 2i.
        const float z0r = data[0];
        const float z0i = data[1];
        data[0] = z0r + z0i;
        data[1] = z0r - z0i;
        for (size_t k = 1; k <= m / 2; ++k) {
                float      *p = data + 2 * k;
                float      *q = data + 2 * (m - k);
                const float er = 0.5f * (p[0] + q[0]);
                const float ei = 0.5f * (p[1] - q[1]);
                const float orr = 0.5f * (p[1] + q[1]);
                const float oi = -0.5f * (p[0] - q[0]);
                const float wr = w[2 * k];
                const float wi = w[2 * k + 1];
                const float tr = wr * orr - wi * oi;
                const float ti = wr * oi + wi * orr;
                p[0] = er + tr;
                p[1] = ei + ti;
                if (q != p) {
                        q[0] = er - tr;
                        q[1] = -(ei - ti);
                }
        }
        return;
}

void Fft::power(const float *packed, size_t points, float *out) {
        out[0] = packed[0] * packed[0];
        out[points / 2] = packed[1] * packed[1];
        for (size_t k = 1; k < points / 2; ++k)
                out[k] = packed[2 * k] * packed[2 * k] + packed[2 * k + 1] * packed[2 * k + 1];
        return;
}

BMDS_NAMESPACE_END
