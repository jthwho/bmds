/**
 * @file      notifier.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A program's notifications through the toolkit (devplan/notifications.md §3.5).
 */

#include <cstring>
#include <bmds/guiapplication.h>
#include <bmds/notifier.h>

BMDS_NAMESPACE_BEGIN

Notifier::Notifier(GuiApplication &app) : _app(app) {}

Notifier::~Notifier() {
        _listener.stop();
        _app.cancelPosts(this);
}

Result<uint32_t> Notifier::show(const Notification &n) {
        if (Error err = _listener.start(); err.isError()) return makeError<uint32_t>(err);
        if (n.app[0] != '\0' || _app.applicationName().isEmpty())
                return Notifications::post(n, Notifications::listenerOwner(_listener), &_listener);
        // Named by the application.
        Notification     named = n;
        const StringView name = _app.applicationName();
        const size_t     len = name.size() < sizeof(named.app) - 1 ? name.size() : sizeof(named.app) - 1;
        std::memcpy(named.app, name.data(), len);
        named.app[len] = '\0';
        return Notifications::post(named, Notifications::listenerOwner(_listener), &_listener);
}

Error Notifier::close(uint32_t id) {
        if (_listener.id() == 0) return Error::NotExist;
        return Notifications::close(id, Notifications::listenerOwner(_listener));
}

// The server's thread, the registry's lock held: queued for the loop.
void Notifier::told(void *arg, uint32_t id, Event event, uint32_t action) {
        auto *self = static_cast<Notifier *>(arg);
        {
                SpinLock::Locker lock(self->_lock);
                if (self->_count == QueueEvents) return; // the loop is far behind: dropped
                self->_queue[self->_count++] = Pending{id, event, action};
        }
        if (!self->_posted.exchange(true) && self->_app.post(due, self).isError()) self->_posted = false;
        return;
}

void Notifier::due(void *arg) {
        auto *self = static_cast<Notifier *>(arg);
        self->_posted = false;
        Pending events[QueueEvents];
        size_t  n;
        {
                SpinLock::Locker lock(self->_lock);
                n = self->_count;
                for (size_t i = 0; i < n; ++i) events[i] = self->_queue[i];
                self->_count = 0;
        }
        for (size_t i = 0; i < n; ++i)
                if (self->_fn != nullptr) self->_fn(self->_arg, *self, events[i].id, events[i].event, events[i].action);
        return;
}

BMDS_NAMESPACE_END
