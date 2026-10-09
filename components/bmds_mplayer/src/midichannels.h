/**
 * @file      midichannels.h
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
#include <bmds/mididecoder.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief What the 16 channels of a MIDI file playing are doing, at the frame
 *        heard (`devplan/mediaplayer.md` §6h), pure: the decoder's activity
 *        queued as it decodes ahead, applied as the played frames reach it.
 *
 * Holds `QueueSize` waiting; past that it notes the overflow
 * (`hasOverflowed()`: the player asks the decoder for a snapshot) and drops
 * the newest.
 *
 * @par Thread Safety
 * A value with one owner.
 */
class MidiChannels {
        public:
                using Activity = MidiDecoder::Activity;

                static constexpr size_t   Channels = 16;
                static constexpr size_t   QueueSize = 1024;
                static constexpr uint32_t DefaultTempo = 500000; ///< µs a quarter: 120 beats a minute.

                /** @brief A channel's state. */
                struct Channel {
                                uint8_t program = 0;
                                uint8_t bankMsb = 0;
                                uint8_t bankLsb = 0;
                                uint8_t volume = 100;
                                uint8_t pan = 64;
                                uint8_t expression = 127;
                                uint8_t notes = 0;    ///< Keys sounding.
                                uint8_t loudest = 0;  ///< The highest velocity sounding.
                                uint8_t struck = 0;   ///< The highest velocity struck since `takeStruck()`.
                                bool    used = false; ///< Has played a note.
                };

                /** @brief Where in the music: bar and beat from 1, the tempo, the signature. */
                struct Position {
                                uint32_t bar = 1;
                                uint32_t beat = 1;
                                uint32_t tempo = DefaultTempo;
                                uint8_t  beatsPerBar = 4;
                                uint8_t  beatNote = 4;
                };

                /** @brief Channels at their start, the file's rate @p rateHz. */
                explicit MidiChannels(uint32_t rateHz = 48000);

                /** @brief Queues @p activity (in frame order). */
                void add(const Activity &activity);

                /** @brief Applies what is queued for frames up to @p frame. */
                void advanceTo(uint64_t frame);

                /** @brief Forgets everything (a new file, a seek before its snapshot). */
                void reset();

                /** @brief Returns true if activity was dropped since the last `reset()`. */
                bool hasOverflowed() const { return _overflowed; }

                /** @brief Returns channel @p channel (0 to 15). */
                const Channel &channel(size_t channel) const { return _channels[channel]; }

                /** @brief Returns channel @p channel's strikes since the last call, and clears them. */
                uint8_t takeStruck(size_t channel);

                /** @brief Returns where the music is at the last frame applied. */
                Position position() const;

        private:
                void apply(const Activity &activity);
                void resetChannels();
                void noteOff(Channel &c, size_t channel, uint8_t key);
                void beatsTo(uint64_t frame); // counts the beats up to frame at the tempo

                Channel  _channels[Channels];
                uint8_t  _velocity[Channels][128]; // 0: not sounding
                Activity _queue[QueueSize];
                size_t   _head = 0;
                size_t   _count = 0;
                bool     _overflowed = false;
                uint32_t _rateHz;
                uint32_t _tempo = DefaultTempo;
                uint8_t  _beatsPerBar = 4;
                uint8_t  _beatNote = 4;
                uint64_t _frame = 0;        // the last frame counted
                double   _beats = 0;        // beats (of the signature's note) since the start
                double   _barStartBeat = 0; // where bars are counted from (the last signature)
                uint32_t _barsBefore = 0;   // bars before the last signature
};

BMDS_NAMESPACE_END
