/**
 * @file      applemidi.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/result.h>
#include <bmds/rtpmidi.h>
#include <bmds/span.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief AppleMIDI's sessions (the session protocol RTP-MIDI is spoken with:
 *        macOS, iOS, rtpMIDI on Windows, rtpmidid; `devplan/audio.md` §23), on one
 *        pair of UDP ports, control and data (control + 1). Portable: no sockets,
 *        no clock; the caller gives it datagrams and the time, and it answers
 *        through a `Transport`.
 *
 * - **Accepting:** an invitation (`IN`) on the control port is answered `OK`
 *   (or `NO` when not accepting, or full), then the same on the data port; the
 *   session is then up.
 * - **Inviting:** `invite()` sends `IN` to a peer's control port, then its data
 *   port, each asked again every second, 12 times.
 * - **Clock sync** (`CK`): the inviter starts it, 6 times 1.5 s apart, then every
 *   10 s; either side's round trip and offset are kept.
 * - **Receiver feedback** (`RS`): the last sequence number received, at most once
 *   a second, so the peer can trim its journal.
 * - **The end:** `BY` either way, `end()`, or nothing heard from the peer for 60 s.
 *
 * Times are `int64_t` nanoseconds on one monotonic clock; AppleMIDI's are its
 * 100 µs units.
 *
 * @par Thread Safety
 * One task at a time (the network task, under its lock).
 */
class AppleMidi {
        public:
                /** @brief Sessions at once. */
                static constexpr size_t MaxSessions = 4;

                /** @brief The longest name kept, with its NUL (a peer's longer one is cut). */
                static constexpr size_t MaxName = 32;

                /** @brief The two ports. */
                enum class Channel : uint8_t {
                        Control, ///< Invitations, the end, receiver feedback.
                        Data,    ///< MIDI (RTP), and clock sync.
                };

                /** @brief An IPv4 address (as the caller keeps it) and a port. */
                struct Endpoint {
                                uint32_t address = 0;
                                uint16_t port = 0;
                                bool     operator==(const Endpoint &other) const {
                                        return address == other.address && port == other.port;
                                }
                };

                /** @brief A session's state. */
                enum class State : uint8_t {
                        Free,         ///< No session.
                        Inviting,     ///< Our `IN` to the control port, waiting.
                        InvitingData, ///< Our `IN` to the data port, waiting.
                        Accepting,    ///< Their control `IN` answered; waiting for the data port's.
                        Up,           ///< Established.
                };

                /** @brief Why a session ended (`Transport::down()`). */
                enum class End : uint8_t {
                        Ours,     ///< `end()`.
                        Theirs,   ///< Their `BY`.
                        Refused,  ///< Their `NO` to our invitation.
                        NoAnswer, ///< Our invitation went unanswered.
                        Silent,   ///< Nothing heard for 60 s.
                };

                /** @brief What is known of a session. */
                struct Info {
                                State    state = State::Free;
                                bool     inviter = false;    ///< We invited.
                                char     name[MaxName] = {}; ///< The peer's name.
                                Endpoint control;            ///< The peer's control port.
                                Endpoint data;               ///< The peer's data port.
                                uint32_t ssrc = 0;           ///< The peer's.
                                int64_t  upNs = 0;           ///< When it came up.
                                int64_t  offsetUnits = 0;    ///< Their clock less ours, 100 µs units (0: unknown).
                                uint32_t roundTripUnits = 0; ///< The last sync's round trip, 100 µs units.
                                uint32_t syncs = 0;          ///< Clock syncs completed.
                                uint32_t packetsIn = 0;      ///< MIDI packets received.
                                uint32_t packetsOut = 0;     ///< MIDI packets sent.
                                uint32_t lost = 0;           ///< Gaps in their sequence numbers.
                                uint32_t journals = 0;       ///< Packets with a journal (skipped).
                };

                /** @brief Where the sessions speak and say what happened. Called from its methods. */
                class Transport {
                        public:
                                /** @brief Sends @p bytes from our @p channel port to @p to. */
                                virtual void send(Channel channel, const Endpoint &to, ByteSpan bytes) = 0;

                                /** @brief Session @p session is up. */
                                virtual void up(size_t session) = 0;

                                /** @brief Session @p session, up or invited by us, ends (`info()` still has it). */
                                virtual void down(size_t session, End why) = 0;

                                /** @brief A MIDI packet from session @p session. */
                                virtual void received(size_t session, const RtpMidi::Packet &packet, int64_t nowNs) = 0;

                        protected:
                                ~Transport() = default;
                };

                /**
                 * @brief Starts: our @p ssrc, @p name (cut to 63 bytes), whether
                 *        invitations are taken, and @p seed for invitation tokens.
                 */
                void start(Transport &transport, uint32_t ssrc, StringView name, bool accept, uint32_t seed);

                /** @brief Whether invitations are taken. */
                void setAccept(bool accept) { _accept = accept; }

                /** @brief Our name, sent in invitations and answers. */
                void setName(StringView name);

                /** @brief Takes a datagram from @p from on our @p channel port. */
                void receive(Channel channel, const Endpoint &from, ByteSpan datagram, int64_t nowNs);

                /**
                 * @brief Invites the peer whose control port is @p control.
                 * @return The session, `Exists` (one with it already), or `Full`.
                 */
                Result<size_t> invite(const Endpoint &control, int64_t nowNs);

                /** @brief Ends @p session (`BY` to the peer; `down(Ours)` if it was up or invited). */
                void end(size_t session);

                /** @brief Ends every session. */
                void endAll();

                /**
                 * @brief Does what is due at @p nowNs (invitations again, syncs, feedback,
                 *        timeouts).
                 * @return When it next has something to do (`INT64_MAX`: nothing).
                 */
                int64_t tick(int64_t nowNs);

                /** @brief Returns @p session's state. */
                const Info &info(size_t session) const { return _sessions[session].info; }

                /** @brief Returns true if @p session is up. */
                bool isUp(size_t session) const {
                        return session < MaxSessions && _sessions[session].info.state == State::Up;
                }

                /** @brief Returns our SSRC. */
                uint32_t ssrc() const { return _ssrc; }

                /** @brief Returns @p nowNs as RTP's timestamp (100 µs units, wrapping). */
                static uint32_t rtpTime(int64_t nowNs) { return static_cast<uint32_t>(units(nowNs)); }

                /** @brief Takes @p session's next RTP sequence number for a packet sent to it. */
                uint16_t takeSequence(size_t session);

        private:
                struct Session {
                                Info     info;
                                uint32_t token = 0;      // the invitation's
                                int64_t  dueNs = 0;      // an invitation asked again, or an acceptance given up
                                uint32_t tries = 0;      // invitations sent
                                int64_t  heardNs = 0;    // the last packet from the peer
                                int64_t  nextSyncNs = 0; // the inviter's next CK0
                                int64_t  nextFeedbackNs = 0;
                                bool     feedbackDue = false;
                                bool     sequenced = false; // a sequence number seen
                                uint16_t lastSequence = 0;  // theirs
                                uint16_t sequence = 0;      // ours, the next sent
                };

                static int64_t units(int64_t ns) { return ns / 100'000; }

                void   command(Channel channel, const Endpoint &from, ByteSpan d, int64_t nowNs);
                void   rtp(const Endpoint &from, ByteSpan d, int64_t nowNs);
                void   invitation(Channel channel, const Endpoint &from, uint32_t token, uint32_t ssrc, StringView name,
                                  int64_t nowNs);
                void   sync(size_t s, const uint8_t *d, int64_t nowNs);
                void   sendSession(Channel channel, const Endpoint &to, const char (&command)[3], uint32_t token,
                                   bool withName);
                void   sendSync(size_t s, uint8_t count, const uint64_t (&times)[3]);
                void   finish(size_t s, End why, bool tell);
                size_t bySsrc(uint32_t ssrc) const;
                size_t byToken(uint32_t token, State state) const;
                size_t freeSlot() const;
                uint32_t nextToken();

                Transport *_transport = nullptr;
                uint32_t   _ssrc = 0;
                uint32_t   _seed = 1;
                bool       _accept = true;
                char       _name[64] = {};
                Session    _sessions[MaxSessions];
                uint8_t    _out[16 + sizeof(_name)] = {}; // a session packet being sent
};

BMDS_NAMESPACE_END
