/**
 * @file      apps.h
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
#include <bmds/desktopentry.h>
#include <bmds/error.h>
#include <bmds/folderwatches.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/thread.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The installed applications (`devplan/apps.md` §4): every `NAME.app` folder in the
 *        roots, each described by its `app.desktop`, found by looking and kept current by
 *        watching.
 *
 * A scan reads each root in order, its `*.app` folders sorted by name, and each folder's
 * `app.desktop`. An application's id is its `X-BMDS-Id` (else the folder's name without
 * `.app`), its command its `X-BMDS-Command` (else the id), its program `Exec`'s first word, a
 * file in the folder. The first of an id or a command wins; a folder that can't be used (no
 * `app.desktop`, no program, a name taken...) is kept as a problem, with why. `Hidden=true`
 * hides a folder without a problem.
 *
 * Watches on the roots (a root not there: its parent) and on each folder found ask for a scan,
 * done on the service's thread once the changes have settled (a folder being copied in), as a
 * change of roots does. Optionally (`Config::links`) each scan's commands become `/apps`'s links
 * (`Vfs::setAppLinks()`).
 *
 * @par Thread Safety
 * `start()` and `stop()` from the owner; the rest from any thread. The notify is called on the
 * service's thread.
 */
class Apps {
        public:
                /** @brief The folders' extension. */
                static constexpr StringView Extension = ".app";

                /** @brief The description in each folder. */
                static constexpr StringView DescriptionFile = "app.desktop";

                /** @brief The largest `app.desktop` read. */
                static constexpr size_t MaxDescription = 4096;

                /** @brief How long changes must settle before a scan, and the longest a scan waits. */
                static constexpr unsigned int SettleMs = 300;
                static constexpr unsigned int MaxSettleMs = 2000;

                /** @brief The thread's stack (PSRAM), where every scan runs. */
                static constexpr size_t StackBytes = 12 * 1024;

                /** @brief An application. */
                struct App {
                                DesktopEntry entry; ///< Its `app.desktop`.
                                char         id[DesktopEntry::MaxId + 1] = {};
                                char         command[DesktopEntry::MaxId + 1] = {};
                                String       folder{MemSpace::Psram};  ///< Its folder's path.
                                String       program{MemSpace::Psram}; ///< Its program's path.
                                uint8_t      root = 0;                 ///< Its root's place in the list.
                };

                /** @brief A folder not used, and why. */
                struct Problem {
                                String      folder{MemSpace::Psram};
                                const char *why = ""; ///< Static text.
                };

                /** @brief Writes the roots to scan (`;`-separated) into @p out: each scan asks. */
                using Roots = Error (*)(void *arg, String &out);

                /** @brief Told after a scan that changed what is installed. */
                using Notify = void (*)(void *arg);

                /** @brief How it runs. */
                struct Config {
                                Roots       roots = nullptr;
                                void       *rootsArg = nullptr;
                                Notify      notify = nullptr; ///< May be null.
                                void       *notifyArg = nullptr;
                                bool        links = false; ///< `/apps` follows the scans.
                                const char *name = "apps"; ///< The thread's.
                };

                Apps() = default;
                ~Apps() { stop(); }
                Apps(const Apps &) = delete;
                Apps &operator=(const Apps &) = delete;

                /**
                 * @brief Starts the thread, which scans once (the list is ready when this
                 *        returns), then again on changes.
                 * @return `Ok`, `InvalidState` (started), `Invalid` (no roots function), `NoMem`,
                 *         or the thread's error.
                 */
                Error start(const Config &config);

                /** @brief Stops the thread and the watches. Idempotent. @blocking */
                void stop();

                /** @brief Asks for a scan (the roots may have changed). @nonblocking */
                void rescan();

                /** @brief Returns the count of scans that changed what is installed. */
                uint32_t generation() const { return _generation.load(std::memory_order_acquire); }

                /** @brief Called with each application, in order, under the lock: must not call it. */
                using Visit = void (*)(void *arg, const App &app);

                /** @brief Called with each problem, under the lock: must not call it. */
                using VisitProblem = void (*)(void *arg, const Problem &problem);

                /** @brief Calls @p fn with each application. */
                void visit(Visit fn, void *arg);

                /** @brief Calls @p fn with each folder not used. */
                void visitProblems(VisitProblem fn, void *arg);

                /** @brief Returns how many are installed. */
                size_t count();

                /**
                 * @brief Writes the id of the application in folder @p folder (a path as
                 *        scanned, compared without case: FAT's) into @p out.
                 * @return True if one is installed there; else @p out is left as it was.
                 */
                bool idOf(StringView folder, char (&out)[DesktopEntry::MaxId + 1]);

                /**
                 * @brief Returns true if @p name can be an id or a command: 1 to
                 *        `DesktopEntry::MaxId` letters, digits, `.`, `_`, `+` or `-`, not
                 *        beginning with `.` or `-`.
                 */
                static bool isValidName(StringView name);

                /** @brief Returns true if @p name (a folder's) is an application's: `NAME.app`, any case. */
                static bool isFolderName(StringView name);

                /**
                 * @brief The system's applications (`init()`), or null.
                 * @isrsafe No; @nonblocking.
                 */
                static Apps *system();

                /**
                 * @brief Boot stage: the system's applications, from the `apps/path` setting,
                 *        `/apps` following them (target only).
                 * @return `Ok` (a failure to start is logged: no applications), or `NoMem`.
                 */
                static Error init();

        private:
                struct Scan; // one scan's results, built before they replace these

                void  run();
                void  scan();
                void  publish(Scan &scan);
                Error watchLocked(StringView path); // a folder to watch this scan
                void  unwatchLocked(StringView path);

                static void threadEntry(void *arg);
                static void wakeEntry(void *arg);

                Config                _config;
                Mutex                 _lock; // the lists
                List<App>             _apps{MemSpace::Psram};
                List<Problem>         _problems{MemSpace::Psram};
                uint64_t              _hash = 0; // what is installed, to see a change
                std::atomic<uint32_t> _generation{0};

                // The thread's.
                FolderWatches     _watches{wakeEntry, this};
                List<String>      _watched{MemSpace::Psram}; // the folders the last scan watched
                List<String>      _seen{MemSpace::Psram};    // this scan's
                Semaphore         _wake{64, 0};
                Semaphore         _scanned{1, 0}; // the first scan done
                std::atomic<bool> _stopping{false};
                Thread            _thread;
                bool              _started = false;
};

BMDS_NAMESPACE_END
