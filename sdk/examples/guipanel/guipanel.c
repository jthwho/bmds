/**
 * @file      guipanel.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A panel written as an ordinary program (devplan/guiprograms.md §12 4d), through the SHELL and
 * SERVICES extensions only: the test that a panel from a card can do what the system's does. A
 * strip at the screen edge the system's panel isn't on (or -e's), its work area reserved:
 *   - Start: a menu of the programs in a folder (-d; the panel's own by default), each started as
 *     a session of its own with the starting pointer shown; Ctrl+Escape opens it too.
 *   - A button for each application window: its icon and title; a click raises it, or minimizes
 *     it if it has the focus; a right click opens its window menu.
 *   - With -t, the tray, when no other program is (the system's panel without its tray item):
 *     programs' tray icons (bmds/tray.h), told of clicks and the wheel; an icon goes with its
 *     program.
 *   - A clock.
 *
 * The program must be named by the setting gui/shell-clients.
 *
 *     guipanel [-e top|bottom] [-d FOLDER] [-t] [-v]
 *
 * -v prints each event as it comes.
 */

#include <bmds/guiclasses.h>
#include <bmds/tray.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <bmds/sys.h>

#define PanelHeight 28
#define MaxPrograms 32
#define MaxTasks 32
#define MaxIcons 16
#define TitleChars 24

static int verbose; // -v: each event, and the tray's messages, printed

typedef struct Task {
                int32_t  wm;     // the window manager's number
                uint32_t button; // its button, and the image its icon is copied into
                uint32_t image;
                int      focused;
} Task;

typedef struct TrayIcon {
                char     reply[32]; // its program's reply service
                uint32_t id;        // its number in its program
                uint32_t button, image;
} TrayIcon;

typedef struct Panel {
                bmds_gui_t *gui;
                uint32_t    window, start, menu, tasks, tray, clock, timer, shell, services;
                int         y; // the strip's top on the screen
                int         programCount;
                char        programs[MaxPrograms][256];
                uint32_t    items[MaxPrograms];
                int         taskCount;
                Task        task[MaxTasks];
                int         iconCount;
                TrayIcon    icon[MaxIcons];
                int         hosting; // the tray is ours
} Panel;

static int byName(const void *a, const void *b) {
        return strcmp((const char *)a, (const char *)b);
}

// The programs in @p folder (`.elf`, not the debug copies, not the panel), sorted.
static void readPrograms(Panel *p, const char *folder, const char *self) {
        DIR *d = opendir(folder);
        if (d == NULL) {
                perror(folder);
                return;
        }
        struct dirent *e;
        while ((e = readdir(d)) != NULL && p->programCount < MaxPrograms) {
                const size_t n = strlen(e->d_name);
                if (n < 5 || strcmp(e->d_name + n - 4, ".elf") != 0) continue;
                if ((n > 10 && strcmp(e->d_name + n - 10, ".debug.elf") == 0) || strcmp(e->d_name, self) == 0) continue;
                snprintf(p->programs[p->programCount++], sizeof(p->programs[0]), "%s/%s", folder, e->d_name);
        }
        closedir(d);
        qsort(p->programs, (size_t)p->programCount, sizeof(p->programs[0]), byName);
        return;
}

// Program @p path started as a session of its own (so the starting pointer knows its windows).
static void launch(Panel *p, const char *path) {
        const pid_t pid = vfork();
        if (pid == 0) {
                setsid();
                char *argv[] = {(char *)path, NULL};
                execv(path, argv);
                _exit(127);
        }
        if (pid < 0) {
                perror("guipanel: start");
                return;
        }
        bmds_shell_program_starting(p->gui, p->shell, pid);
        return;
}

// The start menu opened under (or over) the Start button.
static void openStart(Panel *p) {
        bmds_gui_value_t g;
        if (bmds_widget_get_geometry(p->gui, p->start, &g) != 0) return;
        bmds_menu_popup(p->gui, p->menu, g.x, p->y == 0 ? PanelHeight : 0);
        return;
}

static int hasName(bmds_gui_t *gui, const bmds_gui_value_t *list, const char *name) {
        (void)gui;
        for (uint32_t i = 0; i < list->count; ++i) {
                bmds_gui_value_t item;
                if (bmds_gui_list_item(list, i, &item) == 0 && item.length == strlen(name) &&
                    memcmp(item.text, name, item.length) == 0)
                        return 1;
        }
        return 0;
}

static void listenContext(bmds_gui_t *gui, uint32_t widget) {
        const uint32_t atom = bmds_gui_atom(gui, BMDS_GUI_WIDGET_CONTEXT_MENU);
        bmds_gui_listen(gui, widget, &atom, 1, 1);
        return;
}

// ---- Window buttons

static Task *taskOf(Panel *p, int32_t wm) {
        for (int i = 0; i < p->taskCount; ++i)
                if (p->task[i].wm == wm) return &p->task[i];
        return NULL;
}

static void windowChanged(Panel *p, const bmds_gui_event_t *e) {
        bmds_gui_t *gui = p->gui;
        Task       *t = taskOf(p, (int32_t)e->values[0].number);
        if (t == NULL) {
                if (p->taskCount == MaxTasks) return;
                t = &p->task[p->taskCount++];
                t->wm = (int32_t)e->values[0].number;
                t->button = bmds_button_new(gui, p->tasks);
                t->image = bmds_gui_new(gui, "Image", 0);
                listenContext(gui, t->button);
        }
        char title[4 * TitleChars + 4];
        snprintf(title, sizeof(title), "%.*s", (int)e->values[1].length, e->values[1].text);
        // Cut at TitleChars characters (UTF-8: lead bytes counted).
        int chars = 0;
        for (char *c = title; *c != '\0'; ++c)
                if (((unsigned char)*c & 0xc0) != 0x80 && ++chars > TitleChars) {
                        strcpy(c, "...");
                        break;
                }
        bmds_button_set_text(gui, t->button, title);
        t->focused = hasName(gui, &e->values[2], "focused");
        if (hasName(gui, &e->values[2], "icon")) {
                bmds_gui_value_t copied;
                if (bmds_shell_copy_icon(gui, p->shell, t->wm, t->image, &copied) == 0 && copied.number)
                        bmds_button_set_icon_image(gui, t->button, t->image); // the new picture
        } else {
                bmds_button_set_icon_image(gui, t->button, 0);
        }
        return;
}

static void windowClosed(Panel *p, int32_t wm) {
        Task *t = taskOf(p, wm);
        if (t == NULL) return;
        bmds_gui_destroy(p->gui, t->button);
        bmds_gui_destroy(p->gui, t->image);
        *t = p->task[--p->taskCount];
        return;
}

// ---- The tray (bmds/tray.h), when it is ours

static TrayIcon *iconOf(Panel *p, const char *reply, uint32_t id) {
        for (int i = 0; i < p->iconCount; ++i)
                if (p->icon[i].id == id && strncmp(p->icon[i].reply, reply, sizeof(p->icon[i].reply)) == 0)
                        return &p->icon[i];
        return NULL;
}

static void removeIcon(Panel *p, TrayIcon *t) {
        bmds_gui_destroy(p->gui, t->button);
        bmds_gui_destroy(p->gui, t->image);
        *t = p->icon[--p->iconCount];
        return;
}

// A program's icon told something by the tray: @p kind, @p reason (or the wheel's @p notches), its
// place on the screen.
static void tellIcon(Panel *p, const TrayIcon *t, uint32_t kind, uint32_t reason, int32_t notches) {
        bmds_tray_event_t e;
        memset(&e, 0, sizeof(e));
        e.size = sizeof(e);
        e.kind = kind;
        e.id = t->id;
        e.reason = reason;
        e.notches = notches;
        bmds_gui_value_t g;
        if (bmds_widget_get_geometry(p->gui, t->button, &g) == 0) {
                e.x = (int16_t)g.x;
                e.y = (int16_t)(g.y + p->y);
                e.width = (uint16_t)g.width;
                e.height = (uint16_t)g.height;
        }
        bmds_services_send(p->gui, p->services, t->reply, &e, sizeof(e));
        return;
}

static void trayMessage(Panel *p, const uint8_t *bytes, uint32_t length) {
        bmds_gui_t        *gui = p->gui;
        bmds_tray_pixels_t m; // the largest: what was sent, the rest zero
        memset(&m, 0, sizeof(m));
        uint32_t size = 0;
        if (length < sizeof(bmds_tray_header_t)) return;
        memcpy(&size, bytes, sizeof(size));
        if (size < sizeof(bmds_tray_header_t) || size > length) return;
        memcpy(&m, bytes, size < sizeof(m) ? size : sizeof(m));
        char reply[33];
        snprintf(reply, sizeof(reply), "%.*s", (int)sizeof(m.reply), m.reply);
        TrayIcon *t = iconOf(p, reply, m.id);
        if (verbose) {
                printf("guipanel: tray: kind %u, icon %u of %s, %u bytes\n", (unsigned)m.kind, (unsigned)m.id, reply,
                       (unsigned)size);
                fflush(stdout);
        }
        if (m.kind == BMDS_TRAY_REMOVE) {
                if (t != NULL) removeIcon(p, t);
                return;
        }
        if (t == NULL) {
                if (m.kind != BMDS_TRAY_UPDATE || p->iconCount == MaxIcons) return;
                t = &p->icon[p->iconCount++];
                snprintf(t->reply, sizeof(t->reply), "%s", reply);
                t->id = m.id;
                t->button = bmds_tool_button_new(gui, p->tray);
                t->image = bmds_gui_new(gui, "Image", 0);
                listenContext(gui, t->button);
                const uint32_t wheel = bmds_gui_atom(gui, BMDS_GUI_WIDGET_WHEEL);
                bmds_gui_listen(gui, t->button, &wheel, 1, 1);
                bmds_services_watch(gui, p->services, reply); // it goes with its program
        }
        if (m.kind == BMDS_TRAY_UPDATE) {
                bmds_tray_update_t u;
                memset(&u, 0, sizeof(u));
                memcpy(&u, bytes, size < sizeof(u) ? size : sizeof(u));
                char icon[65];
                snprintf(icon, sizeof(icon), "%.*s", (int)sizeof(u.icon), u.icon);
                if (verbose) printf("guipanel: tray: flags %#x, icon '%s'\n", (unsigned)u.flags, icon);
                if ((u.flags & BMDS_TRAY_HAS_PIXELS) == 0) bmds_tool_button_set_icon(gui, t->button, icon);
                bmds_widget_set_visible(gui, t->button, (u.flags & BMDS_TRAY_VISIBLE) != 0);
        } else if (m.kind == BMDS_TRAY_PIXELS && m.width == BMDS_TRAY_ICON_SIDE && m.height == BMDS_TRAY_ICON_SIDE) {
                bmds_gui_put_image(gui, t->image, m.width, m.height, m.argb);
                bmds_tool_button_set_icon_image(gui, t->button, t->image);
        }
        return;
}

// A program's reply service gone: its icons with it.
static void replyGone(Panel *p, const char *reply) {
        for (int i = p->iconCount - 1; i >= 0; --i)
                if (strncmp(p->icon[i].reply, reply, sizeof(p->icon[i].reply)) == 0) removeIcon(p, &p->icon[i]);
        bmds_services_unwatch(p->gui, p->services, reply);
        return;
}

// ---- The clock

static void tick(Panel *p) {
        const time_t now = time(NULL);
        char         text[16];
        strftime(text, sizeof(text), "%H:%M", localtime(&now));
        bmds_label_set_text(p->gui, p->clock, text);
        while (waitpid(-1, NULL, WNOHANG) > 0) continue; // the programs we started, ended
        return;
}

// ---- Events

static void event(Panel *p, const bmds_gui_event_t *e) {
        bmds_gui_t *gui = p->gui;
        char        name[33];
        if (e->source == p->start && bmds_gui_event_is(gui, e, BMDS_GUI_BUTTON_CLICKED)) {
                openStart(p);
        } else if (e->source == p->shell) {
                if (bmds_gui_event_is(gui, e, BMDS_GUI_SHELL_WINDOW_CHANGED) && e->count == 3)
                        windowChanged(p, e);
                else if (bmds_gui_event_is(gui, e, BMDS_GUI_SHELL_WINDOW_CLOSED) && e->count == 1)
                        windowClosed(p, (int32_t)e->values[0].number);
                else if (bmds_gui_event_is(gui, e, BMDS_GUI_SHELL_KEY_GRABBED))
                        openStart(p);
        } else if (e->source == p->timer) {
                tick(p);
        } else if (e->source == p->services && e->count == 2) {
                snprintf(name, sizeof(name), "%.*s", (int)e->values[0].length, e->values[0].text);
                if (bmds_gui_event_is(gui, e, BMDS_GUI_SERVICES_MESSAGE) && strcmp(name, BMDS_TRAY_SERVICE) == 0)
                        trayMessage(p, (const uint8_t *)e->values[1].text, e->values[1].length);
                else if (bmds_gui_event_is(gui, e, BMDS_GUI_SERVICES_CHANGED) && !e->values[1].number)
                        replyGone(p, name);
        } else {
                for (int i = 0; i < p->programCount; ++i)
                        if (e->source == p->items[i]) {
                                launch(p, p->programs[i]);
                                return;
                        }
                for (int i = 0; i < p->taskCount; ++i) {
                        Task *t = &p->task[i];
                        if (e->source != t->button) continue;
                        if (bmds_gui_event_is(gui, e, BMDS_GUI_BUTTON_CLICKED)) {
                                bmds_shell_act(gui, p->shell, t->wm, t->focused ? "minimize" : "raise");
                        } else if (bmds_gui_event_is(gui, e, BMDS_GUI_WIDGET_CONTEXT_MENU) && e->count == 2) {
                                // The panel's window is at the screen's left edge, frameless.
                                bmds_shell_show_window_menu(gui, p->shell, t->wm, (int32_t)e->values[0].number,
                                                            (int32_t)e->values[1].number + p->y);
                        }
                        return;
                }
                for (int i = 0; i < p->iconCount; ++i) {
                        if (e->source != p->icon[i].button) continue;
                        if (bmds_gui_event_is(gui, e, BMDS_GUI_TOOL_BUTTON_CLICKED))
                                tellIcon(p, &p->icon[i], BMDS_TRAY_ACTIVATED, BMDS_TRAY_TRIGGER, 0);
                        else if (bmds_gui_event_is(gui, e, BMDS_GUI_WIDGET_CONTEXT_MENU))
                                tellIcon(p, &p->icon[i], BMDS_TRAY_ACTIVATED, BMDS_TRAY_CONTEXT, 0);
                        else if (bmds_gui_event_is(gui, e, BMDS_GUI_WIDGET_WHEEL) && e->count == 1) // up: positive
                                tellIcon(p, &p->icon[i], BMDS_TRAY_SCROLLED, 0, (int32_t)e->values[0].number);
                        return;
                }
        }
        return;
}

// The edge the system's panel isn't on (panel/edge).
static int otherEdgeIsTop(void) {
        char          value[16] = "";
        const int32_t id = bmds_sysconf_find("panel/edge");
        if (id >= 0) bmds_sysconf_get(id, BMDS_SYSCONF_FIELD_VALUE, value, sizeof(value));
        return strcmp(value, "top") != 0;
}

int main(int argc, char **argv) {
        static Panel p;
        const char  *folder = NULL;
        int          top = -1, tray = 0, opt;
        while ((opt = getopt(argc, argv, "e:d:tv")) != -1) {
                if (opt == 'e' && (strcmp(optarg, "top") == 0 || strcmp(optarg, "bottom") == 0)) {
                        top = strcmp(optarg, "top") == 0;
                } else if (opt == 'd') {
                        folder = optarg;
                } else if (opt == 't') {
                        tray = 1;
                } else if (opt == 'v') {
                        verbose = 1;
                } else {
                        fprintf(stderr, "usage: guipanel [-e top|bottom] [-d FOLDER] [-t] [-v]\n");
                        return 2;
                }
        }
        p.gui = bmds_gui_connect(NULL);
        if (p.gui == NULL) {
                perror("guipanel: no GUI");
                return 1;
        }
        bmds_gui_t *gui = p.gui;
        if (!bmds_gui_has_extension(gui, "SHELL")) {
                fprintf(stderr, "guipanel: not a shell here: name %s in gui/shell-clients\n", argv[0]);
                return 1;
        }
        // The programs: the folder's, or the panel's own folder's.
        char        own[256];
        const char *slash = strrchr(argv[0], '/');
        snprintf(own, sizeof(own), "%.*s", slash != NULL ? (int)(slash - argv[0]) : 1, slash != NULL ? argv[0] : ".");
        readPrograms(&p, folder != NULL ? folder : own, slash != NULL ? slash + 1 : argv[0]);

        bmds_gui_value_t screen;
        if (bmds_gui_display(gui, "screen", &screen) != 0) return 1;
        if (top < 0) top = otherEdgeIsTop();
        p.y = top ? 0 : (int)screen.height - PanelHeight;

        p.shell = bmds_shell_new(gui, 0);
        bmds_shell_grab_key(gui, p.shell, 0x29, 0x01); // Ctrl+Escape: the start menu
        p.window = bmds_window_new(gui, 0);
        bmds_window_set_title(gui, p.window, "guipanel");
        bmds_window_set_level(gui, p.window, "panel");
        bmds_window_set_reserve(gui, p.window, PanelHeight);
        bmds_window_set_frame(gui, p.window, 0);
        bmds_window_set_title_bar(gui, p.window, 0);
        char where[48];
        snprintf(where, sizeof(where), "0 %d %u %d", p.y, (unsigned)screen.width, PanelHeight);
        bmds_window_set_placement(gui, p.window, where);
        const uint32_t row = bmds_gui_new(gui, "Box", p.window);
        bmds_box_set_direction(gui, row, "across");
        bmds_box_set_margin(gui, row, 2);
        bmds_box_set_spacing(gui, row, 4);
        p.start = bmds_button_new(gui, row);
        bmds_button_set_text(gui, p.start, "Start");
        p.menu = bmds_menu_new(gui, p.window);
        for (int i = 0; i < p.programCount; ++i) {
                const char *base = strrchr(p.programs[i], '/');
                char        label[64];
                snprintf(label, sizeof(label), "%s", base != NULL ? base + 1 : p.programs[i]);
                char *dot = strrchr(label, '.');
                if (dot != NULL) *dot = '\0';
                p.items[i] = bmds_menu_item_new(gui, p.menu);
                bmds_menu_item_set_text(gui, p.items[i], label);
        }
        p.tasks = bmds_gui_new(gui, "Box", row);
        bmds_box_set_direction(gui, p.tasks, "across");
        bmds_box_set_spacing(gui, p.tasks, 4);
        bmds_widget_set_stretch(gui, p.tasks, 1);
        p.tray = bmds_gui_new(gui, "ToolBar", row); // tool buttons live in tool bars
        p.clock = bmds_label_new(gui, row);
        p.timer = bmds_timer_new(gui, 0);
        bmds_timer_set_interval(gui, p.timer, 1000);
        bmds_timer_start(gui, p.timer);
        tick(&p);

        if (tray) {
                p.services = bmds_services_new(gui, 0);
                bmds_gui_value_t taken;
                if (bmds_services_has(gui, p.services, BMDS_TRAY_SERVICE, &taken) == 0 && taken.number) {
                        fprintf(stderr, "guipanel: another program is the tray (the system's panel's tray item?)\n");
                } else {
                        bmds_services_register(gui, p.services, BMDS_TRAY_SERVICE);
                        p.hosting = 1;
                }
        }
        bmds_window_show(gui, p.window);

        bmds_gui_event_t e;
        int              r;
        while ((r = bmds_gui_next_event(gui, &e, -1)) == 1) {
                if (verbose) {
                        const char *name = bmds_gui_atom_name(gui, e.name);
                        printf("guipanel: %s from %u (%u values)\n", name != NULL ? name : "?", (unsigned)e.source,
                               (unsigned)e.count);
                        fflush(stdout);
                }
                event(&p, &e);
        }
        bmds_gui_close(gui);
        return r < 0 ? 1 : 0;
}
