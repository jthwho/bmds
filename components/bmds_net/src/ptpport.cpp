/**
 * @file      ptpport.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * PTP's ordinary clock port (devplan/network.md §31): the state machine, the foreign masters
 * and BMCA, the master's Announce, Sync and Follow_Up and its Delay_Resp, the slave's
 * measurements of offset and path delay feeding the servo.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/ptp.h>

BMDS_NAMESPACE_BEGIN

namespace Ptp {

        namespace {

                // Times from the wire are anyone's: their sums and differences are kept within
                // ±73 years (a quarter of int64's range), so nothing after them can overflow.
                constexpr int64_t Limit = INT64_MAX / 4;

                int64_t clamp(int64_t v) {
                        return v > Limit ? Limit : v < -Limit ? -Limit : v;
                }

                int64_t sum(int64_t a, int64_t b) {
                        int64_t r;
                        if (__builtin_add_overflow(a, b, &r)) return a > 0 ? Limit : -Limit;
                        return clamp(r);
                }

                int64_t diff(int64_t a, int64_t b) {
                        int64_t r;
                        if (__builtin_sub_overflow(a, b, &r)) return a > b ? Limit : -Limit;
                        return clamp(r);
                }

                bool isSlaveSide(Port::State s) {
                        return s == Port::State::Slave || s == Port::State::Uncalibrated;
                }

        } // namespace

        Port::Port(Io &io, const ClockIdentity &identity, const Config &config) : _io(io), _config(config) {
                _identity.clock = identity;
                _identity.port = 1;
                // The random numbers (the timers' spread) differ per clock: FNV-1a of the identity.
                uint32_t h = 2166136261u;
                for (uint8_t b : identity.bytes) h = (h ^ b) * 16777619u;
                _random = h != 0 ? h : 1;
        }

        const char *Port::stateName(State s) {
                switch (s) {
                        case State::Disabled: return "disabled";
                        case State::Listening: return "listening";
                        case State::Master: return "master";
                        case State::Passive: return "passive";
                        case State::Uncalibrated: return "uncalibrated";
                        case State::Slave: return "slave";
                }
                return "?";
        }

        uint32_t Port::random() {
                // xorshift32
                uint32_t x = _random;
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                _random = x;
                return x;
        }

        void Port::start(int64_t nowNs) {
                _foreignCount = 0;
                _counts = Counts{};
                _status = Status{};
                _servo.configure(_config.servo, _config.logSyncInterval);
                _servo.reset(_io.frequency());
                _status.frequencyPpb = _io.frequency();
                resetMeasurements();
                setState(State::Listening, nowNs);
                restartAnnounceTimeout(nowNs);
                return;
        }

        void Port::stop() {
                _status.state = State::Disabled;
                _announceTimeoutAt = _announceAt = _syncAt = _delayReqAt = _syncTimeoutAt = Never;
                _syncPending = false;
                _foreignCount = 0;
                resetMeasurements();
                return;
        }

        void Port::setConfig(const Config &config, int64_t nowNs) {
                const bool intervals = config.logSyncInterval != _config.logSyncInterval ||
                                       config.servo.firstStepNs != _config.servo.firstStepNs ||
                                       config.servo.stepNs != _config.servo.stepNs ||
                                       config.servo.maxFrequencyPpb != _config.servo.maxFrequencyPpb;
                const bool domain = config.domain != _config.domain;
                _config = config;
                if (_status.state == State::Disabled) return;
                if (intervals) _servo.configure(_config.servo, _config.logSyncInterval);
                if (domain) {
                        // Another domain's masters are strangers: start over.
                        start(nowNs);
                        return;
                }
                if (_status.state == State::Master) {
                        _status.grandmaster = ownDataset();
                        _announceAt = nowNs; // the new attributes at once
                }
                decide(nowNs, false);
                return;
        }

        Dataset Port::ownDataset() const {
                Dataset d;
                d.priority1 = _config.priority1;
                d.quality = _config.quality;
                if (_config.slaveOnly) d.quality.clockClass = 255;
                d.priority2 = _config.priority2;
                d.grandmaster = _identity.clock;
                d.stepsRemoved = 0;
                d.sender = _identity;
                d.receiver = _identity;
                return d;
        }

        bool Port::isFromParent(const Header &h) const {
                return h.source == _status.parent;
        }

        void Port::setState(State s, int64_t nowNs) {
                const State was = _status.state;
                if (s == was) return;
                ++_counts.stateChanges;
                if (was == State::Master) {
                        _announceAt = _syncAt = Never;
                        _syncPending = false;
                }
                if (isSlaveSide(was)) holdOver();
                _status.state = s;
                switch (s) {
                        case State::Master:
                                _announceTimeoutAt = Never;
                                _delayReqAt = Never;
                                _announceAt = nowNs;
                                _syncAt = nowNs;
                                _status.parent = PortIdentity{};
                                _status.grandmaster = ownDataset();
                                _status.utcOffset = _io.utcOffset();
                                _status.timeFlags = _config.timeFlags;
                                _status.haveOffset = false;
                                _status.offsetNs = 0;
                                _status.meanPathDelayNs = 0;
                                break;
                        case State::Uncalibrated:
                        case State::Slave:
                                if (_delayReqAt == Never) _delayReqAt = nowNs + delayReqInterval(nowNs);
                                break;
                        default:
                                _delayReqAt = Never;
                                _syncTimeoutAt = Never;
                                break;
                }
                return;
        }

        void Port::restartAnnounceTimeout(int64_t nowNs) {
                const int64_t interval = intervalNs(_config.logAnnounceInterval);
                // Spread within an interval, so two clocks started together don't time out together.
                const int64_t spread = static_cast<int64_t>(random() % static_cast<uint32_t>(interval / 1000)) * 1000;
                _announceTimeoutAt = nowNs + static_cast<int64_t>(_config.announceReceiptTimeout) * interval + spread;
                return;
        }

        void Port::resetMeasurements() {
                _haveHold = false;
                _lockedAt = Never;
                _syncHalf = 0;
                _haveSync = false;
                _delayReqState = 0;
                _haveT4 = false;
                _delayCount = 0;
                _delayNext = 0;
                _masterDelayLog = _config.logMinDelayReqInterval;
                _status.haveOffset = false;
                _status.offsetNs = 0;
                _status.meanPathDelayNs = 0;
                return;
        }

        void Port::ageForeign(int64_t nowNs) {
                const int64_t window = static_cast<int64_t>(ForeignWindow) * intervalNs(_config.logAnnounceInterval);
                size_t        kept = 0;
                for (size_t i = 0; i < _foreignCount; ++i) {
                        Foreign &f = _foreign[i];
                        if (f.heard == 0 || f.heardNs[0] < nowNs - window) continue; // silent for the window: gone
                        f.qualified = f.heard >= ForeignThreshold && f.heardNs[ForeignThreshold - 1] >= nowNs - window;
                        if (kept != i) _foreign[kept] = f;
                        ++kept;
                }
                _foreignCount = kept;
                return;
        }

        void Port::noteAnnounce(const Message &m, int64_t nowNs) {
                if (m.stepsRemoved >= 255) return;
                Foreign *f = nullptr;
                for (size_t i = 0; i < _foreignCount; ++i) {
                        if (_foreign[i].port == m.header.source) f = &_foreign[i];
                }
                if (f == nullptr) {
                        if (_foreignCount < MaxForeign) {
                                f = &_foreign[_foreignCount++];
                        } else {
                                // Full: the one heard from longest ago gives way.
                                f = &_foreign[0];
                                for (size_t i = 1; i < _foreignCount; ++i) {
                                        if (_foreign[i].heardNs[0] < f->heardNs[0]) f = &_foreign[i];
                                }
                        }
                        *f = Foreign{};
                        f->port = m.header.source;
                }
                f->dataset = datasetOf(m, _identity);
                for (size_t i = ForeignThreshold - 1; i > 0; --i) f->heardNs[i] = f->heardNs[i - 1];
                f->heardNs[0] = nowNs;
                if (f->heard < ForeignThreshold) ++f->heard;
                if (_status.state != State::Master && _status.state != State::Listening && isFromParent(m.header)) {
                        restartAnnounceTimeout(nowNs);
                        _status.grandmaster = f->dataset;
                        _status.utcOffset.seconds = m.utcOffset;
                        _status.utcOffset.valid = (m.header.flags & UtcOffsetValid) != 0;
                        _status.utcOffset.leap61 = (m.header.flags & Leap61) != 0;
                        _status.utcOffset.leap59 = (m.header.flags & Leap59) != 0;
                        _status.timeFlags = m.header.flags & 0x00ff;
                }
                decide(nowNs, false);
                return;
        }

        // IEEE 1588-2008 §9.3.3, for an ordinary clock's one port.
        void Port::decide(int64_t nowNs, bool timedOut) {
                ageForeign(nowNs);
                const Foreign *best = nullptr;
                for (size_t i = 0; i < _foreignCount; ++i) {
                        if (!_foreign[i].qualified) continue;
                        if (best == nullptr || compare(_foreign[i].dataset, best->dataset) < 0) best = &_foreign[i];
                }
                const Dataset own = ownDataset();
                if (best == nullptr) {
                        if (_config.slaveOnly) {
                                if (_status.state != State::Listening) {
                                        setState(State::Listening, nowNs);
                                        restartAnnounceTimeout(nowNs);
                                }
                                return;
                        }
                        if (timedOut || _status.state == State::Master) setState(State::Master, nowNs);
                        return; // otherwise wait for the timeout
                }
                const bool weAreBetter = !_config.slaveOnly && compare(own, best->dataset) < 0;
                if (weAreBetter) {
                        setState(State::Master, nowNs);
                        return;
                }
                if (!_config.slaveOnly && own.quality.clockClass <= 127) {
                        // A primary-class clock never follows another: passive.
                        if (_status.state != State::Passive || !(_status.parent == best->port)) {
                                _status.parent = best->port;
                                setState(State::Passive, nowNs);
                                restartAnnounceTimeout(nowNs);
                        }
                        _status.grandmaster = best->dataset;
                        return;
                }
                if (!isSlaveSide(_status.state) || !(_status.parent == best->port)) {
                        if (isSlaveSide(_status.state)) holdOver();
                        ++_counts.parentChanges;
                        _status.parent = best->port;
                        resetMeasurements();
                        _servo.reset(_io.frequency()); // holdover: start from the frequency we have
                        setState(State::Uncalibrated, nowNs);
                        restartAnnounceTimeout(nowNs);
                }
                _status.grandmaster = best->dataset;
                return;
        }

        void Port::receive(Span<const uint8_t> bytes, int64_t rxNs, int64_t nowNs) {
                Message       m;
                const Verdict v = parse(bytes, m);
                if (v != Verdict::Ok && v != Verdict::Unsupported) {
                        ++_counts.ignored;
                        return;
                }
                if (m.header.domain != _config.domain || m.header.transportSpecific != 0 ||
                    m.header.source.clock == _identity.clock) {
                        ++_counts.ignored;
                        return;
                }
                ++_counts.received[static_cast<uint8_t>(m.header.type) & 0x0f];
                if (v != Verdict::Ok || _status.state == State::Disabled) return;
                switch (m.header.type) {
                        case MessageType::Announce: noteAnnounce(m, nowNs); break;
                        case MessageType::Sync: onSync(m, rxNs, nowNs); break;
                        case MessageType::FollowUp: onFollowUp(m, nowNs); break;
                        case MessageType::DelayReq: onDelayReq(m, rxNs); break;
                        case MessageType::DelayResp: onDelayResp(m, nowNs); break;
                        default: break;
                }
                return;
        }

        void Port::onSync(const Message &m, int64_t rxNs, int64_t nowNs) {
                if (!isSlaveSide(_status.state) || !isFromParent(m.header)) return;
                const int64_t correction = correctionNs(m.header);
                if ((m.header.flags & TwoStep) == 0) {
                        _syncHalf = 0;
                        syncMeasured(sum(m.timeNs, correction), rxNs, nowNs);
                        return;
                }
                if (_syncHalf == 2 && _syncSeqHeard == m.header.sequence) {
                        _syncHalf = 0;
                        syncMeasured(sum(_followUpT1, correction), rxNs, nowNs);
                        return;
                }
                _syncHalf = 1;
                _syncSeqHeard = m.header.sequence;
                _syncRxNs = rxNs;
                _syncCorrection = correction;
                return;
        }

        void Port::onFollowUp(const Message &m, int64_t nowNs) {
                if (!isSlaveSide(_status.state) || !isFromParent(m.header)) return;
                const int64_t t1 = sum(m.timeNs, correctionNs(m.header));
                if (_syncHalf == 1 && _syncSeqHeard == m.header.sequence) {
                        _syncHalf = 0;
                        syncMeasured(sum(t1, _syncCorrection), _syncRxNs, nowNs);
                        return;
                }
                // Before its Sync (they came by different queues).
                _syncHalf = 2;
                _syncSeqHeard = m.header.sequence;
                _followUpT1 = t1;
                return;
        }

        int64_t Port::medianDelay() const {
                int64_t v[DelayDepth];
                for (size_t i = 0; i < _delayCount; ++i) v[i] = _delays[i];
                for (size_t i = 1; i < _delayCount; ++i) { // insertion sort: seven at most
                        const int64_t x = v[i];
                        size_t        j = i;
                        for (; j > 0 && v[j - 1] > x; --j) v[j] = v[j - 1];
                        v[j] = x;
                }
                return _delayCount == 0 ? 0 : v[_delayCount / 2];
        }

        // No master to follow any more: the rate as averaged, without the last samples' noise.
        void Port::holdOver() {
                if (_lockedAt == Never) return; // never locked: nothing better than what is set
                _status.frequencyPpb = _haveHold ? _holdPpb : _servo.frequency();
                _io.setFrequency(_status.frequencyPpb);
                return;
        }

        void Port::stepped() {
                // Times taken before the step don't mix with those after.
                ++_counts.steps;
                ++_epoch;
                _syncHalf = 0;
                _haveSync = false;
                _delayReqState = 0;
                _haveT4 = false;
                return;
        }

        void Port::syncMeasured(int64_t t1, int64_t t2, int64_t nowNs) {
                _syncTimeoutAt = nowNs + static_cast<int64_t>(SyncReceiptTimeout) * intervalNs(_config.logSyncInterval);
                _lastT1 = t1;
                _lastT2 = t2;
                _lastSyncEpoch = _epoch;
                _haveSync = true;
                if (_delayCount == 0) return; // no path delay yet: no offset
                const int64_t delay = medianDelay();
                const int64_t offset = diff(diff(t2, t1), delay);
                _status.offsetNs = offset;
                _status.meanPathDelayNs = delay;
                _status.haveOffset = true;
                const double ppb = _servo.sample(offset, t2);
                _status.servo = _servo.state();
                switch (_servo.state()) {
                        case Servo::State::Unlocked: break;
                        case Servo::State::Jump:
                                _io.step(-offset);
                                stepped();
                                _io.setFrequency(ppb);
                                _status.frequencyPpb = ppb;
                                break;
                        case Servo::State::Locked: {
                                _io.setFrequency(ppb);
                                _status.frequencyPpb = ppb;
                                const double alpha = static_cast<double>(intervalNs(_config.logSyncInterval)) /
                                                     static_cast<double>(HoldoverTimeNs);
                                if (_lockedAt == Never) _lockedAt = nowNs;
                                if (nowNs - _lockedAt >= HoldoverSettleNs) {
                                        _holdPpb = _haveHold ? _holdPpb + (ppb - _holdPpb) * alpha : _servo.frequency();
                                        _haveHold = true;
                                }
                                if (_status.state == State::Uncalibrated) setState(State::Slave, nowNs);
                                break;
                        }
                }
                return;
        }

        void Port::delayMeasured(int64_t t4) {
                _delayReqState = 0;
                _haveT4 = false;
                if (!_haveSync || _lastSyncEpoch != _delayReqEpoch || _delayReqEpoch != _epoch) return;
                int64_t delay = (diff(_lastT2, _lastT1) + diff(t4, _delayReqT3)) / 2;
                if (delay < 0) delay = 0; // the stamps' noise on a short path
                _delays[_delayNext] = delay;
                _delayNext = (_delayNext + 1) % DelayDepth;
                if (_delayCount < DelayDepth) ++_delayCount;
                _status.meanPathDelayNs = medianDelay();
                return;
        }

        void Port::onDelayReq(const Message &m, int64_t rxNs) {
                if (_status.state != State::Master) return;
                Message r;
                r.header = headerFor(MessageType::DelayResp, _config.logMinDelayReqInterval);
                r.header.sequence = m.header.sequence;
                r.header.correction = m.header.correction;
                r.timeNs = rxNs;
                r.requesting = m.header.source;
                (void)send(r, nullptr);
                return;
        }

        void Port::onDelayResp(const Message &m, int64_t nowNs) {
                (void)nowNs;
                if (!isSlaveSide(_status.state) || !isFromParent(m.header)) return;
                if (!(m.requesting == _identity) || m.header.sequence != _delayReqSeqOut || _delayReqState == 0) return;
                if (m.header.logInterval >= -7 && m.header.logInterval <= 7) _masterDelayLog = m.header.logInterval;
                const int64_t t4 = diff(m.timeNs, correctionNs(m.header));
                if (_delayReqState == 2) {
                        delayMeasured(t4);
                } else {
                        _delayRespT4 = t4; // the request's stamp is still to come
                        _haveT4 = true;
                }
                return;
        }

        void Port::transmitted(uint32_t ticket, int64_t txNs, int64_t nowNs) {
                (void)nowNs;
                if (_syncPending && ticket == _syncTicket) {
                        _syncPending = false;
                        if (_status.state != State::Master) return;
                        Message f;
                        f.header = headerFor(MessageType::FollowUp, _config.logSyncInterval);
                        f.header.sequence = _syncTicketSeq;
                        f.timeNs = txNs;
                        (void)send(f, nullptr);
                        return;
                }
                if (_delayReqState == 1 && ticket == _delayReqTicket) {
                        _delayReqT3 = txNs;
                        _delayReqState = 2;
                        if (_haveT4) delayMeasured(_delayRespT4);
                }
                return;
        }

        Header Port::headerFor(MessageType type, int8_t logInterval) {
                Header h;
                h.type = type;
                h.domain = _config.domain;
                h.source = _identity;
                h.logInterval = logInterval;
                return h;
        }

        bool Port::send(Message &m, uint32_t *ticket) {
                uint8_t      bytes[MaxMessageBytes];
                const size_t n = build(m, Span<uint8_t>(bytes, sizeof(bytes)));
                const auto   type = static_cast<uint8_t>(m.header.type) & 0x0f;
                const bool   ok = n != 0 && _io.send(isEvent(m.header.type), Span<const uint8_t>(bytes, n), ticket);
                if (ok) {
                        ++_counts.sent[type];
                } else {
                        ++_counts.sendFailures;
                }
                return ok;
        }

        void Port::sendAnnounce() {
                const UtcOffset utc = _io.utcOffset();
                _status.utcOffset = utc;
                Message m;
                m.header = headerFor(MessageType::Announce, _config.logAnnounceInterval);
                m.header.sequence = _announceSeq++;
                m.header.flags = static_cast<uint16_t>(_config.timeFlags & 0x00f8);
                if (utc.valid) m.header.flags |= UtcOffsetValid;
                if (utc.leap61) m.header.flags |= Leap61;
                if (utc.leap59) m.header.flags |= Leap59;
                m.timeNs = _io.now();
                m.utcOffset = utc.seconds;
                const Dataset own = ownDataset();
                m.priority1 = own.priority1;
                m.quality = own.quality;
                m.priority2 = own.priority2;
                m.grandmaster = own.grandmaster;
                m.stepsRemoved = 0;
                m.timeSource = _config.timeSource;
                (void)send(m, nullptr);
                return;
        }

        void Port::sendSync(int64_t nowNs) {
                (void)nowNs;
                if (_syncPending) ++_counts.followUpsMissed; // the last one's stamp never came
                _syncPending = false;
                Message m;
                m.header = headerFor(MessageType::Sync, _config.logSyncInterval);
                m.header.sequence = _syncSeq++;
                m.header.flags = TwoStep;
                m.timeNs = _io.now();
                uint32_t ticket = 0;
                if (send(m, &ticket)) {
                        _syncTicket = ticket;
                        _syncTicketSeq = m.header.sequence;
                        _syncPending = true;
                }
                return;
        }

        void Port::sendDelayReq(int64_t nowNs) {
                (void)nowNs;
                if (!_haveSync) return; // a Sync first: the delay needs its pair
                Message m;
                m.header = headerFor(MessageType::DelayReq, 0x7f);
                m.header.sequence = _delayReqSeq++;
                m.timeNs = _io.now();
                uint32_t ticket = 0;
                _delayReqSeqOut = m.header.sequence;
                _delayReqEpoch = _epoch;
                _haveT4 = false;
                _delayReqState = 0;
                if (send(m, &ticket)) {
                        _delayReqTicket = ticket;
                        _delayReqState = 1;
                }
                return;
        }

        // Uniform over [0, 2^(log+1)] s, as IEEE 1588-2008 §9.5.11.2 says; never less than 1/16 of it.
        int64_t Port::delayReqInterval(int64_t nowNs) {
                (void)nowNs;
                const int64_t  base = intervalNs(_masterDelayLog);
                const uint64_t r = (uint64_t(random()) << 32) | random();
                const int64_t  v = static_cast<int64_t>(r % static_cast<uint64_t>(2 * base + 1));
                return v < base / 16 ? base / 16 : v;
        }

        void Port::advance(int64_t nowNs) {
                if (_status.state == State::Disabled) return;
                if (_announceTimeoutAt <= nowNs) {
                        ++_counts.announceTimeouts;
                        _announceTimeoutAt = Never;
                        if (_status.state != State::Listening) {
                                // The parent fell silent: forget it, and decide without it.
                                for (size_t i = 0; i < _foreignCount; ++i) {
                                        if (_foreign[i].port == _status.parent) _foreign[i].heard = 0;
                                }
                        }
                        decide(nowNs, true);
                        if (_status.state != State::Master && _announceTimeoutAt == Never)
                                restartAnnounceTimeout(nowNs);
                }
                if (_announceAt <= nowNs) {
                        sendAnnounce();
                        const int64_t interval = intervalNs(_config.logAnnounceInterval);
                        _announceAt += interval;
                        if (_announceAt <= nowNs) _announceAt = nowNs + interval;
                }
                if (_syncAt <= nowNs) {
                        sendSync(nowNs);
                        const int64_t interval = intervalNs(_config.logSyncInterval);
                        _syncAt += interval;
                        if (_syncAt <= nowNs) _syncAt = nowNs + interval;
                }
                if (_syncTimeoutAt <= nowNs) {
                        _syncTimeoutAt = Never;
                        holdOver(); // the servo carries on from where it was when they come back
                }
                if (_delayReqAt <= nowNs) {
                        sendDelayReq(nowNs);
                        _delayReqAt = nowNs + delayReqInterval(nowNs);
                }
                return;
        }

        int64_t Port::deadline() const {
                int64_t d = _announceTimeoutAt;
                if (_announceAt < d) d = _announceAt;
                if (_syncAt < d) d = _syncAt;
                if (_delayReqAt < d) d = _delayReqAt;
                if (_syncTimeoutAt < d) d = _syncTimeoutAt;
                return d;
        }

} // namespace Ptp

BMDS_NAMESPACE_END
