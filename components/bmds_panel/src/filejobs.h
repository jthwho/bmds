/**
 * @file      filejobs.h
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
#include <bmds/dialog.h>
#include <bmds/fileworker.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/messagebox.h>
#include <bmds/string.h>
#include <bmds/stringview.h>
#include <bmds/widget.h>

BMDS_NAMESPACE_BEGIN

/** @brief Writes @p bytes as a person reads them (bytes, KB, MB or GB, whole) into @p out. */
void formatBytes(uint64_t bytes, char *out, size_t size);

/**
 * @brief The panel's long file operations (`devplan/filebrowser.md` §4j,
 *        FB13): a `FileWorker` and its windows.
 *
 * - **Progress:** a window opened once a job has run for a second (the item,
 *   where it goes, a bar, the counts, the jobs waiting, Cancel), closed when
 *   none is left.
 * - **A name taken:** a window asking Replace (Merge for a folder), Keep Both,
 *   Skip or Cancel, with "Do the same for the others".
 * - **Failures:** a message box when the job is done.
 *
 * The views follow the changes by their watches; nothing is told to them.
 *
 * @par Thread Safety
 * The panel's loop.
 */
class FileJobs {
        public:
                /** @brief A second: a job running longer shows its progress. */
                static constexpr uint32_t ShowAfterMs = 1000;

                explicit FileJobs(GuiApplication &app) : _app(app) {}

                /** @brief Cancels the running job and drops the queue (the worker waited for). */
                ~FileJobs();

                FileJobs(const FileJobs &) = delete;
                FileJobs &operator=(const FileJobs &) = delete;

                /** @brief Copies @p paths (absolute, a line each) into folder @p target. */
                void copy(StringView paths, StringView target);

                /** @brief Moves @p paths (absolute, a line each) into folder @p target. */
                void move(StringView paths, StringView target);

                /**
                 * @brief Deletes @p paths (absolute, a line each) for good, once a message
                 *        box naming them is answered Yes.
                 */
                void removeAsking(StringView paths);

        private:
                // A name taken: the answers (the dialog's results).
                enum Answer : int {
                        AnswerReplace = 1,
                        AnswerKeepBoth,
                        AnswerSkip,
                        AnswerCancel,
                };

                static void notified(void *arg); // the worker's thread
                static void due(void *arg);      // the loop's
                static void showDue(void *arg);  // a second after a job began
                static void progressFinished(void *arg, Dialog &dialog, int result);
                static void conflictFinished(void *arg, Dialog &dialog, int result);
                static void confirmed(void *arg, MessageBox::Answer answer);

                void  add(FileWorker::Kind kind, StringView paths, StringView target);
                void  update();
                void  showProgress();
                void  closeProgress();
                void  ask();
                void  tellDone(const FileWorker::Done &done);
                Error buildProgress();
                Error buildConflict();

                GuiApplication    &_app;
                FileWorker         _worker{notified, this};
                std::atomic<bool>  _posted{false}; // due() on its way
                FileWorker::Status _status;
                uint32_t           _seenJob = 0; // the job last seen running
                int                _showTimer = -1;
                Dialog            *_progress = nullptr; // PSRAM, made the first time
                Label             *_what = nullptr;
                Label             *_where = nullptr;
                ProgressBar       *_bar = nullptr;
                Label             *_counts = nullptr;
                Label             *_waiting = nullptr;
                bool               _progressShown = false;
                Dialog            *_conflict = nullptr; // PSRAM, made the first time
                Label             *_lines[3] = {};
                CheckBox          *_forAll = nullptr;
                Button            *_replace = nullptr;
                bool               _conflictShown = false;
                String             _removing{MemSpace::Psram}; // paths waiting for Yes
};

BMDS_NAMESPACE_END
