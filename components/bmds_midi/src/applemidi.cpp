/**
 * @file      applemidi.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * AppleMIDI's session protocol (devplan/audio.md §23). Its packets: two bytes
 * FF FF, a two-letter command, then (big-endian)
 * - IN, OK, NO, BY: the protocol version (2), the invitation's token, the
 *   sender's SSRC, and (IN, OK) its name, NUL-terminated;
 * - CK: the sender's SSRC, a count (0, 1, 2) and 3 bytes of padding, three
 *   64-bit times (100 µs units): the inviter's, the answer's, the inviter's;
 * - RS: the sender's SSRC, the last sequence number received, 2 bytes of padding.
 */

#include <climits>
#include <cstdio>
#include <cstring>
#include <bmds/applemidi.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint32_t Version = 2;
        constexpr size_t   SessionBytes = 16; // IN, OK, NO, BY without a name
        constexpr size_t   SyncBytes = 36;
        constexpr size_t   FeedbackBytes = 12;

        constexpr int64_t  InviteRetryNs = 1'000'000'000;
        constexpr uint32_t InviteTries = 12;
        constexpr int64_t  AcceptWaitNs = 5'000'000'000; // their data port's IN after the control's
        constexpr int64_t  SilentNs = 60'000'000'000;    // nothing heard: the session is gone
        constexpr int64_t  FirstSyncsNs = 1'500'000'000; // the first syncs, this far apart
        constexpr uint32_t FirstSyncs = 6;
        constexpr int64_t  SyncNs = 10'000'000'000;    // then
        constexpr int64_t  FeedbackNs = 1'000'000'000; // RS at most this often

        uint32_t be32(const uint8_t *p) {
                return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        uint64_t be64(const uint8_t *p) {
                return (uint64_t(be32(p)) << 32) | be32(p + 4);
        }

        void putBe32(uint8_t *p, uint32_t v) {
                p[0] = static_cast<uint8_t>(v >> 24);
                p[1] = static_cast<uint8_t>(v >> 16);
                p[2] = static_cast<uint8_t>(v >> 8);
                p[3] = static_cast<uint8_t>(v);
                return;
        }

        void putBe64(uint8_t *p, uint64_t v) {
                putBe32(p, static_cast<uint32_t>(v >> 32));
                putBe32(p + 4, static_cast<uint32_t>(v));
                return;
        }

        bool is(const uint8_t *d, const char (&command)[3]) {
                return d[2] == uint8_t(command[0]) && d[3] == uint8_t(command[1]);
        }

        // A name from a packet: up to its NUL or the end, cut to fit, printable ASCII kept.
        void copyName(StringView from, char *out, size_t size) {
                size_t n = 0;
                for (size_t i = 0; i < from.size() && from[i] != '\0' && n + 1 < size; ++i) {
                        const char c = from[i];
                        out[n++] = c >= 0x20 && c < 0x7f ? c : '?';
                }
                out[n] = '\0';
                return;
        }

} // namespace

void AppleMidi::start(Transport &transport, uint32_t ssrc, StringView name, bool accept, uint32_t seed) {
        _transport = &transport;
        _ssrc = ssrc;
        _accept = accept;
        _seed = seed != 0 ? seed : 1;
        setName(name);
        for (Session &s : _sessions) s = Session();
        return;
}

void AppleMidi::setName(StringView name) {
        copyName(name, _name, sizeof(_name));
        return;
}

uint32_t AppleMidi::nextToken() {
        // xorshift32: tokens only need to differ.
        _seed ^= _seed << 13;
        _seed ^= _seed >> 17;
        _seed ^= _seed << 5;
        return _seed;
}

size_t AppleMidi::bySsrc(uint32_t ssrc) const {
        for (size_t i = 0; i < MaxSessions; ++i)
                if (_sessions[i].info.state != State::Free && _sessions[i].info.ssrc == ssrc) return i;
        return MaxSessions;
}

size_t AppleMidi::byToken(uint32_t token, State state) const {
        for (size_t i = 0; i < MaxSessions; ++i)
                if (_sessions[i].info.state == state && _sessions[i].token == token) return i;
        return MaxSessions;
}

size_t AppleMidi::freeSlot() const {
        for (size_t i = 0; i < MaxSessions; ++i)
                if (_sessions[i].info.state == State::Free) return i;
        return MaxSessions;
}

void AppleMidi::sendSession(Channel channel, const Endpoint &to, const char (&command)[3], uint32_t token,
                            bool withName) {
        _out[0] = 0xff;
        _out[1] = 0xff;
        _out[2] = uint8_t(command[0]);
        _out[3] = uint8_t(command[1]);
        putBe32(_out + 4, Version);
        putBe32(_out + 8, token);
        putBe32(_out + 12, _ssrc);
        size_t n = SessionBytes;
        if (withName) {
                const size_t len = std::strlen(_name);
                std::memcpy(_out + n, _name, len + 1);
                n += len + 1;
        }
        _transport->send(channel, to, ByteSpan(_out, n));
        return;
}

void AppleMidi::sendSync(size_t s, uint8_t count, const uint64_t (&times)[3]) {
        _out[0] = 0xff;
        _out[1] = 0xff;
        _out[2] = 'C';
        _out[3] = 'K';
        putBe32(_out + 4, _ssrc);
        _out[8] = count;
        _out[9] = _out[10] = _out[11] = 0;
        for (size_t i = 0; i < 3; ++i) putBe64(_out + 12 + 8 * i, times[i]);
        _transport->send(Channel::Data, _sessions[s].info.data, ByteSpan(_out, SyncBytes));
        return;
}

// A session ends: BY to the peer if @p tell, down() if the transport knew of it.
void AppleMidi::finish(size_t s, End why, bool tell) {
        Session    &x = _sessions[s];
        const State was = x.info.state;
        if (was == State::Free) return;
        if (tell && was != State::Inviting) sendSession(Channel::Control, x.info.control, "BY", x.token, false);
        if (was != State::Accepting) _transport->down(s, why); // it was up, or our invitation (info still there)
        x = Session();
        return;
}

void AppleMidi::receive(Channel channel, const Endpoint &from, ByteSpan datagram, int64_t nowNs) {
        if (_transport == nullptr) return;
        const uint8_t *d = datagram.data();
        if (datagram.size() >= 4 && d[0] == 0xff && d[1] == 0xff) {
                command(channel, from, datagram, nowNs);
                return;
        }
        if (channel == Channel::Data) rtp(from, datagram, nowNs);
        return;
}

void AppleMidi::command(Channel channel, const Endpoint &from, ByteSpan datagram, int64_t nowNs) {
        const uint8_t *d = datagram.data();
        const size_t   n = datagram.size();
        if (is(d, "IN") || is(d, "OK") || is(d, "NO") || is(d, "BY")) {
                if (n < SessionBytes) return;
                const uint32_t   token = be32(d + 8);
                const uint32_t   ssrc = be32(d + 12);
                const StringView name(reinterpret_cast<const char *>(d + SessionBytes), n - SessionBytes);
                if (is(d, "IN")) {
                        invitation(channel, from, token, ssrc, name, nowNs);
                        return;
                }
                if (is(d, "BY")) {
                        const size_t s = bySsrc(ssrc);
                        if (s < MaxSessions) finish(s, End::Theirs, false);
                        return;
                }
                const State  waiting = channel == Channel::Control ? State::Inviting : State::InvitingData;
                const size_t s = byToken(token, waiting);
                if (s == MaxSessions) return;
                Session &x = _sessions[s];
                if (is(d, "NO")) {
                        finish(s, End::Refused, false);
                        return;
                }
                x.heardNs = nowNs;
                if (channel == Channel::Control) {
                        // Accepted on the control port: now the data port, the next one up.
                        x.info.ssrc = ssrc;
                        copyName(name, x.info.name, sizeof(x.info.name));
                        x.info.state = State::InvitingData;
                        x.info.data = Endpoint{x.info.control.address, uint16_t(x.info.control.port + 1)};
                        x.tries = 1;
                        x.dueNs = nowNs + InviteRetryNs;
                        sendSession(Channel::Data, x.info.data, "IN", x.token, true);
                        return;
                }
                x.info.data = from;
                x.info.state = State::Up;
                x.info.upNs = nowNs;
                x.nextSyncNs = nowNs; // the inviter syncs first, at once
                _transport->up(s);
                return;
        }
        if (is(d, "CK")) {
                if (n < SyncBytes || channel != Channel::Data) return;
                const size_t s = bySsrc(be32(d + 4));
                if (s < MaxSessions && _sessions[s].info.state == State::Up) sync(s, d, nowNs);
                return;
        }
        if (is(d, "RS")) {
                if (n < FeedbackBytes) return;
                const size_t s = bySsrc(be32(d + 4));
                if (s < MaxSessions) _sessions[s].heardNs = nowNs; // no journal is sent: nothing to trim
                return;
        }
        return; // a command we don't know
}

void AppleMidi::invitation(Channel channel, const Endpoint &from, uint32_t token, uint32_t ssrc, StringView name,
                           int64_t nowNs) {
        size_t s = bySsrc(ssrc);
        if (channel == Channel::Data) {
                // The second half: their data port, after the control port's.
                if (s == MaxSessions ||
                    (_sessions[s].info.state != State::Accepting && _sessions[s].info.state != State::Up) ||
                    _sessions[s].token != token) {
                        sendSession(Channel::Data, from, "NO", token, false);
                        return;
                }
                Session &x = _sessions[s];
                x.info.data = from;
                x.heardNs = nowNs;
                sendSession(Channel::Data, from, "OK", token, true);
                if (x.info.state == State::Accepting) {
                        x.info.state = State::Up;
                        x.info.upNs = nowNs;
                        _transport->up(s);
                }
                return;
        }
        // A new invitation; one from a peer we have a session with starts it again.
        if (s < MaxSessions) finish(s, End::Theirs, false);
        s = freeSlot();
        if (!_accept || s == MaxSessions) {
                sendSession(Channel::Control, from, "NO", token, false);
                return;
        }
        Session &x = _sessions[s];
        x = Session();
        x.info.state = State::Accepting;
        x.info.inviter = false;
        x.info.control = from;
        x.info.ssrc = ssrc;
        copyName(name, x.info.name, sizeof(x.info.name));
        x.token = token;
        x.heardNs = nowNs;
        x.dueNs = nowNs + AcceptWaitNs;
        sendSession(Channel::Control, from, "OK", token, true);
        return;
}

// Clock sync: count 0 asks (answered 1), 1 answers ours (finished with 2), 2 finishes theirs.
void AppleMidi::sync(size_t s, const uint8_t *d, int64_t nowNs) {
        Session      &x = _sessions[s];
        const uint8_t count = d[8];
        // The peer's times are whatever it sends: worked in unsigned arithmetic (wrapping), the
        // results as the two's complement differences they are.
        uint64_t       t[3] = {be64(d + 12), be64(d + 20), be64(d + 28)};
        const uint64_t now = uint64_t(units(nowNs));
        auto           middle = [](uint64_t a, uint64_t b) {
                return a / 2 + b / 2 + ((a & 1) + (b & 1)) / 2;
        };
        x.heardNs = nowNs;
        if (count == 0) {
                t[1] = now;
                sendSync(s, 1, t);
        } else if (count == 1) {
                t[2] = now;
                sendSync(s, 2, t);
                // t[0] and t[2] are ours, t[1] theirs, taken halfway (on average).
                x.info.roundTripUnits = uint32_t(t[2] - t[0]);
                x.info.offsetUnits = int64_t(t[1] - middle(t[0], t[2]));
                ++x.info.syncs;
        } else if (count == 2) {
                // t[0] and t[2] are theirs, t[1] ours.
                x.info.roundTripUnits = uint32_t(t[2] - t[0]);
                x.info.offsetUnits = int64_t(middle(t[0], t[2]) - t[1]);
                ++x.info.syncs;
        }
        return;
}

void AppleMidi::rtp(const Endpoint &from, ByteSpan datagram, int64_t nowNs) {
        RtpMidi::Packet p;
        if (RtpMidi::parse(datagram, p).isError()) return;
        const size_t s = bySsrc(p.ssrc);
        if (s == MaxSessions || _sessions[s].info.state != State::Up || _sessions[s].info.data.address != from.address)
                return;
        Session &x = _sessions[s];
        x.heardNs = nowNs;
        if (x.sequenced && p.sequence != uint16_t(x.lastSequence + 1)) {
                // Missed packets (a repeated or older one counts too: rare on a LAN).
                x.info.lost += uint16_t(p.sequence - x.lastSequence - 1) < 0x8000
                                       ? uint32_t(uint16_t(p.sequence - x.lastSequence - 1))
                                       : 1u;
        }
        x.sequenced = true;
        x.lastSequence = p.sequence;
        ++x.info.packetsIn;
        if (p.journal) ++x.info.journals;
        x.feedbackDue = true; // sent by tick(), at most once a second
        _transport->received(s, p, nowNs);
        return;
}

Result<size_t> AppleMidi::invite(const Endpoint &control, int64_t nowNs) {
        for (const Session &x : _sessions)
                if (x.info.state != State::Free && x.info.control == control) return makeError<size_t>(Error::Exists);
        const size_t s = freeSlot();
        if (s == MaxSessions) return makeError<size_t>(Error::Full);
        Session &x = _sessions[s];
        x = Session();
        x.info.state = State::Inviting;
        x.info.inviter = true;
        x.info.control = control;
        x.token = nextToken();
        x.tries = 1;
        x.dueNs = nowNs + InviteRetryNs;
        x.heardNs = nowNs;
        sendSession(Channel::Control, control, "IN", x.token, true);
        return makeResult(s);
}

void AppleMidi::end(size_t session) {
        if (session < MaxSessions) finish(session, End::Ours, true);
        return;
}

void AppleMidi::endAll() {
        for (size_t s = 0; s < MaxSessions; ++s) finish(s, End::Ours, true);
        return;
}

uint16_t AppleMidi::takeSequence(size_t session) {
        Session &x = _sessions[session];
        ++x.info.packetsOut;
        return x.sequence++;
}

int64_t AppleMidi::tick(int64_t nowNs) {
        int64_t next = INT64_MAX;
        auto    due = [&next](int64_t at) {
                if (at < next) next = at;
                return;
        };
        for (size_t s = 0; s < MaxSessions; ++s) {
                Session &x = _sessions[s];
                switch (x.info.state) {
                        case State::Free: break;
                        case State::Inviting:
                        case State::InvitingData:
                                if (nowNs >= x.dueNs) {
                                        if (x.tries >= InviteTries) {
                                                finish(s, End::NoAnswer, x.info.state == State::InvitingData);
                                                break;
                                        }
                                        const bool control = x.info.state == State::Inviting;
                                        sendSession(control ? Channel::Control : Channel::Data,
                                                    control ? x.info.control : x.info.data, "IN", x.token, true);
                                        ++x.tries;
                                        x.dueNs = nowNs + InviteRetryNs;
                                }
                                due(x.dueNs);
                                break;
                        case State::Accepting:
                                if (nowNs >= x.dueNs) {
                                        x = Session(); // their data port never came: forgotten
                                        break;
                                }
                                due(x.dueNs);
                                break;
                        case State::Up:
                                if (nowNs - x.heardNs >= SilentNs) {
                                        finish(s, End::Silent, true);
                                        break;
                                }
                                due(x.heardNs + SilentNs);
                                if (x.info.inviter) {
                                        if (nowNs >= x.nextSyncNs) {
                                                const uint64_t t[3] = {uint64_t(units(nowNs)), 0, 0};
                                                sendSync(s, 0, t);
                                                x.nextSyncNs =
                                                        nowNs + (x.info.syncs < FirstSyncs ? FirstSyncsNs : SyncNs);
                                        }
                                        due(x.nextSyncNs);
                                }
                                if (x.feedbackDue) {
                                        if (nowNs >= x.nextFeedbackNs) {
                                                _out[0] = 0xff;
                                                _out[1] = 0xff;
                                                _out[2] = 'R';
                                                _out[3] = 'S';
                                                putBe32(_out + 4, _ssrc);
                                                _out[8] = uint8_t(x.lastSequence >> 8);
                                                _out[9] = uint8_t(x.lastSequence);
                                                _out[10] = _out[11] = 0;
                                                _transport->send(Channel::Control, x.info.control,
                                                                 ByteSpan(_out, FeedbackBytes));
                                                x.feedbackDue = false;
                                                x.nextFeedbackNs = nowNs + FeedbackNs;
                                        } else {
                                                due(x.nextFeedbackNs);
                                        }
                                }
                                break;
                }
        }
        return next;
}

BMDS_NAMESPACE_END
