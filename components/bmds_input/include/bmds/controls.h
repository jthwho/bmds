/**
 * @file      controls.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/chord.h>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/inputevent.h>
#include <bmds/padmapping.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief An application's actions and their bindings, and the matching of
 *        key and pad events against them (`devplan/controls.md` §7c).
 *
 * `declare()` reads a `controls.conf` text; `applyUser()` lays the user's
 * changed bindings over it; `setSet()` chooses the active set. `key()` and
 * `pad()` match an event against the active set: a match is told as action
 * outputs and is consumed (not to be delivered raw).
 *
 * @par Thread Safety
 * A plain value: its owner locks.
 */
class Controls {
        public:
                static constexpr size_t MaxActions = 64;
                static constexpr size_t MaxSets = 8;
                static constexpr size_t MaxKeys = 4; ///< Key bindings per action (an axis's: pairs).
                static constexpr size_t MaxPad = 4;  ///< Pad bindings per action.
                static constexpr size_t MaxName = 32;
                static constexpr size_t MaxLabel = 40;
                static constexpr size_t MaxGroup = 24;
                static constexpr size_t MaxHeld = 16; ///< Keys held matched at once.
                static constexpr size_t MaxPads = 8;  ///< Pads told apart (their numbers).

                /** @brief What an action is. */
                enum class Kind : uint8_t {
                        Button, ///< Pressed and released.
                        Axis,   ///< −32767 to 32767.
                };

                /** @brief A pad control a binding names. */
                struct PadSource {
                                enum class Type : uint8_t {
                                        None,
                                        Button,    ///< `Pad.South`: a `PadState::Button`.
                                        Axis,      ///< `Pad.LeftX`: a `PadState::Axis`, whole.
                                        AxisPlus,  ///< `Pad.LeftX+`: its upper half, as a button.
                                        AxisMinus, ///< `Pad.LeftX-`: its lower half.
                                };
                                Type    type = Type::None;
                                uint8_t index = 0;
                };

                /** @brief An action, as declared (and the user's bindings laid over). */
                struct Action {
                                char      name[MaxName] = {};
                                char      label[MaxLabel] = {};
                                char      group[MaxGroup] = {};
                                uint16_t  id = 0;
                                Kind      kind = Kind::Button;
                                uint8_t   sets = 0xff; ///< A bit per set.
                                bool      repeat = false;
                                Chord     keys[MaxKeys];     ///< A button's keys; an axis's low halves.
                                Chord     keysHigh[MaxKeys]; ///< An axis's high halves.
                                PadSource pad[MaxPad];       ///< A button's; an axis's whole or low halves.
                                PadSource padHigh[MaxPad];   ///< An axis's high halves (pad buttons).
                };

                /** @brief An action event to deliver. */
                struct Output {
                                /** @brief What happened: the ABI's `BMDS_ACTION_*`. */
                                enum class Type : uint8_t {
                                        Press = 1,
                                        Release,
                                        Repeat,
                                        Axis,
                                };
                                Type     type = Type::Press;
                                uint16_t id = 0;
                                int32_t  value = 0;  ///< 1 or 0; an axis's value.
                                uint32_t source = 0; ///< `Keyboard`, or `Pad` with the pad's number << 8.
                };

                /** @brief `Output::source`'s low byte. */
                enum Source : uint32_t {
                        Keyboard = 1,
                        Pad = 2,
                };

                /** @brief Told each output, in order. */
                using Emit = void (*)(void *arg, const Output &output);

                Controls() = default;
                Controls(const Controls &) = delete;
                Controls &operator=(const Controls &) = delete;

                /**
                 * @brief Reads a declaration (`controls.conf`'s text); the actions and sets it had are
                 *        replaced, nothing held, the first set active.
                 * @param badLine Set to the first line refused (0: none), on `Invalid`.
                 * @return `Invalid` (a line refused: a name, an Id twice, an action without keys...),
                 *         or `Full` (past `MaxActions` or `MaxSets`). Nothing is kept on an error.
                 */
                Error declare(StringView text, uint32_t &badLine);

                /**
                 * @brief Lays the user's bindings (the same format: `[Action NAME]` with `Keys=` and
                 *        `Pad=`) over the declaration's. Unknown actions and bad lines are skipped.
                 */
                void applyUser(StringView text);

                /** @brief Returns true once a declaration was read. */
                bool isDeclared() const { return _count != 0; }

                /**
                 * @brief Makes set @p name active (empty: none, every event raw). What was held is let
                 *        go first, through @p emit.
                 * @return `NotExist` if there is no such set.
                 */
                Error setSet(StringView name, Emit emit, void *arg);

                /**
                 * @brief Matches a key event: @p usage (extended), @p modifiers (`Chord`'s side-less
                 *        bits).
                 * @return True if it matched (consumed: not delivered raw).
                 * @hotpath
                 */
                bool key(uint32_t usage, uint8_t modifiers, KeyEvent::Action action, Emit emit, void *arg);

                /**
                 * @brief Matches a pad event (`Attached` and `Detached` are never matched).
                 * @return True if it matched.
                 * @hotpath
                 */
                bool pad(const PadEvent &event, Emit emit, void *arg);

                /** @brief Lets everything held go (focus lost), through @p emit. */
                void releaseAll(Emit emit, void *arg);

                /**
                 * @brief Writes action @p id's key bindings (or with @p pad its pad bindings) as text:
                 *        `Z; Ctrl`, `Left Right; A D`, `Pad.South`.
                 * @return Its length, or `NotExist` (no such action).
                 */
                Error bindingText(uint16_t id, bool pad, char *out, size_t size, size_t &length) const;

                /** @brief Returns the actions' count. */
                size_t count() const { return _count; }

                /** @brief Returns action @p index (by declaration order). */
                const Action &action(size_t index) const { return _actions[index]; }

        private:
                struct State {
                                uint8_t down = 0;  // a button's sources down; an axis's low ones
                                uint8_t high = 0;  // an axis's high ones
                                int16_t stick = 0; // an axis's from a stick
                                int16_t out = 0;   // an axis's last told
                };

                struct Held {
                                uint32_t usage = 0;
                                uint8_t  action = 0; // index + 1; 0: free
                                bool     high = false;
                };

                void clear();
                bool inSet(const Action &a) const { return _active != NoSet && (a.sets >> _active & 1u) != 0; }
                void press(size_t index, bool high, uint32_t source, Emit emit, void *arg);
                void release(size_t index, bool high, uint32_t source, Emit emit, void *arg);
                void tellAxis(size_t index, uint32_t source, Emit emit, void *arg);

                static constexpr uint8_t NoSet = 0xff;

                Action   _actions[MaxActions];
                State    _states[MaxActions];
                char     _sets[MaxSets][MaxName] = {};
                Held     _held[MaxHeld];
                uint16_t _padButtons[MaxPads][PadState::ButtonCount] = {}; // action index + 1, high << 15
                int16_t  _padAxes[MaxPads][PadState::AxisCount] = {};      // as last told
                size_t   _count = 0;
                uint8_t  _setCount = 0;
                uint8_t  _active = NoSet;
};

BMDS_NAMESPACE_END
