/**
 * @file      startwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Preso's start window (devplan/preso.md §8b): a few buttons on the GUI service, the toolkit's
 * file dialog for a deck, and message boxes for decks that won't load. The window is closed
 * while a deck is up (a full screen covers it anyway, and nobody reads its events then) and
 * made again when the deck ends.
 */

#include "startwindow.h"

#include <bmds/guiclasses.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace preso {

        namespace {

                constexpr size_t MaxPath = 512;
                constexpr size_t MaxTold = 900; ///< A message box's text: the first problems.

                bool isFile(const char *path) {
                        struct stat st;
                        return stat(path, &st) == 0 && S_ISREG(st.st_mode);
                }

                // The folder @p program (argv[0]) is in: as given with a `/`, else the first `PATH`
                // folder that has it. False if neither.
                bool programFolder(const char *program, char *out, size_t size) {
                        if (strchr(program, '/') != nullptr) {
                                const char *slash = strrchr(program, '/');
                                snprintf(out, size, "%.*s", int(slash - program), program);
                                return true;
                        }
                        const char *path = getenv("PATH");
                        while (path != nullptr && *path != '\0') {
                                const char  *end = strchr(path, ':');
                                const size_t n = end != nullptr ? size_t(end - path) : strlen(path);
                                char         candidate[MaxPath];
                                snprintf(candidate, sizeof(candidate), "%.*s/%s", int(n), path, program);
                                if (n > 0 && isFile(candidate)) {
                                        snprintf(out, size, "%.*s", int(n), path);
                                        return true;
                                }
                                path = end != nullptr ? end + 1 : nullptr;
                        }
                        return false;
                }

                // A message box on @p gui, owned by @p owner (0: none): its id.
                uint32_t openMessage(bmds_gui_t *gui, uint32_t owner, const char *title, const char *text) {
                        const uint32_t box = bmds_message_box_new(gui, 0);
                        bmds_message_box_set_kind(gui, box, "error");
                        bmds_message_box_set_title(gui, box, title);
                        bmds_message_box_set_text(gui, box, text);
                        if (owner != 0) bmds_message_box_set_owner(gui, box, owner);
                        bmds_message_box_open(gui, box);
                        return box;
                }

                // What a deck at @p path that didn't present said, for a message box.
                void failure(const char *path, int status, const char *problems, char *out, size_t size) {
                        const char *name = strrchr(path, '/') != nullptr ? strrchr(path, '/') + 1 : path;
                        const int   n = snprintf(out, size, "%s %s.", name,
                                               status == 2 ? "couldn't be opened" : "stopped with a problem");
                        if (problems != nullptr && *problems != '\0' && n > 0 && size_t(n) + 2 < size)
                                snprintf(out + n, size - size_t(n), "\n\n%s", problems);
                        return;
                }

                /**
                 * @brief The start window: made, closed while a deck is up, made again.
                 *
                 * @par Thread Safety
                 * The program's one thread.
                 */
                class StartWindow {
                        public:
                                StartWindow(bmds_gui_t *gui, PresentDeck present) : _gui(gui), _present(present) {}

                                /** @brief Finds the examples beside @p program, and the folder to start in. */
                                void setUp(const char *program);

                                /** @brief Shows the window and takes its events until it is closed. */
                                void run();

                        private:
                                void build();
                                void chooseDeck();
                                void openDeck(const char *path);

                                bmds_gui_t *_gui;
                                PresentDeck _present;
                                uint32_t    _window = 0, _open = 0, _tour = 0, _talk = 0, _show = 0, _quit = 0;
                                uint32_t    _dialog = 0; ///< The file dialog while it is up.
                                char        _tourPath[MaxPath] = {}, _talkPath[MaxPath] = {}, _showPath[MaxPath] = {};
                                char        _folder[MaxPath] = {}; ///< The last deck's, else $HOME.
                };

                void StartWindow::setUp(const char *program) {
                        char folder[MaxPath / 2]; // room for the examples' paths after it
                        if (programFolder(program, folder, sizeof(folder))) {
                                snprintf(_tourPath, sizeof(_tourPath), "%s/../share/preso/examples/tour.md", folder);
                                snprintf(_talkPath, sizeof(_talkPath), "%s/../share/preso/examples/bmds/bmds.md",
                                         folder);
                                snprintf(_showPath, sizeof(_showPath),
                                         "%s/../share/preso/examples/showcase/showcase.md", folder);
                        }
                        if (!isFile(_tourPath)) _tourPath[0] = '\0';
                        if (!isFile(_talkPath)) _talkPath[0] = '\0';
                        if (!isFile(_showPath)) _showPath[0] = '\0';
                        const char *home = getenv("HOME");
                        snprintf(_folder, sizeof(_folder), "%s", home != nullptr ? home : "/");
                        return;
                }

                void StartWindow::build() {
                        _window = bmds_window_new(_gui, 0);
                        bmds_window_set_title(_gui, _window, "Preso");
                        bmds_box_set_margin(_gui, _window, 10);
                        bmds_box_set_spacing(_gui, _window, 6);
                        const uint32_t heading = bmds_label_new(_gui, _window);
                        bmds_widget_set_font(_gui, heading, "IBM Plex Sans 18 bold");
                        bmds_label_set_text(_gui, heading, "Preso");
                        const uint32_t about = bmds_label_new(_gui, _window);
                        bmds_label_set_text(_gui, about, "Presentations written in Markdown.");
                        _open = bmds_button_new(_gui, _window);
                        bmds_button_set_text(_gui, _open, "&Open a Deck...");
                        _tour = _talk = _show = 0;
                        if (_tourPath[0] != '\0' || _talkPath[0] != '\0' || _showPath[0] != '\0') {
                                const uint32_t examples = bmds_label_new(_gui, _window);
                                bmds_label_set_text(_gui, examples, "Or an example:");
                                const uint32_t row = bmds_box_new(_gui, _window);
                                bmds_box_set_direction(_gui, row, "across");
                                bmds_box_set_spacing(_gui, row, 6);
                                if (_tourPath[0] != '\0') {
                                        _tour = bmds_button_new(_gui, row);
                                        bmds_button_set_text(_gui, _tour, "Preso's &Tour");
                                }
                                if (_talkPath[0] != '\0') {
                                        _talk = bmds_button_new(_gui, row);
                                        bmds_button_set_text(_gui, _talk, "The &BMDS Talk");
                                }
                                if (_showPath[0] != '\0') {
                                        _show = bmds_button_new(_gui, row);
                                        bmds_button_set_text(_gui, _show, "&Scenes Showcase");
                                }
                        }
                        const uint32_t bottom = bmds_box_new(_gui, _window);
                        bmds_box_set_direction(_gui, bottom, "across");
                        const uint32_t gap = bmds_label_new(_gui, bottom);
                        bmds_widget_set_stretch(_gui, gap, 1);
                        _quit = bmds_button_new(_gui, bottom);
                        bmds_button_set_text(_gui, _quit, "&Quit");
                        bmds_window_show(_gui, _window);
                        bmds_widget_set_focus(_gui, _open);
                        return;
                }

                void StartWindow::chooseDeck() {
                        if (_dialog != 0) return;
                        _dialog = bmds_file_dialog_new(_gui, 0);
                        bmds_file_dialog_set_mode(_gui, _dialog, "open");
                        bmds_file_dialog_set_title(_gui, _dialog, "Open a Deck");
                        bmds_file_dialog_set_folder(_gui, _dialog, _folder);
                        bmds_file_dialog_add_filter(_gui, _dialog, "Decks", "*.md");
                        bmds_file_dialog_add_filter(_gui, _dialog, "All files", "*");
                        bmds_file_dialog_set_owner(_gui, _dialog, _window);
                        bmds_file_dialog_open(_gui, _dialog);
                        return;
                }

                void StartWindow::openDeck(const char *path) {
                        const char *slash = strrchr(path, '/');
                        if (slash != nullptr && path != _tourPath && path != _talkPath && path != _showPath)
                                snprintf(_folder, sizeof(_folder), "%.*s", int(slash - path), path);
                        // The window goes while the deck is up: the screen and the memory are the deck's.
                        bmds_gui_destroy(_gui, _window);
                        _window = 0;
                        char     *problems = nullptr;
                        size_t    length = 0;
                        FILE     *messages = open_memstream(&problems, &length);
                        const int status = _present(path, messages != nullptr ? messages : stdout);
                        if (messages != nullptr) fclose(messages);
                        build();
                        if (status != 0) {
                                char text[MaxTold];
                                failure(path, status, problems, text, sizeof(text));
                                (void)openMessage(_gui, _window, "Preso", text);
                        }
                        free(problems);
                        return;
                }

                void StartWindow::run() {
                        build();
                        for (;;) {
                                bmds_gui_event_t e;
                                if (bmds_gui_next_event(_gui, &e, -1) < 0) break;
                                if (e.source == _window && bmds_gui_event_is(_gui, &e, BMDS_GUI_WINDOW_CLOSED)) break;
                                if (e.source == _dialog && bmds_gui_event_is(_gui, &e, BMDS_GUI_FILE_DIALOG_FINISHED)) {
                                        bmds_gui_value_t item = {};
                                        char             path[MaxPath] = {};
                                        if (e.count == 2 && e.values[0].number != 0 &&
                                            bmds_gui_list_item(&e.values[1], 0, &item) == 0)
                                                snprintf(path, sizeof(path), "%.*s", int(item.length), item.text);
                                        bmds_gui_destroy(_gui, _dialog);
                                        _dialog = 0;
                                        if (path[0] != '\0') openDeck(path);
                                        continue;
                                }
                                if (!bmds_gui_event_is(_gui, &e, BMDS_GUI_BUTTON_CLICKED)) continue;
                                if (e.source == _quit) break;
                                if (e.source == _open) chooseDeck();
                                if (e.source == _tour && _tour != 0) openDeck(_tourPath);
                                if (e.source == _talk && _talk != 0) openDeck(_talkPath);
                                if (e.source == _show && _show != 0) openDeck(_showPath);
                        }
                        return;
                }

        } // namespace

        int runStartWindow(const char *program, PresentDeck present) {
                bmds_gui_t *gui = bmds_gui_connect(nullptr);
                if (gui == nullptr) {
                        fputs("preso: no GUI to open a deck from (preso DECK presents one)\n", stderr);
                        return 1;
                }
                StartWindow window(gui, present);
                window.setUp(program);
                window.run();
                bmds_gui_close(gui);
                return 0;
        }

        bool tellInWindow(const char *title, const char *text) {
                bmds_gui_t *gui = bmds_gui_connect(nullptr);
                if (gui == nullptr) return false;
                const uint32_t box = openMessage(gui, 0, title, text);
                for (;;) {
                        bmds_gui_event_t e;
                        if (bmds_gui_next_event(gui, &e, -1) < 0) break;
                        if (e.source == box && bmds_gui_event_is(gui, &e, BMDS_GUI_MESSAGE_BOX_ANSWERED)) break;
                }
                bmds_gui_close(gui);
                return true;
        }

} // namespace preso
