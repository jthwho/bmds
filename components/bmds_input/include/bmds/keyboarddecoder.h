/**
 * @file      keyboarddecoder.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/inputevent.h>
#include <bmds/span.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief The US keymap: a key and its modifiers and locks as a character.
 *
 * @par Thread Safety
 * Stateless.
 */
namespace Keymap {

        /**
         * @brief Returns the character @p usage types with @p modifiers and
         *        @p locks (`KeyEvent` bits), or 0 for a key that types none
         *        (Enter, Tab, Backspace and Escape included: they are keys,
         *        turned into bytes where they land).
         */
        uint32_t us(uint8_t usage, uint8_t modifiers, uint8_t locks);

} // namespace Keymap

/**
 * @brief A boot-protocol keyboard's reports as key events (`devplan/input.md`
 *        I4, I5).
 *
 * Each report is the state of the keys (eight modifier bits and up to six
 * keys down); this diffs it with the last into presses and releases, keeps
 * the lock keys, and repeats the last key pressed while it is held.
 *
 * @par Thread Safety
 * Not thread-safe: one task feeds it.
 */
class KeyboardDecoder {
        public:
                /** @brief Before the first repeat. */
                static constexpr uint32_t RepeatDelayMs = 500;

                /** @brief Between repeats (30 a second). */
                static constexpr uint32_t RepeatIntervalMs = 33;

                /** @brief `repeat()`'s answer when nothing repeats. */
                static constexpr uint32_t NoRepeat = UINT32_MAX;

                /** @brief Where the events go. */
                using Emit = void (*)(void *arg, const KeyEvent &event);

                KeyboardDecoder() = default;

                /**
                 * @brief Takes a boot report (the modifiers, a reserved byte, six keys)
                 *        at @p nowMs, emitting what changed: releases first, then
                 *        presses. A short or rollover-error report is ignored.
                 * @return True if a lock key changed the locks (the LEDs to update).
                 */
                bool report(Span<const uint8_t> data, uint32_t nowMs, Emit emit, void *arg);

                /**
                 * @brief Emits the held key's repeat if due at @p nowMs.
                 * @return Milliseconds until the next is due, or `NoRepeat`.
                 */
                uint32_t repeat(uint32_t nowMs, Emit emit, void *arg);

                /** @brief Releases every key down (the keyboard went away). */
                void releaseAll(Emit emit, void *arg);

                /** @brief Returns the locks (`KeyEvent::Lock` bits). */
                uint8_t locks() const { return _locks; }

                /** @brief Sets the locks (as another keyboard has them). */
                void setLocks(uint8_t locks) { _locks = locks; }

        private:
                static constexpr size_t Keys = 6;

                void send(uint8_t usage, KeyEvent::Action action, Emit emit, void *arg) const;

                uint8_t  _modifiers = 0;
                uint8_t  _keys[Keys] = {};
                uint8_t  _locks = 0;
                uint8_t  _held = HidKey::None; // the key repeating
                uint32_t _repeatAt = 0;        // when it repeats next
};

BMDS_NAMESPACE_END
