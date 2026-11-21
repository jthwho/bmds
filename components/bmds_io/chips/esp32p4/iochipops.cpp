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
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
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

        namespace {

                // The LP IO mux's registers are readable and writable while their APB clock runs
                // (readPad()'s note).
                bool lpReachable(unsigned pad) {
                        return pad < SOC_RTCIO_PIN_COUNT && HP_SYS_CLKRST.soc_clk_ctrl3.reg_iomux_apb_clk_en != 0;
                }

                // A USB PHY's pad enable for pads 24-27 (gpio_ll_func_sel() turns it off).
                int8_t usbPadEnabled(unsigned pad) {
                        if (pad == USB_USJ_INT_PHY_DM_GPIO_NUM || pad == USB_USJ_INT_PHY_DP_GPIO_NUM)
                                return int8_t(USB_SERIAL_JTAG.conf0.usb_pad_enable);
                        if (pad == USB_OTG_INT_PHY_DM_GPIO_NUM || pad == USB_OTG_INT_PHY_DP_GPIO_NUM)
                                return int8_t(USB_WRAP.otg_conf.usb_pad_enable);
                        return -1;
                }

                void setUsbPadEnabled(unsigned pad, bool on) {
                        if (pad == USB_USJ_INT_PHY_DM_GPIO_NUM || pad == USB_USJ_INT_PHY_DP_GPIO_NUM)
                                USB_SERIAL_JTAG.conf0.usb_pad_enable = on;
                        else if (pad == USB_OTG_INT_PHY_DM_GPIO_NUM || pad == USB_OTG_INT_PHY_DP_GPIO_NUM)
                                USB_WRAP.otg_conf.usb_pad_enable = on;
                        return;
                }

        } // namespace

        void savePad(unsigned pad, Saved &out) {
                out.ioMux = IO_MUX.gpio[pad].val;
                out.outSel = GPIO.func_out_sel_cfg[pad].val;
                out.pin = GPIO.pin[pad].val;
                const uint32_t bit = 1u << (pad % 32);
                out.enabled = ((pad < 32 ? GPIO.enable.val : GPIO.enable1.val) & bit) != 0;
                out.latch = ((pad < 32 ? GPIO.out.val : GPIO.out1.val) & bit) != 0;
                out.lpIo = lpReachable(pad) && LP_IOMUX.pad[pad].mux_sel != 0;
                out.usbPad = usbPadEnabled(pad);
                return;
        }

        void restorePad(unsigned pad, const Saved &saved) {
                // The latch before the driver, the driver before the signal, the domain last: the
                // pad never drives what it didn't.
                gpio_ll_set_level(&GPIO, pad, saved.latch);
                GPIO.pin[pad].val = saved.pin;
                IO_MUX.gpio[pad].val = saved.ioMux;
                if (saved.enabled)
                        gpio_ll_output_enable(&GPIO, pad);
                else
                        gpio_ll_output_disable(&GPIO, pad);
                GPIO.func_out_sel_cfg[pad].val = saved.outSel;
                if (saved.usbPad >= 0) setUsbPadEnabled(pad, saved.usbPad != 0);
                if (lpReachable(pad)) LP_IOMUX.pad[pad].mux_sel = saved.lpIo;
                // A peripheral still reading it through the matrix (a monitor: another owner's
                // receiver on a pad this one drove) keeps its input: the saved word may have it off.
                constexpr unsigned Signals = sizeof(GPIO.func_in_sel_cfg) / sizeof(GPIO.func_in_sel_cfg[0]);
                for (unsigned signal = 0; signal < Signals; ++signal)
                        if (gpio_ll_get_in_signal_connected_io(&GPIO, signal) == int(pad)) {
                                gpio_ll_input_enable(&GPIO, pad);
                                break;
                        }
                return;
        }

        void configurePad(unsigned pad, const IoPinSetup &setup) {
                // The HP domain's pad (an LP IO pad given back to it).
                if (lpReachable(pad)) LP_IOMUX.pad[pad].mux_sel = 0;
                const bool gpio = setup.function < 0;
                gpio_ll_func_sel(&GPIO, uint8_t(pad), gpio ? PIN_FUNC_GPIO : uint32_t(setup.function));
                if (setup.pullUp)
                        gpio_ll_pullup_en(&GPIO, pad);
                else
                        gpio_ll_pullup_dis(&GPIO, pad);
                if (setup.pullDown)
                        gpio_ll_pulldown_en(&GPIO, pad);
                else
                        gpio_ll_pulldown_dis(&GPIO, pad);
                gpio_ll_set_drive_capability(&GPIO, pad, gpio_drive_cap_t(setup.drive));
                // Input on for anything that may read it: an input, an open drain line (it reads
                // back), an input signal, an IO_MUX function (its peripheral decides).
                const bool reads =
                        setup.mode == IoPinSetup::Mode::Input || setup.mode == IoPinSetup::Mode::OpenDrain || !gpio;
                if ((reads && setup.mode != IoPinSetup::Mode::Off) || setup.inputCount > 0)
                        gpio_ll_input_enable(&GPIO, pad);
                else
                        gpio_ll_input_disable(&GPIO, pad);
                if (setup.mode == IoPinSetup::Mode::OpenDrain)
                        gpio_ll_od_enable(&GPIO, pad);
                else
                        gpio_ll_od_disable(&GPIO, pad);
                gpio_ll_set_level(&GPIO, pad, setup.high);
                // The direction, then the signal: the matrix output's enable from the GPIO bit.
                if (setup.isOutput() && gpio)
                        gpio_ll_output_enable(&GPIO, pad);
                else
                        gpio_ll_output_disable(&GPIO, pad);
                gpio_ll_set_output_enable_ctrl(&GPIO, uint8_t(pad), false, false);
                gpio_ll_set_output_signal_matrix_source(
                        &GPIO, pad, setup.outSignal != IoPinSetup::NoSignal ? setup.outSignal : SIG_GPIO_OUT_IDX,
                        false);
                return;
        }

        bool level(unsigned pad) {
                return gpio_ll_get_level(&GPIO, pad) != 0;
        }

        void setLevel(unsigned pad, bool high) {
                gpio_ll_set_level(&GPIO, pad, high);
                return;
        }

        uint32_t inputWord(unsigned signal) {
                return GPIO.func_in_sel_cfg[signal].val;
        }

        void setInputWord(unsigned signal, uint32_t word) {
                GPIO.func_in_sel_cfg[signal].val = word;
                return;
        }

        void routeInput(unsigned signal, unsigned pad) {
                gpio_ll_set_input_signal_matrix_source(&GPIO, signal, pad, false);
                return;
        }

        void enableInput(unsigned pad) {
                gpio_ll_input_enable(&GPIO, pad);
                return;
        }

} // namespace IoChipOps

BMDS_NAMESPACE_END
