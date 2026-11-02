/**
 * @file      sdcard.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <bmds/blockdevice.h>
#include <bmds/config.h>
#include <bmds/mutex.h>
#include <bmds/semaphore.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The microSD card slot, as a block device.
 *
 * Owns the slot's hardware, as the board definition describes it: the IO
 * supply (an on-chip LDO), the card power switch, the card-detect switch
 * and the SDMMC host. The card protocol is ESP-IDF's (`sdmmc`); requests
 * are serialized by a mutex.
 *
 * **No allocation per request.** A request whose buffer the host can reach
 * by DMA (DMA-capable memory, cache-line aligned start and length, as
 * `MemSpace::PsramDma` allocations are) goes straight to DMA. Anything else
 * is copied through one bounce buffer allocated by `init()`
 * (`CONFIG_BMDS_SD_BOUNCE_BYTES`, `PsramDma`), so ESP-IDF never allocates
 * one per call.
 *
 * **Hot-plug** is driven by the storage service (`Storage`): the card-detect
 * line interrupts on every edge, `waitForDetectChange()` lets the service
 * wait for one (and debounce), and it calls `attach()` and `detach()`.
 * Requests while no card is attached, or once the detect switch says it was
 * pulled, fail with `NoMedia`.
 *
 * @par Thread Safety
 * Thread-safe. `init()` is called once, at boot.
 */
class SdCard final : public BlockDevice {
        public:
                /** @brief Card types. */
                enum class Type : uint8_t {
                        Sdsc,  ///< Standard capacity (up to 2 GB).
                        Sdhc,  ///< High capacity (up to 32 GB).
                        Sdxc,  ///< Extended capacity (over 32 GB).
                        Mmc,   ///< MMC.
                        Other, ///< Anything else (SDIO).
                };

                /** @brief What is known about the attached card. */
                struct Info {
                                bool     attached = false;   ///< A card is initialized and usable (the rest is valid).
                                Type     type = Type::Other; ///< Card type.
                                char     name[8] = {};       ///< Product name (NUL-terminated).
                                uint8_t  manufacturerId = 0; ///< CID manufacturer ID.
                                uint16_t oemId = 0;          ///< CID OEM/application ID.
                                uint8_t  revision = 0;       ///< CID product revision (BCD n.m).
                                uint32_t serial = 0;         ///< CID serial number.
                                uint16_t year = 0;           ///< Manufacturing year.
                                uint8_t  month = 0;          ///< Manufacturing month (1-12).
                                uint64_t sectorCount = 0;    ///< Capacity in sectors.
                                uint32_t sectorSize = 0;     ///< Sector size in bytes.
                                uint32_t clockKhz = 0;       ///< Bus clock in use.
                                uint8_t  busWidth = 0;       ///< Data lines in use (1 or 4).
                                bool     uhs = false;        ///< UHS-I: signalling at 1.8 V.
                };

                /**
                 * @brief Brings up the slot: IO supply, power switch, card
                 *        detect and its interrupt, the bounce buffer.
                 *
                 * Doesn't touch a card; the storage service attaches one. The
                 * SDMMC host runs only while a card is attached.
                 * @return `Ok`, `NotSupported` (the board has no SD slot),
                 *         `NoMem`, or the ESP-IDF failure.
                 */
                static Error init();

                /** @brief Returns the slot. Valid after a successful `init()`. */
                static SdCard &instance();

                /**
                 * @brief Returns true if the card-detect switch says a card is in.
                 *
                 * The raw switch: it bounces while a card goes in or out.
                 * @nonblocking
                 */
                bool isInserted() const;

                /**
                 * @brief Waits for the card-detect switch to change.
                 *
                 * Edges since the last call count; several collapse into one.
                 * @param timeoutMs Timeout, or `WaitForever`.
                 * @return `Ok` (it changed) or `Timeout`.
                 * @blocking
                 */
                Error waitForDetectChange(unsigned int timeoutMs);

                /**
                 * @brief Powers and initializes the inserted card.
                 *
                 * Starts a fresh SDMMC host, power-cycles the card, identifies
                 * it at 400 kHz on one data line, then switches to
                 * 4 bits and the fastest clock both sides support (up to
                 * `CONFIG_BMDS_SD_MAX_KHZ`). Takes 100-300 ms.
                 * @return `Ok`, `NoMedia` (no card), or the reason it failed.
                 * @blocking
                 */
                Error attach();

                /**
                 * @brief Makes the command in flight fail now: the card has been pulled out (its
                 *        switch has said so for the debounce's time). Without it a transfer
                 *        waits out ESP-IDF's timeouts first. Later requests fail with `NoMedia`
                 *        while the switch says the card is out.
                 * @nonblocking Doesn't take the card's lock (a transfer holds it).
                 */
                void cancelTransfer();

                /**
                 * @brief Forgets the card, stops the SDMMC host and cuts the
                 *        card's power. Requests fail with `NoMedia` from now on.
                 * @blocking Waits for a request in progress.
                 */
                void detach();

                /** @brief Returns true while a card is attached. */
                bool isAttached() const { return _attached.load(std::memory_order_acquire); }

                /** @brief Returns the attached card's details (`attached` false if none). */
                Info info() const;

                uint32_t sectorSize() const override;
                uint64_t sectorCount() const override;
                bool     isPresent() const override;
                uint32_t eraseBlockSectors() const override;
                Error    sync() override;
                Error    trim(uint64_t first, uint64_t count) override;

                /** @brief Use `init()` and `instance()`. */
                explicit SdCard(uint8_t *bounce);

        protected:
                Error readSectors(uint64_t sector, WritableByteSpan buf) override;
                Error writeSectors(uint64_t sector, ByteSpan data) override;

        private:
                static void detectIsr(void *arg);

                // The ESP-IDF card state lives in sdcard.cpp, beside the
                // singleton; these use it under _lock.
                Error checkAttached() const;
                Error transfer(uint64_t sector, uint8_t *buf, size_t bytes, bool write);

                mutable Mutex     _lock;
                Semaphore         _detect;
                std::atomic<bool> _attached = false;
                uint8_t          *_bounce;
};

BMDS_NAMESPACE_END
