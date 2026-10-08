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
#include <bmds/config.h>
#include <bmds/desktopentry.h>
#include <bmds/error.h>
#include <bmds/fileops.h>
#include <bmds/guiapplication.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

BMDS_NAMESPACE_BEGIN

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

        protected:
                ~FolderHost() = default;
};

/**
 * @brief What a folder window and the desktop both do to files
 *        (`devplan/filebrowser.md` §4h, FB8), written once: open them (by their
 *        type's application, a launcher by its command, a folder in a window),
 *        copy their paths to the clipboard, paste a URI list into a folder,
 *        move them to the trash, make new ones and rename them. A failure is
 *        told in a message box.
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
                 * @brief Opens @p path: a folder in a window, a launcher (`.desktop`) by its
                 *        command, another file by its type's application.
                 */
                void open(StringView path);

                /** @brief Opens file @p path of type @p type by the application `mimeapps.list` names. */
                void openFile(StringView path, StringView type);

                /** @brief Puts @p paths on the clipboard (`text/uri-list`, and as text). */
                void copy(StringView paths);

                /** @brief Pastes the clipboard's files into @p directory (a copy's name if one is taken). */
                void paste(StringView directory);

                /** @brief Returns true if the clipboard has files to paste. */
                bool canPaste();

                /** @brief Moves @p paths to their filesystems' trash. */
                void trash(StringView paths);

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
                static void  pasted(void *arg, Error err, StringView type, StringView data);
                static Error render(void *arg, StringView type, String &out);
                bool         openWith(StringView id, StringView file);

                GuiApplication                &_app;
                const BuiltinProgram::Context &_context;
                FolderHost                    &_host;
                char                           _applications[256] = {};
                char                           _mimeApps[128] = {};
                String                         _copied{MemSpace::Psram};  // the paths copied, a line each
                String                         _pasteTo{MemSpace::Psram}; // the folder a paste goes to
};

BMDS_NAMESPACE_END
