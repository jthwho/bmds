/**
 * @file      i2cread.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * i2cread: a device's registers through Linux's i2c-dev only (linux/i2c-dev.h, the SDK's ioctl()
 * on i2c_ctl): I2C_SLAVE, then one register by SMBus (I2C_SMBUS read byte data), or COUNT from
 * it as one write-then-read (I2C_RDWR). -f is I2C_SLAVE_FORCE, for an address a BMDS driver owns.
 *
 *   i2cread [-f] BUS ADDR REG [COUNT]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/i2c-dev.h>

int main(int argc, char **argv) {
        int force = argc > 1 && strcmp(argv[1], "-f") == 0;
        if (argc - force < 4 || argc - force > 5) {
                fprintf(stderr, "usage: i2cread [-f] BUS ADDR REG [COUNT]\n");
                return 2;
        }
        const char   *bus = argv[1 + force];
        const long    addr = strtol(argv[2 + force], NULL, 0);
        const long    reg = strtol(argv[3 + force], NULL, 0);
        const long    count = argc - force == 5 ? strtol(argv[4 + force], NULL, 0) : 1;
        char          path[32];
        unsigned long funcs = 0;
        snprintf(path, sizeof(path), "/dev/i2c-%s", bus);
        const int fd = open(path, O_RDWR);
        if (fd < 0) {
                fprintf(stderr, "i2cread: %s: %s\n", path, strerror(errno));
                return 1;
        }
        if (ioctl(fd, I2C_FUNCS, &funcs) < 0 || ioctl(fd, force ? I2C_SLAVE_FORCE : I2C_SLAVE, addr) < 0) {
                fprintf(stderr, "i2cread: 0x%02lx: %s\n", addr, strerror(errno));
                close(fd);
                return 1;
        }
        if (count == 1 && (funcs & I2C_FUNC_SMBUS_READ_BYTE_DATA)) {
                union i2c_smbus_data        data;
                struct i2c_smbus_ioctl_data d = {.read_write = I2C_SMBUS_READ,
                                                 .command = (uint8_t)reg,
                                                 .size = I2C_SMBUS_BYTE_DATA,
                                                 .data = &data};
                if (ioctl(fd, I2C_SMBUS, &d) < 0) {
                        fprintf(stderr, "i2cread: 0x%02lx: %s\n", addr, strerror(errno));
                        close(fd);
                        return 1;
                }
                printf("0x%02x\n", data.byte);
                close(fd);
                return 0;
        }
        if (count < 1 || count > 256) {
                fprintf(stderr, "i2cread: COUNT is 1 to 256\n");
                close(fd);
                return 2;
        }
        uint8_t        r = (uint8_t)reg, bytes[256];
        struct i2c_msg msgs[2] = {{(uint16_t)addr, 0, 1, &r}, {(uint16_t)addr, I2C_M_RD, (uint16_t)count, bytes}};
        struct i2c_rdwr_ioctl_data rdwr = {msgs, 2};
        if (ioctl(fd, I2C_RDWR, &rdwr) < 0) {
                fprintf(stderr, "i2cread: 0x%02lx: %s\n", addr, strerror(errno));
                close(fd);
                return 1;
        }
        for (long i = 0; i < count; i++) printf("%02x%s", bytes[i], (i + 1) % 16 == 0 || i + 1 == count ? "\n" : " ");
        close(fd);
        return 0;
}
