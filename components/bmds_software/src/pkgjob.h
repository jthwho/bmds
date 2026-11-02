/**
 * @file      pkgjob.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/list.h>
#include <bmds/pkgrecord.h>
#include <bmds/programlauncher.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

class GuiApplication;

/**
 * @brief One `pkg --json` command run for a window (devplan/packages.md §24): `/bin/sh -c
 *        'pkg --json ARGS'` started through the program's launcher, its records read on a thread
 *        of its own and handed to the window on the loop's thread, one at a time, then its end.
 *
 * The first-run window and the store use it: they drive `pkg` through its machine-readable
 * output, as any program could, not through a package API.
 *
 * @par Thread Safety
 * `start()`, `isRunning()` and the destructor on the loop's thread; the handlers are called there.
 */
class PkgJob {
        public:
                /** @brief A record `pkg` wrote (valid during the call). */
                using RecordHandler = void (*)(void *arg, const PkgRecord &record);
                /** @brief `pkg` ended: its exit status (0: it did what it was asked). */
                using FinishedHandler = void (*)(void *arg, int status);

                explicit PkgJob(GuiApplication &app);
                /** @brief Ends a `pkg` still running (`SIGTERM`) and waits for the thread. */
                ~PkgJob();

                PkgJob(const PkgJob &) = delete;
                PkgJob &operator=(const PkgJob &) = delete;

                /** @brief Has @p onRecord and @p onFinished told (with @p arg). */
                void setHandlers(RecordHandler onRecord, FinishedHandler onFinished, void *arg);

                /**
                 * @brief Runs `pkg --json` with @p args (shell words, quoted by the caller), started
                 *        with @p context's launcher and environment.
                 * @return `Ok`; `InvalidState` while one runs; `NotSupported` without a launcher or a process;
                 *         the launcher's or the thread's error.
                 */
                Error start(const BuiltinProgram::Context &context, StringView args);

                /** @brief Returns true from `start()` until the finished handler has been called. */
                bool isRunning() const { return _running; }

        private:
                static void workerMain(void *arg);
                static void deliver(void *arg);
                static void finished(void *arg);
                void        handOver(); // the worker: _record to the loop, waiting for it taken

                GuiApplication   &_app;
                ProgramLauncher  *_launcher = nullptr;
                ProcessControl   *_process = nullptr; // the program's: takes pkg's end
                RecordHandler     _onRecord = nullptr;
                FinishedHandler   _onFinished = nullptr;
                void             *_arg = nullptr;
                Thread            _worker;
                FileHandle::Ptr   _output; // pkg's stdout, read by the worker
                int32_t           _pid = -1;
                int               _status = 0;
                bool              _running = false;
                bool              _started = false; // the worker, to join
                std::atomic<bool> _stopping{false};
                Semaphore         _taken; // the loop took the record handed over
                PkgRecord         _record;
};

BMDS_NAMESPACE_END
