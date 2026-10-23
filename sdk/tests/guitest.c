/**
 * @file      guitest.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * guitest: the GUI service's board test (devplan/guiprograms.md §10, 2f). Run on the board with a
 * display; it opens one window for a moment. Checks Describe for every class, the error each bad
 * request gets, events from methods (a button's click), what a client's own change doesn't tell
 * back, containers letting go of children (tabs, a splitter, a menu, a menu bar), a one-shot
 * timer, images (shown by widgets and outliving their objects while shown), a canvas's pixels,
 * and many objects made and destroyed. Prints "guitest: ok", or each failure and exits 1.
 */

#include <bmds/guiclasses.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bmds/sys.h>

static int failures;

#define CHECK(cond)                                                                                                    \
        do {                                                                                                           \
                if (!(cond)) {                                                                                         \
                        printf("guitest: %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
                        ++failures;                                                                                    \
                }                                                                                                      \
        } while (0)

// The errors the service reported, counted; the last one kept.
static uint32_t errorCount, lastCode, lastOpcode;

static void onError(void *arg, const bmds_gui_error_t *e) {
        (void)arg;
        ++errorCount;
        lastCode = e->code;
        lastOpcode = e->opcode;
        return;
}

// Every error so far has come: none since the last look, or exactly one of @p code.
static void expectErrors(bmds_gui_t *gui, uint32_t code, int line) {
        bmds_gui_sync(gui);
        const int ok = code == 0 ? errorCount == 0 : errorCount == 1 && lastCode == code;
        if (!ok) {
                printf("guitest: line %d: wanted %s, got %u error(s), the last %s (opcode %u)\n", line,
                       code == 0 ? "no error" : bmds_gui_error_name(code), (unsigned)errorCount,
                       errorCount != 0 ? bmds_gui_error_name(lastCode) : "-", (unsigned)lastOpcode);
                ++failures;
        }
        errorCount = 0;
        return;
}
#define EXPECT(code) expectErrors(gui, code, __LINE__)
#define EXPECT_NONE() expectErrors(gui, 0, __LINE__)

static uint32_t u32At(const uint8_t *p) {
        return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// Class @p name's description: its parent, whether it can be made, and whether it has entry
// @p entry (a property, method or event, by @p section: 0, 1, 2) of tag @p tag (the property's;
// the method's result; the event's first value; ~0: any). @return 0, or -1 (no such class).
static int describe(bmds_gui_t *gui, const char *name, uint32_t *parent, uint32_t *creatable, int section,
                    const char *entry, uint32_t tag, int *found) {
        const uint8_t *b = NULL;
        uint32_t       n = 0;
        if (bmds_gui_describe(gui, bmds_gui_atom(gui, name), &b, &n) != 0) return -1;
        const uint32_t want = entry != NULL ? bmds_gui_atom(gui, entry) : 0;
        uint32_t       at = 0;
        *found = 0;
#define NEXT() (at + 4 <= n ? (at += 4, u32At(b + at - 4)) : (at = n + 1, 0u))
        *parent = NEXT();
        *creatable = NEXT();
        for (int s = 0; s < 3 && at <= n; ++s) {
                const uint32_t count = NEXT();
                for (uint32_t i = 0; i < count && at <= n; ++i) {
                        const uint32_t atom = NEXT();
                        uint32_t       first = NEXT(); // a property's tag; a method's result; an event's default
                        if (s == 0) {
                                (void)NEXT(); // flags
                        } else {
                                const uint32_t values = NEXT();
                                uint32_t       firstValue = ~0u;
                                for (uint32_t v = 0; v < values; ++v) {
                                        const uint32_t t = NEXT();
                                        if (v == 0) firstValue = t;
                                }
                                if (s == 2) first = firstValue;
                        }
                        if (s == section && atom == want && (tag == ~0u || first == tag)) *found = 1;
                }
        }
#undef NEXT
        return at <= n ? 0 : -1;
}

// The window the tests run in: its own events (resized when shown, activated) are skipped.
static uint32_t testWindow;

// The next event within @p ms that isn't the window's own, in @p out. @return 1, or 0 (none).
static int nextEvent(bmds_gui_t *gui, bmds_gui_event_t *out, int ms) {
        for (;;) {
                if (bmds_gui_next_event(gui, out, ms) != 1) return 0;
                if (out->source != testWindow) return 1;
        }
}

// The next event within @p ms, if it is @p name from @p source. @return 1, or 0.
static int nextEventIs(bmds_gui_t *gui, uint32_t source, const char *name, int ms) {
        bmds_gui_event_t e;
        if (nextEvent(gui, &e, ms) != 1) return 0;
        return e.source == source && bmds_gui_event_is(gui, &e, name);
}

static void describeAll(bmds_gui_t *gui) {
        // Every class, and whether it can be made (the bases can't).
        static const struct {
                        const char *name;
                        int         creatable;
        } classes[] = {
                {"Widget", 0},      {"Window", 1},   {"Box", 1},         {"Label", 1},       {"Button", 1},
                {"LineEdit", 1},    {"CheckBox", 1}, {"NumberField", 1}, {"Slider", 1},      {"ComboBox", 1},
                {"ProgressBar", 1}, {"Spacer", 1},   {"Form", 1},        {"FormSection", 1}, {"FormRow", 1},
                {"Splitter", 1},    {"Tabs", 1},     {"TabPage", 1},     {"ToolBar", 1},     {"ToolButton", 1},
                {"StatusBar", 1},   {"ItemView", 0}, {"ListView", 1},    {"DetailsView", 1}, {"IconView", 1},
                {"TreeView", 1},    {"TextEdit", 1}, {"CodeEdit", 1},    {"Terminal", 1},    {"MenuBar", 1},
                {"Menu", 1},        {"MenuItem", 1}, {"MessageBox", 1},  {"FileDialog", 1},  {"ColorDialog", 1},
                {"Timer", 1},       {"Image", 1},    {"Canvas", 1},
        };
        uint32_t parent = 0, creatable = 0;
        int      found = 0;
        for (size_t i = 0; i < sizeof(classes) / sizeof(classes[0]); ++i) {
                const int r = describe(gui, classes[i].name, &parent, &creatable, 0, NULL, 0, &found);
                if (r != 0 || creatable != (uint32_t)classes[i].creatable)
                        printf("guitest: Describe %s: %s\n", classes[i].name, r != 0 ? "failed" : "creatable wrong");
                CHECK(r == 0 && creatable == (uint32_t)classes[i].creatable);
        }
        // Entries: a property's tag, a method, an event's value; the parents.
        CHECK(describe(gui, "Window", &parent, &creatable, 0, "title", BMDS_GUI_STRING, &found) == 0 && found);
        CHECK(parent == 0);
        CHECK(describe(gui, "Window", &parent, &creatable, 1, "show", ~0u, &found) == 0 && found);
        CHECK(describe(gui, "Window", &parent, &creatable, 2, "resized", BMDS_GUI_INT, &found) == 0 && found);
        CHECK(describe(gui, "TabPage", &parent, &creatable, 0, "title", BMDS_GUI_STRING, &found) == 0 && found);
        CHECK(parent == bmds_gui_atom(gui, "Box"));
        CHECK(describe(gui, "LineEdit", &parent, &creatable, 2, "accepted", BMDS_GUI_STRING, &found) == 0 && found);
        CHECK(parent == bmds_gui_atom(gui, "Widget"));
        CHECK(describe(gui, "StatusBar", &parent, &creatable, 1, "addField", BMDS_GUI_INT, &found) == 0 && found);
        // A name that isn't a class.
        CHECK(describe(gui, "NoSuchClass", &parent, &creatable, 0, NULL, 0, &found) != 0);
        EXPECT(BMDS_GUI_BAD_CLASS);
        return;
}

static void badRequests(bmds_gui_t *gui, uint32_t window) {
        // A class that isn't one; a widget with no window; a window inside a window.
        bmds_gui_create(gui, bmds_gui_new_id(gui), bmds_gui_atom(gui, "NoSuchClass"), 0, NULL, 0);
        EXPECT(BMDS_GUI_BAD_CLASS);
        CHECK(bmds_label_new(gui, 0) != 0);
        EXPECT(BMDS_GUI_BAD_MATCH);
        CHECK(bmds_window_new(gui, window) != 0);
        EXPECT(BMDS_GUI_BAD_MATCH);
        // A value not among a property's names; a write-only property read; a name not its.
        const uint32_t box = bmds_gui_new(gui, "Box", window);
        bmds_box_set_direction(gui, box, "sideways");
        EXPECT(BMDS_GUI_BAD_VALUE);
        bmds_gui_value_t v;
        CHECK(bmds_gui_get_named(gui, box, "spacing", &v) != 0);
        EXPECT(BMDS_GUI_BAD_ACCESS);
        bmds_gui_set_named(gui, box, "noSuchProperty", bmds_gui_int(1));
        EXPECT(BMDS_GUI_BAD_NAME);
        // A method with the wrong arguments; an id that isn't one.
        const bmds_gui_value_t extra = bmds_gui_int(1);
        bmds_gui_call_named(gui, window, "show", &extra, 1, NULL);
        EXPECT(BMDS_GUI_BAD_VALUE);
        bmds_gui_destroy(gui, bmds_gui_new_id(gui));
        EXPECT(BMDS_GUI_BAD_ID);
        // Menus: a bar in a box, an item in a window, a popup before its window is shown.
        CHECK(bmds_menu_bar_new(gui, box) != 0);
        EXPECT(BMDS_GUI_BAD_MATCH);
        CHECK(bmds_menu_item_new(gui, window) != 0);
        EXPECT(BMDS_GUI_BAD_MATCH);
        const uint32_t popup = bmds_menu_new(gui, window);
        bmds_menu_popup(gui, popup, 10, 10);
        EXPECT(BMDS_GUI_BAD_ACCESS);
        // A shortcut that isn't one; a timer faster than allowed.
        const uint32_t item = bmds_menu_item_new(gui, popup);
        bmds_menu_item_set_shortcut(gui, item, "Ctrl+Nonsense");
        EXPECT(BMDS_GUI_BAD_VALUE);
        bmds_menu_item_set_shortcut(gui, item, "Ctrl+Shift+F5");
        EXPECT_NONE();
        const uint32_t timer = bmds_gui_new(gui, "Timer", 0);
        bmds_timer_set_interval(gui, timer, 1);
        EXPECT(BMDS_GUI_BAD_VALUE);
        bmds_gui_destroy(gui, timer);
        bmds_gui_destroy(gui, popup);
        bmds_gui_destroy(gui, box);
        EXPECT_NONE();
        return;
}

static void events(bmds_gui_t *gui, uint32_t window) {
        // A method's event: a button clicked.
        const uint32_t button = bmds_button_new(gui, window);
        bmds_button_click(gui, button);
        CHECK(nextEventIs(gui, button, BMDS_GUI_BUTTON_CLICKED, 1000));
        // Turned off, it isn't sent.
        const uint32_t clicked = bmds_gui_atom(gui, BMDS_GUI_BUTTON_CLICKED);
        bmds_gui_listen(gui, button, &clicked, 1, 0);
        bmds_button_click(gui, button);
        bmds_gui_event_t e;
        CHECK(nextEvent(gui, &e, 200) == 0);
        // The client's own changes aren't told back: a list's current row, a text set.
        const uint32_t           list = bmds_list_view_new(gui, window);
        static const char *const rows[] = {"one", "two", "three"};
        bmds_item_view_set_items(gui, list, rows, 3);
        bmds_item_view_set_current(gui, list, 1);
        const uint32_t edit = bmds_text_edit_new(gui, window);
        bmds_text_edit_set_text(gui, edit, "hello");
        CHECK(nextEvent(gui, &e, 200) == 0);
        bmds_gui_value_t v;
        CHECK(bmds_item_view_get_current(gui, list, &v) == 0 && v.number == 1);
        CHECK(bmds_text_edit_get_text(gui, edit, &v) == 0 && v.length == 5 && memcmp(v.text, "hello", 5) == 0);
        CHECK(bmds_text_edit_get_modified(gui, edit, &v) == 0 && v.number == 0);
        // A row removed before the current one moves it: told.
        const bmds_gui_value_t first = bmds_gui_int(0);
        bmds_gui_call_named(gui, list, "removeItem", &first, 1, NULL);
        CHECK(nextEventIs(gui, list, BMDS_GUI_ITEM_VIEW_CURRENT_CHANGED, 1000));
        // A one-shot timer fires once.
        const uint32_t timer = bmds_gui_new(gui, "Timer", 0);
        bmds_timer_set_interval(gui, timer, 50);
        bmds_timer_set_repeat(gui, timer, 0);
        bmds_timer_start(gui, timer);
        CHECK(nextEventIs(gui, timer, BMDS_GUI_TIMER_TIMEOUT, 1000));
        CHECK(nextEvent(gui, &e, 300) == 0);
        CHECK(bmds_timer_get_active(gui, timer, &v) == 0 && v.number == 0);
        bmds_gui_destroy(gui, timer);
        bmds_gui_destroy(gui, edit);
        bmds_gui_destroy(gui, list);
        bmds_gui_destroy(gui, button);
        EXPECT_NONE();
        return;
}

static void containers(bmds_gui_t *gui, uint32_t window) {
        // Tabs take pages only; a splitter two sides; each lets go of a child that goes.
        const uint32_t tabs = bmds_tabs_new(gui, window);
        const uint32_t p1 = bmds_tab_page_new(gui, tabs), p2 = bmds_tab_page_new(gui, tabs);
        bmds_tab_page_set_title(gui, p1, "One");
        bmds_tab_page_set_title(gui, p2, "Two");
        CHECK(bmds_label_new(gui, tabs) != 0);
        EXPECT(BMDS_GUI_BAD_MATCH);
        const uint32_t split = bmds_splitter_new(gui, p2);
        const uint32_t s1 = bmds_label_new(gui, split);
        bmds_label_new(gui, split);
        CHECK(bmds_label_new(gui, split) != 0);
        EXPECT(BMDS_GUI_BAD_MATCH);
        bmds_gui_destroy(gui, p1);
        bmds_gui_destroy(gui, s1);
        bmds_label_new(gui, split); // a side again
        bmds_gui_value_t v;
        CHECK(bmds_tabs_get_current(gui, tabs, &v) == 0 && v.number == 0);
        EXPECT_NONE();
        // A menu bar and its menus, a submenu and items, taken apart while shown.
        const uint32_t bar = bmds_menu_bar_new(gui, window);
        const uint32_t file = bmds_menu_new(gui, bar);
        bmds_menu_set_title(gui, file, "&File");
        const uint32_t more = bmds_menu_new(gui, file);
        bmds_menu_set_title(gui, more, "&More");
        bmds_menu_item_set_text(gui, bmds_menu_item_new(gui, more), "Deep");
        bmds_menu_add_separator(gui, file);
        bmds_menu_item_set_text(gui, bmds_menu_item_new(gui, file), "&Quit");
        bmds_gui_sync(gui);
        bmds_gui_destroy(gui, more);
        bmds_gui_destroy(gui, file);
        bmds_gui_destroy(gui, bar);
        bmds_gui_destroy(gui, tabs);
        EXPECT_NONE();
        return;
}

// Images: their pixels by PutImage, shown by buttons, windows, menus and rows, outliving their
// objects while shown.
static void images(bmds_gui_t *gui, uint32_t window) {
        static uint32_t pixels[16 * 16];
        for (int i = 0; i < 16 * 16; ++i) pixels[i] = 0xff000000u | (uint32_t)(i * 0x010203);
        const uint32_t image = bmds_gui_new(gui, "Image", 0);
        CHECK(bmds_gui_put_image(gui, image, 16, 16, pixels) == 0);
        bmds_gui_value_t v;
        CHECK(bmds_image_get_width(gui, image, &v) == 0 && v.number == 16);
        CHECK(bmds_image_get_height(gui, image, &v) == 0 && v.number == 16);
        EXPECT_NONE();
        // Too big; not an image; an image without pixels yet; an image under a window.
        static uint32_t row[600];
        bmds_gui_put_image(gui, image, 600, 1, row);
        EXPECT(BMDS_GUI_BAD_VALUE);
        const uint32_t label = bmds_label_new(gui, window);
        bmds_gui_put_image(gui, label, 16, 16, pixels);
        EXPECT(BMDS_GUI_BAD_MATCH);
        const uint32_t empty = bmds_gui_new(gui, "Image", 0);
        const uint32_t button = bmds_button_new(gui, window);
        bmds_button_set_icon_image(gui, button, empty);
        EXPECT(BMDS_GUI_BAD_MATCH);
        bmds_button_set_icon_image(gui, button, label);
        EXPECT(BMDS_GUI_BAD_MATCH);
        CHECK(bmds_gui_new(gui, "Image", window) != 0);
        EXPECT(BMDS_GUI_BAD_MATCH);
        // Shown by a button, the window, a menu item and list rows; then destroyed while shown.
        bmds_button_set_icon_image(gui, button, image);
        bmds_window_set_icon_image(gui, window, image);
        CHECK(bmds_button_get_icon_image(gui, button, &v) == 0 && v.number == image);
        const uint32_t popup = bmds_menu_new(gui, window);
        bmds_menu_item_set_icon_image(gui, bmds_menu_item_new(gui, popup), image);
        const uint32_t           list = bmds_list_view_new(gui, window);
        static const char *const rows[] = {"a", "b", "c"};
        bmds_item_view_set_items(gui, list, rows, 3);
        for (int i = 0; i < 3; ++i) bmds_item_view_set_icon_image(gui, list, i, image);
        EXPECT_NONE();
        bmds_gui_destroy(gui, image);
        CHECK(bmds_gui_put_image(gui, image, 16, 16, pixels) == 0);
        EXPECT(BMDS_GUI_BAD_ID); // its id is free at once
        const bmds_gui_value_t first = bmds_gui_int(0);
        bmds_gui_call_named(gui, list, "removeItem", &first, 1, NULL);
        bmds_item_view_clear(gui, list);
        bmds_gui_destroy(gui, list);
        bmds_gui_destroy(gui, popup);
        bmds_window_set_icon_image(gui, window, 0);
        bmds_gui_destroy(gui, button);
        bmds_gui_destroy(gui, empty);
        bmds_gui_destroy(gui, label);
        EXPECT_NONE();
        // A big one, in pieces (512 × 512: 8 pieces).
        static uint32_t big[512 * 512];
        const uint32_t  large = bmds_gui_new(gui, "Image", 0);
        CHECK(bmds_gui_put_image(gui, large, 512, 512, big) == 0);
        CHECK(bmds_image_get_width(gui, large, &v) == 0 && v.number == 512);
        bmds_gui_destroy(gui, large);
        EXPECT_NONE();
        return;
}

// A canvas: its size told when laid out, pixels raw and run-length coded, a rectangle outside
// its picture dropped.
static void canvasTest(bmds_gui_t *gui, uint32_t window) {
        const uint32_t c = bmds_canvas_new(gui, window);
        bmds_canvas_set_preferred_width(gui, c, 64);
        bmds_canvas_set_preferred_height(gui, c, 48);
        bmds_gui_event_t e;
        int              sized = 0;
        while (!sized && nextEvent(gui, &e, 1000) == 1)
                sized = e.source == c && bmds_gui_event_is(gui, &e, BMDS_GUI_CANVAS_RESIZED) && e.count == 2 &&
                        e.values[0].number > 0 && e.values[1].number > 0;
        CHECK(sized);
        bmds_gui_value_t v;
        CHECK(bmds_canvas_get_width(gui, c, &v) == 0 && v.number > 0);
        static uint16_t pixels[32 * 32];
        for (int i = 0; i < 32 * 32; ++i) pixels[i] = (uint16_t)(i * 37); // varied: sent raw
        CHECK(bmds_gui_put_pixels(gui, c, 0, 0, 32, 32, pixels, 32) == 0);
        for (int i = 0; i < 32 * 32; ++i) pixels[i] = 0xf800; // one colour: run-length coded
        CHECK(bmds_gui_put_pixels(gui, c, 8, 8, 32, 32, pixels, 32) == 0);
        CHECK(bmds_gui_put_pixels(gui, c, 10000, 10000, 32, 32, pixels, 32) == 0); // outside: dropped
        CHECK(bmds_gui_put_pixels(gui, c, -16, -16, 32, 32, pixels, 32) == 0);     // partly: cut
        EXPECT_NONE();
        const uint32_t label = bmds_label_new(gui, window);
        bmds_gui_put_pixels(gui, label, 0, 0, 32, 32, pixels, 32);
        EXPECT(BMDS_GUI_BAD_MATCH);
        bmds_gui_destroy(gui, label);
        bmds_gui_destroy(gui, c);
        EXPECT_NONE();
        return;
}

// A canvas laid out at about 64 x 48 (its resized event waited for).
static uint32_t sizedCanvas(bmds_gui_t *gui, uint32_t window, uint32_t *width, uint32_t *height) {
        const uint32_t c = bmds_canvas_new(gui, window);
        bmds_canvas_set_preferred_width(gui, c, 64);
        bmds_canvas_set_preferred_height(gui, c, 48);
        bmds_gui_event_t e;
        int              sized = 0;
        while (!sized && nextEvent(gui, &e, 1000) == 1)
                sized = e.source == c && bmds_gui_event_is(gui, &e, BMDS_GUI_CANVAS_RESIZED) && e.count == 2 &&
                        e.values[0].number > 0 && e.values[1].number > 0;
        CHECK(sized);
        bmds_gui_value_t v;
        CHECK(bmds_canvas_get_width(gui, c, &v) == 0);
        *width = (uint32_t)v.number;
        CHECK(bmds_canvas_get_height(gui, c, &v) == 0);
        *height = (uint32_t)v.number;
        return c;
}

static int surfaceInfo(int fd, bmds_surface_info_t *info) {
        memset(info, 0, sizeof(*info));
        info->size = sizeof(*info);
        return bmds_surface_control(fd, BMDS_SURFACE_INFO, info, sizeof(*info));
}

// One frame: a buffer taken, its top half filled by the CPU and its bottom by the 2D-DMA, presented.
static int frame(int fd, uint32_t width, uint32_t height, uint16_t top, uint16_t bottom) {
        const int32_t index = bmds_surface_acquire(fd, 1000);
        if (index < 0) return index;
        bmds_surface_buffer_t b = {sizeof(b), (uint32_t)index, NULL, 0};
        int32_t               r = bmds_surface_buffer(fd, &b);
        if (r < 0) return r;
        for (uint32_t y = 0; y < height / 2; ++y) {
                uint16_t *row = (uint16_t *)((uint8_t *)b.pixels + y * b.stride);
                for (uint32_t x = 0; x < width; ++x) row[x] = top;
        }
        bmds_surface_piece_t piece = {0, (int32_t)(height / 2), width, height - height / 2, 0, 0};
        bmds_surface_blit_t  bl = {sizeof(bl), (uint32_t)index, BMDS_BLIT_FILL, bottom, 1, &piece};
        r = bmds_surface_control(fd, BMDS_SURFACE_BLIT, &bl, sizeof(bl));
        if (r < 0) return r;
        bmds_surface_present_t p = {sizeof(p), (uint32_t)index, NULL, 0};
        r = bmds_surface_present(fd, &p);
        return r < 0 ? r : 0;
}

// CANVAS-SURFACE: a canvas's swap chain, drawn and presented as a game's (local connections).
static void canvasSurfaceTest(bmds_gui_t *gui, uint32_t window) {
        CHECK(bmds_gui_has_extension(gui, "CANVAS-SURFACE") == 1);
        uint32_t       width = 0, height = 0;
        const uint32_t c = sizedCanvas(gui, window, &width, &height);
        const int      fd = bmds_gui_canvas_surface_open(gui, c, NULL); // the canvas's size, RGB565
        CHECK(fd >= 0);
        bmds_surface_info_t info;
        CHECK(surfaceInfo(fd, &info) == 0 && info.shown == 1 && info.picture_width == width &&
              info.picture_height == height && info.presents == 0);
        bmds_surface_buffer_t b = {sizeof(b), 0, NULL, 0};
        CHECK(bmds_surface_buffer(fd, &b) == 0 && b.pixels != NULL && b.stride == width * 2);
        for (int i = 0; i < 10; ++i) CHECK(frame(fd, width, height, (uint16_t)(0x001f << (i % 3)), 0x07e0) == 0);
        CHECK(surfaceInfo(fd, &info) == 0 && info.presents == 10);
        // A window's: no title, palette or pointer mode; no events.
        CHECK(bmds_surface_control(fd, BMDS_SURFACE_TITLE, "x", 1) == -BMDS_EINVAL);
        uint32_t mode = BMDS_POINTER_HIDDEN;
        CHECK(bmds_surface_control(fd, BMDS_SURFACE_POINTER, &mode, sizeof(mode)) == -BMDS_EINVAL);

        // Another, its size given, in BGR24 with 3 buffers: the canvas shows it instead.
        bmds_surface_config_t config = {sizeof(config), 32, 16, BMDS_PIXEL_BGR24, 3, 0, 0, 0, NULL};
        const int             fd2 = bmds_gui_canvas_surface_open(gui, c, &config);
        CHECK(fd2 >= 0);
        CHECK(surfaceInfo(fd2, &info) == 0 && info.shown == 1 && info.picture_width == 32 && info.picture_height == 16);
        CHECK(surfaceInfo(fd, &info) == 0 && info.shown == 0); // replaced: presents still work
        CHECK(frame(fd, width, height, 0xffff, 0) == 0);
        const int32_t index = bmds_surface_acquire(fd2, 1000);
        CHECK(index >= 0);
        bmds_surface_present_t p = {sizeof(p), (uint32_t)index, NULL, 0};
        CHECK(bmds_surface_present(fd2, &p) >= 0);
        CHECK(bmds_surface_present(fd2, &p) == -BMDS_EINVAL); // presented already: not the program's

        // Refused: a palette's format, half a size, too big, not a canvas, no such object.
        config.format = BMDS_PIXEL_L8;
        CHECK(bmds_gui_canvas_surface_open(gui, c, &config) == -1 && errno == EINVAL);
        config.format = BMDS_PIXEL_RGB565;
        config.width = 0;
        CHECK(bmds_gui_canvas_surface_open(gui, c, &config) == -1 && errno == EINVAL);
        config.width = 2000;
        CHECK(bmds_gui_canvas_surface_open(gui, c, &config) == -1 && errno == EINVAL);
        const uint32_t label = bmds_label_new(gui, window);
        CHECK(bmds_gui_canvas_surface_open(gui, label, NULL) == -1 && errno == EINVAL);
        CHECK(bmds_gui_canvas_surface_open(gui, (c & 0xff000000u) | 0xfffff0u, NULL) == -1 && errno == ENOENT);
        CHECK(bmds_gui_canvas_surface(-1, c, NULL) == -BMDS_EBADF);
        CHECK(bmds_gui_canvas_surface(1, c, NULL) == -BMDS_ENOTTY); // stdout: not a GUI connection
        bmds_gui_destroy(gui, label);

        // The canvas going first: the fd still presents, shown nowhere.
        bmds_gui_destroy(gui, c);
        CHECK(bmds_gui_sync(gui) == 0);
        CHECK(surfaceInfo(fd2, &info) == 0 && info.shown == 0);
        CHECK(frame(fd, width, height, 0, 0) == 0);
        close(fd);
        close(fd2);

        // The fd going first: the canvas keeps the last picture until it goes.
        const uint32_t c2 = sizedCanvas(gui, window, &width, &height);
        const int      fd3 = bmds_gui_canvas_surface_open(gui, c2, NULL);
        CHECK(fd3 >= 0 && frame(fd3, width, height, 0xf800, 0x001f) == 0);
        close(fd3);
        CHECK(bmds_gui_sync(gui) == 0);
        bmds_gui_destroy(gui, c2);
        EXPECT_NONE();
        return;
}

// The next event @p name from @p source within @p ms (others skipped), in @p out. @return 1, or 0.
static int waitFor(bmds_gui_t *gui, uint32_t source, const char *name, int ms, bmds_gui_event_t *out) {
        while (nextEvent(gui, out, ms) == 1)
                if (out->source == source && bmds_gui_event_is(gui, out, name)) return 1;
        return 0;
}

// @p clip's paste of @p type from @p selection, waited for: its bytes in @p out (NUL-ended, at most
// @p max), its length returned; or -1 (pasteFailed: its reason's atom in @p why), -2 (no answer).
static int pasteOf(bmds_gui_t *gui, uint32_t clip, const char *selection, const char *type, char *out, size_t max,
                   uint32_t *why) {
        bmds_clipboard_paste(gui, clip, selection, type);
        bmds_gui_event_t e;
        while (nextEvent(gui, &e, 3000) == 1) {
                if (e.source != clip) continue;
                if (bmds_gui_event_is(gui, &e, BMDS_GUI_CLIPBOARD_PASTED) && e.count == 3) {
                        const size_t n = e.values[2].length < max - 1 ? e.values[2].length : max - 1;
                        memcpy(out, e.values[2].text, n);
                        out[n] = '\0';
                        return (int)e.values[2].length;
                }
                if (bmds_gui_event_is(gui, &e, BMDS_GUI_CLIPBOARD_PASTE_FAILED) && e.count == 3) {
                        if (why != NULL) *why = (uint32_t)e.values[2].number;
                        return -1;
                }
        }
        return -2;
}

// The clipboard (CLIPBOARD): copies and pastes between two connections, offers answered on
// request, losses, refusals. The user's clipboard text is put back after.
static void clipboardTest(bmds_gui_t *gui) {
        CHECK(bmds_gui_has_extension(gui, "CLIPBOARD") == 1);
        bmds_gui_t *other = bmds_gui_connect(NULL);
        CHECK(other != NULL);
        if (other == NULL) return;
        bmds_gui_on_error(other, onError, NULL);
        const uint32_t a = bmds_clipboard_new(gui, 0), b = bmds_clipboard_new(other, 0);
        static char    saved[2][4096], got[256];
        int            savedLength[2];
        uint32_t       why = 0;
        savedLength[0] = pasteOf(other, b, "clipboard", "text", saved[0], sizeof(saved[0]), NULL);
        savedLength[1] = pasteOf(other, b, "primary", "text", saved[1], sizeof(saved[1]), NULL);

        // Text copied by one, pasted by the other.
        bmds_clipboard_copy_text(gui, a, "clipboard", "hello, clipboard");
        bmds_gui_sync(gui);
        bmds_gui_value_t v;
        CHECK(bmds_clipboard_owns(gui, a, "clipboard", &v) == 0 && v.number == 1);
        CHECK(bmds_clipboard_types(other, b, "clipboard", &v) == 0 && v.count == 1);
        CHECK(pasteOf(other, b, "clipboard", "text/plain", got, sizeof(got), NULL) == 16 &&
              strcmp(got, "hello, clipboard") == 0);
        CHECK(pasteOf(other, b, "clipboard", "image/png", got, sizeof(got), &why) == -1 &&
              why == bmds_gui_atom(other, "notOffered"));

        // An offer answered on request, its bytes any (a NUL among them), in two pieces.
        const char *types[] = {"application/x-guitest", "text"};
        bmds_clipboard_offer(gui, a, "primary", types, 2);
        bmds_gui_sync(gui);
        bmds_clipboard_paste(other, b, "primary", "application/x-guitest");
        bmds_gui_flush(other);
        bmds_gui_event_t e;
        CHECK(waitFor(gui, a, BMDS_GUI_CLIPBOARD_REQUESTED, 3000, &e) && e.count == 3 &&
              e.values[1].number == bmds_gui_atom(gui, "primary") && e.values[2].length == 21 &&
              memcmp(e.values[2].text, "application/x-guitest", 21) == 0);
        const int32_t request = (int32_t)e.values[0].number;
        bmds_clipboard_answer(gui, a, request, "ab\0", 3);
        bmds_clipboard_answer(gui, a, request, "cd", 2);
        bmds_clipboard_finish(gui, a, request);
        bmds_gui_flush(gui);
        CHECK(waitFor(other, b, BMDS_GUI_CLIPBOARD_PASTED, 3000, &e) && e.count == 3 &&
              e.values[2].tag == BMDS_GUI_BYTES && e.values[2].length == 5 &&
              memcmp(e.values[2].text, "ab\0cd", 5) == 0);
        bmds_clipboard_finish(gui, a, request); // finished already
        EXPECT(BMDS_GUI_BAD_VALUE);
        // Its own offer pasted: it has it.
        CHECK(pasteOf(gui, a, "primary", "text", got, sizeof(got), &why) == -1 && why == bmds_gui_atom(gui, "own"));

        // Copied over by the other: lost.
        bmds_clipboard_copy_text(other, b, "primary", "mine now");
        bmds_gui_flush(other);
        CHECK(waitFor(gui, a, BMDS_GUI_CLIPBOARD_LOST, 3000, &e) && e.count == 1 &&
              e.values[0].number == bmds_gui_atom(gui, "primary"));
        CHECK(bmds_clipboard_owns(gui, a, "primary", &v) == 0 && v.number == 0);
        CHECK(pasteOf(gui, a, "primary", "text", got, sizeof(got), NULL) == 8 && strcmp(got, "mine now") == 0);

        // Cleared by its owner only; empty pastes fail.
        bmds_clipboard_clear(other, b, "clipboard"); // not its copy: left
        CHECK(bmds_clipboard_types(other, b, "clipboard", &v) == 0 && v.count == 1);
        bmds_clipboard_clear(gui, a, "clipboard");
        bmds_gui_sync(gui);
        CHECK(bmds_clipboard_types(other, b, "clipboard", &v) == 0 && v.count == 0);
        CHECK(pasteOf(other, b, "clipboard", "text", got, sizeof(got), &why) == -1 &&
              why == bmds_gui_atom(other, "empty"));

        // An offer goes with its object.
        bmds_clipboard_offer(gui, a, "clipboard", types, 2);
        bmds_gui_destroy(gui, a);
        bmds_gui_sync(gui);
        CHECK(bmds_clipboard_types(other, b, "clipboard", &v) == 0 && v.count == 0);

        // Refused: a selection that isn't one, no types, an unknown request.
        const uint32_t c = bmds_clipboard_new(gui, 0);
        bmds_clipboard_copy_text(gui, c, "drag", "x");
        EXPECT(BMDS_GUI_BAD_VALUE);
        bmds_clipboard_offer(gui, c, "clipboard", types, 0);
        EXPECT(BMDS_GUI_BAD_VALUE);
        bmds_clipboard_answer(gui, c, 12345, "x", 1);
        EXPECT(BMDS_GUI_BAD_VALUE);

        // The user's text back (or nothing, as it was).
        for (int i = 0; i < 2; ++i) {
                const char *selection = i == 0 ? "clipboard" : "primary";
                if (savedLength[i] >= 0)
                        bmds_clipboard_copy_text(other, b, selection, saved[i]);
                else
                        bmds_clipboard_clear(other, b, selection); // only if its copy is there
        }
        bmds_gui_sync(other);
        bmds_gui_destroy(gui, c);
        EXPECT_NONE();
        bmds_gui_close(other);
        return;
}

// Many windows with children made and destroyed; the service answers throughout.
static void many(bmds_gui_t *gui) {
        for (int i = 0; i < 100; ++i) {
                const uint32_t w = bmds_window_new(gui, 0);
                bmds_label_set_text(gui, bmds_label_new(gui, w), "a label");
                const uint32_t row = bmds_gui_new(gui, "Box", w);
                bmds_button_new(gui, row);
                bmds_combo_box_add_item(gui, bmds_combo_box_new(gui, row), "an item");
                bmds_gui_destroy(gui, w);
        }
        EXPECT_NONE();
        return;
}

int main(void) {
        bmds_gui_t *gui = bmds_gui_connect(NULL);
        if (gui == NULL) {
                printf("guitest: no GUI\n");
                return 1;
        }
        bmds_gui_on_error(gui, onError, NULL);
        bmds_gui_value_t screen;
        CHECK(bmds_gui_display(gui, "screen", &screen) == 0 && screen.width > 0 && screen.height > 0);
        describeAll(gui);
        const uint32_t window = bmds_window_new(gui, 0);
        testWindow = window;
        bmds_window_set_title(gui, window, "guitest");
        badRequests(gui, window);
        bmds_window_show(gui, window);
        events(gui, window);
        containers(gui, window);
        images(gui, window);
        canvasTest(gui, window);
        canvasSurfaceTest(gui, window);
        clipboardTest(gui);
        many(gui);
        bmds_gui_close(gui);
        if (failures != 0) {
                printf("guitest: %d failure(s)\n", failures);
                return 1;
        }
        printf("guitest: ok\n");
        return 0;
}
