/**
 * @file      input.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/inputevent.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Where keyboards' and mice's events go (`devplan/input.md` §4): one
 *        consumer, the desktop, sets itself as the sink.
 *
 * Without a sink, events are dropped.
 *
 * @par Thread Safety
 * All functions from any task. The sink's functions run on the task that
 * delivers the event (a USB host's), one event at a time; `setSink()` waits
 * for a delivery in progress, so after it returns the old sink is not called.
 */
namespace Input {

        /** @brief A consumer of events. Its functions must not block for long. */
        struct Sink {
                        void (*key)(void *arg, const KeyEvent &event) = nullptr;
                        void (*pointer)(void *arg, const PointerEvent &event) = nullptr;
                        void *arg = nullptr;
        };

        /** @brief Boot stage: makes the sink's lock. Before anything delivers or sets a sink. */
        Error init();

        /** @brief Sets the consumer (copied); null: none. */
        void setSink(const Sink *sink);

        /** @brief Delivers a key event to the sink. */
        void key(const KeyEvent &event);

        /** @brief Delivers a pointer event to the sink. */
        void pointer(const PointerEvent &event);

        /** @brief Told of a key (its HID usage) down in a keyboard's first second attached. */
        using AttachKey = void (*)(uint8_t usage);

        /** @brief How long after a keyboard is attached its keys go to the `AttachKey` handler. */
        inline constexpr uint32_t AttachKeyMs = 1000;

        /**
         * @brief Sets who is told of the keys down in a keyboard's first second attached: a key held
         *        as it was plugged in, or as the system started (devplan/update.md §17: Esc starts
         *        recovery); null: none. Called on the keyboard's task, before the key's event.
         */
        void setAttachKeyHandler(AttachKey handler);

        /** @brief Tells the `AttachKey` handler, if any, of @p usage (the keyboard's driver). */
        void attachKey(uint8_t usage);

} // namespace Input

BMDS_NAMESPACE_END
