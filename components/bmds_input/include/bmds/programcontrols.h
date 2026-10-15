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
                /** @brief Boot stage: makes the running programs' registry's lock. */
                static Error init();

                /** @brief Makes one (PSRAM). @return It, or null when PSRAM is out. */
                static ProgramControls *create();

                ProgramControls(const ProgramControls &) = delete;
                ProgramControls &operator=(const ProgramControls &) = delete;

                /** @brief Adds a reference. */
                void ref() { _refs.fetch_add(1, std::memory_order_relaxed); }

                /** @brief Drops a reference; the last frees it. */
                void unref();

                /** @brief Bytes of a declaration kept (and read). */
                static constexpr size_t MaxDeclaration = 16 * 1024;

                /** @brief Bytes of an application's id, NUL included. */
                static constexpr size_t MaxId = 32;

                /**
                 * @brief Reads @p declaration, then lays @p user (the user's file's text; may be
                 *        empty) over it. What was held is let go (told to the target). The
                 *        declaration is kept, and @p appId (may be empty) names the program for
                 *        `userChanged()`.
                 * @param badLine The first line refused, on `Invalid` (0: no line: an action
                 *        without keys or an Id).
                 * @return `Controls::declare()`'s error, `NoMem`, or `Full` (too long).
                 */
                Error declare(StringView declaration, StringView user, StringView appId, uint32_t &badLine);

                /**
                 * @brief Lays @p user (the user's file, changed: the Controls window, `controls`)
                 *        over every running program of application @p appId at once, its active set
                 *        kept and what was held let go.
                 */
                static void userChanged(StringView appId, StringView user);

                /** @brief Copies the application's id (`declare()`'s; empty: none) into @p out. */
                void appId(char (&out)[MaxId]);

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
                ~ProgramControls();

                void relay(StringView user); // userChanged() for this one

                std::atomic<uint32_t> _refs{1};
                Mutex                 _lock;
                Controls::Emit        _target = nullptr;
                void                 *_targetArg = nullptr;
                char                 *_declaration = nullptr; // kept (Psram) for userChanged()
                size_t                _declarationBytes = 0;
                char                  _appId[MaxId] = {};
                bool                  _listed = false; // in the running programs' registry
                Controls              _controls;
};

BMDS_NAMESPACE_END
