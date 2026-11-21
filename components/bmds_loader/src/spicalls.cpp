/**
 * @file      spicalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `spi_ctl` (slot 141, devplan/io.md §11.5): `/dev/spidevB.C`'s operations. Linux's spidev is
 * translated onto it by the SDK's `linux.c`.
 */

#include <cstddef>
#include <bmds/errno.h>
#include <bmds/process.h>
#include <bmds/spidevice.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::KernelCall;
using bmds::Span;
using bmds::SpiDevice;
using bmds::detail::canRead;
using bmds::detail::canWrite;
using bmds::detail::copyIn;
using bmds::detail::copyOut;

static_assert(BMDS_SPI_CPHA == SpiDevice::Cpha && BMDS_SPI_CPOL == SpiDevice::Cpol &&
                      BMDS_SPI_CS_HIGH == SpiDevice::CsHigh && BMDS_SPI_LSB_FIRST == SpiDevice::LsbFirst &&
                      BMDS_SPI_3WIRE == SpiDevice::ThreeWire && BMDS_SPI_LOOP == SpiDevice::Loop &&
                      BMDS_SPI_NO_CS == SpiDevice::NoCs,
              "the mode bits are Linux's, in the kernel and the ABI");
static_assert(BMDS_SPI_MAX_TRANSFERS == SpiDevice::MaxTransfers && BMDS_SPI_MAX_BYTES == SpiDevice::MaxBytes,
              "the limits are the device's");

namespace {

        int32_t errnoOf(bmds::Error err) {
                switch (err.code()) {
                        case bmds::Error::Ok: return 0;
                        case bmds::Error::NotExist: return -BMDS_ENODEV;
                        case bmds::Error::OutOfRange: return -BMDS_EMSGSIZE;
                        case bmds::Error::Invalid: return -BMDS_EINVAL;
                        case bmds::Error::NoMem: return -BMDS_ENOMEM;
                        default: return -BMDS_EIO;
                }
        }

        int32_t info(SpiDevice &d, bmds_spi_info_t *arg) {
                bmds_spi_info_t i;
                if (const int32_t r = copyIn(arg, i); r < 0) return r;
                SpiDevice::Settings s;
                uint32_t            actual = 0;
                if (const bmds::Error err = d.settings(s, &actual); err.isError()) return errnoOf(err);
                i.bus = d.bus();
                i.cs = d.index();
                i.mode = s.mode;
                i.bits_per_word = s.bits;
                i.max_speed_hz = s.speedHz;
                i.actual_hz = actual;
                i.reserved = 0;
                return copyOut(i, arg);
        }

        int32_t setup(SpiDevice &d, bmds_spi_setup_t *arg) {
                bmds_spi_setup_t s;
                if (const int32_t r = copyIn(arg, s); r < 0) return r;
                if (s.bits_per_word > 0xff) return -BMDS_EINVAL;
                SpiDevice::Settings n;
                n.mode = s.mode;
                n.bits = uint8_t(s.bits_per_word);
                n.speedHz = s.max_speed_hz;
                return errnoOf(d.setSettings(n));
        }

        int32_t transfer(SpiDevice &d, bmds_spi_transfer_t *arg) {
                bmds_spi_transfer_t t;
                if (const int32_t r = copyIn(arg, t); r < 0) return r;
                if (t.count == 0 || t.count > BMDS_SPI_MAX_TRANSFERS) return -BMDS_EINVAL;
                if (!canRead(t.transfers, size_t(t.count) * sizeof(bmds_spi_xfer_t))) return -BMDS_EFAULT;
                SpiDevice::Transfer list[SpiDevice::MaxTransfers];
                for (uint32_t i = 0; i < t.count; ++i) {
                        const bmds_spi_xfer_t x = t.transfers[i]; // read once
                        // One line each way (dual and quad need their pins); no gap between words.
                        if (x.tx_nbits > 1 || x.rx_nbits > 1 || x.word_delay_usecs != 0) return -BMDS_EINVAL;
                        if (x.tx_buf > UINTPTR_MAX || x.rx_buf > UINTPTR_MAX) return -BMDS_EFAULT;
                        if (x.len > BMDS_SPI_MAX_BYTES) return -BMDS_EMSGSIZE;
                        auto *tx = reinterpret_cast<const uint8_t *>(uintptr_t(x.tx_buf));
                        auto *rx = reinterpret_cast<uint8_t *>(uintptr_t(x.rx_buf));
                        if (x.len > 0 &&
                            ((tx != nullptr && !canRead(tx, x.len)) || (rx != nullptr && !canWrite(rx, x.len))))
                                return -BMDS_EFAULT;
                        SpiDevice::Transfer &o = list[i];
                        o.tx = tx;
                        o.rx = rx;
                        o.length = x.len;
                        o.speedHz = x.speed_hz;
                        o.delayUs = x.delay_usecs;
                        o.bits = x.bits_per_word;
                        o.csChange = x.cs_change != 0;
                }
                auto [moved, err] = d.transfer(Span<const SpiDevice::Transfer>(list, t.count));
                return err.isOk() ? int32_t(moved) : errnoOf(err);
        }

} // namespace

extern "C" int32_t bmds_k_spi_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        SpiDevice *device = handle->stream().spiDevice();
        if (device == nullptr) return -BMDS_ENOTTY;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_SPI_INFO: return info(*device, static_cast<bmds_spi_info_t *>(arg));
                case BMDS_SPI_SETUP: return setup(*device, static_cast<bmds_spi_setup_t *>(arg));
                case BMDS_SPI_TRANSFER: return transfer(*device, static_cast<bmds_spi_transfer_t *>(arg));
                default: return -BMDS_EINVAL;
        }
}
