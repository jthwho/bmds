/**
 * @file      logstream.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/mutex.h>
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The kernel log as a stream: what a program a kernel service runs
 *        writes to its stdout and stderr (the SSH server's, devplan/network.md
 *        §24) becomes log lines, each prefixed with the service's name.
 *
 * Lines are cut at `LineBytes`; a CR is dropped. Reads end at once.
 *
 * @par Thread Safety
 * Thread-safe: writers take turns a write at a time.
 */
class LogStream final : public Stream {
        public:
                /** @brief The longest line logged; a longer one is logged in pieces. */
                static constexpr size_t LineBytes = 160;

                /** @brief Logs as @p prefix (static text: kept). */
                explicit LogStream(const char *prefix) : _prefix(prefix) {}

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;

                using Stream::write;

        private:
                void emit(); // _lock held

                const char *_prefix;
                Mutex       _lock;
                char        _line[LineBytes];
                size_t      _used = 0;
};

/**
 * @brief `/dev/log` (devplan/logging.md §3.3): each write is one syslog
 *        message (RFC 3164, as musl's `syslog()` sends it): its `<PRI>` gives
 *        the level, its timestamp is dropped, its `ident[pid]:` is the lines'
 *        tag. Reads end at once.
 *
 * @par Thread Safety
 * Thread-safe; stateless (the log's lock keeps each line whole).
 */
class SyslogStream final : public Stream {
        public:
                /** @brief The one every open of `/dev/log` shares. */
                static SyslogStream &instance();

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;

                using Stream::write;
};

BMDS_NAMESPACE_END
