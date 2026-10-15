/**
 * @file      programcontrols.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/controls.h>
#include <bmds/error.h>
#include <bmds/mutex.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A program's actions (`devplan/controls.md` §7c): its `Controls`,
 *        shared by the process and its surfaces, which match their events
 *        against it.
 *
 * Counted: `create()` gives one reference, `ref()` adds one, `unref()` drops
 * one and frees it with the last. The matching runs on the surfaces' side
 * (the window manager's task); the declaration and the set change on the
 * program's. Outputs of a set change go to the surface that has the focus
 * (`setTarget()`).
 *
 * @par Thread Safety
 * All functions from any task; each takes the object's lock, and its outputs
 * are told with the lock held (they must not call it).
 */
class ProgramControls {
        public:
                /** @brief Makes one (PSRAM). @return It, or null when PSRAM is out. */
                static ProgramControls *create();

                ProgramControls(const ProgramControls &) = delete;
                ProgramControls &operator=(const ProgramControls &) = delete;

                /** @brief Adds a reference. */
                void ref() { _refs.fetch_add(1, std::memory_order_relaxed); }

                /** @brief Drops a reference; the last frees it. */
                void unref();

                /**
                 * @brief Reads @p declaration, then lays @p user (the user's file's text; may be
                 *        empty) over it. What was held is let go (told to the target).
                 * @param badLine The first line refused, on `Invalid` (0: no line: an action
                 *        without keys or an Id).
                 * @return `Controls::declare()`'s error.
                 */
                Error declare(StringView declaration, StringView user, uint32_t &badLine);

                /** @brief `Controls::setSet()`, told to the target. */
                Error setSet(StringView name);

                /** @brief Sets where a set change's outputs go (null: nowhere): the focused surface. */
                void setTarget(Controls::Emit emit, void *arg);

                /** @brief Clears the target if it is @p arg's (a surface losing the focus or closing). */
                void clearTarget(void *arg);

                /** @brief `Controls::key()`. @hotpath */
                bool key(uint32_t usage, uint8_t modifiers, KeyEvent::Action action, Controls::Emit emit, void *arg);

                /** @brief `Controls::pad()`. @hotpath */
                bool pad(const PadEvent &event, Controls::Emit emit, void *arg);

                /** @brief `Controls::releaseAll()`. */
                void releaseAll(Controls::Emit emit, void *arg);

                /** @brief `Controls::bindingText()`. */
                Error bindingText(uint16_t id, bool pad, char *out, size_t size, size_t &length);

        private:
                ProgramControls() = default;
                ~ProgramControls() = default;

                std::atomic<uint32_t> _refs{1};
                Mutex                 _lock;
                Controls::Emit        _target = nullptr;
                void                 *_targetArg = nullptr;
                Controls              _controls;
};

BMDS_NAMESPACE_END
