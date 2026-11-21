/**
 * @file      ioregistry.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Who holds each I/O resource (devplan/io.md §3.2): pads, and the peripheral
 * instances and channels behind them. Every driver claims what it uses
 * before configuring it; a claim is all or nothing, and fails naming the
 * holder. Pure logic, host-tested; the system's one registry is `Io`'s
 * (<bmds/io.h>).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/iochip.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

class IoRegistry;

/** @brief What a pad is to the board, as the registry keeps it (from `BoardPinUse`). */
enum class IoBoardUse : uint8_t {
        None,   ///< The board doesn't connect it.
        Fixed,  ///< Wired to something a change would break.
        Device, ///< Wired to an on-board device.
        Free,   ///< Brought out, or a part nothing in the system drives.
};

/**
 * @brief A resource's class (devplan/io.md §3.2), what decides whether the
 *        user may change it without force.
 */
enum class IoClass : uint8_t {
        Free,   ///< Nobody holds it, and the board leaves it to the user.
        Fixed,  ///< The board wires it to something a change would break.
        System, ///< A subsystem holds it, or the board wires it to an on-board device.
        User,   ///< The user holds it (a setup in sysconf, a program's line).
};

/** @brief Who an owner is. */
enum class IoOwnerKind : uint8_t {
        System, ///< A driver or subsystem: may claim board-wired pads without force.
        User,   ///< The user, or a program for them: board-wired and held resources need force.
};

/** @brief One resource in a claim, with what it is used for. */
struct IoUse {
                IoResource  resource = NoIoResource; ///< The resource.
                const char *role = "";               ///< Its role for the owner (`RMII TXD1`): rodata, kept.
};

/** @brief Who holds a resource (`IoRegistry::holder()`). */
struct IoHolder {
                /** @brief Room for an owner's name, NUL included. */
                static constexpr size_t NameBytes = 16;

                IoClass     resourceClass = IoClass::Free; ///< Its class.
                char        owner[NameBytes] = {};         ///< The holder's name, or "" when nobody holds it.
                const char *role = "";                     ///< The holder's role for it, or "".
                char        lender[NameBytes] = {};        ///< Who it was forced from, or "".
                const char *lenderRole = "";               ///< The lender's role for it, or "".

                /** @brief True if someone holds it. */
                bool isHeld() const { return owner[0] != '\0'; }

                /** @brief True if it was taken by force from another holder. */
                bool isForced() const { return lender[0] != '\0'; }
};

/**
 * @brief An owner's hold on resources: released when destroyed.
 *
 * Move-only. A default-made claim holds nothing.
 *
 * @par Thread Safety
 * Not thread-safe: one claim is used by one task at a time (its owner's).
 * Distinct claims may be used concurrently.
 */
class IoClaim {
        public:
                /** @brief Holds nothing. */
                IoClaim() = default;

                /** @brief Releases everything held. */
                ~IoClaim();

                IoClaim(const IoClaim &) = delete;
                IoClaim &operator=(const IoClaim &) = delete;

                /** @brief Takes @p other's hold; @p other then holds nothing. */
                IoClaim(IoClaim &&other);

                /** @brief Releases what this holds, then takes @p other's hold. */
                IoClaim &operator=(IoClaim &&other);

                /** @brief True if this is an owner's claim (it may hold nothing for now). */
                bool isValid() const { return _registry != nullptr; }

                /**
                 * @brief Claims more resources for the same owner, all or none.
                 * @param uses   What to claim.
                 * @param flags  `IoRegistry::Force` to take resources from their holders.
                 * @param[out] blocked The first resource that refused, if not null.
                 * @return `Ok`; `Busy` (held by another owner, or board-wired
                 *         for a user's claim, without `Force`); `Invalid`
                 *         (no such resource, or this claim is empty);
                 *         `Exists` (this owner holds it already).
                 * @blocking Waits for the registry's lock.
                 */
                Error add(Span<const IoUse> uses, unsigned flags, IoResource *blocked);

                /**
                 * @brief Releases one resource (a forced one goes back to its lender).
                 * @return `Ok`, or `NotExist` if this claim doesn't hold it.
                 * @blocking Waits for the registry's lock.
                 */
                Error release(IoResource resource);

                /**
                 * @brief Releases everything held; the claim stays the owner's,
                 *        so `add()` still works.
                 * @blocking Waits for the registry's lock.
                 */
                void releaseAll();

                /**
                 * @brief Releases everything and ends the owner; the claim then
                 *        holds nothing and isn't valid.
                 * @blocking Waits for the registry's lock.
                 */
                void reset();

        private:
                friend class IoRegistry;

                IoClaim(IoRegistry *registry, uint8_t owner) : _registry(registry), _owner(owner) {}

                IoRegistry *_registry = nullptr;
                uint8_t     _owner = 0;
};

/**
 * @brief The holders of a chip's I/O resources.
 *
 * One per system (`Io::registry()`); the host tests make their own.
 * Owners are a fixed pool of `MaxOwners`, resources at most
 * `MaxResources`: nothing is allocated after construction.
 *
 * @par Thread Safety
 * Thread-safe.
 */
class IoRegistry {
        public:
                /** @brief Owners at once. */
                static constexpr size_t MaxOwners = 48;

                /** @brief Resources a chip may have (pads and instances). */
                static constexpr size_t MaxResources = 192;

                /** @brief Claim flags. */
                enum ClaimFlag : unsigned {
                        Force = 1u
                                << 0, ///< Take resources from their holders, and board-wired pads (devplan/io.md §3.3).
                };

                /**
                 * @brief A registry for @p chip, with nothing held and every pad
                 *        unconnected until `setBoardUse()`.
                 */
                explicit IoRegistry(const IoChip &chip);

                IoRegistry(const IoRegistry &) = delete;
                IoRegistry &operator=(const IoRegistry &) = delete;

                /** @brief The chip. */
                const IoChip &chip() const { return _chip; }

                /**
                 * @brief Records what @p pad is to the board (at boot, before any claim).
                 * @return `Ok`, or `Invalid` (not a pad).
                 */
                Error setBoardUse(IoResource pad, IoBoardUse use);

                /** @brief What @p pad is to the board (`None` for a resource that isn't a pad). */
                IoBoardUse boardUse(IoResource pad) const;

                /**
                 * @brief Starts an owner and claims @p uses for it, all or none.
                 * @param owner  Its name, shown to the user (`eth0`, `sd0`, `io`); at most 15 bytes are kept.
                 * @param kind   System or user.
                 * @param uses   What to claim (may be empty: claim later with `IoClaim::add()`).
                 * @param flags  `Force` to take resources from their holders.
                 * @param[out] blocked The first resource that refused, if not null.
                 * @return The claim; or `Full` (`MaxOwners`), and `add()`'s errors.
                 * @blocking Waits for the lock.
                 */
                Result<IoClaim> claim(StringView owner, IoOwnerKind kind, Span<const IoUse> uses, unsigned flags,
                                      IoResource *blocked);

                /**
                 * @brief Who holds @p resource, and its class.
                 * @return `Ok`, or `Invalid` (no such resource).
                 * @blocking Waits for the lock.
                 */
                Error holder(IoResource resource, IoHolder &out) const;

                /**
                 * @brief The resource's class.
                 * @blocking Waits for the lock.
                 */
                IoClass resourceClass(IoResource resource) const;

                /**
                 * @brief A number that changes whenever a claim does, for viewers
                 *        that refresh what they show.
                 * @nonblocking
                 */
                uint32_t generation() const { return _generation; }

                /** @brief What `setChangeHook()` takes. */
                using ChangeHook = void (*)(void *arg);

                /**
                 * @brief Has @p hook called after every claim, release and ended owner (a failed
                 *        claim too), the registry's lock let go: it may read the registry. Set once,
                 *        before claims are taken (devplan/io.md §12.3).
                 */
                void setChangeHook(ChangeHook hook, void *arg) {
                        _hook = hook;
                        _hookArg = arg;
                        return;
                }

        private:
                friend class IoClaim;

                // Calls the hook when it goes: made before a method's lock, so it goes after it.
                struct Told {
                                const IoRegistry &registry;
                                ~Told() {
                                        if (registry._hook != nullptr) registry._hook(registry._hookArg);
                                }
                };

                static constexpr uint8_t NoOwner = 0xff;

                struct Owner {
                                char        name[IoHolder::NameBytes] = {};
                                IoOwnerKind kind = IoOwnerKind::System;
                                bool        inUse = false;
                };

                struct Slot {
                                uint8_t     owner = NoOwner;
                                uint8_t     lender = NoOwner;
                                IoBoardUse  board = IoBoardUse::None;
                                const char *role = "";
                                const char *lenderRole = "";
                };

                Error   addLocked(uint8_t owner, Span<const IoUse> uses, unsigned flags, IoResource *blocked);
                Error   add(uint8_t owner, Span<const IoUse> uses, unsigned flags, IoResource *blocked);
                Error   release(uint8_t owner, IoResource resource);
                void    releaseAll(uint8_t owner);
                void    endOwner(uint8_t owner);
                void    releaseSlotLocked(uint8_t owner, Slot &slot);
                IoClass classLocked(const Slot &slot) const;

                const IoChip &_chip;
                size_t        _resourceCount = 0;
                mutable Mutex _lock;
                uint32_t      _generation = 0;
                ChangeHook    _hook = nullptr;
                void         *_hookArg = nullptr;
                Owner         _owners[MaxOwners];
                Slot          _slots[MaxResources];
};

/** @brief The class's name, lower case (`free`, `fixed`, `system`, `user`). */
const char *ioClassName(IoClass resourceClass);

BMDS_NAMESPACE_END
