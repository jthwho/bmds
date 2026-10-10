/**
 * @file      notifier.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/notifications.h>
#include <bmds/result.h>
#include <bmds/spinlock.h>

BMDS_NAMESPACE_BEGIN

class GuiApplication;

/**
 * @brief A program's notifications through the toolkit
 *        (`devplan/notifications.md` §3.5): posts them as the system's
 *        (`Notifications`), and tells the program on its loop what became of
 *        them (clicked, an action, dismissed, expired).
 *
 * A notification's `replaces` updates one of this notifier's own (a player's
 * "now playing"). Destroying it stops the events; its notifications stay.
 *
 * @par Thread Safety
 * The application's lock (the loop's thread).
 */
class Notifier {
        public:
                using Event = Notifications::Event;

                /** @brief Told on the loop: @p id's @p event (`Activated`: @p action 0 its body, else a button). */
                using Told = void (*)(void *arg, Notifier &notifier, uint32_t id, Event event, uint32_t action);

                explicit Notifier(GuiApplication &app);
                ~Notifier();
                Notifier(const Notifier &) = delete;
                Notifier &operator=(const Notifier &) = delete;

                void setTold(Told fn, void *arg) {
                        _fn = fn;
                        _arg = arg;
                }

                /**
                 * @brief Posts @p n (`replaces`: one of this notifier's to update).
                 * @return Its id, or `NoMem`, `Full` (too many listeners), `PermissionDenied`.
                 */
                Result<uint32_t> show(const Notification &n);

                /** @brief Closes @p id, one of this notifier's. @return `Ok`, `NotExist`, `PermissionDenied`. */
                Error close(uint32_t id);

        private:
                static constexpr size_t QueueEvents = 8;

                struct Pending {
                                uint32_t id = 0;
                                Event    event = Event::Expired;
                                uint32_t action = 0;
                };

                static void told(void *arg, uint32_t id, Event event, uint32_t action); // the server's thread
                static void due(void *arg);                                             // the loop

                GuiApplication         &_app;
                Notifications::Listener _listener{told, this};
                SpinLock                _lock;
                Pending                 _queue[QueueEvents];
                size_t                  _count = 0;
                std::atomic<bool>       _posted{false};
                Told                    _fn = nullptr;
                void                   *_arg = nullptr;
};

BMDS_NAMESPACE_END
