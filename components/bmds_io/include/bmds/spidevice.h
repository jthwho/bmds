/**
 * @file      spidevice.h
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
#include <bmds/stream.h>

BMDS_NAMESPACE_BEGIN

namespace SpiPorts {
        struct Port;
}

/**
 * @brief A device on an SPI port, `/dev/spidevB.C` (devplan/io.md §11.5): chip select C of
 *        `io/spiB`, with its own settings, as Linux's `spi_device`.
 *
 * Made when its port first starts with that chip select, and kept: a file open on it across a
 * stop gets `NotExist` until the port runs again.
 *
 * @par Thread Safety
 * Any task: the port's lock keeps messages whole.
 */
class SpiDevice final : public Stream {
        public:
                /** @brief Linux's `SPI_*` mode bits (`bmds/sys.h`'s `BMDS_SPI_...`). */
                static constexpr uint32_t Cpha = 0x1;       ///< Clock phase.
                static constexpr uint32_t Cpol = 0x2;       ///< Clock polarity (idles high).
                static constexpr uint32_t CsHigh = 0x4;     ///< Chip select active high.
                static constexpr uint32_t LsbFirst = 0x8;   ///< Least significant bit first.
                static constexpr uint32_t ThreeWire = 0x10; ///< MOSI both ways (the port's).
                static constexpr uint32_t Loop = 0x20;      ///< MISO read from MOSI (the port's).
                static constexpr uint32_t NoCs = 0x40;      ///< No chip select (the port's).

                /** @brief The bits a program sets; the rest are the port's. */
                static constexpr uint32_t SettableModeBits = Cpha | Cpol | CsHigh | LsbFirst;

                /** @brief The most transfers a message takes, and bytes in all. */
                static constexpr size_t MaxTransfers = 64;
                static constexpr size_t MaxBytes = 65536;

                /** @brief A device's settings. */
                struct Settings {
                                uint32_t mode = 0;          ///< The mode bits (`Cpha`...).
                                uint8_t  bits = 8;          ///< 8, 16 or 32.
                                uint32_t speedHz = 1000000; ///< As set (the controller may run slower).
                };

                /** @brief One transfer of a message (pointers the caller checked). */
                struct Transfer {
                                const uint8_t *tx = nullptr;     ///< Sent, or null: zeros.
                                uint8_t       *rx = nullptr;     ///< Read into, or null.
                                uint32_t       length = 0;       ///< Bytes.
                                uint32_t       speedHz = 0;      ///< 0: the device's.
                                uint16_t       delayUs = 0;      ///< A wait after it.
                                uint8_t        bits = 0;         ///< 0: the device's.
                                bool           csChange = false; ///< CS released after it (not the last).
                };

                SpiDevice(SpiPorts::Port &port, uint8_t index) : _port(port), _index(index) {}

                /** @brief B of `/dev/spidevB.C`. */
                unsigned bus() const;

                /** @brief C of `/dev/spidevB.C`. */
                unsigned index() const { return _index; }

                /** @brief True while its port runs with it (its file is in `/dev`). */
                bool isRunning() const;

                /** @brief Device @p index of port `io/spi`@p bus, if it was ever started, or null. */
                static SpiDevice *find(unsigned bus, unsigned index);

                /**
                 * @brief Its settings, and the clock the controller runs it at (0: not used yet).
                 * @return `Ok`, or `NotExist` (its port isn't running with it).
                 * @blocking The port's lock.
                 */
                Error settings(Settings &out, uint32_t *actualHz) const;

                /**
                 * @brief Sets its settings (Linux's `SPI_IOC_WR_*`), until the port starts again.
                 * @return `Ok`; `Invalid` (a mode bit not settable, or the port's changed; a word
                 *         size; a speed under 1 kHz), `NotExist`.
                 * @blocking The port's lock.
                 */
                Error setSettings(const Settings &s);

                /**
                 * @brief Moves @p transfers as one message, its chip select held between them
                 *        (released after one with `csChange`, and at the end).
                 * @return The bytes moved; `Invalid` (a word size or a length not whole words, a
                 *         count, both ways at once on 3-wire), `OutOfRange` (past `MaxBytes`),
                 *         `NotExist`, `IoError`.
                 * @blocking The port's lock, and the transfers.
                 */
                Result<size_t> transfer(Span<const Transfer> transfers);

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                SpiDevice     *spiDevice() override { return this; }

        private:
                friend struct SpiPorts::Port;

                SpiPorts::Port &_port;
                uint8_t         _index;
                Settings        _settings;
                void           *_handle = nullptr; // ESP-IDF's device, added for _handleMode and _handleHz
                uint32_t        _handleMode = 0;
                uint32_t        _handleHz = 0;
                uint32_t        _actualHz = 0;
};

BMDS_NAMESPACE_END
