/**
 * @file      i2c-dev.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Linux's i2c-dev as BMDS provides it (devplan/io.md §11.3): `/dev/i2c-N`, the system's I²C
 * buses, with these requests, which the SDK's `ioctl()` carries out on `i2c_ctl` (`bmds/sys.h`).
 * Written for BMDS; the layouts and numbers are Linux's (`include/uapi/linux/i2c-dev.h`).
 *
 * Supported: `I2C_SLAVE`, `I2C_SLAVE_FORCE` (an address a BMDS driver owns is `EBUSY` without
 * it, for `I2C_RDWR` and `I2C_SMBUS` too), `I2C_FUNCS`, `I2C_RDWR` (7-bit addresses, `I2C_M_RD`
 * only; as many messages as the controller's eight commands hold: a write then a read),
 * `I2C_SMBUS` (quick, byte, byte data, word data, process call, I2C block, block write: block
 * read and block process call are `EOPNOTSUPP`), `I2C_TIMEOUT` and `I2C_RETRIES` (taken, the
 * bus keeps its own), `I2C_TENBIT` and `I2C_PEC` off only. `read()` and `write()` move bytes at
 * the target.
 *
 * ABI: frozen. Append only.
 */

#ifndef BMDS_LINUX_I2C_DEV_H
#define BMDS_LINUX_I2C_DEV_H

#include <stdint.h>
#include <linux/i2c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I2C_RETRIES 0x0701
#define I2C_TIMEOUT 0x0702
#define I2C_SLAVE 0x0703
#define I2C_TENBIT 0x0704
#define I2C_FUNCS 0x0705
#define I2C_SLAVE_FORCE 0x0706
#define I2C_RDWR 0x0707
#define I2C_PEC 0x0708
#define I2C_SMBUS 0x0720

/** @brief `I2C_SMBUS`'s argument. */
struct i2c_smbus_ioctl_data {
                uint8_t               read_write; /**< `I2C_SMBUS_READ` or `_WRITE`. */
                uint8_t               command;    /**< The command (register) byte. */
                uint32_t              size;       /**< `I2C_SMBUS_...` (the transaction's kind). */
                union i2c_smbus_data *data;
};

/** @brief `I2C_RDWR`'s argument. */
struct i2c_rdwr_ioctl_data {
                struct i2c_msg *msgs;
                uint32_t        nmsgs;
};

#define I2C_RDWR_IOCTL_MAX_MSGS 42

#ifdef __cplusplus
}
#endif

#endif /* BMDS_LINUX_I2C_DEV_H */
