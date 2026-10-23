/* bmds/guiclasses.h: the GUI classes' C wrappers (devplan/guiprograms.md §10). Generated
   by tools/guigen.py from sdk/gui/classes.def: don't edit; change the schema and run it. */
/* clang-format off */

#ifndef BMDS_GUICLASSES_H
#define BMDS_GUICLASSES_H

#include <bmds/gui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Widget */
static inline int bmds_widget_set_enabled(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "enabled", bmds_gui_bool(value));
}
static inline int bmds_widget_get_enabled(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "enabled", out);
}
static inline int bmds_widget_set_visible(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "visible", bmds_gui_bool(value));
}
static inline int bmds_widget_get_visible(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "visible", out);
}
static inline int bmds_widget_set_focus(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "setFocus", NULL, 0, NULL);
}

/* ---- Window */
/** @brief A new Window under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_window_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Window", parent);
}
static inline int bmds_window_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_window_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}
static inline int bmds_window_set_width(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "width", bmds_gui_int(value));
}
static inline int bmds_window_get_width(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "width", out);
}
static inline int bmds_window_set_height(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "height", bmds_gui_int(value));
}
static inline int bmds_window_get_height(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "height", out);
}
static inline int bmds_window_show(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "show", NULL, 0, NULL);
}
static inline int bmds_window_close(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "close", NULL, 0, NULL);
}
#define BMDS_GUI_WINDOW_CLOSED "closed" /**< Window's event (no values). */

/* ---- Box (of Widget) */
/** @brief A new Box under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_box_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Box", parent);
}
static inline int bmds_box_set_direction(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "direction", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_box_get_direction(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "direction", out);
}
static inline int bmds_box_set_spacing(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "spacing", bmds_gui_int(value));
}
static inline int bmds_box_set_margin(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "margin", bmds_gui_int(value));
}

/* ---- Label (of Widget) */
/** @brief A new Label under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_label_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Label", parent);
}
static inline int bmds_label_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_label_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}

/* ---- Button (of Widget) */
/** @brief A new Button under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_button_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Button", parent);
}
static inline int bmds_button_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_button_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}
static inline int bmds_button_click(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "click", NULL, 0, NULL);
}
#define BMDS_GUI_BUTTON_CLICKED "clicked" /**< Button's event (no values). */

/* ---- LineEdit (of Widget) */
/** @brief A new LineEdit under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_line_edit_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "LineEdit", parent);
}
static inline int bmds_line_edit_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_line_edit_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}
static inline int bmds_line_edit_set_masked(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "masked", bmds_gui_bool(value));
}
static inline int bmds_line_edit_set_max_length(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "maxLength", bmds_gui_int(value));
}
static inline int bmds_line_edit_select_all(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "selectAll", NULL, 0, NULL);
}
#define BMDS_GUI_LINE_EDIT_ACCEPTED "accepted" /**< LineEdit's event (String). */
#define BMDS_GUI_LINE_EDIT_EDITED "edited" /**< LineEdit's event (String). */
static inline int bmds_window_set_min_width(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "minWidth", bmds_gui_int(value));
}
static inline int bmds_window_get_min_width(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "minWidth", out);
}
static inline int bmds_window_set_min_height(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "minHeight", bmds_gui_int(value));
}
static inline int bmds_window_get_min_height(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "minHeight", out);
}
static inline int bmds_window_set_placement(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "placement", bmds_gui_string(value));
}
static inline int bmds_window_get_placement(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "placement", out);
}
static inline int bmds_window_set_icon(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "icon", bmds_gui_string(value));
}
static inline int bmds_window_get_icon(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "icon", out);
}
static inline int bmds_window_set_pointer(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "pointer", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_window_get_pointer(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "pointer", out);
}
static inline int bmds_window_set_frame(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "frame", bmds_gui_bool(value));
}
static inline int bmds_window_get_frame(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "frame", out);
}
static inline int bmds_window_set_title_bar(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "titleBar", bmds_gui_bool(value));
}
static inline int bmds_window_get_title_bar(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "titleBar", out);
}
static inline int bmds_window_set_buttons(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "buttons", bmds_gui_bool(value));
}
static inline int bmds_window_get_buttons(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "buttons", out);
}
static inline int bmds_window_set_resizable(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "resizable", bmds_gui_bool(value));
}
static inline int bmds_window_get_resizable(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "resizable", out);
}
static inline int bmds_window_set_above(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "above", bmds_gui_bool(value));
}
static inline int bmds_window_get_above(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "above", out);
}
static inline int bmds_window_set_shaded(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "shaded", bmds_gui_bool(value));
}
static inline int bmds_window_get_shaded(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "shaded", out);
}
static inline int bmds_window_get_minimized(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "minimized", out);
}
static inline int bmds_window_get_maximized(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "maximized", out);
}
static inline int bmds_window_get_active(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "active", out);
}
static inline int bmds_window_set_owner(bmds_gui_t *gui, uint32_t id, uint32_t value) {
        return bmds_gui_set_named(gui, id, "owner", bmds_gui_id(value));
}
static inline int bmds_window_get_owner(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "owner", out);
}
static inline int bmds_window_set_modal(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "modal", bmds_gui_bool(value));
}
static inline int bmds_window_get_modal(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "modal", out);
}
static inline int bmds_window_set_window_menu_item(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "windowMenuItem", bmds_gui_string(value));
}
static inline int bmds_window_get_window_menu_item(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "windowMenuItem", out);
}
static inline int bmds_window_minimize(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "minimize", NULL, 0, NULL);
}
static inline int bmds_window_maximize(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "maximize", NULL, 0, NULL);
}
static inline int bmds_window_restore(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "restore", NULL, 0, NULL);
}
static inline int bmds_window_raise(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "raise", NULL, 0, NULL);
}
#define BMDS_GUI_WINDOW_RESIZED "resized" /**< Window's event (Int, Int). */
#define BMDS_GUI_WINDOW_ACTIVATED "activated" /**< Window's event (Bool). */
#define BMDS_GUI_WINDOW_CLOSE_REQUESTED "closeRequested" /**< Window's event (no values). */
#define BMDS_GUI_WINDOW_WINDOW_MENU_CHOSEN "windowMenuChosen" /**< Window's event (no values). */

#ifdef __cplusplus
}
#endif

#endif
