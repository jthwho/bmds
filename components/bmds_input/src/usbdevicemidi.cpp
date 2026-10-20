/**
 * @file      usbdevicemidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The board's MIDI cables as a USB device (devplan/audio.md §23), on
 * TinyUSB's MIDI device class: a port per cable while a computer has the
 * device configured. What the computer sends is parsed on the device's task
 * and sent from the cable's port; what the router delivers waits in a ring
 * until the device's task packs it into USB-MIDI packets (TinyUSB's device
 * calls are made on its task).
 */

#include <atomic>
#include <cstdio>
#include <tusb.h>
#include <bmds/clock.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/midi.h>
#include <bmds/midiparser.h>
#include <bmds/midiqueue.h>
#include <bmds/midiusbpacker.h>
#include "usbdevice_p.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t  MaxCables = 4;  // usb/device/midi-cables' largest
        constexpr size_t  OutEvents = 64; // the ring of events to send
        constexpr size_t  ReadBytes = 64; // read from TinyUSB at a time
        constexpr uint8_t Interface = 0;  // TinyUSB's MIDI interface (there is one)

        // A cable: a port. Its parser and packer are used on the device's task only.
        class Cable final : public MidiSink {
                public:
                        Delivery deliver(const MidiEvent &event) override;
                        void     kick() override;

                        uint8_t       number = 0;
                        uint8_t       port = MidiRouter::NoPort;
                        MidiParser    parser;
                        MidiUsbPacker packer;
        };

        struct State {
                        bool                  used = false; // ports made (the device's task)
                        uint8_t               cableCount = 0;
                        Cable                 cables[MaxCables];
                        MidiQueue             out;                  // what the router delivers (its lock held)
                        MidiEvent            *outStorage = nullptr; // PSRAM, while configured
                        std::atomic<bool>     drainQueued{false};
                        std::atomic<uint32_t> dropped{0};
                        MidiUsbPacker::Packet waiting[MidiUsbPacker::MaxPackets] = {}; // didn't fit
                        uint8_t               waitingAt = 0;
                        uint8_t               waitingCount = 0;
        };

        constinit State state;

        MidiSink::Delivery Cable::deliver(const MidiEvent &event) {
                MidiEvent e = event;
                e.flags = number; // which cable, for the packer
                if (state.out.push(e)) return Delivery::TakenKick;
                state.dropped.fetch_add(1, std::memory_order_relaxed);
                return Delivery::Dropped;
        }

        void drainLater(void *) {
                state.drainQueued.store(false, std::memory_order_release);
                (void)detail::usbDeviceMidiDrain();
                return;
        }

        // Events were queued: drained now on the device's task, else queued to it once.
        void Cable::kick() {
                if (detail::usbDeviceIsOnTask()) {
                        (void)detail::usbDeviceMidiDrain();
                        return;
                }
                if (!state.drainQueued.exchange(true, std::memory_order_acq_rel) &&
                    !detail::usbDeviceDefer(drainLater, nullptr))
                        state.drainQueued.store(false, std::memory_order_release);
                return;
        }

        // Packets left over from the last drain, first. False: TinyUSB is still full.
        bool sendWaiting() {
                while (state.waitingAt < state.waitingCount) {
                        if (!tud_midi_n_packet_write(Interface, state.waiting[state.waitingAt])) return false;
                        ++state.waitingAt;
                }
                state.waitingAt = state.waitingCount = 0;
                return true;
        }

} // namespace

void detail::usbDeviceMidiStart(uint8_t cables) {
        if (state.used || !Midi::isReady()) return;
        state.outStorage = static_cast<MidiEvent *>(Memory::alloc(MemSpace::Psram, OutEvents * sizeof(MidiEvent)));
        if (state.outStorage == nullptr) {
                bmdsWarn("USB device: MIDI: no memory");
                return;
        }
        (void)state.out.setStorage(Span<MidiEvent>(state.outStorage, OutEvents));
        state.used = true;
        state.drainQueued.store(false, std::memory_order_release); // one queued before a stop never ran
        state.waitingAt = state.waitingCount = 0;
        state.cableCount = cables < MaxCables ? cables : static_cast<uint8_t>(MaxCables);
        Midi::applySettings();
        for (uint8_t i = 0; i < state.cableCount; ++i) {
                Cable &c = state.cables[i];
                c.number = i;
                c.port = MidiRouter::NoPort;
                c.parser.reset();
                c.packer.reset();
                char name[MidiRouter::MaxName];
                std::snprintf(name, sizeof(name), "USB device %u", i + 1u);
                auto [n, err] =
                        Midi::router().addPort(MidiRouter::Kind::UsbDevice,
                                               MidiRouter::Source | MidiRouter::Destination, StringView(name), &c);
                if (err.isError()) {
                        bmdsWarn("USB device: MIDI cable %u: no port: %s", i + 1u, err.desc());
                        continue;
                }
                c.port = n;
        }
        return;
}

void detail::usbDeviceMidiStop() {
        if (!state.used) return;
        for (uint8_t i = 0; i < state.cableCount; ++i) {
                if (state.cables[i].port != MidiRouter::NoPort) Midi::router().removePort(state.cables[i].port);
                state.cables[i].port = MidiRouter::NoPort;
        }
        // Nothing more is delivered (a drain still queued finds it unused); what waits goes.
        state.used = false;
        (void)state.out.setStorage(Span<MidiEvent>());
        Memory::free(state.outStorage);
        state.outStorage = nullptr;
        state.waitingAt = state.waitingCount = 0;
        state.cableCount = 0;
        return;
}

// On the device's task: the ring into packets, as many as TinyUSB takes; the rest when the
// task looks again (its buffer drains as the computer reads).
bool detail::usbDeviceMidiDrain() {
        if (!state.used) return false;
        bool      full = !sendWaiting();
        MidiEvent e;
        while (!full && state.out.pop(e)) {
                Cable                &c = state.cables[e.flags < state.cableCount ? e.flags : 0];
                MidiUsbPacker::Packet packets[MidiUsbPacker::MaxPackets];
                const size_t          n = c.packer.pack(c.number, e, packets);
                for (size_t i = 0; i < n; ++i) {
                        if (!full && tud_midi_n_packet_write(Interface, packets[i])) continue;
                        full = true;
                        for (size_t k = 0; k < 4; ++k) state.waiting[state.waitingCount][k] = packets[i][k];
                        ++state.waitingCount;
                }
        }
        return full;
}

bool detail::usbDeviceMidiWaiting() {
        return state.used && state.waitingCount != 0;
}

uint32_t detail::usbDeviceMidiDropped() {
        return state.dropped.load(std::memory_order_relaxed);
}

BMDS_NAMESPACE_END

// TinyUSB's MIDI device callback (weak in TinyUSB), on the device's task: what the computer
// sent, parsed per cable and sent from each cable's port. Read until TinyUSB has no more (a
// read stops at a change of cable).
extern "C" void tud_midi_rx_cb(uint8_t) {
        using namespace bmds;
        if (!state.used) {
                uint8_t packet[4];
                while (tud_midi_n_packet_read(Interface, packet)) {}
                return;
        }
        uint8_t buf[ReadBytes];
        for (;;) {
                uint8_t        cable = 0;
                const uint32_t n = tud_midi_n_demux_stream_read(Interface, &cable, buf, sizeof(buf));
                if (n == 0) break;
                if (cable >= state.cableCount || state.cables[cable].port == MidiRouter::NoPort) continue;
                Cable        &c = state.cables[cable];
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
