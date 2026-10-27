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
 * Menus: File (Open and Save As through the file dialog, Quit), View (the status bar, a checked
 * item; the pointer, a submenu), Help (About), their shortcuts as keys. Dialogs: a message box, the colour dialog, a popup
 * menu, and a timer counting seconds. Images: a diamond the program draws, sent once (PutImage) and
 * shown on a tool button, a list's rows and in the icon view. Paint: a canvas the program draws
 * with the mouse, its picture kept here and only the changed rectangle sent (PutPixels); the
 * colour dialog's colour is the brush's. Clipboard: a field's text copied, or offered and answered
 * only when another program asks (as plain text, or as HTML in bold), and pasted back; what
 * happens is shown below (CLIPBOARD); a label below takes drops of files or text (`dropTypes`).
 * Desktop: a notification with two buttons, and a tray icon with a menu; what became of them shows
 * below. One gallery runs at a time: a second start asks the first, on the service bus (SERVICES),
 * to come forward, and ends.
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
                uint32_t    open, saveAs, quitItem, statusItem, aboutItem, fileDialog, saving;
                uint32_t    ask, answer, colorButton, colorDialog, popupButton, popup, popupItems[3];
                uint32_t    fontButton, fontDialog, fontChooser;
                uint32_t    timer, timerButton, ticks;
                uint32_t    canvas, clearButton;
                uint32_t    clipboard, clipEdit, clipCopy, clipOffer, clipPaste, clipState, dropTarget;
                uint32_t    notifyButton, note, trayCheck, tray, trayShow, trayNotify, deskState;
                uint32_t    services; // our name on the service bus: one gallery at a time
                uint16_t   *picture; // the canvas's, as drawn here (RGB565)
                int         pictureWidth, pictureHeight, lastX, lastY, drawing;
                uint16_t    brush;
                int         statusShown, seconds;
                uint32_t    arrowItem, busyItem, nameEdit;
                char        folder[256]; // the details view's
                int         sortColumn, sortDown;
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
// A folder's entry, as the details view shows it.
typedef struct Entry {
                char      name[64];
                long long size; // -1: a folder
                int       folder;
} Entry;

static int sortBy, sortDown; // qsort's comparison has no argument of its own

static int compareEntries(const void *a, const void *b) {
        const Entry *x = (const Entry *)a, *y = (const Entry *)b;
        int          c = sortBy == 1 ? (x->size > y->size) - (x->size < y->size) : strcmp(x->name, y->name);
        if (c == 0) c = strcmp(x->name, y->name);
        return sortDown ? -c : c;
}

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
                // Every entry, sorted by the column the header asked for, then shown.
                static Entry   entries[200];
                size_t         n = 0;
                struct dirent *d;
                while (dir != NULL && n < 200 && (d = readdir(dir)) != NULL) {
                        if (d->d_name[0] == '.') continue;
                        char        full[512];
                        struct stat st;
                        snprintf(full, sizeof(full), "%s/%s", path, d->d_name);
                        Entry *en = &entries[n++];
                        snprintf(en->name, sizeof(en->name), "%s", d->d_name);
                        en->folder = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
                        en->size = en->folder ? -1 : (long long)st.st_size;
                }
                sortBy = g->sortColumn;
                sortDown = g->sortDown;
                qsort(entries, n, sizeof(Entry), compareEntries);
                bmds_item_view_clear(g->gui, g->files);
                for (size_t i = 0; i < n; ++i) {
                        char size[24];
                        if (entries[i].folder)
                                snprintf(size, sizeof(size), "folder");
                        else
                                snprintf(size, sizeof(size), "%lld", entries[i].size);
                        const char *texts[] = {entries[i].name, size};
                        bmds_details_view_add_row(g->gui, g->files, texts, 2);
                        bmds_item_view_set_icon(g->gui, g->files, (int32_t)i,
                                                entries[i].folder ? "folder" : "text-x-generic");
                }
                bmds_details_view_set_sort_indicator(g->gui, g->files, g->sortColumn, g->sortDown);
                snprintf(g->folder, sizeof(g->folder), "%s", path);
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

// ---- Paint: the canvas's picture kept here, the changed part sent

static uint16_t rgb565(uint32_t rgb) {
        return (uint16_t)(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) | ((rgb >> 3) & 0x001f));
}

// A new size: a white picture, sent whole (run-length coded, it costs little).
static void paintResized(Gallery *g, int width, int height) {
        free(g->picture);
        g->picture = width > 0 && height > 0 ? (uint16_t *)malloc((size_t)width * height * 2) : NULL;
        g->pictureWidth = g->picture != NULL ? width : 0;
        g->pictureHeight = g->picture != NULL ? height : 0;
        if (g->picture == NULL) return;
        for (size_t i = 0; i < (size_t)width * height; ++i) g->picture[i] = 0xffff;
        bmds_gui_put_pixels(g->gui, g->canvas, 0, 0, (uint32_t)width, (uint32_t)height, g->picture, (uint32_t)width);
        return;
}

// A line of round dots from the last point to (@p x, @p y); the rectangle it touched sent.
static void paintTo(Gallery *g, int x, int y) {
        const int r = 3;
        int       left = g->lastX < x ? g->lastX : x, right = g->lastX > x ? g->lastX : x;
        int       top = g->lastY < y ? g->lastY : y, bottom = g->lastY > y ? g->lastY : y;
        const int steps = (right - left > bottom - top ? right - left : bottom - top) + 1;
        for (int i = 0; i <= steps; ++i) {
                const int cx = g->lastX + (x - g->lastX) * i / steps, cy = g->lastY + (y - g->lastY) * i / steps;
                for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx) {
                                const int px = cx + dx, py = cy + dy;
                                if (dx * dx + dy * dy > r * r || px < 0 || py < 0 || px >= g->pictureWidth ||
                                    py >= g->pictureHeight)
                                        continue;
                                g->picture[(size_t)py * g->pictureWidth + px] = g->brush;
                        }
        }
        g->lastX = x;
        g->lastY = y;
        left -= r;
        top -= r;
        right += r;
        bottom += r;
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right >= g->pictureWidth) right = g->pictureWidth - 1;
        if (bottom >= g->pictureHeight) bottom = g->pictureHeight - 1;
        if (right < left || bottom < top) return;
        bmds_gui_put_pixels(g->gui, g->canvas, left, top, (uint32_t)(right - left + 1), (uint32_t)(bottom - top + 1),
                            g->picture + (size_t)top * g->pictureWidth + left, (uint32_t)g->pictureWidth);
        return;
}

// The views', text's, terminal's and paint's events. @return 1 if it was one of theirs.
// The Clipboard tab's events: its buttons, and the clipboard's requests, losses and pastes.
static int clipboardEvent(Gallery *g, const bmds_gui_event_t *e) {
        bmds_gui_t *gui = g->gui;
        char        text[160];
        if (e->source == g->clipCopy || e->source == g->clipOffer) {
                bmds_gui_value_t v;
                if (bmds_line_edit_get_text(gui, g->clipEdit, &v) != 0) return 1;
                snprintf(text, sizeof(text), "%.*s", (int)v.length, v.text);
                if (e->source == g->clipCopy) {
                        bmds_clipboard_copy_text(gui, g->clipboard, "clipboard", text);
                        bmds_label_set_text(gui, g->clipState, "Copied.");
                } else {
                        static const char *const types[] = {"text/html", "text"};
                        bmds_clipboard_offer(gui, g->clipboard, "clipboard", types, 2);
                        bmds_label_set_text(gui, g->clipState, "Offered: answered when pasted elsewhere.");
                }
                return 1;
        }
        if (e->source == g->dropTarget && bmds_gui_event_is(gui, e, BMDS_GUI_WIDGET_DROPPED) && e->count == 4) {
                snprintf(text, sizeof(text), "Dropped at %d, %d as %.*s: %.*s", (int)e->values[2].number,
                         (int)e->values[3].number, (int)e->values[0].length, e->values[0].text,
                         (int)e->values[1].length, e->values[1].text);
                bmds_label_set_text(gui, g->clipState, text);
                return 1;
        }
        if (e->source == g->clipPaste) {
                bmds_clipboard_paste(gui, g->clipboard, "clipboard", "text");
                return 1;
        }
        if (e->source != g->clipboard) return 0;
        if (bmds_gui_event_is(gui, e, BMDS_GUI_CLIPBOARD_REQUESTED) && e->count == 3) {
                // Answered from the field as it is now.
                bmds_gui_value_t v;
                if (bmds_line_edit_get_text(gui, g->clipEdit, &v) != 0) v.length = 0;
                const int     html = e->values[2].length >= 9 && memcmp(e->values[2].text, "text/html", 9) == 0;
                const int     n = snprintf(text, sizeof(text), html ? "<b>%.*s</b>" : "%.*s", (int)v.length, v.text);
                const int32_t request = (int32_t)e->values[0].number;
                bmds_clipboard_answer(gui, g->clipboard, request, text, (uint32_t)(n < (int)sizeof(text) ? n : 0));
                bmds_clipboard_finish(gui, g->clipboard, request);
                snprintf(text, sizeof(text), "Asked for %.*s: answered.", (int)e->values[2].length, e->values[2].text);
                bmds_label_set_text(gui, g->clipState, text);
        } else if (bmds_gui_event_is(gui, e, BMDS_GUI_CLIPBOARD_LOST)) {
                bmds_label_set_text(gui, g->clipState, "Another program copied over ours.");
        } else if (bmds_gui_event_is(gui, e, BMDS_GUI_CLIPBOARD_PASTED) && e->count == 3) {
                snprintf(text, sizeof(text), "Pasted: %.*s", (int)e->values[2].length, e->values[2].text);
                bmds_label_set_text(gui, g->clipState, text);
        } else if (bmds_gui_event_is(gui, e, BMDS_GUI_CLIPBOARD_PASTE_FAILED) && e->count == 3) {
                const char *why = bmds_gui_atom_name(gui, (uint32_t)e->values[2].number);
                snprintf(text, sizeof(text), "Nothing pasted (%s).", why != NULL ? why : "?");
                bmds_label_set_text(gui, g->clipState, text);
        }
        return 1;
}

// The Desktop tab's events: a notification shown, the tray icon shown or hidden, and what became
// of each.
static int desktopEvent(Gallery *g, const bmds_gui_event_t *e) {
        bmds_gui_t *gui = g->gui;
        char        text[96];
        if ((e->source == g->notifyButton && bmds_gui_event_is(gui, e, BMDS_GUI_BUTTON_CLICKED)) ||
            (e->source == g->trayNotify && bmds_gui_event_is(gui, e, BMDS_GUI_MENU_ITEM_CHOSEN))) {
                bmds_notification_show(gui, g->note); // again: in place of the last
                bmds_label_set_text(gui, g->deskState, "Notification shown.");
                return 1;
        }
        if (e->source == g->trayCheck && bmds_gui_event_is(gui, e, BMDS_GUI_CHECK_BOX_TOGGLED) && e->count == 1) {
                bmds_tray_icon_set_visible(gui, g->tray, (int)e->values[0].number);
                return 1;
        }
        if (e->source == g->trayShow && bmds_gui_event_is(gui, e, BMDS_GUI_MENU_ITEM_CHOSEN)) {
                bmds_window_raise(gui, g->window);
                return 1;
        }
        if (e->source == g->note) {
                if (bmds_gui_event_is(gui, e, BMDS_GUI_NOTIFICATION_ACTIVATED) && e->count == 1)
                        snprintf(text, sizeof(text), "Notification clicked (%s).",
                                 e->values[0].number == 0   ? "its body"
                                 : e->values[0].number == 1 ? "Raise"
                                                            : "Dismiss");
                else
                        snprintf(text, sizeof(text), "Notification %s.",
                                 bmds_gui_event_is(gui, e, BMDS_GUI_NOTIFICATION_DISMISSED) ? "dismissed" : "expired");
                if (bmds_gui_event_is(gui, e, BMDS_GUI_NOTIFICATION_ACTIVATED) && e->values[0].number == 1)
                        bmds_window_raise(gui, g->window);
                bmds_label_set_text(gui, g->deskState, text);
                return 1;
        }
        if (e->source != g->tray) return 0;
        if (bmds_gui_event_is(gui, e, BMDS_GUI_TRAY_ICON_ACTIVATED) && e->count == 1) {
                const char *why = bmds_gui_atom_name(gui, (uint32_t)e->values[0].number);
                snprintf(text, sizeof(text), "Tray icon: %s.", why != NULL ? why : "?");
        } else if (bmds_gui_event_is(gui, e, BMDS_GUI_TRAY_ICON_SCROLLED) && e->count == 1) {
                snprintf(text, sizeof(text), "Tray icon: the wheel, %d.", (int)e->values[0].number);
        } else if (bmds_gui_event_is(gui, e, BMDS_GUI_TRAY_ICON_AVAILABILITY_CHANGED) && e->count == 1) {
                snprintf(text, sizeof(text), "The tray %s.", e->values[0].number ? "came" : "went");
        } else {
                return 1;
        }
        bmds_label_set_text(gui, g->deskState, text);
        return 1;
}

// The gallery's name on the service bus.
static const char GalleryService[] = "org.bmds.guigallery";

static int pageEvent(Gallery *g, const bmds_gui_event_t *e) {
        bmds_gui_t *gui = g->gui;
        if (clipboardEvent(g, e)) return 1;
        if (desktopEvent(g, e)) return 1;
        if (e->source == g->services && bmds_gui_event_is(gui, e, BMDS_GUI_SERVICES_MESSAGE) && e->count == 2) {
                // Another gallery started: we come forward instead.
                if (e->values[1].length == 5 && memcmp(e->values[1].text, "raise", 5) == 0) {
                        bmds_window_restore(gui, g->window);
                        bmds_window_raise(gui, g->window);
                        bmds_label_set_text(gui, g->deskState, "Another start asked us to come forward.");
                }
                return 1;
        }
        if (e->source == g->files && bmds_gui_event_is(gui, e, BMDS_GUI_DETAILS_VIEW_SORT_ASKED) && e->count == 1) {
                // The same column again turns the order round.
                const int column = (int)e->values[0].number;
                g->sortDown = column == g->sortColumn ? !g->sortDown : 0;
                g->sortColumn = column;
                if (g->folder[0] != '\0') readFolder(g, g->folder, 0);
                return 1;
        }
        if (e->source == g->arrowItem || e->source == g->busyItem) {
                const int busy = e->source == g->busyItem;
                bmds_window_set_pointer(gui, g->window, busy ? "starting" : "arrow");
                bmds_menu_item_set_checked(gui, g->arrowItem, !busy);
                bmds_menu_item_set_checked(gui, g->busyItem, busy);
                return 1;
        }
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
        if (bmds_gui_event_is(gui, e, BMDS_GUI_MENU_ITEM_CHOSEN) && (e->source == g->open || e->source == g->saveAs)) {
                g->saving = e->source == g->saveAs;
                bmds_file_dialog_set_mode(gui, g->fileDialog, g->saving ? "save" : "open");
                bmds_file_dialog_set_title(gui, g->fileDialog, g->saving ? "Save as" : "Open");
                bmds_file_dialog_set_file_name(gui, g->fileDialog, g->saving ? "notes.txt" : "");
                bmds_file_dialog_open(gui, g->fileDialog);
                return 1;
        }
        if (e->source == g->fileDialog && e->count == 2) {
                char             text[160];
                bmds_gui_value_t path = {0};
                if (e->values[0].number != 0 && bmds_gui_list_item(&e->values[1], 0, &path) == 0)
                        snprintf(text, sizeof(text), "%s: %.*s", g->saving ? "Save as" : "Open", (int)path.length,
                                 path.text);
                else
                        snprintf(text, sizeof(text), "%s: cancelled", g->saving ? "Save as" : "Open");
                bmds_status_bar_set_field(gui, g->status, 0, text);
                return 1;
        }
        if (e->source == g->statusItem) {
                g->statusShown = !g->statusShown;
                bmds_menu_item_set_checked(gui, g->statusItem, g->statusShown);
                bmds_widget_set_visible(gui, g->status, g->statusShown);
                return 1;
        }
        if (e->source == g->ask) {
                const uint32_t box = bmds_gui_new(gui, "MessageBox", 0);
                bmds_message_box_set_kind(gui, box, "question");
                bmds_message_box_set_title(gui, box, "A question");
                bmds_message_box_set_text(gui, box, "Do you like the gallery?");
                bmds_message_box_set_buttons(gui, box, "yesNoCancel");
                bmds_message_box_open(gui, box);
                return 1;
        }
        if (bmds_gui_event_is(gui, e, BMDS_GUI_MESSAGE_BOX_ANSWERED) && e->count == 1) {
                char text[64];
                snprintf(text, sizeof(text), "The answer: %s", bmds_gui_atom_name(gui, (uint32_t)e->values[0].number));
                bmds_label_set_text(gui, g->answer, text);
                bmds_gui_destroy(gui, e->source); // answered: done with it
                return 1;
        }
        if (e->source == g->colorButton) {
                bmds_color_dialog_open(gui, g->colorDialog);
                return 1;
        }
        if (e->source == g->fontButton) {
                bmds_font_dialog_open(gui, g->fontDialog);
                return 1;
        }
        // A font chosen (the dialog's, or the font button's): the answer written in it.
        if ((e->source == g->fontDialog && bmds_gui_event_is(gui, e, BMDS_GUI_FONT_DIALOG_FINISHED) && e->count == 2 &&
             e->values[0].number != 0) ||
            (e->source == g->fontChooser && bmds_gui_event_is(gui, e, BMDS_GUI_FONT_BUTTON_CHANGED) && e->count == 1)) {
                const bmds_gui_value_t *v = &e->values[e->count - 1];
                char                    name[96], text[128];
                snprintf(name, sizeof(name), "%.*s", (int)v->length, v->text);
                snprintf(text, sizeof(text), "The font: %s", name);
                bmds_label_set_text(gui, g->answer, text);
                bmds_widget_set_font(gui, g->answer, name);
                return 1;
        }
        if (e->source == g->canvas) {
                if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_RESIZED) && e->count == 2) {
                        paintResized(g, (int)e->values[0].number, (int)e->values[1].number);
                } else if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_PRESSED) && e->count == 3 && g->picture != NULL) {
                        g->drawing = 1;
                        g->lastX = (int)e->values[0].number;
                        g->lastY = (int)e->values[1].number;
                        paintTo(g, g->lastX, g->lastY);
                } else if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_MOVED) && e->count == 3 && g->drawing) {
                        paintTo(g, (int)e->values[0].number, (int)e->values[1].number);
                } else if (bmds_gui_event_is(gui, e, BMDS_GUI_CANVAS_RELEASED)) {
                        g->drawing = 0;
                }
                return 1;
        }
        if (e->source == g->clearButton) {
                paintResized(g, g->pictureWidth, g->pictureHeight);
                return 1;
        }
        if (e->source == g->colorDialog && e->count == 2) {
                char text[64];
                snprintf(text, sizeof(text),
                         e->values[0].number != 0 ? "The colour: #%06x" : "Colour: cancelled (#%06x)",
                         (unsigned)e->values[1].number);
                bmds_label_set_text(gui, g->answer, text);
                if (e->values[0].number != 0) g->brush = rgb565((uint32_t)e->values[1].number); // the brush's
                return 1;
        }
        if (e->source == g->popupButton) {
                bmds_menu_popup(gui, g->popup, 120, 160);
                return 1;
        }
        for (int i = 0; i < 3; ++i) {
                if (e->source != g->popupItems[i]) continue;
                bmds_gui_value_t text;
                if (bmds_menu_item_get_text(gui, e->source, &text) == 0) {
                        char shown[64];
                        snprintf(shown, sizeof(shown), "Popup: %.*s", (int)text.length, text.text);
                        bmds_label_set_text(gui, g->answer, shown);
                }
                return 1;
        }
        if (e->source == g->timerButton) {
                bmds_gui_value_t active;
                if (bmds_timer_get_active(gui, g->timer, &active) == 0 && active.number != 0) {
                        bmds_timer_stop(gui, g->timer);
                        bmds_button_set_text(gui, g->timerButton, "Start the &timer");
                } else {
                        bmds_timer_start(gui, g->timer);
                        bmds_button_set_text(gui, g->timerButton, "Stop the &timer");
                }
                return 1;
        }
        if (e->source == g->timer) {
                char text[32];
                snprintf(text, sizeof(text), "%d s", ++g->seconds);
                bmds_label_set_text(gui, g->ticks, text);
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

// A diamond @p side pixels square in @p argb (ARGB8888): an image the program draws itself.
static uint32_t diamondImage(bmds_gui_t *gui, int side, uint32_t *argb) {
        const int half = side / 2;
        for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x) {
                        const int d = abs(x - half + (x >= half)) + abs(y - half + (y >= half));
                        const int edge = d == half - 1;
                        argb[y * side + x] = d >= half ? 0x00000000u
                                             : edge    ? 0xff802000u
                                                       : 0xffffa020u - (uint32_t)(d * 0x0400);
                }
        const uint32_t image = bmds_gui_new(gui, "Image", 0);
        bmds_gui_put_image(gui, image, (uint32_t)side, (uint32_t)side, argb);
        return image;
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
        // One gallery at a time (SERVICES): a second asks the first to come forward, and ends.
        g.services = bmds_services_new(gui, 0);
        bmds_gui_value_t running;
        if (bmds_services_has(gui, g.services, GalleryService, &running) == 0 && running.number) {
                bmds_services_send(gui, g.services, GalleryService, "raise", 5);
                bmds_gui_close(gui);
                printf("guigallery: running already: shown\n");
                return 0;
        }
        bmds_services_register(gui, g.services, GalleryService);
        g.window = bmds_window_new(gui, 0);
        bmds_window_set_title(gui, g.window, "GUI gallery");
        bmds_window_set_icon(gui, g.window, "applications-system");
        bmds_window_set_min_width(gui, g.window, 280);
        bmds_window_set_min_height(gui, g.window, 120);
        bmds_window_set_window_menu_item(gui, g.window, "About GUI gallery");
        const uint32_t closeRequested = bmds_gui_atom(gui, BMDS_GUI_WINDOW_CLOSE_REQUESTED);
        bmds_gui_listen(gui, g.window, &closeRequested, 1, 1);

        // A tool bar, the tabs (stretching), a status bar.
        // The menu bar first: the window's top edge
        const uint32_t bar = bmds_menu_bar_new(gui, g.window);
        const uint32_t file = bmds_menu_new(gui, bar);
        bmds_menu_set_title(gui, file, "&File");
        g.open = bmds_menu_item_new(gui, file);
        bmds_menu_item_set_text(gui, g.open, "&Open...");
        bmds_menu_item_set_shortcut(gui, g.open, "Ctrl+O");
        bmds_menu_item_set_icon(gui, g.open, "document-open");
        g.saveAs = bmds_menu_item_new(gui, file);
        bmds_menu_item_set_text(gui, g.saveAs, "Save &As...");
        bmds_menu_item_set_icon(gui, g.saveAs, "document-save-as");
        bmds_menu_add_separator(gui, file);
        g.quitItem = bmds_menu_item_new(gui, file);
        bmds_menu_item_set_text(gui, g.quitItem, "&Quit");
        bmds_menu_item_set_shortcut(gui, g.quitItem, "Ctrl+Q");
        const uint32_t view = bmds_menu_new(gui, bar);
        bmds_menu_set_title(gui, view, "&View");
        g.statusItem = bmds_menu_item_new(gui, view);
        bmds_menu_item_set_text(gui, g.statusItem, "&Status bar");
        bmds_menu_item_set_checked(gui, g.statusItem, 1);
        g.statusShown = 1;
        const uint32_t pointer = bmds_menu_new(gui, view); // a submenu
        bmds_menu_set_title(gui, pointer, "&Pointer");
        g.arrowItem = bmds_menu_item_new(gui, pointer);
        bmds_menu_item_set_text(gui, g.arrowItem, "&Arrow");
        bmds_menu_item_set_checked(gui, g.arrowItem, 1);
        g.busyItem = bmds_menu_item_new(gui, pointer);
        bmds_menu_item_set_text(gui, g.busyItem, "&Busy");
        const uint32_t help = bmds_menu_new(gui, bar);
        bmds_menu_set_title(gui, help, "&Help");
        g.aboutItem = bmds_menu_item_new(gui, help);
        bmds_menu_item_set_text(gui, g.aboutItem, "&About GUI gallery");
        bmds_menu_item_set_shortcut(gui, g.aboutItem, "F1");
        g.fileDialog = bmds_gui_new(gui, "FileDialog", 0);
        bmds_file_dialog_add_filter(gui, g.fileDialog, "Text files", "*.txt *.md");
        bmds_file_dialog_add_filter(gui, g.fileDialog, "All files", "*");

        const uint32_t tools = bmds_tool_bar_new(gui, g.window);
        g.toolAbout = bmds_tool_button_new(gui, tools);
        bmds_tool_button_set_icon(gui, g.toolAbout, "help-about");
        static uint32_t small[16 * 16], large[32 * 32];
        const uint32_t  diamond = diamondImage(gui, 16, small), bigDiamond = diamondImage(gui, 32, large);
        bmds_tool_button_set_icon_image(gui, bmds_tool_button_new(gui, tools), diamond);
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
        bmds_gui_new(gui, "Spacer", row); // Quit at the right
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
        r = bmds_form_row_new(gui, form);
        bmds_form_row_set_label(gui, r, "Line edit:");
        bmds_form_row_set_help(gui, r, "Enter shows it in the status bar.");
        g.nameEdit = bmds_line_edit_new(gui, r);
        bmds_widget_set_stretch(gui, g.nameEdit, 1);
        bmds_line_edit_set_max_length(gui, g.nameEdit, 40);

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
        bmds_item_view_set_icon_image(gui, list, 2, diamond);
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
        bmds_item_view_add_item(gui, icons, "Diamond");
        bmds_item_view_set_icon_image(gui, icons, 5, bigDiamond);
        bmds_splitter_set_position(gui, split, 140);

        // Dialogs: a message box, the colour and font dialogs, a font button, a popup menu, a timer
        const uint32_t dialogs = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, dialogs, "Dialogs");
        bmds_box_set_margin(gui, dialogs, 8);
        const uint32_t row1 = bmds_gui_new(gui, "Box", dialogs);
        bmds_box_set_direction(gui, row1, "across");
        g.ask = bmds_button_new(gui, row1);
        bmds_button_set_text(gui, g.ask, "&Message box...");
        g.colorButton = bmds_button_new(gui, row1);
        bmds_button_set_text(gui, g.colorButton, "&Colour...");
        g.popupButton = bmds_button_new(gui, row1);
        bmds_button_set_text(gui, g.popupButton, "&Popup menu");
        g.fontButton = bmds_button_new(gui, row1);
        bmds_button_set_text(gui, g.fontButton, "&Font...");
        g.fontChooser = bmds_font_button_new(gui, row1); // its name drawn in it; a click opens the dialog
        bmds_widget_set_font(gui, g.fontChooser, "IBM Plex Serif 16");
        g.answer = bmds_label_new(gui, dialogs);
        bmds_label_set_text(gui, g.answer, "No answer yet");
        const uint32_t row2 = bmds_gui_new(gui, "Box", dialogs);
        bmds_box_set_direction(gui, row2, "across");
        g.timerButton = bmds_button_new(gui, row2);
        bmds_button_set_text(gui, g.timerButton, "Start the &timer");
        g.ticks = bmds_label_new(gui, row2);
        bmds_label_set_text(gui, g.ticks, "0 s");
        g.timer = bmds_gui_new(gui, "Timer", 0);
        bmds_timer_set_interval(gui, g.timer, 1000);
        g.colorDialog = bmds_gui_new(gui, "ColorDialog", 0);
        bmds_color_dialog_set_title(gui, g.colorDialog, "Pick a colour");
        bmds_color_dialog_set_color(gui, g.colorDialog, 0x3a6ea5);
        g.fontDialog = bmds_font_dialog_new(gui, 0);
        bmds_font_dialog_set_title(gui, g.fontDialog, "Pick a font");
        bmds_font_dialog_set_font(gui, g.fontDialog, "ui");
        g.popup = bmds_menu_new(gui, g.window);
        static const char *const choices[] = {"&Apple", "&Banana", "&Cherry"};
        for (int i = 0; i < 3; ++i) {
                g.popupItems[i] = bmds_menu_item_new(gui, g.popup);
                bmds_menu_item_set_text(gui, g.popupItems[i], choices[i]);
        }

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
        const uint32_t textSplit = bmds_splitter_new(gui, textPage);
        bmds_splitter_set_direction(gui, textSplit, "down");
        bmds_splitter_set_min_first(gui, textSplit, 60);
        bmds_splitter_set_min_second(gui, textSplit, 40);
        g.code = bmds_code_edit_new(gui, textSplit);
        bmds_code_edit_set_syntax(gui, g.code, "c");
        bmds_code_edit_set_line_numbers(gui, g.code, 1);
        bmds_text_edit_set_text(gui, g.code,
                                "#include <stdio.h>\n\n"
                                "/* Edit me: the line below shows the cursor. */\n"
                                "int main(void) {\n"
                                "        printf(\"Hello from the gallery\\n\");\n"
                                "        return 0;\n"
                                "}\n");
        const uint32_t notes = bmds_text_edit_new(gui, textSplit); // plain text, no colours
        bmds_text_edit_set_text(gui, notes, "Notes: a plain text editor below the code editor.\n");
        bmds_splitter_set_position(gui, textSplit, 200);
        const uint32_t cursorMoved = bmds_gui_atom(gui, BMDS_GUI_TEXT_EDIT_CURSOR_MOVED);
        bmds_gui_listen(gui, g.code, &cursorMoved, 1, 1);
        g.codeState = bmds_label_new(gui, textPage);
        bmds_label_set_text(gui, g.codeState, "Line 1, byte 1");

        // Paint: a canvas drawn with the mouse
        const uint32_t paint = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, paint, "Paint");
        g.canvas = bmds_canvas_new(gui, paint);
        const uint32_t moved = bmds_gui_atom(gui, BMDS_GUI_CANVAS_MOVED);
        bmds_gui_listen(gui, g.canvas, &moved, 1, 1);
        g.clearButton = bmds_button_new(gui, paint);
        bmds_button_set_text(gui, g.clearButton, "C&lear (the brush: Dialogs, Colour...)");
        g.brush = rgb565(0x3a6ea5);

        // Clipboard: a field copied, offered, and pasted back
        const uint32_t clipPage = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, clipPage, "Clipboard");
        g.clipboard = bmds_clipboard_new(gui, 0);
        g.clipEdit = bmds_line_edit_new(gui, clipPage);
        bmds_line_edit_set_text(gui, g.clipEdit, "Text from the gallery");
        const uint32_t clipRow = bmds_gui_new(gui, "Box", clipPage);
        bmds_box_set_direction(gui, clipRow, "across");
        g.clipCopy = bmds_button_new(gui, clipRow);
        bmds_button_set_text(gui, g.clipCopy, "&Copy");
        g.clipOffer = bmds_button_new(gui, clipRow);
        bmds_button_set_text(gui, g.clipOffer, "&Offer");
        g.clipPaste = bmds_button_new(gui, clipRow);
        bmds_button_set_text(gui, g.clipPaste, "&Paste");
        g.clipState = bmds_label_new(gui, clipPage);
        bmds_label_set_text(gui, g.clipState, "Copy stores the text now; Offer answers when asked.");
        g.dropTarget = bmds_label_new(gui, clipPage);
        bmds_label_set_text(gui, g.dropTarget, "[ Drop a file or text here ]");
        static const char *const dropTypes[] = {"text/uri-list", "text"};
        bmds_widget_set_drop_types(gui, g.dropTarget, dropTypes, 2);
        bmds_gui_new(gui, "Spacer", clipPage);

        // Desktop: a notification and a tray icon, shown by the panel
        const uint32_t deskPage = bmds_tab_page_new(gui, g.tabs);
        bmds_tab_page_set_title(gui, deskPage, "Desktop");
        g.note = bmds_notification_new(gui, 0);
        bmds_notification_set_title(gui, g.note, "Widget Gallery");
        bmds_notification_set_body(gui, g.note, "A notification from a program, with two buttons.");
        bmds_notification_set_icon(gui, g.note, "dialog-information");
        static const char *const noteActions[] = {"Raise", "Dismiss"};
        bmds_notification_set_actions(gui, g.note, noteActions, 2);
        const uint32_t deskRow = bmds_gui_new(gui, "Box", deskPage);
        bmds_box_set_direction(gui, deskRow, "across");
        g.notifyButton = bmds_button_new(gui, deskRow);
        bmds_button_set_text(gui, g.notifyButton, "&Notify");
        g.tray = bmds_tray_icon_new(gui, 0);
        bmds_tray_icon_set_icon(gui, g.tray, "dialog-information");
        bmds_tray_icon_set_tool_tip(gui, g.tray, "Widget Gallery's tray icon");
        const uint32_t trayMenu = bmds_menu_new(gui, g.tray);
        g.trayShow = bmds_menu_item_new(gui, trayMenu);
        bmds_menu_item_set_text(gui, g.trayShow, "&Show the gallery");
        g.trayNotify = bmds_menu_item_new(gui, trayMenu);
        bmds_menu_item_set_text(gui, g.trayNotify, "&Notify");
        g.trayCheck = bmds_check_box_new(gui, deskPage);
        bmds_check_box_set_text(gui, g.trayCheck, "A &tray icon (right click: its menu)");
        g.deskState = bmds_label_new(gui, deskPage);
        bmds_label_set_text(gui, g.deskState, "What became of them shows here.");
        bmds_gui_new(gui, "Spacer", deskPage);

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
                            bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) ||
                           e.source == g.quitItem) {
                        if (g.dialog == 0) openBox(&g, "Quit?", "Quit the gallery?", 1);
                } else if (((e.source == g.about || e.source == g.toolAbout) &&
                            bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) ||
                           (e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_WINDOW_MENU_CHOSEN)) ||
                           e.source == g.aboutItem) {
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
