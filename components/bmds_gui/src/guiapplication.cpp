/**
 * @file      guiapplication.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A GUI program's event loop (devplan/gui.md §4b, G2).
 */

#include <cstring>
#include <new>
#include <bmds/deadline.h>
#include <bmds/guiapplication.h>
#include <bmds/log.h>
#include <bmds/guiclipboard.h>
#include <bmds/guiwindow.h>
#include <bmds/interrupt.h>
#include <bmds/memory.h>
#include <bmds/system.h>
#include <bmds/tooltip.h>

BMDS_NAMESPACE_BEGIN

GuiApplication::~GuiApplication() {
        GuiStyle::unfollow(*this); // no restyle posted from here on
        // Its tooltip is one of its windows: gone while the application is whole.
        if (_toolTip != nullptr) {
                _toolTip->~ToolTip();
                Memory::free(_toolTip);
                _toolTip = nullptr;
        }
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
        // After the connection: no notification can come for them now.
        for (Service *&s : _services) {
                Memory::free(s);
                s = nullptr;
        }
}

Error GuiApplication::connect(size_t display) {
        auto [wm, err] = WindowManager::manager(display);
        if (err.isError()) return err;
        auto [client, cerr] = wm->connect(gone, this);
        if (cerr.isError()) return cerr;
        _wm = wm;
        _client = client;
        if (Error ferr = GuiStyle::follow(*this); ferr.isError())
                bmdsWarn("gui: an application won't restyle when the fonts change: %s", ferr.desc());
        return Error();
}

void GuiApplication::restyle() {
        for (Slot &slot : _slots)
                if (slot.window != nullptr) slot.window->restyle();
        return;
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

Error GuiApplication::registerService(StringView name, ServiceMessage fn, void *arg) {
        if (fn == nullptr || name.size() > WindowManager::MaxServiceName) return Error::Invalid;
        for (Service *&slot : _services) {
                if (slot != nullptr) continue;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Service));
                if (mem == nullptr) return Error::NoMem;
                auto *s = ::new (mem) Service();
                s->app = this;
                s->fn = fn;
                s->arg = arg;
                std::memcpy(s->name, name.data(), name.size());
                const Error err = _wm->registerService(_client, name, serviceNotified, s);
                if (err.isError()) {
                        Memory::free(s);
                        return err;
                }
                slot = s;
                return Error::Ok;
        }
        return Error::Full;
}

// A message waits (the sender's task, the window manager locked): taken on the loop. A post
// lost to a full queue loses nothing: the next message's post takes it too.
void GuiApplication::serviceNotified(void *arg) {
        auto *s = static_cast<Service *>(arg);
        (void)s->app->post(serviceDue, s);
        return;
}

// Every message waiting, told in order (the loop's thread, the lock held).
void GuiApplication::serviceDue(void *arg) {
        auto *s = static_cast<Service *>(arg);
        for (;;) {
                auto [n, err] = s->app->_wm->takeServiceMessage(s->app->_client, StringView(s->name),
                                                                WritableByteSpan(s->buffer, sizeof(s->buffer)));
                if (err.isError() || n == 0) return;
                s->fn(s->arg, ByteSpan(s->buffer, n));
        }
}

Error GuiApplication::unregisterService(StringView name) {
        for (Service *&slot : _services) {
                if (slot == nullptr || StringView(slot->name) != name) continue;
                const Error err = _wm->unregisterService(_client, name);
                // No notification now: a post of it waiting goes, then the record.
                cancelPosts(slot);
                Memory::free(slot);
                slot = nullptr;
                return err;
        }
        return Error::NotFound;
}

Error GuiApplication::watchServices(Callback fn, void *arg) {
        if (fn == nullptr) return Error::Invalid;
        for (Watcher &w : _serviceWatches) {
                if (w.fn != nullptr) continue;
                if (!_watchingServices) {
                        const Error err = _wm->watchServices(_client, servicesNotified, this);
                        if (err.isError()) return err;
                        _watchingServices = true;
                }
                w = Watcher{fn, arg};
                return Error::Ok;
        }
        return Error::Full;
}

void GuiApplication::unwatchServices(Callback fn, void *arg) {
        for (Watcher &w : _serviceWatches)
                if (w.fn == fn && w.arg == arg) w = Watcher();
        return;
}

// A service named or gone (another task, the window manager locked): the watches told on the
// loop, once for any number of changes before it runs.
void GuiApplication::servicesNotified(void *arg) {
        auto *self = static_cast<GuiApplication *>(arg);
        if (self->_servicesPosted.exchange(true, std::memory_order_acq_rel)) return;
        if (self->post(servicesDue, self).isError()) self->_servicesPosted.store(false, std::memory_order_release);
        return;
}

void GuiApplication::servicesDue(void *arg) {
        auto *self = static_cast<GuiApplication *>(arg);
        self->_servicesPosted.store(false, std::memory_order_release);
        for (size_t i = 0; i < MaxServiceWatches; ++i) {
                const Watcher w = self->_serviceWatches[i]; // one may stop itself
                if (w.fn != nullptr) w.fn(w.arg);
        }
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
                // A drag's motion over the same window, likewise.
                if (_count > 0 && event.type == Event::Type::Drop &&
                    event.drop.type == WindowManager::DropEvent::Type::Motion) {
                        Event &last = _events[(_head + _count - 1) % QueueSize];
                        if (last.type == Event::Type::Drop && last.slot == event.slot &&
                            last.drop.type == WindowManager::DropEvent::Type::Motion) {
                                last = event;
                                return;
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

void GuiApplication::cancelPosts(void *arg) {
        SpinLock::Locker lock(_queueLock);
        for (size_t i = 0; i < _count; ++i) {
                Event &e = _events[(_head + i) % QueueSize];
                if (e.type == Event::Type::Post && e.arg == arg) e.fn = nullptr; // made a no-op
        }
        return;
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
                (void)_wm->invalidate(id, dirty.rects()); // in one round
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
        // Named after the application for `clip list` (until it names it otherwise).
        if (_clipboard != nullptr && _applicationName[0] != '\0') _clipboard->setName(applicationName());
        return _clipboard;
}

void GuiApplication::setApplicationName(StringView name) {
        size_t n = name.size() < sizeof(_applicationName) - 1 ? name.size() : sizeof(_applicationName) - 1;
        if (n < name.size())
                while (n > 0 && (static_cast<uint8_t>(name[n]) & 0xc0u) == 0x80u) --n; // a whole character
        std::memcpy(_applicationName, name.data(), n);
        _applicationName[n] = '\0';
        return;
}

// ---- Tooltips (devplan/gui.md §4s)

void GuiApplication::hoverToolTip(Widget *widget, int screenX, int screenY) {
        if (_toolTip == nullptr) {
                if (widget == nullptr || widget->toolTip().isEmpty()) return;
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(ToolTip));
                if (mem == nullptr) return;
                _toolTip = ::new (mem) ToolTip(*this);
        }
        _toolTip->hover(widget, screenX, screenY);
        return;
}

void GuiApplication::hideToolTip() {
        if (_toolTip != nullptr) _toolTip->hide();
        return;
}

void GuiApplication::toolTipChanged(Widget *widget) {
        if (_toolTip != nullptr) _toolTip->changed(widget);
        return;
}

void GuiApplication::forgetToolTip(Widget *widget) {
        if (_toolTip != nullptr) _toolTip->forget(widget);
        return;
}

void GuiApplication::beginDrag(uint32_t drag, Widget *widget, GuiWindow &window) {
        _dragId = drag;
        _dragSource = widget;
        _dragSlot = window.slot();
        _dragGeneration = window.slotGeneration();
        return;
}

void GuiApplication::forgetDragSource(Widget *widget) {
        for (Widget *p = _dragSource; p != nullptr; p = p->parent()) {
                if (p == widget) {
                        _dragSource = nullptr;
                        break;
                }
        }
        return;
}

// On the window manager's task: the end, as an event for the source's window (dropped if it went).
void GuiApplication::dragEndedEntry(void *arg, uint32_t drag, DropAction done) {
        auto *self = static_cast<GuiApplication *>(arg);
        Event e;
        e.type = Event::Type::DragEnded;
        e.drop.drag = drag;
        e.width = uint16_t(done);
        {
                SpinLock::Locker lock(self->_queueLock);
                e.slot = self->_dragSlot;
                e.generation = self->_dragGeneration;
        }
        self->queue(e);
        return;
}

BMDS_NAMESPACE_END
