/**
 * @file      sinetone.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cmath>
#include <bmds/sinetone.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr float FadeSeconds = 0.005f;
        constexpr float FullScale = 2147483520.0f; // the largest float below 2^31

} // namespace

void SineTone::start(uint32_t frequencyHz, uint32_t rateHz, int32_t levelCentiDb, Slots slots) {
        _phase = 0;
        _increment = static_cast<uint32_t>((static_cast<uint64_t>(frequencyHz) << 32) / rateHz);
        _amplitude = FullScale * std::pow(10.0f, static_cast<float>(levelCentiDb) / 2000.0f);
        _gain = 0;
        _target = 1;
        _gainStep = 1.0f / (FadeSeconds * static_cast<float>(rateHz));
        _slots = slots;
        return;
}

void SineTone::fadeOut() {
        _target = 0;
        return;
}

void SineTone::fill(int32_t *out, size_t frames) {
        constexpr float Turn = 6.28318530717958647692f / 4294967296.0f;
        for (size_t i = 0; i < frames; ++i) {
                if (_gain < _target) {
                        _gain = std::fmin(_target, _gain + _gainStep);
                } else if (_gain > _target) {
                        _gain = std::fmax(_target, _gain - _gainStep);
                }
                const float   s = _amplitude * _gain * std::sin(static_cast<float>(_phase) * Turn);
                const int32_t v = static_cast<int32_t>(s);
                out[2 * i] = (_slots & Left) != 0 ? v : 0;
                out[2 * i + 1] = (_slots & Right) != 0 ? v : 0;
                _phase += _increment;
        }
        return;
}

BMDS_NAMESPACE_END
