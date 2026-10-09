/**
 * @file      spectrum.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "spectrum.h"

#include <cmath>

BMDS_NAMESPACE_BEGIN

Error Spectrum::init(size_t points, uint32_t rateHz, MemSpace space) {
        if (rateHz == 0) return Error::Invalid;
        Error err = _fft.init(points, space);
        if (err.isError()) return err;
        List<float>    window(space);
        List<uint16_t> edges(space);
        if (window.reserve(points).isError() || edges.reserve(2 * Bands).isError()) return Error::NoMem;
        // A periodic Hann window: its coherent gain 1/2, its noise bandwidth 1.5 bins.
        for (size_t i = 0; i < points; ++i)
                (void)window.append(static_cast<float>(
                        0.5 - 0.5 * std::cos(2.0 * M_PI * static_cast<double>(i) / static_cast<double>(points))));
        const double binHz = static_cast<double>(rateHz) / static_cast<double>(points);
        const size_t lastBin = points / 2; // Nyquist
        const auto   binOf = [&](double hz) {
                size_t b = static_cast<size_t>(std::lround(hz / binHz));
                if (b < 1) b = 1;
                if (b > lastBin) b = lastBin;
                return b;
        };
        for (size_t i = 0; i < Bands; ++i) {
                const double low =
                        LowHz * std::pow(static_cast<double>(HighHz) / LowHz, static_cast<double>(i) / Bands);
                const double high =
                        LowHz * std::pow(static_cast<double>(HighHz) / LowHz, static_cast<double>(i + 1) / Bands);
                size_t first = binOf(low);
                size_t end = binOf(high);
                if (end <= first) end = first + 1;
                if (end > lastBin + 1) end = lastBin + 1;
                if (first >= end) first = end - 1;
                (void)edges.append(static_cast<uint16_t>(first)); // reserved
                (void)edges.append(static_cast<uint16_t>(end));
        }
        _window = std::move(window);
        _edges = std::move(edges);
        // A full-scale sine: a peak of N/2 times the window's gain of 1/2, its power spread
        // over the window's 1.5 bins.
        const float peak = static_cast<float>(points) / 4;
        _reference = peak * peak * 1.5f;
        return Error::Ok;
}

void Spectrum::analyse(const float *samples, float *work, float *db) const {
        const size_t n = _fft.points();
        const float *w = _window.data();
        for (size_t i = 0; i < n; ++i) work[i] = samples[i] * w[i];
        _fft.forward(work);
        const float floorPower = _reference * std::pow(10.0f, FloorDb / 10);
        for (size_t b = 0; b < Bands; ++b) {
                float power = 0;
                for (size_t k = _edges[2 * b]; k < _edges[2 * b + 1]; ++k) {
                        if (k == n / 2) {
                                power += work[1] * work[1];
                        } else {
                                power += work[2 * k] * work[2 * k] + work[2 * k + 1] * work[2 * k + 1];
                        }
                }
                db[b] = power <= floorPower ? FloorDb : 10 * std::log10(power / _reference);
        }
        return;
}

MeterBars::MeterBars(size_t bars, float floorDb) : _bars(bars < MaxBars ? bars : MaxBars), _floor(floorDb) {
        reset();
}

void MeterBars::reset() {
        for (size_t i = 0; i < MaxBars; ++i) {
                _level[i] = _floor;
                _peak[i] = _floor;
                _held[i] = 0;
        }
        return;
}

void MeterBars::fall(uint32_t elapsedMs) {
        const float drop = FallDbPerSecond * static_cast<float>(elapsedMs) / 1000;
        for (size_t i = 0; i < _bars; ++i) {
                _level[i] = _level[i] - drop > _floor ? _level[i] - drop : _floor;
                // The mark holds, then falls from where it was for the time past the hold.
                const uint32_t held = _held[i] + elapsedMs;
                if (held > HoldMs) {
                        const uint32_t falling = held - HoldMs < elapsedMs ? held - HoldMs : elapsedMs;
                        const float    markDrop = FallDbPerSecond * static_cast<float>(falling) / 1000;
                        _peak[i] = _peak[i] - markDrop > _floor ? _peak[i] - markDrop : _floor;
                }
                _held[i] = held < HoldMs + 60000 ? held : HoldMs + 60000; // no wrap, however long
                if (_peak[i] < _level[i]) _peak[i] = _level[i];
        }
        return;
}

void MeterBars::update(const float *db, uint32_t elapsedMs) {
        fall(elapsedMs);
        for (size_t i = 0; i < _bars; ++i) {
                const float v = db[i] > _floor ? db[i] : _floor;
                if (v > _level[i]) _level[i] = v;
                if (v >= _peak[i]) {
                        _peak[i] = v;
                        _held[i] = 0;
                }
        }
        return;
}

bool MeterBars::isAtFloor() const {
        for (size_t i = 0; i < _bars; ++i)
                if (_level[i] > _floor || _peak[i] > _floor) return false;
        return true;
}

BMDS_NAMESPACE_END
