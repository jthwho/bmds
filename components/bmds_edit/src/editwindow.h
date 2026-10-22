/**
 * @file      editwindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/filedialog.h>
#include <bmds/findbar.h>
#include <bmds/furniture.h>
#include <bmds/guiapplication.h>
#include <bmds/guiwindow.h>
#include <bmds/menu.h>
#include <bmds/menubar.h>
#include <bmds/string.h>
#include <bmds/sysconf.h>
#include <bmds/tabwidget.h>
#include <bmds/codeedit.h>
#include <bmds/furniture.h>
#include <bmds/terminalwidget.h>
#include "languagesupport.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief `edit`'s window (`devplan/editor.md` §8): documents in tabs, each a `CodeEdit`
 *        coloured for its language; the menus; the find bar; the status bar; a pane below
 *        (Output, Problems, Help) for a language's tools (§9): AngelScript's checks as you
 *        type, Run and Stop, F1 help, completion.
 *
 * @par Thread Safety
 * The GUI loop's thread (the application's lock held).
 */
class EditWindow final : public GuiWindow, private LanguageSupport::Listener {
        public:
                /** @brief What it starts programs with (its process's). */
                struct Startup {
                                StringView             cwd;
                                Span<const StringView> environment; ///< Kept: the program's, for its life.
                                ProgramLauncher       *launcher = nullptr;
                                ProcessControl        *process = nullptr;
                };

                EditWindow(GuiApplication &app, const Config &config, const Startup &startup);
                ~EditWindow() override;

                /** @brief Makes its widgets. */
                Error build();
                /** @brief Opens @p path (absolute) in a tab, or shows the tab it is in; a file not there yet is new. */
                void open(StringView path);
                /** @brief A new, empty, untitled document. */
                void newDocument();

        protected:
                bool keyFirst(const KeyEvent &event) override;
                bool keyUnused(const KeyEvent &event) override;
                void closedByManager() override;

        private:
                struct Document {
                                CodeEdit *edit = nullptr;
                                String    path{MemSpace::Psram}; // "" for an untitled one
                                uint32_t  untitled = 0;          // its number, "Untitled 2"
                                bool      wasModified = false;   // as the title shows it
                                bool      readOnly = false;      // its file can't be written (/rom's)
                                uint32_t  id = 0;                // for checks' answers, which may come late
                };

                enum Choice : int {
                        ChooseNew = 1,
                        ChooseOpen,
                        ChooseSave,
                        ChooseSaveAs,
                        ChooseSaveAll,
                        ChooseRevert,
                        ChooseClose,
                        ChooseQuit,
                        ChooseUndo,
                        ChooseRedo,
                        ChooseCut,
                        ChooseCopy,
                        ChoosePaste,
                        ChooseDelete,
                        ChooseSelectAll,
                        ChooseFind,
                        ChooseFindNext,
                        ChooseFindPrevious,
                        ChooseReplace,
                        ChooseGoToLine,
                        ChooseIndent,
                        ChooseUnindent,
                        ChooseEndingsLf,
                        ChooseEndingsCrLf,
                        ChooseComment,
                        ChooseLineNumbers,
                        ChooseLanguage,                     // + the language's index (0: plain text)
                        ChooseScheme = ChooseLanguage + 32, // + the scheme's index
                        ChooseRun = ChooseScheme + 16,
                        ChooseStop,
                        ChooseCheck,
                        ChooseClearOutput,
                        ChoosePane,
                        ChooseNextProblem,
                        ChoosePreviousProblem,
                        ChooseHelpWord,
                        ChooseComplete,
                        ChooseExamples,
                        ChooseTemplate,                         // + the template's index
                        ChooseCompletion = ChooseTemplate + 16, // + the candidate's index
                        ChooseAbout,
                };

                // What a question about unsaved changes leads to once answered.
                enum class Then : uint8_t {
                        CloseTab,
                        Quit,
                };

                static void chosen(void *arg, int item);
                static void aboutToShow(void *arg, MenuBar &bar, size_t index, Menu &menu);
                static void tabChanged(void *arg, TabWidget &tabs, size_t index);
                static void textChanged(void *arg, TextEdit &edit);
                static void cursorMoved(void *arg, TextEdit &edit);
                static void findToggled(void *arg, FindBar &bar);
                static void opened(void *arg, FileDialog &dialog, int result);
                static void savedAs(void *arg, FileDialog &dialog, int result);
                static void answered(void *arg, MessageBox::Answer answer);
                static void settingsChanged(void *arg);
                static void settingsDue(void *arg);
                static void checkDue(void *arg);
                static void runEnded(void *arg, TerminalWidget &terminal, int status);
                static void problemPicked(void *arg, TextEdit &edit);
                static void completionChosen(void *arg, int item);
                static void examplesChosen(void *arg, FileDialog &dialog, int result);

                // LanguageSupport::Listener
                void checked(uint32_t token, Span<const CodeEdit::Diagnostic> diagnostics) override;
                void namesChanged() override;

                Error     buildMenus();
                Document *current();
                int       indexOf(const TextEdit *edit) const;
                int       indexOfPath(StringView path) const;
                Document *addDocument();
                Error     load(Document &doc, StringView path);
                bool      save(size_t index);   // false: it needs a name first (Save As asked)
                Error     write(Document &doc); // to doc.path
                void      saveAs(size_t index);
                void      closeTab(size_t index, bool ask);
                void      quit();
                bool      askNext(Then then); // the next modified document's question; false: none left
                void      revert(size_t index);
                void      choose(int item);
                void      showStatus();
                void      showTitle(size_t index);
                void      readSettings();
                void      apply(CodeEdit &edit);
                void      chooseLanguage(Document &doc);
                void      setSetting(StringView key, StringView value);
                void      warn(StringView text, StringView path, Error err);
                String    nameOf(const Document &doc) const;
                // The language's tools (editrun.cpp)
                Error            buildPane(Widget *parent);
                LanguageSupport *supportFor(const Document &doc);
                Document        *documentById(uint32_t id);
                void             scheduleCheck(uint32_t delayMs);
                void             checkCurrent();
                void             run();
                void             stopRun();
                void             showPane(size_t tab);
                void             showProblems();
                void             helpAtCursor();
                void             complete();
                void             newFromTemplate(size_t index);
                void             wordAtCursor(StringView &owner, StringView &word, TextRange &range);
                String           declaredType(StringView owner);

                String          _cwd{MemSpace::Psram};
                Startup         _startup;
                Menu            _fileMenu;
                Menu            _editMenu;
                Menu            _viewMenu;
                Menu            _helpMenu;
                Menu            _endingsMenu;
                Menu            _languageMenu;
                Menu            _schemeMenu;
                Menu            _runMenu;
                Menu            _newMenu;
                Menu            _completionMenu;
                Splitter       *_split = nullptr;
                TabWidget      *_pane = nullptr;
                TerminalWidget *_output = nullptr;
                TextEdit       *_problems = nullptr;
                TextEdit       *_helpView = nullptr;
                FileDialog      _examplesDialog;
                AngelSupport   *_angel = nullptr;
                int             _checkTimer = -1;
                uint32_t        _nextId = 0;
                uint32_t        _runDoc = 0;              // the document running (its id)
                FileHandle::Ptr _report;                  // the run's report's end
                bool            _runAfterSave = false;    // Save As asked by Run: run once saved
                bool            _stopAsked = false;       // Stop pressed once: the next kills
                bool            _problemsShowing = false; // filling the list (not the user's click)
                List<const LanguageSupport::Entry *> _completions{MemSpace::Psram};
                TextRange                            _completionRange;
                TabWidget                           *_tabs = nullptr;
                FindBar                             *_findBar = nullptr;
                StatusBar                           *_status = nullptr;
                BoxLayout                           *_column = nullptr;
                FileDialog                           _openDialog;
                FileDialog                           _saveDialog;
                Document                             _docs[TabWidget::MaxTabs];
                size_t                               _count = 0;
                uint32_t                             _untitled = 0;  // the last "Untitled N" given
                int                                  _savingAs = -1; // the document Save As is for
                int                                  _asking = -1;   // the document a question is about
                Then                                 _then = Then::CloseTab;
                bool                                 _quitting = false; // after the questions, quit
                SysConf::Watcher                     _settings;
                uint32_t                             _tabWidth = 8;
                bool                                 _insertSpaces = false;
                bool                                 _autoIndent = true;
                bool                                 _lineNumbers = true;
                const CodeEdit::Scheme              *_scheme = nullptr;
};

BMDS_NAMESPACE_END
