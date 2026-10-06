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

BMDS_NAMESPACE_BEGIN

/** @brief A signal set's bit for @p sig (1-31): `sig - 1`, Linux's layout. */
constexpr uint32_t signalBit(int sig) {
        return 1u << (sig - 1);
}

/**
 * @brief A process's signals (devplan/programs.md §15): what each does, the
 *        mask, the pending set, and the interrupt its blocking calls wake on.
 *
 * Signals 1-31, Linux's numbers; a set has bit `sig - 1`. A signal is
 * **due** when it is pending, not blocked, and either caught (the program
 * takes it with `take()` and runs its handler) or one that ends the process
 * (`terminateSignal()`). Pending signals that are ignored, or dropped by
 * default (`SIGCHLD`...), are never due, and `take()` discards them.
 *
 * Senders only add pending bits; every decision looks at the actions and
 * the mask of the moment, so a signal racing a change of its action is
 * handled as if it came just before or just after the change.
 *
 * **Stopping** (devplan/processes.md §3.7.1): `SIGSTOP`, `SIGTSTP`,
 * `SIGTTIN` and `SIGTTOU` stop the process by default (`post()` says
 * `Stop`; a blocked one stays pending, `stopSignal()`); `SIGCONT` continues
 * it whatever its action (`Continue`). While stopped the interrupt is held
 * (its waits park) and other signals stay pending; `SIGKILL` continues it
 * to end it.
 *
 * The interrupt is raised when a sent signal is due. `prepare()`, at the
 * start of each system call, clears it while nothing is due.
 *
 * @par Thread Safety
 * `post()`, `isDue()`, `terminateSignal()`, `pending()` and `interrupt()`
 * from any task; the rest from the process's own task (or before it runs).
 * No locks, no allocation.
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
                        Pending,   ///< Pending: blocked, or caught (then due: the interrupt is raised).
                        Taken,     ///< An `Interrupt` action: the interrupt is raised, nothing pending.
                        Terminate, ///< The process must end (`terminateSignal()`); the interrupt is raised.
                        Stop,      ///< It stopped (`isStopped()`): the interrupt is held.
                        Continue,  ///< It was stopped and goes on: the hold is released.
                };

                /** @brief A taken signal. */
                struct Taken {
                                int  signal = 0;        ///< The signal; 0 if none was due.
                                int  code = 0;          ///< `BMDS_SI_USER` or `BMDS_SI_KERNEL`.
                                bool terminate = false; ///< Not caught: the process must end with it.
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

                /**
                 * @brief Sends @p sig (1-31) with code @p code.
                 * @return What happened; `Terminate` also for `SIGKILL`.
                 */
                Posted post(int sig, int code);

                /** @brief Returns @p sig's action. */
                Action action(int sig) const;

                /**
                 * @brief Sets @p sig's action (not `SIGKILL` or `SIGSTOP`: false).
                 *        Ignoring discards a pending one.
                 */
                bool setAction(int sig, Action action);

                /** @brief Returns the mask. */
                uint32_t blocked() const { return _blocked.load(); }

                /** @brief Sets the mask (`SIGKILL` and `SIGSTOP` are left out). */
                void setBlocked(uint32_t set);

                /** @brief Returns the pending set. */
                uint32_t pending() const { return _pending.load(); }

                /** @brief Returns true if a signal is due. */
                bool isDue() const { return dueCaught() != 0 || terminateSignal() != 0; }

                /** @brief Returns the signal that ends the process now (due, not caught), or 0. */
                int terminateSignal() const;

                /** @brief Returns true while stopped. */
                bool isStopped() const { return _stopped.load(); }

                /**
                 * @brief Stops the process with a pending stop signal (`stopSignal()`),
                 *        taking it off the pending set.
                 * @return True if it stopped (it wasn't already).
                 */
                bool stopWith(int sig) {
                        _pending.fetch_and(~bit(sig));
                        return stop();
                }

                /**
                 * @brief Returns a stop signal pending and no longer blocked (its
                 *        default action), or 0: the process stops itself with it.
                 */
                int stopSignal() const;

                /**
                 * @brief Takes the lowest due signal: a caught one, off the pending
                 *        set; or the one that ends the process (left pending).
                 *        Nothing while stopped.
                 */
                Taken take();

                /** @brief At the start of a system call: clears the interrupt while nothing is due. */
                void prepare();

                /** @brief Returns the interrupt raised when a signal is due. */
                Interrupt &interrupt() { return _interrupt; }

                /** @brief For a vfork child: @p parent's actions and mask, nothing pending. */
                void inherit(const Signals &parent);

                /** @brief For an exec: caught signals go back to their default. */
                void resetForExec();

        private:
                // SIGCHLD, SIGCONT, SIGURG, SIGWINCH.
                static constexpr uint32_t DroppedByDefault =
                        signalBit(17) | signalBit(18) | signalBit(23) | signalBit(28);
                // SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU.
                static constexpr uint32_t StopSignals = signalBit(19) | signalBit(20) | signalBit(21) | signalBit(22);
                static constexpr uint32_t Unblockable = signalBit(9) | signalBit(19); // SIGKILL, SIGSTOP

                uint32_t dueCaught() const {
                        return _stopped.load() ? 0 : _pending.load() & ~_blocked.load() & _caught.load();
                }
                uint32_t ending() const; // pending, not blocked, ending the process
                Posted   deliver(int sig, int code); // post() past stopping and continuing
                void     markPending(uint32_t b, int code);
                bool     stop();   // true if it wasn't stopped
                bool     resume(); // ends a stop; true if stopped

                Interrupt             _interrupt;
                std::atomic<uint32_t> _pending{0};
                std::atomic<uint32_t> _kernelSent{0}; // pending bits sent by the kernel (their code)
                std::atomic<uint32_t> _blocked{0};
                std::atomic<uint32_t> _caught{0};
                std::atomic<uint32_t> _ignored{0};
                std::atomic<uint32_t> _interrupting{0}; // Action::Interrupt
                std::atomic<bool>     _stopped{false};
};

BMDS_NAMESPACE_END
