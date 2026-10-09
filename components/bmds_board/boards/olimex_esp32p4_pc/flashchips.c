/**
 * @file      flashchips.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The flash chip drivers ESP-IDF probes (CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST), so the
 * board's W25Q128JV (ID 0xEF4018, docs/HARDWARE.md) may auto-suspend an erase or a write
 * (CONFIG_SPI_FLASH_AUTO_SUSPEND): the caches stay on while /boot is written, and the audio
 * engine keeps running (devplan/audio.md §17). IDF allows suspend only for its 64 Mbit sibling
 * (0xEF4017), which has the same suspend and resume commands (75h, 7Ah) and SUS bit (SR2 bit 7)
 * that IDF's Winbond driver sets up. This is IDF's Winbond driver with that one check widened:
 * its functions are IDF's (exported, though not in a public header), so recheck this list
 * against spi_flash_chip_winbond.c when IDF is updated. C, as IDF's own drivers are.
 */

#include <esp_attr.h>
#include <spi_flash_chip_driver.h>
#include <spi_flash_chip_generic.h>

// IDF's Winbond driver (components/spi_flash/spi_flash_chip_winbond.c).
esp_err_t spi_flash_chip_winbond_probe(esp_flash_t *chip, uint32_t flash_id);
esp_err_t spi_flash_chip_winbond_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t length);
esp_err_t spi_flash_chip_winbond_page_program(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t length);
esp_err_t spi_flash_chip_winbond_erase_sector(esp_flash_t *chip, uint32_t start_address);
esp_err_t spi_flash_chip_winbond_erase_block(esp_flash_t *chip, uint32_t start_address);
spi_flash_caps_t spi_flash_chip_winbond_get_caps(esp_flash_t *chip);
esp_err_t        spi_flash_chip_winbond_suspend_cmd_conf(esp_flash_t *chip);

#define BMDS_W25Q128JV 0xEF4018u

static IRAM_ATTR spi_flash_caps_t bmds_winbond_get_caps(esp_flash_t *chip) {
        spi_flash_caps_t caps = spi_flash_chip_winbond_get_caps(chip);
#if CONFIG_SPI_FLASH_AUTO_SUSPEND
        if (chip->chip_id == BMDS_W25Q128JV) caps |= SPI_FLASH_CHIP_CAP_SUSPEND;
#endif
        return caps;
}

static const DRAM_ATTR char bmds_winbond_name[] = "winbond";

static const DRAM_ATTR spi_flash_chip_t bmds_flash_chip_winbond = {
        .name = bmds_winbond_name,
        .timeout = &spi_flash_chip_generic_timeout,
        .probe = spi_flash_chip_winbond_probe,
        .reset = spi_flash_chip_generic_reset,
        .detect_size = spi_flash_chip_generic_detect_size,
        .erase_chip = spi_flash_chip_generic_erase_chip,
        .erase_sector = spi_flash_chip_winbond_erase_sector,
        .erase_block = spi_flash_chip_winbond_erase_block,
        .sector_size = 4 * 1024,
        .block_erase_size = 64 * 1024,

        .get_chip_write_protect = spi_flash_chip_generic_get_write_protect,
        .set_chip_write_protect = spi_flash_chip_generic_set_write_protect,

        .num_protectable_regions = 0,
        .protectable_regions = NULL,
        .get_protected_regions = NULL,
        .set_protected_regions = NULL,

        .read = spi_flash_chip_winbond_read,
        .write = spi_flash_chip_generic_write,
        .program_page = spi_flash_chip_winbond_page_program,
        .page_size = 256,
        .write_encrypted = spi_flash_chip_generic_write_encrypted,

        .wait_idle = spi_flash_chip_generic_wait_idle,
        .set_io_mode = spi_flash_chip_generic_set_io_mode,
        .get_io_mode = spi_flash_chip_generic_get_io_mode,

        .read_reg = spi_flash_chip_generic_read_reg,
        .yield = spi_flash_chip_generic_yield,
        .sus_setup = spi_flash_chip_winbond_suspend_cmd_conf,
        .read_unique_id = spi_flash_chip_generic_read_unique_id,
        .get_chip_caps = bmds_winbond_get_caps,
        .config_host_io_mode = spi_flash_chip_generic_config_host_io_mode,
};

// Probed in order: the board's chip, then IDF's generic driver for any other.
const spi_flash_chip_t *default_registered_chips[] = {
        &bmds_flash_chip_winbond,
        &esp_flash_chip_generic,
        NULL,
};
