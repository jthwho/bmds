/**
 * @file      midievent.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief One MIDI message, or a piece of a SysEx, with where and when it
 *        came from (`devplan/audio.md` §21). The layout is the ABI's
 *        `bmds_midi_event_t`.
 *
 * A message is whole: a status byte and its data bytes, never running
 * status. A SysEx comes in pieces in order, `F0` first in the first and `F7`
 * last in the last. A real-time byte (`F8`-`FF`) is an event of its own.
 */
struct MidiEvent {
                /** @brief The most bytes in one event (a SysEx piece). */
                static constexpr size_t MaxBytes = 5;

                int64_t timeNs = 0;          ///< `Clock::monotonicNs()` when it arrived.
                uint8_t port = 0;            ///< The port whose source produced it.
                uint8_t size = 0;            ///< Bytes in `data`.
                uint8_t flags = 0;           ///< None yet: 0.
                uint8_t data[MaxBytes] = {}; ///< The bytes.
};

static_assert(sizeof(MidiEvent) == 16, "MidiEvent is bmds_midi_event_t");

BMDS_NAMESPACE_END
