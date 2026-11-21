/**
 * @file      i2ccalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `i2c_ctl` (slot 140, devplan/io.md §11.3): `/dev/i2c-N`'s operations. Linux's i2c-dev is
 * translated onto it by the SDK's `linux.c`.
 */

#include <cstddef>
#include <bmds/errno.h>
#include <bmds/i2cbus.h>
#include <bmds/i2cclient.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::I2cBus;
using bmds::I2cClient;
using bmds::I2cMessage;
using bmds::KernelCall;
using bmds::Span;
using bmds::detail::canRead;
using bmds::detail::canWrite;
using bmds::detail::copyIn;
using bmds::detail::copyOut;

namespace {

        int32_t errnoOf(bmds::Error err) {
                switch (err.code()) {
                        case bmds::Error::Ok: return 0;
                        case bmds::Error::NotFound: return -BMDS_ENXIO;
                        case bmds::Error::Timeout: return -BMDS_ETIMEDOUT;
                        case bmds::Error::Busy: return -BMDS_EBUSY;
                        case bmds::Error::NoMem: return -BMDS_ENOMEM;
                        case bmds::Error::Invalid: return -BMDS_EINVAL;
                        default: return -BMDS_EIO;
                }
        }

        // The commands a message takes on the bus (I2cBus::transfer()'s count).
        size_t commandsOf(const bmds_i2c_msg_t &m) {
                if (m.length == 0) return 2;
                return (m.flags & BMDS_I2C_MSG_READ) != 0 && m.length > 1 ? 4 : 3;
        }

        int32_t info(I2cClient &c, bmds_i2c_info_t *arg) {
                bmds_i2c_info_t i;
                if (const int32_t r = copyIn(arg, i); r < 0) return r;
                i.bus = uint32_t(c.bus().number());
                i.clock_hz = c.bus().clockHz();
                i.target = c.target();
                i.flags = c.isForced() ? BMDS_I2C_FORCE : 0;
                i.max_commands = uint32_t(I2cBus::MaxCommands);
                return copyOut(i, arg);
        }

        int32_t setTarget(I2cClient &c, bmds_i2c_target_t *arg) {
                bmds_i2c_target_t t;
                if (const int32_t r = copyIn(arg, t); r < 0) return r;
                if (t.address > 0x7f || (t.flags & ~BMDS_I2C_FORCE) != 0) return -BMDS_EINVAL;
                return errnoOf(c.setTarget(uint8_t(t.address), (t.flags & BMDS_I2C_FORCE) != 0));
        }

        int32_t transfer(I2cClient &c, bmds_i2c_transfer_t *arg) {
                bmds_i2c_transfer_t t;
                if (const int32_t r = copyIn(arg, t); r < 0) return r;
                if (t.count == 0 || t.count > BMDS_I2C_MAX_MESSAGES) return -BMDS_EINVAL;
                if (!canRead(t.messages, size_t(t.count) * sizeof(bmds_i2c_msg_t))) return -BMDS_EFAULT;
                // Every message costs two commands at least, and one stop: few fit (a write then a read).
                constexpr size_t Most = I2cBus::MaxCommands / 2;
                if (t.count > Most) return -BMDS_ENOTSUP;
                I2cMessage messages[Most];
                size_t     commands = 1; // the stop
                for (uint32_t i = 0; i < t.count; ++i) {
                        const bmds_i2c_msg_t m = t.messages[i]; // read once
                        if (m.address > 0x7f || (m.flags & ~BMDS_I2C_MSG_READ) != 0) return -BMDS_EINVAL;
                        const bool read = (m.flags & BMDS_I2C_MSG_READ) != 0;
                        if (m.length > 0 && (read ? !canWrite(m.data, m.length) : !canRead(m.data, m.length)))
                                return -BMDS_EFAULT;
                        if (!c.mayReach(uint8_t(m.address))) return -BMDS_EBUSY;
                        commands += commandsOf(m);
                        messages[i] = I2cMessage{m.address, read, m.data, m.length};
                }
                if (commands > I2cBus::MaxCommands) return -BMDS_ENOTSUP;
                const bmds::Error err =
                        c.bus().transfer(Span<const I2cMessage>(messages, t.count), I2cClient::TimeoutMs);
                return err.isOk() ? int32_t(t.count) : errnoOf(err);
        }

} // namespace

extern "C" int32_t bmds_k_i2c_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        I2cClient *client = handle->stream().i2cClient();
        if (client == nullptr) return -BMDS_ENOTTY;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_I2C_INFO: return info(*client, static_cast<bmds_i2c_info_t *>(arg));
                case BMDS_I2C_SET_TARGET: return setTarget(*client, static_cast<bmds_i2c_target_t *>(arg));
                case BMDS_I2C_TRANSFER: return transfer(*client, static_cast<bmds_i2c_transfer_t *>(arg));
                default: return -BMDS_EINVAL;
        }
}
