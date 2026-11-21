/**
 * @file      ioregistry.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Who holds each I/O resource (devplan/io.md §3.2, §3.3).
 */

#include <cstring>
#include <utility>
#include <bmds/assert.h>
#include <bmds/ioregistry.h>

BMDS_NAMESPACE_BEGIN

namespace {

        void copyName(char (&out)[IoHolder::NameBytes], const char *name) {
                const size_t n = std::strlen(name);
                const size_t k = n < IoHolder::NameBytes - 1 ? n : IoHolder::NameBytes - 1;
                std::memcpy(out, name, k);
                out[k] = '\0';
                return;
        }

} // namespace

// -- IoClaim

IoClaim::~IoClaim() {
        reset();
}

IoClaim::IoClaim(IoClaim &&other) : _registry(other._registry), _owner(other._owner) {
        other._registry = nullptr;
}

IoClaim &IoClaim::operator=(IoClaim &&other) {
        if (this != &other) {
                reset();
                _registry = other._registry;
                _owner = other._owner;
                other._registry = nullptr;
        }
        return *this;
}

Error IoClaim::add(Span<const IoUse> uses, unsigned flags, IoResource *blocked) {
        if (_registry == nullptr) return Error::Invalid;
        return _registry->add(_owner, uses, flags, blocked);
}

Error IoClaim::release(IoResource resource) {
        if (_registry == nullptr) return Error::NotExist;
        return _registry->release(_owner, resource);
}

void IoClaim::releaseAll() {
        if (_registry != nullptr) _registry->releaseAll(_owner);
        return;
}

void IoClaim::reset() {
        if (_registry != nullptr) _registry->endOwner(_owner);
        _registry = nullptr;
        return;
}

// -- IoRegistry

IoRegistry::IoRegistry(const IoChip &chip) : _chip(chip), _resourceCount(chip.resourceCount()) {
        BMDS_ASSERT(_resourceCount <= MaxResources);
        if (_resourceCount > MaxResources) _resourceCount = MaxResources;
}

Error IoRegistry::setBoardUse(IoResource pad, IoBoardUse use) {
        if (pad >= _chip.padCount()) return Error::Invalid;
        Mutex::Locker lock(_lock, WaitForever);
        _slots[pad].board = use;
        ++_generation;
        return Error::Ok;
}

IoBoardUse IoRegistry::boardUse(IoResource pad) const {
        if (pad >= _chip.padCount()) return IoBoardUse::None;
        Mutex::Locker lock(_lock, WaitForever);
        return _slots[pad].board;
}

Result<IoClaim> IoRegistry::claim(StringView owner, IoOwnerKind kind, Span<const IoUse> uses, unsigned flags,
                                  IoResource *blocked) {
        if (owner.isEmpty()) return makeError<IoClaim>(Error::Invalid);
        const Told    told{*this}; // after the lock
        Mutex::Locker lock(_lock, WaitForever);
        uint8_t       index = NoOwner;
        for (size_t i = 0; i < MaxOwners; ++i) {
                if (!_owners[i].inUse) {
                        index = static_cast<uint8_t>(i);
                        break;
                }
        }
        if (index == NoOwner) return makeError<IoClaim>(Error::Full);
        Owner       &o = _owners[index];
        const size_t n = owner.size() < IoHolder::NameBytes - 1 ? owner.size() : IoHolder::NameBytes - 1;
        std::memcpy(o.name, owner.data(), n);
        o.name[n] = '\0';
        o.kind = kind;
        o.inUse = true;
        const Error err = addLocked(index, uses, flags, blocked);
        if (err.isError()) {
                o.inUse = false;
                return makeError<IoClaim>(err);
        }
        return makeResult(IoClaim(this, index));
}

Error IoRegistry::add(uint8_t owner, Span<const IoUse> uses, unsigned flags, IoResource *blocked) {
        const Told    told{*this}; // after the lock
        Mutex::Locker lock(_lock, WaitForever);
        return addLocked(owner, uses, flags, blocked);
}

Error IoRegistry::addLocked(uint8_t owner, Span<const IoUse> uses, unsigned flags, IoResource *blocked) {
        if ((flags & ~static_cast<unsigned>(Force)) != 0) return Error::Invalid;
        const bool force = (flags & Force) != 0;
        const bool user = _owners[owner].kind == IoOwnerKind::User;
        // Every resource first, so a refusal changes nothing.
        for (size_t i = 0; i < uses.size(); ++i) {
                const IoResource r = uses[i].resource;
                if (r >= _resourceCount) {
                        if (blocked != nullptr) *blocked = r;
                        return Error::Invalid;
                }
                for (size_t j = 0; j < i; ++j) {
                        if (uses[j].resource == r) {
                                if (blocked != nullptr) *blocked = r;
                                return Error::Invalid;
                        }
                }
                const Slot &s = _slots[r];
                if (s.owner == owner) {
                        if (blocked != nullptr) *blocked = r;
                        return Error::Exists;
                }
                // A forced resource is borrowed already: a second borrower would lose the first's
                // saved state (devplan/io.md §3.3), so it waits for the first to give it back.
                const bool held = s.owner != NoOwner;
                const bool wired = user && (s.board == IoBoardUse::Fixed || s.board == IoBoardUse::Device);
                if ((held || wired) && (!force || s.lender != NoOwner)) {
                        if (blocked != nullptr) *blocked = r;
                        return Error::Busy;
                }
        }
        for (const IoUse &u : uses) {
                Slot &s = _slots[u.resource];
                if (s.owner != NoOwner) {
                        s.lender = s.owner;
                        s.lenderRole = s.role;
                }
                s.owner = owner;
                s.role = u.role != nullptr ? u.role : "";
        }
        if (!uses.isEmpty()) ++_generation;
        return Error::Ok;
}

void IoRegistry::releaseSlotLocked(uint8_t owner, Slot &slot) {
        if (slot.owner == owner) {
                // The holder lets go: a forced resource goes back to its lender.
                slot.owner = slot.lender;
                slot.role = slot.lender != NoOwner ? slot.lenderRole : "";
                slot.lender = NoOwner;
                slot.lenderRole = "";
        } else if (slot.lender == owner) {
                // The lender lets go while it is borrowed: the borrower keeps it.
                slot.lender = NoOwner;
                slot.lenderRole = "";
        }
        return;
}

Error IoRegistry::release(uint8_t owner, IoResource resource) {
        if (resource >= _resourceCount) return Error::NotExist;
        const Told    told{*this}; // after the lock
        Mutex::Locker lock(_lock, WaitForever);
        Slot         &s = _slots[resource];
        if (s.owner != owner && s.lender != owner) return Error::NotExist;
        releaseSlotLocked(owner, s);
        ++_generation;
        return Error::Ok;
}

void IoRegistry::releaseAll(uint8_t owner) {
        const Told    told{*this}; // after the lock
        Mutex::Locker lock(_lock, WaitForever);
        for (size_t i = 0; i < _resourceCount; ++i) releaseSlotLocked(owner, _slots[i]);
        ++_generation;
        return;
}

void IoRegistry::endOwner(uint8_t owner) {
        const Told    told{*this}; // after the lock
        Mutex::Locker lock(_lock, WaitForever);
        for (size_t i = 0; i < _resourceCount; ++i) releaseSlotLocked(owner, _slots[i]);
        _owners[owner] = Owner();
        ++_generation;
        return;
}

IoClass IoRegistry::classLocked(const Slot &slot) const {
        if (slot.owner != NoOwner)
                return _owners[slot.owner].kind == IoOwnerKind::User ? IoClass::User : IoClass::System;
        switch (slot.board) {
                case IoBoardUse::Fixed: return IoClass::Fixed;
                case IoBoardUse::Device: return IoClass::System;
                default: return IoClass::Free;
        }
}

Error IoRegistry::holder(IoResource resource, IoHolder &out) const {
        out = IoHolder();
        if (resource >= _resourceCount) return Error::Invalid;
        Mutex::Locker lock(_lock, WaitForever);
        const Slot   &s = _slots[resource];
        out.resourceClass = classLocked(s);
        if (s.owner != NoOwner) {
                copyName(out.owner, _owners[s.owner].name);
                out.role = s.role;
        }
        if (s.lender != NoOwner) {
                copyName(out.lender, _owners[s.lender].name);
                out.lenderRole = s.lenderRole;
        }
        return Error::Ok;
}

IoClass IoRegistry::resourceClass(IoResource resource) const {
        if (resource >= _resourceCount) return IoClass::Free;
        Mutex::Locker lock(_lock, WaitForever);
        return classLocked(_slots[resource]);
}

const char *ioClassName(IoClass resourceClass) {
        switch (resourceClass) {
                case IoClass::Free: return "free";
                case IoClass::Fixed: return "fixed";
                case IoClass::System: return "system";
                case IoClass::User: return "user";
        }
        return "?";
}

BMDS_NAMESPACE_END
