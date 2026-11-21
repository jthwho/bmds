/**
 * @file      rmtchannel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

namespace RmtPorts {
        struct Port;
}

/**
 * @brief An RMT port's stream, `/dev/rmtN` (devplan/io.md §13.1): a sender's `write()` takes
 *        the hardware's 32-bit symbols into a ring the RMT interrupt drains with no gap; a
 *        receiver's `read()` gives received symbols, each run behind a header (a word 0 and the
 *        64-bit monotonic time of its first edge).
 *
 * Made when its port first starts, kept (its file points at it); a stopped port's operations
 * are `NotExist`.
 *
 * @par Thread Safety
 * Any task; one writer or reader at a time (the stream's lock). The interrupt fills or drains
 * the ring.
 */
class RmtChannel final : public Stream {
        public:
                /** @brief A word of a received stream that starts a header (two words of time follow). */
                static constexpr uint32_t HeaderWord = 0;

                /** @brief What `BMDS_RMT_INFO` reports. */
                struct Info {
                                uint32_t port = 0;        ///< N of `/dev/rmtN`.
                                int32_t  channel = -1;    ///< The hardware channel, or -1 (stopped).
                                bool     receive = false; ///< A receiver (else a sender).
                                bool     running = false; ///< Its port runs.
                                bool     sending = false; ///< A sender's transaction is on.
                                bool     continuous = false;
                                bool     idleHigh = false; ///< A sender rests high.
                                uint32_t resolution = 0;  ///< Ticks a second.
                                uint32_t sourceHz = 0;    ///< The RMT's source clock.
                                uint32_t buffer = 0;      ///< The ring, in symbols.
                                int64_t  startNs = 0;     ///< A sender's stream start (monotonic), or 0.
                                uint64_t symbols = 0;     ///< Sent (handed to the hardware), or received.
                                uint64_t ticks = 0;       ///< A sender's ticks handed over since the start, filler too.
                                uint64_t fillerTicks = 0; ///< A continuous sender's idle filler, in ticks.
                                uint32_t underruns = 0;   ///< Times a continuous sender's ring ran dry.
                                uint32_t overruns = 0;    ///< A receiver's blocks dropped (the ring was full).
                };

                /** @brief Its info; `NotExist` before its port first started. @blocking */
                Error info(Info &out) const;

                /** @brief Port @p port's stream, if the port ever started, or null. */
                static RmtChannel *find(unsigned port);

                /** @brief True while its port runs (its file is in `/dev`). */
                bool isRunning() const;

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                uint8_t        readiness() override;
                WaitList      *readinessWaiters() override { return &_pollers; }
                RmtChannel    *rmtChannel() override { return this; }

        private:
                friend struct RmtPorts::Port;

                explicit RmtChannel(RmtPorts::Port &port) : _port(port) {}

                RmtPorts::Port &_port;
                Mutex           _lock;        // one writer or reader at a time
                Semaphore       _moved{1, 0}; // released by the interrupt: room (a sender), words (a receiver)
                WaitListOf<4>   _pollers;
};

BMDS_NAMESPACE_END
