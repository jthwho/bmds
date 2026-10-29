/**
 * @file      display.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <sdkconfig.h>
#include <bmds/board.h>
#include <bmds/display.h>
#include <bmds/i2cbus.h>
#include <bmds/log.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<Display> displays[Display::MaxDisplays];

        // The monitor's EDID on the DDC bus (the bridge's I2C).
        constexpr uint8_t      EdidAddress = 0x50;
        constexpr uint32_t     DdcHz = 100'000; // DDC's standard clock
        constexpr unsigned int DdcTimeoutMs = 100;

        // The pixel clocks the DPI clock divides from (its PLLs), kHz.
        constexpr uint32_t Plls[] = {240'000, 160'000};

        // The display's counters (devplan/sysmon.md §6): names, units and labels are an interface
        // once shipped.
        using Stat::Health;
        using Stat::Kind;
        using Stat::Rule;
        using Stat::Unit;

        enum StatIndex : size_t {
                StatRunning,
                StatWidth,
                StatHeight,
                StatPixelHz,
                StatFrames,
                StatBlocks,
                StatBlocksPerFrame,
                StatSwaps,
                StatStalls,
                StatUnderruns,
                StatResyncs,
                StatRebuilds,
                StatFailures,
                StatIsrUs,
                StatIsrCyclesMax,
                StatDmaItem,
                StatBridgeFifo,
                StatPresents,
                StatShown,
                StatForwards,
                StatForwardPixels,
                StatForwardUs,
                StatForwardUsMax,
                StatWaitUs,
                StatWaitUsMax,
                StatSwitches,
                StatCount
        };

        constexpr Stat::Desc DisplayStats[] = {
                {"running", Unit::Count, Kind::State, 0, {}, "no|yes", "Whether the display is scanning out"},
                {"width", Unit::Count, Kind::Info, 0, {}, "", "The picture's width in pixels"},
                {"height", Unit::Count, Kind::Info, 0, {}, "", "The picture's height in pixels"},
                {"pixel_hz", Unit::Hz, Kind::Info, 0, {}, "", "The pixel clock as set"},
                {"scanout.frames",
                 Unit::Count,
                 Kind::Total,
                 Stat::KeepHistory,
                 {},
                 "",
                 "Frames begun (a rate: the refresh rate)"},
                {"scanout.blocks", Unit::Count, Kind::Total, 0, {}, "", "Blocks the DMA sent"},
                {"scanout.blocks_per_frame", Unit::Count, Kind::Info, 0, {}, "", "A frame's blocks"},
                {"scanout.swaps", Unit::Count, Kind::Total, 0, {}, "", "Double-buffer swaps made"},
                {"scanout.stalls",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Critical},
                 "",
                 "The DMA met an invalid descriptor"},
                {"scanout.underruns",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Critical},
                 "",
                 "The DSI bridge ran dry (the picture broke up)"},
                {"scanout.resyncs",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Restarts of the stream"},
                {"scanout.rebuilds",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Warning},
                 "",
                 "Rebuilds of the whole DSI path (a stuck stream's)"},
                {"scanout.failures",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {Rule::Zero, Health::Critical},
                 "",
                 "Restarts that failed"},
                {"scanout.isr_us",
                 Unit::Micros,
                 Kind::Total,
                 Stat::KeepHistory,
                 {},
                 "",
                 "Time in the block interrupt (charged to whichever task it interrupted)"},
                {"scanout.isr_cycles_max", Unit::Cycles, Kind::Peak, 0, {}, "", "The slowest block interrupt"},
                {"scanout.dma_item", Unit::Count, Kind::Level, 0, {}, "", "The block the DMA is on"},
                {"scanout.bridge_fifo", Unit::Count, Kind::Level, 0, {}, "", "The DSI bridge's FIFO depth"},
                {"surface.presents", Unit::Count, Kind::Total, Stat::KeepHistory, {}, "", "Presents asked for"},
                {"surface.shown", Unit::Count, Kind::Total, 0, {}, "", "Presents shown"},
                {"surface.forwards",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Rectangles copied into the other buffer after swaps"},
                {"surface.forward_pixels", Unit::Count, Kind::Total, 0, {}, "", "Their pixels"},
                {"surface.forward_us", Unit::Micros, Kind::Total, 0, {}, "", "Time copying them"},
                {"surface.forward_us_max",
                 Unit::Micros,
                 Kind::Peak,
                 Stat::Adjusted,
                 {Rule::Above, Health::Warning},
                 "",
                 "The longest copy (judged against two frames and four)"},
                {"surface.wait_us", Unit::Micros, Kind::Total, 0, {}, "", "Drawing waiting for a pending swap"},
                {"surface.wait_us_max",
                 Unit::Micros,
                 Kind::Peak,
                 Stat::Adjusted,
                 {Rule::Above, Health::Warning},
                 "",
                 "The longest wait (judged against two frames and four)"},
                {"scanout.switches",
                 Unit::Count,
                 Kind::Total,
                 0,
                 {},
                 "",
                 "Pictures switched to (a full screen, back to the desktop): the link rebuilt for each"},
        };
        static_assert(sizeof(DisplayStats) / sizeof(DisplayStats[0]) == StatCount);

} // namespace

Result<Display *> Display::display(size_t number) {
        const BoardDisplay &d = board().display;
        if (number >= MaxDisplays || d.bridge != BoardDisplayBridge::Lt8912b)
                return makeError<Display *>(Error::NotSupported);
        StaticObject<Display> &slot = displays[number];
        if (!slot.isCreated()) {
                I2cBus *bus = I2cBus::bus(static_cast<size_t>(d.i2c));
                if (bus == nullptr) return makeError<Display *>(Error::NotExist);
                slot.create(*bus, d.i2cAddr, static_cast<uint8_t>(number));
                slot->_stats.setName("display", int(number));
                const Error err = Stats::add(slot->_stats);
                if (err.isError()) bmdsWarn("display%u's counters aren't shown: %s", unsigned(number), err.desc());
        }
        if (!slot->_attached) { // tried again until the bridge answers
                const Error err = slot->_bridge.attach();
                if (err.isError()) return makeError<Display *>(err);
                slot->_attached = true;
        }
        return makeResult(&*slot);
}

Display::Display(I2cBus &bus, uint8_t bridgeAddress, uint8_t number)
    : _bridge(bus, bridgeAddress), _scanout(0), _number(number),
      _stats(DisplayStats, StatCount, readStats, this, adjustStats) {}

void Display::readStats(void *arg, Span<int64_t> v) {
        Display             &d = *static_cast<Display *>(arg);
        const Scanout::Stats o = d._scanout.stats();
        const Surface::Stats f = d._surface.stats();
        v[StatRunning] = d.isRunning() ? 1 : 0;
        v[StatWidth] = d._surface.width();
        v[StatHeight] = d._surface.height();
        v[StatPixelHz] = o.pixelHz;
        v[StatFrames] = o.frames;
        v[StatBlocks] = o.blocks;
        v[StatBlocksPerFrame] = o.blocksPerFrame;
        v[StatSwaps] = o.swaps;
        v[StatStalls] = o.stalls;
        v[StatUnderruns] = o.underruns;
        v[StatResyncs] = o.resyncs;
        v[StatRebuilds] = o.rebuilds;
        v[StatSwitches] = o.switches;
        v[StatFailures] = o.failures;
        // The interrupt's 32-bit cycle count widened here (it wraps in 12 s of interrupts alone).
        if (o.isrCycles < d._isrCyclesSeen) d._isrCyclesSeen = 0; // a restart cleared it
        d._isrCyclesTotal += o.isrCycles - d._isrCyclesSeen;
        d._isrCyclesSeen = o.isrCycles;
        v[StatIsrUs] = int64_t(d._isrCyclesTotal / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ); // no frequency scaling
        v[StatIsrCyclesMax] = o.isrCyclesMax;
        v[StatDmaItem] = o.dmaItem;
        v[StatBridgeFifo] = o.bridgeFifo;
        v[StatPresents] = f.presents;
        v[StatShown] = d._surface.shownPresents();
        v[StatForwards] = f.forwards;
        v[StatForwardPixels] = int64_t(f.forwardPixels);
        v[StatForwardUs] = int64_t(f.forwardUs);
        v[StatForwardUsMax] = f.forwardUsMax;
        v[StatWaitUs] = int64_t(f.waitUs);
        v[StatWaitUsMax] = f.waitUsMax;
        return;
}

void Display::adjustStats(void *arg, size_t index, Stat::Limits &limits) {
        if (index != StatForwardUsMax && index != StatWaitUsMax) return;
        const VideoTiming &t = static_cast<Display *>(arg)->_timing;
        const uint64_t     pixels = uint64_t(t.hTotal()) * t.vTotal();
        const int64_t      frameUs = t.pixelKhz == 0 ? 0 : int64_t(pixels * 1000 / t.pixelKhz);
        // A whole screen copied or waited for takes about a frame and a half: two frames warn.
        limits.warn = frameUs == 0 ? Stat::NoLevel : 2 * frameUs;
        limits.crit = frameUs == 0 ? Stat::NoLevel : 4 * frameUs;
        return;
}

Result<VideoTiming> Display::fitMode(const VideoTiming &mode) {
        return fitMode(mode, 0, 0);
}

Result<VideoTiming> Display::fitMode(const VideoTiming &mode, uint8_t lanes, uint32_t minKhz) {
        // The link carries 24 bits a pixel, 1.5 Gbps a lane at most.
        const uint32_t maxKhz = (lanes != 0 ? lanes : board().display.dsiLanes) * 1'500'000u / 24u;
        auto [fitted, err] = VideoModes::fitToClock(mode, Plls, sizeof(Plls) / sizeof(Plls[0]), minKhz, maxKhz);
        if (err.isError()) return makeError<VideoTiming>(Error::NotSupported);
        return makeResult(fitted);
}

Error Display::start(const VideoTiming &mode, Surface::Buffering buffering, uint32_t laneMbps) {
        stop();
        auto [timing, ferr] = fitMode(mode);
        if (ferr.isError()) return ferr;
        const BoardDisplay &d = board().display;
        Scanout::Config     c;
        c.timing = timing;
        c.lanes = d.dsiLanes;
        c.phyLdo = d.phyLdo;
        c.phyLdoMv = d.phyLdoMv;
        c.onRestart = onLinkRestart;
        c.onRestartArg = this;
        c.laneMbps = laneMbps;
        _timing = timing;
        Error err = _surface.start(timing.hActive, timing.vActive, buffering, Memory::Lifetime::System);
        if (err.isOk()) err = _bridge.configure(timing, d.dsiLanes, true);
        if (err.isOk()) err = _scanout.start(c, _surface);
        if (err.isOk()) err = _bridge.resetReceiver();
        if (err.isError()) stop();
        return err;
}

Error Display::show(Surface &surface, const VideoTiming &mode, uint8_t lanes, uint32_t minKhz, unsigned int timeoutMs) {
        const uint8_t n = lanes != 0 ? lanes : board().display.dsiLanes;
        auto [timing, err] = fitMode(mode, n, minKhz);
        if (err.isError()) return err;
        return _scanout.show(surface, timing, n, timeoutMs);
}

Error Display::showDesktop(unsigned int timeoutMs) {
        return _scanout.show(_surface, _timing, board().display.dsiLanes, timeoutMs);
}

Error Display::readEdidBlock(I2cBus &bus, uint8_t n, uint8_t (&block)[Edid::BlockBytes]) {
        auto [ddc, err] = bus.device(EdidAddress, DdcHz, DdcTimeoutMs);
        if (err.isError()) return err;
        const uint8_t offset = static_cast<uint8_t>(n * Edid::BlockBytes);
        return ddc->writeRead(ByteSpan(&offset, 1), WritableByteSpan(block, sizeof(block)), DdcTimeoutMs);
}

Result<Edid> Display::monitor() {
        uint8_t     block[Edid::BlockBytes];
        const Error err = readEdidBlock(_bridge.bus(), 0, block);
        if (err.isError()) return makeError<Edid>(err);
        auto [edid, perr] = Edid::parse(ByteSpan(block, sizeof(block)));
        if (perr.isError()) return makeError<Edid>(perr);
        if (edid.extensions > 0 && readEdidBlock(_bridge.bus(), 1, block).isOk())
                (void)edid.parseExtension(ByteSpan(block, sizeof(block)));
        return makeResult(edid);
}

void Display::stop() {
        _scanout.stop();
        _surface.stop();
        return;
}

Error Display::claim(const char *owner) {
        if (_owner != nullptr && _owner != owner) return Error::Busy;
        _owner = owner;
        return Error::Ok;
}

// A rebuilt DSI link: the bridge set up again, as at the start (after a link
// drop its receiver alone doesn't always lock), at the timing now sent (a
// program's full-screen mode, or surface 0's).
void Display::onLinkRestart(void *arg) {
        auto &self = *static_cast<Display *>(arg);
        if (self._bridge.configure(self._scanout.timing(), self._scanout.lanes(), true).isOk())
                (void)self._bridge.resetReceiver();
        return;
}

BMDS_NAMESPACE_END
