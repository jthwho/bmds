/**
 * @file      ymodem.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * YMODEM batch file transfer (what `lrzsz`'s `sz --ymodem` / `rz --ymodem`
 * speak): file names, sizes and times in block 0, 1 KB data blocks with a
 * CRC-16, an ACK per block. Pure protocol over a `Stream`, so the host tests
 * run both ends against each other.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/error.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stream.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief YMODEM sender and receiver.
 *
 * The line is a `Stream` that reads with a timeout (a console in raw mode,
 * or pipes in tests). Both ends follow Forsberg's YMODEM description as
 * `lrzsz` implements it:
 *
 * - The receiver asks with `C` (CRC mode); block 0 carries the name,
 *   then the decimal size, octal modification time and octal mode; an
 *   empty name ends the batch.
 * - Data blocks are `STX` (1024 bytes) or `SOH` (128), numbered from 1
 *   modulo 256, with the complement and a CRC-16/XMODEM; the last one is
 *   padded with `0x1A` and the receiver cuts the file to the size.
 * - A bad block is answered with `NAK` (10 tries), a repeated one with
 *   `ACK`; two `CAN` bytes cancel either way; `EOT` ends a file (the
 *   receiver answers the first with `NAK`, the second with `ACK`).
 *
 * @par Thread Safety
 * Each call runs one transfer on the calling task.
 */
namespace Ymodem {

        /** @brief The protocol's control bytes. */
        enum Byte : uint8_t {
                Soh = 0x01, ///< A 128-byte block follows.
                Stx = 0x02, ///< A 1024-byte block follows.
                Eot = 0x04, ///< End of the file.
                Ack = 0x06, ///< Block received.
                Nak = 0x15, ///< Send it again.
                Can = 0x18, ///< Cancel (two in a row).
                Crc = 'C',  ///< The receiver wants CRC-16 blocks.
                Pad = 0x1A, ///< Pads the last block (CP/M end of file).
        };

        /** @brief Bytes in a data block. */
        inline constexpr size_t BlockBytes = 1024;

        /** @brief Tries for one block before giving up. */
        inline constexpr int MaxRetries = 10;

        /** @brief How long either side waits for the other to start, in milliseconds. */
        inline constexpr unsigned int StartTimeoutMs = 60000;

        /** @brief Returns the CRC-16/XMODEM (polynomial 0x1021, initial 0) of @p data, continuing @p crc. */
        uint16_t crc16(ByteSpan data, uint16_t crc = 0);

        /** @brief What block 0 says about a file. */
        struct FileHeader {
                        String    name;            ///< As sent (the receiver decides what to make of paths).
                        uint64_t  size = 0;        ///< Its size, if `hasSize`.
                        bool      hasSize = false; ///< The sender gave a size.
                        Timestamp modified;        ///< Modification time; zero if not given.
        };

        /** @brief Counts of a transfer. */
        struct Stats {
                        size_t   files = 0;   ///< Files completed.
                        uint64_t bytes = 0;   ///< Bytes of completed files.
                        unsigned retries = 0; ///< Blocks sent or asked for again.
        };

        /** @brief Where a receiver puts what arrives. */
        class ReceiveHandler {
                public:
                        /** @brief A file starts; an error cancels the transfer. */
                        virtual Error begin(const FileHeader &header) = 0;

                        /** @brief The next piece of the current file (padding removed when the size is known). */
                        virtual Error data(ByteSpan bytes) = 0;

                        /** @brief The current file is complete. */
                        virtual Error end() = 0;

                        /** @brief The current file is incomplete: the transfer failed. */
                        virtual void abort() = 0;

                protected:
                        ~ReceiveHandler() = default;
        };

        /** @brief Where a sender gets its files. */
        class SendSource {
                public:
                        /**
                         * @brief Moves to the next file and describes it.
                         * @return True for a file, false when there are no more; an error cancels.
                         */
                        virtual Result<bool> next(FileHeader &header) = 0;

                        /** @brief Reads the current file; 0 bytes at its end. */
                        virtual Result<size_t> read(WritableByteSpan buf) = 0;

                protected:
                        ~SendSource() = default;
        };

        /**
         * @brief Receives a batch of files.
         * @param line       The serial line (raw: no translation, no signals).
         * @param handler    Gets the files.
         * @param stats      [out] What was received.
         * @param ctrlCQuits True if a Ctrl-C byte before the first block cancels
         *                   (someone typed it at the terminal).
         * @return `Ok`; `Cancelled` (by the other end, Ctrl-C, or a handler
         *         error, which is then returned instead); `Timeout`;
         *         `CorruptData` (too many bad blocks or lost sync).
         * @blocking
         */
        Error receive(Stream &line, ReceiveHandler &handler, Stats &stats, bool ctrlCQuits);

        /**
         * @brief Sends a batch of files.
         * @return `Ok`, `Cancelled`, `Timeout`, `CorruptData` (too many NAKs),
         *         or a source error.
         * @blocking
         */
        Error send(Stream &line, SendSource &source, Stats &stats, bool ctrlCQuits);

} // namespace Ymodem

BMDS_NAMESPACE_END
