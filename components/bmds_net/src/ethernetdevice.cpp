/**
 * @file      ethernetdevice.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The Ethernet driver (devplan/network.md §4) on ESP-IDF's register layer
 * (`hal/emac_ll.h`), not its `esp_eth` driver: the rings, buffers and
 * timestamps are ours, so the receive path hands buffers over without a copy
 * and the real-time path (step 6) can be added beside it.
 *
 * The bring-up follows ESP-IDF's (esp_eth_mac_esp.c, emac_hal.c): the PHY out
 * of reset first (it supplies the 50 MHz reference clock the MAC's reset
 * needs), then the clocks and pins, the reset, the MAC and DMA settings.
 */

#include <cstring>
#include <driver/gpio.h>
#include <esp_cache.h>
#include <esp_intr_alloc.h>
#include <esp_mac.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <esp_private/esp_clk.h>
#include <esp_private/gpio.h>
#include <esp_private/periph_ctrl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal/emac_hal.h>
#include <hal/emac_ll.h>
#include <soc/emac_periph.h>
#include <soc/emac_reg.h>
#include <soc/interrupts.h>
#include <bmds/board.h>
#include <bmds/clock.h>
#include <bmds/deadline.h>
#include <bmds/ethernetdevice.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/placement.h>
#include <bmds/staticobject.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using RxDesc = eth_dma_rx_descriptor_t;
        using TxDesc = eth_dma_tx_descriptor_t;

        constexpr size_t DescBytes = EMAC_HAL_DMA_DESC_SIZE; // one cache line each
        constexpr size_t CacheLine = 128;                    // the largest line (L2)
        static_assert(sizeof(RxDesc) == DescBytes && sizeof(TxDesc) == DescBytes);
        static_assert(EthernetDevice::BufferBytes % CacheLine == 0);
        static_assert(EthernetDevice::RxBuffers <= 255, "slots are bytes");

        constexpr unsigned int LinkCheckMs = 1000; // no PHY interrupt line: the one timed wake-up
        constexpr unsigned int ResetTimeoutMs = 100;
        constexpr uint32_t     MdioTimeoutUs = 2000;
        constexpr uint32_t     PtpTimeoutUs = 100;
        constexpr size_t       FcsBytes = 4;

        // The 1588 clock: the crystal (25 ns) with digital rollover (the sub-second counter
        // counts nanoseconds), stepped 40 ns at a time by the fine method: the addend's carry
        // out at 25 MHz on average, 2^32 × 25 / 40 (ESP-IDF's choice).
        constexpr uint32_t PtpIncrementNs = 40;
        constexpr uint32_t PtpAddend = 0xa0000000u;
        constexpr int32_t  MaxPpb = 5'000'000;

        // TDES0's control bits (the alternate, "enhanced" descriptor).
        constexpr uint32_t TxOwn = 1u << 31;
        constexpr uint32_t TxInterrupt = 1u << 30;
        constexpr uint32_t TxLast = 1u << 29;
        constexpr uint32_t TxFirst = 1u << 28;
        constexpr uint32_t TxTimestampEnable = 1u << 25;
        constexpr uint32_t TxChained = 1u << 20;
        constexpr uint32_t TxTimestampStatus = 1u << 17;
        constexpr uint32_t TxErrorSummary = 1u << 15;

        // Without a reason to (a stamp, a ring half full), a frame asks for the sent interrupt only
        // this often, so the counters of sent frames trail by fewer when traffic stops.
        constexpr uint32_t TxInterruptEvery = 8;

        // DMA status bits the task acts on.
        constexpr uint32_t DmaInterrupts = EMAC_LL_INTR_TRANSMIT_ENABLE | EMAC_LL_INTR_RECEIVE_ENABLE |
                                           EMAC_LL_INTR_RECEIVE_BUFF_UNAVAILABLE_ENABLE | EMAC_LL_INTR_OVERFLOW_ENABLE |
                                           EMAC_LL_INTR_FATAL_BUS_ERR_ENABLE | EMAC_LL_INTR_ABNORMAL_SUMMARY_ENABLE |
                                           EMAC_LL_INTR_NORMAL_SUMMARY_ENABLE;

        // PHY registers (IEEE 802.3 clause 22).
        constexpr uint8_t  PhyControl = 0;
        constexpr uint8_t  PhyStatus = 1;
        constexpr uint8_t  PhyId1 = 2;
        constexpr uint8_t  PhyId2 = 3;
        constexpr uint8_t  PhyAdvertise = 4;
        constexpr uint8_t  PhyPartner = 5;
        constexpr uint16_t ControlAutoneg = 1u << 12;
        constexpr uint16_t ControlRestartAutoneg = 1u << 9;
        constexpr uint16_t StatusLink = 1u << 2;
        constexpr uint16_t StatusAutonegDone = 1u << 5;
        constexpr uint16_t Ability100Full = 1u << 8;
        constexpr uint16_t Ability100Half = 1u << 7;
        constexpr uint16_t Ability10Full = 1u << 6;

        constinit StaticObject<EthernetDevice> devices[EthernetDevice::MaxDevices];
        constinit size_t                       deviceCount = 0;

        Error espError(esp_err_t err) {
                switch (err) {
                        case ESP_OK: return Error::Ok;
                        case ESP_ERR_NO_MEM: return Error::NoMem;
                        case ESP_ERR_INVALID_ARG: return Error::Invalid;
                        case ESP_ERR_INVALID_STATE: return Error::InvalidState;
                        case ESP_ERR_NOT_FOUND: return Error::NotExist;
                        case ESP_ERR_TIMEOUT: return Error::Timeout;
                        default: return Error::LibraryFailure;
                }
        }

        // Cache maintenance on DMA memory, in whole lines.
        void writeBack(const void *p, size_t bytes, bool invalidate) {
                const auto   at = reinterpret_cast<uintptr_t>(p) & ~(CacheLine - 1);
                const size_t len = (reinterpret_cast<uintptr_t>(p) + bytes - at + CacheLine - 1) & ~(CacheLine - 1);
                int          flags = ESP_CACHE_MSYNC_FLAG_DIR_C2M;
                if (invalidate) flags |= ESP_CACHE_MSYNC_FLAG_INVALIDATE;
                (void)esp_cache_msync(reinterpret_cast<void *>(at), len, flags);
                return;
        }

        void invalidate(const void *p, size_t bytes) {
                const auto   at = reinterpret_cast<uintptr_t>(p) & ~(CacheLine - 1);
                const size_t len = (reinterpret_cast<uintptr_t>(p) + bytes - at + CacheLine - 1) & ~(CacheLine - 1);
                (void)esp_cache_msync(reinterpret_cast<void *>(at), len, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                return;
        }

        // A descriptor is one 64-byte line of its own (DescBytes), so it is synced alone.
        void writeBackDesc(const void *d) {
                (void)esp_cache_msync(const_cast<void *>(d), DescBytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                return;
        }

        void invalidateDesc(const void *d) {
                (void)esp_cache_msync(const_cast<void *>(d), DescBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                return;
        }

        // A pin through the IO MUX, from ESP-IDF's table of the pads that can carry the signal.
        Error iomuxPin(GpioNum gpio, const emac_iomux_info_t *pads, uint32_t signal, bool input) {
                for (const emac_iomux_info_t *p = pads; p != nullptr && p->gpio_num != GPIO_NUM_MAX; ++p) {
                        if (p->gpio_num != gpio) continue;
                        const auto pin = static_cast<gpio_num_t>(gpio);
                        esp_err_t  err =
                                input ? gpio_iomux_input(pin, p->func, signal) : gpio_iomux_output(pin, p->func);
                        if (err == ESP_OK) err = gpio_set_pull_mode(pin, GPIO_FLOATING);
                        return espError(err);
                }
                return Error::Invalid;
        }

        // MDC and MDIO through the GPIO matrix.
        void smiPins(GpioNum mdc, GpioNum mdio) {
                const auto c = static_cast<gpio_num_t>(mdc);
                const auto d = static_cast<gpio_num_t>(mdio);
                esp_rom_gpio_connect_out_signal(c, emac_io_idx.mdc_idx, false, false);
                (void)gpio_set_pull_mode(c, GPIO_FLOATING);
                (void)gpio_func_sel(c, PIN_FUNC_GPIO);
                (void)gpio_input_enable(d);
                esp_rom_gpio_connect_out_signal(d, emac_io_idx.mdo_idx, false, false);
                esp_rom_gpio_connect_in_signal(d, emac_io_idx.mdi_idx, false);
                (void)gpio_set_pull_mode(d, GPIO_FLOATING);
                (void)gpio_func_sel(d, PIN_FUNC_GPIO);
                return;
        }

        bool waitUs(bool (*done)(), uint32_t timeoutUs) {
                for (uint32_t t = 0; t < timeoutUs; ++t) {
                        if (done()) return true;
                        esp_rom_delay_us(1);
                }
                return done();
        }

} // namespace

struct EthernetDevice::Isr {
                BMDS_ISR_ATTR static void handle(void *arg) {
                        auto          &d = *static_cast<EthernetDevice *>(arg);
                        const uint32_t status = emac_ll_get_intr_status(&EMAC_DMA);
                        emac_ll_clear_corresponding_intr(&EMAC_DMA, status);
                        d._pendingStatus.fetch_or(status, std::memory_order_relaxed);
                        d._wake.releaseFromIsr();
                        return;
                }
};

Error EthernetDevice::init() {
        const BoardEthernet &e = board().ethernet;
        if (e.phy == BoardEthPhy::None) return Error::Ok;
        EthernetDevice &d = devices[0].create(static_cast<uint8_t>(0));
        deviceCount = 1;
        const Error err = d.start();
        if (err.isError()) bmdsErr("%s: not started: %s", d.name(), err.desc());
        return err;
}

Result<EthernetDevice *> EthernetDevice::device(size_t number) {
        if (number >= deviceCount) return makeError<EthernetDevice *>(Error::NotExist);
        return makeResult(&*devices[number]);
}

size_t EthernetDevice::count() {
        return deviceCount;
}

EthernetDevice::EthernetDevice(uint8_t number) : _number(number), _txSpace(1, 0), _wake(1, 0) {
        _name[0] = 'e';
        _name[1] = 't';
        _name[2] = 'h';
        _name[3] = static_cast<char>('0' + number);
        return;
}

Error EthernetDevice::start() {
        const BoardEthernet &e = board().ethernet;

        // The descriptors and buffers, in one block: descriptors first (a line each), then the
        // receive pool and the transmit buffers, all in whole lines.
        const size_t descBytes = (RxDescriptors + TxDescriptors) * DescBytes;
        const size_t total = descBytes + (RxBuffers + TxDescriptors) * BufferBytes;
        _memory = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, total, CacheLine));
        if (_memory == nullptr) return Error::NoMem;
        std::memset(_memory, 0, total);
        _rxDesc = _memory;
        _txDesc = _memory + RxDescriptors * DescBytes;
        _rxPool = _memory + descBytes;
        _txPool = _rxPool + RxBuffers * BufferBytes;
        writeBack(_memory, total, true);

        // The MAC's bus clock and register reset, the pins, the RMII clock from the PHY.
        Error err = Error::Ok;
        PERIPH_RCC_ATOMIC() {
                emac_ll_enable_bus_clock(0, true);
                emac_ll_reset_register(0);
        }
        smiPins(e.mdc, e.mdio);
        const emac_rmii_iomux_info_t &pads = emac_rmii_iomux_pins;
        if (err.isOk()) err = iomuxPin(e.txEn, pads.tx_en, emac_io_idx.mii_tx_en_o_idx, false);
        if (err.isOk()) err = iomuxPin(e.txd0, pads.txd0, emac_io_idx.mii_txd0_o_idx, false);
        if (err.isOk()) err = iomuxPin(e.txd1, pads.txd1, emac_io_idx.mii_txd1_o_idx, false);
        if (err.isOk()) err = iomuxPin(e.crsDv, pads.crs_dv, emac_io_idx.mii_crs_i_idx, true);
        if (err.isOk()) err = iomuxPin(e.rxd0, pads.rxd0, emac_io_idx.mii_rxd0_i_idx, true);
        if (err.isOk()) err = iomuxPin(e.rxd1, pads.rxd1, emac_io_idx.mii_rxd1_i_idx, true);
        if (err.isOk()) {
                if (!e.refClkIn) return Error::NotSupported; // the board's PHY supplies it
                err = iomuxPin(e.refClk, pads.clki, emac_io_idx.rmii_refclk_i_idx, true);
        }
        if (err.isError()) return err;
        PERIPH_RCC_ATOMIC() {
                emac_ll_clock_enable_rmii_input(nullptr);
                emac_ll_clock_rmii_rx_tx_div(nullptr, 1); // 100 Mbit/s until the link says otherwise
        }

        // The MAC's reset needs the PHY's reference clock. A PHY that is running (with its link
        // up, across a reboot of the ESP) is kept: resetting it takes the link down, and the switch
        // then forwarded nothing for 13-20 s after the link came back (2026-10-10; likely spanning
        // tree, docs/HARDWARE.md). Reset only when the MAC can't reset without it.
        err = startMac();
        if (err == Error::Timeout) {
                err = resetPhy();
                if (err.isOk()) {
                        _phyWasReset = true;
                        err = startMac();
                }
        }
        if (err.isError()) return err;
        readFeatures();
        probeFilters();
        startPtp();

        // The rings: chained descriptors, the receive ring given buffers from the pool.
        auto *rx = static_cast<RxDesc *>(_rxDesc);
        auto *tx = static_cast<TxDesc *>(_txDesc);
        for (size_t i = 0; i < RxBuffers; ++i) _rxFree[i] = static_cast<uint8_t>(RxBuffers - 1 - i);
        _rxFreeCount = RxBuffers;
        for (size_t i = 0; i < RxDescriptors; ++i) {
                rx[i].Buffer2NextDescAddr = reinterpret_cast<uint32_t>(&rx[(i + 1) % RxDescriptors]);
                writeBackDesc(&rx[i]);
        }
        _rxEmpty.store(RxDescriptors, std::memory_order_relaxed);
        refill();
        for (size_t i = 0; i < TxDescriptors; ++i) {
                tx[i].TDES0.Value = TxChained;
                tx[i].Buffer1Addr = reinterpret_cast<uint32_t>(_txPool + i * BufferBytes);
                tx[i].Buffer2NextDescAddr = reinterpret_cast<uint32_t>(&tx[(i + 1) % TxDescriptors]);
                writeBackDesc(&tx[i]);
        }
        emac_ll_set_rx_desc_addr(&EMAC_DMA, reinterpret_cast<uint32_t>(rx));
        emac_ll_set_tx_desc_addr(&EMAC_DMA, reinterpret_cast<uint32_t>(tx));

        intr_handle_t handle = nullptr;
        err = espError(esp_intr_alloc(ETS_ETH_MAC_INTR_SOURCE, ESP_INTR_FLAG_LOWMED, &Isr::handle, this, &handle));
        if (err.isError()) return err;
        _intr = handle;

        char mac[MacAddress::TextBytes + 1];
        _address.toString(mac);
        bmdsInfo("%s: %s, MAC %08lx, features %08lx (%u+%u DMA channels%s, %u group filters)", _name, mac,
                 static_cast<unsigned long>(_features.version), static_cast<unsigned long>(_features.raw),
                 _features.rxChannels, _features.txChannels, _features.avb ? ", AV" : "",
                 static_cast<unsigned>(_multicastFilters));
        Task::Config tc;
        tc.name = _name;
        tc.stackBytes = 4096;
        tc.priority = CONFIG_BMDS_NET_PRIORITY;
        err = _task.start(tc, &EthernetDevice::taskEntry, this);
        if (err.isError()) return err;

        // The DMA's interrupts. Traffic starts when the link comes up (applyLink()).
        emac_ll_enable_corresponding_intr(&EMAC_DMA, DmaInterrupts);
        emac_ll_clear_all_pending_intr(&EMAC_DMA);

        return Error::Ok;
}

Error EthernetDevice::resetPhy() {
        const GpioLine reset = board().ethernet.phyReset;
        if (!reset.isValid()) return Error::Ok;
        const auto    pin = static_cast<gpio_num_t>(reset.gpio);
        gpio_config_t io = {};
        io.pin_bit_mask = 1ull << reset.gpio;
        io.mode = GPIO_MODE_OUTPUT;
        (void)gpio_set_level(pin, reset.activeLow ? 0 : 1);
        const esp_err_t err = gpio_config(&io);
        if (err != ESP_OK) return espError(err);
        Task::sleep(10); // IP101GR: a reset pulse of at least 10 ms
        (void)gpio_set_level(pin, reset.activeLow ? 1 : 0);
        Task::sleep(10); // its clock and registers ready
        return Error::Ok;
}

Error EthernetDevice::startMac() {
        emac_ll_reset(&EMAC_DMA);
        const Deadline deadline(ResetTimeoutMs);
        while (!emac_ll_is_reset_done(&EMAC_DMA)) {
                if (deadline.isExpired()) return Error::Timeout; // no reference clock from the PHY?
                Task::sleep(1);
        }

        // The MDIO clock: the CSR clock divided into 1-2.5 MHz (ESP-IDF's table).
        const int hz = esp_clk_apb_freq();
        uint32_t  div = 5;
        if (hz < 35'000'000)
                div = 2;
        else if (hz < 60'000'000)
                div = 3;
        else if (hz < 100'000'000)
                div = 0;
        else if (hz < 150'000'000)
                div = 1;
        else if (hz < 250'000'000)
                div = 4;
        emac_ll_set_csr_clock_division(&EMAC_MAC, div);

        // The MAC (emac_hal_init_mac_default()).
        emac_mac_dev_t *mac = &EMAC_MAC;
        emac_ll_disable_corresponding_emac_intr(mac, 0xffffffffu);
        emac_ll_watchdog_enable(mac, true);
        emac_ll_jabber_enable(mac, true);
        emac_ll_set_inter_frame_gap(mac, EMAC_LL_INTERFRAME_GAP_96BIT);
        emac_ll_carrier_sense_enable(mac, true);
        emac_ll_set_port_speed(mac, ETH_SPEED_100M);
        emac_ll_recv_own_enable(mac, true);
        emac_ll_loopback_enable(mac, false);
        emac_ll_set_duplex(mac, ETH_DUPLEX_FULL);
        emac_ll_checksum_offload_mode(mac, ETH_CHECKSUM_HW);
        emac_ll_retry_enable(mac, true);
        emac_ll_auto_pad_crc_strip_enable(mac, false);
        emac_ll_set_back_off_limit(mac, EMAC_LL_BACKOFF_LIMIT_10);
        emac_ll_deferral_check_enable(mac, false);
        emac_ll_set_preamble_length(mac, EMAC_LL_PREAMBLE_LENGTH_7);
        emac_ll_receive_all_enable(mac, false);
        emac_ll_set_src_addr_filter(mac, EMAC_LL_SOURCE_ADDR_FILTER_DISABLE);
        emac_ll_sa_inverse_filter_enable(mac, false);
        emac_ll_set_pass_ctrl_frame_mode(mac, EMAC_LL_CONTROL_FRAME_BLOCKALL);
        emac_ll_broadcast_frame_enable(mac, true);
        emac_ll_da_inverse_filter_enable(mac, false);
        emac_ll_promiscuous_mode_enable(mac, false);

        // The DMA (emac_hal_init_dma_default()). The receive FIFO is only 256 bytes on the P4,
        // too small for store-and-forward: frames are passed through as they come, so a frame
        // that turns out bad still arrives, with its error in the descriptor.
        emac_dma_dev_t *dma = &EMAC_DMA;
        emac_ll_drop_tcp_err_frame_enable(dma, true);
        emac_ll_recv_store_forward_enable(dma, false);
        emac_ll_flush_recv_frame_enable(dma, true);
        emac_ll_trans_store_forward_enable(dma, false);
        emac_ll_flush_trans_fifo_enable(dma, true);
        if (!waitUs([] { return !emac_ll_get_flush_trans_fifo(&EMAC_DMA); }, 1000)) return Error::Timeout;
        emac_ll_set_transmit_threshold(dma, EMAC_LL_TRANSMIT_THRESHOLD_CONTROL_64);
        emac_ll_forward_err_frame_enable(dma, false);
        emac_ll_forward_undersized_good_frame_enable(dma, false);
        emac_ll_set_recv_threshold(dma, EMAC_LL_RECEIVE_THRESHOLD_CONTROL_64);
        emac_ll_opt_second_frame_enable(dma, true);
        emac_ll_mixed_burst_enable(dma, true);
        emac_ll_addr_align_enable(dma, true);
        emac_ll_use_separate_pbl_enable(dma, false);
        emac_ll_set_prog_burst_len(dma, ETH_DMA_BURST_LEN_32);
        emac_ll_enhance_desc_enable(dma, true); // 64-byte descriptors, with the timestamps
        emac_ll_set_desc_skip_len(dma, 0);
        emac_ll_fixed_arbitration_enable(dma, false);
        emac_ll_set_priority_ratio(dma, EMAC_LL_DMA_ARBITRATION_ROUNDROBIN_RXTX_1_1);

        uint8_t         addr[MacAddress::Bytes];
        const esp_err_t macErr = esp_read_mac(addr, ESP_MAC_ETH);
        if (macErr != ESP_OK) return espError(macErr);
        std::memcpy(_factoryAddress.bytes, addr, MacAddress::Bytes);
        if (_address.isZero()) _address = _factoryAddress; // a set address survives the PHY's reset
        emac_ll_set_addr(mac, _address.bytes);
        return Error::Ok;
}

void EthernetDevice::readFeatures() {
        const uint32_t f = REG_READ(EMAC_HWFEATURE_REG);
        _features.raw = f;
        _features.version = REG_READ(EMAC_VERSION_REG);
        _features.rxChannels = static_cast<uint8_t>(1 + ((f >> EMAC_RXCHCNT_S) & EMAC_RXCHCNT));
        _features.txChannels = static_cast<uint8_t>(1 + ((f >> EMAC_TXCHCNT_S) & EMAC_TXCHCNT));
        _features.avb = (f & EMAC_AVSEL) != 0;
        _features.timestampV2 = (f & EMAC_TSVER2SEL) != 0;
        _features.txChecksum = (f & EMAC_TXCOESEL) != 0;
        _features.rxChecksum = (f & EMAC_RXTYP2COE) != 0;
        _features.hashFilter = (f & EMAC_HASHSEL) != 0;
        _features.extraAddresses = (f & EMAC_ADDMACADRSEL) != 0;
        _features.l3l4Filter = (f & EMAC_L3L4FLTREN) != 0;
        _features.flexiblePps = (f & EMAC_FLEXIPPSEN) != 0;
        _features.mmc = (f & EMAC_MMCSEL) != 0;
        return;
}

void EthernetDevice::probeFilters() {
        // The register file has room for 15 filters beyond the station's; ESP-IDF's register layer
        // says 8. A filter that is there reads back what was written: written from the last down,
        // so a missing one that aliased a lower one reads that one's value. None are enabled.
        _multicastFilters = 0;
        if (!_features.extraAddresses) return;
        emac_mac_dev_t *mac = &EMAC_MAC;
        for (size_t i = MaxFilters; i-- > 0;) mac->emacaddr[i].emacaddrlow = 0x5a000000u | static_cast<uint32_t>(i);
        size_t n = 0;
        while (n < MaxFilters && mac->emacaddr[n].emacaddrlow == (0x5a000000u | n)) ++n;
        for (size_t i = 0; i < MaxFilters; ++i) mac->emacaddr[i].emacaddrlow = 0;
        _multicastFilters = n;
        return;
}

MacAddress EthernetDevice::address() const {
        SpinLock::Locker lock(_stateLock);
        return _address;
}

Error EthernetDevice::setAddress(const MacAddress &address) {
        if (address.isMulticast() || address.isZero()) return Error::Invalid;
        Mutex::Locker lock(_filterLock, WaitForever);
        {
                SpinLock::Locker state(_stateLock);
                _address = address;
        }
        emac_ll_set_addr(&EMAC_MAC, address.bytes); // the high half, then the low half that loads both
        return Error::Ok;
}

Error EthernetDevice::setMulticast(Span<const MacAddress> groups) {
        for (const MacAddress &g : groups)
                if (!g.isMulticast()) return Error::Invalid;
        Mutex::Locker   lock(_filterLock, WaitForever);
        emac_mac_dev_t *mac = &EMAC_MAC;
        const bool      all = groups.size() > _multicastFilters;
        for (size_t i = 0; i < _multicastFilters; ++i) {
                if (!all && i < groups.size())
                        emac_ll_add_addr_filter(mac, static_cast<uint8_t>(i + 1), groups[i].bytes, 0, false);
                else
                        emac_ll_rm_addr_filter(mac, static_cast<uint8_t>(i + 1));
        }
        emac_ll_pass_all_multicast_enable(mac, all);
        _allMulticast = all;
        return Error::Ok;
}

void EthernetDevice::startPtp() {
        PERIPH_RCC_ATOMIC() {
                emac_ll_clock_enable_ptp(nullptr, EMAC_PTP_CLK_SRC_XTAL, true);
        }
        emac_ptp_dev_t *ptp = &EMAC_PTP;
        emac_ll_ts_ptp_ether_enable(ptp, true);
        emac_ll_ts_ptp_ip4_enable(ptp, true);
        emac_ll_ptp_v2_proc_enable(ptp, true);
        emac_ll_ts_enable(ptp, true);
        emac_ll_ts_digital_roll_enable(ptp, true);
        emac_ll_ts_all_enable(ptp, true); // every received frame stamped, not only PTP's
        emac_ll_set_ts_sub_second_incre_val(ptp, PtpIncrementNs);
        emac_ll_set_ts_update_method(ptp, ETH_PTP_UPDATE_METHOD_FINE);
        emac_ll_set_ts_addend_val(ptp, PtpAddend);
        emac_ll_ts_addend_do_update(ptp);
        if (!waitUs([] { return emac_ll_is_ts_addend_update_done(&EMAC_PTP); }, PtpTimeoutUs))
                bmdsWarn("%s: the 1588 clock's addend didn't load", _name);
        // Started from the system clock if a source has set it (UTC; PTP's TAI offset is PTP's
        // business, step 7), else from 0.
        (void)setPtpTime(Clock::isSet() ? PtpTime::fromNs(Clock::now().ns) : PtpTime{});
        return;
}

void EthernetDevice::taskEntry(void *arg) {
        static_cast<EthernetDevice *>(arg)->run();
        return;
}

void EthernetDevice::run() {
        checkLink();
        uint64_t nextCheckUs = System::uptimeUs() + LinkCheckMs * 1000ull;
        for (;;) {
                const uint64_t now = System::uptimeUs();
                const auto waitMs = static_cast<unsigned int>(nextCheckUs > now ? (nextCheckUs - now + 999) / 1000 : 0);
                if (waitMs > 0) (void)_wake.acquire(waitMs);
                const uint32_t status = _pendingStatus.exchange(0, std::memory_order_relaxed);
                {
                        SpinLock::Locker lock(_stateLock);
                        if (status != 0) ++_counters.interrupts;
                        if (status & EMAC_LL_DMA_RECEIVE_BUFF_UNAVAILABLE_INTR) ++_counters.rxNoBuffer;
                        if (status & EMAC_LL_DMA_FATAL_BUS_ERROR_INRT) bmdsErr("%s: DMA bus error", _name);
                        const uint32_t missed = EMAC_DMA.dmamissedfr.val; // clears on read
                        _counters.rxMissed += missed & 0xffff;
                        _counters.rxOverflow += (missed >> 17) & 0x7ff;
                }
                receive();
                refill();
                reclaim();
                if (_phyRestart.exchange(false)) {
                        _phyWasReset = resetPhy().isOk();
                        _phyStarted = false;
                        checkLink(); // starts it
                        nextCheckUs = 0;
                }
                if (System::uptimeUs() >= nextCheckUs) {
                        checkLink();
                        nextCheckUs = System::uptimeUs() + LinkCheckMs * 1000ull;
                }
        }
}

void EthernetDevice::receive() {
        auto *rx = static_cast<RxDesc *>(_rxDesc);
        while (_rxEmpty.load(std::memory_order_relaxed) < RxDescriptors) {
                RxDesc &d = rx[_rxNext];
                invalidateDesc(&d);
                if (d.RDES0.Own == EMAC_LL_DMADESC_OWNER_DMA) break;
                const uint8_t slot = _rxSlot[_rxNext];
                const bool    whole = d.RDES0.FirstDescriptor && d.RDES0.LastDescriptor;
                const size_t  frameBytes = d.RDES0.FrameLength;
                RxFrame       f;
                f.buffer = slot;
                if (!whole || d.RDES0.ErrSummary || frameBytes <= FcsBytes || frameBytes > BufferBytes) {
                        SpinLock::Locker lock(_stateLock);
                        ++_counters.rxErrors;
                        f.length = 0;
                } else {
                        f.data = _rxPool + slot * BufferBytes;
                        f.length = static_cast<uint16_t>(frameBytes - FcsBytes);
                        invalidate(f.data, f.length);
                        if (d.RDES0.TSAvailIPChecksumErrGiantFrame) {
                                f.stamped = true;
                                f.stamp.seconds = d.TimeStampHigh;
                                f.stamp.nanoseconds = d.TimeStampLow;
                        }
                }
                _rxNext = (_rxNext + 1) % RxDescriptors;
                _rxEmpty.fetch_add(1, std::memory_order_relaxed);
                if (f.length == 0) {
                        release(f);
                        continue;
                }
                Receiver to;
                void    *arg;
                {
                        SpinLock::Locker lock(_stateLock);
                        to = _receiver;
                        arg = _receiverArg;
                        _inReceiver = to != nullptr; // setReceiver() waits for the call to end
                        ++_counters.rxFrames;
                        _counters.rxBytes += f.length;
                        ++_counters.rxHeld;
                        if (_counters.rxHeld > _counters.rxHeldMax) _counters.rxHeldMax = _counters.rxHeld;
                        if (to == nullptr) ++_counters.rxUnclaimed;
                }
                if (to != nullptr) {
                        to(arg, *this, f);
                        _inReceiver.store(false);
                } else {
                        release(f);
                }
        }
        return;
}

void EthernetDevice::release(const RxFrame &frame) {
        {
                SpinLock::Locker lock(_rxFreeLock);
                _rxFree[_rxFreeCount++] = frame.buffer;
        }
        if (frame.length != 0) {
                SpinLock::Locker lock(_stateLock);
                --_counters.rxHeld;
        }
        // A ring waiting for buffers is refilled by the task.
        if (_rxEmpty.load(std::memory_order_relaxed) > 0 && _task.handle() != xTaskGetCurrentTaskHandle())
                _wake.release();
        return;
}

void EthernetDevice::refill() {
        auto *rx = static_cast<RxDesc *>(_rxDesc);
        bool  gave = false;
        while (_rxEmpty.load(std::memory_order_relaxed) > 0) {
                uint8_t slot;
                {
                        SpinLock::Locker lock(_rxFreeLock);
                        if (_rxFreeCount == 0) break;
                        slot = _rxFree[--_rxFreeCount];
                }
                // No dirty line may cover a buffer the DMA writes (docs/platforms/esp32p4.md §3).
                uint8_t *buffer = _rxPool + slot * BufferBytes;
                writeBack(buffer, BufferBytes, true);
                RxDesc &d = rx[_rxRefill];
                _rxSlot[_rxRefill] = slot;
                d.Buffer1Addr = reinterpret_cast<uint32_t>(buffer);
                d.RDES1.Value = 0;
                d.RDES1.ReceiveBuffer1Size = BufferBytes;
                d.RDES1.SecondAddressChained = 1;
                d.ExtendedStatus.Value = 0;
                d.TimeStampLow = 0;
                d.TimeStampHigh = 0;
                d.RDES0.Value = 0;
                d.RDES0.Own = EMAC_LL_DMADESC_OWNER_DMA;
                writeBackDesc(&d);
                _rxRefill = (_rxRefill + 1) % RxDescriptors;
                _rxEmpty.fetch_sub(1, std::memory_order_relaxed);
                gave = true;
        }
        if (gave) emac_ll_receive_poll_demand(&EMAC_DMA, 0);
        return;
}

Result<uint32_t> EthernetDevice::transmit(Span<const uint8_t> frame, uint32_t flags, unsigned int timeoutMs) {
        const Span<const uint8_t> pieces[] = {frame};
        return transmit(Span<const Span<const uint8_t>>(pieces, 1), flags, timeoutMs);
}

Result<uint32_t> EthernetDevice::transmit(Span<const Span<const uint8_t>> pieces, uint32_t flags,
                                          unsigned int timeoutMs) {
        size_t bytes = 0;
        for (const Span<const uint8_t> &p : pieces) bytes += p.size();
        if (bytes < Ethernet::HeaderBytes || bytes > Ethernet::MaxFrameBytes)
                return makeError<uint32_t>(Error::Invalid);
        if ((flags & ~static_cast<uint32_t>(TxStamp)) != 0) return makeError<uint32_t>(Error::Invalid);
        if (!link().up) return makeError<uint32_t>(Error::NotReady);
        // The sender takes back the sent descriptors itself, so a frame asks for an interrupt (which
        // wakes the task to do it) only when a sender may come to wait for room or a stamp: an
        // interrupt and two semaphore operations a frame were most of the task's 72 µs a frame
        // (devplan/network.md §28).
        const Deadline deadline(timeoutMs);
        Mutex::Locker  lock(_txLock, WaitForever);
        if (reclaimLocked()) _txStampWaiters.notify();
        while (_txQueued == TxDescriptors) {
                {
                        SpinLock::Locker state(_stateLock);
                        ++_counters.txFull;
                }
                _txWaiting = true;
                _txLock.unlock();
                const Error err = _txSpace.acquire(deadline);
                (void)_txLock.lock(WaitForever);
                if (reclaimLocked()) _txStampWaiters.notify();
                if (err.isError() && _txQueued == TxDescriptors) {
                        _txWaiting = false;
                        return makeError<uint32_t>(err);
                }
        }
        _txWaiting = false;
        const size_t  i = _txHead;
        auto         &d = static_cast<TxDesc *>(_txDesc)[i];
        uint8_t      *buffer = _txPool + i * BufferBytes;
        size_t        at = 0;
        for (const Span<const uint8_t> &p : pieces) {
                std::memcpy(buffer + at, p.data(), p.size());
                at += p.size();
        }
        writeBack(buffer, bytes, false);
        const bool stamp = (flags & TxStamp) != 0;
        TxSlot    &s = _txSlots[i];
        s.ticket = ++_txTicket;
        s.length = static_cast<uint16_t>(bytes);
        s.wantsStamp = stamp;
        s.done = false;
        s.stamped = false;
        d.TDES1.Value = 0;
        d.TDES1.TransmitBuffer1Size = static_cast<uint32_t>(bytes);
        d.TimeStampLow = 0;
        d.TimeStampHigh = 0;
        const bool interrupt = stamp || _txQueued + 1 >= TxDescriptors / 2 || (_txTicket % TxInterruptEvery) == 0;
        d.TDES0.Value =
                TxOwn | (interrupt ? TxInterrupt : 0) | TxLast | TxFirst | TxChained | (stamp ? TxTimestampEnable : 0);
        writeBackDesc(&d);
        emac_ll_transmit_poll_demand(&EMAC_DMA, 0);
        _txHead = (i + 1) % TxDescriptors;
        ++_txQueued;
        {
                SpinLock::Locker state(_stateLock);
                if (_txQueued > _counters.txQueuedMax) _counters.txQueuedMax = static_cast<uint32_t>(_txQueued);
        }
        return makeResult(s.ticket);
}

void EthernetDevice::reclaim() {
        bool stamped;
        {
                Mutex::Locker lock(_txLock, WaitForever);
                stamped = reclaimLocked();
                if (_txWaiting) _txSpace.release();
        }
        if (stamped) _txStampWaiters.notify();
        return;
}

bool EthernetDevice::reclaimLocked() {
        bool  stamped = false;
        auto *tx = static_cast<TxDesc *>(_txDesc);
        while (_txQueued > 0) {
                TxDesc &d = tx[_txTail];
                invalidateDesc(&d);
                const uint32_t status = d.TDES0.Value;
                if (status & TxOwn) break;
                TxSlot &s = _txSlots[_txTail];
                if (s.wantsStamp && (status & TxTimestampStatus)) {
                        s.stamp.seconds = d.TimeStampHigh;
                        s.stamp.nanoseconds = d.TimeStampLow;
                        s.stamped = true;
                }
                s.done = true;
                stamped |= s.wantsStamp;
                {
                        SpinLock::Locker state(_stateLock);
                        if (status & TxErrorSummary)
                                ++_counters.txErrors;
                        else {
                                ++_counters.txFrames;
                                _counters.txBytes += s.length;
                        }
                }
                _txTail = (_txTail + 1) % TxDescriptors;
                --_txQueued;
        }
        return stamped;
}

Result<PtpTime> EthernetDevice::waitTxStamp(uint32_t ticket, unsigned int timeoutMs) {
        const Deadline  deadline(timeoutMs);
        Semaphore       woken(1, 0);
        WaitList::Entry entry(_txStampWaiters, woken);
        for (;;) {
                {
                        Mutex::Locker lock(_txLock, WaitForever);
                        const TxSlot *found = nullptr;
                        for (const TxSlot &s : _txSlots)
                                if (s.ticket == ticket) found = &s;
                        if (found == nullptr || !found->wantsStamp) return makeError<PtpTime>(Error::NotExist);
                        if (found->done) {
                                if (!found->stamped) return makeError<PtpTime>(Error::NotExist);
                                return makeResult(found->stamp);
                        }
                }
                if (!entry.isAdded()) return makeError<PtpTime>(Error::Busy);
                if (woken.acquire(deadline).isError()) return makeError<PtpTime>(Error::Timeout);
        }
}

EthernetDevice::Link EthernetDevice::link() const {
        SpinLock::Locker lock(_stateLock);
        return _link;
}

EthernetDevice::Counters EthernetDevice::counters() const {
        SpinLock::Locker lock(_stateLock);
        return _counters;
}

void EthernetDevice::setReceiver(Receiver fn, void *arg) {
        // The task reads the pair under the same lock, so it calls one or the other; a call to
        // the old one that has begun is waited for (receivers don't block), so the old one's
        // argument may go away once this returns.
        {
                SpinLock::Locker lock(_stateLock);
                _receiver = fn;
                _receiverArg = arg;
        }
        if (_task.handle() != xTaskGetCurrentTaskHandle())
                while (_inReceiver.load()) Task::sleep(1);
        return;
}

EthernetDevice::Receiver EthernetDevice::receiver(void *&arg) const {
        SpinLock::Locker lock(_stateLock);
        arg = _receiverArg;
        return _receiver;
}

void EthernetDevice::setLinkHandler(LinkHandler handler, void *arg) {
        // As setReceiver().
        if (handler != nullptr) {
                _linkHandlerArg = arg;
                _linkHandler = handler;
        } else {
                _linkHandler = nullptr;
                _linkHandlerArg = arg;
        }
        return;
}

PtpTime EthernetDevice::ptpTime() const {
        emac_ptp_dev_t *ptp = &EMAC_PTP;
        uint32_t        s = emac_ll_get_ts_seconds_val(ptp);
        uint32_t        ns = emac_ll_get_ts_sub_seconds_val(ptp);
        const uint32_t  again = emac_ll_get_ts_seconds_val(ptp);
        if (again != s) {
                s = again;
                ns = emac_ll_get_ts_sub_seconds_val(ptp);
        }
        return {s, ns};
}

Error EthernetDevice::setPtpTime(PtpTime time) {
        emac_ptp_dev_t *ptp = &EMAC_PTP;
        if (!emac_ll_is_ts_init_done(ptp)) return Error::Busy;
        emac_ll_set_ts_update_second_val(ptp, static_cast<uint32_t>(time.seconds));
        emac_ll_set_ts_update_sub_second_val(ptp, time.nanoseconds);
        emac_ll_ts_init_do(ptp);
        if (!waitUs([] { return emac_ll_is_ts_init_done(&EMAC_PTP); }, PtpTimeoutUs)) return Error::Timeout;
        return Error::Ok;
}

Error EthernetDevice::setPtpFrequency(int32_t ppb) {
        if (ppb > MaxPpb || ppb < -MaxPpb) return Error::OutOfRange;
        emac_ptp_dev_t *ptp = &EMAC_PTP;
        if (!emac_ll_is_ts_addend_update_done(ptp)) return Error::Busy;
        const int64_t addend = static_cast<int64_t>(PtpAddend) + static_cast<int64_t>(PtpAddend) * ppb / 1'000'000'000;
        emac_ll_set_ts_addend_val(ptp, static_cast<uint32_t>(addend));
        emac_ll_ts_addend_do_update(ptp);
        return Error::Ok;
}

void EthernetDevice::restartPhy() {
        _phyRestart.store(true);
        _wake.release();
        return;
}

EthernetDevice::Registers EthernetDevice::registers() {
        Registers r;
        r.macConfig = REG_READ(EMAC_MACCONFIGURATION_REG);
        r.frameFilter = REG_READ(EMAC_MACFRAMEFILTER_REG);
        r.macDebug = REG_READ(EMAC_DEBUG_REG);
        r.dmaStatus = REG_READ(EMAC_STATUS_REG);
        r.dmaMode = REG_READ(EMAC_OPERATIONMODE_REG);
        r.gmacCtrl = HP_SYSTEM.sys_gmac_ctrl0.val;
        r.clkCtrl0 = HP_SYS_CLKRST.peri_clk_ctrl00.val;
        r.clkCtrl1 = HP_SYS_CLKRST.peri_clk_ctrl01.val;
        r.rxList = REG_READ(EMAC_RECEIVEDESCRIPTORLISTADDRESS_REG);
        r.rxCurrent = REG_READ(EMAC_CURRENTHOSTRECEIVEDESCRIPTOR_REG);
        r.rxNext = static_cast<uint32_t>(_rxNext);
        r.rxEmpty = static_cast<uint32_t>(_rxEmpty.load(std::memory_order_relaxed));
        {
                SpinLock::Locker lock(_rxFreeLock);
                r.rxFree = static_cast<uint32_t>(_rxFreeCount);
        }
        auto *rx = static_cast<RxDesc *>(_rxDesc);
        for (size_t i = 0; i < RxDescriptors; ++i) {
                invalidateDesc(&rx[i]);
                if (rx[i].RDES0.Own == EMAC_LL_DMADESC_OWNER_DMA) ++r.rxOwnedByDma;
        }
        return r;
}

Result<uint16_t> EthernetDevice::readPhy(uint8_t reg) {
        Mutex::Locker   lock(_mdioLock, WaitForever);
        emac_mac_dev_t *mac = &EMAC_MAC;
        if (!waitUs([] { return !emac_ll_is_mii_busy(&EMAC_MAC); }, MdioTimeoutUs))
                return makeError<uint16_t>(Error::Busy);
        emac_ll_set_phy_addr(mac, board().ethernet.phyAddr);
        emac_ll_set_phy_reg(mac, reg);
        emac_ll_write_enable(mac, false);
        emac_ll_set_busy(mac, true);
        if (!waitUs([] { return !emac_ll_is_mii_busy(&EMAC_MAC); }, MdioTimeoutUs))
                return makeError<uint16_t>(Error::Timeout);
        return makeResult(static_cast<uint16_t>(emac_ll_get_phy_data(mac)));
}

Error EthernetDevice::writePhy(uint8_t reg, uint16_t value) {
        Mutex::Locker   lock(_mdioLock, WaitForever);
        emac_mac_dev_t *mac = &EMAC_MAC;
        if (!waitUs([] { return !emac_ll_is_mii_busy(&EMAC_MAC); }, MdioTimeoutUs)) return Error::Busy;
        emac_ll_set_phy_data(mac, value);
        emac_ll_set_phy_addr(mac, board().ethernet.phyAddr);
        emac_ll_set_phy_reg(mac, reg);
        emac_ll_write_enable(mac, true);
        emac_ll_set_busy(mac, true);
        if (!waitUs([] { return !emac_ll_is_mii_busy(&EMAC_MAC); }, MdioTimeoutUs)) return Error::Timeout;
        return Error::Ok;
}

void EthernetDevice::checkLink() {
        // The first time: the PHY's identity. Just out of its reset it is negotiating already;
        // otherwise a link it has is kept (see start()), and without one negotiation restarts.
        if (!_phyStarted) {
                auto [id1, e1] = readPhy(PhyId1);
                auto [id2, e2] = readPhy(PhyId2);
                if (e1.isError() || e2.isError()) {
                        bmdsErr("%s: no answer from the PHY", _name);
                        return;
                }
                _phyId = (static_cast<uint32_t>(id1) << 16) | id2;
                (void)readPhy(PhyStatus);
                auto [status, se] = readPhy(PhyStatus);
                const bool linked = se.isOk() && (status & StatusLink) && (status & StatusAutonegDone);
                if (!_phyWasReset && !linked) (void)writePhy(PhyControl, ControlAutoneg | ControlRestartAutoneg);
                bmdsInfo("%s: PHY %08lx at address %u%s", _name, static_cast<unsigned long>(_phyId),
                         board().ethernet.phyAddr,
                         _phyWasReset ? ", reset"
                         : linked     ? ", link kept"
                                      : "");
                _phyWasReset = false;
                _phyStarted = true;
        }
        // The link bit latches low: the second read is the state now.
        (void)readPhy(PhyStatus);
        auto [status, err] = readPhy(PhyStatus);
        if (err.isError()) return;
        Link l;
        l.up = (status & StatusLink) && (status & StatusAutonegDone);
        if (l.up) {
                auto [ours, e1] = readPhy(PhyAdvertise);
                auto [theirs, e2] = readPhy(PhyPartner);
                if (e1.isError() || e2.isError()) return;
                const uint16_t common = ours & theirs;
                if (common & Ability100Full) {
                        l.speedMbps = 100;
                        l.fullDuplex = true;
                } else if (common & Ability100Half) {
                        l.speedMbps = 100;
                } else if (common & Ability10Full) {
                        l.speedMbps = 10;
                        l.fullDuplex = true;
                } else {
                        l.speedMbps = 10;
                }
        }
        const Link was = link();
        if (l.up == was.up && l.speedMbps == was.speedMbps && l.fullDuplex == was.fullDuplex) return;
        applyLink(l);
        return;
}

void EthernetDevice::applyLink(const Link &l) {
        // The MAC and its DMA run only while the link is up, as ESP-IDF's driver does: with the
        // receiver on while the PHY reset and negotiated, the MAC's receiver sometimes never
        // passed a frame again (the DMA waiting, the MAC idle; 2026-10-10).
        const bool wasUp = link().up;
        if (wasUp) stopTraffic();
        if (l.up) {
                PERIPH_RCC_ATOMIC() {
                        emac_ll_clock_rmii_rx_tx_div(nullptr, l.speedMbps == 10 ? 19 : 1);
                }
                emac_ll_set_port_speed(&EMAC_MAC, l.speedMbps == 10 ? ETH_SPEED_10M : ETH_SPEED_100M);
                emac_ll_set_duplex(&EMAC_MAC, l.fullDuplex ? ETH_DUPLEX_FULL : ETH_DUPLEX_HALF);
                startTraffic();
        }
        {
                SpinLock::Locker lock(_stateLock);
                _link = l;
                ++_counters.linkChanges;
        }
        if (l.up)
                bmdsInfo("%s: link up, %u Mbit/s, %s duplex", _name, l.speedMbps, l.fullDuplex ? "full" : "half");
        else
                bmdsInfo("%s: link down", _name);
        const LinkHandler handler = _linkHandler;
        if (handler != nullptr) handler(_linkHandlerArg, *this);
        return;
}

void EthernetDevice::startTraffic() {
        // Transmit (the MAC, then its DMA: the other order can hang the MAC's transmitter,
        // ESP-IDF found), then receive (the DMA, then the MAC).
        emac_ll_clear_all_pending_intr(&EMAC_DMA);
        emac_ll_transmit_enable(&EMAC_MAC, true);
        emac_ll_start_stop_dma_transmit(&EMAC_DMA, true);
        emac_ll_start_stop_dma_receive(&EMAC_DMA, true);
        emac_ll_receive_enable(&EMAC_MAC, true);
        return;
}

void EthernetDevice::stopTraffic() {
        // emac_hal_stop()'s order: the transmit DMA, the MAC's receiver and transmitter, the
        // receive DMA, then the transmit FIFO flushed. Frames in the rings stay there: the DMA
        // resumes at the descriptor it stopped at.
        emac_ll_start_stop_dma_transmit(&EMAC_DMA, false);
        (void)waitUs([] { return emac_ll_transmit_frame_ctrl_status(&EMAC_MAC) == 0; }, 1000);
        emac_ll_receive_enable(&EMAC_MAC, false);
        emac_ll_transmit_enable(&EMAC_MAC, false);
        (void)waitUs([] { return emac_ll_receive_read_ctrl_state(&EMAC_MAC) == 0; }, 1000);
        emac_ll_start_stop_dma_receive(&EMAC_DMA, false);
        emac_ll_flush_trans_fifo_enable(&EMAC_DMA, true);
        (void)waitUs([] { return !emac_ll_get_flush_trans_fifo(&EMAC_DMA); }, 1000);
        return;
}

BMDS_NAMESPACE_END
