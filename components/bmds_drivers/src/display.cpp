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

Error Display::start(const VideoTiming &mode, Surface::Buffering buffering) {
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
        _timing = timing;
        Error err = _surface.start(timing.hActive, timing.vActive, buffering);
        if (err.isOk()) err = _bridge.configure(timing, d.dsiLanes, true);
        if (err.isOk()) err = _scanout.start(c, _surface);
        if (err.isOk()) err = _bridge.resetReceiver();
        if (err.isError()) stop();
        return err;
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
// drop its receiver alone doesn't always lock).
void Display::onLinkRestart(void *arg) {
        auto &self = *static_cast<Display *>(arg);
        if (self._bridge.configure(self._timing, board().display.dsiLanes, true).isOk())
                (void)self._bridge.resetReceiver();
        return;
}

BMDS_NAMESPACE_END
