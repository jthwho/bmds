/**
 * @file      sdcard.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <new>
#include <driver/gpio.h>
#include <driver/sdmmc_default_configs.h>
#include <driver/sdmmc_host.h>
#include <esp_memory_utils.h>
#include <sd_protocol_defs.h>
#include <sd_pwr_ctrl_by_on_chip_ldo.h>
#include <sdmmc_cmd.h>
#include <bmds/board.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/sdcard.h>
#include <bmds/staticobject.h>
#include <bmds/task.h>

BMDS_DEBUG(SdCard)

BMDS_NAMESPACE_BEGIN

namespace {

        static_assert(CONFIG_BMDS_SD_BOUNCE_BYTES % 512 == 0, "SD bounce buffer: a multiple of 512 bytes");

        // How long the card stays unpowered, so it really resets, and how long
        // its supply gets to come up before the first command.
        constexpr unsigned int PowerOffMs = 20;
        constexpr unsigned int PowerOnMs = 10;

        // SDHC cards stop at 32 GB; anything bigger is SDXC.
        constexpr uint64_t SdhcMaxSectors = 32ull * 1024 * 1024 * 1024 / 512;

        constinit StaticObject<SdCard> slot;

        // The ESP-IDF card state (including its copy of the host
        // configuration), in PSRAM: only tasks touch it, under the lock.
        sdmmc_card_t        *card = nullptr;
        sd_pwr_ctrl_handle_t ioSupply = nullptr;

        void setPower(bool on) {
                const GpioLine &power = board().sdCard.powerOn;
                if (power.isValid()) (void)gpio_set_level(static_cast<gpio_num_t>(power.gpio), on != power.activeLow);
                return;
        }

        Error espError(esp_err_t err) {
                // Past the end of the card (a smaller card swapped in).
                if (err == ESP_ERR_INVALID_SIZE) return Error::OutOfRange;
                return Error::fromEspErr(err);
        }

        sdmmc_host_t hostConfig() {
                const BoardSdCard &sd = board().sdCard;
                sdmmc_host_t       host = SDMMC_HOST_DEFAULT();
                host.slot = sd.slot;
                host.max_freq_khz = CONFIG_BMDS_SD_MAX_KHZ;
                // DDR is for MMC and UHS-I, which this slot doesn't do (yet).
                host.flags &= ~SDMMC_HOST_FLAG_DDR;
                host.io_voltage = static_cast<float>(sd.ioLdoMv) / 1000.0f;
                host.pwr_ctrl_handle = ioSupply;
                return host;
        }

        // The SDMMC host and slot, brought up for each card and taken down
        // after it: once a card has been set up, IDF's slot keeps its bus
        // width (and the D3 handling that puts a new card into SD mode)
        // for good, so only a fresh slot can identify the next card.
        bool hostRunning = false;

        esp_err_t startHost() {
                const BoardSdCard &sd = board().sdCard;
                esp_err_t          err = sdmmc_host_init();
                if (err != ESP_OK) return err;
                sdmmc_slot_config_t config = {};
                config.clk = static_cast<gpio_num_t>(sd.clk);
                config.cmd = static_cast<gpio_num_t>(sd.cmd);
                config.d0 = static_cast<gpio_num_t>(sd.d0);
                config.d1 = static_cast<gpio_num_t>(sd.d1);
                config.d2 = static_cast<gpio_num_t>(sd.d2);
                config.d3 = static_cast<gpio_num_t>(sd.d3);
                config.d4 = config.d5 = config.d6 = config.d7 = GPIO_NUM_NC;
                // Card detect is ours (an interrupt); there is no write protect.
                config.cd = SDMMC_SLOT_NO_CD;
                config.wp = SDMMC_SLOT_NO_WP;
                config.width = sd.busWidth;
                err = sdmmc_host_init_slot(sd.slot, &config);
                if (err != ESP_OK) {
                        (void)sdmmc_host_deinit();
                        return err;
                }
                hostRunning = true;
                return ESP_OK;
        }

        void stopHost() {
                if (!hostRunning) return;
                // Removing the last slot deletes the controller too.
                (void)sdmmc_host_deinit_slot(board().sdCard.slot);
                hostRunning = false;
                return;
        }

        Error initGpio(gpio_isr_t detectIsr, SdCard &c) {
                const BoardSdCard &sd = board().sdCard;
                if (sd.powerOn.isValid()) {
                        // Set the level before driving the pin: the card is
                        // powered by default, and stays so without a glitch.
                        setPower(true);
                        gpio_config_t power = {};
                        power.pin_bit_mask = 1ull << sd.powerOn.gpio;
                        power.mode = GPIO_MODE_OUTPUT;
                        const esp_err_t err = gpio_config(&power);
                        if (err != ESP_OK) return Error::fromEspErr(err);
                }
                if (!sd.cardDetect.isValid()) return Error();
                // The pull-up is on the board (powered by the IO supply).
                gpio_config_t detect = {};
                detect.pin_bit_mask = 1ull << sd.cardDetect.gpio;
                detect.mode = GPIO_MODE_INPUT;
                detect.intr_type = GPIO_INTR_ANYEDGE;
                esp_err_t err = gpio_config(&detect);
                // Shared with any other GPIO interrupt user; already there is fine.
                if (err == ESP_OK) err = gpio_install_isr_service(0);
                if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
                if (err == ESP_OK)
                        err = gpio_isr_handler_add(static_cast<gpio_num_t>(sd.cardDetect.gpio), detectIsr, &c);
                return Error::fromEspErr(err);
        }

} // namespace

SdCard::SdCard(uint8_t *bounce) : _detect(1, 0), _bounce(bounce) {}

Error SdCard::init() {
        const BoardSdCard &sd = board().sdCard;
        if (sd.busWidth == 0) return Error::NotSupported;

        auto *bounce = static_cast<uint8_t *>(Memory::alloc(MemSpace::PsramDma, CONFIG_BMDS_SD_BOUNCE_BYTES));
        card = static_cast<sdmmc_card_t *>(Memory::alloc(MemSpace::Psram, sizeof(sdmmc_card_t)));
        if (bounce == nullptr || card == nullptr) {
                Memory::free(bounce);
                Memory::free(card);
                card = nullptr;
                return Error::NoMem;
        }
        std::memset(card, 0, sizeof(*card));
        SdCard &c = slot.create(bounce);

        // The IO supply first: the card-detect pull-up hangs off it.
        if (sd.ioLdo != NoLdo) {
                const sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = sd.ioLdo};
                const esp_err_t                err = sd_pwr_ctrl_new_on_chip_ldo(&ldo, &ioSupply);
                if (err != ESP_OK) {
                        bmdsErr("SdCard: IO supply (LDO %d) failed: %s", sd.ioLdo, Error::fromEspErr(err).desc());
                        return Error::fromEspErr(err);
                }
        }
        Error err = initGpio(detectIsr, c);
        if (err.isError()) {
                bmdsErr("SdCard: GPIO setup failed: %s", err.desc());
                return err;
        }

        bmdsDebug("SdCard(%p): slot %u, %u-bit, card %s", &c, sd.slot, sd.busWidth,
                  c.isInserted() ? "inserted" : "not inserted");
        return Error();
}

SdCard &SdCard::instance() {
        return *slot;
}

void SdCard::detectIsr(void *arg) {
        static_cast<SdCard *>(arg)->_detect.releaseFromIsr();
        return;
}

bool SdCard::isInserted() const {
        const GpioLine &detect = board().sdCard.cardDetect;
        if (!detect.isValid()) return true; // No switch: assume a card.
        return (gpio_get_level(static_cast<gpio_num_t>(detect.gpio)) != 0) != detect.activeLow;
}

Error SdCard::waitForDetectChange(unsigned int timeoutMs) {
        return _detect.acquire(timeoutMs);
}

Error SdCard::attach() {
        Mutex::Locker lock(_lock, WaitForever);
        _attached.store(false, std::memory_order_release);
        if (!isInserted()) return Error::NoMedia;

        // A power cycle resets a card left in any state (after a reboot, or a
        // failed attempt).
        setPower(false);
        Task::sleep(PowerOffMs);
        setPower(true);
        Task::sleep(PowerOnMs);

        stopHost();
        esp_err_t err = startHost();
        if (err != ESP_OK) {
                setPower(false);
                bmdsErr("SdCard: SDMMC host setup failed: %s", Error::fromEspErr(err).desc());
                return Error::fromEspErr(err);
        }
        const sdmmc_host_t host = hostConfig();
        err = sdmmc_card_init(&host, card);
        if (err != ESP_OK) {
                stopHost();
                setPower(false);
                // Pulled out while it was being set up.
                if (!isInserted()) return Error::NoMedia;
                return Error::fromEspErr(err);
        }
        _attached.store(true, std::memory_order_release);
        return Error();
}

void SdCard::detach() {
        Mutex::Locker lock(_lock, WaitForever);
        _attached.store(false, std::memory_order_release);
        stopHost();
        setPower(false);
        return;
}

SdCard::Info SdCard::info() const {
        Mutex::Locker lock(_lock, WaitForever);
        Info          info;
        if (!isAttached()) return info;
        info.attached = true;
        if (card->is_mmc) {
                info.type = Type::Mmc;
        } else if (!card->is_mem) {
                info.type = Type::Other;
        } else if ((card->ocr & SD_OCR_SDHC_CAP) == 0) {
                info.type = Type::Sdsc;
        } else {
                info.type = static_cast<uint64_t>(card->csd.capacity) > SdhcMaxSectors ? Type::Sdxc : Type::Sdhc;
        }
        static_assert(sizeof(info.name) == sizeof(card->cid.name));
        std::memcpy(info.name, card->cid.name, sizeof(info.name));
        info.name[sizeof(info.name) - 1] = '\0';
        info.manufacturerId = static_cast<uint8_t>(card->cid.mfg_id);
        info.oemId = static_cast<uint16_t>(card->cid.oem_id);
        info.revision = static_cast<uint8_t>(card->cid.revision);
        info.serial = static_cast<uint32_t>(card->cid.serial);
        // SD: years since 2000 in the high bits, the month in the low four.
        info.year = static_cast<uint16_t>(2000 + (card->cid.date >> 4));
        info.month = static_cast<uint8_t>(card->cid.date & 0xf);
        info.sectorCount = static_cast<uint64_t>(card->csd.capacity);
        info.sectorSize = static_cast<uint32_t>(card->csd.sector_size);
        info.clockKhz = static_cast<uint32_t>(card->real_freq_khz);
        info.busWidth = static_cast<uint8_t>(1u << card->log_bus_width);
        return info;
}

uint32_t SdCard::sectorSize() const {
        return 512; // The SD block length; also what sdmmc reports for every card it accepts.
}

uint64_t SdCard::sectorCount() const {
        if (!isAttached()) return 0;
        Mutex::Locker lock(_lock, WaitForever);
        return isAttached() ? static_cast<uint64_t>(card->csd.capacity) : 0;
}

uint32_t SdCard::eraseBlockSectors() const {
        Mutex::Locker lock(_lock, WaitForever);
        // The SD allocation unit (from the SD status register), in sectors.
        if (!isAttached() || card->ssr.alloc_unit_kb == 0) return 1;
        return card->ssr.alloc_unit_kb * 2;
}

bool SdCard::isPresent() const {
        return isAttached() && isInserted();
}

Error SdCard::checkAttached() const {
        return isAttached() && isInserted() ? Error() : Error::NoMedia;
}

Error SdCard::sync() {
        // Writes are on the card when they return: sdmmc waits for the card
        // to finish programming.
        Mutex::Locker lock(_lock, WaitForever);
        return checkAttached();
}

Error SdCard::trim(uint64_t first, uint64_t count) {
        Mutex::Locker lock(_lock, WaitForever);
        Error         err = checkAttached();
        if (err.isError()) return err;
        const uint64_t total = static_cast<uint64_t>(card->csd.capacity);
        if (count == 0 || first >= total || count > total - first) return Error::OutOfRange;
        // Only discard, which is quick; a full erase can take seconds.
        if (sdmmc_can_discard(card) != ESP_OK) return Error::NotSupported;
        err = espError(
                sdmmc_erase_sectors(card, static_cast<size_t>(first), static_cast<size_t>(count), SDMMC_DISCARD_ARG));
        if (err.isError() && !isInserted()) return Error::NoMedia;
        return err;
}

Error SdCard::readSectors(uint64_t sector, WritableByteSpan buf) {
        return transfer(sector, buf.data(), buf.size(), false);
}

Error SdCard::writeSectors(uint64_t sector, ByteSpan data) {
        // sdmmc takes a const source; the shared path needs a mutable one.
        return transfer(sector, const_cast<uint8_t *>(data.data()), data.size(), true);
}

Error SdCard::transfer(uint64_t sector, uint8_t *buf, size_t bytes, bool write) {
        Mutex::Locker lock(_lock, WaitForever);
        Error         err = checkAttached();
        if (err.isError()) return err;
        const size_t sectorBytes = card->csd.sector_size;
        if (sector > SIZE_MAX) return Error::OutOfRange;
        const size_t first = static_cast<size_t>(sector);
        const size_t count = bytes / sectorBytes;

        esp_err_t espErr = ESP_OK;
        // Straight to DMA if the host can reach the buffer.
        // Window addresses (large blocks, programs' pages) aren't PSRAM to IDF, so its check asks
        // only internal RAM's alignment of them: the L2's line is asked here.
        const bool window = !esp_ptr_dma_capable(buf) && Memory::isExternal(buf, bytes) && !esp_ptr_external_ram(buf);
        const bool lineAligned = (reinterpret_cast<uintptr_t>(buf) | bytes) % Memory::DmaAlign == 0;
        if ((esp_ptr_dma_capable(buf) || Memory::isExternal(buf, bytes)) && (!window || lineAligned) &&
            card->host.check_buffer_alignment(card->host.slot, buf, bytes)) {
                espErr = write ? sdmmc_write_sectors(card, buf, first, count)
                               : sdmmc_read_sectors(card, buf, first, count);
        } else {
                const size_t chunkSectors = CONFIG_BMDS_SD_BOUNCE_BYTES / sectorBytes;
                for (size_t done = 0; done < count && espErr == ESP_OK;) {
                        const size_t n = count - done < chunkSectors ? count - done : chunkSectors;
                        uint8_t     *at = buf + done * sectorBytes;
                        if (write) {
                                std::memcpy(_bounce, at, n * sectorBytes);
                                espErr = sdmmc_write_sectors(card, _bounce, first + done, n);
                        } else {
                                espErr = sdmmc_read_sectors(card, _bounce, first + done, n);
                                if (espErr == ESP_OK) std::memcpy(at, _bounce, n * sectorBytes);
                        }
                        done += n;
                }
        }
        if (espErr == ESP_OK) return Error();
        if (!isInserted()) return Error::NoMedia;
        bmdsDebug("SdCard(%p): %s %u+%u failed: %s", this, write ? "write" : "read", static_cast<unsigned>(first),
                  static_cast<unsigned>(count), espError(espErr).desc());
        return espError(espErr);
}

BMDS_NAMESPACE_END
