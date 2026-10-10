/**
 * @file      notifications.h
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
#include <bmds/filehandle.h>
#include <bmds/notificationlog.h>
#include <bmds/result.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The system's notifications (`devplan/notifications.md` §3.2): any
 *        program posts them; the server (the panel) shows them as balloons and
 *        keeps the last `NotificationLog::Capacity` in its centre; a sender may
 *        be told what became of its own.
 *
 * Without a server they are kept all the same, and shown when one comes.
 *
 * @par Thread Safety
 * Thread-safe (a lock). The server's and listeners' functions are called with
 * no lock of the caller's held, from the thread that made the change; they
 * must not block, and a listener's must not call `Notifications`.
 */
namespace Notifications {

        /** @brief What became of a notification, told to its listener. */
        enum class Event : uint32_t {
                Activated = 1, ///< Clicked: its body (action 0) or an action button (1 to 3).
                Dismissed = 2, ///< Closed by the user.
                Expired = 3,   ///< Its balloon's time passed (it stays in the centre).
        };

        /** @brief A record of the events fd (`openEvents()`): the SDK's `bmds_notify_event_t`. */
        struct EventRecord {
                        uint32_t size = sizeof(EventRecord);
                        uint32_t id = 0;
                        uint32_t event = 0; ///< `Event`.
                        uint32_t action = 0;
        };

        /** @brief Listeners at once, every program's and the toolkit's together. */
        constexpr size_t MaxListeners = 32;

        /**
         * @brief Told of the events of the notifications posted with it, until
         *        stopped (its notifications are kept, told to nobody).
         */
        class Listener {
                public:
                        using Told = void (*)(void *arg, uint32_t id, Event event, uint32_t action);

                        Listener(Told fn, void *arg) : _fn(fn), _arg(arg) {}
                        ~Listener() { stop(); }
                        Listener(const Listener &) = delete;
                        Listener &operator=(const Listener &) = delete;

                        /** @brief Starts it. @return `Ok`, `Full` or `InvalidState` (before `init()`). */
                        Error start();

                        /** @brief Stops it (waits for a function running). */
                        void stop();

                        /** @brief Returns its number in the log (0: not started). */
                        uint32_t id() const { return _id; }

                        /** @brief Tells it (the registry's, under its lock). */
                        void tell(uint32_t id, Event event, uint32_t action) const {
                                if (_fn != nullptr) _fn(_arg, id, event, action);
                        }

                private:
                        Told     _fn;
                        void    *_arg;
                        uint32_t _id = 0;
        };

        /**
         * @brief Returns the owner for notifications posted with @p listener by a
         *        program that doesn't know its pid (the toolkit's, in-process): the
         *        listener's, never a pid's (they are negative, below -1).
         */
        inline int32_t listenerOwner(const Listener &listener) {
                return -2 - int32_t(listener.id() & 0x3fffffffu);
        }

        /** @brief Makes the registry (the boot's stage). @return `Ok`. */
        Error init();

        /**
         * @brief Posts @p n from @p owner (a pid; 0: the kernel's), its events to
         *        @p listener (null: nobody); `n.replaces` one of @p owner's own
         *        updates it.
         * @return Its id, or `NoMem`, `PermissionDenied`, `InvalidState`.
         */
        Result<uint32_t> post(const Notification &n, int32_t owner, Listener *listener);

        /** @brief Closes @p id, one of @p owner's. @return `Ok`, `NotExist`, `PermissionDenied`. */
        Error close(uint32_t id, int32_t owner);

        // ---- The server's (the panel's)

        /** @brief Told that something changed (the server reads what it shows again). */
        using Changed = void (*)(void *arg);

        /** @brief Becomes the server, told of changes. @return `Ok`, or `Exists` (another). */
        Error setServer(Changed fn, void *arg);

        /** @brief Stops being the server (its @p arg). */
        void clearServer(void *arg);

        /** @brief Returns true while there is a server. */
        bool hasServer();

        /** @brief Returns the last change's number. */
        uint64_t lastChange();

        /** @brief Returns how many are unread. */
        size_t unread();

        /** @brief Fills @p out with the ids kept, the newest first. @return How many there are. */
        size_t ids(Span<uint32_t> out);

        /** @brief Copies @p id. @return `Ok` or `NotExist`. */
        Error get(uint32_t id, Notification &out);

        /**
         * @brief @p id's balloon ended @p how (with @p action, for a click: 0 the
         *        body, else the button's): its sender told.
         */
        void endBalloon(uint32_t id, NotificationLog::Ended how, uint32_t action = 0);

        /** @brief Clicked in the centre (its balloon gone): its sender told, as a balloon's click. */
        void activate(uint32_t id, uint32_t action);

        /** @brief The user removed @p id from the centre. */
        void remove(uint32_t id);

        /** @brief The user cleared the centre (the balloons due stay). */
        void clear();

        /** @brief Marks @p id read (0: all). */
        void markRead(uint32_t id);

        // ---- Programs' events (the SDK's notify_open)

        /**
         * @brief Opens an fd of `EventRecord`s: readable and pollable; its
         *        listener (`listenerOf()`) is what `notify_send` posts with.
         * @return The handle, or `NoMem`, `Full`.
         */
        Result<FileHandle::Ptr> openEvents();

        /** @brief Returns the listener of an events fd's @p stream, or null if it isn't one. */
        Listener *listenerOf(Stream &stream);

} // namespace Notifications

BMDS_NAMESPACE_END
