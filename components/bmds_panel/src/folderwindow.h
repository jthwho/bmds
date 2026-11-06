/**
 * @file      folderwindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/blockdevices.h>
#include <bmds/config.h>
#include <bmds/detailsview.h>
#include <bmds/fileicons.h>
#include <bmds/folderitemmodel.h>
#include <bmds/foldersettings.h>
#include <bmds/folderwatches.h>
#include <bmds/furniture.h>
#include <bmds/guiwindow.h>
#include <bmds/iconview.h>
#include <bmds/lineedit.h>
#include <bmds/list.h>
#include <bmds/menu.h>
#include <bmds/menubar.h>
#include <bmds/messagebox.h>
#include <bmds/navigationhistory.h>
#include <bmds/popupedit.h>
#include <bmds/string.h>
#include <bmds/treeview.h>

#include "folderactions.h"

BMDS_NAMESPACE_BEGIN

class FolderMenus;
class FolderWindow;

/** @brief What a folder window needs of the panel (`devplan/filebrowser.md` §4h). */
class FolderWindowHost : public FolderHost {
        public:
                /** @brief Returns the actions on files. */
                virtual FolderActions &folderActions() = 0;

                /** @brief Returns the type icons. */
                virtual FileIcons &fileIcons() = 0;

                /** @brief Returns the folders watched (`devplan/filebrowser.md` §4i), shared by every view. */
                virtual FolderWatches &folderWatches() = 0;

                /** @brief Returns the menus every folder window shares. */
                virtual FolderMenus &folderMenus() = 0;

                /** @brief Returns how each folder was shown (`folders.conf`). */
                virtual FolderSettings &folderSettings() = 0;

                /** @brief `folderSettings()` changed: written soon. */
                virtual void folderSettingsChanged() = 0;

                /** @brief @p window closed: destroyed after the event (not within it). */
                virtual void folderClosed(FolderWindow &window) = 0;

                /** @brief Returns `files/show-hidden`. */
                virtual bool showHidden() const = 0;

                /** @brief Returns `files/default-view`. */
                virtual FolderSettings::View defaultView() const = 0;

        protected:
                ~FolderWindowHost() = default;
};

/**
 * @brief A folder window (`devplan/filebrowser.md` §4h, FB1-FB4): a menu bar,
 *        a toolbar, an address bar, the folder tree beside the folder's items
 *        (Large Icons or Details) and a status bar. It shows one folder and goes
 *        to others in place (Back, Forward, Up, the address, the tree, a folder
 *        opened).
 *
 * @par Thread Safety
 * The panel's loop.
 */
class FolderWindow final : public GuiWindow {
        public:
                /** @brief What the menus ask a window to do. */
                enum Choice : int {
                        ChooseOpen = 1,
                        ChooseOpenNew,
                        ChooseNewFolder,
                        ChooseNewText,
                        ChooseNewFile,
                        ChooseDelete,
                        ChooseRename,
                        ChooseClose,
                        ChooseCopy,
                        ChoosePaste,
                        ChooseSelectAll,
                        ChooseInvert,
                        ChooseIcons,
                        ChooseDetails,
                        ChooseSortName,
                        ChooseSortType,
                        ChooseSortSize,
                        ChooseSortModified,
                        ChooseFolders,
                        ChooseStatusBar,
                        ChooseRefresh,
                        ChooseBack,
                        ChooseForward,
                        ChooseUp,
                        ChooseHome,
                        ChooseDesktop,
                        ChooseAbout,
                        ChooseCut,
                        ChooseDeleteForGood,
                        ChooseShowContents, // an application's folder, as a folder
                        ChooseRestore,      // /trash: put back where they were (devplan/filebrowser.md §4k)
                        ChooseEmptyTrash,
                        ChooseSortOrigin,
                        ChooseSortDeleted,
                        ChooseProperties, // the selection's, else the folder's (devplan/filebrowser.md §4k)
                        ChooseFormat,     // the card selected or shown, else Format chooses (format.md §5)
                        ChooseOpenWith = 100, // + an index: Open With's application (filebrowser.md §4l)
                };

                FolderWindow(GuiApplication &app, const Config &config, FolderWindowHost &host);
                ~FolderWindow() override;

                /** @brief Makes its widgets. @return `Ok` or `NoMem`. */
                Error build();

                /**
                 * @brief Goes to folder @p path (made absolute against the one shown),
                 *        @p select (a name in it) selected once read.
                 * @return `Ok`, or the error (told; the folder shown stays).
                 */
                Error go(StringView path, StringView select = StringView());

                /** @brief Returns the folder shown. */
                StringView path() const { return _items.folder().path(); }

                /** @brief Reads the folder again, the selection kept by name. */
                void refresh();

                /** @brief Folder @p folder changed: its row in the tree (if open) read again. */
                void treeChanged(StringView folder);

                /**
                 * @brief A watch told of @p changes in folder @p folder (`DirectoryWatch`
                 *        bits): the folder shown read again (once a name edited is
                 *        done), its row in the tree for entries.
                 */
                void watchedChanged(StringView folder, uint8_t changes);

                /** @brief Its items' icons drawn again: the files cut changed, or icons were loaded. */
                void redrawIcons();

                /** @brief `files/show-hidden` or the theme changed: read again, icons found again. */
                void settingsChanged();

                /** @brief Does what a menu item asks. */
                void choose(int choice);

                /** @brief Returns true if the view shown is Details. */
                bool showsDetails() const { return _view == FolderSettings::View::Details; }

                /** @brief Returns the sort. */
                FolderModel::Sort sort() const { return _items.folder().sort(); }

                /** @brief Returns true if the folder tree shows. */
                bool showsTree() const { return _treeShown; }

                /** @brief Returns true if the status bar shows. */
                bool showsStatusBar() const { return _statusShown; }

                /** @brief Returns the items selected. */
                size_t selectedCount() const { return view().selection().selectedCount(); }

                /** @brief Returns true if only a folder is selected. */
                bool folderSelected() const;

                /** @brief Returns true if one item is selected and it is an application's folder (`NAME.app`). */
                bool applicationSelected() const;

                /**
                 * @brief Returns true if one item is selected and it is a file Open With
                 *        applies to (not a folder, a launcher or a built-in program), its path
                 *        into @p path.
                 */
                bool fileSelected(String &path) const;

                /** @brief Returns the folder actions the window shares with the desktop. */
                FolderActions &folderActions() { return _host.folderActions(); }

                /**
                 * @brief Returns the card Format would format (`sd0`: the one card folder
                 *        selected, else the card the folder is on), or false if none.
                 */
                bool formatTarget(char (&name)[BlockDevices::MaxName + 1]) const;

                /** @brief Returns true if one item is selected and it is a card's folder (`/sd0`, `/sd0/p0`). */
                bool cardSelected() const;

                /** @brief Returns true if the clipboard has files to paste. */
                bool canPaste();

                /** @brief Returns true if Back has somewhere to go. */
                bool canGoBack() const { return _history.canGoBack(); }

                /** @brief Returns true if Forward has somewhere to go. */
                bool canGoForward() const { return _history.canGoForward(); }

                /** @brief Returns true if Up has somewhere to go. */
                bool canGoUp() const { return path() != "/"; }

                /** @brief Returns true if the folder shown is the trash (`/trash`). */
                bool isTrash() const { return _items.folder().isTrash(); }

        protected:
                void closedByManager() override;
                bool keyUnused(const KeyEvent &event) override;

        private:
                static void fillTick(void *arg);
                static void viewActivated(void *arg, ItemView &view, size_t row);
                static void viewChanged(void *arg, ItemView &view);
                static void viewMenu(void *arg, ItemView &view, int row, uint16_t x, uint16_t y);
                static void viewRename(void *arg, ItemView &view, size_t row);
                static bool viewKey(void *arg, ItemView &view, const KeyEvent &event);
                static bool       viewDragAsked(void *arg, ItemView &view, GuiDrag &drag);
                static DropAction viewDropAsked(void *arg, ItemView &view, int &row, const GuiDropEvent &event);
                static bool       viewDropped(void *arg, ItemView &view, int row, GuiDropEvent &event);
                static DropAction treeDropAsked(void *arg, ItemView &view, int &row, const GuiDropEvent &event);
                static bool       treeDropped(void *arg, ItemView &view, int row, GuiDropEvent &event);
                bool              dropTarget(ItemView &view, int &row, const GuiDropEvent &event, String &out);
                static void sortAsked(void *arg, DetailsView &view, size_t column);
                static void treeExpanding(void *arg, TreeView &view, size_t row);
                static void treeChosen(void *arg, ItemView &view);
                static void treeRowsChanged(void *arg, TreeView &view);
                static void addressAccepted(void *arg, LineEdit &edit);
                static void addressCancelled(void *arg, LineEdit &edit);
                static void toolClicked(void *arg, Button &button);
                static void renamed(void *arg, bool accepted, StringView text);
                static void renameRetry(void *arg, MessageBox::Answer answer);

                ItemView   &view() const;
                void        startReading(); // the folder read again from its start, in batches
                void        stopReading();
                void        keepSelection();
                void        restoreSelection();
                void        itemPath(size_t row, String &out) const;
                void        selectedPaths(String &out) const;
                void        openRow(size_t row, bool newWindow);
                void        showView(FolderSettings::View view);
                void        setSort(FolderModel::Sort sort, bool descending);
                void        remember(); // the folder's view and sort, into folders.conf
                void        syncTree();
                void        watchFolder(); // the folder shown watched, not the one before
                void        unwatchAll();
                void        updateStatus();
                void        updateTools();
                void        setIcons();
                void        markNetworkMounts(); // the tree's network mounts drawn as such
                void        startRename(size_t row, StringView text);
                void        createNew(FileOps::NewKind kind);
                void        closeWindow();
                const char *folderName() const;

                FolderWindowHost    &_host;
                FolderItemModel      _items;
                NavigationHistory    _history;
                MenuBar             *_bar = nullptr;
                ToolBar             *_tools = nullptr;
                Button              *_toolButtons[12] = {}; // those with icons, with _toolIcons' ids
                IconId               _toolIcons[12] = {};
                Button              *_copyTool = nullptr; // shown or not as the folder is the trash
                Button              *_pasteTool = nullptr;
                Button              *_restoreTool = nullptr;
                Button              *_emptyTool = nullptr;
                Button              *_back = nullptr;
                Button              *_forward = nullptr;
                Button              *_up = nullptr;
                LineEdit            *_address = nullptr;
                Splitter            *_split = nullptr;
                TreeView            *_tree = nullptr;
                IconView            *_icons = nullptr;
                DetailsView         *_details = nullptr;
                StatusBar           *_status = nullptr;
                PopupEdit           *_nameEdit = nullptr; // PSRAM, the first rename
                String               _renamePath{MemSpace::Psram};
                String               _renameTried{MemSpace::Psram};
                String               _kept{MemSpace::Psram};        // the names selected, NUL after each
                String               _keptCurrent{MemSpace::Psram}; // the current row's name
                String               _select{MemSpace::Psram};      // a name to select once read
                bool                 _renameSelected = false;       // and to rename then (a new item)
                FolderSettings::View _view = FolderSettings::View::Icons;
                uint64_t             _freeBytes = 0;
                bool                 _hasFree = false;
                int                  _fillTimer = -1;
                bool                 _treeShown = true;
                bool                 _statusShown = true;
                bool                 _syncingTree = false; // the tree follows: its changes aren't the user's
                bool                 _refreshDue = false;  // a change told while a name was edited
                String               _watched{MemSpace::Psram};     // the folder watched for the view
                String               _treeWatched{MemSpace::Psram}; // the tree's expanded folders, NUL after each
};

/**
 * @brief The menus every folder window shares (`devplan/filebrowser.md` §4h): a
 *        menu bar's and the items' and background's, filled for the window that
 *        opens them, as it is then (one menu shows at a time).
 *
 * @par Thread Safety
 * The panel's loop.
 */
class FolderMenus {
        public:
                explicit FolderMenus(GuiApplication &app);

                /** @brief Adds the menus' titles to @p bar. */
                Error attach(MenuBar &bar);

                /** @brief Opens an item's menu (@p item) or the background's for @p window, at (@p x, @p y). */
                void popup(FolderWindow &window, bool item, uint16_t x, uint16_t y);

                /** @brief Forgets @p window (closing): its menus dismissed. */
                void forget(FolderWindow &window);

        private:
                static void aboutToShow(void *arg, MenuBar &bar, size_t index, Menu &menu);
                static void triggered(void *arg, int choice);
                void        fill(Menu &menu, FolderWindow &window);

                GuiApplication &_app;
                FolderWindow   *_owner = nullptr;
                Menu            _file;
                Menu            _edit;
                Menu            _view;
                Menu            _go;
                Menu            _help;
                Menu            _new;     // File's and the background's New
                Menu            _arrange; // View's and the background's Arrange Icons
                Menu            _views;   // the background's View: Large Icons, Details
                Menu            _openWith; // File's and the item's Open With (filebrowser.md §4l)
                Menu            _item;
                Menu            _background;
};

BMDS_NAMESPACE_END
