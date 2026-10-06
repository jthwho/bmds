/**
 * @file      signals.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/signals.h>
#include <bmds/sys.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The lowest signal in @p set, or 0.
        int lowest(uint32_t set) {
                return set != 0 ? __builtin_ctz(set) + 1 : 0;
        }

} // namespace

// All atomics are sequentially consistent: a sender adds its pending bit
// before raising, and prepare() clears before it looks again, so a signal
// that becomes due during a clear still leaves the interrupt raised. What
// looks at the thread list (and so every send) holds _lock.

void Signals::attach(Thread &thread) {
        Mutex::Locker locker(_lock, WaitForever);
        if (&thread == &_main) {
                _mainGone = false; // a thread that execs goes on as the first
        } else {
                thread.next = _others;
                _others = &thread;
        }
        updateBlocked();
        // A stop or a kill that came before it was on the list.
        if (_stopped.load()) thread.interrupt.hold();
        if (ending() != 0) thread.interrupt.raise();
        return;
}

void Signals::detach(Thread &thread) {
        Mutex::Locker locker(_lock, WaitForever);
        if (&thread == &_main) {
                _mainGone = true;
        } else {
                for (Thread **t = &_others; *t != nullptr; t = &(*t)->next) {
                        if (*t != &thread) continue;
                        *t = thread.next;
                        break;
                }
                thread.next = nullptr;
        }
        thread.pending.store(0);
        updateBlocked();
        return;
}

Signals::Thread &Signals::current() {
        const Interrupt *mine = Interrupt::current();
        Mutex::Locker    locker(_lock, WaitForever);
        for (Thread *t = _others; t != nullptr; t = t->next) {
                if (&t->interrupt == mine) return *t;
        }
        return _main;
}

void Signals::wake(Thread &thread) {
        thread.wake.store(true);
        thread.interrupt.raise();
        return;
}

Signals::Posted Signals::deliverTo(Thread *to, int sig, int code) {
        const uint32_t b = bit(sig);
        Mutex::Locker  locker(_lock, WaitForever);
        // SIGCONT continues whatever its action; it and the stop signals cancel each other's
        // pending ones; SIGKILL continues a stopped process to end it.
        if (sig == BMDS_SIGCONT) {
                clearPending(StopSignals);
                const bool   continued = resume();
                const Posted posted = deliver(to, sig, code);
                return continued ? Posted::Continue : posted;
        }
        if ((b & StopSignals) != 0) clearPending(bit(BMDS_SIGCONT));
        if (sig == BMDS_SIGKILL && _stopped.load()) {
                // Pending before the continue, so its waiters wake to it (resume() raises).
                markPending(_pending, _kernelSent, b, code);
                (void)resume();
                return Posted::Terminate;
        }
        return deliver(to, sig, code);
}

void Signals::markPending(std::atomic<uint32_t> &pending, std::atomic<uint32_t> &kernelSent, uint32_t b, int code) {
        if (code == BMDS_SI_KERNEL) {
                kernelSent.fetch_or(b);
        } else {
                kernelSent.fetch_and(~b);
        }
        pending.fetch_or(b);
        return;
}

Signals::Posted Signals::deliver(Thread *to, int sig, int code) {
        const uint32_t b = bit(sig);
        if ((_ignored.load() & b) != 0) return Posted::Dropped;
        if ((_interrupting.load() & b) != 0) {
                _main.interrupt.raise();
                return Posted::Taken;
        }
        const bool caught = (_caught.load() & b) != 0;
        if (!caught && !endsByDefault(sig) && (b & StopSignals) == 0) return Posted::Dropped;
        // Blocked where it would act: by the thread it was sent to, or by every thread.
        const bool blocked = ((to != nullptr ? to->blocked.load() : _allBlocked.load()) & b) != 0;
        // A stop signal stops at once unless blocked (then at its unblocking: stopSignal()).
        if (!caught && (b & StopSignals) != 0 && !blocked) return stop() ? Posted::Stop : Posted::Dropped;
        // Sent to a thread, it waits for that thread when caught or blocked; otherwise it acts
        // on the process.
        if (to != nullptr && (caught || blocked)) {
                markPending(to->pending, to->kernelSent, b, code);
        } else {
                markPending(_pending, _kernelSent, b, code);
        }
        if (ending() != 0) {
                raiseAll();
                return Posted::Terminate;
        }
        if (caught && !blocked && !_stopped.load()) {
                if (to != nullptr) {
                        to->interrupt.raise();
                } else {
                        raiseFor(b);
                }
        }
        return Posted::Pending;
}

void Signals::raiseFor(uint32_t b) {
        if (!_mainGone && (_main.blocked.load() & b) == 0) {
                _main.interrupt.raise();
                return;
        }
        for (Thread *t = _others; t != nullptr; t = t->next) {
                if ((t->blocked.load() & b) != 0) continue;
                t->interrupt.raise();
                return;
        }
        return;
}

void Signals::raiseAll() {
        _main.interrupt.raise();
        for (Thread *t = _others; t != nullptr; t = t->next) t->interrupt.raise();
        return;
}

void Signals::clearPending(uint32_t set) {
        _pending.fetch_and(~set);
        _main.pending.fetch_and(~set);
        for (Thread *t = _others; t != nullptr; t = t->next) t->pending.fetch_and(~set);
        return;
}

void Signals::updateBlocked() {
        uint32_t all = _mainGone ? ~0u : _main.blocked.load();
        for (const Thread *t = _others; t != nullptr; t = t->next) all &= t->blocked.load();
        _allBlocked.store(all);
        return;
}

bool Signals::stop() {
        bool running = false;
        if (!_stopped.compare_exchange_strong(running, true)) return false;
        _main.interrupt.hold();
        for (Thread *t = _others; t != nullptr; t = t->next) t->interrupt.hold();
        return true;
}

bool Signals::resume() {
        bool stopped = true;
        if (!_stopped.compare_exchange_strong(stopped, false)) return false;
        // What came while it was stopped acts now: raised before its waiters go on, so none of
        // them misses it (and, say, reads the terminal again).
        const bool ends = ending() != 0;
        auto       release = [&](Thread &t) {
                if (ends || dueCaught(t) != 0) t.interrupt.raise();
                t.interrupt.resume();
        };
        release(_main);
        for (Thread *t = _others; t != nullptr; t = t->next) release(*t);
        return true;
}

bool Signals::stopWith(Thread &thread, int sig) {
        Mutex::Locker locker(_lock, WaitForever);
        _pending.fetch_and(~bit(sig));
        thread.pending.fetch_and(~bit(sig));
        return stop();
}

Signals::Action Signals::action(int sig) const {
        const uint32_t b = bit(sig);
        if ((_ignored.load() & b) != 0) return Action::Ignore;
        if ((_caught.load() & b) != 0) return Action::Catch;
        if ((_interrupting.load() & b) != 0) return Action::Interrupt;
        return Action::Default;
}

bool Signals::setAction(int sig, Action action) {
        const uint32_t b = bit(sig);
        if ((b & Unblockable) != 0) return false;
        Mutex::Locker locker(_lock, WaitForever);
        switch (action) {
                case Action::Default:
                        _caught.fetch_and(~b);
                        _ignored.fetch_and(~b);
                        _interrupting.fetch_and(~b);
                        break;
                case Action::Ignore:
                        _caught.fetch_and(~b);
                        _interrupting.fetch_and(~b);
                        _ignored.fetch_or(b);
                        clearPending(b);
                        break;
                case Action::Catch:
                        _ignored.fetch_and(~b);
                        _interrupting.fetch_and(~b);
                        _caught.fetch_or(b);
                        break;
                case Action::Interrupt:
                        _caught.fetch_and(~b);
                        _ignored.fetch_and(~b);
                        _interrupting.fetch_or(b);
                        clearPending(b);
                        break;
        }
        return true;
}

void Signals::setBlocked(Thread &thread, uint32_t set) {
        Mutex::Locker locker(_lock, WaitForever);
        thread.blocked.store(set & ~Unblockable);
        updateBlocked();
        // Sent to it alone and no longer blocked: one that ends or stops the process acts on it.
        const uint32_t acting = thread.pending.load() & ~thread.blocked.load() & ~_caught.load() & ~_ignored.load();
        if (acting != 0) {
                thread.pending.fetch_and(~acting);
                const uint32_t kernel = thread.kernelSent.load() & acting;
                _kernelSent.fetch_or(kernel);
                _kernelSent.fetch_and(~(acting & ~kernel));
                _pending.fetch_or(acting);
        }
        return;
}

uint32_t Signals::fatal() const {
        return ~_caught.load() & ~_ignored.load() & ~DroppedByDefault & ~StopSignals;
}

uint32_t Signals::ending() const {
        // Nothing ends a stopped process but SIGKILL, which continues it first.
        if (_stopped.load()) return 0;
        return _pending.load() & ~_allBlocked.load() & fatal();
}

int Signals::stopSignal(const Thread &thread) const {
        const uint32_t live =
                (_pending.load() & ~_allBlocked.load()) | (thread.pending.load() & ~thread.blocked.load());
        return lowest(live & StopSignals & ~_caught.load() & ~_ignored.load() & ~_interrupting.load());
}

int Signals::terminateSignal() const {
        const uint32_t set = ending();
        return (set & bit(9)) != 0 ? 9 : lowest(set); // SIGKILL first
}

Signals::Taken Signals::take(Thread &thread) {
        Taken taken;
        for (;;) {
                if (_stopped.load()) return taken;
                const uint32_t mask = thread.blocked.load();
                const uint32_t caught = _caught.load();
                const uint32_t dropping = _ignored.load() | (DroppedByDefault & ~caught);
                const uint32_t own = thread.pending.load() & ~mask;
                const uint32_t shared = _pending.load() & ~mask;
                if ((own & dropping) != 0) thread.pending.fetch_and(~(own & dropping));
                if ((shared & dropping) != 0) _pending.fetch_and(~(shared & dropping));
                if (const int sig = terminateSignal(); sig != 0) {
                        taken.signal = sig;
                        taken.code = (_kernelSent.load() & bit(sig)) != 0 ? BMDS_SI_KERNEL : BMDS_SI_USER;
                        taken.terminate = true;
                        return taken;
                }
                const int sig = lowest((own | shared) & caught);
                if (sig == 0) return taken;
                const uint32_t b = bit(sig);
                // Its own first; one of the process's may be taken by another thread meanwhile.
                std::atomic<uint32_t> &from = (own & b) != 0 ? thread.pending : _pending;
                std::atomic<uint32_t> &sent = (own & b) != 0 ? thread.kernelSent : _kernelSent;
                if ((from.fetch_and(~b) & b) == 0) continue;
                taken.signal = sig;
                taken.code = (sent.load() & b) != 0 ? BMDS_SI_KERNEL : BMDS_SI_USER;
                return taken;
        }
}

void Signals::prepare(Thread &thread) {
        if (thread.wake.load() || isDue(thread)) return;
        thread.interrupt.clear();
        if (thread.wake.load() || isDue(thread)) thread.interrupt.raise();
        return;
}

void Signals::inherit(const Signals &parent, const Thread &thread) {
        _caught.store(parent._caught.load());
        _ignored.store(parent._ignored.load());
        _interrupting.store(parent._interrupting.load());
        _main.blocked.store(thread.blocked.load());
        _allBlocked.store(thread.blocked.load());
        _pending.store(0);
        _kernelSent.store(0);
        _stopped.store(false);
        return;
}

void Signals::resetForExec() {
        _caught.store(0);
        _interrupting.store(0);
        return;
}

BMDS_NAMESPACE_END
