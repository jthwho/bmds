/**
 * @file      usbmidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * USB MIDI devices as MIDI ports (devplan/audio.md §21), on TinyUSB's MIDI
 * host class: a port per cable. What a device sends is parsed on the host's
 * task and sent from the port's source; what the router delivers to a port
 * waits in its device's ring until the host's task packs it into USB-MIDI
 * packets (TinyUSB is called from its own task only).
 */

#include <atomic>
#include <cstdio>
#include <tusb.h>
#include <host/usbh_pvt.h>
#include <bmds/clock.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midiparser.h>
#include <bmds/midiqueue.h>
#include <bmds/midiusbpacker.h>
#include <bmds/usbhost.h>
#include "usbmidi.h"

BMDS_NAMESPACE_BEGIN

namespace detail {
        UsbHost *activeUsbHost();
} // namespace detail

namespace {

        constexpr size_t MaxDevices = CFG_TUH_MIDI;
        constexpr size_t MaxCables = 8;  // ports a device gets at most
        constexpr size_t OutEvents = 64; // a device's ring of events to send
        constexpr size_t ReadBytes = 64; // read from TinyUSB at a time
        constexpr char   DefaultName[] = "USB MIDI";

        class Device;

        // A cable: a port. Its parser and packer are used on the host's task only.
        class Cable final : public MidiSink {
                public:
                        Delivery deliver(const MidiEvent &event) override;
                        void     kick() override;

                        Device       *device = nullptr;
                        uint8_t       number = 0;
                        uint8_t       port = MidiRouter::NoPort;
                        MidiParser    parser;
                        MidiUsbPacker packer;
        };

        class Device {
                public:
                        void start(uint8_t index, uint8_t address, uint8_t rxCables, uint8_t txCables);
                        void stop();
                        void received();
                        void drain();
                        void kick();
                        void name(StringView product);

                        bool    isUsed() const { return _used; }
                        uint8_t address() const { return _address; }

                        // What the router delivers (its lock held: one producer at a time).
                        MidiQueue out;

                private:
                        static void drainLater(void *arg);
                        bool        sendWaiting();

                        bool                  _used = false;
                        uint8_t               _index = 0;
                        uint8_t               _address = 0;
                        uint8_t               _cableCount = 0;
                        Cable                 _cables[MaxCables];
                        MidiEvent            *_outStorage = nullptr; // PSRAM, while mounted
                        std::atomic<bool>     _drainQueued{false};
                        MidiUsbPacker::Packet _waiting[MidiUsbPacker::MaxPackets] = {}; // packets that didn't fit
                        uint8_t               _waitingAt = 0;
                        uint8_t               _waitingCount = 0;
        };

        constinit Device devices[MaxDevices];

        // A port's name: the device's, and the cable's number after the first.
        void portName(StringView product, uint8_t cable, char (&out)[MidiRouter::MaxName]) {
                const StringView base = product.isEmpty() ? StringView(DefaultName) : product;
                if (cable == 0)
                        std::snprintf(out, sizeof(out), "%.*s", static_cast<int>(base.size()), base.data());
                else
                        std::snprintf(out, sizeof(out), "%.*s %u", static_cast<int>(base.size()), base.data(),
                                      static_cast<unsigned>(cable + 1));
                return;
        }

        // The product string UsbHost has for @p address, or "".
        void productOf(uint8_t address, char (&out)[UsbHost::MaxName]) {
                out[0] = '\0';
                UsbHost *host = detail::activeUsbHost();
                if (host == nullptr) return;
                UsbHost::DeviceInfo infos[UsbHost::MaxDevices];
                const size_t        n = host->devices(Span<UsbHost::DeviceInfo>(infos, UsbHost::MaxDevices));
                for (size_t i = 0; i < n; ++i)
                        if (infos[i].address == address) std::snprintf(out, sizeof(out), "%s", infos[i].name);
                return;
        }

        MidiSink::Delivery Cable::deliver(const MidiEvent &event) {
                MidiEvent e = event;
                e.flags = number; // which cable, for the packer
                return device->out.push(e) ? Delivery::TakenKick : Delivery::Dropped;
        }

        void Cable::kick() {
                device->kick();
                return;
        }

        void Device::start(uint8_t index, uint8_t address, uint8_t rxCables, uint8_t txCables) {
                _index = index;
                _address = address;
                _outStorage = static_cast<MidiEvent *>(Memory::alloc(MemSpace::Psram, OutEvents * sizeof(MidiEvent)));
                if (_outStorage == nullptr) {
                        bmdsWarn("USB: %u: MIDI: no memory", address);
                        return;
                }
                (void)out.setStorage(Span<MidiEvent>(_outStorage, OutEvents));
                _used = true;
                _waitingAt = _waitingCount = 0;
                const uint8_t cables = rxCables > txCables ? rxCables : txCables;
                _cableCount = cables < MaxCables ? cables : static_cast<uint8_t>(MaxCables);
                if (cables > MaxCables)
                        bmdsWarn("USB: %u: MIDI: %u cables; ports for the first %u", address, cables,
                                 static_cast<unsigned>(MaxCables));
                char product[UsbHost::MaxName];
                productOf(address, product);
                for (uint8_t i = 0; i < _cableCount; ++i) {
                        Cable &c = _cables[i];
                        c.device = this;
                        c.number = i;
                        c.port = MidiRouter::NoPort;
                        c.parser.reset();
                        c.packer.reset();
                        const uint8_t sides =
                                (i < rxCables ? MidiRouter::Source : 0) | (i < txCables ? MidiRouter::Destination : 0);
                        char name[MidiRouter::MaxName];
                        portName(StringView(product), i, name);
                        auto [n, err] = Midi::router().addPort(MidiRouter::Kind::Usb, sides, StringView(name),
                                                               (sides & MidiRouter::Destination) != 0 ? &c : nullptr);
                        if (err.isError()) {
                                bmdsWarn("USB: %u: MIDI cable %u: no port: %s", address, i + 1u, err.desc());
                                continue;
                        }
                        c.port = n;
                }
                bmdsInfo("USB: %u: MIDI, %u in and %u out cables", address, rxCables, txCables);
                return;
        }

        void Device::stop() {
                for (uint8_t i = 0; i < _cableCount; ++i) {
                        if (_cables[i].port != MidiRouter::NoPort) Midi::router().removePort(_cables[i].port);
                        _cables[i].port = MidiRouter::NoPort;
                }
                // Nothing more is delivered (a drain still queued finds it unused); what waits goes.
                _used = false;
                (void)out.setStorage(Span<MidiEvent>());
                Memory::free(_outStorage);
                _outStorage = nullptr;
                _waitingAt = _waitingCount = 0;
                _cableCount = 0;
                return;
        }

        void Device::name(StringView product) {
                for (uint8_t i = 0; i < _cableCount; ++i) {
                        if (_cables[i].port == MidiRouter::NoPort) continue;
                        char name[MidiRouter::MaxName];
                        portName(product, i, name);
                        (void)Midi::router().rename(_cables[i].port, StringView(name));
                }
                return;
        }

        // What the device sent: parsed per cable, from each cable's port. Read until TinyUSB has
        // no more (its read stops at a change of cable).
        void Device::received() {
                uint8_t buf[ReadBytes];
                for (;;) {
                        uint8_t        cable = 0;
                        const uint32_t n = tuh_midi_stream_read(_index, &cable, buf, sizeof(buf));
                        if (n == 0) break;
                        if (cable >= _cableCount || _cables[cable].port == MidiRouter::NoPort) continue;
                        Cable        &c = _cables[cable];
                        const int64_t now = Clock::monotonicNs();
                        for (uint32_t i = 0; i < n; ++i) {
                                MidiEvent    events[MidiParser::MaxEvents];
                                const size_t count = c.parser.feed(buf[i], events);
                                for (size_t k = 0; k < count; ++k) {
                                        events[k].timeNs = now;
                                        (void)Midi::router().send(c.port, events[k]);
                                }
                        }
                        Midi::router().noteParseErrors(c.port, c.parser.takeErrors());
                }
                return;
        }

        // Packets left over from the last drain, first. False: TinyUSB is still full.
        bool Device::sendWaiting() {
                while (_waitingAt < _waitingCount) {
                        if (!tuh_midi_packet_write(_index, _waiting[_waitingAt])) return false;
                        ++_waitingAt;
                }
                _waitingAt = _waitingCount = 0;
                return true;
        }

        // On the host's task: the ring into packets, as many as TinyUSB takes; the rest after its
        // next transfer (tuh_midi_tx_cb).
        void Device::drain() {
                if (!_used) return;
                bool      full = !sendWaiting();
                MidiEvent e;
                while (!full && out.pop(e)) {
                        Cable                &c = _cables[e.flags < _cableCount ? e.flags : 0];
                        MidiUsbPacker::Packet packets[MidiUsbPacker::MaxPackets];
                        const size_t          n = c.packer.pack(c.number, e, packets);
                        for (size_t i = 0; i < n; ++i) {
                                if (!full && tuh_midi_packet_write(_index, packets[i])) continue;
                                full = true;
                                for (size_t k = 0; k < 4; ++k) _waiting[_waitingCount][k] = packets[i][k];
                                ++_waitingCount;
                        }
                }
                (void)tuh_midi_write_flush(_index);
                return;
        }

        void Device::drainLater(void *arg) {
                auto *d = static_cast<Device *>(arg);
                d->_drainQueued.store(false, std::memory_order_release);
                d->drain();
                return;
        }

        // Events were queued: drained now on the host's task, else queued to it once.
        void Device::kick() {
                UsbHost *host = detail::activeUsbHost();
                if (host != nullptr && host->isOnTask()) {
                        drain();
                        return;
                }
                if (!_drainQueued.exchange(true, std::memory_order_acq_rel)) usbh_defer_func(drainLater, this, false);
                return;
        }

} // namespace

void detail::usbMidiNamed(uint8_t address, StringView product) {
        if (!Midi::isReady()) return;
        Midi::applySettings();
        for (Device &d : devices)
                if (d.isUsed() && d.address() == address) d.name(product);
        return;
}

BMDS_NAMESPACE_END

// TinyUSB's MIDI callbacks (weak in TinyUSB), on the host's task.
extern "C" {

void tuh_midi_mount_cb(uint8_t idx, const tuh_midi_mount_cb_t *mount) {
        if (idx >= bmds::MaxDevices || !bmds::Midi::isReady()) return;
        bmds::Midi::applySettings();
        bmds::devices[idx].start(idx, mount->daddr, mount->rx_cable_count, mount->tx_cable_count);
        return;
}

void tuh_midi_umount_cb(uint8_t idx) {
        if (idx < bmds::MaxDevices && bmds::devices[idx].isUsed()) bmds::devices[idx].stop();
        return;
}

void tuh_midi_rx_cb(uint8_t idx, uint32_t) {
        if (idx < bmds::MaxDevices && bmds::devices[idx].isUsed()) bmds::devices[idx].received();
        return;
}

void tuh_midi_tx_cb(uint8_t idx, uint32_t) {
        if (idx < bmds::MaxDevices && bmds::devices[idx].isUsed()) bmds::devices[idx].drain();
        return;
}

} // extern "C"
