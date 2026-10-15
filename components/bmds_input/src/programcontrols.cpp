/**
 * @file      programcontrols.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program's actions, shared by its process and surfaces (devplan/controls.md §7c).
 */

#include <cstring>
#include <new>
#include <bmds/memory.h>
#include <bmds/programcontrols.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        void nowhere(void *, const Controls::Output &) {
                return;
        }

        // The programs that declared, for userChanged(): a slot each, under the lock.
        constexpr size_t              MaxListed = 16;
        constinit StaticObject<Mutex> listLock;
        constinit ProgramControls    *listed[MaxListed] = {};

} // namespace

Error ProgramControls::init() {
        if (!listLock.isCreated()) listLock.create();
        return Error::Ok;
}

ProgramControls *ProgramControls::create() {
        void *mem = Memory::allocAligned(MemSpace::Psram, sizeof(ProgramControls), alignof(ProgramControls));
        return mem != nullptr ? ::new (mem) ProgramControls() : nullptr;
}

ProgramControls::~ProgramControls() {
        if (_listed) {
                Mutex::Locker lock(*listLock, WaitForever);
                for (ProgramControls *&p : listed)
                        if (p == this) p = nullptr;
        }
        Memory::free(_declaration);
}

void ProgramControls::unref() {
        if (_refs.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        this->~ProgramControls();
        Memory::free(this);
        return;
}

Error ProgramControls::declare(StringView declaration, StringView user, StringView appId, uint32_t &badLine) {
        badLine = 0;
        if (declaration.size() > MaxDeclaration || appId.size() >= MaxId) return Error::Full;
        char *kept = static_cast<char *>(Memory::alloc(MemSpace::Psram, declaration.size() + 1));
        if (kept == nullptr) return Error::NoMem;
        std::memcpy(kept, declaration.data(), declaration.size());
        char *old;
        Error err;
        {
                Mutex::Locker lock(_lock, WaitForever);
                _controls.releaseAll(_target != nullptr ? _target : nowhere, _targetArg);
                err = _controls.declare(declaration, badLine);
                if (err.isOk() && !user.isEmpty()) _controls.applyUser(user);
                old = _declaration;
                _declaration = err.isOk() ? kept : nullptr;
                _declarationBytes = err.isOk() ? declaration.size() : 0;
                std::memcpy(_appId, appId.data(), appId.size());
                _appId[appId.size()] = '\0';
        }
        Memory::free(old);
        if (err.isError()) {
                Memory::free(kept);
                return err;
        }
        if (!_listed && !appId.isEmpty() && listLock.isCreated()) {
                Mutex::Locker lock(*listLock, WaitForever);
                for (ProgramControls *&p : listed)
                        if (p == nullptr) {
                                p = this;
                                _listed = true;
                                break;
                        }
        }
        return Error::Ok;
}

void ProgramControls::userChanged(StringView appId, StringView user) {
        ProgramControls *found[MaxListed] = {};
        size_t           n = 0;
        if (!listLock.isCreated()) return;
        {
                Mutex::Locker lock(*listLock, WaitForever);
                for (ProgramControls *p : listed)
                        if (p != nullptr && appId == StringView(p->_appId)) {
                                p->ref(); // kept until done with, outside the spin lock
                                found[n++] = p;
                        }
        }
        for (size_t i = 0; i < n; ++i) {
                found[i]->relay(user);
                found[i]->unref();
        }
        return;
}

void ProgramControls::appId(char (&out)[MaxId]) {
        Mutex::Locker lock(_lock, WaitForever);
        std::memcpy(out, _appId, sizeof(out));
        return;
}

void ProgramControls::relay(StringView user) {
        Mutex::Locker lock(_lock, WaitForever);
        if (_declaration == nullptr) return;
        char             set[Controls::MaxName];
        const StringView active = _controls.activeSet();
        std::memcpy(set, active.data(), active.size());
        set[active.size()] = '\0';
        const Controls::Emit emit = _target != nullptr ? _target : nowhere;
        _controls.releaseAll(emit, _targetArg);
        uint32_t line = 0;
        if (_controls.declare(StringView(_declaration, _declarationBytes), line).isError()) return; // read before
        _controls.applyUser(user);
        (void)_controls.setSet(StringView(set), emit, _targetArg);
        return;
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
