/**
 * @file      filedialog.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <bmds/combobox.h>
#include <bmds/config.h>
#include <bmds/detailsview.h>
#include <bmds/dialog.h>
#include <bmds/error.h>
#include <bmds/fileicons.h>
#include <bmds/folderitemmodel.h>
#include <bmds/folderwatches.h>
#include <bmds/lineedit.h>
#include <bmds/list.h>
#include <bmds/messagebox.h>
#include <bmds/navigationhistory.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief Asks for files to open, a name to save as, or a folder
 *        (`devplan/mediaplayer.md` §6d).
 *
 * A "Look in" row (the folder's path, Back and Up), places on the left (the
 * home folder, each mounted filesystem, `/`), the folder in a `DetailsView`,
 * then "File name" and "Files of type" over its buttons. The folder is read a
 * batch at a time and kept current by a directory watch while it is open.
 *
 * - **Open** takes a file that exists; **OpenMany** one or more (each name
 *   in quotes); **Save** a name in a folder that exists, given the type's
 *   extension when it has none, and asks before replacing a file;
 *   **Folder** a folder (the one shown when no name is given).
 * - A folder's name typed (or a path, absolute or relative) goes there; a
 *   name with `*` or `?` filters the folder with it; Return on a folder in
 *   the list enters it, Backspace goes up.
 *
 * It isn't modal, as `Dialog`: `Finished` is told `Accepted` (then `count()`
 * and `path()` are the choice) or `Rejected`. It may be opened again. Destroy
 * it closed, with no message box of its own open.
 *
 * @par Thread Safety
 * As `GuiWindow`.
 */
class FileDialog : public Dialog {
        public:
                /** @brief What it asks for. */
                enum class Mode : uint8_t {
                        Open,
                        OpenMany,
                        Save,
                        Folder,
                };

                /** @brief The results told. */
                enum Result : int {
                        Rejected = 0,
                        Accepted = 1,
                };

                /** @brief Told the result (on the loop's thread). */
                using Finished = void (*)(void *arg, FileDialog &dialog, int result);

                static constexpr size_t MaxFilters = 8;
                static constexpr size_t MaxPlaces = 2 + Vfs::MaxMounts;

                /** @brief A dialog of @p app asking as @p mode, titled @p title. */
                FileDialog(GuiApplication &app, Mode mode, StringView title);
                ~FileDialog() override;

                /**
                 * @brief Adds a choice of "Files of type": @p name ("Media files")
                 *        showing the files matching @p patterns (`NameFilter`'s,
                 *        `*.mp3;*.ogg`; empty: all). The first is chosen at first.
                 * @return `Ok` or `Full`.
                 */
                Error addFilter(StringView name, StringView patterns);

                /** @brief Sets the folder it opens in (by default the last one shown, else `/home`). */
                Error setFolder(StringView path);

                /** @brief Sets the name it opens with (Save's suggestion). */
                Error setFileName(StringView name);

                /** @brief Has @p fn told the result. */
                void setFinished(Finished fn, void *arg) {
                        _told = fn;
                        _toldArg = arg;
                        return;
                }

                /** @brief Opens it. @return `Ok`, `NoMem`, or the window manager's error. */
                Error open();

                /** @brief Returns the paths chosen (after `Accepted`). */
                size_t count() const { return _paths.size(); }

                /** @brief Returns chosen path @p index (absolute). */
                StringView path(size_t index) const {
                        return index < _paths.size() ? _paths[index].view() : StringView();
                }

                /** @brief Returns the folder shown last. */
                StringView folder() const { return _items.folder().path(); }

        private:
                struct Filter {
                                char name[ComboBox::MaxText + 1] = {};
                                char patterns[96] = {};
                };

                struct Place {
                                Button *button = nullptr;
                                char    path[Vfs::MaxMountPath + 1] = {};
                };

                Error build();
                void  setPlaces();
                Error go(StringView path, StringView select);
                void  reload(); // the folder read again, the current row kept
                void  startReading();
                void  stopReading();
                void  rowsChanged(); // the view reset to the model, the row asked for selected
                void  applyFilter();
                void  accept();
                bool  acceptNames(); // false: nothing chosen yet (gone to a folder, asked)
                void  choose(StringView path);
                void  finishWith(); // the paths kept: Accepted
                void  warn(StringView format, StringView name);
                void  updateButtons();

                static void finished(void *arg, Dialog &dialog, int result);
                static void fillTick(void *arg);
                static void iconsAsked(void *arg);
                static void iconsDue(void *arg);
                static void watchNotify(void *arg);
                static void watchesDue(void *arg);
                static void watchChanged(void *arg, StringView path, uint8_t changes);
                static void backClicked(void *arg, Button &button);
                static void upClicked(void *arg, Button &button);
                static void placeClicked(void *arg, Button &button);
                static void acceptClicked(void *arg, Button &button);
                static void lookInAccepted(void *arg, LineEdit &edit);
                static void lookInCancelled(void *arg, LineEdit &edit);
                static void nameAccepted(void *arg, LineEdit &edit);
                static void nameCancelled(void *arg, LineEdit &edit);
                static void typeChanged(void *arg, ComboBox &box, int index);
                static void activated(void *arg, ItemView &view, size_t row);
                static void selectionChanged(void *arg, ItemView &view);
                static bool viewKey(void *arg, ItemView &view, const KeyEvent &event);
                static void sortAsked(void *arg, DetailsView &view, size_t column);
                static void overwriteAnswered(void *arg, MessageBox::Answer answer);

                Mode              _mode;
                FileIcons         _icons;
                FolderItemModel   _items{_icons};
                FolderWatches     _watches{watchNotify, this};
                NavigationHistory _history;
                List<String>      _paths{MemSpace::Psram};
                String            _start{MemSpace::Psram};          // where open() goes
                String            _select{MemSpace::Psram};         // a name to select once read
                String            _watched{MemSpace::Psram};        // the folder watched
                String            _pending{MemSpace::Psram};        // Save's path, asked about
                String            _selectionNames{MemSpace::Psram}; // what the selection put in the name field
                Filter            _filters[MaxFilters];
                size_t            _filterCount = 0;
                Place             _places[MaxPlaces];
                LineEdit         *_lookIn = nullptr;
                Button           *_back = nullptr;
                Button           *_up = nullptr;
                DetailsView      *_view = nullptr;
                LineEdit         *_name = nullptr;
                ComboBox         *_type = nullptr;
                Button           *_accept = nullptr;
                Finished          _told = nullptr;
                void             *_toldArg = nullptr;
                int               _fillTimer = -1;
                std::atomic<bool> _iconsPosted{false};
                std::atomic<bool> _watchPosted{false};
                bool              _asking = false; // the overwrite question is open
};

BMDS_NAMESPACE_END
