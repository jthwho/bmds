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
 * a running command even when nothing is reading input. A `read()` blocked
 * at the time returns `Error::Interrupted`.
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
                        Ansi = 1u << 0, ///< Understands ANSI/VT100 cursor and erase sequences.
                };

                /** @brief Terminal modes ("termios-lite"). */
                struct TermMode {
                                bool signals = true; ///< Ctrl-C raises `interrupt()` instead of being read (ISIG).
                                bool nlToCrNl =
                                        true; ///< Output `\n` is sent as CR LF where the terminal needs it (ONLCR).
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
                 * @brief Returns the terminal size (80x24 until set: a serial console
                 *        can't see its terminal; `term size` asks it).
                 */
                Size size() const {
                        const uint32_t v = _size.load(std::memory_order_relaxed);
                        return Size{static_cast<uint16_t>(v >> 16), static_cast<uint16_t>(v & 0xFFFF)};
                }

                /** @brief Records the terminal size. */
                void setSize(Size size) {
                        _size.store((static_cast<uint32_t>(size.columns) << 16) | size.rows, std::memory_order_relaxed);
                        return;
                }

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
                 * @param buf       Destination.
                 * @param timeoutMs How long to wait, or `WaitForever`.
                 * @return The byte count (at least 1), `Error::Timeout`, or
                 *         `Error::Interrupted` (Ctrl-C, with `TermMode::signals`).
                 * @blocking
                 */
                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override = 0;

                /**
                 * @brief Reads whatever input is available now.
                 * @return The byte count (at least 1), or `Error::WouldBlock`.
                 * @nonblocking
                 */
                virtual Result<size_t> tryRead(WritableByteSpan buf) = 0;

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

                /** @brief Consoles are terminals. */
                bool isTerminal() const override { return true; }

                /** @brief Returns the interrupt Ctrl-C raises (the foreground job's). */
                Interrupt &interrupt() { return _interrupt; }

                /** @brief Returns the terminal modes. */
                TermMode mode() const {
                        return TermMode{_signals.load(std::memory_order_relaxed),
                                        _nlToCrNl.load(std::memory_order_relaxed)};
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

        private:
                Interrupt             _interrupt;
                std::atomic<bool>     _signals{true};
                std::atomic<bool>     _nlToCrNl{true};
                std::atomic<uint8_t>  _depth;
                std::atomic<uint32_t> _size{(80u << 16) | 24u};
};

BMDS_NAMESPACE_END
