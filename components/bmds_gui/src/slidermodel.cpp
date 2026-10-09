/**
 * @file      slidermodel.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a slider shows (devplan/gui.md §4s).
 */

#include <bmds/slidermodel.h>

BMDS_NAMESPACE_BEGIN

void SliderModel::setRange(int32_t minimum, int32_t maximum) {
        _min = minimum < maximum ? minimum : maximum;
        _max = minimum < maximum ? maximum : minimum;
        (void)setValue(_value);
        return;
}

bool SliderModel::setValue(int64_t value) {
        const int32_t v = static_cast<int32_t>(value < _min ? _min : value > _max ? _max : value);
        if (v == _value) return false;
        _value = v;
        return true;
}

void SliderModel::setSteps(int32_t step, int32_t page) {
        _step = step > 0 ? step : 1;
        _page = page > 0 ? page : 1;
        return;
}

uint16_t SliderModel::handleStart(uint16_t length, uint16_t handle) const {
        if (length <= handle || _max == _min) return 0;
        const int64_t travel = length - handle;
        const int64_t from = _inverted ? _max - _value : _value - _min;
        // Rounded to the nearest pixel.
        return static_cast<uint16_t>((from * travel + (_max - _min) / 2) / (_max - _min));
}

int32_t SliderModel::valueAt(int start, uint16_t length, uint16_t handle) const {
        if (length <= handle || _max == _min) return _min;
        const int64_t travel = length - handle;
        const int64_t at = start < 0 ? 0 : (start > travel ? travel : start);
        const int64_t from = (at * (_max - _min) + travel / 2) / travel;
        return static_cast<int32_t>(_inverted ? _max - from : _min + from);
}

BMDS_NAMESPACE_END
