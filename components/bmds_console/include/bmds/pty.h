/**
 * @file      pty.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/bytering.h>
#include <bmds/config.h>
#include <bmds/console.h>
#include <bmds/filehandle.h>
#include <bmds/memspace.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

class Pty;

/**
 * @brief A pseudo-terminal's program end: the console a shell or program
 *        runs on (`devplan/pty.md`).
 *
 * A `Console` like any other: its modes, Ctrl-C and Ctrl-\ sending signals,
 * its size and `resized()`, its readiness. What is written goes to the
 * emulator's end (`\n` as CR LF while the mode says so); what is read is
 * what the emulator wrote. Once the emulator has hung up, reads end
 * (`EndOfFile`) and writes are dropped.
 *
 * @par Thread Safety
 * Thread-safe, as consoles are.
 */
class PtyConsole final : public Console {
        public:
                const char    *name() const override { return _name; }
                uint32_t       capabilities() const override { return Ansi; }
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                Error          flush(unsigned int timeoutMs) override;
                void           handleOpened(uint8_t mode) override;
                void           handleClosed(uint8_t mode) override;
                void           addUser() override;
                void           removeUser() override;
                int            ptyNumber() const override;

                using Console::write;

        protected:
                Result<size_t> readInput(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> tryReadInput(WritableByteSpan buf) override;
                bool           hasInput() const override;

        private:
                friend class Pty;

                explicit PtyConsole(Pty &pty) : _pty(pty) {}
                size_t push(ByteSpan data); // typed input (the emulator's writes), Ctrl-C and Ctrl-\ handled

                Pty                  &_pty;
                char                  _name[8] = {}; // "ptyN"
                Mutex                 _inputLock;    // the input's producers
                std::atomic<bool>     _flush{false}; // input before a Ctrl-C or Ctrl-\ to drop
                std::atomic<uint32_t> _flushTo{0};
};

/**
 * @brief A pseudo-terminal: a numbered pair (`ptyN`, no singletons) whose
 *        program end is a `PtyConsole` and whose emulator end is this
 *        stream (`devplan/pty.md`).
 *
 * The emulator (a terminal window) reads what the program writes and writes
 * what is typed; it sets the size (the program sees `resized()`, so
 * `SIGWINCH`). Output is never dropped: a writer waits while the output is
 * full, for the emulator to read. Either end can finish: `hangUp()` (the
 * emulator's: the program's reads end), `programEnded()` (the program's:
 * the emulator's reads end once its output is read). `release()` returns
 * the pair once both are done.
 *
 * A counted pair (`openCounted()`, `/dev/ptmx`'s) finishes on its own
 * instead: its ends count their users (handles opened on either end, and
 * processes that have the program's end as their terminal); the emulator
 * end's last user going hangs up, the program end's last ends the program's
 * side, and both gone release it. Its emulator end's writes then take what
 * fits and wait only while nothing does, and it is writable only with room,
 * as a descriptor's users expect.
 *
 * @par Thread Safety
 * Thread-safe: one emulator reads at a time; writers and readers of either
 * end may be on any task.
 */
class Pty final : public Stream {
        public:
                /** @brief Pairs at once. */
                static constexpr size_t MaxPtys = 8;

                /** @brief Output buffered for the emulator. */
                static constexpr size_t OutputBytes = 4096;

                /** @brief Input buffered for the program. */
                static constexpr size_t InputBytes = 512;

                /**
                 * @brief Opens the next free pair, its buffers in @p space.
                 * @return It, `Full`, or `NoMem`.
                 */
                static Result<Pty *> open(MemSpace space);

                /**
                 * @brief Opens the next free pair as a counted one (see the class),
                 *        its buffers in @p space. The caller opens a handle on it
                 *        at once; a pair whose handle couldn't be opened is
                 *        `release()`d.
                 * @return It, `Full`, or `NoMem`.
                 */
                static Result<Pty *> openCounted(MemSpace space);

                /**
                 * @brief Opens a handle with @p mode (`FileHandle::Mode`) on counted
                 *        pair @p number's program end (`/dev/pts/N`).
                 * @return The handle, `NotExist` (no such counted pair open, or
                 *         finishing), or `NoMem`.
                 */
                static Result<FileHandle::Ptr> openProgramEnd(uint8_t number, uint8_t mode);

                /** @brief Returns true while counted pair @p number is open (listings). */
                static bool isCountedOpen(uint8_t number);

                /** @brief Returns pair @p number's program end, or null (never made): its identity. */
                static const PtyConsole *programEnd(uint8_t number);

                explicit Pty(uint8_t number);
                Pty(const Pty &) = delete;
                Pty &operator=(const Pty &) = delete;

                /** @brief Returns its number. */
                uint8_t number() const { return _number; }

                /** @brief Returns the program's end. */
                PtyConsole &console() { return _console; }

                /**
                 * @brief Reads the program's output, waiting while there is none.
                 * @return The bytes (at least 1), `Timeout`, `Interrupted` (the
                 *         caller's interrupt), or `EndOfFile` (the program's end
                 *         is done and everything was read).
                 * @blocking
                 */
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;

                /**
                 * @brief Gives the program input (keys), as much as fits; a Ctrl-C
                 *        sends `SIGINT` (Ctrl-\ `SIGQUIT`) while its signals are on.
                 * @return The bytes taken, or `BrokenPipe` (the program's end is done).
                 * @nonblocking
                 */
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;

                using Stream::write;

                /**
                 * @brief Gives the program all of @p data as input (a paste), waiting
                 *        up to @p timeoutMs for it to read what doesn't fit.
                 * @return The bytes taken: all, or fewer with `Timeout`, `BrokenPipe`
                 *         (the program's end is done) or `EndOfFile` (hung up).
                 * @blocking
                 */
                Result<size_t> input(ByteSpan data, unsigned int timeoutMs);

                /** @brief Readable while output waits (or the program's end is done). */
                uint8_t readiness() override;

                /** @brief Released when output arrives or the program's end finishes. */
                WaitList *readinessWaiters() override { return &_outputWaiters; }

                /** @brief Returns this pair (a descriptor's `term_ctl`). */
                Pty *pty() override { return this; }

                /** @brief A counted pair counts its emulator end's handles. */
                void handleOpened(uint8_t mode) override;

                /** @brief See `handleOpened()`: the last one hangs up. */
                void handleClosed(uint8_t mode) override;

                /** @brief Sets the terminal size the program sees. */
                void setSize(Console::Size size) {
                        _console.setSize(size);
                        return;
                }

                /**
                 * @brief The emulator's end closes: the session gets `SIGHUP` (once), the
                 *        program's reads end, its writes are dropped.
                 */
                void hangUp();

                /** @brief The program's end is done: the emulator's reads end once its output is read. */
                void programEnded();

                /** @brief Returns the pair (both ends done); its buffers are freed. */
                void release();

        private:
                friend class PtyConsole;

                Error reset(MemSpace space);
                void  emulatorUsers(int delta); // a counted pair's emulator end gained or lost a user
                void  programUsers(int delta);  // a counted pair's program end gained or lost a user

                PtyConsole        _console{*this};
                Mutex             _writeLock;         // the program's writers, one at a time
                void             *_block = nullptr;   // the rings and their storage
                ByteRing         *_output = nullptr;  // program -> emulator
                ByteRing         *_input = nullptr;   // emulator -> program
                Semaphore         _outputReady{1, 0}; // output arrived (or the program's end finished)
                Semaphore         _outputSpace{1, 0}; // output was read (or the emulator hung up)
                Semaphore         _inputReady{1, 0};  // input arrived (or the emulator hung up)
                Semaphore         _inputSpace{1, 0};  // input was read (or either end finished)
                WaitListOf<4>     _outputWaiters;     // the emulator's pollers
                std::atomic<bool> _hungUp{false};
                std::atomic<bool> _ended{false};
                std::atomic<bool> _inUse{false};
                mutable Mutex     _countLock;         // a counted pair's users and its release decision
                std::atomic<bool> _counted{false};    // openCounted(): it finishes when its users are gone
                bool              _done = false;      // counted: its release is decided (_countLock)
                uint16_t          _emulatorUsers = 0; // counted: handles on this end (_countLock)
                uint16_t          _programUsers = 0;  // counted: handles and processes on the console (_countLock)
                uint8_t           _number;
};

BMDS_NAMESPACE_END
