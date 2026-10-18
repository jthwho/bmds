/**
 * @file      propertiesdialog.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <atomic>
#include <bmds/config.h>
#include <bmds/dialog.h>
#include <bmds/error.h>
#include <bmds/fileicons.h>
#include <bmds/fileworker.h>
#include <bmds/furniture.h>
#include <bmds/lineedit.h>
#include <bmds/string.h>
#include <bmds/stringview.h>

#include "folderactions.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief The Properties of files (`devplan/filebrowser.md` §4k, FB27): one
 *        item's name (renamed at OK), type, location, size, modification time
 *        and attributes, a link's target, a trashed item's original location
 *        and deletion date; several items' totals; a filesystem's type,
 *        capacity and free space.
 *
 * A folder's contents are counted on a `FileWorker` of the dialog's own
 * (`Measure`), so counting never waits behind a long copy; the totals show
 * as they grow, and counting stops when the dialog closes.
 *
 * Made by `show()`, it destroys itself after it closes.
 *
 * @par Thread Safety
 * The panel's loop.
 */
class PropertiesDialog final : public Dialog {
        public:
                /** @brief The most open at once. */
                static constexpr size_t MaxOpen = 8;

                /**
                 * @brief Opens the properties of @p paths (absolute, a line each).
                 * @return `Ok`, `Invalid` (no paths), `Full` (`MaxOpen` already), `NoMem`,
                 *         or the window manager's error.
                 */
                static Error show(GuiApplication &app, FolderActions &actions, FileIcons &icons, StringView paths);

                ~PropertiesDialog() override;

        private:
                // A line of the form: its label, and the value beside it.
                struct Line {
                                Label *label = nullptr;
                                Label *value = nullptr;
                };

                PropertiesDialog(GuiApplication &app, FolderActions &actions, StringView title, uint16_t height);

                Line addLine(BoxLayout &column, StringView label, StringView value);
                void startCounting();
                void showCounts(uint64_t items, uint64_t bytes, bool done, const FileOps::Totals *totals);
                void rename();

                static void notified(void *arg); // the worker's thread
                static void due(void *arg);      // the loop's
                static void finished(void *arg, Dialog &dialog, int result);
                static void destroy(void *arg);
                static bool isOpen(const PropertiesDialog *dialog);

                FolderActions    &_actions;
                std::atomic<bool> _posted{false};
                FileWorker        _worker{notified, this}; // after what its notify uses: gone first
                String            _paths{MemSpace::Psram}; // what it describes, a line each
                String            _name{MemSpace::Psram};  // one item's name, as it was
                LineEdit         *_nameEdit = nullptr;     // one item's, if it can be renamed
                Label            *_size = nullptr;         // counted: the size
                Label            *_contains = nullptr;     // and what folders hold
                uint64_t          _folders = 0;            // the folders selected (not counted as contents)
                uint64_t          _files = 0;              // the files selected
                bool              _counting = false;
                uint32_t         *_iconPixels = nullptr; // PSRAM: the icon's copy (a new theme drops the theme's)
};

BMDS_NAMESPACE_END
