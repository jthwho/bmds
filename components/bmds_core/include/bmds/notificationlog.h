/**
 * @file      notificationlog.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/memspace.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A notification (`devplan/notifications.md` §3.1): what its sender
 *        says, and what the system keeps of it.
 */
struct Notification {
                /** @brief How much it matters. */
                enum class Urgency : uint8_t {
                        Low,      ///< A balloon, no sound.
                        Normal,   ///< A balloon and a sound.
                        Critical, ///< A sound, a red edge, and it stays until dismissed.
                };

                /** @brief What a click on it does (the system does it). */
                enum class Target : uint8_t {
                        None,
                        Open,    ///< `target`: a path, opened by its type's application (a folder in a window).
                        Show,    ///< `target`: a path, shown in its folder, selected.
                        Launch,  ///< `target`: a Desktop Entry's id (`sysmon.desktop`), started.
                        Service, ///< `target`: a service's name, sent `message`.
                };

                /** @brief `flags`. */
                enum Flag : uint8_t {
                        Silent = 1u << 0,    ///< No sound.
                        Transient = 1u << 1, ///< Not kept once its balloon is gone.
                        Question = 1u << 2,  ///< Gone once answered: a click, a button or its close box.
                };

                static constexpr int32_t DefaultTimeout = 0; ///< `timeoutMs`: the setting's.
                static constexpr int32_t NoTimeout = -1;     ///< `timeoutMs`: until dismissed.
                static constexpr size_t  MaxActions = 3;
                static constexpr size_t  AppBytes = 32;
                static constexpr size_t  IconBytes = 64;
                static constexpr size_t  TitleBytes = 64;
                static constexpr size_t  BodyBytes = 256;
                static constexpr size_t  TargetBytes = 192;
                static constexpr size_t  MessageBytes = 64;
                static constexpr size_t  LabelBytes = 24;

                // Said by the sender.
                uint32_t replaces = 0; ///< One of its own to update in place (0: a new one).
                Urgency  urgency = Urgency::Normal;
                uint8_t  flags = 0;
                Target   target = Target::None;
                uint8_t  actionCount = 0;
                uint8_t  actionTargets = 0; ///< Bit i: button i + 1 runs the target, as a click on the body.
                int32_t  timeoutMs = DefaultTimeout;
                char     app[AppBytes] = {};     ///< The application's name (the centre groups by it).
                char     icon[IconBytes] = {};   ///< A theme icon's name (empty: the application's).
                char     title[TitleBytes] = {}; ///< UTF-8.
                char     body[BodyBytes] = {};   ///< UTF-8, may have `\n`.
                char     targetText[TargetBytes] = {};
                char     message[MessageBytes] = {}; ///< `Target::Service`'s.
                char     actions[MaxActions][LabelBytes] = {};

                // Kept by the system.
                uint32_t id = 0;
                int32_t  owner = 0;       ///< The sender's pid (0: the kernel's own).
                uint32_t listener = 0;    ///< Who is told of its events (0: nobody).
                uint64_t changed = 0;     ///< The change that last touched it.
                int64_t  postedNs = 0;    ///< When (real time).
                bool     balloon = false; ///< Its balloon is due or showing.
                bool     read = false;    ///< Seen (a balloon, or the centre, showed it).
};

/**
 * @brief The notifications kept, pure (`devplan/notifications.md` §3.2): the
 *        last `Capacity` in their order, ids, replacing, the balloon's end,
 *        closing, read and unread, each change numbered.
 *
 * Full, a new one takes the place of the oldest without a balloon due (or the
 * oldest, if every one has).
 *
 * @par Thread Safety
 * Not thread-safe: the kernel's `Notifications` holds a lock around it.
 */
class NotificationLog {
        public:
                /** @brief Notifications kept. */
                static constexpr size_t Capacity = 64;

                /** @brief How a balloon ended. */
                enum class Ended : uint8_t {
                        Expired,   ///< Its time passed: kept in the centre.
                        Dismissed, ///< Closed by the user: kept in the centre.
                        Activated, ///< Clicked: kept, read.
                };

                explicit NotificationLog(MemSpace space = MemSpace::Psram) : _space(space) {}
                ~NotificationLog();
                NotificationLog(const NotificationLog &) = delete;
                NotificationLog &operator=(const NotificationLog &) = delete;

                /**
                 * @brief Keeps @p n from @p owner, its events to @p listener, posted
                 *        at @p nowNs; with `replaces` one of @p owner's own, that one is
                 *        updated in place (its id kept, its balloon due again), else a
                 *        new one. Its texts are cut at a character's end, its actions
                 *        at `MaxActions`.
                 * @return Its id, or `NoMem` (the first one's storage), `PermissionDenied`
                 *         (`replaces` someone else's).
                 */
                Result<uint32_t> post(const Notification &n, int32_t owner, uint32_t listener, int64_t nowNs);

                /**
                 * @brief Notification @p id's balloon ended @p how: kept (unless transient, or a
                 *        question answered or dismissed), not due.
                 */
                Error endBalloon(uint32_t id, Ended how);

                /** @brief Notification @p id clicked in the centre: read, and gone if a question. */
                Error activate(uint32_t id);

                /** @brief Forgets @p id, if @p owner sent it (-1: anyone). @return `Ok`, `NotExist`, `PermissionDenied`. */
                Error close(uint32_t id, int32_t owner);

                /** @brief Forgets every one whose balloon isn't due (the centre's Clear All). */
                void clear();

                /** @brief Marks @p id read (0: every one). */
                void markRead(uint32_t id);

                /** @brief Stops telling @p listener: its notifications are kept, told to nobody. */
                void dropListener(uint32_t listener);

                /** @brief Returns how many are unread. */
                size_t unread() const;

                /** @brief Returns how many are kept. */
                size_t count() const;

                /** @brief Returns the last change's number (0: none yet). */
                uint64_t lastChange() const { return _change; }

                /** @brief Copies @p id into @p out. @return `Ok` or `NotExist`. */
                Error get(uint32_t id, Notification &out) const;

                /** @brief Fills @p out with the ids kept, the newest first. @return How many there are. */
                size_t ids(Span<uint32_t> out) const;

        private:
                struct Slot {
                                Notification n;
                                uint64_t     order = 0; // when posted or replaced: newer is larger
                                bool         used = false;
                };

                Slot *find(uint32_t id) const;
                Slot *freeSlot();
                void  touch(Slot &s) { s.n.changed = ++_change; }

                MemSpace _space;
                Slot    *_slots = nullptr; // Capacity, allocated with the first
                uint64_t _change = 0;
                uint64_t _order = 0;
                uint32_t _lastId = 0;
};

BMDS_NAMESPACE_END
