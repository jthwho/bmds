/**
 * @file      guidemo.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * guidemo: a window from the GUI service (bmds/gui.h, devplan/guiprograms.md §8): a label, a
 * field, two buttons. Enter in the field or "Say hello" greets whoever is named there; "Quit" or
 * closing the window ends it.
 */

#include <bmds/gui.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

enum {
        Window,
        Box,
        Label,
        Button,
        LineEdit,
        Title,
        Text,
        Direction,
        Across,
        Show,
        SetFocus,
        Clicked,
        Accepted,
        Closed,
        Names
};

static const char *const names[Names] = {"Window",    "Box",    "Label", "Button",   "LineEdit", "title",    "text",
                                         "direction", "across", "show",  "setFocus", "clicked",  "accepted", "closed"};

int main(void) {
        bmds_gui_t *gui = bmds_gui_connect(NULL);
        if (gui == NULL) {
                fprintf(stderr, "guidemo: no GUI (%s)\n", strerror(errno));
                return 1;
        }
        uint32_t a[Names];
        if (bmds_gui_atoms(gui, names, Names, a) != 0) {
                fprintf(stderr, "guidemo: %s\n", strerror(errno));
                return 1;
        }
        const uint32_t window = bmds_gui_new_id(gui), label = bmds_gui_new_id(gui), field = bmds_gui_new_id(gui);
        const uint32_t row = bmds_gui_new_id(gui), hello = bmds_gui_new_id(gui), quit = bmds_gui_new_id(gui);

        // One batch: nothing waits until the first event.
        const bmds_gui_prop_t title = {a[Title], bmds_gui_string("GUI demo")};
        bmds_gui_create(gui, window, a[Window], 0, &title, 1);
        const bmds_gui_prop_t prompt = {a[Text], bmds_gui_string("What's your name?")};
        bmds_gui_create(gui, label, a[Label], window, &prompt, 1);
        bmds_gui_create(gui, field, a[LineEdit], window, NULL, 0);
        const bmds_gui_prop_t across = {a[Direction], bmds_gui_atom_value(a[Across])};
        bmds_gui_create(gui, row, a[Box], window, &across, 1);
        const bmds_gui_prop_t hi = {a[Text], bmds_gui_string("&Say hello")};
        bmds_gui_create(gui, hello, a[Button], row, &hi, 1);
        const bmds_gui_prop_t bye = {a[Text], bmds_gui_string("&Quit")};
        bmds_gui_create(gui, quit, a[Button], row, &bye, 1);
        bmds_gui_call(gui, window, a[Show], NULL, 0, NULL);
        bmds_gui_call(gui, field, a[SetFocus], NULL, 0, NULL);

        for (;;) {
                bmds_gui_event_t e;
                const int        r = bmds_gui_next_event(gui, &e, -1);
                if (r < 0) break;
                if ((e.name == a[Clicked] && e.source == quit) || (e.name == a[Closed] && e.source == window)) break;
                if ((e.name == a[Clicked] && e.source == hello) || (e.name == a[Accepted] && e.source == field)) {
                        bmds_gui_value_t name;
                        if (bmds_gui_get(gui, field, &a[Text], 1, &name) != 0) break;
                        char greeting[96];
                        snprintf(greeting, sizeof(greeting), "Hello, %.*s!", (int)name.length,
                                 name.length != 0 ? name.text : "stranger");
                        const bmds_gui_prop_t text = {a[Text], bmds_gui_string(greeting)};
                        bmds_gui_set(gui, label, &text, 1);
                        printf("%s\n", greeting);
                }
        }
        bmds_gui_close(gui);
        return 0;
}
