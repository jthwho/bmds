/**
 * @file      guigallery.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * guigallery: the GUI service's classes, a part at a time (devplan/guiprograms.md §10), through
 * the generated wrappers (bmds/guiclasses.h). Windows: its size shown as it changes, a smallest
 * size, a busy pointer, an About box (owned and modal, also on the window menu), and a close that
 * asks first.
 */

#include <bmds/guiclasses.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct Gallery {
                bmds_gui_t *gui;
                uint32_t    window, size, busy, minimize, maximize, about, quit;
                uint32_t    dialog, dialogOk, dialogNo; // the open About or Quit box (0: none)
                int         asking;                     // the box is "Quit?"
                int         busyOn;
} Gallery;

// A box owned by the main window and modal over it: About (one button) or Quit? (two).
static void openBox(Gallery *g, const char *title, const char *text, int asking) {
        bmds_gui_t *gui = g->gui;
        g->dialog = bmds_window_new(gui, 0);
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

        g.size = bmds_label_new(gui, g.window);
        bmds_label_set_text(gui, g.size, "Size: (not shown yet)");
        const uint32_t ops = bmds_gui_new(gui, "Box", g.window);
        bmds_box_set_direction(gui, ops, "across");
        g.minimize = bmds_button_new(gui, ops);
        bmds_button_set_text(gui, g.minimize, "Mi&nimize");
        g.maximize = bmds_button_new(gui, ops);
        bmds_button_set_text(gui, g.maximize, "Ma&ximize");
        g.busy = bmds_button_new(gui, ops);
        bmds_button_set_text(gui, g.busy, "&Busy pointer");
        const uint32_t row = bmds_gui_new(gui, "Box", g.window);
        bmds_box_set_direction(gui, row, "across");
        g.about = bmds_button_new(gui, row);
        bmds_button_set_text(gui, g.about, "&About...");
        g.quit = bmds_button_new(gui, row);
        bmds_button_set_text(gui, g.quit, "&Quit");
        bmds_window_show(gui, g.window);

        int done = 0;
        while (!done) {
                bmds_gui_event_t e;
                if (bmds_gui_next_event(gui, &e, -1) < 0) break;
                if (e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_RESIZED) && e.count == 2) {
                        char text[48];
                        snprintf(text, sizeof(text), "Size: %d x %d", (int)e.values[0].number, (int)e.values[1].number);
                        bmds_label_set_text(gui, g.size, text);
                } else if (e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSED)) {
                        done = 1;
                } else if ((e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSE_REQUESTED)) ||
                           (e.source == g.quit && bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED))) {
                        if (g.dialog == 0) openBox(&g, "Quit?", "Quit the gallery?", 1);
                } else if ((e.source == g.about && bmds_gui_event_is(gui, &e, BMDS_GUI_BUTTON_CLICKED)) ||
                           (e.source == g.window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_WINDOW_MENU_CHOSEN))) {
                        if (g.dialog == 0) openBox(&g, "About", "The GUI service's classes, a part at a time.", 0);
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
        bmds_gui_close(gui);
        return 0;
}
