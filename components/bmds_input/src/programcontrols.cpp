/**
 * @file      programcontrols.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program's actions, shared by its process and surfaces (devplan/controls.md §7c).
 */

#include <new>
#include <bmds/memory.h>
#include <bmds/programcontrols.h>

BMDS_NAMESPACE_BEGIN

namespace {

        void nowhere(void *, const Controls::Output &) {
                return;
        }

} // namespace

ProgramControls *ProgramControls::create() {
        void *mem = Memory::allocAligned(MemSpace::Psram, sizeof(ProgramControls), alignof(ProgramControls));
        return mem != nullptr ? ::new (mem) ProgramControls() : nullptr;
}

void ProgramControls::unref() {
        if (_refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        this->~ProgramControls();
        Memory::free(this);
        return;
}

Error ProgramControls::declare(StringView declaration, StringView user, uint32_t &badLine) {
        Mutex::Locker lock(_lock, WaitForever);
        _controls.releaseAll(_target != nullptr ? _target : nowhere, _targetArg);
        const Error err = _controls.declare(declaration, badLine);
        if (err.isOk() && !user.isEmpty()) _controls.applyUser(user);
        return err;
}

Error ProgramControls::setSet(StringView name) {
        Mutex::Locker lock(_lock, WaitForever);
        return _controls.setSet(name, _target != nullptr ? _target : nowhere, _targetArg);
}

void ProgramControls::setTarget(Controls::Emit emit, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        _target = emit;
        _targetArg = arg;
        return;
}

void ProgramControls::clearTarget(void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_targetArg != arg) return;
        _target = nullptr;
        _targetArg = nullptr;
        return;
}

bool ProgramControls::key(uint32_t usage, uint8_t modifiers, KeyEvent::Action action, Controls::Emit emit, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        return _controls.key(usage, modifiers, action, emit, arg);
}

bool ProgramControls::pad(const PadEvent &event, Controls::Emit emit, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        return _controls.pad(event, emit, arg);
}

void ProgramControls::releaseAll(Controls::Emit emit, void *arg) {
        Mutex::Locker lock(_lock, WaitForever);
        _controls.releaseAll(emit, arg);
        return;
}

Error ProgramControls::bindingText(uint16_t id, bool pad, char *out, size_t size, size_t &length) {
        Mutex::Locker lock(_lock, WaitForever);
        return _controls.bindingText(id, pad, out, size, length);
}

BMDS_NAMESPACE_END
