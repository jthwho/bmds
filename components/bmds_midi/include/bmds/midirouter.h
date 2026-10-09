/**
 * @file      midirouter.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/midievent.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where the router puts events (`devplan/audio.md` §21): a port's
 *        destination (a USB cable's output, a virtual port's owner) or a tap
 *        (an fd reading a port's source).
 *
 * `deliver()` is called with the router's lock held, so it must not wait or
 * call the router: it queues the event and wakes a reader at most. Work that
 * may wait (handing data to TinyUSB's task) goes in `kick()`, which the
 * router calls after letting go of its lock when `deliver()` asked for it.
 */
class MidiSink {
        public:
                /** @brief What `deliver()` did. */
                enum class Delivery : uint8_t {
                        Taken,     ///< Queued.
                        TakenKick, ///< Queued; call `kick()` once the lock is free.
                        Dropped,   ///< No room: counted by the router.
                };

                /** @brief Queues @p event. @nonblocking (under the router's lock). */
                virtual Delivery deliver(const MidiEvent &event) = 0;

                /**
                 * @brief Starts sending what `deliver()` queued (without the router's
                 *        lock). It may come after the port was removed: a sink that
                 *        kicks outlives the router's use of it.
                 */
                virtual void kick() { return; }

                /** @brief A tap's port was removed (under the router's lock). @nonblocking */
                virtual void portGone() { return; }

        protected:
                constexpr MidiSink() = default;
                ~MidiSink() = default;
};

/**
 * @brief The MIDI ports and the connections between them
 *        (`devplan/audio.md` §21).
 *
 * A port, `midiN`, has a source (the events it produces) and/or a
 * destination (the events delivered to it, through its `MidiSink`). A
 * connection takes a source to a destination: `send()` gives an event from a
 * source to every destination connected to it and every tap reading that
 * source. `deliver()` gives one to a port's destination alone (a write to
 * `/dev/midiN`).
 *
 * Connections by name (`setAutoConnect()`, the `midi/connect` setting): a
 * list of `SOURCE>DEST` pairs, separated by commas, made whenever both ports
 * exist, so a keyboard plugged in again reaches the synth again.
 *
 * Counted per port: events from its source, events to its destination and
 * those its sink had no room for, parse and device errors its producers
 * report, and its last event's time.
 *
 * @par Thread Safety
 * Thread-safe (one lock). Sinks are called under it (`MidiSink`).
 */
class MidiRouter {
        public:
                static constexpr size_t  MaxPorts = 16;        ///< `midi0` to `midi15`.
                static constexpr size_t  MaxConnections = 32;  ///< Connections at once.
                static constexpr size_t  MaxTaps = 32;         ///< Fds on ports at once.
                static constexpr size_t  MaxName = 32;         ///< A name's bytes, with its NUL.
                static constexpr size_t  MaxAutoConnect = 256; ///< `setAutoConnect()`'s list, with its NUL.
                static constexpr uint8_t NoPort = 0xff;        ///< No port.

                /** @brief A port's sides (`bmds_midi_*`'s `BMDS_MIDI_SOURCE`, `..._DESTINATION`). */
                static constexpr uint8_t Source = 0x1;      ///< It produces events.
                static constexpr uint8_t Destination = 0x2; ///< Events can be delivered to it.

                /** @brief What a port is (`bmds_midi_status_t::kind`'s numbers). */
                enum class Kind : uint8_t {
                        Usb,     ///< A USB MIDI device's cable.
                        Serial,  ///< A serial MIDI port.
                        Virtual, ///< A program's.
                };

                /** @brief A port's counters. */
                struct Counters {
                                uint64_t eventsIn = 0;     ///< Events from its source.
                                uint64_t eventsOut = 0;    ///< Events its destination took.
                                uint32_t dropped = 0;      ///< Events its destination had no room for.
                                uint32_t parseErrors = 0;  ///< Bytes its producers threw away, SysExes cut.
                                uint32_t deviceErrors = 0; ///< Transfer, framing and overrun errors.
                                int64_t  lastNs = 0;       ///< Its last event's time (0: none yet).
                };

                /** @brief A port, as `port()` and `ports()` tell it. */
                struct PortInfo {
                                uint8_t  number = NoPort;
                                Kind     kind = Kind::Virtual;
                                uint8_t  sides = 0;
                                char     name[MaxName] = {};
                                Counters counters;
                };

                /** @brief A connection. */
                struct Connection {
                                uint8_t source = NoPort;
                                uint8_t destination = NoPort;
                };

                MidiRouter() = default;
                MidiRouter(const MidiRouter &) = delete;
                MidiRouter &operator=(const MidiRouter &) = delete;

                /**
                 * @brief Adds a port: the lowest free number, @p name cut to fit.
                 *        With `Destination` in @p sides, @p destination is its sink
                 *        (until `removePort()`). Connections by name follow.
                 * @return The number, or `Invalid` (no sides; `Destination` without
                 *         a sink), `Full`.
                 */
                Result<uint8_t> addPort(Kind kind, uint8_t sides, StringView name, MidiSink *destination);

                /**
                 * @brief Removes port @p number: its connections go, its taps are told
                 *        (`MidiSink::portGone()`) and let go. Once it returns, its sink
                 *        gets nothing more but a `kick()` already under way.
                 */
                void removePort(uint8_t number);

                /** @brief Renames port @p number. @return `Ok`, `NotExist`. */
                Error rename(uint8_t number, StringView name);

                /** @brief Returns port @p number, or `NotExist`. */
                Result<PortInfo> port(uint8_t number);

                /**
                 * @brief Finds a port by @p name: `midiN` or `N`, else its name
                 *        (ignoring ASCII case; the lowest number if several).
                 * @return Its number, or `NotExist`.
                 */
                Result<uint8_t> find(StringView name);

                /** @brief Fills @p out with the ports, in order. @return How many there are (all of them). */
                size_t ports(Span<PortInfo> out);

                /**
                 * @brief Connects @p source's source to @p destination's destination.
                 * @return `Ok`, `Exists`, `NotExist`, `Invalid` (a side missing), `Full`.
                 */
                Error connect(uint8_t source, uint8_t destination);

                /** @brief Ends a connection. @return `Ok`, `NotExist`. */
                Error disconnect(uint8_t source, uint8_t destination);

                /** @brief Fills @p out with the connections. @return How many there are (all of them). */
                size_t connections(Span<Connection> out);

                /**
                 * @brief Adds @p tap on port @p number: told when it goes, and with
                 *        @p reading, given its source's events.
                 * @return `Ok`, `NotExist`, `Invalid` (reading a port without a
                 *         source, or @p tap already on), `Full`.
                 */
                Error attach(uint8_t number, MidiSink &tap, bool reading);

                /** @brief Takes @p tap off its port (nothing if it isn't on one). */
                void detach(MidiSink &tap);

                /**
                 * @brief An event from port @p source's source (its `port` set here):
                 *        to every destination connected and every tap reading it.
                 * @return `Ok`, `NotExist`, `Invalid` (it has no source).
                 */
                Error send(uint8_t source, const MidiEvent &event);

                /**
                 * @brief An event to port @p destination's destination alone.
                 * @return `Ok`, `NotExist`, `Invalid` (it has no destination),
                 *         `WouldBlock` (no room: counted).
                 */
                Error deliver(uint8_t destination, const MidiEvent &event);

                /** @brief Adds @p count to port @p number's parse errors. */
                void noteParseErrors(uint8_t number, uint32_t count);

                /** @brief Counts a device error on port @p number. */
                void noteDeviceError(uint8_t number);

                /**
                 * @brief Sets the connections by name (`SOURCE>DEST,...`) and makes
                 *        those whose ports exist. Connections made before stay.
                 * @return `Ok`, `Invalid` (longer than `MaxAutoConnect` - 1).
                 */
                Error setAutoConnect(StringView list);

        private:
                struct Port {
                                bool      used = false;
                                Kind      kind = Kind::Virtual;
                                uint8_t   sides = 0;
                                char      name[MaxName] = {};
                                MidiSink *sink = nullptr;
                                Counters  counters;
                };

                struct Tap {
                                MidiSink *sink = nullptr;
                                uint8_t   port = NoPort;
                                bool      reading = false;
                };

                bool        isPortLocked(uint8_t number, uint8_t side) const;
                Error       connectLocked(uint8_t source, uint8_t destination);
                void        autoConnectLocked();
                void        findAllLocked(StringView name, bool (&out)[MaxPorts]) const;
                static void setName(char (&out)[MaxName], StringView name);

                Mutex      _lock;
                Port       _ports[MaxPorts];
                Connection _connections[MaxConnections];
                size_t     _connectionCount = 0;
                Tap        _taps[MaxTaps];
                char       _autoConnect[MaxAutoConnect] = {};
};

BMDS_NAMESPACE_END
