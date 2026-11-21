/**
 * @file      gpiolines.h
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
#include <bmds/filehandle.h>
#include <bmds/ioline.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stream.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A program's line request on `/dev/gpiochip0` (devplan/io.md §12.1): Linux's GPIO v2
 *        line fd. Its pads are claimed for the consumer and set up as the config says; their
 *        values are read and set by mask; their edges are read from it as Linux's
 *        `gpio_v2_line_event` records.
 *
 * Made by `request()` in PSRAM with its lines and its event ring; when its last handle closes
 * (the program closed it, or ended) the interrupts stop, the pads are put back as they were and
 * the claim ends.
 *
 * @par Thread Safety
 * Any task. Its pads' interrupts write the ring.
 */
class GpioLines final : public Stream {
        public:
                /** @brief Events kept per line unless asked (Linux's default), and at most in all. */
                static constexpr uint32_t EventsPerLine = 16;
                static constexpr uint32_t MaxEvents = 1024;

                /** @brief Room for a consumer's name, NUL included (Linux's `GPIO_MAX_NAME_SIZE`). */
                static constexpr size_t ConsumerBytes = 32;

                /** @brief An edge as a program reads it: Linux's `gpio_v2_line_event`. */
                struct Event {
                                uint64_t timestampNs = 0;
                                uint32_t id = 0; ///< `Rising` or `Falling`.
                                uint32_t offset = 0;
                                uint32_t seqno = 0;     ///< The request's, from 1.
                                uint32_t lineSeqno = 0; ///< The line's, from 1.
                                uint32_t padding[6] = {};
                };
                static_assert(sizeof(Event) == 48, "Linux's gpio_v2_line_event");

                static constexpr uint32_t Rising = 1;  ///< `GPIO_V2_LINE_EVENT_RISING_EDGE`.
                static constexpr uint32_t Falling = 2; ///< `GPIO_V2_LINE_EVENT_FALLING_EDGE`.

                /**
                 * @brief Requests @p offsets for @p consumer, set up as @p config says.
                 * @param eventBufferSize Events kept (0: 16 per line), at most `MaxEvents`.
                 * @return Its handle (read and write); `Invalid` (an offset off the chip or twice,
                 *         the config, a size), `NotSupported` (the HTE clock), `Busy` (a line
                 *         somebody holds), `NoMem`, `NotReady`.
                 * @blocking
                 */
                static Result<FileHandle::Ptr> request(Span<const uint32_t> offsets, StringView consumer,
                                                       const IoLineConfig &config, uint32_t eventBufferSize);

                /**
                 * @brief Sets every line up again from @p config (Linux's `SET_CONFIG`).
                 * @return `Ok`, or `IoLine::resolve()`'s error (nothing changed then).
                 * @blocking
                 */
                Error setConfig(const IoLineConfig &config);

                /** @brief The lines' logical values for @p mask's bits (a bit per line, in order). @blocking */
                Error values(uint64_t mask, uint64_t &bits);

                /**
                 * @brief Sets the logical values of @p mask's lines to @p bits'.
                 * @return `Ok`; `Invalid` (a bit past the lines, or none), `PermissionDenied` (a
                 *         line that isn't an output).
                 * @blocking
                 */
                Error setValues(uint64_t mask, uint64_t bits);

                /** @brief How many lines it has. */
                size_t count() const { return _count; }

                /** @brief Its pads as they were before it (a restart's hook; nothing else changes). @nonblocking */
                void putBack();

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan, unsigned int) override { return makeError<size_t>(Error::NotSupported); }
                uint8_t        readiness() override;
                WaitList      *readinessWaiters() override;
                GpioLines     *gpioLines() override { return this; }
                void           handleClosed(uint8_t mode) override;

                struct Line; // private: the pads (gpiolines.cpp)
                struct State;

        private:
                GpioLines() = default;
                ~GpioLines() = default;

                State *_state = nullptr;
                Line  *_lines = nullptr;
                size_t _count = 0;
};

BMDS_NAMESPACE_END
