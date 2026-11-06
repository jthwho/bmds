/**
 * @file      folderactions.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/builtinprograms.h>
#include <bmds/clipboard.h>
#include <bmds/config.h>
#include <bmds/copiedfiles.h>
#include <bmds/desktopentry.h>
#include <bmds/error.h>
#include <bmds/fileops.h>
#include <bmds/guiapplication.h>
#include <bmds/guidrag.h>
#include <bmds/menu.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include "filejobs.h"

BMDS_NAMESPACE_BEGIN

struct Icon;
struct PanelEntry;

/**
 * @brief What the folder actions need of the panel: folders opened in
 *        windows, and views told of changes.
 */
class FolderHost {
        public:
                /** @brief Opens a folder window on @p path, @p select (a name in it) selected if given. */
                virtual void openFolder(StringView path, StringView select) = 0;

                /** @brief Folder @p folder's entries changed (an action here): its views read it again. */
                virtual void folderChanged(StringView folder) = 0;

                /** @brief The files cut changed (cut, pasted, or the clipboard taken): their icons redrawn. */
                virtual void cutChanged() = 0;

                /**
                 * @brief @p paths (a line each) are being dropped into folder @p directory
                 *        (copied, moved or linked), before it is done: the desktop puts them
                 *        where they were dropped.
                 */
                virtual void dropping(StringView directory, StringView paths) {
                        (void)directory;
                        (void)paths;
                }

        protected:
                ~FolderHost() = default;
};

/**
 * @brief What a folder window and the desktop both do to files
 *        (`devplan/filebrowser.md` §4h, §4j, FB8), written once: open them (by
 *        their type's application, a launcher by its command, a folder in a
 *        window), copy or cut them to the clipboard, paste files into a folder,
 *        move them to the trash or delete them for good, make new ones and
 *        rename them. Copies, moves across filesystems and deletes run on the
 *        worker (`FileJobs`). A failure is told in a message box.
 *
 * Paths are absolute; a list of them is one a line.
 *
 * @par Thread Safety
 * The panel's loop.
 */
class FolderActions {
        public:
                FolderActions(GuiApplication &app, const BuiltinProgram::Context &context, FolderHost &host)
                    : _app(app), _context(context), _host(host) {}
                ~FolderActions();
                FolderActions(const FolderActions &) = delete;
                FolderActions &operator=(const FolderActions &) = delete;

                /** @brief Sets the directories of launcher entries (a `;` list) that applications are looked for in. */
                void setApplications(StringView directories);

                /** @brief Sets the `mimeapps.list` used. */
                void setMimeApps(StringView path);

                /** @brief Writes @p a to @p d to the panel's standard error. */
                void say(const char *a, const char *b = "", const char *c = "", const char *d = "");

                /** @brief Tells @p message in a message box titled @p title (else on standard error). */
                void tell(StringView title, StringView message, bool error = true);

                /** @brief Finds @p command (a name on $PATH, or a path). */
                Error programPath(StringView command, String &out) const;

                /** @brief Starts launcher entry @p entry's command (on @p file if given), not waited for. */
                void launch(const DesktopEntry &entry, StringView file = StringView());

                /**
                 * @brief Starts @p entry on @p files (a line each): once with them all if its
                 *        command takes a list, else once a file, `MaxStartsPerDrop` at most
                 *        (`devplan/clipboard.md` §5b D11).
                 */
                void launchFiles(const DesktopEntry &entry, StringView files);

                /** @brief The most starts a drop of files on an application makes. */
                static constexpr size_t MaxStartsPerDrop = 16;

                /**
                 * @brief Opens @p path: a folder in a window (an application's `NAME.app`
                 *        folder: its application run), a launcher (`.desktop`) by its command,
                 *        another file by its type's application.
                 */
                void open(StringView path);

                /**
                 * @brief Starts application @p id (a Desktop Entry file in the launcher's
                 *        directories, the first found) on @p file (empty: none).
                 * @return False if it isn't there.
                 */
                bool openWith(StringView id, StringView file);

                /**
                 * @brief Opens file @p path of type @p type by the application `mimeapps.list`
                 *        names, else by the first whose `MimeType=` has it.
                 */
                void openFile(StringView path, StringView type);

                /** @brief The most applications Open With ▸ lists. */
                static constexpr size_t MaxOpenWith = 8;

                /**
                 * @brief Finds the applications that open file @p path for Open With ▸
                 *        (`devplan/filebrowser.md` §4l): those `mimeapps.list` names for its
                 *        type, its media's and (text) `text/plain`'s, then every launcher entry
                 *        whose `MimeType=` has one; the first is the one Open starts. Kept, with
                 *        @p path, until the next call.
                 * @return How many (`MaxOpenWith` at most; 0 for a folder or a device).
                 */
                size_t findOpenWith(StringView path);

                /** @brief Returns Open With's application @p index (`findOpenWith()`), or null. */
                const DesktopEntry *openWithEntry(size_t index) const;

                /** @brief Returns Open With's application @p index's icon, @p side pixels, or null. */
                const Icon *openWithIcon(size_t index, uint16_t side);

                /** @brief Starts Open With's application @p index on the file it was found for. */
                void openWithChosen(size_t index);

                /** @brief Puts @p paths on the clipboard, copied (`CopiedFiles`' types). */
                void copy(StringView paths);

                /** @brief Puts @p paths on the clipboard, cut: dimmed until pasted or the clipboard changes. */
                void cut(StringView paths);

                /** @brief Returns the files this panel put on the clipboard (`isCut()` for a path). */
                const CopiedFiles &copied() const { return _copied; }

                /**
                 * @brief Pastes the clipboard's files into @p directory: copied, or moved
                 *        if cut (a taken name asks).
                 */
                void paste(StringView directory);

                /** @brief Returns true if the clipboard has files to paste. */
                bool canPaste();

                /**
                 * @brief Fills @p drag to drag @p paths (a line each, kept until the drag
                 *        ends): a URI list and their paths as text; Copy, Move and Link
                 *        (`devplan/clipboard.md` §5e).
                 * @return False if there is nothing to drag.
                 */
                bool dragFiles(StringView paths, GuiDrag &drag);

                /**
                 * @brief What a drop of @p event's drag on @p target does: into a folder,
                 *        copied, moved (the same filesystem) or linked; on an application
                 *        (a launcher, a `NAME.app` folder), started with them (`Copy`); on
                 *        the trash, trashed (`Move` only).
                 *        `None` if it can't: no files, a folder into itself or where they are.
                 */
                DropAction dropAction(StringView target, const GuiDropEvent &event);

                /**
                 * @brief Takes a drop of @p event's drag on @p target (window @p window's,
                 *        at screen @p screenX, @p screenY for a right drag's menu): read,
                 *        then done, and finished.
                 * @return False if refused (`dropAction()` is `None`).
                 */
                bool drop(StringView target, GuiDropEvent &event, uint16_t screenX, uint16_t screenY);

                /** @brief Moves @p paths to their filesystems' trash. */
                void trash(StringView paths);

                /**
                 * @brief Deletes @p paths for good, once a message box naming them is
                 *        answered Yes (it says so when they are in the trash).
                 */
                void removeForGood(StringView paths);

                /** @brief Puts @p paths (in `/trash`) back where they were (`devplan/filebrowser.md` §4k). */
                void restore(StringView paths);

                /** @brief Deletes everything in the trash for good, once asked. */
                void emptyTrash();

                /** @brief Returns true if @p path is `/trash`, or leads there (a link to it). */
                static bool isTrash(StringView path);

                /**
                 * @brief Makes a new item of @p kind in @p directory, under the first free
                 *        name ("New Folder", then "New Folder (2)"...).
                 * @return Its path, or the error (told).
                 */
                Result<String> makeNew(StringView directory, FileOps::NewKind kind);

                /**
                 * @brief Renames @p path to @p name (@p suffix kept after it: a launcher's
                 *        `.desktop`), @p maxName bytes at most, its path into @p to.
                 * @return True if renamed; false with @p problem the message (empty: nothing
                 *         to do, the name the same).
                 */
                bool rename(StringView path, StringView name, StringView suffix, size_t maxName, String &to,
                            Span<char> problem);

        private:
                bool         openByMimeType(StringView path, StringView type);
                static void  pasted(void *arg, Error err, StringView type, StringView data);
                static Error render(void *arg, StringView type, String &out);
                static void  lost(void *arg, Clipboard::Selection selection);
                static Error renderDrag(void *arg, StringView type, String &out);
                static void  dragEnded(void *arg, DropAction done);
                static void  dropRead(void *arg, Error err, StringView type, StringView data);
                static void  askChosen(void *arg, int item);
                static void  askDismissed(void *arg, Menu &menu);
                static void  askCheck(void *arg);
                enum class Target : uint8_t {
                        None,
                        Folder,
                        Application,
                        Trash, // /trash, or a link to it: what is dropped is trashed (FB26)
                };
                Target       targetOf(StringView path, DesktopEntry *entry) const;
                void         dropDone(DropAction action); // _dropPaths into _dropTo, finished
                void         link(StringView paths, StringView directory);
                void         offer(StringView paths, bool cut);
                bool         moveNow(StringView paths, StringView directory);

                GuiApplication                &_app;
                const BuiltinProgram::Context &_context;
                FolderHost                    &_host;
                char                           _applications[256] = {};
                char                           _mimeApps[128] = {};
                CopiedFiles                    _copied;                   // what this panel copied or cut
                String                         _pasteTo{MemSpace::Psram}; // the folder a paste goes to
                FileJobs                       _jobs{_app};
                CopiedFiles                    _dragged;                    // a drag's files, until it ends
                String                         _dropTo{MemSpace::Psram};    // a drop's target
                String                         _dropPaths{MemSpace::Psram}; // and its files, once read
                uint32_t                       _dropDrag = 0;
                DropAction                     _dropAction = DropAction::None;
                bool                           _dropAsk = false;
                bool                           _askPending = false;
                uint16_t                       _dropX = 0; // a right drag's menu, screen coordinates
                uint16_t                       _dropY = 0;
                Menu                          *_askMenu = nullptr; // Move Here, Copy Here... (PSRAM)
                PanelEntry                    *_openWith = nullptr; // Open With's, MaxOpenWith (PSRAM)
                size_t                         _openWithCount = 0;
                String                         _openWithPath{MemSpace::Psram}; // the file they're for
};

BMDS_NAMESPACE_END
