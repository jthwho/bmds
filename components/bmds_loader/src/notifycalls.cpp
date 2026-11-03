/**
 * @file      notifycalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Notifications' system calls (devplan/notifications.md §3.3): an events fd, posting with it (or
 * without), closing one's own.
 */

#include <cstring>
#include <bmds/errno.h>
#include <bmds/notifications.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::KernelCall;
using bmds::Notification;
namespace Notifications = bmds::Notifications;

namespace {

        static_assert(sizeof(bmds_notify_event_t) == sizeof(Notifications::EventRecord) &&
                              offsetof(bmds_notify_event_t, id) == offsetof(Notifications::EventRecord, id) &&
                              offsetof(bmds_notify_event_t, event) == offsetof(Notifications::EventRecord, event) &&
                              offsetof(bmds_notify_event_t, action) == offsetof(Notifications::EventRecord, action),
                      "an events fd's records are the ABI's");
        static_assert(BMDS_NOTIFY_ACTIVATED == uint32_t(Notifications::Event::Activated) &&
                              BMDS_NOTIFY_DISMISSED == uint32_t(Notifications::Event::Dismissed) &&
                              BMDS_NOTIFY_EXPIRED == uint32_t(Notifications::Event::Expired),
                      "the ABI's events");
        static_assert(BMDS_NOTIFY_LOW == uint32_t(Notification::Urgency::Low) &&
                              BMDS_NOTIFY_NORMAL == uint32_t(Notification::Urgency::Normal) &&
                              BMDS_NOTIFY_CRITICAL == uint32_t(Notification::Urgency::Critical),
                      "the ABI's urgencies");
        static_assert(BMDS_NOTIFY_TARGET_NONE == uint32_t(Notification::Target::None) &&
                              BMDS_NOTIFY_TARGET_OPEN == uint32_t(Notification::Target::Open) &&
                              BMDS_NOTIFY_TARGET_SHOW == uint32_t(Notification::Target::Show) &&
                              BMDS_NOTIFY_TARGET_LAUNCH == uint32_t(Notification::Target::Launch) &&
                              BMDS_NOTIFY_TARGET_SERVICE == uint32_t(Notification::Target::Service),
                      "the ABI's targets");
        static_assert(BMDS_NOTIFY_SILENT == Notification::Silent && BMDS_NOTIFY_TRANSIENT == Notification::Transient &&
                              BMDS_NOTIFY_QUESTION == Notification::Question,
                      "the ABI's flags");
        static_assert(BMDS_NOTIFY_NO_TIMEOUT == Notification::NoTimeout &&
                              BMDS_NOTIFY_MAX_ACTIONS == Notification::MaxActions,
                      "the ABI's timeouts and actions");
        static_assert(sizeof(bmds_notification_t::app) == Notification::AppBytes &&
                              sizeof(bmds_notification_t::icon) == Notification::IconBytes &&
                              sizeof(bmds_notification_t::title) == Notification::TitleBytes &&
                              sizeof(bmds_notification_t::body) == Notification::BodyBytes &&
                              sizeof(bmds_notification_t::target_text) == Notification::TargetBytes &&
                              sizeof(bmds_notification_t::message) == Notification::MessageBytes &&
                              sizeof(bmds_notification_t::actions[0]) == Notification::LabelBytes,
                      "the ABI's texts");

        template <size_t N> void copyText(char (&out)[N], const char (&in)[N]) {
                std::memcpy(out, in, N);
                out[N - 1] = '\0'; // the log cuts it at a character's end
                return;
        }

} // namespace

extern "C" {

int32_t bmds_k_notify_open(uint32_t flags) {
        KernelCall call;
        if ((flags & ~BMDS_O_CLOEXEC) != 0) return -BMDS_EINVAL;
        auto [handle, err] = Notifications::openEvents();
        if (err == Error::Full) return -BMDS_EBUSY;
        if (err.isError()) return bmds::Process::errnoFor(err);
        return call->addHandle(std::move(handle), flags);
}

int32_t bmds_k_notify_send(int32_t fd, const bmds_notification_t *n) {
        KernelCall          call;
        bmds_notification_t in;
        const int32_t       r = bmds::detail::copyIn(n, in);
        if (r < 0) return r;
        Notifications::Listener *listener = nullptr;
        bmds::FileHandle::Ptr    handle;
        if (fd != -1) {
                handle = call->handle(fd);
                if (!handle) return -BMDS_EBADF;
                listener = Notifications::listenerOf(handle->stream());
                if (listener == nullptr) return -BMDS_ENOTTY;
        }
        if (in.urgency > BMDS_NOTIFY_CRITICAL || in.target > BMDS_NOTIFY_TARGET_SERVICE ||
            (in.flags & ~(BMDS_NOTIFY_SILENT | BMDS_NOTIFY_TRANSIENT | BMDS_NOTIFY_QUESTION)) != 0 ||
            in.action_count > BMDS_NOTIFY_MAX_ACTIONS || in.timeout_ms < BMDS_NOTIFY_NO_TIMEOUT ||
            (in.action_targets >> BMDS_NOTIFY_MAX_ACTIONS) != 0)
                return -BMDS_EINVAL;
        Notification k;
        k.replaces = in.replaces;
        k.urgency = Notification::Urgency(in.urgency);
        k.flags = uint8_t(in.flags);
        k.target = Notification::Target(in.target);
        k.actionCount = uint8_t(in.action_count);
        k.actionTargets = uint8_t(in.action_targets);
        k.timeoutMs = in.timeout_ms;
        copyText(k.app, in.app);
        copyText(k.icon, in.icon);
        copyText(k.title, in.title);
        copyText(k.body, in.body);
        copyText(k.targetText, in.target_text);
        copyText(k.message, in.message);
        for (size_t i = 0; i < Notification::MaxActions; ++i) copyText(k.actions[i], in.actions[i]);
        if (k.app[0] == '\0') {
                // The program's name: its command's first word, its last part.
                const char *command = call.thread().run->command;
                const char *start = command;
                for (const char *c = command; *c != '\0' && *c != ' '; ++c)
                        if (*c == '/') start = c + 1;
                size_t i = 0;
                while (start[i] != '\0' && start[i] != ' ' && i + 1 < sizeof(k.app)) {
                        k.app[i] = start[i];
                        ++i;
                }
                k.app[i] = '\0';
        }
        auto [id, err] = Notifications::post(k, call.thread().id(BMDS_PROC_SELF), listener);
        if (err == Error::PermissionDenied) return -BMDS_EPERM;
        if (err.isError()) return bmds::Process::errnoFor(err);
        return int32_t(id);
}

int32_t bmds_k_notify_close(uint32_t id) {
        KernelCall  call;
        const Error err = Notifications::close(id, call.thread().id(BMDS_PROC_SELF));
        if (err == Error::PermissionDenied) return -BMDS_EPERM;
        return err.isOk() ? 0 : -BMDS_ENOENT;
}

} // extern "C"
