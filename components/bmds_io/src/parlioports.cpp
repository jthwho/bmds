/**
 * @file      parlioports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * PARLIO ports (devplan/io.md §15.5): `io/parlio0`, `io/parlio1`, each a sender (the TX unit) or
 * a receiver (the RX unit), `/dev/parlioN` while one runs. Built on ESP-IDF's driver: a sender's
 * writes go out as transfers from four internal DMA buffers taken in turn; a receiver samples
 * without end into its ring (a soft delimiter and partial receive, the driver's logic analyzer's
 * way), the reader following it.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <driver/gpio.h>
#include <driver/parlio_rx.h>
#include <driver/parlio_tx.h>
#include <esp_cache.h>
#include <hal/dma_types.h>
#include <soc/axi_dma_struct.h>
#include <soc/ext_mem_defs.h>
#include <soc/gdma_channel.h>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/flashwrites.h>
#include <bmds/gpiochip.h>
#include <bmds/interrupt.h>
#include <bmds/io.h>
#include <bmds/ioport.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/parlioport.h>
#include <bmds/placement.h>
#include <bmds/psramobject.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/vfs.h>

#include "iochipops.h"
#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace ParlioPorts {

        constexpr unsigned PortCount = 2;     // io/parlio0, io/parlio1: the TX unit and the RX unit
        constexpr size_t   TxPieces = 4;      // a sender's DMA buffers, sent in turn
        constexpr size_t   TxPiece = 4096;    // bytes each
        constexpr size_t   CacheLine = 64;    // the L1's: the buffers aligned to it
        constexpr unsigned MaxNodes = 24;     // a receiver's ring in ESP-IDF's descriptors: 64 KB in 4032-byte nodes
        constexpr unsigned AxiRxChannels = 3; // the AXI-DMA's (PARLIO's DMA on the P4)
        constexpr unsigned MaxPads = IoParlioSetup::MaxData + 2;

        struct Port {
                        unsigned      number = 0;
                        char          owner[10] = {}; // `parlio0`: the port, and /dev's name
                        char          key[12] = {};   // `io/parlio0`
                        IoParlioSetup setup;          // as running
                        bool          running = false;
                        IoClaim       claim;
                        SysConf::Id   id = SysConf::NoId;
                        ParlioPort   *stream = nullptr; // made when first started, kept
                        bool          fileAdded = false;
                        // A setting change under way: a waiting reader or writer leaves.
                        std::atomic<bool> leaving{false};
                        // The pads it took (not a monitor's), put back at the stop.
                        int8_t           pads[MaxPads] = {};
                        IoChipOps::Saved saved[MaxPads];
                        unsigned         padCount = 0;
                        // A sender.
                        parlio_tx_unit_handle_t tx = nullptr;
                        uint8_t                *txBuffers = nullptr; // TxPieces × TxPiece, internal DMA RAM
                        uint32_t                txNext = 0;          // the buffer the next write takes
                        std::atomic<uint32_t>   txInFlight{0};
                        std::atomic<uint64_t>   transfers{0};
                        // A receiver.
                        parlio_rx_unit_handle_t      rx = nullptr;
                        parlio_rx_delimiter_handle_t delimiter = nullptr;
                        uint8_t                     *ring = nullptr; // `setup.buffer` bytes, internal DMA RAM
                        uint64_t                     tail = 0;       // the reader's, in bytes since the start
                        // ESP-IDF's descriptors over the ring, found from the DMA channel (their
                        // uncached alias, and the address the DMA knows each by): the nodes it has
                        // handed back are taken by their owner bit, however many one interrupt finds.
                        dma_descriptor_t *nodes[MaxNodes] = {};
                        uint32_t          nodeAddr[MaxNodes] = {};
                        uint32_t          nodeSize[MaxNodes] = {};
                        unsigned          nodeCount = 0;
                        unsigned          cursor = 0;     // the next node the DMA hands back
                        int               rxChannel = -1; // the AXI-DMA channel
                        uint64_t          oddNodes = 0;   // nodes handed back short (not expected)

                        std::atomic<uint64_t> lost{0};
                        int64_t               startNs = 0;
                        // Both: sent, or received (the interrupt's count: a receiver's head).
                        std::atomic<uint64_t> bytes{0};

                        void wakeFromIsr() {
                                stream->_moved.releaseFromIsr();
                                stream->_pollers.notifyFromIsr();
                                return;
                        }
                        void wake() {
                                stream->_moved.release();
                                stream->_pollers.notify();
                                return;
                        }
                        Mutex &streamLock() { return stream->_lock; }

                        void  stop();
                        Error start(const IoParlioSetup &s);
                        void  addFile();
        };

        struct State {
                        SysConf::Watcher watcher;
                        Mutex            lock; // the ports' starts and stops
                        Port             ports[PortCount];
        };

        constinit PsramObject<State> state;

        // The driver wants its callbacks' argument in internal RAM: a pointer to the port there.
        DRAM_ATTR Port *portOf[PortCount] = {};

        bool sentDone(parlio_tx_unit_handle_t, const parlio_tx_done_event_data_t *, void *arg) {
                Port &p = **static_cast<Port **>(arg);
                p.txInFlight.fetch_sub(1, std::memory_order_acq_rel);
                p.transfers.fetch_add(1, std::memory_order_relaxed);
                p.wakeFromIsr();
                return false;
        }

        // ESP-IDF tells of one node an interrupt, and so misses one when an interrupt comes late
        // (two done, one told: devplan/io.md §15.6). Here every node the DMA has handed back is
        // taken, by its owner bit, in order: its lines invalidated, the head moved, the node given
        // back to the DMA.
        bool received(parlio_rx_unit_handle_t, const parlio_rx_event_data_t *, void *arg) {
                Port    &p = **static_cast<Port **>(arg);
                uint64_t moved = 0;
                for (unsigned k = 0; k < p.nodeCount; ++k) {
                        dma_descriptor_t &d = *p.nodes[p.cursor];
                        if (d.dw0.owner != DMA_DESCRIPTOR_BUFFER_OWNER_CPU) break;
                        const uint32_t size = p.nodeSize[p.cursor];
                        if (d.dw0.length != size) ++p.oddNodes;
                        (void)esp_cache_msync(d.buffer, size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                        d.dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
                        moved += size;
                        p.cursor = (p.cursor + 1) % p.nodeCount;
                }
                if (moved != 0) {
                        p.bytes.fetch_add(moved, std::memory_order_release);
                        p.wakeFromIsr();
                }
                return false;
        }

        // The farthest byte the DMA may be writing now: the end of the node its channel holds (it
        // may have prefetched it: an end one node too far, the safe side). The reader's limit.
        uint64_t dmaFrontier(const Port &p) {
                const uint64_t head = p.bytes.load(std::memory_order_acquire);
                const unsigned cursor = p.cursor;
                const uint32_t at = AXI_DMA.in[p.rxChannel].conf.in_dscr.inlink_dscr_chn;
                unsigned       ahead = 0;
                while (ahead < p.nodeCount && p.nodeAddr[(cursor + ahead) % p.nodeCount] != at) ++ahead;
                if (ahead == p.nodeCount) ahead = p.nodeCount - 1; // not one of the ring's: assume the worst
                uint64_t end = head;
                for (unsigned k = 0; k <= ahead; ++k) end += p.nodeSize[(cursor + k) % p.nodeCount];
                return end;
        }

        // The ring's descriptors, from the AXI-DMA channel PARLIO's receiver is connected to: its
        // current descriptor, then their links round to it, put in the buffer's order.
        bool findRing(Port &p) {
                p.rxChannel = -1;
                for (unsigned ch = 0; ch < AxiRxChannels; ++ch)
                        if (AXI_DMA.in[ch].conf.in_peri_sel.peri_in_sel_chn == SOC_GDMA_TRIG_PERIPH_PARLIO0)
                                p.rxChannel = int(ch);
                if (p.rxChannel < 0) return false;
                const uint32_t first = AXI_DMA.in[p.rxChannel].conf.in_dscr.inlink_dscr_chn;
                uint32_t       addr[MaxNodes];
                unsigned       n = 0;
                for (uint32_t a = first; n < MaxNodes; ++n) {
                        addr[n] = a;
                        const auto *d = reinterpret_cast<const dma_descriptor_t *>(a + SOC_NON_CACHEABLE_OFFSET);
                        a = uint32_t(uintptr_t(d->next));
                        if (a == first) {
                                ++n;
                                break;
                        }
                        if (a == 0) return false;
                }
                if (n == 0 || n == MaxNodes) return false;
                // Rotated so the node at the buffer's start is 0, and checked to tile the buffer.
                unsigned zero = n;
                for (unsigned i = 0; i < n; ++i)
                        if (reinterpret_cast<const dma_descriptor_t *>(addr[i] + SOC_NON_CACHEABLE_OFFSET)->buffer ==
                            p.ring)
                                zero = i;
                if (zero == n) return false;
                uint32_t offset = 0;
                for (unsigned i = 0; i < n; ++i) {
                        const uint32_t a = addr[(zero + i) % n];
                        auto          *d = reinterpret_cast<dma_descriptor_t *>(a + SOC_NON_CACHEABLE_OFFSET);
                        if (d->buffer != p.ring + offset) return false;
                        p.nodes[i] = d;
                        p.nodeAddr[i] = a;
                        p.nodeSize[i] = d->dw0.size;
                        offset += d->dw0.size;
                }
                if (offset != p.setup.buffer) return false;
                p.nodeCount = n;
                p.cursor = 0;
                return true;
        }

        // ---- Starting and stopping

        void Port::stop() {
                if (!running) return;
                if (tx != nullptr) {
                        (void)parlio_tx_unit_wait_all_done(tx, 1000);
                        (void)parlio_tx_unit_disable(tx);
                        (void)parlio_del_tx_unit(tx);
                        tx = nullptr;
                }
                if (rx != nullptr) {
                        if (oddNodes != 0)
                                bmdsWarn("io: %s: %llu DMA nodes came back short (the ring's count is off)", owner,
                                         (unsigned long long)oddNodes);
                        (void)parlio_rx_soft_delimiter_start_stop(rx, delimiter, false);
                        (void)parlio_rx_unit_disable(rx);
                        (void)parlio_del_rx_unit(rx);
                        rx = nullptr;
                }
                if (delimiter != nullptr) (void)parlio_del_rx_delimiter(delimiter);
                delimiter = nullptr;
                Memory::free(txBuffers);
                txBuffers = nullptr;
                Memory::free(ring);
                ring = nullptr;
                while (padCount > 0) {
                        --padCount;
                        IoChipOps::restorePad(unsigned(pads[padCount]), saved[padCount]);
                }
                claim.reset();
                running = false;
                FlashWrites::release();
                wake(); // a waiting writer or reader sees it stopped
                bmdsInfo("io: %s stopped", owner);
                return;
        }

        Error Port::start(const IoParlioSetup &s) {
                // The unit (one TX, one RX) and the pads (a monitor's only read: not taken).
                State &st = *state;
                for (const Port &o : st.ports)
                        if (&o != this && o.running && o.setup.receive == s.receive) {
                                bmdsWarn("io: %s not started: the %s unit is %s's", key, s.receive ? "RX" : "TX",
                                         o.key);
                                return Error::Busy;
                        }
                IoUse  uses[MaxPads];
                size_t n = 0;
                if (!s.monitor) {
                        for (size_t i = 0; i < s.dataCount; ++i) uses[n++] = IoUse{IoResource(s.data[i]), "data"};
                        if (s.clk >= 0) uses[n++] = IoUse{IoResource(s.clk), "clock out"};
                        if (s.clkIn >= 0) uses[n++] = IoUse{IoResource(s.clkIn), "clock in"};
                } else if (s.clk >= 0) {
                        uses[n++] = IoUse{IoResource(s.clk), "clock out"};
                }
                // Not the PARLIO group: its TX and RX units are separate (one port each, above).
                IoClaim got; // a monitor without a clock out takes nothing
                if (n > 0) {
                        IoResource blocked = NoIoResource;
                        auto [c, cerr] =
                                Io::registry().claim(StringView(owner), IoOwnerKind::User, Span<const IoUse>(uses, n),
                                                     s.force ? IoRegistry::Force : 0u, &blocked);
                        if (cerr.isError()) {
                                char busy[160];
                                (void)Io::busyText(blocked, busy, sizeof(busy));
                                bmdsWarn("io: %s not started: %s", key, busy);
                                return cerr;
                        }
                        got = std::move(c);
                }
                if (stream == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(ParlioPort));
                        if (mem == nullptr) return Error::NoMem;
                        stream = ::new (mem) ParlioPort(*this);
                }
                // The pads the driver will set up, kept to put back.
                padCount = 0;
                for (size_t i = 0; i < n; ++i) {
                        const IoResource r = uses[i].resource;
                        if (r >= ioChip().padCount()) continue;
                        pads[padCount] = int8_t(r);
                        IoChipOps::savePad(unsigned(r), saved[padCount]);
                        ++padCount;
                }
                setup = s;
                bytes.store(0);
                lost.store(0);
                transfers.store(0);
                txInFlight.store(0);
                txNext = 0;
                tail = 0;
                const int  width = s.width();
                const auto edge = s.rising ? PARLIO_SAMPLE_EDGE_POS : PARLIO_SAMPLE_EDGE_NEG;
                const auto order = s.msb ? PARLIO_BIT_PACK_ORDER_MSB : PARLIO_BIT_PACK_ORDER_LSB;
                esp_err_t  err = ESP_OK;
                if (!s.receive) {
                        txBuffers = static_cast<uint8_t *>(
                                Memory::allocAligned(MemSpace::InternalDma, TxPieces * TxPiece, CacheLine));
                        if (txBuffers == nullptr) err = ESP_ERR_NO_MEM;
                        parlio_tx_unit_config_t c = {};
                        c.clk_src = PARLIO_CLK_SRC_DEFAULT;
                        c.clk_in_gpio_num = gpio_num_t(s.clkIn);
                        c.input_clk_src_freq_hz = s.clkIn >= 0 ? s.clock : 0;
                        c.output_clk_freq_hz = s.clock;
                        c.data_width = size_t(width);
                        for (int i = 0; i < PARLIO_TX_UNIT_MAX_DATA_WIDTH; ++i)
                                c.data_gpio_nums[i] = i < s.dataCount ? gpio_num_t(s.data[i]) : GPIO_NUM_NC;
                        c.clk_out_gpio_num = gpio_num_t(s.clk);
                        c.valid_gpio_num = GPIO_NUM_NC;
                        c.trans_queue_depth = TxPieces;
                        c.max_transfer_size = TxPiece;
                        c.dma_burst_size = 32;
                        c.sample_edge = edge;
                        c.bit_pack_order = order;
                        if (err == ESP_OK) err = parlio_new_tx_unit(&c, &tx);
                        parlio_tx_event_callbacks_t cb = {};
                        cb.on_trans_done = sentDone;
                        if (err == ESP_OK) err = parlio_tx_unit_register_event_callbacks(tx, &cb, &portOf[number]);
                        if (err == ESP_OK) err = parlio_tx_unit_enable(tx);
                        // The lines rest at 0 until a transfer gives them its idle value: a first
                        // short transfer of idle samples sets them there from the start.
                        if (err == ESP_OK && s.idle != 0) {
                                uint32_t word = 0;
                                for (int b = 0; b < 32; b += width) word |= s.idle << b;
                                std::memcpy(txBuffers, &word, 4);
                                std::memcpy(txBuffers + 4, &word, 4);
                                parlio_transmit_config_t t = {};
                                t.idle_value = s.idle;
                                txInFlight.store(1); // its completion counts down as any
                                err = parlio_tx_unit_transmit(tx, txBuffers, 64, &t);
                                if (err == ESP_OK) err = parlio_tx_unit_wait_all_done(tx, 100);
                                txInFlight.store(0);
                                transfers.store(0);
                        }
                } else {
                        ring = static_cast<uint8_t *>(Memory::allocAligned(MemSpace::InternalDma, s.buffer, CacheLine));
                        if (ring == nullptr) err = ESP_ERR_NO_MEM;
                        parlio_rx_unit_config_t c = {};
                        c.trans_queue_depth = 1;
                        c.max_recv_size = s.buffer;
                        c.data_width = size_t(width);
                        c.clk_src = s.clkIn >= 0 ? PARLIO_CLK_SRC_EXTERNAL : PARLIO_CLK_SRC_DEFAULT;
                        c.ext_clk_freq_hz = s.clkIn >= 0 ? s.clock : 0;
                        c.exp_clk_freq_hz = s.clock;
                        c.clk_in_gpio_num = gpio_num_t(s.clkIn);
                        c.clk_out_gpio_num = gpio_num_t(s.clk);
                        c.valid_gpio_num = GPIO_NUM_NC;
                        for (int i = 0; i < PARLIO_RX_UNIT_MAX_DATA_WIDTH; ++i)
                                c.data_gpio_nums[i] = i < s.dataCount ? gpio_num_t(s.data[i]) : GPIO_NUM_NC;
                        if (err == ESP_OK) err = parlio_new_rx_unit(&c, &rx);
                        parlio_rx_soft_delimiter_config_t d = {};
                        d.sample_edge = edge;
                        d.bit_pack_order = order;
                        d.eof_data_len = s.buffer;
                        if (err == ESP_OK) err = parlio_new_rx_soft_delimiter(&d, &delimiter);
                        parlio_rx_event_callbacks_t cb = {};
                        cb.on_partial_receive = received;
                        if (err == ESP_OK) err = parlio_rx_unit_register_event_callbacks(rx, &cb, &portOf[number]);
                        if (err == ESP_OK) err = parlio_rx_unit_enable(rx, true);
                        if (err == ESP_OK) err = parlio_rx_soft_delimiter_start_stop(rx, delimiter, true);
                        parlio_receive_config_t r = {};
                        r.delimiter = delimiter;
                        r.flags.partial_rx_en = 1;
                        nodeCount = 0;
                        oddNodes = 0;
                        if (err == ESP_OK) err = parlio_rx_unit_receive(rx, ring, s.buffer, &r);
                        startNs = Detail::monotonicUs() * 1000;
                        if (err == ESP_OK && !findRing(*this)) {
                                bmdsWarn("io: %s: its DMA ring isn't as ESP-IDF's driver laid it out before", key);
                                err = ESP_ERR_INVALID_STATE;
                        }
                }
                if (err != ESP_OK) {
                        bmdsWarn("io: %s not started: PARLIO (%s)", key, esp_err_to_name(err));
                        running = true; // for stop()'s clean-up
                        FlashWrites::hold();
                        claim = std::move(got);
                        stop();
                        return Error::IoError;
                }
                // The outputs' strength (ESP-IDF's driver leaves its default): a fast clock on a long
                // trace rings less driven gently.
                for (const int8_t pad : {s.clk})
                        if (pad >= 0) (void)gpio_set_drive_capability(gpio_num_t(pad), gpio_drive_cap_t(s.drive));
                if (!s.receive)
                        for (size_t i = 0; i < s.dataCount; ++i)
                                (void)gpio_set_drive_capability(gpio_num_t(s.data[i]), gpio_drive_cap_t(s.drive));
                claim = std::move(got);
                running = true;
                // /flash's writes wait while it runs (devplan/io.md §15.5): no ring covers an erase.
                FlashWrites::hold();
                addFile();
                bmdsInfo("io: %s: /dev/%s, %s %u lines at %u Hz%s", key, owner, s.receive ? "receiving" : "sending",
                         unsigned(width), unsigned(s.clock), s.clkIn >= 0 ? " (external clock)" : "");
                return Error::Ok;
        }

        bool filePresent(void *arg) {
                return static_cast<const ParlioPort *>(arg)->isRunning();
        }

        void Port::addFile() {
                if (fileAdded) return;
                Vfs::DeviceFile file;
                file.name = owner;
                file.arg = stream;
                file.isDevice = true;
                file.present = filePresent;
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        auto         *s = static_cast<ParlioPort *>(arg);
                        const uint8_t mode = ((flags & Vfs::Read) ? FileHandle::Read : 0) |
                                             ((flags & Vfs::Write) ? FileHandle::Write : 0);
                        if (mode == 0) return makeError<FileHandle::Ptr>(Error::Invalid);
                        if (!s->isRunning()) return makeError<FileHandle::Ptr>(Error::NotExist);
                        return FileHandle::open(*s, mode);
                };
                if (const Error err = Vfs::addDeviceFile(file); err.isError())
                        bmdsWarn("io: /dev/%s: %s", owner, err.desc());
                else
                        fileAdded = true;
                return;
        }

        // io/parlioN's check: the value parses, and without `force` (or as a monitor) takes no pin
        // another owner holds.
        template <unsigned N> Error checkParlio(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                if (!Io::isReady()) return Error::NotReady;
                IoParlioSetup s;
                char          why[IoParlioSetup::ReasonBytes];
                if (const Error err = s.parse(ioChip(), value, why); err.isError()) {
                        SysConf::refuse(StringView(why));
                        return err;
                }
                char owner[10];
                std::snprintf(owner, sizeof(owner), "parlio%u", N);
                if (!s.force) {
                        if (!s.monitor)
                                for (size_t i = 0; i < s.dataCount; ++i)
                                        if (IoSetups::takenElsewhere(IoResource(s.data[i]), owner))
                                                return IoSetups::refuseTaken(IoResource(s.data[i]), nullptr);
                        for (const int8_t pad : {s.clk, s.monitor ? int8_t(-1) : s.clkIn})
                                if (pad >= 0 && IoSetups::takenElsewhere(IoResource(pad), owner))
                                        return IoSetups::refuseTaken(IoResource(pad), nullptr);
                }
                return Error::Ok;
        }

        constexpr const char *ParlioHelp = "A PARLIO port, /dev/parlioN while set: 'dir=tx data=46,47,48,16 clock=10m "
                                           "clk=17'; clk-in=, edge=rising|falling, msb; a sender's idle=; a "
                                           "receiver's buffer=, monitor; force (devplan/io.md)";
        BMDS_SYSCONF(ioParlio0, "io/parlio0", Config::Type::String, "", ParlioHelp, 0, 0, nullptr, checkParlio<0>);
        BMDS_SYSCONF(ioParlio1, "io/parlio1", Config::Type::String, "", ParlioHelp, 0, 0, nullptr, checkParlio<1>);

        void apply(Port &p) {
                const GpioChip::Change change; // line watchers told once, after the lock
                Mutex::Locker          lock(state->lock, WaitForever);
                String                 value(MemSpace::Psram);
                if (SysConf::get(p.id, value).isError()) return;
                IoParlioSetup s;
                char          why[IoParlioSetup::ReasonBytes];
                if (!value.isEmpty() && s.parse(ioChip(), value.view(), why).isError()) {
                        bmdsWarn("io: %s: %s: not changed", p.key, why);
                        return;
                }
                if (!value.isEmpty() && p.running && s == p.setup) return;
                // A stream reading or writing it is let go first (the stream's lock): one waiting is
                // woken and leaves, as from a port that went.
                if (p.stream != nullptr) {
                        p.leaving.store(true, std::memory_order_release);
                        p.wake();
                        Mutex::Locker streamLock(p.streamLock(), WaitForever);
                        p.stop();
                }
                if (!value.isEmpty()) (void)p.start(s);
                p.leaving.store(false, std::memory_order_release);
                return;
        }

        void changed(void *) {
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

        // A restart leaves IO_MUX and the matrix as they are (devplan/io.md §10.4).
        void putBackAtRestart() {
                if (!state.isCreated()) return;
                for (Port &p : state->ports)
                        if (p.running)
                                for (unsigned i = p.padCount; i-- > 0;)
                                        IoChipOps::restorePad(unsigned(p.pads[i]), p.saved[i]);
                return;
        }

} // namespace ParlioPorts

// ---- The stream

using ParlioPorts::Port;

bool ParlioPort::isRunning() const {
        return _port.running;
}

Error ParlioPort::info(Info &out) const {
        out = Info();
        out.port = _port.number;
        out.running = _port.running;
        const IoParlioSetup &s = _port.setup;
        out.receive = s.receive;
        out.externalClock = s.clkIn >= 0;
        out.monitor = s.monitor;
        out.width = _port.running ? s.width() : 0;
        out.clockHz = s.clock;
        out.buffer = s.receive ? s.buffer : 0;
        out.startNs = s.receive ? _port.startNs : 0;
        out.bytes = _port.bytes.load(std::memory_order_acquire);
        out.lost = _port.lost.load(std::memory_order_relaxed);
        out.transfers = _port.transfers.load(std::memory_order_relaxed);
        out.idle = s.receive ? 0 : s.idle;
        return Error::Ok;
}

Result<size_t> ParlioPort::write(ByteSpan data, unsigned int timeoutMs) {
        Mutex::Locker lock(_lock, WaitForever);
        Port         &p = _port;
        if (!p.running || p.leaving.load(std::memory_order_acquire)) return makeError<size_t>(Error::NotExist);
        if (p.setup.receive) return makeError<size_t>(Error::Invalid);
        // Whole samples: at 16 lines, two bytes each.
        const size_t sample = p.setup.width() == 16 ? 2 : 1;
        if (data.size() % sample != 0) return makeError<size_t>(Error::Invalid);
        Deadline deadline(timeoutMs);
        size_t   done = 0;
        while (done < data.size()) {
                // A buffer free (transfers end in order, so the next is the oldest done).
                if (p.txInFlight.load(std::memory_order_acquire) >= ParlioPorts::TxPieces) {
                        const Error err = Interrupt::waitCurrent(_moved, deadline);
                        if (err.isError()) {
                                if (done > 0) break;
                                return makeError<size_t>(err);
                        }
                        if (!p.running || p.leaving.load(std::memory_order_acquire))
                                return makeError<size_t>(Error::NotExist);
                        continue;
                }
                const size_t n = data.size() - done < ParlioPorts::TxPiece ? data.size() - done : ParlioPorts::TxPiece;
                uint8_t     *buf = p.txBuffers + p.txNext * ParlioPorts::TxPiece;
                std::memcpy(buf, data.data() + done, n);
                parlio_transmit_config_t t = {};
                t.idle_value = p.setup.idle;
                p.txInFlight.fetch_add(1, std::memory_order_acq_rel);
                if (parlio_tx_unit_transmit(p.tx, buf, n * 8, &t) != ESP_OK) {
                        p.txInFlight.fetch_sub(1, std::memory_order_acq_rel);
                        return done > 0 ? makeResult(done) : makeError<size_t>(Error::IoError);
                }
                p.bytes.fetch_add(n, std::memory_order_relaxed);
                p.txNext = (p.txNext + 1) % ParlioPorts::TxPieces;
                done += n;
        }
        return makeResult(done);
}

Error ParlioPort::flush(unsigned int timeoutMs) {
        Mutex::Locker lock(_lock, WaitForever);
        Port         &p = _port;
        if (!p.running || p.setup.receive) return Error::Ok;
        Deadline deadline(timeoutMs);
        while (p.txInFlight.load(std::memory_order_acquire) > 0) {
                if (const Error err = Interrupt::waitCurrent(_moved, deadline); err.isError()) return err;
                if (!p.running) return Error::NotExist;
        }
        return Error::Ok;
}

Result<size_t> ParlioPort::read(WritableByteSpan buf, unsigned int timeoutMs) {
        Mutex::Locker lock(_lock, WaitForever);
        Port         &p = _port;
        if (!p.running || p.leaving.load(std::memory_order_acquire)) return makeError<size_t>(Error::NotExist);
        if (!p.setup.receive) return makeError<size_t>(Error::Invalid);
        const size_t size = p.setup.buffer;
        Deadline     deadline(timeoutMs);
        for (;;) {
                // What the DMA may be writing over (a ring behind its frontier) is lost: skipped,
                // counted; and if it got there during the copy, the copy is dropped too.
                const uint64_t frontier = ParlioPorts::dmaFrontier(p);
                if (frontier > size && p.tail < frontier - size) {
                        p.lost.fetch_add(frontier - size - p.tail, std::memory_order_relaxed);
                        p.tail = frontier - size;
                }
                const uint64_t head = p.bytes.load(std::memory_order_acquire);
                size_t         n = head > p.tail ? size_t(head - p.tail) : 0;
                if (n > 0) {
                        if (n > buf.size()) n = buf.size();
                        const size_t at = size_t(p.tail % size);
                        const size_t first = n < size - at ? n : size - at;
                        std::memcpy(buf.data(), p.ring + at, first);
                        if (n > first) std::memcpy(buf.data() + first, p.ring, n - first);
                        const uint64_t after = ParlioPorts::dmaFrontier(p);
                        if (after > size && after - size > p.tail) {
                                const uint64_t bad = after - size - p.tail < n ? after - size - p.tail : n;
                                p.lost.fetch_add(bad, std::memory_order_relaxed);
                                p.tail += bad;
                                if (bad < n) {
                                        std::memmove(buf.data(), buf.data() + bad, n - size_t(bad));
                                        n -= size_t(bad);
                                } else {
                                        continue;
                                }
                        }
                        p.tail += n;
                        return makeResult(n);
                }
                const Error err = Interrupt::waitCurrent(_moved, deadline);
                if (err.isError()) return makeError<size_t>(err);
                if (!p.running || p.leaving.load(std::memory_order_acquire)) return makeError<size_t>(Error::NotExist);
        }
}

uint8_t ParlioPort::readiness() {
        const Port &p = _port;
        if (!p.running) return 0;
        if (p.setup.receive) return p.bytes.load(std::memory_order_acquire) != p.tail ? Readable : 0;
        return p.txInFlight.load(std::memory_order_acquire) < ParlioPorts::TxPieces ? Writable : 0;
}

namespace IoSetups {

        Error startParlioPorts() {
                using namespace ParlioPorts;
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                for (unsigned n = 0; n < PortCount; ++n) {
                        Port &p = s->ports[n];
                        p.number = n;
                        portOf[n] = &p;
                        std::snprintf(p.owner, sizeof(p.owner), "parlio%u", n);
                        std::snprintf(p.key, sizeof(p.key), "io/parlio%u", n);
                        auto [id, err] = SysConf::find(StringView(p.key));
                        if (err.isOk()) p.id = id;
                }
                if (System::addRestartHook(putBackAtRestart).isError())
                        bmdsWarn("io: no restart hook: the PARLIO ports' pins will stay set up across a restart");
                for (Port &p : s->ports) apply(p);
                return s->watcher.start(StringView("io"), changed, nullptr);
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
