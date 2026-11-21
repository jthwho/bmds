/**
 * @file      rmtports.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * RMT ports (devplan/io.md §13.1): `io/rmt0` .. `io/rmt7`, checked, started and stopped as
 * they change, `/dev/rmtN` while one runs. Built on ESP-IDF's RMT driver: a sender is one
 * transaction fed from a ring by the simple encoder's callback (the RMT interrupt), so a stream
 * has no gaps; a receiver takes blocks by partial receive into a ring, each run behind a header
 * with the time of its first edge.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <esp_clk_tree.h>
#include <esp_private/rmt.h>
#include <bmds/deadline.h>
#include <bmds/detail/platform.h>
#include <bmds/gpiochip.h>
#include <bmds/interrupt.h>
#include <bmds/io.h>
#include <bmds/ioport.h>
#include <bmds/iosetup.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/psramobject.h>
#include <bmds/rmtchannel.h>
#include <bmds/spinlock.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/system.h>
#include <bmds/vfs.h>

#include "iochipops.h"
#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace RmtPorts {

        constexpr unsigned PortCount = 8;          // io/rmt0 .. io/rmt7
        constexpr size_t   MemSymbols = 48;        // a channel's own memory (SOC_RMT_MEM_WORDS_PER_CHANNEL)
        constexpr size_t   Block = MemSymbols / 2; // a receiver's block: half the memory (its ping-pong)
        constexpr uint32_t FillerUs = 100;         // a continuous sender's idle filler, each half of a symbol

        // A symbol's ticks: both halves, up to a 0 duration (a run's end).
        uint32_t ticksOf(uint32_t s, bool &ended) {
                const uint32_t d0 = s & 0x7fffu, d1 = (s >> 16) & 0x7fffu;
                ended = d0 == 0 || d1 == 0;
                return d0 + d1;
        }

        uint64_t ticksOf(const uint32_t *symbols, size_t n) {
                uint64_t t = 0;
                for (size_t i = 0; i < n; ++i) {
                        bool ended = false;
                        t += ticksOf(symbols[i], ended);
                        if (ended) break;
                }
                return t;
        }

        struct Port {
                        unsigned         number = 0;
                        char             owner[8] = {}; // `rmt0`: the port, and /dev's name
                        char             key[10] = {};  // `io/rmt0`
                        Mutex            lock;          // its start and stop
                        IoRmtSetup       setup;         // as running
                        bool             running = false;
                        IoClaim          claim;
                        RmtChannel      *stream = nullptr; // made when first started, kept
                        bool             fileAdded = false;
                        SysConf::Id      id = SysConf::NoId;
                        uint32_t         sourceHz = 0;
                        IoChipOps::Saved saved;
                        bool             padSaved = false;

                        rmt_channel_handle_t handle = nullptr;
                        rmt_encoder_handle_t encoder = nullptr;
                        int                  channel = -1;

                        // The ring: a sender's symbols (writer → interrupt) or a receiver's words
                        // (interrupt → reader). Counts, not indices: `% capacity`.
                        uint32_t             *ring = nullptr;
                        uint32_t              capacity = 0;
                        std::atomic<uint32_t> head{0};
                        std::atomic<uint32_t> tail{0};

                        // A sender.
                        SpinLock              txLock;           // the transaction's end against a write
                        bool                  txOn = false;     // a transaction runs
                        bool                  txEnding = false; // the encoder said done
                        bool                  stopping = false;
                        bool                  dry = false;     // in an underrun now
                        uint32_t              filler = 0;      // a filler symbol
                        uint32_t              fillerTicks = 0; // its ticks
                        int64_t               startNs = 0;
                        std::atomic<uint64_t> sent{0};
                        std::atomic<uint64_t> ticks{0};
                        std::atomic<uint64_t> fillerTotal{0};
                        std::atomic<uint32_t> underruns{0};

                        // A receiver.
                        rmt_receive_config_t  receiveConfig = {};
                        uint32_t              block[Block] = {};
                        uint32_t              held[Block] = {};
                        size_t                heldCount = 0;
                        bool                  runActive = false;
                        bool                  firstPending = false;
                        bool                  needHeader = false;
                        int64_t               prevNs = 0;
                        std::atomic<uint64_t> received{0};
                        std::atomic<uint32_t> overruns{0};

                        int64_t ticksNs(uint64_t t) const { return int64_t(t * 1000000000ull / setup.resolution); }

                        // The stream's writer or reader, and its pollers, woken.
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
                        Mutex   &streamLock() { return stream->_lock; }
                        uint32_t room() const { return capacity - (head.load() - tail.load()); }

                        Error start(const IoRmtSetup &s);
                        void  stop();
                        void  addFile();
                        Error startTransaction();

                        // Receiving, in the interrupt: words into the ring, all or none.
                        bool put(const uint32_t *words, size_t n, const uint32_t *more, size_t m, bool header,
                                 int64_t timeNs) {
                                const size_t need = n + m + (header ? 3 : 0);
                                if (need > room()) return false;
                                uint32_t   h = head.load(std::memory_order_relaxed);
                                const auto push = [&](uint32_t w) {
                                        ring[h++ % capacity] = w;
                                };
                                if (header) {
                                        push(RmtChannel::HeaderWord);
                                        push(uint32_t(uint64_t(timeNs)));
                                        push(uint32_t(uint64_t(timeNs) >> 32));
                                }
                                for (size_t i = 0; i < n; ++i) push(words[i]);
                                for (size_t i = 0; i < m; ++i) push(more[i]);
                                head.store(h, std::memory_order_release);
                                received.fetch_add(n + m, std::memory_order_relaxed);
                                return true;
                        }
        };

        struct State {
                        SysConf::Watcher watcher;
                        Port             ports[PortCount];
        };

        constinit PsramObject<State> state;

        // ---- A sender: the encoder's callback, in the RMT interrupt

        size_t encode(const void *, size_t, size_t, size_t symbolsFree, rmt_symbol_word_t *symbols, bool *done,
                      void *arg) {
                Port          &p = *static_cast<Port *>(arg);
                const uint32_t t = p.tail.load(std::memory_order_relaxed);
                const uint32_t have = p.head.load(std::memory_order_acquire) - t;
                const size_t   take = have < symbolsFree ? have : symbolsFree;
                if (take > 0) {
                        uint64_t ticks = 0;
                        for (size_t i = 0; i < take; ++i) {
                                const uint32_t s = p.ring[(t + i) % p.capacity];
                                std::memcpy(&symbols[i], &s, sizeof(s));
                                bool ended = false;
                                ticks += ticksOf(s, ended);
                        }
                        p.tail.store(t + uint32_t(take), std::memory_order_release);
                        p.sent.fetch_add(take, std::memory_order_relaxed);
                        p.ticks.fetch_add(ticks, std::memory_order_relaxed);
                        p.dry = false;
                        p.wakeFromIsr();
                        return take;
                }
                // Dry. A burst (or a stop) ends here, against a write that may just have come.
                p.txLock.lockFromIsr();
                const bool empty = p.head.load(std::memory_order_acquire) == t;
                const bool end = empty && (!p.setup.continuous || p.stopping);
                if (end) p.txEnding = true;
                p.txLock.unlockFromIsr();
                if (end) {
                        *done = true;
                        return 0;
                }
                if (!empty) return 0; // a write landed meanwhile: the next call takes it
                // Continuous: idle filler, counted, so the stream's timing never slips.
                std::memcpy(&symbols[0], &p.filler, sizeof(p.filler));
                p.ticks.fetch_add(p.fillerTicks, std::memory_order_relaxed);
                p.fillerTotal.fetch_add(p.fillerTicks, std::memory_order_relaxed);
                if (!p.dry) p.underruns.fetch_add(1, std::memory_order_relaxed);
                p.dry = true;
                return 1;
        }

        bool sendDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *arg) {
                Port &p = *static_cast<Port *>(arg);
                p.txLock.lockFromIsr();
                p.txOn = false;
                p.txLock.unlockFromIsr();
                p.wakeFromIsr();
                return false;
        }

        // ---- A receiver: blocks, in the RMT interrupt

        bool receiveDone(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *e, void *arg) {
                Port         &p = *static_cast<Port *>(arg);
                const int64_t now = Detail::monotonicUs() * 1000;
                const auto   *c = reinterpret_cast<const uint32_t *>(e->received_symbols);
                const size_t  n = e->num_symbols < Block ? e->num_symbols : Block;
                const bool    last = e->flags.is_last != 0;
                const int64_t cNs = p.ticksNs(ticksOf(c, n));
                // When the block's first edge was: its end is the last callback's time (the copy
                // that followed it was this block), or, at a run's end, now less the quiet time.
                const auto startOf = [&]() {
                        return last ? now - p.ticksNs(p.setup.maxTicks()) - cNs : p.prevNs - cNs;
                };
                if (!p.runActive) {
                        p.runActive = true;
                        if (last) {
                                if (!p.put(c, n, nullptr, 0, true, now - p.ticksNs(p.setup.maxTicks()) - cNs))
                                        p.overruns.fetch_add(1, std::memory_order_relaxed);
                        } else {
                                // The run's first block: its time comes with the next.
                                std::memcpy(p.held, c, n * sizeof(uint32_t));
                                p.heldCount = n;
                                p.firstPending = true;
                        }
                } else if (p.firstPending) {
                        const int64_t cStart = startOf();
                        const int64_t hStart = cStart - p.ticksNs(ticksOf(p.held, p.heldCount));
                        if (!p.put(p.held, p.heldCount, c, n, true, hStart)) {
                                p.overruns.fetch_add(1, std::memory_order_relaxed);
                                p.needHeader = true;
                        }
                        p.firstPending = false;
                } else if (!p.put(c, n, nullptr, 0, p.needHeader, startOf())) {
                        p.overruns.fetch_add(1, std::memory_order_relaxed);
                        p.needHeader = true;
                } else {
                        p.needHeader = false;
                }
                p.prevNs = now;
                if (last) {
                        // The input went quiet: the next edge starts a new run, with its own header.
                        p.runActive = false;
                        p.firstPending = false;
                        p.needHeader = false;
                        (void)rmt_receive(channel, p.block, sizeof(p.block), &p.receiveConfig);
                }
                p.wakeFromIsr();
                return false;
        }

        // ---- Starting and stopping

        Error Port::startTransaction() {
                rmt_transmit_config_t t = {};
                t.loop_count = 0;
                t.flags.eot_level = setup.idleHigh ? 1 : 0;
                txLock.lock();
                txOn = true;
                txEnding = false;
                txLock.unlock();
                ticks.store(0);
                fillerTotal.store(0);
                // The encoder ignores its data (it reads the ring); ESP-IDF keeps the pointer.
                static constexpr uint32_t One = 1;
                if (rmt_transmit(handle, encoder, &One, sizeof(One), &t) != ESP_OK) {
                        txLock.lock();
                        txOn = false;
                        txLock.unlock();
                        return Error::IoError;
                }
                // The hardware starts at the end of rmt_transmit(), after the encoder has filled
                // the channel's memory (about 73 µs): the stream's start is when it returns.
                startNs = Detail::monotonicUs() * 1000;
                return Error::Ok;
        }

        void Port::stop() {
                if (!running) return;
                if (!setup.receive) {
                        // What was written goes out, then the transaction ends (a continuous one at
                        // its next dry spell).
                        stopping = true;
                        (void)rmt_tx_wait_all_done(handle, 2000);
                }
                (void)rmt_disable(handle);
                if (encoder != nullptr) rmt_del_encoder(encoder);
                (void)rmt_del_channel(handle);
                handle = nullptr;
                encoder = nullptr;
                channel = -1;
                if (padSaved) IoChipOps::restorePad(unsigned(setup.pin), saved);
                padSaved = false;
                claim.reset();
                running = false;
                txOn = false;
                wake(); // a waiting writer or reader sees it stopped
                bmdsInfo("io: %s stopped", owner);
                return;
        }

        Error Port::start(const IoRmtSetup &s) {
                // The pad (a monitor's only read: not taken), then the channel ESP-IDF picks.
                IoClaim got;
                if (!s.monitor) {
                        IoResource  blocked = NoIoResource;
                        const IoUse use{IoResource(s.pin), s.receive ? "RX" : "TX"};
                        auto [c, cerr] =
                                Io::registry().claim(StringView(owner), IoOwnerKind::User, Span<const IoUse>(&use, 1),
                                                     s.force ? IoRegistry::Force : 0u, &blocked);
                        if (cerr.isError()) {
                                char busy[160];
                                (void)Io::busyText(blocked, busy, sizeof(busy));
                                bmdsWarn("io: %s not started: %s", key, busy);
                                return cerr;
                        }
                        got = std::move(c);
                }
                if (s.monitor) {
                        IoPadState pad;
                        IoChipOps::readPad(unsigned(s.pin), pad);
                        if (pad.function != ioChip().gpioFunction) {
                                bmdsWarn("io: %s not started: GPIO%d runs an IO_MUX function; monitor reads through "
                                         "the matrix",
                                         key, int(s.pin));
                                return Error::Busy;
                        }
                }
                if (stream == nullptr) {
                        void *mem = Memory::alloc(MemSpace::Psram, sizeof(RmtChannel));
                        if (mem == nullptr) return Error::NoMem;
                        stream = ::new (mem) RmtChannel(*this);
                }
                ring = static_cast<uint32_t *>(Memory::alloc(MemSpace::Psram, s.buffer * sizeof(uint32_t)));
                if (ring == nullptr) {
                        bmdsWarn("io: %s not started: no memory for its ring", key);
                        return Error::NoMem;
                }
                capacity = s.buffer;
                head.store(0);
                tail.store(0);
                setup = s;
                if (!s.monitor) {
                        IoChipOps::savePad(unsigned(s.pin), saved);
                        padSaved = true;
                }
                (void)esp_clk_tree_src_get_freq_hz(soc_module_clk_t(RMT_CLK_SRC_DEFAULT),
                                                   ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &sourceHz);
                esp_err_t err = ESP_OK;
                if (!s.receive) {
                        rmt_tx_channel_config_t c = {};
                        c.gpio_num = gpio_num_t(s.pin);
                        c.clk_src = RMT_CLK_SRC_DEFAULT;
                        c.resolution_hz = s.resolution;
                        c.mem_block_symbols = MemSymbols;
                        c.trans_queue_depth = 1;
                        c.flags.invert_out = s.invert;
                        c.flags.init_level = s.idleHigh;
                        err = rmt_new_tx_channel(&c, &handle);
                        rmt_simple_encoder_config_t e = {};
                        e.callback = encode;
                        e.arg = this;
                        e.min_chunk_size = 1; // a symbol at least, always: the ring's, a filler, or done
                        if (err == ESP_OK) err = rmt_new_simple_encoder(&e, &encoder);
                        rmt_tx_event_callbacks_t cb = {};
                        cb.on_trans_done = sendDone;
                        if (err == ESP_OK) err = rmt_tx_register_event_callbacks(handle, &cb, this);
                        uint32_t f = s.resolution / (1000000 / FillerUs);
                        f = f == 0 ? 1 : f > IoRmtSetup::MaxTicks ? IoRmtSetup::MaxTicks : f;
                        const uint32_t level = s.idleHigh ? 1u : 0u;
                        filler = f | level << 15 | f << 16 | level << 31;
                        fillerTicks = 2 * f;
                        stopping = false;
                        txOn = false;
                        sent.store(0);
                        underruns.store(0);
                        startNs = 0;
                } else {
                        rmt_rx_channel_config_t c = {};
                        c.gpio_num = gpio_num_t(s.pin);
                        c.clk_src = RMT_CLK_SRC_DEFAULT;
                        c.resolution_hz = s.resolution;
                        c.mem_block_symbols = MemSymbols;
                        c.flags.invert_in = s.invert;
                        err = rmt_new_rx_channel(&c, &handle);
                        rmt_rx_event_callbacks_t cb = {};
                        cb.on_recv_done = receiveDone;
                        if (err == ESP_OK) err = rmt_rx_register_event_callbacks(handle, &cb, this);
                        receiveConfig = {};
                        receiveConfig.signal_range_min_ns = s.minNs;
                        receiveConfig.signal_range_max_ns =
                                uint32_t(uint64_t(s.maxTicks()) * 1000000000ull / s.resolution);
                        receiveConfig.flags.en_partial_rx = 1;
                        runActive = firstPending = needHeader = false;
                        received.store(0);
                        overruns.store(0);
                }
                if (err == ESP_OK) err = rmt_get_channel_id(handle, &channel);
                // The hardware channel it got, claimed too (for a monitor, its only claim).
                IoResource ch = Io::instance(IoKind::Rmt, unsigned(channel));
                if (err == ESP_OK && ch != NoIoResource) {
                        const IoUse chUse{ch, s.receive ? "receiver" : "sender"};
                        if (s.monitor) {
                                auto [c2, e2] = Io::registry().claim(StringView(owner), IoOwnerKind::User,
                                                                     Span<const IoUse>(&chUse, 1), 0u, nullptr);
                                if (e2.isOk()) got = std::move(c2);
                        } else {
                                (void)got.add(Span<const IoUse>(&chUse, 1), 0u, nullptr);
                        }
                }
                if (err == ESP_OK) err = rmt_enable(handle);
                if (err == ESP_OK && s.receive) err = rmt_receive(handle, block, sizeof(block), &receiveConfig);
                if (err != ESP_OK) {
                        bmdsWarn("io: %s not started: the RMT (%s)", key, esp_err_to_name(err));
                        if (encoder != nullptr) rmt_del_encoder(encoder);
                        if (handle != nullptr) {
                                (void)rmt_disable(handle);
                                (void)rmt_del_channel(handle);
                        }
                        handle = nullptr;
                        encoder = nullptr;
                        if (padSaved) IoChipOps::restorePad(unsigned(s.pin), saved);
                        padSaved = false;
                        Memory::free(ring);
                        ring = nullptr;
                        return Error::IoError;
                }
                claim = std::move(got);
                running = true;
                addFile();
                bmdsInfo("io: %s: /dev/%s on GPIO%d, %s on channel %d at %u ticks a second", key, owner, int(s.pin),
                         s.receive ? (s.monitor ? "receiving (monitor)" : "receiving") : "sending", channel,
                         unsigned(s.resolution));
                return Error::Ok;
        }

        bool filePresent(void *arg) {
                return static_cast<const RmtChannel *>(arg)->isRunning();
        }

        void Port::addFile() {
                if (fileAdded) return;
                Vfs::DeviceFile file;
                file.name = owner;
                file.arg = stream;
                file.isDevice = true;
                file.present = filePresent;
                file.open = [](void *arg, uint8_t flags) -> Result<FileHandle::Ptr> {
                        auto         *s = static_cast<RmtChannel *>(arg);
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

        // io/rmtN's check: the value parses, and without `force` takes no pin another owner holds
        // or the board wires (a monitor takes none).
        template <unsigned N> Error checkRmt(StringView value) {
                if (value.isEmpty()) return Error::Ok;
                if (!Io::isReady()) return Error::NotReady;
                IoRmtSetup s;
                char       why[IoRmtSetup::ReasonBytes];
                if (const Error err = s.parse(ioChip(), value, why); err.isError()) {
                        SysConf::refuse(StringView(why));
                        return err;
                }
                char owner[8];
                std::snprintf(owner, sizeof(owner), "rmt%u", N);
                if (!s.force && !s.monitor && IoSetups::takenElsewhere(IoResource(s.pin), owner))
                        return IoSetups::refuseTaken(IoResource(s.pin), nullptr);
                return Error::Ok;
        }

        constexpr const char *RmtHelp = "An RMT port, /dev/rmtN while set: 'pin=46 dir=tx resolution=1m'; idle=, "
                                        "invert, continuous, buffer=; a receiver's min=, max=, monitor; force "
                                        "(devplan/io.md)";
        BMDS_SYSCONF(ioRmt0, "io/rmt0", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<0>);
        BMDS_SYSCONF(ioRmt1, "io/rmt1", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<1>);
        BMDS_SYSCONF(ioRmt2, "io/rmt2", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<2>);
        BMDS_SYSCONF(ioRmt3, "io/rmt3", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<3>);
        BMDS_SYSCONF(ioRmt4, "io/rmt4", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<4>);
        BMDS_SYSCONF(ioRmt5, "io/rmt5", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<5>);
        BMDS_SYSCONF(ioRmt6, "io/rmt6", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<6>);
        BMDS_SYSCONF(ioRmt7, "io/rmt7", Config::Type::String, "", RmtHelp, 0, 0, nullptr, checkRmt<7>);

        void apply(Port &p) {
                const GpioChip::Change change; // line watchers told once, after the lock
                Mutex::Locker          lock(p.lock, WaitForever);
                String                 value(MemSpace::Psram);
                if (SysConf::get(p.id, value).isError()) return;
                IoRmtSetup s;
                char       why[IoRmtSetup::ReasonBytes];
                if (!value.isEmpty() && s.parse(ioChip(), value.view(), why).isError()) {
                        bmdsWarn("io: %s: %s: not changed", p.key, why);
                        return;
                }
                if (!value.isEmpty() && p.running && s == p.setup) return;
                // A stream reading or writing it is let go first (the stream's lock).
                if (p.stream != nullptr) {
                        Mutex::Locker streamLock(p.streamLock(), WaitForever);
                        p.stop();
                        if (p.ring != nullptr) Memory::free(p.ring);
                        p.ring = nullptr;
                }
                if (!value.isEmpty()) (void)p.start(s);
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
                        if (p.running && p.padSaved) IoChipOps::restorePad(unsigned(p.setup.pin), p.saved);
                return;
        }

} // namespace RmtPorts

// ---- The stream

using RmtPorts::Port;

bool RmtChannel::isRunning() const {
        return _port.running;
}

RmtChannel *RmtChannel::find(unsigned port) {
        if (!RmtPorts::state.isCreated() || port >= RmtPorts::PortCount) return nullptr;
        return RmtPorts::state->ports[port].stream;
}

Error RmtChannel::info(Info &out) const {
        const Port &p = _port;
        out = Info();
        out.port = p.number;
        out.running = p.running;
        if (!p.running) return Error::Ok;
        out.channel = p.channel;
        out.receive = p.setup.receive;
        out.continuous = p.setup.continuous;
        out.idleHigh = p.setup.idleHigh;
        out.resolution = p.setup.resolution;
        out.sourceHz = p.sourceHz;
        out.buffer = p.capacity;
        if (p.setup.receive) {
                out.symbols = p.received.load();
                out.overruns = p.overruns.load();
        } else {
                out.sending = p.txOn;
                out.startNs = p.startNs;
                out.symbols = p.sent.load();
                out.ticks = p.ticks.load();
                out.fillerTicks = p.fillerTotal.load();
                out.underruns = p.underruns.load();
        }
        return Error::Ok;
}

Result<size_t> RmtChannel::write(ByteSpan data, unsigned int timeoutMs) {
        if (data.size() % sizeof(uint32_t) != 0) return makeError<size_t>(Error::Invalid);
        Mutex::Locker lock(_lock, WaitForever);
        Port         &p = _port;
        if (!p.running) return makeError<size_t>(Error::NotExist);
        if (p.setup.receive) return makeError<size_t>(Error::NotSupported);
        Deadline     deadline(timeoutMs);
        const size_t total = data.size() / sizeof(uint32_t);
        size_t       done = 0;
        while (done < total) {
                const uint32_t room = p.room();
                const size_t   n = room < total - done ? room : total - done;
                if (n > 0) {
                        uint32_t h = p.head.load(std::memory_order_relaxed);
                        for (size_t i = 0; i < n; ++i) {
                                uint32_t s;
                                std::memcpy(&s, data.data() + (done + i) * sizeof(uint32_t), sizeof(s));
                                p.ring[h++ % p.capacity] = s;
                        }
                        p.head.store(h, std::memory_order_release);
                        done += n;
                }
                // Sending, or started: a burst that ended (or is ending) starts again once it has.
                p.txLock.lock();
                const bool start = !p.txOn || p.txEnding;
                p.txLock.unlock();
                if (start) {
                        (void)rmt_tx_wait_all_done(p.handle, -1);
                        if (p.startTransaction().isError()) return makeError<size_t>(Error::IoError);
                }
                if (done == total) break;
                // Full: wait for the interrupt to make room.
                const Error err = Interrupt::waitCurrent(_moved, deadline);
                if (err.isError()) {
                        if (done > 0) break;
                        return makeError<size_t>(err);
                }
                if (!p.running) return makeError<size_t>(Error::NotExist);
        }
        return makeResult(done * sizeof(uint32_t));
}

Result<size_t> RmtChannel::read(WritableByteSpan buf, unsigned int timeoutMs) {
        if (buf.size() < sizeof(uint32_t)) return makeError<size_t>(Error::Invalid);
        Mutex::Locker lock(_lock, WaitForever);
        Port         &p = _port;
        if (!p.running) return makeError<size_t>(Error::NotExist);
        if (!p.setup.receive) return makeError<size_t>(Error::NotSupported);
        Deadline     deadline(timeoutMs);
        const size_t room = buf.size() / sizeof(uint32_t);
        for (;;) {
                uint32_t       t = p.tail.load(std::memory_order_relaxed);
                const uint32_t h = p.head.load(std::memory_order_acquire);
                size_t         n = 0;
                while (t != h && n < room) {
                        const uint32_t w = p.ring[t % p.capacity];
                        // A header goes whole (its word 0 and its time's two words).
                        if (w == HeaderWord) {
                                if (room - n < 3) break;
                                for (int k = 0; k < 3; ++k) {
                                        const uint32_t x = p.ring[t++ % p.capacity];
                                        std::memcpy(buf.data() + n++ * sizeof(uint32_t), &x, sizeof(x));
                                }
                                continue;
                        }
                        std::memcpy(buf.data() + n++ * sizeof(uint32_t), &w, sizeof(w));
                        ++t;
                }
                p.tail.store(t, std::memory_order_release);
                if (n > 0) return makeResult(n * sizeof(uint32_t));
                if (t != h) return makeError<size_t>(Error::Invalid); // room for less than a header
                const Error err = Interrupt::waitCurrent(_moved, deadline);
                if (err.isError()) return makeError<size_t>(err);
                if (!p.running) return makeError<size_t>(Error::NotExist);
        }
}

uint8_t RmtChannel::readiness() {
        const Port &p = _port;
        if (!p.running) return Readable | Writable; // a call says it stopped
        if (p.setup.receive) return p.head.load() != p.tail.load() ? Readable : 0;
        return p.room() > 0 ? Writable : 0;
}

namespace IoSetups {

        Error startRmtPorts() {
                using namespace RmtPorts;
                if (state.isCreated()) return Error::Ok;
                State *s = state.create();
                if (s == nullptr) return Error::NoMem;
                for (unsigned n = 0; n < PortCount; ++n) {
                        Port &p = s->ports[n];
                        p.number = n;
                        std::snprintf(p.owner, sizeof(p.owner), "rmt%u", n);
                        std::snprintf(p.key, sizeof(p.key), "io/rmt%u", n);
                        auto [id, err] = SysConf::find(StringView(p.key));
                        if (err.isOk()) p.id = id;
                }
                if (System::addRestartHook(putBackAtRestart).isError())
                        bmdsWarn("io: no restart hook: the RMT ports' pins will stay set up across a restart");
                for (Port &p : s->ports) apply(p);
                return s->watcher.start(StringView("io"), changed, nullptr);
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
