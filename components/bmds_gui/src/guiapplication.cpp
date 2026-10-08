/**
 * @file      guiapplication.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A GUI program's event loop (devplan/gui.md §4b, G2).
 */

#include <new>
#include <bmds/deadline.h>
#include <bmds/guiapplication.h>
#include <bmds/guiclipboard.h>
#include <bmds/guiwindow.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

GuiApplication::~GuiApplication() {
        // The watches first: a notification may be running on another task until stop() returns.
        for (ConfigWatch *&w : _configWatches) {
                if (w == nullptr) continue;
                w->~ConfigWatch();
                Memory::free(w);
                w = nullptr;
        }
        if (_clipboard != nullptr) {
                _clipboard->~GuiClipboard();
                Memory::free(_clipboard);
        }
        if (_wm != nullptr && !_gone.load(std::memory_order_acquire)) _wm->disconnect(_client);
}

Error GuiApplication::connect(size_t display) {
        auto [wm, err] = WindowManager::manager(display);
        if (err.isError()) return err;
        auto [client, cerr] = wm->connect(gone, this);
        if (cerr.isError()) return cerr;
        _wm = wm;
        _client = client;
        return Error();
}

void GuiApplication::gone(void *arg) {
        auto *self = static_cast<GuiApplication *>(arg);
        self->_gone.store(true, std::memory_order_release);
        Event event;
        event.type = Event::Type::Gone;
        self->queue(event);
        return;
}

void GuiApplication::watched(void *arg) {
        Event event;
        event.type = Event::Type::Watch;
        static_cast<GuiApplication *>(arg)->queue(event);
        return;
}

Error GuiApplication::watchConfig(StringView prefix, ConfigChanged fn, void *arg) {
        if (fn == nullptr) return Error::Invalid;
        for (ConfigWatch *&slot : _configWatches) {
                if (slot != nullptr) continue;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ConfigWatch));
                if (mem == nullptr) return Error::NoMem;
                auto *w = ::new (mem) ConfigWatch();
                w->app = this;
                w->fn = fn;
                w->arg = arg;
                const Error err = w->watcher.start(prefix, configNotified, w);
                if (err.isError()) {
                        w->~ConfigWatch();
                        Memory::free(w);
                        return err;
                }
                slot = w;
                return Error::Ok;
        }
        return Error::Full;
}

// On the task that changed a value: the loop woken (no queue slot, so nothing is lost when it
// is full).
void GuiApplication::configNotified(void *arg) {
        GuiApplication *app = static_cast<ConfigWatch *>(arg)->app;
        app->_configChanged.store(true, std::memory_order_release);
        app->_wake.release();
        return;
}

// Each watch's changes, to its function (the loop's thread, the lock held).
void GuiApplication::runConfig() {
        for (ConfigWatch *w : _configWatches) {
                if (w == nullptr) continue;
                SysConf::Id id;
                while (w->watcher.next(id)) w->fn(w->arg, id);
                if (w->watcher.overflowed()) w->fn(w->arg, SysConf::NoId);
        }
        return;
}

Error GuiApplication::grabKey(uint8_t usage, uint8_t modifiers, Callback fn, void *arg) {
        if (fn == nullptr) return Error::Invalid;
        for (KeyGrab &g : _grabs) {
                if (g.fn != nullptr) continue;
                g = KeyGrab{this, fn, arg};
                const Error err = _wm->grabKey(_client, usage, modifiers, keyGrabbed, &g);
                if (err.isError()) g = KeyGrab();
                return err;
        }
        return Error::Full;
}

// A claimed key, on the window manager's task: the call posted to the loop.
void GuiApplication::keyGrabbed(void *arg, uint8_t) {
        const KeyGrab &g = *static_cast<const KeyGrab *>(arg);
        (void)g.app->post(g.fn, g.arg);
        return;
}

Error GuiApplication::setWindowsChanged(Callback fn, void *arg) {
        _watchFn = fn;
        _watchArg = arg;
        return _wm->watch(_client, fn != nullptr ? watched : nullptr, this);
}

void GuiApplication::queue(const Event &event) {
        {
                SpinLock::Locker lock(_queueLock);
                // Motion for the window of the last event, motion too: merged.
                if (_count > 0 && event.type == Event::Type::Mouse &&
                    event.mouse.type == WindowManager::MouseEvent::Type::Motion) {
                        Event &last = _events[(_head + _count - 1) % QueueSize];
                        if (last.type == Event::Type::Mouse && last.slot == event.slot &&
                            last.mouse.type == WindowManager::MouseEvent::Type::Motion) {
                                last = event;
                                return; // the loop is awake already
                        }
                }
                if (_count == QueueSize) {
                        _overflowed = true;
                } else {
                        _events[(_head + _count) % QueueSize] = event;
                        ++_count;
                }
        }
        _wake.release();
        return;
}

Error GuiApplication::post(Callback fn, void *arg) {
        if (fn == nullptr) return Error::Invalid;
        {
                SpinLock::Locker lock(_queueLock);
                if (_count == QueueSize) return Error::Full;
                Event &e = _events[(_head + _count) % QueueSize];
                e = Event();
                e.type = Event::Type::Post;
                e.fn = fn;
                e.arg = arg;
                ++_count;
        }
        _wake.release();
        return Error();
}

void GuiApplication::quit(int code) {
        _code = code;
        _quit.store(true, std::memory_order_release);
        _wake.release();
        return;
}

Result<uint8_t> GuiApplication::addWindow(GuiWindow &window) {
        for (size_t i = 0; i < _slots.size(); ++i) {
                if (_slots[i].window != nullptr) continue;
                _slots[i].window = &window;
                ++_slots[i].generation;
                return makeResult(static_cast<uint8_t>(i));
        }
        if (_slots.size() == MaxWindows) return makeError<uint8_t>(Error::Full);
        if (_slots.append(Slot{&window, 1}).isError()) return makeError<uint8_t>(Error::NoMem);
        return makeResult(static_cast<uint8_t>(_slots.size() - 1));
}

void GuiApplication::removeWindow(GuiWindow &window) {
        for (Slot &s : _slots)
                if (s.window == &window) {
                        s.window = nullptr;
                        ++s.generation; // its queued events go nowhere
                }
        return;
}

Result<int> GuiApplication::startTimer(uint32_t intervalMs, bool repeat, Callback fn, void *arg) {
        if (fn == nullptr) return makeError<int>(Error::Invalid);
        for (size_t i = 0; i < MaxTimers; ++i) {
                Timer &t = _timers[i];
                if (t.used) continue;
                t.fn = fn;
                t.arg = arg;
                t.intervalMs = intervalMs;
                t.repeat = repeat;
                t.dueUs = System::uptimeUs() + uint64_t(intervalMs) * 1000;
                t.used = true;
                return makeResult(static_cast<int>(i));
        }
        return makeError<int>(Error::Full);
}

void GuiApplication::stopTimer(int id) {
        if (id >= 0 && size_t(id) < MaxTimers) _timers[id] = Timer();
        return;
}

void GuiApplication::runTimers() {
        const uint64_t now = System::uptimeUs();
        for (Timer &t : _timers) {
                if (!t.used || t.dueUs > now) continue;
                const Callback fn = t.fn;
                void          *arg = t.arg;
                if (t.repeat) {
                        // The next one on the beat, a late one not run twice.
                        const uint64_t step = uint64_t(t.intervalMs > 0 ? t.intervalMs : 1) * 1000;
                        t.dueUs += step;
                        if (t.dueUs <= now) t.dueUs = now + step;
                } else {
                        t = Timer();
                }
                fn(arg);
        }
        return;
}

uint32_t GuiApplication::waitMs() const {
        uint64_t       next = UINT64_MAX;
        const uint64_t now = System::uptimeUs();
        for (const Timer &t : _timers)
                if (t.used && t.dueUs < next) next = t.dueUs;
        if (next == UINT64_MAX) return WaitForever;
        if (next <= now) return Due;
        const uint64_t ms = (next - now + 999) / 1000;
        return ms > 60000 ? 60000 : static_cast<uint32_t>(ms);
}

void GuiApplication::dispatch(const Event &event) {
        switch (event.type) {
                case Event::Type::Post:
                        if (event.fn != nullptr) event.fn(event.arg);
                        return;
                case Event::Type::Gone: quit(0); return;
                case Event::Type::Watch:
                        if (_watchFn != nullptr) _watchFn(_watchArg);
                        return;
                default: break;
        }
        if (event.slot >= _slots.size()) return;
        const Slot &s = _slots[event.slot];
        if (s.window == nullptr || s.generation != event.generation) return; // gone since
        s.window->handle(event);
        return;
}

// Each window's changes taken under the application's lock and given to the window manager after
// it: called with the lock held, the window manager's lock would be waited for during a round, whose
// redraws would find the application busy and put off theirs.
void GuiApplication::flush() {
        for (size_t i = 0;; ++i) {
                Region  dirty;
                uint8_t id = 0;
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        if (i >= _slots.size()) break;
                        if (_slots[i].window != nullptr) id = _slots[i].window->takeDirty(dirty);
                }
                if (id == 0) continue;
                for (const Rect &r : dirty.rects()) (void)_wm->invalidate(id, r);
        }
        return;
}

int GuiApplication::exec() {
        while (!_quit.load(std::memory_order_acquire)) {
                const uint32_t ms = waitMs();
                // WaitForever when no timer waits, none when one is due; a signal to the program (its
                // interrupt) ends it.
                if (ms != Due && Interrupt::waitCurrent(_wake, Deadline(ms)) == Error::Interrupted) {
                        _code = InterruptedCode;
                        break;
                }
                {
                        Mutex::Locker lock(_lock, WaitForever);
                        for (;;) {
                                Event event;
                                bool  overflowed = false;
                                {
                                        SpinLock::Locker q(_queueLock);
                                        if (_count == 0) break;
                                        event = _events[_head];
                                        _head = (_head + 1) % QueueSize;
                                        --_count;
                                        overflowed = _overflowed;
                                        _overflowed = false;
                                }
                                if (overflowed) {
                                        // Events were lost: everything redrawn, at least.
                                        for (const Slot &s : _slots)
                                                if (s.window != nullptr) s.window->update();
                                }
                                dispatch(event);
                                if (_quit.load(std::memory_order_acquire)) break;
                        }
                        if (_configChanged.exchange(false, std::memory_order_acq_rel)) runConfig();
                        runTimers();
                }
                flush();
        }
        // Copies on request outlive the application: rendered now, while its objects exist.
        if (_clipboard != nullptr) {
                Mutex::Locker lock(_lock, WaitForever);
                _clipboard->keep();
        }
        return _code;
}

GuiClipboard *GuiApplication::clipboard() {
        if (_clipboard != nullptr) return _clipboard;
        void *mem = Memory::alloc(MemSpace::Psram, sizeof(GuiClipboard));
        if (mem != nullptr) _clipboard = ::new (mem) GuiClipboard(*this);
        return _clipboard;
}

BMDS_NAMESPACE_END
