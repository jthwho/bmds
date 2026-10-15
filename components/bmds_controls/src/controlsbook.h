/**
 * @file      controlsbook.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <bmds/config.h>
#include <bmds/controls.h>
#include <bmds/error.h>
#include <bmds/list.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief One application's actions as the Controls window and `controls` see them
 *        (`devplan/controls.md` §7d): its declaration (`controls.conf` in its folder) as the
 *        defaults, the user's file laid over it as what is current, and saving the difference.
 *
 * @par Thread Safety
 * A plain object: one thread at a time.
 */
class ControlsBook {
        public:
                /** @brief An application with actions. */
                struct App {
                                String id{MemSpace::Psram};
                                String name{MemSpace::Psram}; ///< Its `Name=`.
                };

                /** @brief Lists the installed applications whose folders hold a `controls.conf`. */
                static Error apps(List<App> &out);

                /** @brief Makes one (PSRAM). @return It, or null. Free with `destroy()`. */
                static ControlsBook *create();

                /** @brief Frees one made by `create()`. */
                static void destroy(ControlsBook *book);

                /**
                 * @brief Reads application @p id's declaration and the user's file under @p home.
                 * @return `NotExist` (no such application, or no `controls.conf`), `Invalid` (the
                 *         declaration refused), or a file's error.
                 */
                Error load(StringView id, StringView home);

                /** @brief Returns the application's id (after `load()`). */
                StringView id() const { return _id.view(); }

                /** @brief Returns the application's name (its `Name=`; its id if none). */
                StringView name() const { return _name.view(); }

                /** @brief Returns the declaration's bindings. */
                const Controls &defaults() const { return *_defaults; }

                /** @brief Returns the bindings as they are (the user's laid over the defaults). */
                Controls &current() { return *_current; }

                /**
                 * @brief Writes the user's file (only what differs; none: removed) and lays it over
                 *        the application's running programs at once.
                 */
                Error save();

                /** @brief Puts action @p index's bindings back to the declaration's (both kinds). */
                void reset(size_t index);

                /** @brief Puts every action back to the declaration's. */
                void resetAll();

        private:
                ControlsBook() = default;
                ~ControlsBook() = default;

                String    _id{MemSpace::Psram};
                String    _name{MemSpace::Psram};
                String    _userPath{MemSpace::Psram};
                String    _userDir{MemSpace::Psram};
                Controls *_defaults = nullptr;
                Controls *_current = nullptr;
};

BMDS_NAMESPACE_END
