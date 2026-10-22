/**
 * @file      util.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Text (a growable buffer) and script exceptions raised from C++.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "angel.h"

namespace angel {

        Text::~Text() {
                free(_data);
        }

        bool Text::reserve(size_t length) {
                if (length + 1 <= _capacity) return true;
                size_t capacity = _capacity != 0 ? _capacity : 64;
                while (capacity < length + 1) capacity *= 2;
                char *grown = static_cast<char *>(realloc(_data, capacity));
                if (grown == nullptr) return false;
                _data = grown;
                _capacity = capacity;
                return true;
        }

        void Text::append(const char *data, size_t length) {
                if (length == 0 || !reserve(_length + length)) return;
                memcpy(_data + _length, data, length);
                _length += length;
                _data[_length] = 0;
                return;
        }

        void Text::append(const char *text) {
                append(text, strlen(text));
                return;
        }

        void Text::appendf(const char *format, ...) {
                va_list args;
                va_start(args, format);
                char      small[128];
                const int n = vsnprintf(small, sizeof(small), format, args);
                va_end(args);
                if (n < 0) return;
                if (size_t(n) < sizeof(small)) {
                        append(small, size_t(n));
                        return;
                }
                if (!reserve(_length + size_t(n))) return;
                va_start(args, format);
                vsnprintf(_data + _length, size_t(n) + 1, format, args);
                va_end(args);
                _length += size_t(n);
                return;
        }

        char *Text::release() {
                char *data = _data;
                _data = nullptr;
                _length = 0;
                _capacity = 0;
                return data;
        }

        void raise(const char *format, ...) {
                char    message[256];
                va_list args;
                va_start(args, format);
                vsnprintf(message, sizeof(message), format, args);
                va_end(args);
                asIScriptContext *ctx = asGetActiveContext();
                if (ctx != nullptr) ctx->SetException(message);
                return;
        }

} // namespace angel
