/**
 * @file      signals.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/interrupt.h>
#include <bmds/mutex.h>

BMDS_NAMESPACE_BEGIN

/** @brief A signal set's bit for @p sig (1-31): `sig - 1`, Linux's layout. */
constexpr uint32_t signalBit(int sig) {
        return 1u << (sig - 1);
}

/**
 * @brief A process's signals (devplan/programs.md §15): what each does, the
 *        pending set, stopping, and its threads' masks, pending sets and
 *        the interrupts their blocking calls wake on.
 *
 * Signals 1-31, Linux's numbers; a set has bit `sig - 1`. A signal is
 * **due** for a thread when it is pending (for the process, or for that
 * thread), not blocked by the thread, and either caught (the thread takes it
 * with `take()` and runs its handler) or one that ends the process
 * (`terminateSignal()`). Pending signals that are ignored, or dropped by
 * default (`SIGCHLD`...), are never due, and `take()` discards them.
 *
 * **Threads** (devplan/processes.md §3.10): the actions, the process's
 * pending set and stopping are the process's; each thread has a mask, a set
 * of signals sent to it alone (`postTo()`), and an `Interrupt`. The first
 * thread's (`mainThread()`) is part of `Signals`, and kernel code (a built-in
 * program, its threads) shares it; a program's other threads are attached.
 * A signal sent to the process raises the interrupt of the first thread that
 * doesn't block it (the main thread first); one that ends the process does
 * so unless every thread blocks it.
 *
 * Senders only add pending bits; every decision looks at the actions and
 * the masks of the moment, so a signal racing a change of its action is
 * handled as if it came just before or just after the change.
 *
 * **Stopping** (devplan/processes.md §3.7.1): `SIGSTOP`, `SIGTSTP`,
 * `SIGTTIN` and `SIGTTOU` stop the process by default (`post()` says
 * `Stop`; a blocked one stays pending, `stopSignal()`); `SIGCONT` continues
 * it whatever its action (`Continue`). While stopped every thread's
 * interrupt is held (its waits park) and other signals stay pending;
 * `SIGKILL` continues it to end it.
 *
 * A thread's interrupt is raised when a sent signal is due for it. `prepare()`,
 * at the start of each system call, clears it while nothing is due.
 *
 * @par Thread Safety
 * Thread-safe. The thread list is under a lock of its own (`post()`,
 * `postTo()`, `setBlocked()`, `attach()`, `detach()`, stopping and
 * continuing take it); the rest is atomics. A `Thread`'s own calls
 * (`take()`, `prepare()`) from that thread. No allocation.
 */
class Signals {
        public:
                /** @brief One more than the highest signal (1-31). */
                static constexpr int Count = 32;

                /** @brief What a signal does. */
                enum class Action : uint8_t {
                        Default, ///< Dropped, or ends the process (`endsByDefault()`).
                        Ignore,  ///< Dropped (a pending one too).
                        Catch,   ///< Pending until `take()`n.
                        /**
                         * Raises the interrupt, nothing pending: a built-in program's
                         * catch (kernel code has no handlers; the interactive shell's `SIGINT`).
                         */
                        Interrupt,
                };

                /** @brief What `post()` did. */
                enum class Posted : uint8_t {
                        Dropped,   ///< Ignored, or dropped by default.
                        Pending,   ///< Pending: blocked, or caught (then due: an interrupt is raised).
                        Taken,     ///< An `Interrupt` action: the interrupt is raised, nothing pending.
                        Terminate, ///< The process must end (`terminateSignal()`); every interrupt is raised.
                        Stop,      ///< It stopped (`isStopped()`): every interrupt is held.
                        Continue,  ///< It was stopped and goes on: the holds are released.
                };

                /** @brief A taken signal. */
                struct Taken {
                                int  signal = 0;        ///< The signal; 0 if none was due.
                                int  code = 0;          ///< `BMDS_SI_USER` or `BMDS_SI_KERNEL`.
                                bool terminate = false; ///< Not caught: the process must end with it.
                };

                /**
                 * @brief One thread's part: its mask, the signals sent to it alone, its
                 *        interrupt. Attached from before the thread runs until it has ended.
                 */
                struct Thread {
                                Thread() = default;
                                Thread(const Thread &) = delete;
                                Thread &operator=(const Thread &) = delete;

                                Interrupt             interrupt;
                                std::atomic<uint32_t> blocked{0};
                                std::atomic<uint32_t> pending{0};    // sent to it (postTo())
                                std::atomic<uint32_t> kernelSent{0}; // which of those the kernel sent
                                std::atomic<bool>     wake{false};   // wake(): its interrupt stays raised
                                Thread               *next = nullptr;
                };

                Signals() = default;
                Signals(const Signals &) = delete;
                Signals &operator=(const Signals &) = delete;

                /** @brief Returns the set's bit for @p sig (1-31). */
                static constexpr uint32_t bit(int sig) { return signalBit(sig); }

                /** @brief Returns true if @p sig is 1-31. */
                static constexpr bool isValid(int sig) { return sig >= 1 && sig < Count; }

                /** @brief Returns true if @p sig ends the process by default (else it is dropped or stops it). */
                static bool endsByDefault(int sig) { return (bit(sig) & (DroppedByDefault | StopSignals)) == 0; }

                /** @brief Returns true if @p sig stops the process by default. */
                static bool stopsByDefault(int sig) { return (bit(sig) & StopSignals) != 0; }

                /** @brief Returns the first thread's part (kernel code's too). */
                Thread &mainThread() { return _main; }

                /**
                 * @brief Adds @p thread (its mask set, nothing pending), held if the
                 *        process is stopped and raised if it is ending. The main
                 *        thread's part, detached when the first thread ended, comes
                 *        back (a thread that execs goes on as the first).
                 */
                void attach(Thread &thread);

                /** @brief Removes @p thread; signals sent to it alone are lost. */
                void detach(Thread &thread);

                /**
                 * @brief Returns the attached thread whose interrupt is the calling
                 *        task's (`Interrupt::current()`), else the main thread's.
                 */
                Thread &current();

                /**
                 * @brief Sends @p sig (1-31) to the process with code @p code.
                 * @return What happened; `Terminate` also for `SIGKILL`.
                 */
                Posted post(int sig, int code) { return deliverTo(nullptr, sig, code); }

                /**
                 * @brief Sends @p sig (1-31) to @p thread alone (`pthread_kill`): caught,
                 *        it is due for that thread only; otherwise it acts on the
                 *        process (once the thread doesn't block it).
                 */
                Posted postTo(Thread &thread, int sig, int code) { return deliverTo(&thread, sig, code); }

                /**
                 * @brief Ends the call @p thread is blocked in, or its next one, once:
                 *        its interrupt is raised and stays so until `clearWake()`.
                 */
                void wake(Thread &thread);

                /** @brief Ends a `wake()`: the call it was for has returned. */
                static void clearWake(Thread &thread) {
                        thread.wake.store(false);
                        return;
                }

                /** @brief Returns @p sig's action. */
                Action action(int sig) const;

                /**
                 * @brief Sets @p sig's action (not `SIGKILL` or `SIGSTOP`: false).
                 *        Ignoring discards a pending one.
                 */
                bool setAction(int sig, Action action);

                /** @brief Returns the main thread's mask. */
                uint32_t blocked() const { return _main.blocked.load(); }

                /** @brief Returns @p thread's mask. */
                static uint32_t blocked(const Thread &thread) { return thread.blocked.load(); }

                /** @brief Sets the main thread's mask (`SIGKILL` and `SIGSTOP` are left out). */
                void setBlocked(uint32_t set) { setBlocked(_main, set); }

                /** @brief Sets @p thread's mask (`SIGKILL` and `SIGSTOP` are left out). */
                void setBlocked(Thread &thread, uint32_t set);

                /** @brief Returns the process's pending set. */
                uint32_t pending() const { return _pending.load(); }

                /** @brief Returns what is pending for @p thread: the process's and its own. */
                uint32_t pending(const Thread &thread) const {
                        // Acquire: what the sender wrote before is seen. A waiter's ordering against
                        // its interrupt is prepare()'s fence.
                        return _pending.load(std::memory_order_acquire) |
                               thread.pending.load(std::memory_order_acquire);
                }

                /** @brief Returns true if a signal is due for the main thread. */
                bool isDue() const { return isDue(_main); }

                /** @brief Returns true if a signal is due for @p thread. */
                bool isDue(const Thread &thread) const { return dueCaught(thread) != 0 || terminateSignal() != 0; }

                /** @brief `isDue()`, with `terminateSignal()` already known (@p terminate). */
                bool isDue(const Thread &thread, int terminate) const {
                        return terminate != 0 || dueCaught(thread) != 0;
                }

                /** @brief Returns the signal that ends the process now (due, not caught), or 0. */
                int terminateSignal() const;

                /** @brief Returns true while stopped. */
                bool isStopped() const { return _stopped.load(); }

                /**
                 * @brief Stops the process with a pending stop signal (`stopSignal()`),
                 *        taking it off the pending sets.
                 * @return True if it stopped (it wasn't already).
                 */
                bool stopWith(Thread &thread, int sig);

                /** @brief `stopWith()` for the main thread. */
                bool stopWith(int sig) { return stopWith(_main, sig); }

                /**
                 * @brief Returns a stop signal pending and no longer blocked (its
                 *        default action) for @p thread, or 0: the process stops
                 *        itself with it.
                 */
                int stopSignal(const Thread &thread) const;

                /** @brief `stopSignal()` for the main thread. */
                int stopSignal() const { return stopSignal(_main); }

                /**
                 * @brief Takes the lowest signal due for @p thread: a caught one, off
                 *        its pending set; or the one that ends the process (left
                 *        pending). Nothing while stopped.
                 */
                Taken take(Thread &thread);

                /** @brief `take()` for the main thread. */
                Taken take() { return take(_main); }

                /**
                 * @brief At the start of a system call: clears @p thread's interrupt
                 *        while nothing is due for it (and no `wake()` is).
                 */
                void prepare(Thread &thread);

                /** @brief `prepare()` for the main thread. */
                void prepare() { prepare(_main); }

                /** @brief Returns the main thread's interrupt (kernel code's). */
                Interrupt &interrupt() { return _main.interrupt; }

                /** @brief For a vfork child: @p parent's actions and @p thread's mask, nothing pending. */
                void inherit(const Signals &parent, const Thread &thread);

                /** @brief For an exec: caught signals go back to their default. */
                void resetForExec();

        private:
                // SIGCHLD, SIGCONT, SIGURG, SIGWINCH.
                static constexpr uint32_t DroppedByDefault =
                        signalBit(17) | signalBit(18) | signalBit(23) | signalBit(28);
                // SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU.
                static constexpr uint32_t StopSignals = signalBit(19) | signalBit(20) | signalBit(21) | signalBit(22);
                static constexpr uint32_t Unblockable = signalBit(9) | signalBit(19); // SIGKILL, SIGSTOP

                // Every system call asks these on its way in and out (devplan/bench.md §15): the
                // pending sets first, and nothing more when they are empty, as they nearly always are
                // (the same values read, fewer of them: each load is a fence on each side).
                uint32_t dueCaught(const Thread &thread) const {
                        const uint32_t p = pending(thread);
                        if (p == 0 || _stopped.load(std::memory_order_acquire)) return 0;
                        return p & ~thread.blocked.load(std::memory_order_acquire) &
                               _caught.load(std::memory_order_acquire);
                }
                uint32_t ending() const;                           // pending, not blocked everywhere, ending it
                uint32_t fatal() const;                            // what ends the process if not caught
                Posted   deliverTo(Thread *to, int sig, int code); // post()/postTo()
                // Under _lock:
                Posted deliver(Thread *to, int sig, int code); // past stopping and continuing
                void   raiseFor(uint32_t b);                   // the first thread not blocking b
                void   raiseAll();
                void   clearPending(uint32_t set); // the process's and every thread's
                void   updateBlocked();            // _allBlocked from the threads' masks
                bool   stop();                     // true if it wasn't stopped
                bool   resume();                   // ends a stop; true if stopped

                static void markPending(std::atomic<uint32_t> &pending, std::atomic<uint32_t> &kernelSent, uint32_t b,
                                        int code);

                Thread                _main;
                Thread               *_others = nullptr; // the other threads, under _lock
                bool                  _mainGone = false; // the first thread ended, under _lock
                Mutex                 _lock;
                std::atomic<uint32_t> _allBlocked{0}; // blocked by every thread
                std::atomic<uint32_t> _pending{0};
                std::atomic<uint32_t> _kernelSent{0}; // pending bits sent by the kernel (their code)
                std::atomic<uint32_t> _caught{0};
                std::atomic<uint32_t> _ignored{0};
                std::atomic<uint32_t> _interrupting{0}; // Action::Interrupt
                std::atomic<bool>     _stopped{false};
};

BMDS_NAMESPACE_END
