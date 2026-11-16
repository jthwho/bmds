/**
 * @file      softwarepanel.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/widget.h>
#include <bmds/config.h>
#include <bmds/dialog.h>
#include <bmds/itemmodel.h>
#include <bmds/list.h>
#include <bmds/messagebox.h>
#include <bmds/pkgjob.h>
#include <bmds/string.h>

BMDS_NAMESPACE_BEGIN

class Button;
class DetailsView;
class ItemView;
class Label;
class LineEdit;
class SettingsWindow;
class TextBlock;

class SoftwarePanel;

/**
 * @brief Add Source...: a source's URL, looked at first (`pkg source add --dry-run`), its key
 *        shown whole when this system doesn't know it, then added (devplan/thirdparty.md §5).
 *
 * @par Thread Safety
 * The loop's thread.
 */
class AddSourceDialog final : public Dialog {
        public:
                /** @brief `Finished`'s results. */
                enum Choice : int {
                        Rejected = 0,
                        Accepted = 1,
                };

                AddSourceDialog(GuiApplication &app, SoftwarePanel &panel);

                /** @brief Makes the widgets. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief Opens it empty. */
                Error openEmpty();

                /** @brief What the look found (the panel tells it): @p text shown, Add offered or not. */
                void looked(StringView text, bool canAdd, bool newKey);

                /** @brief The URL typed, trimmed. */
                StringView url() const;

        private:
                static void lookClicked(void *arg, Button &button);
                static void urlAccepted(void *arg, LineEdit &edit);
                static void urlEdited(void *arg, LineEdit &edit);
                void        look();

                SoftwarePanel &_panel;
                LineEdit      *_url = nullptr;
                Button        *_look = nullptr;
                Button        *_add = nullptr;
                TextBlock     *_found = nullptr;
};

/**
 * @brief The Software page's sources and keys (devplan/thirdparty.md §5): each a list with its
 *        buttons, over `pkg --json` (`source list|add|remove`, `key list|add|remove`, `update`)
 *        run through `PkgJob`, as any program could. One command at a time; the lists are read
 *        again after each change.
 *
 * @par Thread Safety
 * The loop's thread.
 */
class SoftwarePanel final : public BoxLayout {
        public:
                SoftwarePanel(Widget *parent, SettingsWindow &window);
                ~SoftwarePanel() override;

                /** @brief Makes the widgets and reads the lists. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief Reads the sources and keys again (`pkg/sources` changed). */
                void refresh();

                /** @brief The dialog's Look: `pkg source add --dry-run URL`. */
                void look(StringView url);

        private:
                // What the job running is doing.
                enum class Stage : uint8_t {
                        Idle,
                        Sources,   // source list
                        Keys,      // key list
                        Look,      // source add --dry-run
                        Add,       // source add --trust ID (or -y)
                        Remove,    // source remove -y
                        AddKey,    // key add FILE
                        RemoveKey, // key remove ID
                        Update,    // update
                };

                struct SourceRow {
                                String   location;
                                String   keyName;
                                uint64_t packages = 0;
                                bool     listed = false;
                };

                struct KeyRow {
                                String id;
                                String kind;
                                String comment;
                };

                static void onRecord(void *arg, const PkgRecord &record);
                static void onFinished(void *arg, int status);
                static void clicked(void *arg, Button &button);
                static void selectionChanged(void *arg, ItemView &view);
                static void addFinished(void *arg, Dialog &dialog, int result);
                static void removeAnswered(void *arg, MessageBox::Answer answer);
                static void removeKeyAnswered(void *arg, MessageBox::Answer answer);
                static void keyFileChosen(void *arg, StringView path);

                void run(Stage stage, StringView args); // `pkg --json ARGS`
                void say(StringView text);
                void fillSources();
                void fillKeys();
                void updateButtons();
                void lookRecord(const PkgRecord &r);

                SettingsWindow &_window;
                PkgJob          _job;
                Stage           _stage = Stage::Idle;
                bool            _thenKeys = false; // after the sources, the keys (refresh())
                ListItemModel   _sourceModel;
                ListItemModel   _keyModel;
                List<SourceRow> _sources{MemSpace::Psram};
                List<KeyRow>    _keys{MemSpace::Psram};
                DetailsView    *_sourceView = nullptr;
                DetailsView    *_keyView = nullptr;
                Button         *_addSource = nullptr;
                Button         *_removeSource = nullptr;
                Button         *_update = nullptr;
                Button         *_addKey = nullptr;
                Button         *_removeKey = nullptr;
                Label          *_state = nullptr;
                AddSourceDialog _dialog;
                // The look's findings, for Add and the dialog's text.
                String   _lookLocation{MemSpace::Psram};
                String   _lookId;
                String   _lookText{MemSpace::Psram};
                String   _failure{MemSpace::Psram};
                uint64_t _lookPackages = 0;
                bool     _lookSigned = false;
                bool     _lookNew = false;
                bool     _lookKey = false;          // a key record came
                String   _pending{MemSpace::Psram}; // what Remove asked about
                uint32_t _sourcesRead = 0;          // update's
                uint32_t _sourcesFailed = 0;
};

BMDS_NAMESPACE_END
