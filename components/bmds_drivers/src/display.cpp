/**
 * @file      display.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/board.h>
#include <bmds/display.h>
#include <bmds/i2cbus.h>
#include <bmds/staticobject.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit StaticObject<Display> displays[Display::MaxDisplays];

        // The pixel clocks the DPI clock divides from (its PLLs), kHz.
        constexpr uint32_t Plls[] = {240'000, 160'000};

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
        }
        if (!slot->_attached) { // tried again until the bridge answers
                const Error err = slot->_bridge.attach();
                if (err.isError()) return makeError<Display *>(err);
                slot->_attached = true;
        }
        return makeResult(&*slot);
}

Display::Display(I2cBus &bus, uint8_t bridgeAddress, uint8_t number)
    : _bridge(bus, bridgeAddress), _scanout(0), _number(number) {}

Result<VideoTiming> Display::fitMode(const VideoTiming &mode) {
        // The link carries 24 bits a pixel, 1.5 Gbps a lane at most.
        const uint32_t maxKhz = board().display.dsiLanes * 1'500'000u / 24u;
        auto [fitted, err] = VideoModes::fitToClock(mode, Plls, sizeof(Plls) / sizeof(Plls[0]), maxKhz);
        if (err.isError()) return makeError<VideoTiming>(Error::NotSupported);
        return makeResult(fitted);
}

uint16_t Display::blockLines(uint16_t height, uint16_t most) {
        for (uint16_t lines = most; lines >= MinBlockLines; --lines) {
                if (height % lines == 0) return lines;
        }
        return 0;
}

Error Display::start(const VideoTiming &mode) {
        return begin(mode, nullptr);
}

Error Display::start(const VideoTiming &mode, BlockSource &blocks) {
        return begin(mode, &blocks);
}

// The framebuffer's way (no @p blocks) or a composer's.
Error Display::begin(const VideoTiming &mode, BlockSource *blocks) {
        stop();
        auto [timing, ferr] = fitMode(mode);
        if (ferr.isError()) return ferr;
        const uint16_t lines = blockLines(timing.vActive, MaxBlockLines);
        if (lines == 0) return Error::Invalid;
        const BoardDisplay &d = board().display;
        Scanout::Config     c;
        c.timing = timing;
        c.format = blocks != nullptr ? Scanout::Format::Rgb888 : Scanout::Format::Rgb565Expanded;
        c.lanes = d.dsiLanes;
        c.phyLdo = d.phyLdo;
        c.phyLdoMv = d.phyLdoMv;
        c.linesPerBlock = lines;
        c.blocks = 3;
        c.freeRunning = true;
        c.onRestart = onLinkRestart;
        c.onRestartArg = this;
        _timing = timing;
        _blockLines = lines;
        Error err = _bridge.configure(timing, d.dsiLanes, true);
        if (blocks != nullptr) {
                if (err.isOk()) err = _scanout.start(c, *blocks);
        } else {
                if (err.isOk()) err = _framebuffer.start(timing.hActive, timing.vActive);
                if (err.isOk()) err = _scanout.start(c, _framebuffer);
        }
        if (err.isOk()) err = _bridge.resetReceiver();
        if (err.isError()) stop();
        return err;
}

void Display::stop() {
        _scanout.stop();
        _framebuffer.stop();
        _blockLines = 0;
        return;
}

Error Display::claim(const char *owner) {
        if (_owner != nullptr && _owner != owner) return Error::Busy;
        _owner = owner;
        return Error::Ok;
}

// A rebuilt DSI link: the bridge set up again, as at the start (after a link
// drop its receiver alone doesn't always lock).
void Display::onLinkRestart(void *arg) {
        auto &self = *static_cast<Display *>(arg);
        if (self._bridge.configure(self._timing, board().display.dsiLanes, true).isOk())
                (void)self._bridge.resetReceiver();
        return;
}

BMDS_NAMESPACE_END
