/**
 * @file      iochipops.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ESP32-P4's pad registers for the I/O view (devplan/io.md §3.1): IO_MUX,
 * the GPIO matrix and the LP IO mux, read through ESP-IDF's low-level layer.
 */

#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#include <soc/hp_sys_clkrst_struct.h>
#include <soc/lp_iomux_struct.h>
#include <soc/soc_caps.h>

#include "iochipops.h"

BMDS_NAMESPACE_BEGIN

namespace IoChipOps {

        void readPad(unsigned pad, IoPadState &out) {
                gpio_io_config_t c = {};
                gpio_ll_get_io_config(&GPIO, pad, &c);
                out.function = static_cast<uint8_t>(c.fun_sel);
                out.drive = static_cast<uint8_t>(c.drv);
                out.outputSignal = static_cast<uint16_t>(c.sig_out);
                out.inputEnabled = c.ie;
                out.outputEnabled = c.oe;
                out.outputByPeripheral = c.oe_ctrl_by_periph;
                out.openDrain = c.od;
                out.pullUp = c.pu;
                out.pullDown = c.pd;
                out.level = gpio_ll_get_level(&GPIO, pad) != 0;
                const uint32_t latch = pad < 32 ? GPIO.out.val : GPIO.out1.val;
                out.outputLevel = ((latch >> (pad % 32)) & 1u) != 0;
                // The LP IO mux's registers read 0 while their APB clock is gated. ESP-IDF leaves it on
                // from reset and never gates it (rtc_io_ll.h's note); checked anyway, so a gated
                // clock can't pass for "no pad in the LP domain" without saying so in the code.
                const bool lpReadable = HP_SYS_CLKRST.soc_clk_ctrl3.reg_iomux_apb_clk_en != 0;
                out.lpIo = lpReadable && pad < SOC_RTCIO_PIN_COUNT && LP_IOMUX.pad[pad].mux_sel != 0;
                return;
        }

        int inputSource(unsigned signal) {
                return gpio_ll_get_in_signal_connected_io(&GPIO, signal);
        }

} // namespace IoChipOps

BMDS_NAMESPACE_END
