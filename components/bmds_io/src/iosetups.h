/**
 * @file      iosetups.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What io/gpio and the ports share (devplan/io.md §10, §11): who else holds a resource, and a
 * check's refusal in words. Private to bmds_io; target only.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/iochip.h>

BMDS_NAMESPACE_BEGIN

namespace IoSetups {

        /** @brief The peripheral instance a matrix signal belongs to (`UART1_RXD`: uart1), or `NoIoResource`. */
        IoResource instanceOf(const IoChip &chip, uint16_t signal, bool input);

        /**
         * @brief True if an owner other than @p self holds @p r, or the board wires it (a pad
         *        nobody holds: Fixed, System).
         * @blocking Waits for the registry's lock.
         */
        bool takenElsewhere(IoResource r, const char *self);

        /**
         * @brief Refuses a value's use of @p r (for @p signal, a matrix signal of its, or null) in
         *        words, through `SysConf::refuse()`: the holder named, and how to go on.
         * @return `Busy`.
         */
        Error refuseTaken(IoResource r, const char *signal);

        /** @brief The boot stage's ports (`Io::startSetups()` calls it). */
        Error startPorts();

        /** @brief The SPI ports (`io/spi2`, `io/spi3`: devplan/io.md §11.5); `startPorts()` calls it. */
        Error startSpiPorts();

        /** @brief The RMT ports (`io/rmt0` .. `io/rmt7`: devplan/io.md §13.1); `startPorts()` calls it. */
        Error startRmtPorts();

        /** @brief The PWM channels (`io/pwm0` .. `io/pwm7`, `/dev/pwmchip0`: devplan/io.md §15.1); `startPorts()` calls it. */
        Error startPwmPorts();

        /** @brief The pulse counters (`io/pcnt0` .. `io/pcnt3`, `/dev/counter0`: devplan/io.md §15.3); `startPorts()` calls it. */
        Error startPcntPorts();

        /** @brief The PARLIO ports (`io/parlio0`, `io/parlio1`: devplan/io.md §15.5); `startPorts()` calls it. */
        Error startParlioPorts();

} // namespace IoSetups

BMDS_NAMESPACE_END
