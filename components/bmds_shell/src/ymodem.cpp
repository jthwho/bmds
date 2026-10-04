/**
 * @file      ymodem.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstdio>
#include <cstring>
#include <bmds/ymodem.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using namespace Ymodem;

        constexpr uint8_t CtrlCByte = 0x03;

        // Within a block, between its bytes.
        constexpr unsigned int CharTimeoutMs = 1000;
        // For the next block, or an answer to one.
        constexpr unsigned int BlockTimeoutMs = 10000;
        // The receiver repeats its `C` this often until the sender starts (a
        // sender started after the first one waits at most this long).
        constexpr unsigned int CrcIntervalMs = 1000;
        // Purging after a bad block: until the line has been quiet this long.
        constexpr unsigned int PurgeQuietMs = 200;
        // Writing to the line.
        constexpr unsigned int WriteTimeoutMs = 5000;

        // Header byte, number, complement, data, CRC.
        constexpr size_t FrameBytes = 3 + BlockBytes + 2;

        // Reads one byte; `Timeout` if none came.
        Error readByte(Stream &line, unsigned int timeoutMs, uint8_t &out) {
                auto [n, err] = line.read(WritableByteSpan(&out, 1), timeoutMs);
                if (err.isOk() && n == 0) err = Error::Timeout;
                return err;
        }

        Error readExactly(Stream &line, uint8_t *buf, size_t count) {
                size_t got = 0;
                while (got < count) {
                        auto [n, err] = line.read(WritableByteSpan(buf + got, count - got), CharTimeoutMs);
                        if (err.isError()) return err;
                        if (n == 0) return Error::Timeout;
                        got += n;
                }
                return Error::Ok;
        }

        Error sendByte(Stream &line, uint8_t b) {
                return line.write(ByteSpan(&b, 1), WriteTimeoutMs).error();
        }

        // Throws away input until the line is quiet (after a bad block).
        void purge(Stream &line) {
                uint8_t buf[64];
                for (;;) {
                        auto [n, err] = line.read(WritableByteSpan(buf), PurgeQuietMs);
                        if (err.isError() || n == 0) return;
                }
        }

        // Cancels the transfer, as lrzsz does: CANs, then backspaces over
        // them in case a terminal shows them.
        void cancel(Stream &line) {
                static constexpr uint8_t Sequence[] = {Can,  Can,  Can,  Can,  Can,  Can,  Can,  Can,
                                                       0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08};
                (void)line.write(ByteSpan(Sequence, sizeof(Sequence)), WriteTimeoutMs);
                return;
        }

        enum class Got : uint8_t {
                Block,   // a good block: number and size set
                Eot,     // end of file
                Cancel,  // CAN CAN
                CtrlC,   // a Ctrl-C byte (when it quits)
                Timeout, // nothing
                Bad,     // a damaged block (purged)
                Failed,  // the line failed
        };

        // What the receiver reads: one block or control byte.
        struct Frame {
                        uint8_t buf[FrameBytes];
                        uint8_t number = 0;
                        size_t  size = 0;
                        Error   error;

                        ByteSpan data() const { return ByteSpan(buf + 3, size); }

                        Got read(Stream &line, unsigned int timeoutMs, bool ctrlCQuits) {
                                for (;;) {
                                        uint8_t b = 0;
                                        error = readByte(line, timeoutMs, b);
                                        if (error == Error::Timeout) return Got::Timeout;
                                        if (error.isError()) return Got::Failed;
                                        switch (b) {
                                                case Soh:
                                                case Stx: return body(line, b);
                                                case Eot: return Got::Eot;
                                                case Can:
                                                        if (readByte(line, CharTimeoutMs, b).isOk() && b == Can)
                                                                return Got::Cancel;
                                                        break;
                                                case CtrlCByte:
                                                        if (ctrlCQuits) return Got::CtrlC;
                                                        break;
                                                default: break; // noise before a block
                                        }
                                }
                        }

                private:
                        Got body(Stream &line, uint8_t header) {
                                size = header == Stx ? BlockBytes : 128;
                                buf[0] = header;
                                error = readExactly(line, buf + 1, 2 + size + 2);
                                if (error.isError() && error != Error::Timeout) return Got::Failed;
                                const uint16_t crc = static_cast<uint16_t>((buf[3 + size] << 8) | buf[4 + size]);
                                if (error.isError() || static_cast<uint8_t>(buf[1] ^ buf[2]) != 0xFF ||
                                    crc16(data()) != crc) {
                                        purge(line);
                                        return Got::Bad;
                                }
                                number = buf[1];
                                return Got::Block;
                        }
        };

        // Parses block 0: "name\0size mtime mode ...".
        Error parseHeader(ByteSpan data, FileHeader &h) {
                const char *text = reinterpret_cast<const char *>(data.data());
                size_t      i = 0;
                while (i < data.size() && text[i] != '\0') ++i;
                Error err = h.name.assign(StringView(text, i));
                if (err.isError() || i >= data.size()) return err;
                ++i;
                auto number = [&](unsigned base, uint64_t &out) {
                        while (i < data.size() && text[i] == ' ') ++i;
                        bool any = false;
                        out = 0;
                        while (i < data.size() && text[i] >= '0' && text[i] < static_cast<char>('0' + base)) {
                                out = out * base + static_cast<uint64_t>(text[i++] - '0');
                                any = true;
                        }
                        return any;
                };
                uint64_t value = 0;
                if (number(10, value)) {
                        h.size = value;
                        h.hasSize = true;
                        if (number(8, value)) h.modified = Timestamp::fromSeconds(static_cast<int64_t>(value));
                }
                return Error::Ok;
        }

        // One file's data blocks, after its block 0. Returns at its end (EOT
        // acknowledged) or on an error.
        Error receiveFile(Stream &line, Frame &frame, ReceiveHandler &handler, const FileHeader &h, Stats &stats) {
                uint8_t  expected = 1;
                uint64_t remaining = h.size;
                uint64_t received = 0;
                bool     eotSeen = false;
                int      errors = 0;
                for (;;) {
                        switch (frame.read(line, BlockTimeoutMs, false)) {
                                case Got::Block: {
                                        errors = 0;
                                        if (frame.number == static_cast<uint8_t>(expected - 1)) {
                                                // Our ACK was lost: it came again.
                                                if (sendByte(line, Ack).isError()) return Error::Timeout;
                                                break;
                                        }
                                        if (frame.number != expected) {
                                                cancel(line);
                                                return Error::CorruptData;
                                        }
                                        ++expected;
                                        eotSeen = false;
                                        // Acknowledge first: the sender prepares the next
                                        // block while this one is stored.
                                        if (sendByte(line, Ack).isError()) return Error::Timeout;
                                        ByteSpan data = frame.data();
                                        if (h.hasSize) {
                                                if (data.size() > remaining)
                                                        data = data.first(static_cast<size_t>(remaining));
                                                remaining -= data.size();
                                        }
                                        received += data.size();
                                        if (data.isEmpty()) break;
                                        Error err = handler.data(data);
                                        if (err.isError()) {
                                                cancel(line);
                                                return err;
                                        }
                                        break;
                                }
                                case Got::Eot:
                                        // The first EOT is answered with NAK, in case it was noise.
                                        if (!eotSeen) {
                                                eotSeen = true;
                                                if (sendByte(line, Nak).isError()) return Error::Timeout;
                                                break;
                                        }
                                        if (sendByte(line, Ack).isError()) return Error::Timeout;
                                        {
                                                Error err = handler.end();
                                                if (err.isError()) {
                                                        cancel(line);
                                                        return err;
                                                }
                                        }
                                        ++stats.files;
                                        stats.bytes += received;
                                        return Error::Ok;
                                case Got::Cancel: return Error::Cancelled;
                                case Got::Failed: return frame.error;
                                case Got::CtrlC:
                                case Got::Timeout:
                                case Got::Bad:
                                        ++stats.retries;
                                        if (++errors > MaxRetries) {
                                                cancel(line);
                                                return Error::CorruptData;
                                        }
                                        if (sendByte(line, Nak).isError()) return Error::Timeout;
                                        break;
                        }
                }
        }

} // namespace

uint16_t Ymodem::crc16(ByteSpan data, uint16_t crc) {
        for (uint8_t b : data) {
                crc = static_cast<uint16_t>(crc ^ (b << 8));
                for (int i = 0; i < 8; ++i)
                        crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
        }
        return crc;
}

Error Ymodem::receive(Stream &line, ReceiveHandler &handler, Stats &stats, bool ctrlCQuits) {
        stats = Stats();
        Frame frame;
        bool  first = true;
        for (;;) {
                // Block 0: ask with `C` until it comes (a minute at first, then
                // ten tries).
                unsigned waited = 0;
                int      errors = 0;
                bool     haveHeader = false;
                while (!haveHeader) {
                        if (sendByte(line, Crc).isError()) return Error::Timeout;
                        switch (frame.read(line, CrcIntervalMs, first && ctrlCQuits)) {
                                case Got::Block:
                                        if (frame.number != 0) {
                                                // An XMODEM sender (no block 0), or lost sync.
                                                cancel(line);
                                                return Error::CorruptData;
                                        }
                                        haveHeader = true;
                                        break;
                                case Got::Eot:
                                        // The last file's EOT again: our ACK was lost.
                                        if (sendByte(line, Ack).isError()) return Error::Timeout;
                                        break;
                                case Got::Cancel: return Error::Cancelled;
                                case Got::CtrlC: cancel(line); return Error::Cancelled;
                                case Got::Failed: return frame.error;
                                case Got::Timeout:
                                        waited += CrcIntervalMs;
                                        if (first ? waited >= StartTimeoutMs : ++errors > MaxRetries) {
                                                cancel(line);
                                                return Error::Timeout;
                                        }
                                        break;
                                case Got::Bad:
                                        ++stats.retries;
                                        if (++errors > MaxRetries) {
                                                cancel(line);
                                                return Error::CorruptData;
                                        }
                                        break;
                        }
                }
                first = false;
                FileHeader h;
                if (parseHeader(frame.data(), h).isError()) {
                        cancel(line);
                        return Error::NoMem;
                }
                if (h.name.isEmpty()) return sendByte(line, Ack); // end of the batch
                Error err = handler.begin(h);
                if (err.isError()) {
                        cancel(line);
                        return err;
                }
                err = sendByte(line, Ack);
                if (err.isOk()) err = sendByte(line, Crc);
                if (err.isOk()) err = receiveFile(line, frame, handler, h, stats);
                if (err.isError()) {
                        handler.abort();
                        return err;
                }
        }
}

namespace {

        // Waits for the receiver's `C` (CAN CAN cancels).
        Error waitForCrc(Stream &line, unsigned int timeoutMs, bool ctrlCQuits) {
                uint8_t  b = 0;
                bool     can = false;
                unsigned waited = 0;
                while (waited < timeoutMs) {
                        Error err = readByte(line, 1000, b);
                        if (err == Error::Timeout) {
                                waited += 1000;
                                continue;
                        }
                        if (err.isError()) return err;
                        if (b == Crc) return Error::Ok;
                        if (b == CtrlCByte && ctrlCQuits) return Error::Cancelled;
                        if (b == Can && can) return Error::Cancelled;
                        can = b == Can;
                }
                return Error::Timeout;
        }

        // Sends one block until it is acknowledged.
        Error sendBlock(Stream &line, uint8_t *frame, uint8_t number, size_t size, Stats &stats) {
                frame[0] = size == BlockBytes ? Stx : Soh;
                frame[1] = number;
                frame[2] = static_cast<uint8_t>(~number);
                const uint16_t crc = crc16(ByteSpan(frame + 3, size));
                frame[3 + size] = static_cast<uint8_t>(crc >> 8);
                frame[4 + size] = static_cast<uint8_t>(crc);
                for (int attempt = 0; attempt <= MaxRetries; ++attempt) {
                        if (attempt > 0) ++stats.retries;
                        Error err = line.write(ByteSpan(frame, size + 5), WriteTimeoutMs).error();
                        if (err.isError()) return err;
                        // The answer: ACK, NAK (again), CAN CAN. (A `C` is not taken as
                        // a NAK for block 0: it may be one the receiver sent before we
                        // started.)
                        bool can = false;
                        for (;;) {
                                uint8_t b = 0;
                                err = readByte(line, BlockTimeoutMs, b);
                                if (err == Error::Timeout) break;
                                if (err.isError()) return err;
                                if (b == Ack) return Error::Ok;
                                if (b == Nak) break;
                                if (b == Can && can) return Error::Cancelled;
                                can = b == Can;
                        }
                }
                cancel(line);
                return Error::CorruptData;
        }

        Error sendEot(Stream &line, Stats &stats) {
                for (int attempt = 0; attempt <= MaxRetries; ++attempt) {
                        // The receiver answers the first EOT with NAK on purpose.
                        if (attempt > 1) ++stats.retries;
                        Error err = sendByte(line, Eot);
                        if (err.isError()) return err;
                        uint8_t b = 0;
                        err = readByte(line, BlockTimeoutMs, b);
                        if (err.isOk() && b == Ack) return Error::Ok;
                        if (err.isError() && err != Error::Timeout) return err;
                }
                cancel(line);
                return Error::CorruptData;
        }

} // namespace

Error Ymodem::send(Stream &line, SendSource &source, Stats &stats, bool ctrlCQuits) {
        stats = Stats();
        uint8_t frame[FrameBytes];
        Error   err = waitForCrc(line, StartTimeoutMs, ctrlCQuits);
        if (err.isError()) {
                if (err != Error::Cancelled || ctrlCQuits) cancel(line);
                return err;
        }
        for (;;) {
                FileHeader h;
                auto [more, nerr] = source.next(h);
                if (nerr.isError()) {
                        cancel(line);
                        return nerr;
                }
                std::memset(frame + 3, 0, BlockBytes);
                size_t size = 128;
                if (more) {
                        // name NUL size mtime(octal) mode(octal), as lrzsz writes it.
                        char *const  text = reinterpret_cast<char *>(frame + 3);
                        const size_t nameBytes = h.name.size() < BlockBytes - 64 ? h.name.size() : BlockBytes - 64;
                        std::memcpy(text, h.name.data(), nameBytes);
                        const int n = std::snprintf(
                                text + nameBytes + 1, BlockBytes - nameBytes - 1, "%llu %llo 100644",
                                static_cast<unsigned long long>(h.size),
                                static_cast<unsigned long long>(h.modified.seconds() > 0 ? h.modified.seconds() : 0));
                        if (nameBytes + 1 + static_cast<size_t>(n) + 1 > 128) size = BlockBytes;
                }
                // Drop any further `C`s the receiver sent while waiting.
                uint8_t stale = 0;
                while (readByte(line, 1, stale).isOk()) {}
                err = sendBlock(line, frame, 0, size, stats);
                if (err.isError() || !more) return err;
                err = waitForCrc(line, BlockTimeoutMs, false);
                uint8_t  number = 1;
                uint64_t sent = 0;
                while (err.isOk()) {
                        // Fill a whole block (the source may return less).
                        size_t got = 0;
                        while (got < BlockBytes) {
                                auto [n, rerr] = source.read(WritableByteSpan(frame + 3 + got, BlockBytes - got));
                                if (rerr.isError()) {
                                        cancel(line);
                                        return rerr;
                                }
                                if (n == 0) break;
                                got += n;
                        }
                        if (got == 0) break;
                        size = got <= 128 ? 128 : BlockBytes;
                        std::memset(frame + 3 + got, Pad, size - got);
                        err = sendBlock(line, frame, number++, size, stats);
                        sent += got;
                }
                if (err.isOk()) err = sendEot(line, stats);
                if (err.isOk()) err = waitForCrc(line, BlockTimeoutMs, false);
                if (err.isError()) return err;
                ++stats.files;
                stats.bytes += sent;
        }
}

BMDS_NAMESPACE_END
