/**
 * @file      trackclock.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "trackclock.h"

#include <cstdio>

BMDS_NAMESPACE_BEGIN

void TrackClock::start(uint32_t track, uint64_t streamFrame, uint64_t fileFrame, bool fresh) {
        if (fresh) _count = 0;
        if (_count == MaxMarks) {
                for (size_t i = 1; i < _count; ++i) _marks[i - 1] = _marks[i];
                --_count;
        }
        _marks[_count++] = Mark{track, streamFrame, fileFrame};
        return;
}

TrackClock::Now TrackClock::at(uint64_t heard) {
        if (_count == 0) return Now();
        // The last mark reached; those before it are over.
        size_t reached = 0;
        while (reached + 1 < _count && _marks[reached + 1].streamFrame <= heard) ++reached;
        if (reached > 0) {
                for (size_t i = reached; i < _count; ++i) _marks[i - reached] = _marks[i];
                _count -= reached;
        }
        const Mark &m = _marks[0];
        Now         now;
        now.track = m.track;
        now.frame = m.fileFrame + (heard > m.streamFrame ? heard - m.streamFrame : 0);
        return now;
}

uint64_t TrackClock::heard(const Sample &sample, int64_t nowNs) {
        uint64_t frames = sample.playedFrames;
        if (sample.playing && sample.timestampNs > 0 && nowNs > sample.timestampNs && sample.rateHz > 0) {
                uint64_t ns = uint64_t(nowNs - sample.timestampNs);
                if (ns > uint64_t(MaxExtrapolateMs) * 1000000) ns = uint64_t(MaxExtrapolateMs) * 1000000;
                frames += ns * sample.rateHz / 1000000000;
        }
        return frames > sample.lagFrames ? frames - sample.lagFrames : 0;
}

const char *TimeText::format(uint64_t ms, Span<char> out) {
        if (out.size() == 0) return out.data();
        const uint64_t s = ms / 1000;
        if (s >= 3600) {
                std::snprintf(out.data(), out.size(), "%llu:%02u:%02u", static_cast<unsigned long long>(s / 3600),
                              unsigned(s / 60 % 60), unsigned(s % 60));
        } else {
                std::snprintf(out.data(), out.size(), "%u:%02u", unsigned(s / 60), unsigned(s % 60));
        }
        return out.data();
}

BMDS_NAMESPACE_END
