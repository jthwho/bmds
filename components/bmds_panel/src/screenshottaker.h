/**
 * @file      screenshottaker.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/guiapplication.h>
#include <bmds/notifier.h>
#include <bmds/screenshot.h>
#include <bmds/string.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Print Screen (`devplan/notifications.md` §4): the screen saved where
 *        `screenshot/path` says, by a thread of its own, then a notification
 *        (the file shown in its folder when clicked; a failure says why).
 *
 * @par Thread Safety
 * The application's lock (the loop's thread); the thread posts its end.
 */
class ScreenshotTaker {
        public:
                explicit ScreenshotTaker(GuiApplication &app);
                ~ScreenshotTaker(); // waits for a screenshot being written
                ScreenshotTaker(const ScreenshotTaker &) = delete;
                ScreenshotTaker &operator=(const ScreenshotTaker &) = delete;

                /** @brief Takes one (ignored while one is being written). */
                void take();

        private:
                static constexpr size_t StackBytes = 8192;

                static void run(void *arg);  // the thread
                static void done(void *arg); // the loop

                GuiApplication    &_app;
                Notifier           _notifier;
                Thread             _thread;
                String             _path{MemSpace::Psram}; // the thread's until it ends
                Screenshot::Report _report;
                Error              _err;
                std::atomic<bool>  _busy{false};
};

BMDS_NAMESPACE_END
