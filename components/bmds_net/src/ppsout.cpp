/**
 * @file      ppsout.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The 1588 clock's PPS made by a GPTimer and the ETM (devplan/network.md §31b).
 */

#include <driver/gpio.h>
#include <driver/gpio_etm.h>
#include <driver/gptimer.h>
#include <driver/gptimer_etm.h>
#include <esp_etm.h>
#include <esp_private/gpio.h>
#include <hal/efuse_hal.h>
#include <hal/gpio_ll.h>
#include <hal/timer_ll.h>
#include <soc/emac_ptp_struct.h>
#include <soc/gpio_sig_map.h>
#include <soc/hp_system_struct.h>
#include <freertos/FreeRTOS.h>
#include <bmds/ethernetdevice.h>
#include <bmds/log.h>

#include "ppsout.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // 25 ns counts from PLL_F80M halved (a timer can't take its clock undivided): the PLL is a
        // whole multiple of the crystal the 1588 clock counts, so the rate between them stays exact.
        constexpr uint32_t TimerHz = 40'000'000;
        constexpr int64_t  NsPerCount = 25;
        constexpr int64_t  WidthNs = 100'000'000; // high for 100 ms
        constexpr int64_t  LeadNs = 2'000'000;    // an edge is armed at least this far ahead
        constexpr int64_t  NsPerSecond = 1'000'000'000;

        // Serializes arming between the interrupt and rearm().
        constinit portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

} // namespace

Error PpsOut::start(GpioNum pin, EthernetDevice &device) {
        if (pin == NoGpio || _timer != nullptr) return Error::InvalidState;
        _device = &device;
        _pin = pin;
        gpio_config_t io = {};
        io.pin_bit_mask = 1ull << pin;
        io.mode = GPIO_MODE_INPUT_OUTPUT; // read back by `net`
        esp_err_t err = gpio_config(&io);
        if (err == ESP_OK) err = gpio_set_level(static_cast<gpio_num_t>(pin), 0);
        if (err == ESP_OK && efuse_hal_chip_revision() >= 300) {
                // From revision 3 the MAC's PPS reaches the GPIO matrix (as ESP-IDF's
                // esp_eth_mac_set_pps_out_gpio()): a pulse one 1588 clock period wide as each second
                // turns over (digital rollover's 1 Hz setting falls on the second instead).
                // Untested: this board is revision 1.3.
                const auto p = static_cast<gpio_num_t>(pin);
                err = gpio_matrix_output(p, EMAC_PTP_PPS_PAD_OUT_IDX, false, false);
                gpio_ll_set_output_enable_ctrl(&GPIO, static_cast<uint8_t>(pin), false, false); // the GPIO's enable
                EMAC_PTP.pps_ctrl.val = 0;
                if (err != ESP_OK) return Error::fromEspErr(err);
                _hardware = true;
                return Error::Ok;
        }

        gptimer_handle_t timer = nullptr;
        gptimer_config_t config = {};
        config.clk_src = GPTIMER_CLK_SRC_PLL_F80M;
        config.direction = GPTIMER_COUNT_UP;
        config.resolution_hz = TimerHz;
        if (err == ESP_OK) err = gptimer_new_timer(&config, &timer);

        // The alarm's match toggles the pin: no software between the count and the edge.
        esp_etm_event_handle_t   event = nullptr;
        esp_etm_task_handle_t    task = nullptr;
        esp_etm_channel_handle_t channel = nullptr;
        if (err == ESP_OK) {
                gptimer_etm_event_config_t e = {};
                e.event_type = GPTIMER_ETM_EVENT_ALARM_MATCH;
                err = gptimer_new_etm_event(timer, &e, &event);
        }
        if (err == ESP_OK) {
                gpio_etm_task_config_t t = {};
                t.action = GPIO_ETM_TASK_ACTION_TOG;
                err = gpio_new_etm_task(&t, &task);
        }
        if (err == ESP_OK) err = gpio_etm_task_add_gpio(task, pin);
        if (err == ESP_OK) {
                esp_etm_channel_config_t c = {};
                err = esp_etm_new_channel(&c, &channel);
        }
        if (err == ESP_OK) err = esp_etm_channel_connect(channel, event, task);
        if (err == ESP_OK) err = esp_etm_channel_enable(channel);
        if (err == ESP_OK) {
                gptimer_event_callbacks_t callbacks = {};
                callbacks.on_alarm = [](gptimer_handle_t, const gptimer_alarm_event_data_t *, void *arg) {
                        return static_cast<PpsOut *>(arg)->onAlarm();
                };
                err = gptimer_register_event_callbacks(timer, &callbacks, this);
        }
        if (err == ESP_OK) err = gptimer_enable(timer);
        if (err == ESP_OK) {
                // Which of the timer groups' timers it is (the driver doesn't say): the one whose
                // alarm register holds a value set for the purpose.
                constexpr uint64_t     Marker = 0x0012'3456'789a'bcdeull;
                gptimer_alarm_config_t alarm = {};
                alarm.alarm_count = Marker;
                err = gptimer_set_alarm_action(timer, &alarm);
                for (int g = 0; g < 2 && err == ESP_OK && _hw == nullptr; ++g) {
                        timg_dev_t *hw = TIMER_LL_GET_HW(g);
                        for (uint32_t n = 0; n < sizeof(hw->hw_timer) / sizeof(hw->hw_timer[0]); ++n) {
                                const uint64_t a = (uint64_t(hw->hw_timer[n].alarmhi.tx_alarm_hi) << 32) |
                                                   hw->hw_timer[n].alarmlo.tx_alarm_lo;
                                if (a != Marker) continue;
                                _hw = hw;
                                _num = n;
                        }
                }
                if (err == ESP_OK && _hw == nullptr) err = ESP_ERR_NOT_FOUND;
        }
        if (err == ESP_OK) err = gptimer_start(timer);
        if (err != ESP_OK) {
                bmdsErr("PpsOut: GPIO%d: %s", static_cast<int>(pin), esp_err_to_name(err));
                return Error::fromEspErr(err); // what was made stays: a boot-time failure, once
        }
        _timer = timer;
        portENTER_CRITICAL(&lock);
        (void)arm(true);
        portEXIT_CRITICAL(&lock);
        return Error::Ok;
}

// Arms the next edge: a rising one at the 1588 clock's next second, or the falling one after it.
// Under `lock`.
bool PpsOut::arm(bool rising) {
        auto *timer = static_cast<gptimer_handle_t>(_timer);
        // Where the two clocks are: the timer's count latched between two reads of the 1588 clock,
        // read from HP_SYSTEM's copy of the MAC's time bus (seconds and nanoseconds, cheaper than the
        // MAC's own registers); the tightest of four. The latch is the register write; the wait for the timer's clock
        // domain comes after the second read, so it doesn't widen the bracket.
        auto    *hw = static_cast<timg_dev_t *>(_hw);
        uint64_t count = 0;
        int64_t  now = 0, bracket = INT64_MAX;
        for (int i = 0; i < 4 || bracket == INT64_MAX; ++i) {
                // Only the nanoseconds straddle the latch; the seconds are read around them.
                const uint32_t s0 = HP_SYSTEM.sys_gmac_ctrl2.val;
                const uint32_t n0 = HP_SYSTEM.sys_gmac_ctrl1.val;
                hw->hw_timer[_num].update.tx_update = 1;
                __asm__ volatile("fence iorw, iorw" ::: "memory");
                const uint32_t n1 = HP_SYSTEM.sys_gmac_ctrl1.val;
                const uint32_t s1 = HP_SYSTEM.sys_gmac_ctrl2.val;
                while (hw->hw_timer[_num].update.tx_update) {}
                if (s1 != s0) continue; // the second turned over meanwhile: another try
                const int64_t  m0 = static_cast<int64_t>(s0) * NsPerSecond + n0;
                const int64_t  m1 = static_cast<int64_t>(s0) * NsPerSecond + n1;
                const uint64_t c = timer_ll_get_counter_value(hw, _num);
                if (m1 - m0 >= bracket) continue;
                bracket = m1 - m0;
                now = m0 + bracket / 2;
                count = c;
        }
        _bracketNs.store(static_cast<uint32_t>(bracket), std::memory_order_relaxed);

        int64_t target;
        if (rising) {
                target = (now / NsPerSecond + 1) * NsPerSecond;
                if (target - now < LeadNs) target += NsPerSecond;
                _risingAtNs = target;
        } else {
                target = _risingAtNs + WidthNs;
                if (target - now < LeadNs) return false; // too late for this one
        }
        // The timer counts the crystal; the 1588 clock runs addend / nominal of it.
        const int64_t          ahead = target - now;
        const uint32_t         addend = _device->ptpAddend();
        const int64_t          crystalNs = ahead * static_cast<int64_t>(EthernetDevice::NominalPtpAddend) / addend;
        gptimer_alarm_config_t alarm = {};
        alarm.alarm_count = count + static_cast<uint64_t>(crystalNs / NsPerCount);
        (void)gptimer_set_alarm_action(timer, &alarm);
        _high = !rising; // what the pin is until this edge
        return true;
}

bool PpsOut::onAlarm() {
        portENTER_CRITICAL_ISR(&lock);
        const bool wasRising = !_high;
        if (wasRising) _edges.fetch_add(1, std::memory_order_relaxed);
        // The pin should now be the other level; if a toggle went astray, put it right first.
        const int want = wasRising ? 1 : 0;
        if (gpio_get_level(static_cast<gpio_num_t>(_pin)) != want)
                (void)gpio_set_level(static_cast<gpio_num_t>(_pin), want);
        if (!arm(!wasRising)) {
                // The falling edge's time passed (a long interrupt delay): low now, the next second.
                (void)gpio_set_level(static_cast<gpio_num_t>(_pin), 0);
                _late.fetch_add(1, std::memory_order_relaxed);
                (void)arm(true);
        }
        portEXIT_CRITICAL_ISR(&lock);
        return false;
}

void PpsOut::rearm() {
        if (_timer == nullptr) return;
        portENTER_CRITICAL(&lock);
        // Low, and the next second's rising edge from the clock as it is now.
        (void)gpio_set_level(static_cast<gpio_num_t>(_pin), 0);
        (void)arm(true);
        portEXIT_CRITICAL(&lock);
        return;
}

PpsOut::Status PpsOut::status() const {
        Status s;
        s.pin = _pin;
        s.running = _timer != nullptr || _hardware;
        s.hardware = _hardware;
        s.edges = _edges.load(std::memory_order_relaxed);
        s.bracketNs = _bracketNs.load(std::memory_order_relaxed);
        s.late = _late.load(std::memory_order_relaxed);
        return s;
}

BMDS_NAMESPACE_END
