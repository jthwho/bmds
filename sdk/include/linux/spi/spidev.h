/**
 * @file      spidev.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's spidev as BMDS provides it (devplan/io.md §11.5): `/dev/spidevB.C`, a device on an SPI
 * port (`io/spiB`), with these requests, which the SDK's `ioctl()` carries out on `spi_ctl`
 * (`bmds/sys.h`). Written for BMDS; the layouts and numbers are Linux's
 * (`include/uapi/linux/spi/spidev.h`).
 *
 * Supported: `SPI_IOC_RD_`/`WR_MODE`, `MODE32` (`CPHA`, `CPOL`, `CS_HIGH`, `LSB_FIRST` set;
 * `3WIRE`, `LOOP`, `NO_CS` are the port's), `LSB_FIRST`, `BITS_PER_WORD` (8, 16, 32),
 * `MAX_SPEED_HZ`, `SPI_IOC_MESSAGE(N)` (up to 64 transfers, 64 KB; `tx_nbits`, `rx_nbits` 1,
 * `word_delay_usecs` 0). `read()` and `write()` are half-duplex.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_SPI_SPIDEV_H
#define BMDS_LINUX_SPI_SPIDEV_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/spi/spi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_IOC_MAGIC 'k'

#ifndef _IOC_SIZEBITS
#define _IOC_SIZEBITS 14 /* Linux's (linux/ioctl.h): musl's sys/ioctl.h has the encoding, not this */
#endif

/** @brief One transfer of `SPI_IOC_MESSAGE(N)`. */
struct spi_ioc_transfer {
                uint64_t tx_buf; /**< The bytes sent (a pointer), or 0. */
                uint64_t rx_buf; /**< Where the bytes read go (a pointer), or 0. */
                uint32_t len;
                uint32_t speed_hz;
                uint16_t delay_usecs;
                uint8_t  bits_per_word;
                uint8_t  cs_change;
                uint8_t  tx_nbits;
                uint8_t  rx_nbits;
                uint8_t  word_delay_usecs;
                uint8_t  pad;
};

#define SPI_MSGSIZE(N)                                                                                                 \
        ((((N) * (sizeof(struct spi_ioc_transfer))) < (1 << _IOC_SIZEBITS))                                            \
                 ? ((N) * (sizeof(struct spi_ioc_transfer)))                                                           \
                 : 0)
#define SPI_IOC_MESSAGE(N) _IOW(SPI_IOC_MAGIC, 0, char[SPI_MSGSIZE(N)])

#define SPI_IOC_RD_MODE _IOR(SPI_IOC_MAGIC, 1, uint8_t)
#define SPI_IOC_WR_MODE _IOW(SPI_IOC_MAGIC, 1, uint8_t)
#define SPI_IOC_RD_LSB_FIRST _IOR(SPI_IOC_MAGIC, 2, uint8_t)
#define SPI_IOC_WR_LSB_FIRST _IOW(SPI_IOC_MAGIC, 2, uint8_t)
#define SPI_IOC_RD_BITS_PER_WORD _IOR(SPI_IOC_MAGIC, 3, uint8_t)
#define SPI_IOC_WR_BITS_PER_WORD _IOW(SPI_IOC_MAGIC, 3, uint8_t)
#define SPI_IOC_RD_MAX_SPEED_HZ _IOR(SPI_IOC_MAGIC, 4, uint32_t)
#define SPI_IOC_WR_MAX_SPEED_HZ _IOW(SPI_IOC_MAGIC, 4, uint32_t)
#define SPI_IOC_RD_MODE32 _IOR(SPI_IOC_MAGIC, 5, uint32_t)
#define SPI_IOC_WR_MODE32 _IOW(SPI_IOC_MAGIC, 5, uint32_t)

#ifdef __cplusplus
}
#endif

#endif /* BMDS_LINUX_SPI_SPIDEV_H */
