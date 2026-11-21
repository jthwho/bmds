/**
 * @file      spiports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * SPI ports (devplan/io.md §11.5): `io/spi2` and `io/spi3`, checked, started and stopped as they
 * change, `/dev/spidevB.C` per chip select while one runs. The bus's pins and transfers through
 * ESP-IDF's master (DMA off: the controller's 64-byte buffer, nothing allocated); the chip
 * selects are the port's own GPIO outputs.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#include <driver/spi_master.h>
#include <esp_cache.h>
#include <esp_clk_tree.h>
#include <esp_intr_alloc.h>
#include <esp_private/esp_clk_tree_common.h>
#include <esp_private/gdma.h>
#include <esp_private/periph_ctrl.h>
#include <esp_private/spi_common_internal.h>
#include <esp_rom_sys.h>
#include <hal/spi_hal.h>
#include <hal/spi_ll.h>
#include <bmds/gpiochip.h>
#include <bmds/io.h>
#include <bmds/ioport.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/psramobject.h>
#include <bmds/semaphore.h>
#include <bmds/spidevice.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/task.h>
#include <bmds/vfs.h>

#include "iochipops.h"
#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace SpiPorts {

        constexpr unsigned FirstBus = 2; // the P4's GP-SPIs: io/spi2, io/spi3
        constexpr unsigned PortCount = 2;
        constexpr size_t   MaxDevices = IoSpiSetup::MaxCs;
        constexpr size_t   Piece = 64;            // a transaction's bytes: the controller's buffer (DMA off)
        constexpr size_t   DmaPiece = 4096;       // a DMA transaction's bytes: the bounce buffers (internal RAM)
        constexpr size_t   CacheLine = 64;        // the L1 cache's line: a DMA buffer's sync is whole lines
        constexpr uint32_t SrcFreqMax = 80000000; // what the controller takes from its clock source (ESP-IDF's)
        constexpr size_t   MaxInputs = 24;        // the controller's input signals, saved
        constexpr uint32_t SpinBelowUs = 100;     // delay_usecs: spun below, slept from

        constexpr const char *CsRoles[MaxDevices] = {"CS0", "CS1", "CS2", "CS3", "CS4", "CS5"};

        struct SavedPad {
                        int8_t           pad = -1;
                        IoChipOps::Saved saved;
        };

        struct SavedInput {
                        uint16_t signal = 0;
                        uint32_t word = 0;
        };

        struct Port {
                        unsigned    bus = 0;        // 2, 3
                        char        owner[10] = {}; // `spidev2`
                        char        key[10] = {};   // `io/spi2`
                        Mutex       lock;           // its start and stop, and each message
                        IoSpiSetup  setup;          // as running
                        bool        running = false;
                        IoClaim     claim;
                        SavedPad    pads[3 + MaxDevices];
                        size_t      padCount = 0;
                        SavedInput  inputs[MaxInputs];
                        size_t      inputCount = 0;
                        size_t      deviceCount = 0;            // running: the chip selects (one if none)
                        SpiDevice  *devices[MaxDevices] = {};   // made when first started, kept
                        char        names[MaxDevices][12] = {}; // `spidev2.0`
                        bool        fileAdded[MaxDevices] = {};
                        SysConf::Id id = SysConf::NoId;
                        alignas(4) uint8_t tx[Piece] = {};
                        alignas(4) uint8_t rx[Piece] = {};

                        // DMA (`dma`, devplan/io.md §14.1): ESP-IDF's bus (pins, the two GDMA channels,
                        // their descriptors) with no ESP-IDF device on it; the transactions run here
                        // on spi_hal, as ESP-IDF's own SPI slave does, from bounce buffers of our own
                        // whose cache lines are synced whole whatever a piece's length.
                        spi_hal_context_t    hal = {};
                        intr_handle_t        intr = nullptr;
                        Semaphore            dmaDone{1, 0};
                        uint8_t             *dtx = nullptr;
                        uint8_t             *drx = nullptr;
                        spi_hal_dev_config_t halDev = {};
                        const SpiDevice     *applied = nullptr; // the device halDev is for, as applied
                        uint32_t             appliedMode = 0;
                        uint32_t             appliedHz = 0;

                        spi_host_device_t host() const { return spi_host_device_t(bus - 1); } // SPI2_HOST is 1

                        // Chip select @p index on (selected) or off. The lock held.
                        void select(size_t index, bool on) {
                                if (setup.csCount == 0) return;
                                const bool high = (devices[index]->_settings.mode & SpiDevice::CsHigh) != 0;
                                IoChipOps::setLevel(unsigned(setup.cs[index]), on == high);
                                return;
                        }

                        // Device @p d's ESP-IDF device for @p mode at @p hz, added again if it was
                        // for others. The lock held; the bus not acquired.
                        Error prepare(SpiDevice &d, uint32_t mode, uint32_t hz) {
                                if (d._handle != nullptr && d._handleMode == mode && d._handleHz == hz)
                                        return Error::Ok;
                                if (d._handle != nullptr) {
                                        spi_bus_remove_device(static_cast<spi_device_handle_t>(d._handle));
                                        d._handle = nullptr;
                                        d._actualHz = 0;
                                }
                                spi_device_interface_config_t c = {};
                                c.mode = uint8_t(mode & (SpiDevice::Cpol | SpiDevice::Cpha));
                                c.clock_speed_hz = int(hz);
                                c.spics_io_num = -1; // the port's own (§11.5)
                                c.queue_size = 1;
                                if (mode & SpiDevice::LsbFirst) c.flags |= SPI_DEVICE_BIT_LSBFIRST;
                                const bool halfDuplex = (mode & SpiDevice::ThreeWire) != 0;
                                if (halfDuplex) c.flags |= SPI_DEVICE_3WIRE | SPI_DEVICE_HALFDUPLEX;
                                // Full duplex reads only so fast through the matrix: lowered to the limit,
                                // as Linux lowers to a controller's maximum.
                                if (!halfDuplex) {
                                        const spi_bus_attr_t *attr = spi_bus_get_attr(host());
                                        const bool            matrix =
                                                attr == nullptr || !(attr->flags & SPICOMMON_BUSFLAG_IOMUX_PINS);
                                        const int limit = spi_get_freq_limit(matrix, 0);
                                        if (limit > 0 && c.clock_speed_hz > limit) c.clock_speed_hz = limit;
                                }
                                spi_device_handle_t handle = nullptr;
                                const esp_err_t     err = spi_bus_add_device(host(), &c, &handle);
                                if (err != ESP_OK) {
                                        bmdsWarn("io: %s.%u: no device at %u Hz (%s)", owner, unsigned(d._index),
                                                 unsigned(hz), esp_err_to_name(err));
                                        return Error::IoError;
                                }
                                int khz = 0;
                                (void)spi_device_get_actual_freq(handle, &khz);
                                d._handle = handle;
                                d._handleMode = mode;
                                d._handleHz = hz;
                                d._actualHz = uint32_t(khz) * 1000;
                                return Error::Ok;
                        }

                        // Stops the port: the devices, the bus, the pads and input selectors as they
                        // were, the claim. The lock held.
                        void stop() {
                                if (!running) return;
                                if (setup.dma) {
                                        if (intr != nullptr) esp_intr_free(intr);
                                        intr = nullptr;
                                        spi_hal_deinit(&hal);
                                        Memory::free(dtx);
                                        Memory::free(drx);
                                        dtx = drx = nullptr;
                                        applied = nullptr;
                                }
                                for (size_t i = 0; i < deviceCount; ++i) {
                                        SpiDevice *d = devices[i];
                                        if (d->_handle != nullptr)
                                                spi_bus_remove_device(static_cast<spi_device_handle_t>(d->_handle));
                                        d->_handle = nullptr;
                                        d->_actualHz = 0;
                                }
                                spi_bus_free(host());
                                putBack();
                                claim.reset();
                                running = false;
                                deviceCount = 0;
                                bmdsInfo("io: %s stopped", owner);
                                return;
                        }

                        // The input selectors and pads as they were before the port.
                        void putBack() {
                                for (size_t i = inputCount; i-- > 0;)
                                        IoChipOps::setInputWord(inputs[i].signal, inputs[i].word);
                                for (size_t i = padCount; i-- > 0;)
                                        IoChipOps::restorePad(unsigned(pads[i].pad), pads[i].saved);
                                inputCount = 0;
                                padCount = 0;
                                return;
                        }

                        void  start(const IoSpiSetup &s);
                        void  addFiles();
                        Error startDma();
                        Error prepareDma(SpiDevice &d, uint32_t mode, uint32_t hz);
                        Error dmaPiece(const uint8_t *from, uint8_t *to, size_t n, bool readOnly, unsigned timeoutMs);
        };

        struct State {
                        SysConf::Watcher watcher;
                        Port             ports[PortCount];
        };

        constinit PsramObject<State> state;

        // ---- DMA (devplan/io.md §14.1)

        // A transaction done: the waiting task woken (its interrupt stays off till the next).
        void dmaIsr(void *arg) {
                Port &p = *static_cast<Port *>(arg);
                spi_ll_clear_int_stat(p.hal.hw);
                p.dmaDone.releaseFromIsr();
                return;
        }

        // The divider before the controller: even, at least 2, as ESP-IDF's master picks it.
        uint32_t preDivFor(uint32_t srcHz, uint32_t hz) {
                uint32_t minDiv = ((srcHz / SrcFreqMax) + 1) & ~1u;
                if (minDiv < 2) minDiv = 2;
                const uint32_t total = srcHz / hz;
                for (uint32_t d = minDiv; d <= total && d <= SPI_LL_SRC_PRE_DIV_MAX; d += 2)
                        if (total % d == 0 && total / d <= SPI_LL_PERIPH_CLK_DIV_MAX) return d;
                return minDiv;
        }

        Error Port::startDma() {
                const size_t bytes = DmaPiece;
                dtx = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, bytes, CacheLine));
                drx = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, bytes, CacheLine));
                if (dtx == nullptr || drx == nullptr) return Error::NoMem;
                // The transaction-done interrupt, off till the stale status spi_hal_init() sets is
                // cleared.
                if (esp_intr_alloc(spicommon_irqsource_for_host(host()), ESP_INTR_FLAG_INTRDISABLED, dmaIsr, this,
                                   &intr) != ESP_OK)
                        return Error::IoError;
                spi_hal_init(&hal, uint32_t(host()));
                spi_hal_config_io_default_level(&hal, false);
                spi_ll_clear_int_stat(hal.hw);
                (void)dmaDone.tryAcquire();
                esp_intr_enable(intr);
                applied = nullptr;
                return Error::Ok;
        }

        // Device @p d's timing and mode on the controller, for @p mode at @p hz (as ESP-IDF's master
        // computes and applies them), when not already. The lock held.
        Error Port::prepareDma(SpiDevice &d, uint32_t mode, uint32_t hz) {
                if (applied == &d && appliedMode == mode && appliedHz == hz) return Error::Ok;
                const spi_clock_source_t src = SPI_CLK_SRC_DEFAULT;
                uint32_t                 srcHz = 0;
                if (esp_clk_tree_enable_src(soc_module_clk_t(src), true) != ESP_OK ||
                    esp_clk_tree_src_get_freq_hz(soc_module_clk_t(src), ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                                 &srcHz) != ESP_OK)
                        return Error::IoError;
                const uint32_t         preDiv = preDivFor(srcHz, hz);
                const bool             halfDuplex = (mode & SpiDevice::ThreeWire) != 0;
                const spi_bus_attr_t  *attr = spi_bus_get_attr(host());
                const bool             matrix = attr == nullptr || !(attr->flags & SPICOMMON_BUSFLAG_IOMUX_PINS);
                spi_hal_timing_param_t tp = {};
                tp.half_duplex = halfDuplex;
                tp.no_compensate = 0;
                tp.clk_src_hz = srcHz / preDiv;
                tp.expected_freq = int(hz);
                tp.duty_cycle = 128;
                tp.input_delay_ns = 0;
                tp.use_gpio = matrix;
                spi_hal_dev_config_t dev = {};
                if (spi_hal_cal_clock_conf(&tp, &dev.timing_conf) != ESP_OK) {
                        // Full duplex reads only so fast through the matrix: lowered to the limit.
                        tp.expected_freq = spi_get_freq_limit(matrix, 0);
                        if (spi_hal_cal_clock_conf(&tp, &dev.timing_conf) != ESP_OK) return Error::Invalid;
                }
                dev.timing_conf.clock_source = src;
                dev.timing_conf.source_pre_div = preDiv;
                dev.timing_conf.source_real_freq = srcHz / preDiv;
                dev.mode = int(mode & (SpiDevice::Cpol | SpiDevice::Cpha));
                dev.cs_setup = 0;
                dev.cs_hold = 1;
                dev.cs_pin_id = -1; // no hardware chip select: the port's own (§11.5)
                dev.sio = halfDuplex;
                dev.half_duplex = halfDuplex;
                dev.tx_lsbfirst = (mode & SpiDevice::LsbFirst) != 0;
                dev.rx_lsbfirst = dev.tx_lsbfirst;
                halDev = dev;
                spi_hal_setup_device(&hal, &halDev);
                PERIPH_RCC_ATOMIC() {
                        spi_ll_clk_source_pre_div(hal.hw, uint8_t(preDiv / 2), 2);
                        spi_ll_set_clk_source(hal.hw, src);
                }
                applied = &d;
                appliedMode = mode;
                appliedHz = hz;
                d._actualHz = uint32_t(halDev.timing_conf.real_freq);
                return Error::Ok;
        }

        // One piece of a transfer (up to DmaPiece bytes) through the bounce buffers. The lock held,
        // the device prepared, its chip select on.
        Error Port::dmaPiece(const uint8_t *from, uint8_t *to, size_t n, bool readOnly, unsigned timeoutMs) {
                if (n <= Piece) {
                        // Up to the controller's buffer: by the CPU, as without DMA. A message of a few
                        // bytes costs half the time this way (devplan/io.md §14.2): no cache syncs, no
                        // descriptors, no DMA started.
                        spi_hal_trans_config_t f = {};
                        f.line_mode.data_lines = 1;
                        f.line_mode.addr_lines = 1;
                        f.line_mode.cmd_lines = 1;
                        if (!readOnly) {
                                if (from == nullptr)
                                        std::memset(tx, 0, n);
                                else
                                        std::memcpy(tx, from, n);
                                f.send_buffer = tx;
                                f.tx_bitlen = int(n * 8);
                        }
                        if (to != nullptr) {
                                f.rcv_buffer = rx;
                                f.rx_bitlen = int(n * 8);
                        }
                        spi_hal_setup_trans(&hal, &halDev, &f);
                        spi_ll_dma_tx_enable(hal.hw, false);
                        spi_ll_dma_rx_enable(hal.hw, false);
                        spi_hal_push_tx_buffer(&hal, &f);
                        spi_hal_enable_data_line(hal.hw, (!halDev.half_duplex && to != nullptr) || !readOnly,
                                                 to != nullptr);
                        spi_hal_user_start(&hal);
                        if (dmaDone.acquire(timeoutMs).isError()) {
                                bmdsWarn("io: %s: a transfer didn't end", owner);
                                return Error::IoError;
                        }
                        if (to != nullptr) {
                                spi_hal_fetch_result(&hal);
                                std::memcpy(to, rx, n);
                        }
                        return Error::Ok;
                }
                const size_t         lines = (n + CacheLine - 1) & ~(CacheLine - 1);
                const spi_dma_ctx_t *ctx = spi_bus_get_dma_ctx(host());
                if (ctx == nullptr) return Error::IoError;
                spi_hal_trans_config_t t = {};
                t.line_mode.data_lines = 1;
                t.line_mode.addr_lines = 1;
                t.line_mode.cmd_lines = 1;
                uint8_t *send = nullptr;
                uint8_t *receive = to != nullptr ? drx : nullptr;
                if (!readOnly) {
                        if (from == nullptr)
                                std::memset(dtx, 0, n);
                        else if (from != dtx) // (a piece swapped in place is there already)
                                std::memcpy(dtx, from, n);
                        send = dtx;
                        t.tx_bitlen = int(n * 8);
                        // Whole lines written back: the DMA reads memory, not the cache.
                        (void)esp_cache_msync(dtx, lines, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
                }
                if (receive != nullptr) {
                        t.rx_bitlen = int(n * 8);
                        // Whole lines dropped from the cache, so none is written back over what the
                        // DMA brings.
                        (void)esp_cache_msync(drx, lines, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                }
                t.send_buffer = send;
                t.rcv_buffer = receive;
                spi_hal_setup_trans(&hal, &halDev, &t);
                if (receive != nullptr) {
                        spicommon_dma_desc_setup_link(ctx->dmadesc_rx, receive, int(n), true);
                        gdma_reset(ctx->rx_dma_chan);
                        spi_hal_hw_prepare_rx(hal.hw);
                        gdma_start(ctx->rx_dma_chan, intptr_t(ctx->dmadesc_rx));
                }
                if (send != nullptr) {
                        spicommon_dma_desc_setup_link(ctx->dmadesc_tx, send, int(n), false);
                        gdma_reset(ctx->tx_dma_chan);
                        spi_hal_hw_prepare_tx(hal.hw);
                        gdma_start(ctx->tx_dma_chan, intptr_t(ctx->dmadesc_tx));
                }
                // A direction this piece doesn't use: its DMA off (a FIFO piece, or one before, left it).
                if (receive == nullptr) spi_ll_dma_rx_enable(hal.hw, false);
                if (send == nullptr) spi_ll_dma_tx_enable(hal.hw, false);
                spi_hal_enable_data_line(hal.hw, (!halDev.half_duplex && receive != nullptr) || send != nullptr,
                                         receive != nullptr);
                spi_hal_user_start(&hal);
                if (dmaDone.acquire(timeoutMs).isError()) {
                        bmdsWarn("io: %s: a DMA transfer didn't end", owner);
                        return Error::IoError;
                }
                if (receive != nullptr) {
                        (void)esp_cache_msync(drx, lines, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        std::memcpy(to, drx, n);
                }
                return Error::Ok;
        }

        // The controller's matrix signal @p what (`D`, `Q`) for bus @p bus: `SPI2_Q`.
        Result<uint16_t> inputSignal(unsigned bus, const char *what) {
                char name[16];
                std::snprintf(name, sizeof(name), "SPI%u_%s", bus, what);
                return ioChip().findInput(StringView(name));
        }

        // A value's pads with what each does.
        size_t usesOf(const IoSpiSetup &s, IoUse (&out)[3 + MaxDevices]) {
                size_t     n = 0;
                const auto add = [&](int8_t pad, const char *role) {
                        if (pad >= 0) out[n++] = IoUse{IoResource(pad), role};
                };
                add(s.sck, "SCK");
                add(s.mosi, "MOSI");
                add(s.miso, "MISO");
                for (size_t i = 0; i < s.csCount; ++i) add(s.cs[i], CsRoles[i]);
                return n;
        }

        void Port::start(const IoSpiSetup &s) {
                const IoResource spi = Io::instance(IoKind::Spi, bus);
                IoUse            uses[3 + MaxDevices + 1];
                IoUse            pinUses[3 + MaxDevices];
                const size_t     pinCount = usesOf(s, pinUses);
                size_t           u = 0;
                for (size_t i = 0; i < pinCount; ++i) uses[u++] = pinUses[i];
                uses[u++] = IoUse{spi, "the port"};
                IoResource blocked = NoIoResource;
                auto [held, err] =
                        Io::registry().claim(StringView(owner), IoOwnerKind::User, Span<const IoUse>(uses, u),
                                             s.force ? IoRegistry::Force : 0u, &blocked);
                if (err.isError()) {
                        char busy[160];
                        (void)Io::busyText(blocked, busy, sizeof(busy));
                        bmdsWarn("io: %s not started: %s", key, busy);
                        return;
                }
                // The devices, made the first time and kept (their files point at them).
                const size_t count = s.csCount == 0 ? 1 : s.csCount;
                for (size_t i = 0; i < count; ++i) {
                        if (devices[i] != nullptr) continue;
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(SpiDevice));
                        if (mem == nullptr) {
                                bmdsWarn("io: %s not started: no memory", key);
                                return; // the claim ends with this scope
                        }
                        devices[i] = ::new (mem) SpiDevice(*this, uint8_t(i));
                }
                // What ESP-IDF changes and leaves: the pads, and the controller's input selectors.
                padCount = 0;
                for (size_t i = 0; i < pinCount; ++i) {
                        pads[padCount].pad = int8_t(pinUses[i].resource);
                        IoChipOps::savePad(unsigned(pinUses[i].resource), pads[padCount].saved);
                        ++padCount;
                }
                inputCount = 0;
                const IoChip &chip = ioChip();
                for (size_t sig = 0; sig < chip.inputSignals.size() && inputCount < MaxInputs; ++sig) {
                        if (chip.inputSignals[sig] == nullptr || IoSetups::instanceOf(chip, uint16_t(sig), true) != spi)
                                continue;
                        inputs[inputCount].signal = uint16_t(sig);
                        inputs[inputCount].word = IoChipOps::inputWord(unsigned(sig));
                        ++inputCount;
                }
                // The chip selects first, released, so nothing is selected while the bus starts.
                for (size_t i = 0; i < s.csCount; ++i) {
                        IoPinSetup cs;
                        cs.pad = uint8_t(s.cs[i]);
                        cs.mode = IoPinSetup::Mode::Output;
                        cs.high = !s.csHigh;
                        IoChipOps::configurePad(unsigned(s.cs[i]), cs);
                }
                spi_bus_config_t bc = {};
                bc.sclk_io_num = s.sck;
                bc.mosi_io_num = s.mosi;
                bc.miso_io_num = s.miso;
                bc.quadwp_io_num = -1;
                bc.quadhd_io_num = -1;
                bc.max_transfer_sz = int(s.dma ? DmaPiece : Piece);
                bc.flags = SPICOMMON_BUSFLAG_MASTER;
                if (const esp_err_t e = spi_bus_initialize(host(), &bc, s.dma ? SPI_DMA_CH_AUTO : SPI_DMA_DISABLED);
                    e != ESP_OK) {
                        putBack();
                        bmdsWarn("io: %s not started: the controller (%s)", key, esp_err_to_name(e));
                        return;
                }
                if (s.dma) {
                        if (const Error derr = startDma(); derr.isError()) {
                                if (intr != nullptr) esp_intr_free(intr);
                                intr = nullptr;
                                Memory::free(dtx);
                                Memory::free(drx);
                                dtx = drx = nullptr;
                                spi_bus_free(host());
                                putBack();
                                bmdsWarn("io: %s not started: its DMA (%s)", key, derr.desc());
                                return;
                        }
                }
                // What ESP-IDF's master doesn't: MOSI read back, as the controller's D (3-wire) or
                // its Q (loopback: MISO from MOSI inside the chip).
                if (s.threeWire || s.loopback) {
                        const auto [signal, serr] = inputSignal(bus, s.threeWire ? "D" : "Q");
                        if (serr.isOk()) {
                                IoChipOps::enableInput(unsigned(s.mosi));
                                IoChipOps::routeInput(signal, unsigned(s.mosi));
                        }
                }
                const uint32_t mode = uint32_t(s.mode) | (s.csHigh ? SpiDevice::CsHigh : 0) |
                                      (s.lsbFirst ? SpiDevice::LsbFirst : 0) |
                                      (s.threeWire ? SpiDevice::ThreeWire : 0) | (s.loopback ? SpiDevice::Loop : 0) |
                                      (s.csCount == 0 ? SpiDevice::NoCs : 0);
                for (size_t i = 0; i < count; ++i) {
                        SpiDevice &d = *devices[i];
                        d._settings.mode = mode;
                        d._settings.bits = s.bits;
                        d._settings.speedHz = s.clock;
                        d._handle = nullptr;
                        d._actualHz = 0;
                }
                claim = std::move(held);
                setup = s;
                deviceCount = count;
                running = true;
                addFiles();
                bmdsInfo("io: %s: /dev/%s.0%s at %u Hz, mode %u%s%s", key, owner, count > 1 ? " and on" : "",
                         unsigned(s.clock), unsigned(s.mode), s.loopback ? ", looped back" : "",
                         s.dma ? ", by DMA" : "");
                return;
        }

        bool filePresent(void *arg) {
                return static_cast<const SpiDevice *>(arg)->isRunning();
        }

        // /dev/spidevB.C, there while the port runs with it: added when first started (the table of
        // /dev's files is small, Vfs::MaxDeviceFiles), kept for the boot.
        void Port::addFiles() {
                for (size_t i = 0; i < deviceCount; ++i) {
                        if (fileAdded[i]) continue;
                        std::snprintf(names[i], sizeof(names[i]), "%s.%u", owner, unsigned(i));
                        Vfs::DeviceFile file;
                        file.name = names[i];
                        file.arg = devices[i];
                        file.isDevice = true;
                        file.present = filePresent;
                        file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                                auto         *d = static_cast<SpiDevice *>(arg);
                                const uint8_t mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) |
                                                     ((flags & Vfs::Write) ? FileHandle::Write : 0);
                                if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                                if (!d->isRunning()) return makeError<FileHandle::Ptr>(Error::NotExist);
                                return FileHandle::open(*d, mode);
                        };
                        if (const Error err = Vfs::addDeviceFile(file); err.isError())
                                bmdsWarn("io: /dev/%s: %s", names[i], err.desc());
                        else
                                fileAdded[i] = true;
                }
                return;
        }

        // io/spiN's check: the value parses, and without `force` takes no pin or controller another
        // owner holds or the board wires.
        template <unsigned N> Error checkSpi(StringView value) {
                if (value.isEmpty()) return Error::Ok; // off
                if (!Io::isReady()) return Error::NotReady;
                IoSpiSetup s;
                char       why[IoSpiSetup::ReasonBytes];
                if (const Error err = s.parse(ioChip(), value, why); err.isError()) {
                        SysConf::refuse(StringView(why));
                        return err;
                }
                char owner[10];
                std::snprintf(owner, sizeof(owner), "spidev%u", N);
                const IoResource spi = Io::instance(IoKind::Spi, N);
                if (spi == NoIoResource) {
                        SysConf::refuse("this chip has no such SPI controller");
                        return Error::Invalid;
                }
                if (s.force) return Error::Ok;
                if (IoSetups::takenElsewhere(spi, owner)) return IoSetups::refuseTaken(spi, nullptr);
                IoUse        uses[3 + MaxDevices];
                const size_t n = usesOf(s, uses);
                for (size_t i = 0; i < n; ++i)
                        if (IoSetups::takenElsewhere(uses[i].resource, owner))
                                return IoSetups::refuseTaken(uses[i].resource, nullptr);
                return Error::Ok;
        }

        constexpr const char *SpiHelp = "An SPI port on chosen pins, /dev/spidevN.C while set: 'sck=4 mosi=53 miso=54 "
                                        "cs=5 mode=0 clock=1m'; cs=5,6 (up to 6), bits=, lsb, cs-high, 3wire, "
                                        "loopback, force (devplan/io.md)";
        BMDS_SYSCONF(ioSpi2, "io/spi2", Config::Type::String, "", SpiHelp, 0, 0, nullptr, checkSpi<2>);
        BMDS_SYSCONF(ioSpi3, "io/spi3", Config::Type::String, "", SpiHelp, 0, 0, nullptr, checkSpi<3>);

        // Applies io/spiN's value now.
        void apply(Port &p) {
                const GpioChip::Change change; // line watchers told once, after the lock
                Mutex::Locker          lock(p.lock, WaitForever);
                String                 value(MemSpace::Psram);
                if (SysConf::get(p.id, value).isError()) return;
                if (value.isEmpty()) {
                        p.stop();
                        return;
                }
                IoSpiSetup s;
                char       why[IoSpiSetup::ReasonBytes];
                if (s.parse(ioChip(), value.view(), why).isError()) {
                        bmdsWarn("io: %s: %s: not changed", p.key, why);
                        return;
                }
                if (p.running && s == p.setup) return;
                p.stop();
                p.start(s);
                return;
        }

        void changed(void *arg) {
                (void)arg;
                SysConf::Id id;
                const bool  all = state->watcher.overflowed();
                bool        which[PortCount] = {};
                while (state->watcher.next(id))
                        for (unsigned n = 0; n < PortCount; ++n)
                                if (state->ports[n].id == id) which[n] = true;
                for (unsigned n = 0; n < PortCount; ++n)
                        if (all || which[n]) apply(state->ports[n]);
                return;
        }

        // A restart leaves IO_MUX and the matrix as they are (devplan/io.md §10.4): every port's
        // pads put back first. No lock: the restart's task, the others about to stop.
        void stopAtRestart() {
                if (!state.isCreated()) return;
                for (Port &p : state->ports)
                        if (p.running) p.putBack();
                return;
        }

        // Swaps each word's bytes of @p bytes (@p bits 16 or 32): Linux's words are in the CPU's
        // order, sent most significant bit first.
        void swapWords(uint8_t *bytes, size_t length, uint8_t bits) {
                if (bits == 16) {
                        for (size_t i = 0; i + 1 < length; i += 2) std::swap(bytes[i], bytes[i + 1]);
                } else if (bits == 32) {
                        for (size_t i = 0; i + 3 < length; i += 4) {
                                std::swap(bytes[i], bytes[i + 3]);
                                std::swap(bytes[i + 1], bytes[i + 2]);
                        }
                }
                return;
        }

        bool wordsOk(uint8_t bits) {
                return bits == 8 || bits == 16 || bits == 32;
        }

} // namespace SpiPorts

using SpiPorts::Port;

unsigned SpiDevice::bus() const {
        return _port.bus;
}

bool SpiDevice::isRunning() const {
        return _port.running && _index < _port.deviceCount;
}

SpiDevice *SpiDevice::find(unsigned bus, unsigned index) {
        if (!SpiPorts::state.isCreated() || bus < SpiPorts::FirstBus ||
            bus >= SpiPorts::FirstBus + SpiPorts::PortCount || index >= SpiPorts::MaxDevices)
                return nullptr;
        return SpiPorts::state->ports[bus - SpiPorts::FirstBus].devices[index];
}

Error SpiDevice::settings(Settings &out, uint32_t *actualHz) const {
        Mutex::Locker lock(_port.lock, WaitForever);
        if (!isRunning()) return Error::NotExist;
        out = _settings;
        if (actualHz != nullptr) *actualHz = _actualHz;
        return Error::Ok;
}

Error SpiDevice::setSettings(const Settings &s) {
        Mutex::Locker lock(_port.lock, WaitForever);
        if (!isRunning()) return Error::NotExist;
        const uint8_t bits = s.bits == 0 ? 8 : s.bits;
        if ((s.mode & ~SettableModeBits) != (_settings.mode & ~SettableModeBits)) return Error::Invalid;
        if (!SpiPorts::wordsOk(bits) || s.speedHz < IoSpiSetup::MinClock) return Error::Invalid;
        // A chip select's level follows its polarity at once: released either way.
        const bool csHighChanged = ((s.mode ^ _settings.mode) & CsHigh) != 0;
        _settings.mode = s.mode;
        _settings.bits = bits;
        _settings.speedHz = s.speedHz;
        if (csHighChanged) _port.select(_index, false);
        return Error::Ok;
}

Result<size_t> SpiDevice::transfer(Span<const Transfer> transfers) {
        Mutex::Locker lock(_port.lock, WaitForever);
        if (!isRunning()) return makeError<size_t>(Error::NotExist);
        if (transfers.isEmpty() || transfers.size() > MaxTransfers) return makeError<size_t>(Error::Invalid);
        const bool halfDuplex = (_settings.mode & ThreeWire) != 0;
        size_t     total = 0;
        for (const Transfer &t : transfers) {
                const uint8_t bits = t.bits == 0 ? _settings.bits : t.bits;
                if (!SpiPorts::wordsOk(bits) || t.length % (bits / 8) != 0) return makeError<size_t>(Error::Invalid);
                if (t.speedHz != 0 && t.speedHz < IoSpiSetup::MinClock) return makeError<size_t>(Error::Invalid);
                if (halfDuplex && t.tx != nullptr && t.rx != nullptr) return makeError<size_t>(Error::Invalid);
                total += t.length;
                if (total > MaxBytes) return makeError<size_t>(Error::OutOfRange);
        }
        bool  acquired = false;
        bool  selected = false;
        Error failed = Error::Ok;
        for (size_t i = 0; i < transfers.size() && failed.isOk(); ++i) {
                const Transfer &t = transfers[i];
                const uint8_t   bits = t.bits == 0 ? _settings.bits : t.bits;
                const uint32_t  hz = t.speedHz == 0 ? _settings.speedHz : t.speedHz;
                if (_port.setup.dma) {
                        // DMA: the controller set up for this device, then pieces of up to 4 KB.
                        failed = _port.prepareDma(*this, _settings.mode, hz);
                        if (failed.isError()) break;
                        if (!selected) {
                                _port.select(_index, true);
                                selected = true;
                        }
                        const bool swap = bits > 8 && (_settings.mode & LsbFirst) == 0;
                        for (size_t done = 0; done < t.length && failed.isOk();) {
                                const size_t n =
                                        t.length - done < SpiPorts::DmaPiece ? t.length - done : SpiPorts::DmaPiece;
                                const bool     readOnly = halfDuplex && t.rx != nullptr;
                                const uint8_t *out = t.tx != nullptr ? t.tx + done : nullptr;
                                uint8_t       *in = t.rx != nullptr ? t.rx + done : nullptr;
                                // Words wider than a byte, sent most significant bit first: swapped
                                // in the bounce buffer, both ways.
                                if (swap && out != nullptr) {
                                        std::memcpy(_port.dtx, out, n);
                                        SpiPorts::swapWords(_port.dtx, n, bits);
                                        failed = _port.dmaPiece(_port.dtx, in, n, readOnly, 1000);
                                } else {
                                        failed = _port.dmaPiece(out, in, n, readOnly, 1000);
                                }
                                if (failed.isOk() && swap && in != nullptr) SpiPorts::swapWords(in, n, bits);
                                done += n;
                        }
                        if (failed.isError()) break;
                        if (t.delayUs != 0) {
                                if (t.delayUs < SpiPorts::SpinBelowUs)
                                        esp_rom_delay_us(t.delayUs);
                                else
                                        Task::sleep((t.delayUs + 999u) / 1000u);
                        }
                        if (t.csChange && i + 1 < transfers.size()) {
                                _port.select(_index, false);
                                selected = false;
                        }
                        continue;
                }
                if (_handle == nullptr || _handleMode != _settings.mode || _handleHz != hz) {
                        // Added again for this transfer: the bus let go first, the chip select kept.
                        if (acquired) spi_device_release_bus(static_cast<spi_device_handle_t>(_handle));
                        acquired = false;
                        failed = _port.prepare(*this, _settings.mode, hz);
                        if (failed.isError()) break;
                }
                auto *handle = static_cast<spi_device_handle_t>(_handle);
                if (!acquired) {
                        // Sets the device's mode up: the clock idles as it should before CS falls.
                        if (spi_device_acquire_bus(handle, portMAX_DELAY) != ESP_OK) {
                                failed = Error::IoError;
                                break;
                        }
                        acquired = true;
                }
                if (!selected) {
                        _port.select(_index, true);
                        selected = true;
                }
                const bool swap = bits > 8 && (_settings.mode & LsbFirst) == 0;
                for (size_t done = 0; done < t.length;) {
                        const size_t      n = t.length - done < SpiPorts::Piece ? t.length - done : SpiPorts::Piece;
                        spi_transaction_t x = {};
                        if (halfDuplex && t.rx != nullptr) {
                                x.rxlength = n * 8; // MOSI turned round: read only
                                x.rx_buffer = _port.rx;
                        } else {
                                if (t.tx != nullptr) {
                                        std::memcpy(_port.tx, t.tx + done, n);
                                        if (swap) SpiPorts::swapWords(_port.tx, n, bits);
                                } else {
                                        std::memset(_port.tx, 0, n);
                                }
                                x.length = n * 8;
                                x.tx_buffer = _port.tx;
                                if (!halfDuplex && t.rx != nullptr) x.rx_buffer = _port.rx;
                        }
                        if (spi_device_transmit(handle, &x) != ESP_OK) {
                                failed = Error::IoError;
                                break;
                        }
                        if (t.rx != nullptr) {
                                if (swap) SpiPorts::swapWords(_port.rx, n, bits);
                                std::memcpy(t.rx + done, _port.rx, n);
                        }
                        done += n;
                }
                if (failed.isError()) break;
                if (t.delayUs != 0) {
                        if (t.delayUs < SpiPorts::SpinBelowUs)
                                esp_rom_delay_us(t.delayUs);
                        else
                                Task::sleep((t.delayUs + 999u) / 1000u);
                }
                // cs_change: released between this transfer and the next (on the last, Linux's
                // hint to keep it for the next message isn't kept: released as always).
                if (t.csChange && i + 1 < transfers.size()) {
                        _port.select(_index, false);
                        selected = false;
                }
        }
        if (selected) _port.select(_index, false);
        if (acquired) spi_device_release_bus(static_cast<spi_device_handle_t>(_handle));
        if (failed.isError()) return makeError<size_t>(failed);
        return makeResult(total);
}

Result<size_t> SpiDevice::read(WritableByteSpan buf, unsigned int timeoutMs) {
        (void)timeoutMs;
        Transfer t;
        t.rx = buf.data();
        t.length = uint32_t(buf.size() < MaxBytes ? buf.size() : MaxBytes);
        return transfer(Span<const Transfer>(&t, 1));
}

Result<size_t> SpiDevice::write(ByteSpan data, unsigned int timeoutMs) {
        (void)timeoutMs;
        Transfer t;
        t.tx = data.data();
        t.length = uint32_t(data.size() < MaxBytes ? data.size() : MaxBytes);
        return transfer(Span<const Transfer>(&t, 1));
}

namespace IoSetups {

        Error startSpiPorts() {
                using namespace SpiPorts;
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                for (unsigned n = 0; n < PortCount; ++n) {
                        Port &p = s->ports[n];
                        p.bus = FirstBus + n;
                        std::snprintf(p.owner, sizeof(p.owner), "spidev%u", p.bus);
                        std::snprintf(p.key, sizeof(p.key), "io/spi%u", p.bus);
                        auto [id, err] = SysConf::find(StringView(p.key));
                        if (err.isOk()) p.id = id;
                }
                if (System::addRestartHook(stopAtRestart).isError())
                        bmdsWarn("io: no restart hook: the SPI ports' pins will stay set up across a restart");
                for (Port &p : s->ports) apply(p);
                return s->watcher.start(StringView("io"), changed, nullptr);
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
