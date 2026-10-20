/**
 * @file      midifile.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/filehandle.h>
#include <bmds/midiparser.h>
#include <bmds/midiqueue.h>
#include <bmds/spinlock.h>
#include <bmds/midirouter.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/semaphore.h>
#include <bmds/stream.h>
#include <bmds/waitlist.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What an fd of `/dev/midiN`, `/dev/midi` or `/dev/midinew` refers to
 *        (`devplan/audio.md` §21). The system calls (`midi_config`,
 *        `midi_status`, `midi_control`) reach it through `Stream::midiFile()`.
 *
 * - On a port (`/dev/midiN`; `/dev/midi` is `system`'s): reading gives the
 *   port's source, writing goes to its destination; for `system` (a system
 *   port with no destination), writing is sent from its source.
 * - `/dev/midinew` is nothing until configured `Create`: then it owns a new
 *   virtual port. What it writes is the port's source; what the router
 *   delivers to the port's destination, it reads. The port ends with it.
 *
 * Bytes by default, as OSS's raw MIDI: whole messages read (no running
 * status), and bytes written parsed (running status taken). Configured
 * `Events`, `read()` and `write()` move `bmds_midi_event_t` records of the
 * size the program gave. Reads wait for an event; writes never wait (a full
 * destination drops and counts). A record written with a time is scheduled
 * (`devplan/audio.md` §25): held in the fd's queue (`ScheduledEvents`, made
 * in PSRAM at the first) until the MIDI clock's thread (`midiclock`, woken
 * by the wake timer, not the tick) sends it at its time, carrying that time.
 * The notes an fd turns on and hasn't turned off get a note-off when it
 * closes, so a program that ends (or crashes) mid-note leaves none sounding. A port that goes away ends reads
 * (`EndOfFile`) and breaks writes (`BrokenPipe`).
 *
 * @par Thread Safety
 * Thread-safe: reads serialize on one lock, writes on another.
 */
class MidiFile final : public Stream, private MidiSink {
        public:
                /** @brief `Config::flags` (`bmds_midi_config_t::flags`). */
                static constexpr uint32_t Events = 0x1;      ///< Records instead of bytes.
                static constexpr uint32_t Create = 0x2;      ///< `/dev/midinew`: make a virtual port.
                static constexpr uint32_t Source = 0x4;      ///< With `Create`: it has a source.
                static constexpr uint32_t Destination = 0x8; ///< With `Create`: it has a destination.

                /** @brief The smallest and largest record sizes (`bmds_midi_event_t` is the smallest). */
                static constexpr uint32_t MinEventSize = sizeof(MidiEvent);
                static constexpr uint32_t MaxEventSize = 256;

                /** @brief A reader's ring, in events. */
                static constexpr size_t RingEvents = 256;

                /** @brief Events an fd holds scheduled and unsent (`BMDS_MIDI_SCHEDULED_MAX`). */
                static constexpr size_t ScheduledEvents = 512;

                /** @brief How far ahead an event may be scheduled (`BMDS_MIDI_AHEAD_MAX_NS`). */
                static constexpr int64_t MaxAheadNs = 60'000'000'000;

                /** @brief `bmds_midi_config_t`, in the kernel's types. */
                struct Config {
                                uint32_t flags = 0;
                                uint32_t eventSize = 0;
                                uint8_t  port = MidiRouter::NoPort; ///< Out: its port.
                                char     name[MidiRouter::MaxName] = {};
                };

                /** @brief `bmds_midi_status_t`, in the kernel's types. */
                struct Status {
                                MidiRouter::PortInfo port;
                                bool                 gone = false;
                                uint64_t             eventsRead = 0;
                                uint32_t             dropped = 0;
                };

                /**
                 * @brief Adds `/dev/midi0` to `/dev/midi15` (each there while its port
                 *        is), `/dev/midi` and `/dev/midinew`.
                 * @return `Ok`, or `Vfs::addDeviceFile()`'s error.
                 */
                static Error addFiles();

                /**
                 * @brief Sets the configuration; @p got the result. Waits for a read
                 *        under way.
                 * @return `Ok`, `Invalid` (a flag, size or name not taken; `Create`
                 *         twice, or not on `/dev/midinew`), `Full` (no port left).
                 */
                Error configure(const Config &config, Config &got);

                /** @brief Returns the configuration now (as `configure()`'s @p got). */
                Config config();

                /** @brief Returns the port's state and this fd's counters. */
                Status status();

                /** @brief Connects its port's source to port @p destination. @return `MidiRouter::connect()`'s. */
                Error connect(uint8_t destination);

                /** @brief Ends that connection. @return `Ok`, `NotExist`. */
                Error disconnect(uint8_t destination);

                /** @brief Throws away what waits to be read. */
                void dropInput();

                /** @brief Throws away what it scheduled and hasn't sent (`BMDS_MIDI_CANCEL`). */
                void cancelScheduled();

                Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) override;
                Result<size_t> write(ByteSpan data, unsigned int timeoutMs) override;
                using Stream::write;
                uint8_t   readiness() override;
                WaitList *readinessWaiters() override { return &_pollers; }
                MidiFile *midiFile() override { return this; }
                void      handleClosed(uint8_t mode) override;

        private:
                MidiFile(uint8_t port, Span<MidiEvent> ring);
                ~MidiFile() = default;

                // Opens one on port @p port (NoPort: /dev/midinew).
                static Result<FileHandle::Ptr> open(uint8_t port, uint8_t flags);

                Delivery deliver(const MidiEvent &event) override;
                void     portGone() override;

                Error          applyConfig(const Config &config);
                Result<size_t> readEvents(WritableByteSpan buf);
                Result<size_t> readBytes(WritableByteSpan buf);
                Error          put(uint8_t byte);
                Error          emit(MidiEvent &event);
                void           noteHeld(const MidiEvent &event);
                void           releaseHeld();
                Error          schedule(const MidiEvent &record);
                void           sendScheduled(const MidiEvent &event);
                bool           hasRoomToSchedule() const;
                static void    clockMain(void *arg);
                bool           hasPort() const { return _port != MidiRouter::NoPort; }

                std::atomic<uint8_t>  _port; // NoPort: /dev/midinew, not created yet
                bool                  _owner = false;
                bool                  _asSource = false; // a write is sent from the port's source (`system`)
                uint8_t               _sides = 0; // a created port's
                std::atomic<bool>     _gone{false};
                uint32_t              _flags = 0;
                uint32_t              _eventSize = MinEventSize;
                MidiQueue             _in;
                Mutex                 _configLock;
                Mutex                 _readLock;
                Mutex                 _writeLock;
                Semaphore             _readable{1, 0}; // released when an event arrives or the port goes
                WaitListOf<4>         _pollers;
                MidiParser            _parser;                            // writes
                uint8_t               _pending[MidiEvent::MaxBytes] = {}; // a read event's bytes not yet read
                uint8_t               _pendingAt = 0;
                uint8_t               _pendingSize = 0;
                uint64_t              _eventsRead = 0;
                std::atomic<uint32_t> _dropped{0};
                MidiQueue             _scheduled;                  // the clock's lock, both ends
                MidiEvent            *_scheduledStorage = nullptr; // PSRAM, from the first scheduled write
                int64_t               _lastScheduledNs = 0;        // the latest queued
                SpinLock              _heldLock;
                uint8_t               _held[16][16] = {}; // the notes it holds on: a bit a note, by channel
};

BMDS_NAMESPACE_END
