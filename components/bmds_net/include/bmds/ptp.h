/**
 * @file      ptp.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * IEEE 1588-2008 (PTP version 2) as portable code (devplan/network.md §10, §31): the messages,
 * the best master clock algorithm, the servo and one ordinary clock's port. No sockets, timers
 * or clocks: the owner feeds messages, stamps and the time in, and the port acts through `Io`.
 * Host-tested against a simulated network, and the parser fuzzed (tests/fuzz/ptp.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/ethernet.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief PTP: an ordinary clock with one port, master or slave as the best master clock
 *        algorithm decides; two-step, end-to-end delay.
 *
 * Times on the PTP clock are nanoseconds since PTP's epoch (1970, TAI), as `int64_t`.
 *
 * @par Thread Safety
 * Functions are pure; `Servo` and `Port` are plain objects (the owner's lock covers them).
 */
namespace Ptp {

        /** @brief UDP ports: event messages (stamped) and general ones. */
        constexpr uint16_t EventPort = 319;
        constexpr uint16_t GeneralPort = 320; ///< See `EventPort`.

        /** @brief The EtherType of PTP over Ethernet. */
        constexpr uint16_t EtherType = 0x88f7;

        /** @brief The groups messages go to: 224.0.1.129, and 01-1B-19-00-00-00 on Ethernet. */
        constexpr uint32_t   Group = 0xe0000181;
        constexpr MacAddress GroupMac = {{0x01, 0x1b, 0x19, 0x00, 0x00, 0x00}}; ///< See `Group`.

        /** @brief The largest message this code builds (an Announce). */
        constexpr size_t MaxMessageBytes = 64;

        /** @brief Returns 2^@p log seconds in nanoseconds (@p log from -7 to 7). */
        int64_t intervalNs(int8_t log);

        /** @brief The message types. */
        enum class MessageType : uint8_t {
                Sync = 0x0,
                DelayReq = 0x1,
                PdelayReq = 0x2,
                PdelayResp = 0x3,
                FollowUp = 0x8,
                DelayResp = 0x9,
                PdelayRespFollowUp = 0xa,
                Announce = 0xb,
                Signaling = 0xc,
                Management = 0xd,
        };

        /** @brief Returns true for the types sent with a departure stamp (event messages). */
        constexpr bool isEvent(MessageType t) {
                return static_cast<uint8_t>(t) < 0x8;
        }

        /** @brief A clock's identity: EUI-64. */
        struct ClockIdentity {
                        uint8_t bytes[8] = {};

                        /** @brief Returns the identity of the interface with @p mac (FF-FE in the middle). */
                        static ClockIdentity fromMac(const MacAddress &mac);

                        bool isZero() const;
        };

        /** @brief Orders identities as unsigned big-endian numbers: < 0, 0 or > 0. */
        int  compare(const ClockIdentity &a, const ClockIdentity &b);
        bool operator==(const ClockIdentity &a, const ClockIdentity &b);

        /** @brief A port's identity: its clock and its number (1 for an ordinary clock). */
        struct PortIdentity {
                        ClockIdentity clock;
                        uint16_t      port = 0;
        };

        /** @brief Orders port identities: clock, then port. */
        int  compare(const PortIdentity &a, const PortIdentity &b);
        bool operator==(const PortIdentity &a, const PortIdentity &b);

        /** @brief A clock's quality, as Announce messages carry it. */
        struct ClockQuality {
                        uint8_t  clockClass = 248; ///< 6: locked to a primary reference; 248: default; 255: slave only.
                        uint8_t  accuracy = 0xfe;  ///< 0x20-0x31 (25 ns to 10 s), 0xfe unknown.
                        uint16_t variance = 0xffff; ///< offsetScaledLogVariance.
        };

        /** @brief The flags (flagField: its first byte in the high bits). */
        enum Flags : uint16_t {
                AlternateMaster = 0x0100,
                TwoStep = 0x0200,
                Unicast = 0x0400,
                Leap61 = 0x0001,
                Leap59 = 0x0002,
                UtcOffsetValid = 0x0004,
                PtpTimescale = 0x0008,
                TimeTraceable = 0x0010,
                FrequencyTraceable = 0x0020,
        };

        /** @brief Every message's first 34 bytes. */
        struct Header {
                        MessageType  type = MessageType::Sync;
                        uint8_t      transportSpecific = 0;
                        uint8_t      version = 2;
                        uint16_t     length = 0;
                        uint8_t      domain = 0;
                        uint16_t     flags = 0;
                        int64_t      correction = 0; ///< Nanoseconds times 2^16.
                        PortIdentity source;
                        uint16_t     sequence = 0;
                        int8_t       logInterval = 0x7f;
        };

        /** @brief A message: the header and its type's body. */
        struct Message {
                        Header header;
                        /** Sync and Delay_Req: originTimestamp; Follow_Up: preciseOriginTimestamp;
                         *  Delay_Resp: receiveTimestamp; Announce: originTimestamp. */
                        int64_t      timeNs = 0;
                        PortIdentity requesting; ///< Delay_Resp: whose Delay_Req.
                        // Announce:
                        int16_t       utcOffset = 0;
                        uint8_t       priority1 = 128;
                        ClockQuality  quality;
                        uint8_t       priority2 = 128;
                        ClockIdentity grandmaster;
                        uint16_t      stepsRemoved = 0;
                        uint8_t       timeSource = 0xa0;
        };

        /** @brief Returns @p header's correction in nanoseconds (the fraction dropped). */
        int64_t correctionNs(const Header &header);

        /**
         * @brief Writes @p m into @p out; the header's length is set here.
         * @return The bytes written, or 0 for a type this code doesn't build or too small an @p out.
         */
        size_t build(const Message &m, Span<uint8_t> out);

        /** @brief What `parse()` made of a message. Only `Ok` is one to use. */
        enum class Verdict : uint8_t {
                Ok,
                Short,       ///< Shorter than its header, its length field or its type's body.
                Version,     ///< Not version 2.
                Unsupported, ///< Read as far as the header (peer delay, signaling, management, unknown).
                Bogus,       ///< A time stamp's nanoseconds past a second, or seconds past 2262.
        };

        /** @brief Returns a short lowercase name for @p v. */
        const char *verdictName(Verdict v);

        /** @brief Returns a short name for @p t ("sync", "announce", ...). */
        const char *typeName(MessageType t);

        /**
         * @brief Reads @p bytes into @p m. A TLV after the body is ignored. `Unsupported` still
         *        fills the header.
         */
        Verdict parse(Span<const uint8_t> bytes, Message &m);

        /**
         * @brief What the best master clock algorithm compares of a clock: its grandmaster's
         *        attributes, and where the Announce came from (IEEE 1588-2008 §9.3.4).
         */
        struct Dataset {
                        uint8_t       priority1 = 128;
                        ClockQuality  quality;
                        uint8_t       priority2 = 128;
                        ClockIdentity grandmaster;
                        uint16_t      stepsRemoved = 0;
                        PortIdentity  sender;   ///< The port that sent it (ourselves for our own).
                        PortIdentity  receiver; ///< The port that received it (ours).
        };

        /** @brief Returns the dataset of an Announce @p m received on @p receiver. */
        Dataset datasetOf(const Message &m, const PortIdentity &receiver);

        /**
         * @brief The dataset comparison algorithm: < 0 if @p a is better, > 0 if @p b is, 0 if
         *        they can't be told apart (the same Announce twice: an error in the standard).
         *        ±2 when the grandmasters differ or steps removed differ by more than one, ±1 when
         *        only the topology decides ("better by topology").
         */
        int compare(const Dataset &a, const Dataset &b);

        /**
         * @brief linuxptp's PI servo: a frequency estimate from the first two offsets, a step
         *        if the first offset is beyond `firstStepNs`, then proportional and integral
         *        corrections whose gains follow the sync interval.
         *
         * Offsets are the slave's time less the master's; the frequency returned is the
         * correction to apply (positive: run faster).
         */
        class Servo {
                public:
                        /** @brief Where it is. */
                        enum class State : uint8_t {
                                Unlocked, ///< Fewer than two samples.
                                Jump,     ///< Step the clock by -offset now, then locked.
                                Locked,
                        };

                        struct Config {
                                        int64_t firstStepNs = 20'000; ///< Step the first offset beyond this (0: never).
                                        int64_t stepNs = 0;           ///< Step any later offset beyond this (0: never).
                                        int32_t maxFrequencyPpb = 500'000; ///< The correction's bound.
                        };

                        constexpr Servo() = default;

                        /** @brief Sets the tuning and the sync interval (the gains follow it). */
                        void configure(const Config &config, int8_t logSyncInterval);

                        /** @brief Forgets the samples; @p frequencyPpb is where the integral starts. The next
                         *         offset beyond `firstStepNs` is stepped again (a new master). */
                        void reset(double frequencyPpb);

                        /**
                         * @brief Takes @p offsetNs measured at @p localNs (the slave's clock).
                         * @return The frequency correction to set, in ppb; `state()` says whether
                         *         to step first.
                         */
                        double sample(int64_t offsetNs, int64_t localNs);

                        /**
                         * @brief Returns the integral's frequency: the clock's rate error as
                         *        estimated, without the last offset's proportional part (what to
                         *        hold to once there is no master).
                         */
                        double frequency() const { return -_drift; }

                        State  state() const { return _state; }
                        double kp() const { return _kp; }
                        double ki() const { return _ki; }

                private:
                        Config  _config;
                        double  _kp = 0.7;
                        double  _ki = 0.3;
                        double  _drift = 0;
                        int64_t _offset0 = 0;
                        int64_t _local0 = 0;
                        int     _count = 0;
                        bool    _firstUpdate = true;
                        State   _state = State::Unlocked;
        };

        /**
         * @brief An ordinary clock's one port: the state machine, BMCA over the foreign masters
         *        heard, the master's messages and the slave's measurements and servo.
         *
         * The owner calls `receive()` with each message and its arrival on the PTP clock,
         * `transmitted()` with each event message's departure, and `advance()` at `deadline()`
         * (monotonic nanoseconds, the owner's own clock). Everything else happens through `Io`.
         */
        class Port {
                public:
                        /** @brief The port's states (IEEE 1588-2008 §9.2.5; no pre-master). */
                        enum class State : uint8_t {
                                Disabled,
                                Listening,
                                Master,
                                Passive,
                                Uncalibrated,
                                Slave,
                        };

                        /** @brief The UTC offset a master announces. */
                        struct UtcOffset {
                                        int16_t seconds = 37; ///< TAI less UTC.
                                        bool    valid = false;
                                        bool    leap61 = false; ///< The last minute of today has 61 seconds.
                                        bool    leap59 = false; ///< ... 59 seconds.
                        };

                        /** @brief What the port does and announces. */
                        struct Config {
                                        uint8_t       domain = 0;
                                        uint8_t       priority1 = 128;
                                        uint8_t       priority2 = 128;
                                        ClockQuality  quality;
                                        bool          slaveOnly = false;
                                        int8_t        logAnnounceInterval = 1;     ///< The media profile's: 2 s.
                                        int8_t        logSyncInterval = -3;        ///< 125 ms.
                                        int8_t        logMinDelayReqInterval = -3; ///< 125 ms on average.
                                        uint8_t       announceReceiptTimeout = 3;  ///< Announce intervals.
                                        uint8_t       timeSource = 0xa0;           ///< Internal oscillator.
                                        uint16_t      timeFlags = PtpTimescale;    ///< `TimeTraceable` etc. as master.
                                        Servo::Config servo;
                        };

                        /** @brief What the port does to the world. Implemented by the owner. */
                        class Io {
                                public:
                                        /**
                                         * @brief Sends @p bytes to the group: on the event port with a
                                         *        departure stamp for `transmitted(*ticket)`, or the general one.
                                         * @return false if it couldn't (counted; the port carries on).
                                         */
                                        virtual bool send(bool event, Span<const uint8_t> bytes, uint32_t *ticket) = 0;

                                        /** @brief Returns the PTP clock's time now. */
                                        virtual int64_t now() = 0;

                                        /** @brief Adds @p ns to the PTP clock. */
                                        virtual void step(int64_t ns) = 0;

                                        /** @brief Sets the PTP clock's frequency correction (positive: faster). */
                                        virtual void setFrequency(double ppb) = 0;

                                        /** @brief Returns the PTP clock's frequency correction now. */
                                        virtual double frequency() = 0;

                                        /** @brief Returns the UTC offset to announce as master. */
                                        virtual UtcOffset utcOffset() = 0;

                                protected:
                                        ~Io() = default;
                        };

                        /** @brief Foreign masters remembered. */
                        static constexpr size_t MaxForeign = 8;

                        /** @brief Announces within the window that qualify a foreign master. */
                        static constexpr unsigned int ForeignThreshold = 2;

                        /** @brief The window, in announce intervals. */
                        static constexpr unsigned int ForeignWindow = 4;

                        /** @brief Path delays the median is taken over. */
                        static constexpr size_t DelayDepth = 7;

                        /** @brief The time constant of the frequency held to once no master is left. */
                        static constexpr int64_t HoldoverTimeNs = 32'000'000'000;

                        /** @brief Locked this long before the average starts (the pull-in left out). */
                        static constexpr int64_t HoldoverSettleNs = HoldoverTimeNs / 4;

                        /** @brief Sync intervals without one before a slave holds its rate (until they come back). */
                        static constexpr unsigned int SyncReceiptTimeout = 4;

                        /** @brief A foreign master heard. */
                        struct Foreign {
                                        PortIdentity port;
                                        Dataset      dataset;
                                        int64_t      heardNs[ForeignThreshold] =
                                                {};             ///< The last announces (monotonic), newest first.
                                        unsigned int heard = 0; ///< Announces held in `heardNs`.
                                        bool         qualified = false;
                                        UtcOffset    utcOffset;     ///< Its last Announce's,
                                        uint8_t      timeFlags = 0; ///< taken when it becomes the parent.
                        };

                        /** @brief Counters since `start()`. */
                        struct Counts {
                                        uint32_t received[16] = {}; ///< By message type.
                                        uint32_t sent[16] = {};     ///< By message type.
                                        uint32_t sendFailures = 0;
                                        uint32_t ignored = 0; ///< Not ours: domain, version, our own, a stranger's.
                                        uint32_t announceTimeouts = 0;
                                        uint32_t stateChanges = 0;
                                        uint32_t parentChanges = 0;
                                        uint32_t steps = 0;
                                        uint32_t followUpsMissed = 0; ///< A Sync whose stamp never came.
                        };

                        /** @brief What the port knows: its state, its parent and the last measurements. */
                        struct Status {
                                        State        state = State::Disabled;
                                        PortIdentity parent;        ///< Valid when slave, uncalibrated or passive.
                                        Dataset      grandmaster;   ///< Ourselves when master.
                                        UtcOffset    utcOffset;     ///< The parent's, or ours as master.
                                        uint16_t     timeFlags = 0; ///< The parent's time flags.
                                        int64_t      offsetNs = 0;  ///< Our time less the master's.
                                        int64_t      meanPathDelayNs = 0;
                                        double       frequencyPpb = 0;
                                        bool         haveOffset = false;
                                        Servo::State servo = Servo::State::Unlocked;
                        };

                        Port(Io &io, const ClockIdentity &identity, const Config &config);
                        Port(const Port &) = delete;
                        Port &operator=(const Port &) = delete;

                        /** @brief Starts listening at monotonic @p nowNs. */
                        void start(int64_t nowNs);

                        /** @brief Stops (disabled): forgets the foreign masters and the measurements. */
                        void stop();

                        /** @brief Applies @p config at @p nowNs: BMCA runs again, the servo keeps its frequency. */
                        void setConfig(const Config &config, int64_t nowNs);

                        /** @brief Takes a message received at @p rxNs on the PTP clock, at monotonic @p nowNs. */
                        void receive(Span<const uint8_t> bytes, int64_t rxNs, int64_t nowNs);

                        /** @brief Takes the departure @p txNs of the event message sent with @p ticket. */
                        void transmitted(uint32_t ticket, int64_t txNs, int64_t nowNs);

                        /** @brief Runs the timers due at monotonic @p nowNs. */
                        void advance(int64_t nowNs);

                        /** @brief Returns when `advance()` is next needed (monotonic), or INT64_MAX. */
                        int64_t deadline() const;

                        State               state() const { return _status.state; }
                        const Status       &status() const { return _status; }
                        const Counts       &counts() const { return _counts; }
                        const Config       &config() const { return _config; }
                        const PortIdentity &identity() const { return _identity; }
                        Span<const Foreign> foreign() const { return Span<const Foreign>(_foreign, _foreignCount); }

                        /** @brief Returns a short lowercase name for @p s. */
                        static const char *stateName(State s);

                private:
                        static constexpr int64_t Never = INT64_MAX;

                        Dataset  ownDataset() const;
                        bool     isFromParent(const Header &h) const;
                        void     setState(State s, int64_t nowNs);
                        void     decide(int64_t nowNs, bool timedOut);
                        void     noteAnnounce(const Message &m, int64_t nowNs);
                        void     ageForeign(int64_t nowNs);
                        void     takeTimeProperties(const Foreign &f);
                        void     restartAnnounceTimeout(int64_t nowNs);
                        void     resetMeasurements();
                        void     onSync(const Message &m, int64_t rxNs, int64_t nowNs);
                        void     onFollowUp(const Message &m, int64_t nowNs);
                        void     onDelayReq(const Message &m, int64_t rxNs);
                        void     onDelayResp(const Message &m, int64_t nowNs);
                        void     syncMeasured(int64_t t1, int64_t t2, int64_t nowNs);
                        void     delayMeasured(int64_t t4);
                        int64_t  medianDelay() const;
                        void     stepped();
                        void     holdOver();
                        void     sendAnnounce();
                        void     sendSync(int64_t nowNs);
                        void     sendDelayReq(int64_t nowNs);
                        bool     send(Message &m, uint32_t *ticket);
                        Header   headerFor(MessageType type, int8_t logInterval);
                        int64_t  delayReqInterval(int64_t nowNs);
                        uint32_t random();

                        Io          &_io;
                        PortIdentity _identity;
                        Config       _config;
                        Status       _status;
                        Counts       _counts;
                        Servo        _servo;

                        Foreign _foreign[MaxForeign];
                        size_t  _foreignCount = 0;

                        // Timers (monotonic; Never: off).
                        int64_t _announceTimeoutAt = Never;
                        int64_t _announceAt = Never;
                        int64_t _syncAt = Never;
                        int64_t _delayReqAt = Never;
                        int64_t _syncTimeoutAt = Never; // slave: Syncs stopped, hold the rate

                        // Sequence numbers.
                        uint16_t _announceSeq = 0;
                        uint16_t _syncSeq = 0;
                        uint16_t _delayReqSeq = 0;

                        // Master: the Sync waiting for its departure stamp.
                        uint32_t _syncTicket = 0;
                        uint16_t _syncTicketSeq = 0;
                        bool     _syncPending = false;

                        // Slave: the half of a two-step Sync first heard, matched by sequence.
                        uint16_t _syncSeqHeard = 0;
                        int64_t  _syncRxNs = 0;       // t2
                        int64_t  _syncCorrection = 0; // ns
                        int64_t  _followUpT1 = 0;     // a Follow_Up before its Sync
                        uint8_t  _syncHalf = 0;       // 0 none, 1 Sync waits for Follow_Up, 2 Follow_Up waits for Sync
                        uint32_t _epoch = 0;          // bumped at each step: older times don't mix with newer

                        // Slave: the last Sync measured (t1, t2) for the delay, and the Delay_Req out.
                        int64_t  _lastT1 = 0;
                        int64_t  _lastT2 = 0;
                        uint32_t _lastSyncEpoch = 0;
                        bool     _haveSync = false;
                        uint32_t _delayReqTicket = 0;
                        uint16_t _delayReqSeqOut = 0;
                        int64_t  _delayReqT3 = 0;
                        uint32_t _delayReqEpoch = 0;
                        uint8_t  _delayReqState = 0; // 0 none, 1 sent (no stamp yet), 2 stamped
                        int64_t  _delayRespT4 = 0;   // a Delay_Resp before its request's stamp
                        bool     _haveT4 = false;
                        int64_t  _delays[DelayDepth] = {};
                        size_t   _delayCount = 0;
                        size_t   _delayNext = 0;
                        int8_t   _masterDelayLog = -3; // Delay_Resp's logMessageInterval

                        double  _holdPpb = 0; // the frequency applied, averaged over HoldoverTimeNs
                        bool    _haveHold = false;
                        int64_t _lockedAt = INT64_MAX; // monotonic: the servo's first lock on this parent

                        uint32_t _random = 0;
        };

} // namespace Ptp

BMDS_NAMESPACE_END
