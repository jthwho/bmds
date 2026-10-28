/**
 * @file      midisequence.h
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
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A Standard MIDI File's channel messages in time order (`devplan/mediaplayer.md` §12),
 *        for a player that sends them to a port itself: the media player to `/dev/midi`.
 *
 * Parsed by TinyMidiLoader (as `MidiDecoder` parses), its tempo map applied: each message has its
 * time in milliseconds from the start. Kept as an array (8 bytes a message, PSRAM), so a time is
 * found by a binary search. A file is untrusted input: one the parser refuses, or with no
 * message, is `Invalid`.
 *
 * @par Thread Safety
 * Not thread-safe: one thread at a time.
 */
class MidiSequence {
        public:
                /** @brief A channel message and its time. */
                struct Message {
                                uint32_t ms = 0;     ///< From the start.
                                uint8_t  length = 0; ///< 2 or 3.
                                uint8_t  data[3] = {};
                };

                /** @brief The largest file taken. */
                static constexpr size_t MaxFileBytes = 4u << 20;

                /** @brief The most messages `stateAt()` gives (16 channels' worth). */
                static constexpr size_t MaxStateMessages = 16 * 16;

                MidiSequence() = default;
                ~MidiSequence() { clear(); }
                MidiSequence(const MidiSequence &) = delete;
                MidiSequence &operator=(const MidiSequence &) = delete;

                /**
                 * @brief Parses @p file (in place of what it held).
                 * @return `Ok`, `Invalid` (not a MIDI file, damaged, too big, no message), `NoMem`.
                 */
                Error load(ByteSpan file);

                /** @brief Forgets the file (its memory goes). */
                void clear();

                /** @brief Returns its messages. */
                size_t count() const { return _count; }

                /** @brief Returns message @p index (below `count()`). */
                const Message &message(size_t index) const { return _messages[index]; }

                /** @brief Returns the last message's time (0 when empty). */
                uint32_t lengthMs() const { return _count > 0 ? _messages[_count - 1].ms : 0; }

                /** @brief Returns the first message at or after @p ms (`count()` if none). */
                size_t indexAt(uint32_t ms) const;

                /**
                 * @brief Fills @p out with the messages that set every channel as it is at @p ms (its
                 *        bank, program, modulation, volume, pan, expression, reverb, chorus, the bend's
                 *        range and the bend), without its notes: what a seek sends first.
                 * @return How many (at most `MaxStateMessages`, and @p out's size).
                 */
                size_t stateAt(uint32_t ms, Span<Message> out) const;

        private:
                Message *_messages = nullptr; // PSRAM
                size_t   _count = 0;
};

BMDS_NAMESPACE_END
