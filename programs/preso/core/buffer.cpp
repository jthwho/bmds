/**
 * @file      buffer.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "buffer.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

namespace preso {

        Buffer::~Buffer() {
                free(_data);
        }

        bool Buffer::reserve(uint32_t length) {
                if (length <= _capacity) return true;
                uint32_t capacity = _capacity < 64 ? 64 : _capacity;
                while (capacity < length) capacity *= 2;
                char *data = static_cast<char *>(realloc(_data, capacity));
                if (data == nullptr) {
                        _failed = true;
                        return false;
                }
                _data = data;
                _capacity = capacity;
                return true;
        }

        void Buffer::append(StringView text) {
                if (text.isEmpty() || !reserve(_length + text.length())) return;
                memcpy(_data + _length, text.data(), text.length());
                _length += text.length();
                return;
        }

        void Buffer::appendf(const char *format, ...) {
                char    small[160];
                va_list args;
                va_start(args, format);
                int n = vsnprintf(small, sizeof small, format, args);
                va_end(args);
                if (n < 0) return;
                if (size_t(n) < sizeof small) {
                        append(StringView(small, uint32_t(n)));
                        return;
                }
                if (!reserve(_length + uint32_t(n) + 1)) return;
                va_start(args, format);
                vsnprintf(_data + _length, size_t(n) + 1, format, args);
                va_end(args);
                _length += uint32_t(n);
                return;
        }

} // namespace preso
