/**
 * @file      console.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/color.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/interrupt.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stream.h>
#include <bmds/stringview.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A terminal endpoint: a byte stream in each direction.
 *
 * Shells, the line editor and (later) programs run against this interface,
 * so the same code works on the serial console, the HDMI text console, or
 * anything else that can carry a terminal. Output is text with ANSI/VT
 * control sequences when `capabilities()` includes `Ansi`; input is the
 * raw bytes a terminal sends (keys, escape sequences).
 *
 * Text written to a console uses `'\n'` line endings. A console whose
 * terminal needs something else (a serial terminal wants CR LF) translates
 * as it writes.
 *
 * A console is a `Stream` (it is what fds 0/1/2 refer to in an
 * interactive shell) and owns the `Interrupt` of the job in its foreground:
 * with `TermMode::signals` set, a Ctrl-C byte in the input is not queued
 * but raises that interrupt, straight from the receive path, so it stops
 * a running command even when nothing is reading input (programs get
 * `SIGINT` from the loader, which waits on it). Like every stream, a
 * `read()` waits through the calling task's interrupt
 * (`Interrupt::current()`): the shell's tasks have this one.
 *
 * For `poll`, a console is readable while input is waiting (the terminal's
 * size answer, which `read()` takes out, counts until then) and always
 * writable; input arriving notifies `readinessWaiters()`.
 *
 * Consoles are functional objects with identity: not copyable, owned by
 * whoever created them, and never destroyed through this interface.
 *
 * @par Thread Safety
 * Implementations are thread-safe for writing: one `write()` call is
 * atomic with respect to other writers. Reading supports a single reader
 * task.
 */
class Console : public Stream {
        public:
                /** @brief Capability flags (`capabilities()`). */
                enum Capability : uint32_t {
                        Ansi = 1u << 0,      ///< Understands ANSI/VT100 cursor and erase sequences.
                        SizeQuery = 1u << 1, ///< A terminal that may answer `requestSize()` (serial).
                };

                /** @brief Terminal modes ("termios-lite"). */
                struct TermMode {
                                bool signals = true; ///< Ctrl-C raises `interrupt()` instead of being read (ISIG).
                                bool nlToCrNl =
                                        true; ///< Output `\n` is sent as CR LF where the terminal needs it (ONLCR).
                                bool colors = true; ///< Output SGR colors are fitted to `colorDepth()`.
                                bool logs = true;   ///< Log lines are shown (else they go only to the history).

                                /** @brief Everything off: bytes pass both ways untouched (a file transfer). */
                                static constexpr TermMode binary() { return TermMode{false, false, false, false}; }
                };

                /** @brief Terminal size in character cells. */
                struct Size {
                                uint16_t columns = 80; ///< Width.
                                uint16_t rows = 24;    ///< Height.
                };

                Console(const Console &) = delete;
                Console &operator=(const Console &) = delete;

                /** @brief Returns the console's name, e.g. `"usb"`. */
                virtual const char *name() const = 0;

                /** @brief Returns the `Capability` flags. */
                virtual uint32_t capabilities() const = 0;

                /**
                 * @brief Returns the terminal size: 80x24 until known. A display or
                 *        network console sets it; a serial one learns it from the
                 *        terminal's answer to `requestSize()`.
                 */
                Size size() const {
                        const uint32_t v = _size.load(std::memory_order_relaxed);
                        return Size{static_cast<uint16_t>(v >> 16), static_cast<uint16_t>(v & 0xFFFF)};
                }

                /**
                 * @brief Records the terminal size; if it changed, `sizeGeneration()`
                 *        changes and `resized()` is raised. Task context.
                 */
                void setSize(Size size) {
                        const uint32_t v = (static_cast<uint32_t>(size.columns) << 16) | size.rows;
                        if (_size.exchange(v, std::memory_order_relaxed) != v) {
                                _sizeGeneration.fetch_add(1, std::memory_order_relaxed);
                                _resized.raise();
                        }
                        return;
                }

                /**
                 * @brief Raised whenever the size changes (never cleared): watch it
                 *        (`Interrupt::Watch`) to wake on resizes (`SIGWINCH`).
                 */
                Interrupt &resized() { return _resized; }

                /**
                 * @brief Returns a number that changes whenever the size does (so a
                 *        shell can update `COLUMNS` and `LINES`, and later signal
                 *        `SIGWINCH`).
                 * @isrsafe
                 */
                uint32_t sizeGeneration() const { return _sizeGeneration.load(std::memory_order_relaxed); }

                /**
                 * @brief Asks the terminal for its size, without waiting.
                 *
                 * Writes `ESC [ 18 t`; the answer (`ESC [ 8 ; rows ; cols t`) is
                 * taken out of the input by `read()`, wherever it comes among typed
                 * keys, and sets `size()`. Nothing is sent while a request is
                 * outstanding (2 s), nor after three went unanswered (a terminal
                 * that doesn't answer), until `resetSizeRequests()`.
                 * @nonblocking
                 */
                void requestSize();

                /**
                 * @brief Lets `requestSize()` ask again at once: after unanswered
                 *        requests, or with one outstanding (`reset`, `term size`).
                 */
                void resetSizeRequests() {
                        _unanswered = 0;
                        _requestDeadlineUs.store(0, std::memory_order_relaxed);
                        return;
                }

                /** @brief Returns how many size requests in a row went unanswered. */
                uint8_t unansweredSizeRequests() const { return _unanswered; }

                /** @brief Returns true while a size request waits for its answer. */
                bool isSizeRequestPending() const { return _requestDeadlineUs.load(std::memory_order_relaxed) != 0; }

                /**
                 * @brief Returns how many colors the terminal shows.
                 *
                 * Output may use any SGR colors; below `TrueColor` the console
                 * rewrites them to fit (see `ColorFilter`).
                 */
                ColorDepth colorDepth() const {
                        return static_cast<ColorDepth>(_depth.load(std::memory_order_relaxed));
                }

                /**
                 * @brief Sets the color depth (a serial console can't detect its terminal's).
                 */
                void setColorDepth(ColorDepth depth) {
                        _depth.store(static_cast<uint8_t>(depth), std::memory_order_relaxed);
                        return;
                }

                /**
                 * @brief Reads input, waiting until at least one byte is available.
                 *
                 * The terminal's answer to `requestSize()` is taken out on the way.
                 *
                 * @param buf       Destination.
                 * @param timeoutMs How long to wait, or `WaitForever`.
                 * @return The byte count (at least 1), `Error::Timeout`, or
                 *         `Error::Interrupted` (the caller's interrupt was raised).
                 * @blocking
                 */
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) final;

                /**
                 * @brief Reads whatever input is available now (the size answer taken out).
                 * @return The byte count (at least 1), or `Error::WouldBlock`.
                 * @nonblocking
                 */
                Result<size_t> tryRead(WritableByteSpan buf);

                /**
                 * @brief Writes all of @p data, waiting for space up to @p timeoutMs.
                 *
                 * The call is atomic with respect to other writers. A console
                 * that cannot deliver output (e.g. nobody has the serial port
                 * open) may fail fast with `Error::Timeout` instead of waiting.
                 *
                 * @return The bytes of @p data accepted: all of it with `Ok`, or
                 *         fewer with `Error::Timeout`.
                 * @blocking
                 */
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override = 0;

                using Stream::write;

                /**
                 * @brief Waits until buffered output has been handed to the terminal.
                 * @return `Error::Ok`, or `Error::Timeout`.
                 * @blocking
                 */
                Error flush(unsigned int timeoutMs) override = 0;

                /** @brief Readable while input waits; always writable. */
                uint8_t readiness() override {
                        return static_cast<uint8_t>(Writable |
                                                    (hasInput() || _releasePos < _releaseCount ? Readable : 0));
                }

                /** @brief Notified when input arrives. */
                WaitList *readinessWaiters() override { return &_inputWaiters; }

                /** @brief Consoles are terminals. */
                bool isTerminal() const override { return true; }

                /** @brief Returns the interrupt Ctrl-C raises (the foreground job's). */
                Interrupt &interrupt() { return _interrupt; }

                /** @brief Returns the terminal modes. */
                TermMode mode() const {
                        return TermMode{_signals.load(std::memory_order_relaxed),
                                        _nlToCrNl.load(std::memory_order_relaxed),
                                        _colors.load(std::memory_order_relaxed), _logs.load(std::memory_order_relaxed)};
                }

                /**
                 * @brief Sets the terminal modes.
                 *
                 * A program that wants raw keys clears `signals` while it runs;
                 * the shell restores the modes when it exits.
                 */
                void setMode(const TermMode &mode) {
                        _signals.store(mode.signals, std::memory_order_relaxed);
                        _nlToCrNl.store(mode.nlToCrNl, std::memory_order_relaxed);
                        _colors.store(mode.colors, std::memory_order_relaxed);
                        _logs.store(mode.logs, std::memory_order_relaxed);
                        return;
                }

        protected:
                /** @brief Constructs a console of color depth @p depth. */
                explicit constexpr Console(ColorDepth depth = ColorDepth::TrueColor)
                    : _depth(static_cast<uint8_t>(depth)) {}
                ~Console() = default;

                /**
                 * @brief Returns true if Ctrl-C should raise the interrupt (receive path).
                 * @isrsafe
                 */
                bool signalsEnabled() const { return _signals.load(std::memory_order_relaxed); }

                /**
                 * @brief The input as the terminal sent it, waiting until at least one
                 *        byte is available (as `read()`, for implementations).
                 * @blocking
                 */
                virtual Result<size_t> readInput(WritableByteSpan buf, unsigned int timeoutMs) = 0;

                /** @brief The input available now (as `tryRead()`, for implementations). */
                virtual Result<size_t> tryReadInput(WritableByteSpan buf) = 0;

                /**
                 * @brief Returns true if input is waiting (for implementations).
                 * @nonblocking
                 */
                virtual bool hasInput() const = 0;

                /**
                 * @brief Tells pollers that input arrived (receive interrupt).
                 * @isrsafe
                 */
                void inputArrivedFromIsr() {
                        _inputWaiters.notifyFromIsr();
                        return;
                }

                /** @brief Tells pollers that input arrived (task context). */
                void inputArrived() {
                        _inputWaiters.notify();
                        return;
                }

        private:
                Interrupt             _interrupt;
                Interrupt             _resized;
                WaitListOf<4>         _inputWaiters;
                std::atomic<bool>     _signals{true};
                std::atomic<bool>     _nlToCrNl{true};
                std::atomic<bool>     _colors{true};
                std::atomic<bool>     _logs{true};
                std::atomic<uint8_t>  _depth;
                std::atomic<uint32_t> _size{(80u << 16) | 24u};
                std::atomic<uint32_t> _sizeGeneration{0};

                // The size answer filter (the reader's; requestSize() starts it).
                Result<size_t> filter(WritableByteSpan buf, size_t n);
                size_t         drainHeld(WritableByteSpan buf, bool force = false);
                bool           expireRequest();

                std::atomic<int64_t> _requestDeadlineUs{0}; // 0: no request outstanding
                uint8_t              _unanswered = 0;
                uint8_t              _held[16] = {};  // the start of an answer, across reads
                uint8_t              _heldCount = 0;
                uint8_t              _release[16] = {}; // held bytes that weren't an answer
                uint8_t              _releaseCount = 0;
                uint8_t              _releasePos = 0;
};

BMDS_NAMESPACE_END
