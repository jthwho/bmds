/**
 * @file      settingsdialogs.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/dialog.h>
#include <bmds/itemmodel.h>
#include <bmds/list.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

class DetailsView;
class ItemView;
class LineEdit;
class TextBlock;

/**
 * @brief Chooses a time zone (`devplan/controlpanel.md` §6.4): the regions on the
 *        left, the chosen region's places on the right, from `/share/zoneinfo`
 *        (the zone database shown as folders). Zones not in a region (`UTC`,
 *        `Japan`) are under Other; "Same as the system" first when the setting
 *        may be empty.
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class ZoneDialog final : public Dialog {
        public:
                /** @brief Where the zones are, as folders. */
                static constexpr const char *ZoneDir = "/share/zoneinfo";

                explicit ZoneDialog(GuiApplication &app);
                ~ZoneDialog() override;

                /** @brief Makes the widgets. @return `Ok` or `NoMem`. */
                Error build();

                /**
                 * @brief Opens it at @p zone (a region's place chosen), offering "Same as the
                 *        system" (an empty zone) if @p allowEmpty.
                 */
                Error open(StringView zone, bool allowEmpty);

                /** @brief Returns the zone chosen ("" for the system's), after `Accepted`. */
                StringView zone();

                /** @brief Returns true if a place (or the system's zone) is chosen. */
                bool hasChoice() const;

                /** @brief `Finished`'s results. */
                enum Choice : int {
                        Rejected = 0,
                        Accepted = 1,
                };

        private:
                static void regionChanged(void *arg, ItemView &view);
                static void placeChosen(void *arg, ItemView &view, size_t row);

                void fillRegions(StringView select);
                void fillPlaces(StringView select);
                bool regionIsOther(size_t row) const;

                ListItemModel _regions;
                ListItemModel _places;
                DetailsView  *_regionView = nullptr;
                DetailsView  *_placeView = nullptr;
                String        _chosen{MemSpace::Psram};
                bool          _allowEmpty = false;
                bool          _filling = false;
};

/**
 * @brief Asks for a new password twice (`devplan/controlpanel.md` §5), not shown,
 *        and sets it (`Password::set()`: hashed and saved).
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class PasswordDialog final : public Dialog {
        public:
                explicit PasswordDialog(GuiApplication &app);

                /** @brief Makes the widgets. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief Opens it, both fields empty. */
                Error open();

                /**
                 * @brief Takes what was typed (after OK): the password set, or why not, in
                 *        @p problem. Both fields are emptied.
                 * @return `Ok`, `Invalid` (they differ, or empty), or `Password::set()`'s error.
                 */
                Error take(const char *&problem);

        private:
                LineEdit *_first = nullptr;
                LineEdit *_second = nullptr;
};

/**
 * @brief Asks for a name (`devplan/wallpaper.md` §4f: a palette's, for Save As...): a prompt
 *        and a field.
 *
 * @par Thread Safety
 * The loop's thread, as the widgets.
 */
class NameDialog final : public Dialog {
        public:
                explicit NameDialog(GuiApplication &app);

                /** @brief Makes the widgets. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief Opens it titled @p title, @p prompt before the field, holding @p initial. */
                Error open(StringView title, StringView prompt, StringView initial);

                /** @brief Returns what was typed, trimmed. */
                StringView name() const;

                /** @brief `Finished`'s results. */
                enum Choice : int {
                        Rejected = 0,
                        Accepted = 1,
                };

        private:
                Label    *_prompt = nullptr;
                LineEdit *_edit = nullptr;
};

BMDS_NAMESPACE_END
