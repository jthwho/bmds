/**
 * @file      console.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What every console does on top of its device: the size request, and
 * taking the terminal's answer out of the input.
 */

#include <cstring>
#include <bmds/console.h>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // How long a request waits for its answer (a terminal answers in
        // milliseconds; this also bounds how long a lone ESC typed meanwhile
        // can be held back), and how many go unanswered before asking stops.
        constexpr int64_t RequestWindowUs = 2'000'000;
        constexpr uint8_t MaxUnanswered = 3;
        // Input read at a time while an answer may be coming.
        constexpr size_t SlowChunk = 32;

        enum class Match : uint8_t {
                No,
                Partial,
                Complete
        };

        // Whether s[0..n) is (the start of) ESC [ 8 ; rows ; cols t.
        Match match(const uint8_t *s, size_t n, Console::Size &size) {
                static constexpr char Prefix[] = "\x1b[8;";
                for (size_t i = 0; i < n && i < 4; ++i) {
                        if (s[i] != static_cast<uint8_t>(Prefix[i])) return Match::No;
                }
                if (n <= 4) return Match::Partial;
                unsigned value[2] = {0, 0};
                size_t   digits = 0;
                int      field = 0;
                for (size_t i = 4; i < n; ++i) {
                        const uint8_t c = s[i];
                        if (c >= '0' && c <= '9') {
                                if (++digits > 4) return Match::No;
                                value[field] = value[field] * 10 + (c - '0');
                        } else if (c == ';' && field == 0 && digits > 0) {
                                field = 1;
                                digits = 0;
                        } else if (c == 't' && field == 1 && digits > 0 && i == n - 1) {
                                if (value[0] == 0 || value[1] == 0) return Match::No;
                                size.rows = static_cast<uint16_t>(value[0]);
                                size.columns = static_cast<uint16_t>(value[1]);
                                return Match::Complete;
                        } else {
                                return Match::No;
                        }
                }
                return Match::Partial;
        }

} // namespace

void Console::requestSize() {
        if ((capabilities() & SizeQuery) == 0 || _unanswered >= MaxUnanswered) return;
        if (_requestDeadlineUs.load(std::memory_order_relaxed) != 0 && !expireRequest()) return;
        _requestDeadlineUs.store(Detail::monotonicUs() + RequestWindowUs, std::memory_order_relaxed);
        (void)write(StringView("\x1b[18t"), 100);
        return;
}

// Ends an outstanding request whose time is up; true if it did.
bool Console::expireRequest() {
        const int64_t deadline = _requestDeadlineUs.load(std::memory_order_relaxed);
        if (deadline == 0 || Detail::monotonicUs() < deadline) return false;
        _requestDeadlineUs.store(0, std::memory_order_relaxed);
        if (_unanswered < MaxUnanswered) ++_unanswered;
        return true;
}

// Copies released bytes, then held ones (once they can't be an answer, or
// with @p force), to @p buf.
size_t Console::drainHeld(WritableByteSpan buf, bool force) {
        if (_releasePos == _releaseCount && _heldCount > 0 &&
            (force || _requestDeadlineUs.load(std::memory_order_relaxed) == 0)) {
                std::memcpy(_release, _held, _heldCount);
                _releaseCount = _heldCount;
                _releasePos = 0;
                _heldCount = 0;
        }
        size_t n = 0;
        while (n < buf.size() && _releasePos < _releaseCount) buf[n++] = _release[_releasePos++];
        if (_releasePos == _releaseCount) _releasePos = _releaseCount = 0;
        return n;
}

// Takes the answer out of buf[0..n) (raw input, at most SlowChunk bytes);
// returns how many bytes are left for the reader, in order, the ones that
// don't fit queued in _release.
Result<size_t> Console::filter(WritableByteSpan buf, size_t n) {
        uint8_t in[SlowChunk];
        std::memcpy(in, buf.data(), n);
        size_t out = 0;
        auto   emit = [&](uint8_t b) {
                if (out < buf.size()) {
                        buf[out++] = b;
                } else if (_releaseCount < sizeof(_release)) {
                        _release[_releaseCount++] = b;
                }
        };
        for (size_t i = 0; i < n; ++i) {
                const uint8_t b = in[i];
                const bool    pending = _requestDeadlineUs.load(std::memory_order_relaxed) != 0;
                if (_heldCount == 0 && !(pending && b == 0x1B)) {
                        emit(b);
                        continue;
                }
                _held[_heldCount++] = b;
                Size        size;
                const Match m = match(_held, _heldCount, size);
                if (m == Match::Complete) {
                        setSize(size);
                        _requestDeadlineUs.store(0, std::memory_order_relaxed);
                        _unanswered = 0;
                        _heldCount = 0;
                } else if (m == Match::No || _heldCount == sizeof(_held)) {
                        // Not the answer: what was held is input after all. An ESC
                        // ending it may start the answer.
                        const bool restart = b == 0x1B && _heldCount > 1;
                        const size_t keep = restart ? _heldCount - 1 : _heldCount;
                        for (size_t k = 0; k < keep; ++k) emit(_held[k]);
                        _heldCount = 0;
                        if (restart) _held[_heldCount++] = 0x1B;
                }
        }
        return makeResult(out);
}

Result<size_t> Console::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        if (size_t n = drainHeld(buf); n > 0) return makeResult(n);
        // No answer expected: the device's input as it is.
        if (_requestDeadlineUs.load(std::memory_order_relaxed) == 0 && _heldCount == 0)
                return readInput(buf, timeoutMs);
        const Deadline deadline(timeoutMs);
        for (;;) {
                (void)expireRequest();
                if (size_t n = drainHeld(buf); n > 0) return makeResult(n);
                const int64_t until = _requestDeadlineUs.load(std::memory_order_relaxed);
                if (until == 0 && _heldCount == 0) {
                        // Answered meanwhile: back to the plain path, for what time is left.
                        if (deadline.isForever()) return readInput(buf, WaitForever);
                        if (deadline.isExpired()) return makeError<size_t>(Error::Timeout);
                        return readInput(buf, static_cast<unsigned int>(deadline.remainingUs() / 1000 + 1));
                }
                // Wait no longer than the request has left, so held bytes go out.
                int64_t waitUs = until != 0 ? until - Detail::monotonicUs() : 0;
                if (!deadline.isForever() && deadline.remainingUs() < waitUs) waitUs = deadline.remainingUs();
                const unsigned int wait = static_cast<unsigned int>(waitUs > 0 ? waitUs / 1000 + 1 : 1);
                const size_t       chunk = buf.size() < SlowChunk ? buf.size() : SlowChunk;
                Result<size_t>     r = readInput(buf.first(chunk), wait);
                if (r.isError()) {
                        if (r.error() == Error::Timeout && !deadline.isExpired()) continue;
                        // Held bytes still reach the reader before the error.
                        if (size_t n = drainHeld(buf, true); n > 0) return makeResult(n);
                        return r;
                }
                Result<size_t> f = filter(buf, r.value());
                if (f.value() > 0) return f;
        }
}

Result<size_t> Console::tryRead(WritableByteSpan buf) {
        if (buf.isEmpty()) return makeError<size_t>(Error::Invalid);
        if (size_t n = drainHeld(buf); n > 0) return makeResult(n);
        if (_requestDeadlineUs.load(std::memory_order_relaxed) == 0 && _heldCount == 0) return tryReadInput(buf);
        for (;;) {
                (void)expireRequest();
                if (size_t n = drainHeld(buf); n > 0) return makeResult(n);
                const size_t   chunk = buf.size() < SlowChunk ? buf.size() : SlowChunk;
                Result<size_t> r = tryReadInput(buf.first(chunk));
                if (r.isError()) return r;
                Result<size_t> f = filter(buf, r.value());
                if (f.value() > 0) return f;
        }
}

BMDS_NAMESPACE_END
