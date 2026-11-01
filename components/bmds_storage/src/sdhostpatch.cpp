/**
 * @file      sdhostpatch.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * ESP-IDF's SD host made cheaper per command (sdhostpatch.h, devplan/storage.md §17). Every
 * function wrapped here is called from another file of ESP-IDF (sdmmc_cmd.c, sd_trans_sdmmc.c),
 * so `--wrap` reaches those calls; the slot's fields are ESP-IDF v6.1's (sd_host_private.h).
 */

#include <cstdint>
#include <driver/gpio.h>
#include <esp_private/sd_host_private.h>
#include <hal/sdmmc_ll.h>
#include <soc/sdmmc_struct.h>
extern "C" {
#include <esp_private/sdmmc_common.h>
}
#include <bmds/clock.h>
#include <bmds/semaphore.h>
#include <bmds/staticobject.h>
#include <bmds/waketimer.h>

#include "sdhostpatch.h"

extern "C" {
esp_err_t __real_sd_host_slot_set_card_clk(sd_host_sdmmc_slot_t *slot);
esp_err_t __real_sd_host_slot_get_real_freq(sd_host_sdmmc_slot_t *slot, int *real_freq_khz);
esp_err_t __real_sd_host_slot_set_bus_width(sd_host_sdmmc_slot_t *slot);
esp_err_t __real_sd_host_slot_set_bus_sampling_mode(sd_host_sdmmc_slot_t *slot);
esp_err_t __real_sd_host_set_delay_phase(sd_host_sdmmc_slot_t *slot);
esp_err_t __real_sd_host_set_delay_line(sd_host_sdmmc_slot_t *slot);
esp_err_t __wrap_sd_host_slot_set_card_clk(sd_host_sdmmc_slot_t *slot);
esp_err_t __wrap_sd_host_slot_get_real_freq(sd_host_sdmmc_slot_t *slot, int *real_freq_khz);
esp_err_t __wrap_sd_host_slot_set_bus_width(sd_host_sdmmc_slot_t *slot);
esp_err_t __wrap_sd_host_slot_set_bus_sampling_mode(sd_host_sdmmc_slot_t *slot);
esp_err_t __wrap_sd_host_set_delay_phase(sd_host_sdmmc_slot_t *slot);
esp_err_t __wrap_sd_host_set_delay_line(sd_host_sdmmc_slot_t *slot);
esp_err_t __wrap_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status);
}

BMDS_NAMESPACE_BEGIN

namespace {

        // What was last applied to the controller, and for which slot (null: nothing known).
        struct Applied {
                        const sd_host_sdmmc_slot_t *slot;
                        int                         value;
                        int                         state;
                        int                         freqHz;
        };

        Applied width{};
        Applied sampling{};
        Applied phase{};
        Applied line{};
        // The clock read back (value, kHz) for a slot's frequency and source (state).
        Applied realFreq{};

        bool matches(const Applied &a, const sd_host_sdmmc_slot_t *slot, int value, int state, int freqHz) {
                return a.slot == slot && a.value == value && a.state == state && a.freqHz == freqHz;
        }

        // Calls ESP-IDF's setter when the slot's setting isn't the one applied, and remembers it.
        template <typename Set>
        esp_err_t applyOnce(Applied &a, sd_host_sdmmc_slot_t *slot, int value, int state, int freqHz, Set set) {
                if (slot != nullptr && matches(a, slot, value, state, freqHz)) return ESP_OK;
                const esp_err_t err = set(slot);
                a = err == ESP_OK ? Applied{slot, value, state, freqHz} : Applied{};
                return err;
        }

        // The longest a card may stay busy (ESP-IDF's limit). A write keeps this card busy 0.4 to
        // 1.6 ms (storage.md §17).
        constexpr int64_t ReadyTimeoutNs = static_cast<int64_t>(SDMMC_READY_FOR_DATA_TIMEOUT_US) * 1000;
        // How long a wait for D0's edge lasts before the line is looked at again (an edge lost).
        constexpr unsigned int SliceMs = 2;
        // A card not busy yet not ready is asked again after this.
        constexpr int64_t RetryNs = 50 * 1000;

        // D0's pad (its GPIO input reads the line while the SDMMC drives the pin), and the
        // semaphore its rising edge releases.
        gpio_num_t                        d0 = GPIO_NUM_NC;
        constinit StaticObject<Semaphore> released;

        void d0Isr(void *) {
                released->releaseFromIsr();
                return;
        }

        bool isCardBusy() {
                return sdmmc_ll_is_card_data_busy(&SDMMC);
        }

        // Sleeps until the card releases D0 or @p deadlineNs passes; false at the deadline.
        bool waitWhileBusy(int64_t deadlineNs) {
                if (!isCardBusy()) return true;
                if (d0 == GPIO_NUM_NC) {
                        while (isCardBusy()) {
                                if (Clock::monotonicNs() >= deadlineNs) return false;
                                (void)WakeTimer::sleepUntil(Clock::monotonicNs() + RetryNs, nullptr);
                        }
                        return true;
                }
                bool ok = true;
                (void)released->tryAcquire(); // an edge from a transfer, before this wait
                (void)gpio_intr_enable(d0);
                while (isCardBusy()) {
                        if (Clock::monotonicNs() >= deadlineNs) {
                                ok = false;
                                break;
                        }
                        (void)released->acquire(SliceMs);
                }
                (void)gpio_intr_disable(d0);
                return ok;
        }

} // namespace

Error SdHostPatch::init(int d0Gpio) {
        if (d0Gpio < 0) return Error();
        released.create(1u, 0u);
        const gpio_num_t pin = static_cast<gpio_num_t>(d0Gpio);
        // Not gpio_config(): that would take the pad from the SDMMC. The ISR service is shared
        // (the card detect's): already there is fine.
        esp_err_t err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE) err = ESP_OK;
        if (err == ESP_OK) err = gpio_set_intr_type(pin, GPIO_INTR_POSEDGE);
        if (err == ESP_OK) err = gpio_intr_disable(pin);
        if (err == ESP_OK) err = gpio_isr_handler_add(pin, d0Isr, nullptr);
        if (err != ESP_OK) return Error::fromEspErr(err);
        d0 = pin;
        return Error();
}

void SdHostPatch::forget() {
        width = sampling = phase = line = realFreq = Applied{};
        return;
}

BMDS_NAMESPACE_END

using namespace bmds;

esp_err_t __wrap_sd_host_slot_set_card_clk(sd_host_sdmmc_slot_t *slot) {
        realFreq = Applied{};
        // The delay phase is applied for a frequency; the clock's setting may change it.
        phase = Applied{};
        return __real_sd_host_slot_set_card_clk(slot);
}

esp_err_t __wrap_sd_host_slot_get_real_freq(sd_host_sdmmc_slot_t *slot, int *real_freq_khz) {
        if (slot != nullptr && real_freq_khz != nullptr &&
            matches(realFreq, slot, realFreq.value, static_cast<int>(slot->clk_src), slot->freq.freq_hz)) {
                *real_freq_khz = realFreq.value;
                return ESP_OK;
        }
        const esp_err_t err = __real_sd_host_slot_get_real_freq(slot, real_freq_khz);
        realFreq = err == ESP_OK ? Applied{slot, *real_freq_khz, static_cast<int>(slot->clk_src), slot->freq.freq_hz}
                                 : Applied{};
        return err;
}

esp_err_t __wrap_sd_host_slot_set_bus_width(sd_host_sdmmc_slot_t *slot) {
        if (slot == nullptr) return __real_sd_host_slot_set_bus_width(slot);
        return applyOnce(width, slot, slot->width.width, slot->width.width_state, 0, __real_sd_host_slot_set_bus_width);
}

esp_err_t __wrap_sd_host_slot_set_bus_sampling_mode(sd_host_sdmmc_slot_t *slot) {
        if (slot == nullptr) return __real_sd_host_slot_set_bus_sampling_mode(slot);
        return applyOnce(sampling, slot, slot->sampling_mode.mode, slot->sampling_mode.sampling_mode_state,
                         slot->width.width, __real_sd_host_slot_set_bus_sampling_mode);
}

esp_err_t __wrap_sd_host_set_delay_phase(sd_host_sdmmc_slot_t *slot) {
        if (slot == nullptr) return __real_sd_host_set_delay_phase(slot);
        return applyOnce(phase, slot, slot->delay_phase.delayphase, slot->delay_phase.delay_phase_state,
                         slot->freq.freq_hz, __real_sd_host_set_delay_phase);
}

esp_err_t __wrap_sd_host_set_delay_line(sd_host_sdmmc_slot_t *slot) {
        if (slot == nullptr) return __real_sd_host_set_delay_line(slot);
        return applyOnce(line, slot, slot->delay_line.delayline, slot->delay_line.delay_line_state, slot->freq.freq_hz,
                         __real_sd_host_set_delay_line);
}

esp_err_t __wrap_sdmmc_wait_for_idle(sdmmc_card_t *card, uint32_t status) {
        // While the card programs, it holds D0 low: wait for its release with the CPU free, then ask.
        const int64_t deadlineNs = Clock::monotonicNs() + ReadyTimeoutNs;
        while (!sdmmc_ready_for_data(status)) {
                if (!waitWhileBusy(deadlineNs)) return ESP_ERR_TIMEOUT;
                const esp_err_t err = sdmmc_send_cmd_send_status(card, &status);
                if (err != ESP_OK) return err;
                if (sdmmc_ready_for_data(status)) break;
                // Not busy on D0, yet not ready: don't ask again at once.
                if (Clock::monotonicNs() >= deadlineNs) return ESP_ERR_TIMEOUT;
                (void)WakeTimer::sleepUntil(Clock::monotonicNs() + RetryNs, nullptr);
        }
        return ESP_OK;
}
