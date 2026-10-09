/**
 * @file      resampler.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Bandlimited interpolation (J. O. Smith, "Digital Audio Resampling Home
 * Page", ccrma.stanford.edu/~jos/resample): each output is the input
 * convolved with a windowed sinc centred on the output's time. The sinc is
 * one table, made here at compile time (devplan/audio.md §17).
 */

#include <array>
#include <cmath>
#include <cstring>
#include <bmds/memory.h>
#include <bmds/resampler.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned Crossings = Resampler::ZeroCrossings;
        constexpr unsigned Density = Resampler::TableDensity;
        constexpr unsigned TableEnd = Crossings * Density; // the last entry, at the window's edge

        // The filter: chosen by the host tests' measurements (tests/host/audio.cpp).
        constexpr double Rolloff = 0.90; // the cutoff, as a fraction of the lower Nyquist frequency
        constexpr double Beta = 8.0;     // the Kaiser window's (about 80 dB of stopband)

        constexpr double Pi = 3.14159265358979323846;

        // constexpr maths: only for the table.
        constexpr double sinOf(double x) {
                while (x > Pi) x -= 2 * Pi;
                while (x < -Pi) x += 2 * Pi;
                double term = x;
                double sum = x;
                for (int n = 1; n < 40; ++n) {
                        term *= -x * x / ((2.0 * n) * (2.0 * n + 1));
                        if (term == 0.0) break;
                        sum += term;
                }
                return sum;
        }

        constexpr double sqrtOf(double x) {
                if (x <= 0) return 0;
                double r = x < 1 ? 1 : x;
                for (int i = 0; i < 100; ++i) {
                        const double next = 0.5 * (r + x / r);
                        if (next == r) break;
                        r = next;
                }
                return r;
        }

        // The zeroth-order modified Bessel function of the first kind.
        constexpr double besselI0(double x) {
                double sum = 1;
                double term = 1;
                for (int k = 1; k < 100; ++k) {
                        term *= (x / (2.0 * k)) * (x / (2.0 * k));
                        sum += term;
                        if (term < sum * 1e-17) break;
                }
                return sum;
        }

        // h(t) for t = 0 to Crossings input frames, Density entries a frame, stored by phase:
        // entry [r][c] is h(c + r / Density), r = 0 to Density (row Density is row 0 of the next
        // crossing, for the interpolation; past the last crossing, 0). An output's taps on one side
        // share a phase, so they are one contiguous row.
        constexpr size_t Rows = Density + 1;

        constexpr std::array<float, Rows * Crossings> makeTable() {
                std::array<float, Rows * Crossings> t{};
                const double                        i0Beta = besselI0(Beta);
                for (unsigned r = 0; r < Rows; ++r) {
                        for (unsigned c = 0; c < Crossings; ++c) {
                                const unsigned l = c * Density + r;
                                if (l >= TableEnd) continue; // the window's edge and past it: 0
                                const double x = static_cast<double>(l) / Density;
                                const double arg = Pi * Rolloff * x;
                                const double sinc = l == 0 ? 1.0 : sinOf(arg) / arg;
                                const double q = x / Crossings;
                                const double window = besselI0(Beta * sqrtOf(1 - q * q)) / i0Beta;
                                t[r * Crossings + c] = static_cast<float>(Rolloff * sinc * window);
                        }
                }
                return t;
        }

        constexpr std::array<float, Rows * Crossings> Table = makeTable();

        // The taps at distance (@p p + c·Density table entries) for c from 0, interpolated between
        // rows: one side of an output going up (or at the same rate), where every tap shares the
        // phase. @p x steps @p dir frames a tap.
        inline void sameStep(const float *x, ptrdiff_t dir, float p, float &left, float &right) {
                auto     row = static_cast<unsigned>(p);
                unsigned c = 0;
                if (row >= Density) { // exactly one frame away (the right side of a whole position)
                        row -= Density;
                        c = 1;
                }
                const float  a = p - static_cast<float>(row + c * Density);
                const float *h0 = &Table[row * Crossings];
                const float *h1 = h0 + Crossings;
                // Two sums a channel, so each multiply-add needn't wait for the one before.
                float l0 = 0, l1 = 0, r0 = 0, r1 = 0;
                if (c == 1) {
                        const float h = h0[1] + a * (h1[1] - h0[1]);
                        l0 = h * x[0];
                        r0 = h * x[1];
                        x += 2 * dir;
                        c = 2;
                }
                const ptrdiff_t next = 2 * dir;
                for (; c < Crossings; c += 2, x += 4 * dir) {
                        const float ha = h0[c] + a * (h1[c] - h0[c]);
                        const float hb = h0[c + 1] + a * (h1[c + 1] - h0[c + 1]);
                        l0 += ha * x[0];
                        r0 += ha * x[1];
                        l1 += hb * x[next];
                        r1 += hb * x[next + 1];
                }
                left += l0 + l1;
                right += r0 + r1;
                return;
        }

        // The same going down: the kernel stretched, so each tap has its own phase.
        inline void stretched(const float *x, ptrdiff_t dir, float p, float step, float &left, float &right) {
                for (;; x += 2 * dir, p += step) {
                        const auto idx = static_cast<unsigned>(p);
                        if (idx >= TableEnd) break;
                        const unsigned row = idx % Density;
                        const unsigned c = idx / Density;
                        const float    a = p - static_cast<float>(idx);
                        const float    h0 = Table[row * Crossings + c];
                        const float    h = h0 + a * (Table[(row + 1) * Crossings + c] - h0);
                        left += h * x[0];
                        right += h * x[1];
                }
                return;
        }

        // h at @p p table entries from the centre (0 past the window), as `stretched()` reads it.
        float kernelAt(float p) {
                const auto idx = static_cast<unsigned>(p);
                if (idx >= TableEnd) return 0.0f;
                const unsigned row = idx % Density;
                const unsigned c = idx / Density;
                const float    h0 = Table[row * Crossings + c];
                return h0 + (p - static_cast<float>(idx)) * (Table[(row + 1) * Crossings + c] - h0);
        }

        // One side of an output from a precomputed row of @p taps coefficients.
        inline void rowSide(const float *x, ptrdiff_t dir, const float *h, size_t taps, float &left, float &right) {
                float           l0 = 0, l1 = 0, r0 = 0, r1 = 0;
                const ptrdiff_t next = 2 * dir;
                size_t          k = 0;
                for (; k + 1 < taps; k += 2, x += 4 * dir) {
                        l0 += h[k] * x[0];
                        r0 += h[k] * x[1];
                        l1 += h[k + 1] * x[next];
                        r1 += h[k + 1] * x[next + 1];
                }
                if (k < taps) {
                        l0 += h[k] * x[0];
                        r0 += h[k] * x[1];
                }
                left += l0 + l1;
                right += r0 + r1;
                return;
        }

        uint32_t gcdOf(uint32_t a, uint32_t b) {
                while (b != 0) {
                        const uint32_t t = a % b;
                        a = b;
                        b = t;
                }
                return a;
        }

} // namespace

Resampler::~Resampler() {
        release();
}

void Resampler::release() {
        Memory::free(_buffer);
        _buffer = nullptr;
        Memory::free(_rows);
        _rows = nullptr;
        _capacity = 0;
        _count = 0;
        return;
}

Error Resampler::configure(uint32_t inRate, uint32_t outRate, size_t maxOutFrames, MemSpace space) {
        if (inRate == 0 || outRate == 0) return Error::Invalid;
        release();
        _inRate = inRate;
        _outRate = outRate;
        _stepWhole = inRate / outRate;
        _stepFraction = inRate % outRate;
        _scale = outRate < inRate ? static_cast<float>(outRate) / static_cast<float>(inRate) : 1.0f;
        _tableStep = _scale * static_cast<float>(Density);
        _halfTaps = static_cast<size_t>((static_cast<uint64_t>(Crossings) * inRate + outRate - 1) / outRate);
        if (_halfTaps < Crossings) _halfTaps = Crossings;
        _halfTaps += 1;
        const size_t maxIn =
                static_cast<size_t>((static_cast<uint64_t>(maxOutFrames) * inRate + outRate - 1) / outRate);
        _capacity = 2 * _halfTaps + maxIn + 2;
        _buffer = static_cast<float *>(Memory::alloc(space, _capacity * 2 * sizeof(float)));
        if (_buffer == nullptr) {
                _capacity = 0;
                return Error::NoMem;
        }
        // A rational ratio has out / gcd phases: each one's coefficients worked out now, when
        // they fit, so an output reads one row instead of interpolating every tap.
        _phaseUnit = gcdOf(inRate, outRate);
        const uint32_t phases = outRate / _phaseUnit;
        _side = static_cast<size_t>(std::ceil(static_cast<float>(TableEnd) / _tableStep)); // taps a side
        if (static_cast<size_t>(phases) * 2 * _side <= MaxRowFloats) {
                _rows = static_cast<float *>(
                        Memory::alloc(space, static_cast<size_t>(phases) * 2 * _side * sizeof(float)));
                if (_rows != nullptr) {
                        for (uint32_t ph = 0; ph < phases; ++ph) {
                                const float f = static_cast<float>(ph) * static_cast<float>(_phaseUnit) /
                                                static_cast<float>(outRate);
                                float *row = _rows + static_cast<size_t>(ph) * 2 * _side;
                                for (size_t k = 0; k < _side; ++k) {
                                        row[k] = kernelAt((f + static_cast<float>(k)) * _tableStep);
                                        row[_side + k] = kernelAt((1.0f - f + static_cast<float>(k)) * _tableStep);
                                }
                        }
                }
        }
        reset();
        return Error::Ok;
}

void Resampler::reset() {
        if (_buffer == nullptr) return;
        std::memset(_buffer, 0, _halfTaps * 2 * sizeof(float));
        _count = _halfTaps;
        _position = _halfTaps;
        _fraction = 0;
        return;
}

size_t Resampler::inputNeeded(size_t outFrames) const {
        if (outFrames == 0 || _buffer == nullptr) return 0;
        const uint64_t ahead = (_fraction + static_cast<uint64_t>(outFrames - 1) * _inRate) / _outRate;
        const uint64_t needed = _position + ahead + _halfTaps + 1;
        return needed > _count ? static_cast<size_t>(needed - _count) : 0;
}

float *Resampler::inputSpace(size_t frames) {
        if (_buffer == nullptr || _count + frames > _capacity) return nullptr;
        return _buffer + 2 * _count;
}

void Resampler::commit(size_t frames) {
        _count += frames;
        if (_count > _capacity) _count = _capacity;
        return;
}

size_t Resampler::process(float *mix, size_t outFrames, float gain) {
        if (_buffer == nullptr) return 0;
        const float invOut = 1.0f / static_cast<float>(_outRate);
        const float g = gain * _scale;
        size_t      made = 0;
        for (; made < outFrames; ++made) {
                if (_position + _halfTaps >= _count) break; // the input ran short
                const float  f = static_cast<float>(_fraction) * invOut;
                const float *x = _buffer + 2 * _position;
                float        left = 0;
                float        right = 0;
                if (_rows != nullptr) {
                        const float *row = _rows + static_cast<size_t>(_fraction / _phaseUnit) * 2 * _side;
                        rowSide(x, -1, row, _side, left, right);            // this frame and before
                        rowSide(x + 2, 1, row + _side, _side, left, right); // after
                } else if (_scale == 1.0f) {
                        sameStep(x, -1, f * Density, left, right);             // this frame and before
                        sameStep(x + 2, 1, (1.0f - f) * Density, left, right); // after
                } else {
                        stretched(x, -1, f * _tableStep, _tableStep, left, right);
                        stretched(x + 2, 1, (1.0f - f) * _tableStep, _tableStep, left, right);
                }
                mix[2 * made] += g * left;
                mix[2 * made + 1] += g * right;
                _position += _stepWhole;
                _fraction += _stepFraction;
                if (_fraction >= _outRate) {
                        _fraction -= _outRate;
                        ++_position;
                }
        }
        // Keep _halfTaps frames before the position: the next output's history.
        if (_position > _halfTaps) {
                const size_t drop = _position - _halfTaps;
                const size_t keep = _count > drop ? _count - drop : 0;
                std::memmove(_buffer, _buffer + 2 * drop, keep * 2 * sizeof(float));
                _count = keep;
                _position -= drop;
        }
        return made;
}

BMDS_NAMESPACE_END
