/**
 * @file      netmidi.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/applemidi.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief MIDI over the network (`devplan/audio.md` §23): RTP-MIDI with
 *        AppleMIDI's sessions, so macOS, iOS, rtpMIDI on Windows and rtpmidid
 *        reach the board's MIDI ports. Each session is a port
 *        (`MidiRouter::Kind::Net`, named as the peer) while it is up.
 *
 * While `midi/net/enabled` is on, the board takes UDP `midi/net/port` (control)
 * and the next (data), advertises `_apple-midi._udp` named `midi/net/name` (the
 * host's name if empty), takes invitations as `midi/net/accept` says, and
 * invites `midi/net/sessions` (`HOST[:PORT]`, comma separated) whenever they
 * aren't up, again after a failure (5 s, doubling to 5 minutes).
 *
 * The protocol runs on the network task (`NetUdp`, its timers); a thread
 * (`netmidi`, its stack in PSRAM) follows the settings and looks up the hosts
 * to invite. Nothing of it exists while it is off.
 *
 * Settings: `midi/net/enabled` (bool, false), `midi/net/port` (int, 5004),
 * `midi/net/name` (string, ""), `midi/net/accept` (enum `all|none`, `all`),
 * `midi/net/sessions` (string, "").
 *
 * @par Thread Safety
 * `init()` once, at boot; the rest from any task, without the network's core lock.
 */
namespace NetMidi {

        /** @brief A session (`sessions()`). */
        struct SessionInfo {
                        uint8_t          index = 0; ///< For `disconnect()`.
                        AppleMidi::State state = AppleMidi::State::Free;
                        bool             inviter = false; ///< We invited.
                        char             name[AppleMidi::MaxName] = {};
                        uint32_t         address = 0;     ///< The peer's, IPv4 in network order.
                        uint16_t         port = 0;        ///< The peer's control port.
                        uint8_t          midiPort = 0xff; ///< Its MIDI port while up (`MidiRouter::NoPort`).
                        uint32_t         roundTripUs = 0; ///< The last clock sync's.
                        uint32_t         syncs = 0;
                        uint32_t         packetsIn = 0;
                        uint32_t         packetsOut = 0;
                        uint32_t         lost = 0;
                        uint32_t         journals = 0; ///< Packets with a recovery journal (skipped).
                        uint32_t         dropped = 0;  ///< Events dropped by a full ring.
                        int64_t          upNs = 0;     ///< When it came up.
        };

        /** @brief The service's state (`status()`). */
        struct Status {
                        bool     enabled = false; ///< `midi/net/enabled`.
                        bool     running = false; ///< Its ports are open.
                        uint16_t port = 0;        ///< The control port.
                        bool     accepting = false;
                        char     name[64] = {}; ///< The name it goes by.
        };

        /**
         * @brief Boot stage: declares the settings and follows them. After the network
         *        stack and MIDI.
         * @return `Ok`, or the error declaring the settings.
         */
        Error init();

        /** @brief Returns the service's state. */
        Status status();

        /** @brief Copies the sessions into @p out. @return How many. */
        size_t sessions(Span<SessionInfo> out);

        /**
         * @brief Invites @p host (a name or a dotted quad) on @p port, its control port.
         * @return `Ok` (asked; `sessions()` shows how it goes), `InvalidState` (not
         *         running), `Exists`, `Full`, or the lookup's error. @blocking
         */
        Error connect(StringView host, uint16_t port);

        /**
         * @brief Ends session @p index.
         * @return `Ok`, or `NotExist`.
         */
        Error disconnect(uint8_t index);

} // namespace NetMidi

BMDS_NAMESPACE_END
