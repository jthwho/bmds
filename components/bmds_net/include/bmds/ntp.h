/**
 * @file      ntp.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * NTP's arithmetic (devplan/network.md §10, §22): packets, NTP's time stamps,
 * a sample's offset and delay, the clock filter and the discipline. Pure:
 * host-tested, and the parser fuzzed (tests/fuzz/ntp.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/datetime.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The client side of NTP (RFC 5905) as plain functions and values:
 *        what the kernel's NTP client (bmds_net) computes, without its
 *        sockets, timers or clock.
 *
 * @par Thread Safety
 * Functions are pure; `Filter` and `Discipline` are plain values (the
 * owner's lock covers them).
 */
namespace Ntp {

        /** @brief NTP's UDP port. */
        constexpr uint16_t Port = 123;

        /** @brief A packet without extensions (all a client needs). */
        constexpr size_t PacketBytes = 48;

        /** @brief Seconds from NTP's epoch (1900) to Unix's (1970). */
        constexpr int64_t UnixEpochSeconds = 2'208'988'800;

        /** @brief The largest root delay or dispersion believed (16 s, RFC 5905's MAXDISP). */
        constexpr int64_t MaxRootNs = 16 * Timestamp::NsPerSecond;

        /** @brief Returns @p t as NTP's 64-bit time stamp (32.32 seconds in its era). */
        uint64_t toNtp(Timestamp t);

        /**
         * @brief Returns NTP's 64-bit time stamp @p v as a time: a seconds field
         *        below 2^31 is era 1 (from 2036), so 1968 to 2104 read right.
         */
        Timestamp fromNtp(uint64_t v);

        /** @brief Returns NTP's 32-bit short format (16.16 seconds) in nanoseconds. */
        int64_t shortToNs(uint32_t v);

        /**
         * @brief Writes a client's request (version 4, mode 3) into @p packet,
         *        @p cookie as its transmit time stamp: the reply echoes it as its
         *        origin (random bits, not the time: RFC 9109).
         */
        void buildRequest(uint8_t (&packet)[PacketBytes], uint64_t cookie);

        /** @brief What `parse()` made of a packet. Only `Ok` is a time to use. */
        enum class Verdict : uint8_t {
                Ok,             ///< A server's answer to our request.
                Short,          ///< Fewer than `PacketBytes` bytes.
                NotServer,      ///< Not mode 4 (a server's).
                Version,        ///< Version 0, or beyond 4.
                NotOurs,        ///< Its origin isn't our request's cookie.
                Kiss,           ///< Stratum 0: a kiss-o'-death (`Reply::kiss`).
                Unsynchronised, ///< Leap indicator 3, or stratum beyond 15.
                Bogus,          ///< A time stamp zero, or a root delay or dispersion beyond `MaxRootNs`.
        };

        /** @brief Returns a short lowercase name for @p v ("ok", "kiss", ...). */
        const char *verdictName(Verdict v);

        /** @brief A server's reply, as `parse()` read it. */
        struct Reply {
                        uint8_t   leap = 0;             ///< Leap indicator (0-3).
                        uint8_t   version = 0;          ///< 1-4.
                        uint8_t   stratum = 0;          ///< 1-15 (0: a kiss).
                        int8_t    poll = 0;             ///< The server's poll exponent.
                        int8_t    precision = 0;        ///< The server's precision exponent.
                        int64_t   rootDelayNs = 0;      ///< To the reference clock, both ways.
                        int64_t   rootDispersionNs = 0; ///< The server's error bound.
                        uint32_t  referenceId = 0;      ///< As sent (network order's bytes, big end first).
                        char      kiss[5] = {};         ///< Stratum 0's code ("RATE", "DENY", ...).
                        Timestamp receive;              ///< T2: the request reached the server.
                        Timestamp transmit;             ///< T3: the reply left it.
        };

        /**
         * @brief Reads @p packet as the reply to the request carrying @p cookie.
         *
         * Extensions and a MAC after the 48 bytes are ignored. A kiss-o'-death
         * fills `reply.kiss` and answers `Kiss`.
         *
         * @return `Ok`, or why the packet isn't a time to use.
         */
        Verdict parse(Span<const uint8_t> packet, uint64_t cookie, Reply &reply);

        /** @brief One exchange's result. */
        struct Sample {
                        int64_t offsetNs = 0; ///< The server's time less ours (positive: we're behind).
                        int64_t delayNs = 0;  ///< The round trip, less the server's time holding it.
                        int64_t monoNs = 0;   ///< When it was taken (monotonic).
        };

        /**
         * @brief Returns the sample of an exchange: @p t1 sent, @p t2 received
         *        by the server, @p t3 sent back, @p t4 received, at monotonic
         *        @p monoNs. A negative delay (clocks' noise) counts as 0.
         */
        Sample sampleOf(Timestamp t1, Timestamp t2, Timestamp t3, Timestamp t4, int64_t monoNs);

        /**
         * @brief A server's clock filter (RFC 5905 §10, simplified): its last
         *        `Depth` samples; the best is the one with the least delay plus
         *        `AgingPpm` of its age, so an old sample gives way.
         */
        class Filter {
                public:
                        /** @brief Samples kept. */
                        static constexpr size_t Depth = 8;

                        /** @brief How fast an old sample's worth goes (RFC 5905's PHI, 15 ppm). */
                        static constexpr int64_t AgingPpm = 15;

                        constexpr Filter() = default;

                        /** @brief Adds @p s, the oldest giving way when full. */
                        void add(const Sample &s);

                        /** @brief Forgets every sample. */
                        void clear();

                        /** @brief Returns the samples kept. */
                        size_t count() const { return _count; }

                        /** @brief Returns the latest sample (zero when empty). */
                        Sample newest() const;

                        /** @brief Returns the best sample at monotonic @p nowNs (zero when empty). */
                        Sample best(int64_t nowNs) const;

                        /** @brief Returns true if the newest sample is the best at @p nowNs. */
                        bool newestIsBest(int64_t nowNs) const;

                        /**
                         * @brief Returns the RMS of the other samples' offsets from the
                         *        best's at @p nowNs (0 with fewer than two).
                         */
                        int64_t jitterNs(int64_t nowNs) const;

                private:
                        size_t bestIndex(int64_t nowNs) const;
                        size_t newestIndex() const { return (_next + Depth - 1) % Depth; }

                        Sample _samples[Depth] = {};
                        size_t _count = 0;
                        size_t _next = 0;
        };

        /**
         * @brief The clock's discipline: what to do with each offset measured
         *        from the chosen server, and how often to poll.
         *
         * - The clock unset, or an offset beyond `StepNs`: a step.
         * - Otherwise a slew of the offset, and, when at least
         *   `FrequencyIntervalNs` passed since the last update, a frequency
         *   correction: the offset the pending slew doesn't explain is the
         *   rate's error over that time, taken in by interval / (interval +
         *   `FrequencyTimeNs`), so a short interval's noise moves it little
         *   (6% at 64 s, half at 1024 s).
         * - Each such interval also estimates the frequency that would have kept the
         *   clock: the one set then, plus its error. From `MinEstimates` on, when their
         *   mean is beyond 4 standard errors of it from the frequency set, the mean is
         *   taken whole (a rate left far off, as by PTP following a slewing master, is
         *   gone in minutes, not hours); the estimates start again then, after a step,
         *   and every `MaxEstimates`.
         * - The poll exponent runs from `MinPoll` (64 s) to `MaxPoll` (1024 s):
         *   up one after `StableUpdates` updates within 4 jitters (at least
         *   `StableNs`), back to `MinPoll` on a step or an offset beyond that.
         */
        class Discipline {
                public:
                        /** @brief An offset beyond this is stepped (RFC 5905's STEPT, 128 ms). */
                        static constexpr int64_t StepNs = 128'000'000;

                        /** @brief The least time between updates that corrects the frequency. */
                        static constexpr int64_t FrequencyIntervalNs = 32 * Timestamp::NsPerSecond;

                        /** @brief The frequency's time constant: an interval this long takes half its error. */
                        static constexpr int64_t FrequencyTimeNs = 1024 * Timestamp::NsPerSecond;

                        /** @brief The frequency correction's bound (the clock's: ±500 ppm). */
                        static constexpr int32_t MaxFrequencyPpb = 500'000;

                        /** @brief The least offset bound counted stable. */
                        static constexpr int64_t StableNs = 1'000'000;

                        /** @brief Frequency estimates before their mean may be taken whole. */
                        static constexpr int64_t MinEstimates = 3;

                        /** @brief Estimates kept before they start again. */
                        static constexpr int64_t MaxEstimates = 16;

                        /** @brief Stable updates before the poll doubles. */
                        static constexpr unsigned int StableUpdates = 4;

                        /** @brief Poll exponents: 2^6 = 64 s to 2^10 = 1024 s. */
                        static constexpr int MinPoll = 6;
                        static constexpr int MaxPoll = 10; ///< See `MinPoll`.

                        /** @brief What to do with the clock. */
                        struct Action {
                                        bool    step = false;         ///< Step by `offsetNs` (else slew it).
                                        int64_t offsetNs = 0;         ///< The offset to step or slew.
                                        bool    setFrequency = false; ///< Set `frequencyPpb`.
                                        int32_t frequencyPpb = 0;     ///< The new frequency correction.
                        };

                        constexpr Discipline() = default;

                        /**
                         * @brief Takes @p offsetNs, measured at monotonic @p monoNs.
                         * @param clockSet Whether the clock has a source (unset: a step).
                         * @param frequencyPpb The clock's frequency correction now.
                         * @param slewRemainingNs The clock's slew still pending.
                         * @param jitterNs The chosen server's jitter.
                         * @return What to do; the poll exponent is updated.
                         */
                        Action update(int64_t monoNs, int64_t offsetNs, bool clockSet, int32_t frequencyPpb,
                                      int64_t slewRemainingNs, int64_t jitterNs);

                        /** @brief Back to the start (the clock keeps its frequency). */
                        void reset();

                        /** @brief Returns the poll exponent (the interval is 2^poll s). */
                        int poll() const { return _poll; }

                        /** @brief Doubles the poll (a server's `RATE` kiss), up to `MaxPoll`. */
                        void slowDown();

                private:
                        void restartEstimates();

                        int64_t      _lastNs = 0;
                        bool         _haveLast = false;
                        int64_t      _estimates = 0; // the frequency's, since they started again
                        int64_t      _estimateSum = 0;
                        int64_t      _estimateSquares = 0;
                        int          _poll = MinPoll;
                        unsigned int _stable = 0;
        };

} // namespace Ntp

BMDS_NAMESPACE_END
