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
// that becomes due during a clear still leaves the interrupt raised.

Signals::Posted Signals::post(int sig, int code) {
        const uint32_t b = bit(sig);
        if ((_ignored.load() & b) != 0) return Posted::Dropped;
        if ((_interrupting.load() & b) != 0) {
                _interrupt.raise();
                return Posted::Taken;
        }
        if ((_caught.load() & b) == 0 && !endsByDefault(sig)) return Posted::Dropped;
        if (code == BMDS_SI_KERNEL) {
                _kernelSent.fetch_or(b);
        } else {
                _kernelSent.fetch_and(~b);
        }
        _pending.fetch_or(b);
        if (ending() != 0) {
                _interrupt.raise();
                return Posted::Terminate;
        }
        if ((dueCaught() & b) != 0) _interrupt.raise();
        return Posted::Pending;
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
                        _pending.fetch_and(~b);
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
                        _pending.fetch_and(~b);
                        break;
        }
        return true;
}

void Signals::setBlocked(uint32_t set) {
        _blocked.store(set & ~Unblockable);
        return;
}

uint32_t Signals::ending() const {
        const uint32_t live = _pending.load() & ~_blocked.load();
        return live & ~_caught.load() & ~_ignored.load() & ~DroppedByDefault;
}

int Signals::terminateSignal() const {
        const uint32_t set = ending();
        return (set & bit(9)) != 0 ? 9 : lowest(set); // SIGKILL first
}

Signals::Taken Signals::take() {
        Taken          taken;
        const uint32_t live = _pending.load() & ~_blocked.load();
        const uint32_t caught = _caught.load();
        const uint32_t dropped = live & (_ignored.load() | (DroppedByDefault & ~caught));
        if (dropped != 0) _pending.fetch_and(~dropped);
        if (const int sig = terminateSignal(); sig != 0) {
                taken.signal = sig;
                taken.code = (_kernelSent.load() & bit(sig)) != 0 ? BMDS_SI_KERNEL : BMDS_SI_USER;
                taken.terminate = true;
                return taken;
        }
        const int sig = lowest(live & caught);
        if (sig == 0) return taken;
        _pending.fetch_and(~bit(sig));
        taken.signal = sig;
        taken.code = (_kernelSent.load() & bit(sig)) != 0 ? BMDS_SI_KERNEL : BMDS_SI_USER;
        return taken;
}

void Signals::prepare() {
        if (isDue()) return;
        _interrupt.clear();
        if (isDue()) _interrupt.raise();
        return;
}

void Signals::inherit(const Signals &parent) {
        _caught.store(parent._caught.load());
        _ignored.store(parent._ignored.load());
        _interrupting.store(parent._interrupting.load());
        _blocked.store(parent._blocked.load());
        _pending.store(0);
        _kernelSent.store(0);
        return;
}

void Signals::resetForExec() {
        _caught.store(0);
        _interrupting.store(0);
        return;
}

BMDS_NAMESPACE_END
