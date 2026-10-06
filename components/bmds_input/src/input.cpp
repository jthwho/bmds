/**
 * @file      input.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Where keyboards' and mice's events go (devplan/input.md §4).
 */

#include <bmds/input.h>
#include <bmds/mutex.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Held while an event is delivered, so setSink() waits for it.
        constinit StaticObject<Mutex> lock;
        constinit Input::Sink         current;

} // namespace

Error Input::init() {
        if (!lock.isCreated()) lock.create();
        return Error::Ok;
}

void Input::setSink(const Sink *sink) {
        Mutex::Locker locker(*lock, WaitForever);
        current = sink != nullptr ? *sink : Sink();
        return;
}

void Input::key(const KeyEvent &event) {
        Mutex::Locker locker(*lock, WaitForever);
        if (current.key != nullptr) current.key(current.arg, event);
        return;
}

void Input::pointer(const PointerEvent &event) {
        Mutex::Locker locker(*lock, WaitForever);
        if (current.pointer != nullptr) current.pointer(current.arg, event);
        return;
}

BMDS_NAMESPACE_END
