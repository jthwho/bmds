/**
 * @file      trackclock.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where the sound being heard is (`devplan/mediaplayer.md` §6b), pure:
 *        the tracks the playback thread put on the stream, each from a frame
 *        of the stream and a frame of its file, and the stream's frames heard,
 *        from its status (the device's clock, never a timer's count).
 *
 * A track started after another without a break (gapless) is marked ahead
 * of what is heard; the one shown changes when the frames heard reach it.
 * A start that threw away what was queued (a seek, a new track chosen, or a
 * stream configured again, which counts from 0) forgets the marks before.
 * Until the frames heard reach the first mark, its track is shown at its
 * first frame (a seek's target, while the old sound goes).
 *
 * @par Thread Safety
 * A value with one owner.
 */
class TrackClock {
        public:
                /** @brief No track (`shown()`, `newest()`). */
                static constexpr uint32_t NoTrack = 0;

                /** @brief Marks kept: the one heard and those ahead of it. */
                static constexpr size_t MaxMarks = 4;

                /** @brief What is heard. */
                struct Now {
                                uint32_t track = NoTrack; ///< `NoTrack`: nothing.
                                uint64_t frame = 0;       ///< In its file.
                };

                /** @brief What a stream's status says (`AudioFile::Status`). */
                struct Sample {
                                uint64_t playedFrames = 0; ///< Taken from the ring.
                                int64_t  timestampNs = 0;  ///< When (0: not yet).
                                uint32_t lagFrames = 0;    ///< Taken but not heard yet: the device's buffers.
                                uint32_t rateHz = 0;       ///< The stream's.
                                bool     playing = false;  ///< Moving on (not paused, waiting or idle).
                };

                /** @brief The longest time `heard()` moves a sample on (a late status). */
                static constexpr uint32_t MaxExtrapolateMs = 50;

                /** @brief Forgets every mark. */
                void clear() {
                        _count = 0;
                        return;
                }

                /**
                 * @brief Track @p track's frame @p fileFrame goes to the stream at its
                 *        frame @p streamFrame; @p fresh: what was queued was thrown away
                 *        (or played out), so the marks before are forgotten. Ahead of
                 *        `MaxMarks` marks, the oldest goes.
                 */
                void start(uint32_t track, uint64_t streamFrame, uint64_t fileFrame, bool fresh);

                /** @brief Returns what is heard at the stream's frame @p heard, forgetting the marks it passed. */
                Now at(uint64_t heard);

                /** @brief Returns the track shown at the last `at()` (or the first marked since), or `NoTrack`. */
                uint32_t shown() const { return _count > 0 ? _marks[0].track : NoTrack; }

                /** @brief Returns the track marked last, or `NoTrack`. */
                uint32_t newest() const { return _count > 0 ? _marks[_count - 1].track : NoTrack; }

                /** @brief Returns the stream's frame heard at @p nowNs, from @p sample. */
                static uint64_t heard(const Sample &sample, int64_t nowNs);

        private:
                struct Mark {
                                uint32_t track = NoTrack;
                                uint64_t streamFrame = 0;
                                uint64_t fileFrame = 0;
                };

                Mark   _marks[MaxMarks];
                size_t _count = 0;
};

/**
 * @brief A media time as text, pure: `M:SS`, or `H:MM:SS` from an hour.
 */
namespace TimeText {

        /** @brief The longest text `format()` writes, with its NUL ("9999999:59:59"). */
        constexpr size_t MaxText = 24;

        /**
         * @brief Writes @p ms (rounded down to a second) into @p out, NUL-terminated.
         * @return @p out.
         */
        const char *format(uint64_t ms, Span<char> out);

} // namespace TimeText

BMDS_NAMESPACE_END
