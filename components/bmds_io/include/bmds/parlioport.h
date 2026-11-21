/**
 * @file      parlioport.h
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
#include <bmds/mutex.h>
#include <bmds/semaphore.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

namespace ParlioPorts {
        struct Port;
}

/**
 * @brief A PARLIO port's stream, `/dev/parlioN` (devplan/io.md §15.5): a sender's `write()` takes
 *        whole samples and sends them in order; a receiver's `read()` gives the samples taken
 *        since it started, the oldest dropped (counted) when a reader falls a ring behind.
 *
 * Made when its port first starts, kept (its file points at it); a stopped port's operations are
 * `NotExist`.
 *
 * @par Thread Safety
 * Any task; one writer or reader at a time (the stream's lock). The driver's interrupt moves the
 * counts.
 */
class ParlioPort final : public Stream {
        public:
                /** @brief What `BMDS_PARLIO_INFO` reports. */
                struct Info {
                                uint32_t port = 0;
                                bool     receive = false;
                                bool     running = false;
                                bool     externalClock = false;
                                bool     monitor = false;
                                uint32_t width = 0;
                                uint32_t clockHz = 0;
                                uint32_t buffer = 0;
                                int64_t  startNs = 0;
                                uint64_t bytes = 0;
                                uint64_t lost = 0;
                                uint64_t transfers = 0;
                                uint32_t idle = 0;
                };

                /** @brief Its info. @blocking */
                Error info(Info &out) const;

                /** @brief True while its port runs (its file is in `/dev`). */
                bool isRunning() const;

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                Error          flush(unsigned int timeoutMs) override;
                uint8_t        readiness() override;
                WaitList      *readinessWaiters() override { return &_pollers; }
                ParlioPort    *parlioPort() override { return this; }

        private:
                friend struct ParlioPorts::Port;

                explicit ParlioPort(ParlioPorts::Port &port) : _port(port) {}

                ParlioPorts::Port &_port;
                Mutex              _lock;        // one writer or reader at a time
                Semaphore          _moved{1, 0}; // released by the interrupt: a transfer done, samples in
                WaitListOf<4>      _pollers;
};

BMDS_NAMESPACE_END
