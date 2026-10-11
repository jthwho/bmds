/**
 * @file      screenshottaker.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Print Screen (devplan/notifications.md §4).
 */

#include <cstdio>
#include "screenshottaker.h"

BMDS_NAMESPACE_BEGIN

namespace {

        template <size_t N> void copy(char (&out)[N], StringView text) {
                std::snprintf(out, N, "%.*s", int(text.size()), text.data());
                return;
        }

} // namespace

ScreenshotTaker::ScreenshotTaker(GuiApplication &app) : _app(app), _notifier(app) {}

ScreenshotTaker::~ScreenshotTaker() {
        _thread.join();
}

void ScreenshotTaker::take() {
        if (_busy.exchange(true)) return; // one at a time: the PNG of the last is being written
        _thread.join();                   // the last one's, ended
        Thread::Config config;
        config.name = "screenshot";
        config.stackBytes = StackBytes;
        const Error err = _thread.start(config, run, this);
        if (err.isError()) {
                _err = err;
                _report = Screenshot::Report{};
                _report.failed = "a thread";
                done(this);
        }
        return;
}

void ScreenshotTaker::run(void *arg) {
        auto *self = static_cast<ScreenshotTaker *>(arg);
        self->_report = Screenshot::Report{};
        self->_err = Screenshot::configuredPath(self->_path);
        if (self->_err.isError()) {
                self->_report.failed = Screenshot::PathKey;
        } else {
                self->_err = Screenshot::save(self->_path, Png::DefaultLevel, Screenshot::Unique | Screenshot::MakeDirs,
                                              Vfs::Context{}, self->_report);
        }
        if (self->_app.post(done, self).isError()) self->_busy = false; // no notification, but the next one works
        return;
}

void ScreenshotTaker::done(void *arg) {
        auto *self = static_cast<ScreenshotTaker *>(arg);
        self->_thread.join();
        Notification n;
        copy(n.app, "Screenshots");
        copy(n.icon, "camera-photo");
        const StringView path = self->_path.view();
        if (self->_err.isOk()) {
                const size_t slash = path.lastIndexOf('/');
                copy(n.title, "Screenshot saved");
                copy(n.body, slash == StringView::NPos ? path : path.mid(slash + 1));
                n.target = Notification::Target::Show;
                copy(n.targetText, path);
        } else {
                n.urgency = Notification::Urgency::Critical;
                copy(n.title, "Screenshot not saved");
                const char *what = self->_report.failed != nullptr ? self->_report.failed : self->_path.cStr();
                std::snprintf(n.body, sizeof(n.body), "%s: %s", what, self->_err.desc());
        }
        (void)self->_notifier.show(n);
        self->_busy = false;
        return;
}

BMDS_NAMESPACE_END
