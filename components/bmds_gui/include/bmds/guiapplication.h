/**
 * @file      guiapplication.h
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
#include <bmds/icontheme.h>
#include <bmds/inputevent.h>
#include <bmds/list.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/spinlock.h>
#include <bmds/stringview.h>
#include <bmds/sysconf.h>
#include <bmds/windowmanager.h>

BMDS_NAMESPACE_BEGIN

class GuiClipboard;
class GuiWindow;

/**
 * @brief A GUI program's connection to the window system and its event loop
 *        (`devplan/gui.md` §4b, G2).
 *
 * The window manager's callbacks only queue an event here and wake the loop;
 * `exec()`, on the program's thread, dispatches them to the windows and their
 * widgets, runs timers and posted calls, and then asks the window manager to
 * redraw what the handlers changed. Handlers run with the application's lock
 * held and may call the window manager. Drawing runs on the window manager's
 * task: a window takes the lock without waiting, and when a handler holds it
 * asks to be drawn at the next round instead.
 *
 * One per program; it reaches the window system only through
 * `WindowManager`'s public calls, as any program may.
 *
 * @par Thread Safety
 * `post()` and `quit()` from any thread; the rest from the loop's thread (or
 * before `exec()`).
 */
class GuiApplication {
        public:
                /** @brief A call made on the loop's thread. */
                using Callback = void (*)(void *arg);

                /** @brief Windows at once (their slots grow as they are made; a slot is a byte). */
                static constexpr size_t MaxWindows = 255;

                /** @brief Timers at once. */
                static constexpr size_t MaxTimers = 16;

                /** @brief Events queued (motion for a window merged). */
                static constexpr size_t QueueSize = 64;

                /** @brief What `exec()` returns when a signal to the program ends it (128 + SIGINT). */
                static constexpr int InterruptedCode = 130;

                GuiApplication() = default;
                ~GuiApplication();
                GuiApplication(const GuiApplication &) = delete;
                GuiApplication &operator=(const GuiApplication &) = delete;

                /**
                 * @brief Connects to display @p display's window manager.
                 * @return `Ok`, `InvalidState` (no window system: not started),
                 *         `NotSupported` (no such display), or `Full`.
                 */
                Error connect(size_t display);

                /** @brief Returns the window manager (after `connect()`). */
                WindowManager &windowManager() { return *_wm; }

                /** @brief Returns the connection. */
                uint8_t client() const { return _client; }

                /**
                 * @brief Runs the loop until `quit()`, the window manager stops, or
                 *        a signal raises the program's interrupt.
                 * @return `quit()`'s code, 0 when the window manager stopped, or
                 *         `InterruptedCode`.
                 * @blocking
                 */
                int exec();

                /** @brief Ends `exec()` with @p code after the event being handled. */
                void quit(int code);

                /**
                 * @brief Calls @p fn(@p arg) on the loop's thread, with the lock held.
                 * @return `Ok`, or `Full` (the queue is).
                 * @nonblocking
                 */
                Error post(Callback fn, void *arg);

                /**
                 * @brief Calls @p fn(@p arg) after @p intervalMs, and every
                 *        @p intervalMs after if @p repeat.
                 * @return The timer's number (for `stopTimer()`), or `Full`.
                 */
                Result<int> startTimer(uint32_t intervalMs, bool repeat, Callback fn, void *arg);

                /** @brief Stops timer @p id (a stopped or unknown one is fine). */
                void stopTimer(int id);

                /**
                 * @brief Has @p fn told (on the loop's thread) when the window
                 *        manager's list of windows changes; null: no more.
                 */
                Error setWindowsChanged(Callback fn, void *arg);

                /**
                 * @brief Told on the loop's thread that system configuration
                 *        parameter @p id changed, or `SysConf::NoId`: more changed than
                 *        were kept, so anything under the prefix may have.
                 */
                using ConfigChanged = void (*)(void *arg, SysConf::Id id);

                /** @brief Configuration watches at once. */
                static constexpr size_t MaxConfigWatches = 4;

                /**
                 * @brief Has @p fn told (on the loop's thread, the lock held) of each
                 *        change to the system configuration under @p prefix ("" all;
                 *        `devplan/sysconf.md` §12.3). Changes that come together are told
                 *        together, after the events before them.
                 * @return `Ok`, `Invalid` (the prefix, a null @p fn), `Full`, `NoMem`.
                 */
                Error watchConfig(StringView prefix, ConfigChanged fn, void *arg);

                /** @brief Key claims at once. */
                static constexpr size_t MaxGrabs = 8;

                /**
                 * @brief Claims a key for the whole desktop (`WindowManager::grabKey()`:
                 *        a modifier with no modifiers is a tap, as the Windows key):
                 *        @p fn(@p arg) is called on the loop's thread when it is typed.
                 * @return `Ok`, `Exists` (claimed), or `Full`.
                 */
                Error grabKey(uint8_t usage, uint8_t modifiers, Callback fn, void *arg);

                /** @brief Told a message sent to the application's service (on the loop's thread, the lock held). */
                using ServiceMessage = void (*)(void *arg, ByteSpan message);

                /** @brief Services at once. */
                static constexpr size_t MaxServices = 2;

                /**
                 * @brief Names a service of the application (`WindowManager::registerService()`,
                 *        `devplan/filebrowser.md` §4h): each message sent to @p name is
                 *        told to @p fn, in order.
                 * @return `Ok`, `Full`, or the window manager's error (`Exists`: another
                 *         program has the name).
                 */
                Error registerService(StringView name, ServiceMessage fn, void *arg);

                /**
                 * @brief Returns the application's clipboard (`devplan/clipboard.md`
                 *        C13), made the first time; null without the memory. Its copies
                 *        on request are kept when `exec()` returns.
                 */
                GuiClipboard *clipboard();

                /** @brief Returns the application's icons (found and kept on the loop's thread). */
                IconTheme &icons() { return _icons; }

                /** @brief Returns the lock handlers run under (and drawing takes). */
                Mutex &lock() { return _lock; }

                /** @brief Registers @p window (its constructor). @return Its slot, `Full` or `NoMem`. */
                Result<uint8_t> addWindow(GuiWindow &window);

                /** @brief Returns slot @p slot's generation (its events carry it). */
                uint8_t generation(uint8_t slot) const { return slot < _slots.size() ? _slots[slot].generation : 0; }

                /** @brief Forgets @p window (its destructor): its queued events dropped. */
                void removeWindow(GuiWindow &window);

                /** @brief An event for a window, as the window manager's callbacks queue it. */
                struct Event {
                                enum class Type : uint8_t {
                                        Mouse,
                                        Key,
                                        Wheel,
                                        Resized,
                                        Closed,
                                        Gone,
                                        Watch,
                                        Post,
                                        Activated, ///< `width` 1: it has the keys now; 0: it lost them.
                                };
                                Type                      type = Type::Post;
                                uint8_t                   slot = 0; ///< The window's (its generation in `generation`).
                                uint8_t                   generation = 0;
                                WindowManager::MouseEvent mouse;
                                KeyEvent                  key;
                                int16_t                   wheel = 0;
                                uint16_t                  width = 0;
                                uint16_t                  height = 0;
                                Callback                  fn = nullptr;
                                void                     *arg = nullptr;
                };

                /** @brief Queues @p event and wakes the loop (any task, any lock). @nonblocking */
                void queue(const Event &event);

        private:
                struct Timer {
                                Callback fn = nullptr;
                                void    *arg = nullptr;
                                uint64_t dueUs = 0;
                                uint32_t intervalMs = 0;
                                bool     repeat = false;
                                bool     used = false;
                };

                struct Slot {
                                GuiWindow *window = nullptr;
                                uint8_t    generation = 0;
                };

                struct KeyGrab {
                                GuiApplication *app = nullptr;
                                Callback        fn = nullptr;
                                void           *arg = nullptr;
                };

                // A registerService()'s: PSRAM, made when asked for, with room for a message.
                struct Service {
                                GuiApplication *app = nullptr;
                                ServiceMessage  fn = nullptr;
                                void           *arg = nullptr;
                                char            name[WindowManager::MaxServiceName + 1] = {};
                                uint8_t         buffer[WindowManager::MaxServiceMessage];
                };

                // A watchConfig()'s: PSRAM, made when asked for.
                struct ConfigWatch {
                                GuiApplication  *app = nullptr;
                                ConfigChanged    fn = nullptr;
                                void            *arg = nullptr;
                                SysConf::Watcher watcher;
                };

                static void gone(void *arg);
                static void configNotified(void *arg);
                void        runConfig();
                static void keyGrabbed(void *arg, uint8_t usage);
                static void               serviceNotified(void *arg);
                static void               serviceDue(void *arg);
                static void watched(void *arg);
                void        dispatch(const Event &event);
                void        runTimers();
                void        flush(); // the windows' changes, to the window manager (unlocked)
                uint32_t                  waitMs() const;   // until the next timer: WaitForever (none), or Due
                static constexpr uint32_t Due = UINT32_MAX; // waitMs(): a timer is due, no wait

                WindowManager    *_wm = nullptr;
                uint8_t           _client = 0;
                Mutex             _lock;
                SpinLock          _queueLock;
                Semaphore         _wake{1, 0};
                Event             _events[QueueSize];
                size_t            _head = 0;
                size_t            _count = 0;
                bool              _overflowed = false; // a redraw of everything after
                Timer             _timers[MaxTimers];
                List<Slot>        _slots; // by slot, grown as windows are made (never shrunk)
                Callback          _watchFn = nullptr;
                void             *_watchArg = nullptr;
                std::atomic<bool> _quit{false};
                int               _code = 0;
                std::atomic<bool> _gone{false};
                IconTheme         _icons;
                KeyGrab           _grabs[MaxGrabs];
                GuiClipboard     *_clipboard = nullptr; // PSRAM, the first time asked for
                ConfigWatch      *_configWatches[MaxConfigWatches] = {};
                Service          *_services[MaxServices] = {};
                std::atomic<bool> _configChanged{false}; // a watch has news: runConfig() at the loop
};

BMDS_NAMESPACE_END
