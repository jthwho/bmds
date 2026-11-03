/**
 * @file      buffer.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#ifndef PRESO_BUFFER_H
#define PRESO_BUFFER_H

#include "types.h"

namespace preso {

        /**
         * @brief Bytes built up piece by piece (malloc'd, grown as needed), for text gathered
         *        before it is copied where it lives.
         *
         * An append that runs out of memory leaves the buffer as it was and marks it failed:
         * the builder checks once at the end (`hasFailed()`), not after every piece.
         *
         * @par Thread Safety
         * Not thread-safe: one owner.
         */
        class Buffer {
                public:
                        Buffer() = default;
                        ~Buffer();
                        Buffer(const Buffer &) = delete;
                        Buffer &operator=(const Buffer &) = delete;

                        void append(StringView text);
                        void append(char c) { append(StringView(&c, 1)); }
                        /** @brief Formatted as printf's. */
                        void appendf(const char *format, ...) __attribute__((format(printf, 2, 3)));
                        void clear() { _length = 0; }

                        StringView view() const { return StringView(_data, _length); }
                        uint32_t   length() const { return _length; }
                        bool       isEmpty() const { return _length == 0; }
                        bool       hasFailed() const { return _failed; }

                private:
                        bool reserve(uint32_t length);

                        char    *_data = nullptr;
                        uint32_t _length = 0;
                        uint32_t _capacity = 0;
                        bool     _failed = false;
        };

} // namespace preso

#endif // PRESO_BUFFER_H
