/**
 * @file      psramobject.h
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
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A kernel-lifetime object kept in `Psram`, made explicitly at boot.
 *
 * `StaticObject`'s counterpart for an object that needn't take internal RAM
 * (devplan/foundation.md §9): at namespace scope it is a pointer in BSS, so
 * it costs no global constructor, and `create()` makes the object in PSRAM.
 * Nothing an interrupt or a cache-disabled path touches belongs here. Never
 * destroyed.
 *
 * @code
 * namespace { constinit PsramObject<Names> names; }
 * Error NetNames::init() { if (names.create() == nullptr) return Error::NoMem; ... }
 * @endcode
 *
 * @tparam T The object type.
 *
 * @par Thread Safety
 * `create()` is not thread-safe: call it once, from the boot sequence,
 * before any other task can use the object. Access afterwards is as
 * thread-safe as `T` itself.
 */
template <typename T> class PsramObject {
        public:
                /** @brief Constant-initializes an empty pointer. */
                constexpr PsramObject() = default;

                PsramObject(const PsramObject &) = delete;
                PsramObject &operator=(const PsramObject &) = delete;

                /**
                 * @brief Makes the object in PSRAM.
                 * @param args Constructor arguments for `T`.
                 * @return The new object, or null when PSRAM is out.
                 */
                template <typename... Args> T *create(Args &&...args) {
                        BMDS_ASSERT(_object == nullptr);
                        void *mem = Memory::allocAligned(MemSpace::Psram, sizeof(T), alignof(T));
                        if (mem == nullptr) return nullptr;
                        _object = ::new (mem) T(std::forward<Args>(args)...);
                        return _object;
                }

                /** @brief Returns true once `create()` has made it. */
                bool isCreated() const { return _object != nullptr; }

                /** @brief Returns the object. It must have been created. */
                T &operator*() { return *ptr(); }

                /** @brief Returns the object. It must have been created. */
                T *operator->() { return ptr(); }

                /** @brief Returns the object. It must have been created. */
                T *ptr() {
                        BMDS_ASSERT(_object != nullptr);
                        return _object;
                }

        private:
                T *_object = nullptr;
};

BMDS_NAMESPACE_END
