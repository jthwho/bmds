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
static inline int bmds_widget_set_stretch(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "stretch", bmds_gui_int(value));
}
static inline int bmds_widget_get_stretch(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "stretch", out);
}

/* ---- CheckBox (of Widget) */
/** @brief A new CheckBox under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_check_box_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "CheckBox", parent);
}
static inline int bmds_check_box_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_check_box_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}
static inline int bmds_check_box_set_checked(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "checked", bmds_gui_bool(value));
}
static inline int bmds_check_box_get_checked(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "checked", out);
}
#define BMDS_GUI_CHECK_BOX_TOGGLED "toggled" /**< CheckBox's event (Bool). */

/* ---- NumberField (of Widget) */
/** @brief A new NumberField under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_number_field_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "NumberField", parent);
}
static inline int bmds_number_field_set_value(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "value", bmds_gui_int(value));
}
static inline int bmds_number_field_get_value(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "value", out);
}
static inline int bmds_number_field_set_minimum(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "minimum", bmds_gui_int(value));
}
static inline int bmds_number_field_get_minimum(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "minimum", out);
}
static inline int bmds_number_field_set_maximum(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "maximum", bmds_gui_int(value));
}
static inline int bmds_number_field_get_maximum(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "maximum", out);
}
static inline int bmds_number_field_set_step(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "step", bmds_gui_int(value));
}
static inline int bmds_number_field_get_step(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "step", out);
}
#define BMDS_GUI_NUMBER_FIELD_CHANGED "changed" /**< NumberField's event (Int). */

/* ---- Slider (of Widget) */
/** @brief A new Slider under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_slider_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Slider", parent);
}
static inline int bmds_slider_set_value(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "value", bmds_gui_int(value));
}
static inline int bmds_slider_get_value(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "value", out);
}
static inline int bmds_slider_set_minimum(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "minimum", bmds_gui_int(value));
}
static inline int bmds_slider_get_minimum(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "minimum", out);
}
static inline int bmds_slider_set_maximum(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "maximum", bmds_gui_int(value));
}
static inline int bmds_slider_get_maximum(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "maximum", out);
}
static inline int bmds_slider_set_step(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "step", bmds_gui_int(value));
}
static inline int bmds_slider_get_step(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "step", out);
}
static inline int bmds_slider_set_direction(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "direction", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_slider_get_direction(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "direction", out);
}
#define BMDS_GUI_SLIDER_CHANGED "changed" /**< Slider's event (Int). */
#define BMDS_GUI_SLIDER_RELEASED "released" /**< Slider's event (Int). */

/* ---- ComboBox (of Widget) */
/** @brief A new ComboBox under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_combo_box_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "ComboBox", parent);
}
static inline int bmds_combo_box_set_items(bmds_gui_t *gui, uint32_t id, const char *const *value, uint32_t value_count) {
        return bmds_gui_set_named(gui, id, "items", bmds_gui_strings(gui, value, value_count));
}
static inline int bmds_combo_box_get_items(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "items", out);
}
static inline int bmds_combo_box_set_current(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "current", bmds_gui_int(value));
}
static inline int bmds_combo_box_get_current(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "current", out);
}
static inline int bmds_combo_box_add_item(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "addItem", args, 1, NULL);
}
static inline int bmds_combo_box_clear(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "clear", NULL, 0, NULL);
}
#define BMDS_GUI_COMBO_BOX_CHANGED "changed" /**< ComboBox's event (Int). */

/* ---- ProgressBar (of Widget) */
/** @brief A new ProgressBar under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_progress_bar_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "ProgressBar", parent);
}
static inline int bmds_progress_bar_set_value(bmds_gui_t *gui, uint32_t id, int64_t value) {
        return bmds_gui_set_named(gui, id, "value", bmds_gui_long(value));
}
static inline int bmds_progress_bar_get_value(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "value", out);
}
static inline int bmds_progress_bar_set_maximum(bmds_gui_t *gui, uint32_t id, int64_t value) {
        return bmds_gui_set_named(gui, id, "maximum", bmds_gui_long(value));
}
static inline int bmds_progress_bar_get_maximum(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "maximum", out);
}

/* ---- Spacer (of Widget) */
/** @brief A new Spacer under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_spacer_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Spacer", parent);
}
static inline int bmds_spacer_set_size(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "size", bmds_gui_int(value));
}
static inline int bmds_spacer_get_size(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "size", out);
}

/* ---- Form (of Widget) */
/** @brief A new Form under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_form_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Form", parent);
}

/* ---- FormSection (of Widget) */
/** @brief A new FormSection under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_form_section_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "FormSection", parent);
}
static inline int bmds_form_section_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_form_section_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}

/* ---- FormRow (of Widget) */
/** @brief A new FormRow under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_form_row_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "FormRow", parent);
}
static inline int bmds_form_row_set_label(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "label", bmds_gui_string(value));
}
static inline int bmds_form_row_get_label(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "label", out);
}
static inline int bmds_form_row_set_help(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "help", bmds_gui_string(value));
}
static inline int bmds_form_row_get_help(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "help", out);
}

/* ---- Splitter (of Widget) */
/** @brief A new Splitter under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_splitter_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Splitter", parent);
}
static inline int bmds_splitter_set_direction(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "direction", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_splitter_get_direction(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "direction", out);
}
static inline int bmds_splitter_set_position(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "position", bmds_gui_int(value));
}
static inline int bmds_splitter_get_position(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "position", out);
}
static inline int bmds_splitter_set_min_first(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "minFirst", bmds_gui_int(value));
}
static inline int bmds_splitter_get_min_first(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "minFirst", out);
}
static inline int bmds_splitter_set_min_second(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "minSecond", bmds_gui_int(value));
}
static inline int bmds_splitter_get_min_second(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "minSecond", out);
}

/* ---- Tabs (of Widget) */
/** @brief A new Tabs under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_tabs_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Tabs", parent);
}
static inline int bmds_tabs_set_current(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "current", bmds_gui_int(value));
}
static inline int bmds_tabs_get_current(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "current", out);
}
#define BMDS_GUI_TABS_CHANGED "changed" /**< Tabs's event (Int). */

/* ---- TabPage (of Box) */
/** @brief A new TabPage under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_tab_page_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "TabPage", parent);
}
static inline int bmds_tab_page_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_tab_page_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}

/* ---- ToolBar (of Widget) */
/** @brief A new ToolBar under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_tool_bar_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "ToolBar", parent);
}
static inline int bmds_tool_bar_add_separator(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "addSeparator", NULL, 0, NULL);
}

/* ---- ToolButton (of Widget) */
/** @brief A new ToolButton under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_tool_button_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "ToolButton", parent);
}
static inline int bmds_tool_button_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_tool_button_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}
static inline int bmds_tool_button_set_icon(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "icon", bmds_gui_string(value));
}
static inline int bmds_tool_button_get_icon(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "icon", out);
}
static inline int bmds_tool_button_click(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "click", NULL, 0, NULL);
}
#define BMDS_GUI_TOOL_BUTTON_CLICKED "clicked" /**< ToolButton's event (no values). */

/* ---- StatusBar (of Widget) */
/** @brief A new StatusBar under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_status_bar_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "StatusBar", parent);
}
static inline int bmds_status_bar_add_field(bmds_gui_t *gui, uint32_t id, int32_t a0, bmds_gui_value_t *result) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0)};
        return bmds_gui_call_named(gui, id, "addField", args, 1, result);
}
static inline int bmds_status_bar_set_field(bmds_gui_t *gui, uint32_t id, int32_t a0, const char *a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_string(a1)};
        return bmds_gui_call_named(gui, id, "setField", args, 2, NULL);
}

#ifdef __cplusplus
}
#endif

#endif
