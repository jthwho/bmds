/**
 * @file      types.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "types.h"

namespace preso {

        const char *errorName(Error error) {
                switch (error) {
                        case Error::Ok: return "no error";
                        case Error::NoMemory: return "out of memory";
                        case Error::NotFound: return "not found";
                        case Error::Io: return "input/output error";
                        case Error::TooBig: return "too big";
                        case Error::Invalid: return "not valid";
                        case Error::NotSupported: return "not supported";
                }
                return "unknown error";
        }

        namespace {

                char lower(char c) {
                        return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                }

        } // namespace

        bool StringView::equalsIgnoringCase(StringView other) const {
                if (_length != other._length) return false;
                for (uint32_t i = 0; i < _length; i++) {
                        if (lower(_data[i]) != lower(other._data[i])) return false;
                }
                return true;
        }

        int32_t StringView::indexOf(char c, uint32_t from) const {
                for (uint32_t i = from; i < _length; i++) {
                        if (_data[i] == c) return int32_t(i);
                }
                return -1;
        }

        int32_t StringView::indexOf(StringView text, uint32_t from) const {
                if (text._length == 0) return from <= _length ? int32_t(from) : -1;
                for (uint32_t i = from; i + text._length <= _length; i++) {
                        if (memcmp(_data + i, text._data, text._length) == 0) return int32_t(i);
                }
                return -1;
        }

        StringView StringView::mid(uint32_t start, uint32_t length) const {
                if (start >= _length) return StringView(_data != nullptr ? _data + _length : nullptr, 0);
                uint32_t left = _length - start;
                return StringView(_data + start, length < left ? length : left);
        }

        StringView StringView::trimmed() const {
                uint32_t start = 0;
                uint32_t end = _length;
                while (start < end && isSpace(_data[start])) start++;
                while (end > start && isSpace(_data[end - 1])) end--;
                return StringView(_data + start, end - start);
        }

        int32_t StringView::toInt(int32_t fallback) const {
                if (_length == 0) return fallback;
                uint32_t i = 0;
                bool     negative = false;
                if (_data[0] == '-' || _data[0] == '+') {
                        negative = _data[0] == '-';
                        i = 1;
                }
                if (i == _length) return fallback;
                int64_t value = 0;
                for (; i < _length; i++) {
                        if (_data[i] < '0' || _data[i] > '9') return fallback;
                        value = value * 10 + (_data[i] - '0');
                        if (value > INT32_MAX) return fallback;
                }
                return int32_t(negative ? -value : value);
        }

        float StringView::toFloat(float fallback) const {
                int32_t dot = indexOf('.');
                if (dot < 0) {
                        int32_t whole = toInt(INT32_MIN);
                        return whole == INT32_MIN ? fallback : float(whole);
                }
                StringView wholePart = left(uint32_t(dot));
                StringView fraction = mid(uint32_t(dot) + 1);
                bool       negative = wholePart.startsWith("-");
                int32_t    whole = 0;
                if (!wholePart.isEmpty() && !wholePart.equals("-") && !wholePart.equals("+")) {
                        whole = wholePart.toInt(INT32_MIN);
                        if (whole == INT32_MIN) return fallback;
                }
                if (fraction.isEmpty() && (wholePart.isEmpty() || wholePart.equals("-") || wholePart.equals("+"))) {
                        return fallback;
                }
                float value = float(whole < 0 ? -whole : whole);
                float scale = 0.1f;
                for (uint32_t i = 0; i < fraction.length(); i++) {
                        if (fraction[i] < '0' || fraction[i] > '9') return fallback;
                        value += float(fraction[i] - '0') * scale;
                        scale *= 0.1f;
                }
                return negative ? -value : value;
        }

} // namespace preso
