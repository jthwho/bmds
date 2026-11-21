/**
 * @file      spi.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's SPI mode bits as BMDS provides them (devplan/io.md §11.5), for `linux/spi/spidev.h`.
 * Written for BMDS; the numbers are Linux's (`include/uapi/linux/spi/spi.h`).
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_SPI_SPI_H
#define BMDS_LINUX_SPI_SPI_H

#define SPI_CPHA 0x00000001
#define SPI_CPOL 0x00000002

#define SPI_MODE_0 (0 | 0)
#define SPI_MODE_1 (0 | SPI_CPHA)
#define SPI_MODE_2 (SPI_CPOL | 0)
#define SPI_MODE_3 (SPI_CPOL | SPI_CPHA)
#define SPI_MODE_X_MASK (SPI_CPOL | SPI_CPHA)

#define SPI_CS_HIGH 0x00000004
#define SPI_LSB_FIRST 0x00000008
#define SPI_3WIRE 0x00000010
#define SPI_LOOP 0x00000020
#define SPI_NO_CS 0x00000040
#define SPI_READY 0x00000080
#define SPI_TX_DUAL 0x00000100
#define SPI_TX_QUAD 0x00000200
#define SPI_RX_DUAL 0x00000400
#define SPI_RX_QUAD 0x00000800
#define SPI_CS_WORD 0x00001000
#define SPI_TX_OCTAL 0x00002000
#define SPI_RX_OCTAL 0x00004000
#define SPI_3WIRE_HIZ 0x00008000
#define SPI_RX_CPHA_FLIP 0x00010000
#define SPI_MOSI_IDLE_LOW 0x00020000
#define SPI_MOSI_IDLE_HIGH 0x00040000

#endif /* BMDS_LINUX_SPI_SPI_H */
