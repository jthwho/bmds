/**
 * @file      staticobject.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <new>
#include <utility>
#include <bmds/assert.h>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Storage for one kernel-lifetime object, constructed explicitly at boot.
 *
 * A `StaticObject<T>` at namespace scope is constant-initialized (it is
 * just aligned bytes in BSS), so it costs no global constructor and has no
 * initialization-order problems. The object is built by `create()` from a
 * boot stage and is never destroyed.
 *
 * @code
 * namespace { constinit StaticObject<UsbSerialConsole> console; }
 * Error UsbSerialConsole::init() { console.create(); ... }
 * @endcode
 *
 * @tparam T The object type.
 *
 * @par Thread Safety
 * `create()` is not thread-safe: call it once, from the boot sequence,
 * before any other task can use the object. Access afterwards is as
 * thread-safe as `T` itself.
 */
template <typename T> class StaticObject {
        public:
                /** @brief Constant-initializes empty storage. */
                constexpr StaticObject() = default;

                StaticObject(const StaticObject &) = delete;
                StaticObject &operator=(const StaticObject &) = delete;

                /**
                 * @brief Constructs the object in place.
                 * @param args Constructor arguments for `T`.
                 * @return The new object.
                 */
                template <typename... Args> T &create(Args &&...args) {
                        BMDS_ASSERT(!_created);
                        T *obj = ::new (static_cast<void *>(_storage)) T(std::forward<Args>(args)...);
                        _created = true;
                        return *obj;
                }

                /** @brief Returns true once `create()` has run. */
                bool isCreated() const { return _created; }

                /** @brief Returns the object. It must have been created. */
                T &operator*() { return *ptr(); }

                /** @brief Returns the object. It must have been created. */
                T *operator->() { return ptr(); }

                /** @brief Returns the object. It must have been created. */
                T *ptr() {
                        BMDS_ASSERT(_created);
                        return std::launder(reinterpret_cast<T *>(_storage));
                }

        private:
                alignas(T) unsigned char _storage[sizeof(T)] = {};
                bool _created = false;
};

BMDS_NAMESPACE_END
