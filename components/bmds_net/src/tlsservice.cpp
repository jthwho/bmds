/**
 * @file      tlsservice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `tls` thread (devplan/network.md §35c): the handshaking sockets, each with the service's
 * semaphore in its readiness list, stepped whenever any of them may have moved; the session
 * count and the `net.tls` statistics. The settings are the platform's (tlssettings.h).
 */

#include <atomic>
#include <cstdint>
#include <bmds/clock.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/socket.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>
#include <bmds/thread.h>
#include <bmds/tlsservice.h>
#include <bmds/waitlist.h>
#include "tlssettings.h"

BMDS_NAMESPACE_BEGIN

namespace TlsService {

        namespace {

                // The most sockets handshaking at once: `net/tls/sessions`' largest.
                constexpr size_t MaxHandshakes = 64;

                // The thread's life with nothing to do.
                constexpr int64_t IdleUs = 30'000'000;

                // Its stack: 2.5 KB at the deepest handshake measured (§35h, an RSA-4096 chain).
                constexpr size_t StackBytes = 6144;

                // The programs' own (CONFIG_BMDS_SHELL_PRIORITY): a handshake shares the CPU with them.
                constexpr unsigned Priority = 2;

                int64_t nowUs() {
                        return Clock::monotonicNs() / 1000;
                }

                // No deadline: a flush waits as long as TCP does.
                constexpr int64_t Never = INT64_MAX;

                struct Entry {
                                Socket *socket = nullptr;
                                int64_t deadlineUs = Never;
                                size_t  slot = WaitList::NoSlot; // ours in its readiness list
                                bool    handshake = true;        // else a record's rest to send
                };

                struct State {
                                Mutex                 lock;
                                Semaphore             wake{1, 0}; // any handshaking socket's readiness, or a new one
                                Semaphore             stepDone{1, 0}; // a step `remove()` waits for has ended
                                Entry                 entries[MaxHandshakes];
                                size_t                count = 0;          // handshaking or flushing
                                size_t                sessions = 0;       // TLS sockets
                                Socket               *stepping = nullptr; // its step in progress, the lock not held
                                bool                  removeWaiting = false;
                                bool                  running = false;
                                int64_t               idleSinceUs = 0;
                                Thread                thread;
                                std::atomic<uint32_t> handshakes{0};
                                std::atomic<uint32_t> failures{0};
                                std::atomic<uint64_t> bytesIn{0};
                                std::atomic<uint64_t> bytesOut{0};
                                Stats::Group          group;
                                State();
                };

                using Unit = Stat::Unit;
                using Kind = Stat::Kind;

                constexpr Stat::Desc Descs[] = {
                        {"sessions", Unit::Count, Kind::Level, 0, {}, "", "TLS sockets open (handshaking or not)"},
                        {"handshaking", Unit::Count, Kind::Level, 0, {}, "", "TLS sockets in their handshake"},
                        {"handshakes", Unit::Count, Kind::Total, 0, {}, "", "Handshakes finished well"},
                        {"failures", Unit::Count, Kind::Total, 0, {}, "", "Handshakes that failed"},
                        {"rx_bytes", Unit::Bytes, Kind::Total, 0, {}, "", "Bytes received through TLS (plaintext)"},
                        {"tx_bytes", Unit::Bytes, Kind::Total, 0, {}, "", "Bytes sent through TLS (plaintext)"},
                };

                void readStats(void *, Span<int64_t> v) {
                        const Counts c = counts();
                        v[0] = c.sessions;
                        v[1] = c.handshaking;
                        v[2] = c.handshakes;
                        v[3] = c.failures;
                        v[4] = int64_t(c.bytesIn);
                        v[5] = int64_t(c.bytesOut);
                        return;
                }

                State::State() : group(Descs, sizeof(Descs) / sizeof(Descs[0]), readStats, nullptr) {}

                constinit StaticObject<State> state;

                // Takes entry @p i out (lock held): its slot in the socket's list first.
                void erase(State &s, size_t i) {
                        s.entries[i].socket->readinessWaiters()->remove(s.entries[i].slot);
                        for (size_t j = i + 1; j < s.count; ++j) s.entries[j - 1] = s.entries[j];
                        --s.count;
                        if (s.count == 0) s.idleSinceUs = nowUs();
                        return;
                }

                size_t find(const State &s, const Socket *socket) {
                        for (size_t i = 0; i < s.count; ++i)
                                if (s.entries[i].socket == socket) return i;
                        return SIZE_MAX;
                }

                // One pass: each handshaking socket stepped once, the lock not held meanwhile.
                void stepAll(State &s) {
                        size_t i = 0;
                        for (;;) {
                                Socket *socket = nullptr;
                                bool    late = false;
                                bool    handshake = false;
                                {
                                        Mutex::Locker lock(s.lock, WaitForever);
                                        if (i >= s.count) return;
                                        socket = s.entries[i].socket;
                                        late = nowUs() >= s.entries[i].deadlineUs;
                                        handshake = s.entries[i].handshake;
                                        s.stepping = socket;
                                }
                                const Socket::TlsStep step = socket->stepTls(late);
                                Mutex::Locker         lock(s.lock, WaitForever);
                                s.stepping = nullptr;
                                const size_t at = find(s, socket);
                                if (at == SIZE_MAX) {
                                        // Removed meanwhile: what was after it moved down to i.
                                } else if (step == Socket::TlsStep::Waiting) {
                                        i = at + 1;
                                } else {
                                        erase(s, at);
                                        if (handshake && step == Socket::TlsStep::Done)
                                                ++s.handshakes;
                                        else if (handshake)
                                                ++s.failures;
                                }
                                if (s.removeWaiting) {
                                        s.removeWaiting = false;
                                        s.stepDone.release();
                                }
                        }
                }

                void run(void *) {
                        State &s = *state;
                        for (;;) {
                                unsigned int waitMs;
                                {
                                        Mutex::Locker lock(s.lock, WaitForever);
                                        const int64_t now = nowUs();
                                        int64_t       until;
                                        if (s.count == 0) {
                                                until = s.idleSinceUs + IdleUs;
                                                if (now >= until) {
                                                        // Detached under the lock: `add()` then starts
                                                        // another with the same Thread.
                                                        s.running = false;
                                                        s.thread.detach();
                                                        return;
                                                }
                                        } else {
                                                until = s.entries[0].deadlineUs;
                                                for (size_t i = 1; i < s.count; ++i)
                                                        if (s.entries[i].deadlineUs < until)
                                                                until = s.entries[i].deadlineUs;
                                        }
                                        // At least 1 ms (0 would be for ever); a late handshake
                                        // is stepped at once; flushes alone wait for a wake.
                                        if (until == Never)
                                                waitMs = WaitForever;
                                        else
                                                waitMs = until > now ? unsigned((until - now + 999) / 1000) : 1;
                                }
                                (void)s.wake.acquire(waitMs);
                                stepAll(s);
                        }
                }

        } // namespace

        Error init() {
                State &s = state.create();
                s.idleSinceUs = nowUs();
                s.group.setName("net.tls");
                (void)Stats::add(s.group); // counters only: TLS works without them
                return declareSettings();
        }

        namespace {

                // An entry for @p socket, the thread started if it isn't (lock held).
                Error enter(State &s, Socket &socket, bool handshake, int64_t deadlineUs) {
                        if (s.count == MaxHandshakes) return Error::Full;
                        const size_t slot = socket.readinessWaiters()->add(s.wake);
                        if (slot == WaitList::NoSlot) return Error::Full;
                        if (!s.running) {
                                Thread::Config config;
                                config.name = "tls";
                                config.stackBytes = StackBytes;
                                config.priority = Priority;
                                config.stackSpace = MemSpace::Psram; // a handshake writes no flash
                                config.kernel = true;
                                if (s.thread.start(config, run, nullptr).isError()) {
                                        socket.readinessWaiters()->remove(slot);
                                        return Error::NoMem;
                                }
                                s.running = true;
                        }
                        Entry &e = s.entries[s.count++];
                        e.socket = &socket;
                        e.slot = slot;
                        e.handshake = handshake;
                        e.deadlineUs = deadlineUs;
                        return Error::Ok;
                }

        } // namespace

        Error add(Socket &socket) {
                if (!state.isCreated()) return Error::NotSupported;
                const Settings settings = TlsService::settings();
                State         &s = *state;
                {
                        Mutex::Locker lock(s.lock, WaitForever);
                        if (s.sessions >= settings.maxSessions) return Error::Full;
                        const Error err = enter(s, socket, true, nowUs() + int64_t(settings.handshakeTimeoutMs) * 1000);
                        if (err.isError()) return err;
                        ++s.sessions;
                }
                s.wake.release();
                return Error::Ok;
        }

        void flushLater(Socket &socket) {
                State &s = *state;
                {
                        Mutex::Locker lock(s.lock, WaitForever);
                        if (find(s, &socket) != SIZE_MAX) return;
                        // Full or no thread: the next write or close sends it instead.
                        if (enter(s, socket, false, Never).isError()) return;
                }
                s.wake.release();
                return;
        }

        void remove(Socket &socket) {
                State &s = *state;
                (void)s.lock.lock(WaitForever);
                --s.sessions;
                for (;;) {
                        const size_t at = find(s, &socket);
                        if (at != SIZE_MAX) erase(s, at);
                        if (s.stepping != &socket) break;
                        // A step of it is running: wait for it to end (its socket is still whole).
                        s.removeWaiting = true;
                        s.lock.unlock();
                        (void)s.stepDone.acquire(WaitForever);
                        (void)s.lock.lock(WaitForever);
                }
                s.lock.unlock();
                return;
        }

        void count(uint64_t in, uint64_t out) {
                if (!state.isCreated()) return;
                if (in != 0) state->bytesIn.fetch_add(in, std::memory_order_relaxed);
                if (out != 0) state->bytesOut.fetch_add(out, std::memory_order_relaxed);
                return;
        }

        Counts counts() {
                Counts c;
                if (!state.isCreated()) return c;
                State &s = *state;
                {
                        Mutex::Locker lock(s.lock, WaitForever);
                        c.sessions = uint32_t(s.sessions);
                        c.handshaking = uint32_t(s.count);
                }
                c.handshakes = s.handshakes.load(std::memory_order_relaxed);
                c.failures = s.failures.load(std::memory_order_relaxed);
                c.bytesIn = s.bytesIn.load(std::memory_order_relaxed);
                c.bytesOut = s.bytesOut.load(std::memory_order_relaxed);
                return c;
        }

} // namespace TlsService

BMDS_NAMESPACE_END
