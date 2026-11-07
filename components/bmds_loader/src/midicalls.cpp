/**
 * @file      midicalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The MIDI system calls (target only, devplan/audio.md §21): the ABI's
 * structs to and from an fd's MidiFile.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <bmds/errno.h>
#include <bmds/midifile.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::Error;
using bmds::KernelCall;
using bmds::MidiEvent;
using bmds::MidiFile;
using bmds::MidiRouter;
using bmds::Process;
using bmds::detail::copyIn;
using bmds::detail::copyOut;
using bmds::detail::copyText;

namespace {

        static_assert(BMDS_MIDI_EVENTS == MidiFile::Events && BMDS_MIDI_CREATE == MidiFile::Create &&
                              BMDS_MIDI_SOURCE == MidiFile::Source && BMDS_MIDI_DESTINATION == MidiFile::Destination,
                      "the ABI's flags are MidiFile's");
        static_assert(BMDS_MIDI_SOURCE == MidiRouter::Source << 2 && BMDS_MIDI_DESTINATION == MidiRouter::Destination
                                                                                                      << 2,
                      "the ABI's sides are the router's, moved up two bits");
        static_assert(BMDS_MIDI_USB == static_cast<uint32_t>(MidiRouter::Kind::Usb) &&
                              BMDS_MIDI_SERIAL == static_cast<uint32_t>(MidiRouter::Kind::Serial) &&
                              BMDS_MIDI_VIRTUAL == static_cast<uint32_t>(MidiRouter::Kind::Virtual) &&
                              BMDS_MIDI_NET == static_cast<uint32_t>(MidiRouter::Kind::Net) &&
                              BMDS_MIDI_USB_DEVICE == static_cast<uint32_t>(MidiRouter::Kind::UsbDevice) &&
                              BMDS_MIDI_SYSTEM == static_cast<uint32_t>(MidiRouter::Kind::System),
                      "the ABI's kinds are the router's");
        static_assert(BMDS_MIDI_SCHEDULED_MAX == MidiFile::ScheduledEvents &&
                              BMDS_MIDI_AHEAD_MAX_NS == MidiFile::MaxAheadNs,
                      "the ABI's scheduling limits are MidiFile's");
        static_assert(BMDS_MIDI_NAME_MAX == MidiRouter::MaxName, "the ABI's name length is the router's");
        static_assert(sizeof(bmds_midi_event_t) == sizeof(MidiEvent) &&
                              offsetof(bmds_midi_event_t, time_ns) == offsetof(MidiEvent, timeNs) &&
                              offsetof(bmds_midi_event_t, port) == offsetof(MidiEvent, port) &&
                              offsetof(bmds_midi_event_t, length) == offsetof(MidiEvent, size) &&
                              offsetof(bmds_midi_event_t, flags) == offsetof(MidiEvent, flags) &&
                              offsetof(bmds_midi_event_t, data) == offsetof(MidiEvent, data),
                      "bmds_midi_event_t is MidiEvent");
        static_assert(MidiFile::MinEventSize == sizeof(bmds_midi_event_t), "the smallest record is the ABI's event");

        // The MIDI file behind @p handle (an fd's), or the errno.
        int32_t midiFileOf(const bmds::FileHandle::Ptr &handle, MidiFile *&file) {
                if (!handle) return -BMDS_EBADF;
                file = handle->stream().midiFile();
                return file != nullptr ? 0 : -BMDS_ENOTTY;
        }

} // namespace

extern "C" {

int32_t bmds_k_midi_config(int32_t fd, const bmds_midi_config_t *set, bmds_midi_config_t *got) {
        KernelCall            call;
        bmds::FileHandle::Ptr handle = call->handle(fd);
        MidiFile             *file = nullptr;
        int32_t               r = midiFileOf(handle, file);
        if (r < 0) return r;
        MidiFile::Config result;
        if (set != nullptr) {
                bmds_midi_config_t c;
                r = copyIn(set, c);
                if (r < 0) return r;
                MidiFile::Config k;
                k.flags = c.flags;
                k.eventSize = c.event_size;
                std::memcpy(k.name, c.name, sizeof(k.name));
                r = Process::errnoFor(file->configure(k, result));
                if (r < 0) return r;
        } else {
                result = file->config();
        }
        if (got == nullptr) return 0;
        bmds_midi_config_t g = {};
        g.flags = result.flags;
        g.event_size = result.eventSize;
        g.port = result.port;
        copyText(bmds::StringView(result.name), g.name, sizeof(g.name));
        return copyOut(g, got);
}

int32_t bmds_k_midi_status(int32_t fd, bmds_midi_status_t *status) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        MidiFile                   *file = nullptr;
        const int32_t               r = midiFileOf(handle, file);
        if (r < 0) return r;
        if (status == nullptr) return -BMDS_EFAULT;
        const MidiFile::Status k = file->status();
        bmds_midi_status_t     s = {};
        s.port = k.port.number;
        s.kind = static_cast<uint32_t>(k.port.kind);
        s.sides = static_cast<uint32_t>(k.port.sides) << 2;
        s.flags = k.gone ? BMDS_MIDI_GONE : 0;
        s.dropped = k.port.counters.dropped;
        s.events_in = k.port.counters.eventsIn;
        s.events_out = k.port.counters.eventsOut;
        s.parse_errors = k.port.counters.parseErrors;
        s.device_errors = k.port.counters.deviceErrors;
        s.last_ns = k.port.counters.lastNs;
        s.fd_events = k.eventsRead;
        s.fd_dropped = k.dropped;
        copyText(bmds::StringView(k.port.name), s.name, sizeof(s.name));
        return copyOut(s, status);
}

int32_t bmds_k_midi_control(int32_t fd, uint32_t op, int32_t value) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        MidiFile                   *file = nullptr;
        const int32_t               r = midiFileOf(handle, file);
        if (r < 0) return r;
        if (op == BMDS_MIDI_FLUSH || op == BMDS_MIDI_CANCEL) {
                if (value != 0) return -BMDS_EINVAL;
                if (op == BMDS_MIDI_FLUSH)
                        file->dropInput();
                else
                        file->cancelScheduled();
                return 0;
        }
        if (op == BMDS_MIDI_LATENCY) { // devplan/sequencer.md §7, K2
                if (value != 0) return -BMDS_EINVAL;
                const int64_t us = file->latencyNs() / 1000;
                return static_cast<int32_t>(us > INT32_MAX ? INT32_MAX : us < 0 ? 0 : us);
        }
        if (value < 0 || value >= static_cast<int32_t>(MidiRouter::MaxPorts))
                return op == BMDS_MIDI_CONNECT || op == BMDS_MIDI_DISCONNECT ? -BMDS_ENOENT : -BMDS_EINVAL;
        switch (op) {
                case BMDS_MIDI_CONNECT: return Process::errnoFor(file->connect(static_cast<uint8_t>(value)));
                case BMDS_MIDI_DISCONNECT: return Process::errnoFor(file->disconnect(static_cast<uint8_t>(value)));
                default: return -BMDS_EINVAL;
        }
}

} // extern "C"
