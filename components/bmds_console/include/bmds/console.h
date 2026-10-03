/**
 * @file      console.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
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
 * Consoles are functional objects with identity: not copyable, owned by
 * whoever created them, and never destroyed through this interface.
 *
 * @par Thread Safety
 * Implementations are thread-safe for writing: one `write()` call is
 * atomic with respect to other writers. Reading supports a single reader
 * task.
 */
class Console {
        public:
                /** @brief Capability flags (`capabilities()`). */
                enum Capability : uint32_t {
                        Ansi = 1u << 0,  ///< Understands ANSI/VT100 cursor and erase sequences.
                        Color = 1u << 1, ///< Understands SGR color sequences.
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

                /** @brief Returns the terminal size (a default when unknown). */
                virtual Size size() const = 0;

                /**
                 * @brief Reads input, waiting until at least one byte is available.
                 * @param buf       Destination.
                 * @param timeoutMs How long to wait, or `WaitForever`.
                 * @return The byte count (at least 1), or `Error::Timeout`.
                 * @blocking
                 */
                virtual Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) = 0;

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
                virtual Result<size_t> write(ByteSpan data, unsigned int timeoutMs) = 0;

                /**
                 * @brief Waits until buffered output has been handed to the terminal.
                 * @return `Error::Ok`, or `Error::Timeout`.
                 * @blocking
                 */
                virtual Error flush(unsigned int timeoutMs) = 0;

                /** @brief Writes text. See `write(ByteSpan, unsigned int)`. @blocking */
                Result<size_t> write(StringView text, unsigned int timeoutMs) {
                        return write(ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()), timeoutMs);
                }

        protected:
                constexpr Console() = default;
                ~Console() = default;
};

BMDS_NAMESPACE_END
