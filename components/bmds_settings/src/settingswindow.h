/**
 * @file      settingswindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/colordialog.h>
#include <bmds/config.h>
#include <bmds/filedialog.h>
#include <bmds/guiwindow.h>
#include <bmds/icon.h>
#include <bmds/itemmodel.h>
#include <bmds/list.h>
#include <bmds/programlauncher.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>

#include "settingsdialogs.h"
#include "settingstable.h"

BMDS_NAMESPACE_BEGIN

class AllSettingsPage;
class DetailsView;
class ItemView;
class LineEdit;
class SettingsPage;
class StatusBar;
class TextBlock;

/**
 * @brief Settings' window (`devplan/controlpanel.md` §4): the groups down the
 *        left under a search field, the chosen group's page on the right, a
 *        status bar under both.
 *
 * Every change applies and saves at once (§6.2); the window says what was
 * refused and why, and what couldn't be saved. It follows the registry
 * (§6.7). One page exists at a time.
 *
 * @par Thread Safety
 * The application's loop (its lock held), as every window.
 */
class SettingsWindow final : public GuiWindow {
        public:
                /** @brief A window of @p app; programs it starts (Controls) as @p program's. */
                SettingsWindow(GuiApplication &app, const Config &config, const BuiltinProgram::Context &program);
                ~SettingsWindow() override;

                /** @brief Makes the widgets and starts following the registry. @return `Ok` or why not. */
                Error build();

                /** @brief Shows page @p page (its index in `SettingsTable::pages()`). */
                void showPage(size_t page);

                /** @brief Shows it again, raised, at page @p page (-1: as it was). */
                void raise(int page);

                /** @brief Says @p text in the status bar. */
                void say(StringView text);

                /** @brief Says why @p label's value was refused (`SysConf::set()`'s @p err). */
                void refused(StringView label, SysConf::Id id, Error err);

                /** @brief Saves @p id, saying so, or why not. */
                void save(SysConf::Id id, StringView label);

                /**
                 * @brief Asks whether to keep @p id's new value (`devplan/controlpanel.md` §6.3):
                 *        Keep saves it; Revert, closing or 15 seconds set @p before back.
                 */
                void confirm(SysConf::Id id, StringView before, StringView label);

                /** @brief Asks for a folder (or a file) for @p id, which is set and saved if one is chosen. */
                void choosePath(SysConf::Id id, bool folder, StringView label);

                /** @brief Asks for a folder added to @p id's list (`paths`), set and saved. */
                void addFolder(SysConf::Id id, StringView label);

                /** @brief Asks for @p id's color, set as it changes, saved at OK, put back at Cancel. */
                void chooseColor(SysConf::Id id, StringView label);

                /** @brief Asks for @p id's time zone (the system's offered if @p allowEmpty), set and saved. */
                void chooseZone(SysConf::Id id, bool allowEmpty, StringView label);

                /** @brief Asks for a new password, twice, and sets it (`Password::set()`). */
                void setPassword();

                /** @brief Starts @p command (a program and its arguments, spaces between), not waited for. */
                void run(StringView command);

                /**
                 * @brief Told of a color asked for: as it changes (@p finished false), then at the
                 *        end (@p accepted: OK; else Cancel, @p rgb the color it started at).
                 */
                using ColorTaken = void (*)(void *arg, uint32_t rgb, bool finished, bool accepted);

                /** @brief Asks for a color, starting at @p rgb, labelled @p label, told to @p fn. */
                void chooseColorFor(uint32_t rgb, StringView label, ColorTaken fn, void *arg);

                /** @brief Told of a name typed (at OK). */
                using NameTaken = void (*)(void *arg, StringView name);

                /** @brief Asks for a name in a dialog titled @p title, @p prompt before the field. */
                void askName(StringView title, StringView prompt, StringView initial, NameTaken fn, void *arg);

                /** @brief Forgets the dialogs' callbacks to @p arg (it is going away). */
                void forget(void *arg);

        protected:
                void closedByManager() override;
                bool keyUnused(const KeyEvent &event) override;

        private:
                // An icon of the theme, copied (its cache may drop its own).
                struct OwnedIcon {
                                Icon      icon;
                                uint32_t *pixels = nullptr;
                                void      take(const Icon *from);
                                ~OwnedIcon();
                };

                // A search's match: a page, and a key on it (empty: the page itself).
                struct Hit {
                                uint8_t page = 0;
                                char    key[::bmds::Config::MaxKey + 1] = {};
                };

                static void sidebarChanged(void *arg, ItemView &view);
                static void sidebarActivated(void *arg, ItemView &view, size_t row);
                static void searchEdited(void *arg, LineEdit &edit);
                static void searchDone(void *arg, LineEdit &edit);
                static void configChanged(void *arg, SysConf::Id id);
                static void rebuildLater(void *arg);
                static void pathChosen(void *arg, FileDialog &dialog, int result);
                static void colorChanged(void *arg, ColorDialog &dialog, uint32_t rgb);
                static void colorDone(void *arg, Dialog &dialog, int result);
                static void confirmDone(void *arg, Dialog &dialog, int result);
                static void confirmTick(void *arg);
                static void zoneDone(void *arg, Dialog &dialog, int result);
                static void passwordDone(void *arg, Dialog &dialog, int result);
                static void nameDone(void *arg, Dialog &dialog, int result);
                static void tick(void *arg);

                void showGroups(int select); // the sidebar's groups, @p select chosen
                void search(StringView text);
                void destroyPage();
                void confirmShown(); // the question with the seconds left

                ListItemModel                  _groups;
                ListItemModel                  _results;
                List<Hit>                      _hits{MemSpace::Psram};
                OwnedIcon                      _icons[16];
                OwnedIcon                      _largeIcon; // Alt+Tab's
                FileDialog                     _folderDialog;
                FileDialog                     _fileDialog;
                ColorDialog                    _colorDialog;
                Dialog                         _confirm;
                ZoneDialog                     _zoneDialog;
                PasswordDialog                 _passwordDialog;
                NameDialog                     _nameDialog;
                const BuiltinProgram::Context &_program;
                int                            _tickTimer = -1; // while the page's status changes by itself
                SysConf::Id                    _zoneId = SysConf::NoId;
                char                           _zoneLabel[64] = {};
                TextBlock                     *_confirmText = nullptr;
                String                         _confirmBefore{MemSpace::Psram};
                SysConf::Id                    _confirmId = SysConf::NoId;
                int                            _confirmTimer = -1;
                int                            _confirmLeft = 0;
                char                           _confirmLabel[64] = {};
                bool                           _syncing = false; // the sidebar set by the window: not followed
                LineEdit                      *_search = nullptr;
                DetailsView                   *_sidebar = nullptr;
                BoxLayout                     *_host = nullptr;
                StatusBar                     *_status = nullptr;
                SettingsPage                  *_page = nullptr;
                AllSettingsPage               *_all = nullptr;
                size_t                         _current = 0;
                bool                           _searching = false;
                bool                           _rebuildPosted = false;
                SysConf::Id                    _pathId = SysConf::NoId; // the dialog's setting
                bool                           _pathAdds = false;       // added to its list, not set
                SysConf::Id                    _colorId = SysConf::NoId;
                char                           _pathLabel[64] = {};
                char                           _colorLabel[64] = {};
                ColorTaken                     _colorFn = nullptr; // chooseColorFor()'s
                void                          *_colorArg = nullptr;
                NameTaken                      _nameFn = nullptr;
                void                          *_nameArg = nullptr;
};

BMDS_NAMESPACE_END
