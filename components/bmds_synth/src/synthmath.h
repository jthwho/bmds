/**
 * @file      synthmath.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Single-precision approximations for TinySoundFont's per-block math
 * (devplan/audio.md §22): musl's powf, expf and tanf compute in double,
 * which the ESP32-P4 does in software, and they were most of a voice's cost.
 * Relative error about 1e-6. Included by tsf.c before tsf.h.
 */

#pragma once

#include <math.h>

/* 2^x: the integer part into the exponent, a degree-6 polynomial for the fraction. */
static inline float bmdsSynthExp2(float x) {
        if (x < -126.0f) return 0.0f;
        if (x > 127.0f) x = 127.0f;
        int i = (int)x;
        if ((float)i > x) --i;
        const float f = x - (float)i;
        const float p =
                1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
        union {
                        float    f;
                        unsigned u;
        } s;
        s.u = (unsigned)(i + 127) << 23;
        return p * s.f;
}

/* log2(x), x > 0: the exponent, and atanh's series for the mantissa (1 to 2). */
static inline float bmdsSynthLog2(float x) {
        union {
                        float    f;
                        unsigned u;
        } v;
        v.f = x;
        const int e = (int)((v.u >> 23) & 0xff) - 127;
        v.u = (v.u & 0x7fffffu) | 0x3f800000u;
        const float t = (v.f - 1.0f) / (v.f + 1.0f), t2 = t * t;
        return (float)e +
               2.88539008f * t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * (0.14285714f + t2 * 0.11111111f))));
}

static inline float bmdsSynthPowf(float a, float b) {
        return a <= 0.0f ? 0.0f : bmdsSynthExp2(b * bmdsSynthLog2(a));
}

static inline float bmdsSynthExpf(float x) {
        return bmdsSynthExp2(x * 1.44269504f);
}

/* tan(x) on [0, pi/2): sine over cosine, each a polynomial (the filter's cutoff, below Nyquist). */
static inline float bmdsSynthTanf(float x) {
        const float x2 = x * x;
        const float s =
                x * (1.0f - x2 * (0.16666667f - x2 * (0.0083333310f - x2 * (0.00019840874f - x2 * 2.7525562e-06f))));
        const float c =
                1.0f -
                x2 * (0.5f - x2 * (0.041666638f - x2 * (0.0013888378f - x2 * (2.4760495e-05f - x2 * 2.6051615e-07f))));
        return s / c;
}

/* The double-precision hooks (TSF_POW, TSF_LOG, TSF_LOG10): a note-on's setup (its pitch ratio,
   gain and filter Q) called them several times a region, each in software: a single note's first
   block took 664 µs on the board. Computed in single precision (pitch error about 0.002 cents). */
static inline double bmdsSynthPow(double a, double b) {
        return (double)bmdsSynthPowf((float)a, (float)b);
}

static inline double bmdsSynthLog(double x) {
        return x <= 0.0 ? -INFINITY : (double)(bmdsSynthLog2((float)x) * 0.69314718f);
}

static inline double bmdsSynthLog10(double x) {
        return x <= 0.0 ? -INFINITY : (double)(bmdsSynthLog2((float)x) * 0.30103000f);
}
