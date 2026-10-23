/**
 * @file      guigallery.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * guigallery: the GUI service's classes, a part at a time (devplan/guiprograms.md §10), through
 * the generated wrappers (bmds/guiclasses.h), on tabs under a tool bar, the last event in the
 * status bar. Windows: its size shown as it changes, a smallest size, a busy pointer, an About box
 * (owned and modal, also on the window menu), and a close that asks first. Controls: a form of
 * check box, number field, slider (with a progress bar following it) and combo box. Layouts: a
 * splitter between a list view and an icon view. Views: the folders as a tree (children read as
 * a folder opens) and the chosen one's entries in a details view. Text: a code editor, its cursor and state shown. Terminal: a shell
 * on the program's own pseudo-terminal, its output written to the widget, the typing sent back.
 */

#include <bmds/guiclasses.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

typedef struct Gallery {
                bmds_gui_t *gui;
                uint32_t    window, size, busy, minimize, maximize, about, quit;
                uint32_t    status, toolAbout, toolQuit, tabs;
                uint32_t    check, number, slider, progress, combo;
                uint32_t    tree, files, code, codeState, term;
                int         master; // the shell's pty (-1: none)
                pid_t       shell;
                uint32_t    dialog, dialogOk, dialogNo; // the open About or Quit box (0: none)
                int         asking;                     // the box is "Quit?"
                int         busyOn;
} Gallery;

// A box owned by the main window and modal over it: About (one button) or Quit? (two).
static void openBox(Gallery *g, const char *title, const char *text, int asking) {
        bmds_gui_t *gui = g->gui;
        g->dialog = bmds_window_new(gui, 0);
        bmds_window_set_margin(gui, g->dialog, 8); // plain widgets: padded
        bmds_window_set_spacing(gui, g->dialog, 4);
        bmds_window_set_title(gui, g->dialog, title);
        bmds_window_set_owner(gui, g->dialog, g->window);
        bmds_window_set_modal(gui, g->dialog, 1);
        bmds_window_set_buttons(gui, g->dialog, 0);
        bmds_window_set_resizable(gui, g->dialog, 0);
        bmds_window_set_icon(gui, g->dialog, asking ? "dialog-question" : "help-about");
        const uint32_t label = bmds_label_new(gui, g->dialog);
        bmds_label_set_text(gui, label, text);
        const uint32_t row = bmds_gui_new(gui, "Box", g->dialog);
        bmds_box_set_direction(gui, row, "across");
        g->dialogOk = bmds_button_new(gui, row);
        bmds_button_set_text(gui, g->dialogOk, asking ? "&Quit" : "OK");
        g->dialogNo = 0;
        if (asking) {
                g->dialogNo = bmds_button_new(gui, row);
                bmds_button_set_text(gui, g->dialogNo, "&Stay");
        }
        g->asking = asking;
        bmds_window_show(gui, g->dialog);
        bmds_widget_set_focus(gui, g->dialogOk);
        return;
}

// ---- Views: folders read as the tree opens them, the chosen one's entries listed

// Folder @p path's entries: the subfolders' names into the tree (@p folders), or every entry with
// its size into the details view.
static void readFolder(Gallery *g, const char *path, int folders) {
        DIR *dir = opendir(path);
        if (folders) {
                const char    *names[64];
                char           store[64][64];
                uint32_t       n = 0;
                struct dirent *d;
                while (dir != NULL && n < 64 && (d = readdir(dir)) != NULL) {
                        if (d->d_name[0] == '.') continue;
                        char        full[512];
                        struct stat st;
                        snprintf(full, sizeof(full), "%s/%s", path, d->d_name);
                        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                        snprintf(store[n], sizeof(store[n]), "%s", d->d_name);
                        names[n] = store[n];
                        ++n;
                }
                bmds_tree_view_set_children(g->gui, g->tree, path, names, n);
        } else {
                bmds_item_view_clear(g->gui, g->files);
                struct dirent *d;
                uint32_t       row = 0;
                while (dir != NULL && row < 200 && (d = readdir(dir)) != NULL) {
                        if (d->d_name[0] == '.') continue;
                        char        full[512], size[24];
                        struct stat st;
                        snprintf(full, sizeof(full), "%s/%s", path, d->d_name);
                        const int folder = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
                        if (folder)
                                snprintf(size, sizeof(size), "folder");
                        else
                                snprintf(size, sizeof(size), "%lld", (long long)st.st_size);
                        const char *texts[] = {d->d_name, size};
                        bmds_details_view_add_row(g->gui, g->files, texts, 2);
                        bmds_item_view_set_icon(g->gui, g->files, (int32_t)row++, folder ? "folder" : "text-x-generic");
                }
        }
        if (dir != NULL) closedir(dir);
        return;
}

// ---- Terminal: a shell on our own pseudo-terminal

static void startShell(Gallery *g) {
        int            s = -1;
        struct winsize ws = {.ws_col = 80, .ws_row = 24};
        if (openpty(&g->master, &s, NULL, NULL, &ws) != 0) {
                g->master = -1;
                bmds_terminal_write(g->gui, g->term, "openpty failed\r\n");
                return;
        }
        const pid_t pid = vfork();
        if (pid == 0) {
                if (setsid() < 0 || ioctl(s, TIOCSCTTY, 0) != 0) _exit(126);
                if (dup2(s, 0) < 0 || dup2(s, 1) < 0 || dup2(s, 2) < 0) _exit(126);
                close(s);
                close(g->master);
                char *argv[] = {"sh", NULL};
                execve("/bin/sh", argv, environ);
                _exit(127);
        }
        close(s);
        g->shell = pid;
        if (pid < 0) {
                close(g->master);
                g->master = -1;
                bmds_terminal_write(g->gui, g->term, "can't start /bin/sh\r\n");
        }
        return;
}

// The shell's output to the widget; its end said.
static void shellOutput(Gallery *g) {
        char          buf[1024];
        const ssize_t n = read(g->master, buf, sizeof(buf) - 1);
        if (n > 0) {
                buf[n] = '\0';
                bmds_terminal_write(g->gui, g->term, buf);
                return;
        }
        int status = 0;
        (void)waitpid(g->shell, &status, 0);
        char text[64];
        snprintf(text, sizeof(text), "\r\n[the shell ended: %d]\r\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        bmds_terminal_write(g->gui, g->term, text);
        close(g->master);
        g->master = -1;
        return;
}

// The views', text's and terminal's events. @return 1 if it was one of theirs.
static int pageEvent(Gallery *g, const bmds_gui_event_t *e) {
        bmds_gui_t *gui = g->gui;
        if (e->source == g->tree && e->count == 1) {
                char path[256];
                snprintf(path, sizeof(path), "%.*s", (int)e->values[0].length, e->values[0].text);
                if (bmds_gui_event_is(gui, e, BMDS_GUI_TREE_VIEW_EXPANDING)) readFolder(g, path, 1);
                if (bmds_gui_event_is(gui, e, BMDS_GUI_TREE_VIEW_CURRENT_CHANGED)) readFolder(g, path, 0);
                return 1;
        }
        if (e->source == g->code) {
                bmds_gui_value_t line, column, modified;
                if (bmds_text_edit_get_cursor_line(gui, g->code, &line) == 0 &&
                    bmds_text_edit_get_cursor_column(gui, g->code, &column) == 0 &&
                    bmds_text_edit_get_modified(gui, g->code, &modified) == 0) {
                        char text[64];
                        snprintf(text, sizeof(text), "Line %d, byte %d%s", (int)line.number + 1, (int)column.number + 1,
                                 modified.number != 0 ? ", modified" : "");
                        bmds_label_set_text(gui, g->codeState, text);
                }
                return 1;
        }
        if (e->source == g->term) {
                if (bmds_gui_event_is(gui, e, BMDS_GUI_TERMINAL_TYPED) && g->master >= 0 && e->count == 1)
                        (void)write(g->master, e->values[0].text, e->values[0].length);
                if (bmds_gui_event_is(gui, e, BMDS_GUI_TERMINAL_SIZED) && g->master >= 0 && e->count == 2) {
                        struct winsize ws = {.ws_col = (unsigned short)e->values[0].number,
                                             .ws_row = (unsigned short)e->values[1].number};
                        (void)ioctl(g->master, TIOCSWINSZ, &ws);
                }
                return 1;
        }
        return 0;
}

// The last event in the status bar: its source, its name, its first value.
static void showEvent(Gallery *g, const bmds_gui_event_t *e) {
        const char *name = bmds_gui_atom_name(g->gui, e->name);
        if (name == NULL) return;
        char text[96];
        int  n = snprintf(text, sizeof(text), "Event: %s from %u", name, (unsigned)(e->source & 0xffffffu));
        if (e->count > 0 && n > 0 && (size_t)n < sizeof(text)) {
                const bmds_gui_value_t *v = &e->values[0];
                if (v->tag == BMDS_GUI_STRING)
                        snprintf(text + n, sizeof(text) - (size_t)n, ": %.*s", (int)v->length, v->text);
                else
                        snprintf(text + n, sizeof(text) - (size_t)n, ": %lld", (long long)v->number);
        }
        bmds_status_bar_set_field(g->gui, g->status, 0, text);
        return;
}

static void closeBox(Gallery *g) {
        bmds_gui_destroy(g->gui, g->dialog);
        g->dialog = g->dialogOk = g->dialogNo = 0;
        return;
}

int main(void) {
        Gallery g;
        memset(&g, 0, sizeof(g));
        g.gui = bmds_gui_connect(NULL);
        if (g.gui == NULL) {
                fprintf(stderr, "guigallery: no GUI (%s)\n", strerror(errno));
                return 1;
        }
        bmds_gui_t *gui = g.gui;
        g.window = bmds_window_new(gui, 0);
        bmds_window_set_title(gui, g.window, "GUI gallery");
        bmds_window_set_icon(gui, g.window, "applications-system");
        bmds_window_set_min_width(gui, g.window, 280);
        bmds_window_set_min_height(gui, g.window, 120);
        bmds_window_set_window_menu_item(gui, g.window, "About GUI gallery");
        const uint32_t closeRequested = bmds_gui_atom(gui, BMDS_GUI_WINDOW_CLOSE_REQUESTED);
        bmds_gui_listen(gui, g.window, &closeRequested, 1, 1);

        // A tool bar, the tabs (stretching), a status bar.
        const uint32_t tools = bmds_tool_bar_new(gui, g.window);
        g.toolAbout = bmds_tool_button_new(gui, tools);
        bmds_tool_button_set_icon(gui, g.toolAbout, "help-about");
        bmds_tool_bar_add_separator(gui, tools);
        g.toolQuit = bmds_tool_button_new(gui, tools);
        bmds_tool_button_set_text(gui, g.toolQuit, "Quit");
        g.tabs = bmds_tabs_new(gui, g.window);
        g.status = bmds_status_bar_new(gui, g.window);
        bmds_status_bar_add_field(gui, g.status, 0, NULL);

        // Windows
        const uint32_t windows = bmds_tab_page_new(gui, g.tabs);
        bmds_box_set_margin(gui, windows, 8);
        bmds_tab_page_set_title(gui, windows, "Windows");
        g.size = bmds_label_new(gui, windows);
        bmds_label_set_text(gui, g.size, "Size: (not shown yet)");
        const uint32_t ops = bmds_gui_new(gui, "Box", windows);
        bmds_box_set_direction(gui, ops, "across");
        g.minimize = bmds_button_new(gui, ops);
        bmds_button_set_text(gui, g.minimize, "Mi&nimize");
        g.maximize = bmds_button_new(gui, ops);
        bmds_button_set_text(gui, g.maximize, "Ma&ximize");
        g.busy = bmds_button_new(gui, ops);
        bmds_button_set_text(gui, g.busy, "&Busy pointer");
        const uint32_t row = bmds_gui_new(gui, "Box", windows);
        bmds_box_set_direction(gui, row, "across");
        g.about = bmds_button_new(gui, row);
        bmds_button_set_text(gui, g.about, "&About...");
        g.quit = bmds_button_new(gui, row);
        bmds_button_set_text(gui, g.quit, "&Quit");

        // Controls: a form
        const uint32_t controls = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, controls, "Controls");
        const uint32_t form = bmds_form_new(gui, controls);
        bmds_widget_set_stretch(gui, form, 1);
        const uint32_t section = bmds_form_section_new(gui, form);
        bmds_form_section_set_title(gui, section, "Controls");
        uint32_t r = bmds_form_row_new(gui, form);
        bmds_form_row_set_label(gui, r, "Check box:");
        g.check = bmds_check_box_new(gui, r);
        bmds_check_box_set_text(gui, g.check, "&Ticked");
        r = bmds_form_row_new(gui, form);
        bmds_form_row_set_label(gui, r, "Number field:");
        bmds_form_row_set_help(gui, r, "From -10 to 10, in twos.");
        g.number = bmds_number_field_new(gui, r);
        bmds_number_field_set_minimum(gui, g.number, -10);
        bmds_number_field_set_maximum(gui, g.number, 10);
        bmds_number_field_set_step(gui, g.number, 2);
        r = bmds_form_row_new(gui, form);
        bmds_form_row_set_label(gui, r, "Slider:");
        g.slider = bmds_slider_new(gui, r);
        bmds_widget_set_stretch(gui, g.slider, 1);
        bmds_slider_set_value(gui, g.slider, 30);
        r = bmds_form_row_new(gui, form);
        bmds_form_row_set_label(gui, r, "Progress bar:");
        g.progress = bmds_progress_bar_new(gui, r);
        bmds_widget_set_stretch(gui, g.progress, 1);
        bmds_progress_bar_set_value(gui, g.progress, 30);
        r = bmds_form_row_new(gui, form);
        bmds_form_row_set_label(gui, r, "Combo box:");
        g.combo = bmds_combo_box_new(gui, r);
        static const char *const fruit[] = {"Apple", "Banana", "Cherry"};
        bmds_combo_box_set_items(gui, g.combo, fruit, 3);
        bmds_combo_box_add_item(gui, g.combo, "Damson");

        // Layouts: a splitter, a list and icons its sides
        const uint32_t layouts = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, layouts, "Layouts");
        const uint32_t split = bmds_splitter_new(gui, layouts);
        bmds_splitter_set_min_first(gui, split, 60);
        bmds_splitter_set_min_second(gui, split, 60);
        const uint32_t           list = bmds_list_view_new(gui, split);
        static const char *const planets[] = {"Mercury", "Venus", "Earth", "Mars", "Jupiter", "Saturn"};
        bmds_item_view_set_items(gui, list, planets, 6);
        bmds_item_view_set_current(gui, list, 2);
        const uint32_t           icons = bmds_icon_view_new(gui, split);
        static const char *const things[][2] = {{"Folder", "folder"},
                                                {"Music", "folder-music"},
                                                {"Pictures", "folder-pictures"},
                                                {"Disk", "drive-harddisk"},
                                                {"Text", "text-x-generic"}};
        for (int i = 0; i < 5; ++i) {
                bmds_item_view_add_item(gui, icons, things[i][0]);
                bmds_item_view_set_icon(gui, icons, i, things[i][1]);
        }
        bmds_splitter_set_position(gui, split, 140);

        // Views: a tree of folders beside a details view of the chosen one's entries
        const uint32_t views = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, views, "Views");
        const uint32_t viewSplit = bmds_splitter_new(gui, views);
        bmds_splitter_set_min_first(gui, viewSplit, 80);
        bmds_splitter_set_min_second(gui, viewSplit, 120);
        g.tree = bmds_tree_view_new(gui, viewSplit);
        bmds_tree_view_add_root(gui, g.tree, "/rom");
        g.files = bmds_details_view_new(gui, viewSplit);
        static const char *const columns[] = {"Name", "Size"};
        bmds_details_view_set_columns(gui, g.files, columns, 2);
        bmds_splitter_set_position(gui, viewSplit, 140);

        // Text: a code editor, and its cursor and state
        const uint32_t textPage = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, textPage, "Text");
        g.code = bmds_code_edit_new(gui, textPage);
        bmds_code_edit_set_syntax(gui, g.code, "c");
        bmds_code_edit_set_line_numbers(gui, g.code, 1);
        bmds_text_edit_set_text(gui, g.code,
                                "#include <stdio.h>\n\n"
                                "/* Edit me: the line below shows the cursor. */\n"
                                "int main(void) {\n"
                                "        printf(\"Hello from the gallery\\n\");\n"
                                "        return 0;\n"
                                "}\n");
        const uint32_t cursorMoved = bmds_gui_atom(gui, BMDS_GUI_TEXT_EDIT_CURSOR_MOVED);
        bmds_gui_listen(gui, g.code, &cursorMoved, 1, 1);
        g.codeState = bmds_label_new(gui, textPage);
        bmds_label_set_text(gui, g.codeState, "Line 1, byte 1");

        // Terminal: a shell on our own pty
        const uint32_t termPage = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, termPage, "Terminal");
        g.term = bmds_terminal_new(gui, termPage);
        g.master = -1;

        bmds_window_set_width(gui, g.window, 440);
        bmds_window_set_height(gui, g.window, 300);
        bmds_window_show(gui, g.window);

        startShell(&g);

        // The GUI's events and the shell's output, whichever comes.
        int done = 0;
        while (!done) {
                bmds_gui_event_t e;
                const int        r = bmds_gui_next_event(gui, &e, 0);
                if (r < 0) break;
                if (r == 0) {
                        if (bmds_gui_flush(gui) != 0) break;
                        struct pollfd fds[2] = {{.fd = bmds_gui_fd(gui), .events = POLLIN},
                                                {.fd = g.master, .events = POLLIN}};
                        if (poll(fds, g.master >= 0 ? 2 : 1, -1) < 0 && errno != EINTR) break;
                        if (g.master >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) shellOutput(&g);
                        continue;
                }
                if (pageEvent(&g, &e)) continue;
                showEvent(&g, &e);
                if (e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_RESIZED) && e.count == 2) {
                        char text[48];
                        snprintf(text, sizeof(text), "Size: %d x %d", (int)e.values[0].number, (int)e.values[1].number);
                        bmds_label_set_text(gui, g.size, text);
                } else if (e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSED)) {
                        done = 1;
                } else if ((e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSE_REQUESTED)) ||
                           ((e.source == g.quit || e.source == g.toolQuit) &&
                            bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED))) {
                        if (g.dialog == 0) openBox(&g, "Quit?", "Quit the gallery?", 1);
                } else if (((e.source == g.about || e.source == g.toolAbout) &&
                            bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) ||
                           (e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_WINDOW_MENU_CHOSEN))) {
                        if (g.dialog == 0) openBox(&g, "About", "The GUI service's classes, a part at a time.", 0);
                } else if (e.source == g.slider && bmds_gui_event_is(gui, &e, BMDS_GUI_SLIDER_CHANGED)) {
                        bmds_progress_bar_set_value(gui, g.progress, e.values[0].number);
                } else if (e.source == g.minimize && bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) {
                        bmds_window_minimize(gui, g.window);
                } else if (e.source == g.maximize && bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) {
                        bmds_gui_value_t maximized;
                        if (bmds_window_get_maximized(gui, g.window, &maximized) == 0 && maximized.number != 0)
                                bmds_window_restore(gui, g.window);
                        else
                                bmds_window_maximize(gui, g.window);
                } else if (e.source == g.busy && bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) {
                        g.busyOn = !g.busyOn;
                        bmds_window_set_pointer(gui, g.window, g.busyOn ? "starting" : "arrow");
                } else if (g.dialog != 0 && e.source == g.dialogOk &&
                           bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) {
                        done = g.asking;
                        closeBox(&g);
                } else if (g.dialog != 0 &&
                           ((e.source == g.dialogNo && bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) ||
                            (e.source == g.dialog && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSED)))) {
                        closeBox(&g);
                }
        }
        bmds_gui_value_t placement;
        if (bmds_window_get_placement(gui, g.window, &placement) == 0)
                printf("guigallery: placement %.*s\n", (int)placement.length, placement.text);
        if (g.master >= 0) {
                close(g.master); // the shell's terminal hangs up
                (void)waitpid(g.shell, NULL, 0);
        }
        bmds_gui_close(gui);
        return 0;
}
