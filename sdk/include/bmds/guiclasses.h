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

/* ---- ItemView (of Widget) */
static inline int bmds_item_view_set_items(bmds_gui_t *gui, uint32_t id, const char *const *value, uint32_t value_count) {
        return bmds_gui_set_named(gui, id, "items", bmds_gui_strings(gui, value, value_count));
}
static inline int bmds_item_view_get_items(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "items", out);
}
static inline int bmds_item_view_set_current(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "current", bmds_gui_int(value));
}
static inline int bmds_item_view_get_current(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "current", out);
}
static inline int bmds_item_view_set_multi_select(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "multiSelect", bmds_gui_bool(value));
}
static inline int bmds_item_view_get_multi_select(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "multiSelect", out);
}
static inline int bmds_item_view_add_item(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "addItem", args, 1, NULL);
}
static inline int bmds_item_view_remove_item(bmds_gui_t *gui, uint32_t id, int32_t a0) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0)};
        return bmds_gui_call_named(gui, id, "removeItem", args, 1, NULL);
}
static inline int bmds_item_view_clear(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "clear", NULL, 0, NULL);
}
static inline int bmds_item_view_set_icon(bmds_gui_t *gui, uint32_t id, int32_t a0, const char *a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_string(a1)};
        return bmds_gui_call_named(gui, id, "setIcon", args, 2, NULL);
}
#define BMDS_GUI_ITEM_VIEW_ACTIVATED "activated" /**< ItemView's event (Int). */
#define BMDS_GUI_ITEM_VIEW_CURRENT_CHANGED "currentChanged" /**< ItemView's event (Int). */

/* ---- ListView (of ItemView) */
/** @brief A new ListView under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_list_view_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "ListView", parent);
}

/* ---- DetailsView (of ItemView) */
/** @brief A new DetailsView under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_details_view_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "DetailsView", parent);
}
static inline int bmds_details_view_set_columns(bmds_gui_t *gui, uint32_t id, const char *const *value, uint32_t value_count) {
        return bmds_gui_set_named(gui, id, "columns", bmds_gui_strings(gui, value, value_count));
}
static inline int bmds_details_view_get_columns(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "columns", out);
}
static inline int bmds_details_view_add_row(bmds_gui_t *gui, uint32_t id, const char *const *a0, uint32_t a0_count) {
        const bmds_gui_value_t args[] = {bmds_gui_strings(gui, a0, a0_count)};
        return bmds_gui_call_named(gui, id, "addRow", args, 1, NULL);
}
static inline int bmds_details_view_set_column_width(bmds_gui_t *gui, uint32_t id, int32_t a0, int32_t a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_int(a1)};
        return bmds_gui_call_named(gui, id, "setColumnWidth", args, 2, NULL);
}
static inline int bmds_details_view_set_sort_indicator(bmds_gui_t *gui, uint32_t id, int32_t a0, int a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_bool(a1)};
        return bmds_gui_call_named(gui, id, "setSortIndicator", args, 2, NULL);
}
#define BMDS_GUI_DETAILS_VIEW_SORT_ASKED "sortAsked" /**< DetailsView's event (Int). */

/* ---- IconView (of ItemView) */
/** @brief A new IconView under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_icon_view_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "IconView", parent);
}

/* ---- TreeView (of Widget) */
/** @brief A new TreeView under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_tree_view_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "TreeView", parent);
}
static inline int bmds_tree_view_set_current(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "current", bmds_gui_string(value));
}
static inline int bmds_tree_view_get_current(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "current", out);
}
static inline int bmds_tree_view_add_root(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "addRoot", args, 1, NULL);
}
static inline int bmds_tree_view_set_children(bmds_gui_t *gui, uint32_t id, const char *a0, const char *const *a1, uint32_t a1_count) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0), bmds_gui_strings(gui, a1, a1_count)};
        return bmds_gui_call_named(gui, id, "setChildren", args, 2, NULL);
}
static inline int bmds_tree_view_set_has_children(bmds_gui_t *gui, uint32_t id, const char *a0, int a1) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0), bmds_gui_bool(a1)};
        return bmds_gui_call_named(gui, id, "setHasChildren", args, 2, NULL);
}
static inline int bmds_tree_view_expand(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "expand", args, 1, NULL);
}
static inline int bmds_tree_view_collapse(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "collapse", args, 1, NULL);
}
static inline int bmds_tree_view_clear(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "clear", NULL, 0, NULL);
}
#define BMDS_GUI_TREE_VIEW_EXPANDING "expanding" /**< TreeView's event (String). */
#define BMDS_GUI_TREE_VIEW_ACTIVATED "activated" /**< TreeView's event (String). */
#define BMDS_GUI_TREE_VIEW_CURRENT_CHANGED "currentChanged" /**< TreeView's event (String). */

/* ---- TextEdit (of Widget) */
/** @brief A new TextEdit under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_text_edit_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "TextEdit", parent);
}
static inline int bmds_text_edit_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_text_edit_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}
static inline int bmds_text_edit_set_read_only(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "readOnly", bmds_gui_bool(value));
}
static inline int bmds_text_edit_get_read_only(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "readOnly", out);
}
static inline int bmds_text_edit_get_modified(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "modified", out);
}
static inline int bmds_text_edit_get_cursor_line(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "cursorLine", out);
}
static inline int bmds_text_edit_get_cursor_column(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "cursorColumn", out);
}
static inline int bmds_text_edit_get_selected_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "selectedText", out);
}
static inline int bmds_text_edit_mark_saved(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "markSaved", NULL, 0, NULL);
}
static inline int bmds_text_edit_insert(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "insert", args, 1, NULL);
}
static inline int bmds_text_edit_go_to_line(bmds_gui_t *gui, uint32_t id, int32_t a0) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0)};
        return bmds_gui_call_named(gui, id, "goToLine", args, 1, NULL);
}
static inline int bmds_text_edit_select_all(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "selectAll", NULL, 0, NULL);
}
static inline int bmds_text_edit_undo(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "undo", NULL, 0, NULL);
}
static inline int bmds_text_edit_redo(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "redo", NULL, 0, NULL);
}
static inline int bmds_text_edit_cut(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "cut", NULL, 0, NULL);
}
static inline int bmds_text_edit_copy(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "copy", NULL, 0, NULL);
}
static inline int bmds_text_edit_paste(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "paste", NULL, 0, NULL);
}
#define BMDS_GUI_TEXT_EDIT_CHANGED "changed" /**< TextEdit's event (no values). */
#define BMDS_GUI_TEXT_EDIT_CURSOR_MOVED "cursorMoved" /**< TextEdit's event (Int, Int). */

/* ---- CodeEdit (of TextEdit) */
/** @brief A new CodeEdit under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_code_edit_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "CodeEdit", parent);
}
static inline int bmds_code_edit_set_syntax(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "syntax", bmds_gui_string(value));
}
static inline int bmds_code_edit_get_syntax(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "syntax", out);
}
static inline int bmds_code_edit_set_line_numbers(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "lineNumbers", bmds_gui_bool(value));
}
static inline int bmds_code_edit_get_line_numbers(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "lineNumbers", out);
}
static inline int bmds_code_edit_set_scheme(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "scheme", bmds_gui_string(value));
}
static inline int bmds_code_edit_get_scheme(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "scheme", out);
}

/* ---- Terminal (of Widget) */
/** @brief A new Terminal under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_terminal_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Terminal", parent);
}
static inline int bmds_terminal_get_columns(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "columns", out);
}
static inline int bmds_terminal_get_rows(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "rows", out);
}
static inline int bmds_terminal_write(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "write", args, 1, NULL);
}
static inline int bmds_terminal_clear(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "clear", NULL, 0, NULL);
}
#define BMDS_GUI_TERMINAL_TYPED "typed" /**< Terminal's event (String). */
#define BMDS_GUI_TERMINAL_SIZED "sized" /**< Terminal's event (Int, Int). */
static inline int bmds_window_set_margin(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "margin", bmds_gui_int(value));
}
static inline int bmds_window_set_spacing(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "spacing", bmds_gui_int(value));
}

/* ---- MenuBar (of Widget) */
/** @brief A new MenuBar under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_menu_bar_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "MenuBar", parent);
}

/* ---- Menu */
/** @brief A new Menu under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_menu_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Menu", parent);
}
static inline int bmds_menu_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_menu_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}
static inline int bmds_menu_add_separator(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "addSeparator", NULL, 0, NULL);
}
static inline int bmds_menu_popup(bmds_gui_t *gui, uint32_t id, int32_t a0, int32_t a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_int(a1)};
        return bmds_gui_call_named(gui, id, "popup", args, 2, NULL);
}

/* ---- MenuItem */
/** @brief A new MenuItem under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_menu_item_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "MenuItem", parent);
}
static inline int bmds_menu_item_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_menu_item_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}
static inline int bmds_menu_item_set_shortcut(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "shortcut", bmds_gui_string(value));
}
static inline int bmds_menu_item_get_shortcut(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "shortcut", out);
}
static inline int bmds_menu_item_set_checked(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "checked", bmds_gui_bool(value));
}
static inline int bmds_menu_item_get_checked(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "checked", out);
}
static inline int bmds_menu_item_set_enabled(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "enabled", bmds_gui_bool(value));
}
static inline int bmds_menu_item_get_enabled(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "enabled", out);
}
static inline int bmds_menu_item_set_icon(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "icon", bmds_gui_string(value));
}
static inline int bmds_menu_item_get_icon(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "icon", out);
}
#define BMDS_GUI_MENU_ITEM_CHOSEN "chosen" /**< MenuItem's event (no values). */

/* ---- MessageBox */
/** @brief A new MessageBox under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_message_box_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "MessageBox", parent);
}
static inline int bmds_message_box_set_kind(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "kind", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_message_box_get_kind(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "kind", out);
}
static inline int bmds_message_box_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_message_box_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}
static inline int bmds_message_box_set_text(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "text", bmds_gui_string(value));
}
static inline int bmds_message_box_get_text(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "text", out);
}
static inline int bmds_message_box_set_buttons(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "buttons", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_message_box_get_buttons(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "buttons", out);
}
static inline int bmds_message_box_open(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "open", NULL, 0, NULL);
}
#define BMDS_GUI_MESSAGE_BOX_ANSWERED "answered" /**< MessageBox's event (Atom). */

/* ---- FileDialog */
/** @brief A new FileDialog under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_file_dialog_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "FileDialog", parent);
}
static inline int bmds_file_dialog_set_mode(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "mode", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_file_dialog_get_mode(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "mode", out);
}
static inline int bmds_file_dialog_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_file_dialog_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}
static inline int bmds_file_dialog_set_folder(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "folder", bmds_gui_string(value));
}
static inline int bmds_file_dialog_get_folder(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "folder", out);
}
static inline int bmds_file_dialog_set_file_name(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "fileName", bmds_gui_string(value));
}
static inline int bmds_file_dialog_get_file_name(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "fileName", out);
}
static inline int bmds_file_dialog_add_filter(bmds_gui_t *gui, uint32_t id, const char *a0, const char *a1) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0), bmds_gui_string(a1)};
        return bmds_gui_call_named(gui, id, "addFilter", args, 2, NULL);
}
static inline int bmds_file_dialog_open(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "open", NULL, 0, NULL);
}
#define BMDS_GUI_FILE_DIALOG_FINISHED "finished" /**< FileDialog's event (Bool, List). */

/* ---- ColorDialog */
/** @brief A new ColorDialog under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_color_dialog_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "ColorDialog", parent);
}
static inline int bmds_color_dialog_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_color_dialog_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}
static inline int bmds_color_dialog_set_color(bmds_gui_t *gui, uint32_t id, uint32_t value) {
        return bmds_gui_set_named(gui, id, "color", bmds_gui_color(value));
}
static inline int bmds_color_dialog_get_color(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "color", out);
}
static inline int bmds_color_dialog_open(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "open", NULL, 0, NULL);
}
#define BMDS_GUI_COLOR_DIALOG_FINISHED "finished" /**< ColorDialog's event (Bool, Color). */
#define BMDS_GUI_COLOR_DIALOG_CHANGED "changed" /**< ColorDialog's event (Color). */

/* ---- Timer */
/** @brief A new Timer under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_timer_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Timer", parent);
}
static inline int bmds_timer_set_interval(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "interval", bmds_gui_int(value));
}
static inline int bmds_timer_get_interval(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "interval", out);
}
static inline int bmds_timer_set_repeat(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "repeat", bmds_gui_bool(value));
}
static inline int bmds_timer_get_repeat(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "repeat", out);
}
static inline int bmds_timer_get_active(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "active", out);
}
static inline int bmds_timer_start(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "start", NULL, 0, NULL);
}
static inline int bmds_timer_stop(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "stop", NULL, 0, NULL);
}
#define BMDS_GUI_TIMER_TIMEOUT "timeout" /**< Timer's event (no values). */

/* ---- Image */
/** @brief A new Image under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_image_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Image", parent);
}
static inline int bmds_image_get_width(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "width", out);
}
static inline int bmds_image_get_height(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "height", out);
}
static inline int bmds_window_set_icon_image(bmds_gui_t *gui, uint32_t id, uint32_t value) {
        return bmds_gui_set_named(gui, id, "iconImage", bmds_gui_id(value));
}
static inline int bmds_window_get_icon_image(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "iconImage", out);
}
static inline int bmds_button_set_icon_image(bmds_gui_t *gui, uint32_t id, uint32_t value) {
        return bmds_gui_set_named(gui, id, "iconImage", bmds_gui_id(value));
}
static inline int bmds_button_get_icon_image(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "iconImage", out);
}
static inline int bmds_tool_button_set_icon_image(bmds_gui_t *gui, uint32_t id, uint32_t value) {
        return bmds_gui_set_named(gui, id, "iconImage", bmds_gui_id(value));
}
static inline int bmds_tool_button_get_icon_image(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "iconImage", out);
}
static inline int bmds_menu_item_set_icon_image(bmds_gui_t *gui, uint32_t id, uint32_t value) {
        return bmds_gui_set_named(gui, id, "iconImage", bmds_gui_id(value));
}
static inline int bmds_menu_item_get_icon_image(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "iconImage", out);
}
static inline int bmds_item_view_set_icon_image(bmds_gui_t *gui, uint32_t id, int32_t a0, uint32_t a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_id(a1)};
        return bmds_gui_call_named(gui, id, "setIconImage", args, 2, NULL);
}

/* ---- Canvas (of Widget) */
/** @brief A new Canvas under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_canvas_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Canvas", parent);
}
static inline int bmds_canvas_get_width(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "width", out);
}
static inline int bmds_canvas_get_height(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "height", out);
}
static inline int bmds_canvas_set_preferred_width(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "preferredWidth", bmds_gui_int(value));
}
static inline int bmds_canvas_get_preferred_width(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "preferredWidth", out);
}
static inline int bmds_canvas_set_preferred_height(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "preferredHeight", bmds_gui_int(value));
}
static inline int bmds_canvas_get_preferred_height(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "preferredHeight", out);
}
#define BMDS_GUI_CANVAS_RESIZED "resized" /**< Canvas's event (Int, Int). */
#define BMDS_GUI_CANVAS_PRESSED "pressed" /**< Canvas's event (Int, Int, Int). */
#define BMDS_GUI_CANVAS_RELEASED "released" /**< Canvas's event (Int, Int, Int). */
#define BMDS_GUI_CANVAS_MOVED "moved" /**< Canvas's event (Int, Int, Int). */
#define BMDS_GUI_CANVAS_WHEEL "wheel" /**< Canvas's event (Int). */
#define BMDS_GUI_CANVAS_KEY "key" /**< Canvas's event (Int, Int, Bool, String). */

/* ---- Clipboard */
/** @brief A new Clipboard under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_clipboard_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Clipboard", parent);
}
static inline int bmds_clipboard_copy_text(bmds_gui_t *gui, uint32_t id, const char *a0, const char *a1) {
        const bmds_gui_value_t args[] = {bmds_gui_atom_value(bmds_gui_atom(gui, a0)), bmds_gui_string(a1)};
        return bmds_gui_call_named(gui, id, "copyText", args, 2, NULL);
}
static inline int bmds_clipboard_offer(bmds_gui_t *gui, uint32_t id, const char *a0, const char *const *a1, uint32_t a1_count) {
        const bmds_gui_value_t args[] = {bmds_gui_atom_value(bmds_gui_atom(gui, a0)), bmds_gui_strings(gui, a1, a1_count)};
        return bmds_gui_call_named(gui, id, "offer", args, 2, NULL);
}
static inline int bmds_clipboard_answer(bmds_gui_t *gui, uint32_t id, int32_t a0, const void *a1, uint32_t a1_size) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_bytes(a1, a1_size)};
        return bmds_gui_call_named(gui, id, "answer", args, 2, NULL);
}
static inline int bmds_clipboard_finish(bmds_gui_t *gui, uint32_t id, int32_t a0) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0)};
        return bmds_gui_call_named(gui, id, "finish", args, 1, NULL);
}
static inline int bmds_clipboard_clear(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_atom_value(bmds_gui_atom(gui, a0))};
        return bmds_gui_call_named(gui, id, "clear", args, 1, NULL);
}
static inline int bmds_clipboard_paste(bmds_gui_t *gui, uint32_t id, const char *a0, const char *a1) {
        const bmds_gui_value_t args[] = {bmds_gui_atom_value(bmds_gui_atom(gui, a0)), bmds_gui_string(a1)};
        return bmds_gui_call_named(gui, id, "paste", args, 2, NULL);
}
static inline int bmds_clipboard_types(bmds_gui_t *gui, uint32_t id, const char *a0, bmds_gui_value_t *result) {
        const bmds_gui_value_t args[] = {bmds_gui_atom_value(bmds_gui_atom(gui, a0))};
        return bmds_gui_call_named(gui, id, "types", args, 1, result);
}
static inline int bmds_clipboard_owns(bmds_gui_t *gui, uint32_t id, const char *a0, bmds_gui_value_t *result) {
        const bmds_gui_value_t args[] = {bmds_gui_atom_value(bmds_gui_atom(gui, a0))};
        return bmds_gui_call_named(gui, id, "owns", args, 1, result);
}
#define BMDS_GUI_CLIPBOARD_REQUESTED "requested" /**< Clipboard's event (Int, Atom, String). */
#define BMDS_GUI_CLIPBOARD_LOST "lost" /**< Clipboard's event (Atom). */
#define BMDS_GUI_CLIPBOARD_PASTED "pasted" /**< Clipboard's event (Atom, String, Bytes). */
#define BMDS_GUI_CLIPBOARD_PASTE_FAILED "pasteFailed" /**< Clipboard's event (Atom, String, Atom). */
static inline int bmds_widget_set_drop_types(bmds_gui_t *gui, uint32_t id, const char *const *value, uint32_t value_count) {
        return bmds_gui_set_named(gui, id, "dropTypes", bmds_gui_strings(gui, value, value_count));
}
static inline int bmds_widget_get_drop_types(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "dropTypes", out);
}
#define BMDS_GUI_WIDGET_DROPPED "dropped" /**< Widget's event (String, Bytes, Int, Int). */

/* ---- Notification */
/** @brief A new Notification under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_notification_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Notification", parent);
}
static inline int bmds_notification_set_title(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "title", bmds_gui_string(value));
}
static inline int bmds_notification_get_title(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "title", out);
}
static inline int bmds_notification_set_body(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "body", bmds_gui_string(value));
}
static inline int bmds_notification_get_body(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "body", out);
}
static inline int bmds_notification_set_icon(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "icon", bmds_gui_string(value));
}
static inline int bmds_notification_get_icon(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "icon", out);
}
static inline int bmds_notification_set_app(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "app", bmds_gui_string(value));
}
static inline int bmds_notification_get_app(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "app", out);
}
static inline int bmds_notification_set_urgency(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "urgency", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_notification_get_urgency(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "urgency", out);
}
static inline int bmds_notification_set_timeout(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "timeout", bmds_gui_int(value));
}
static inline int bmds_notification_get_timeout(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "timeout", out);
}
static inline int bmds_notification_set_actions(bmds_gui_t *gui, uint32_t id, const char *const *value, uint32_t value_count) {
        return bmds_gui_set_named(gui, id, "actions", bmds_gui_strings(gui, value, value_count));
}
static inline int bmds_notification_get_actions(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "actions", out);
}
static inline int bmds_notification_set_silent(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "silent", bmds_gui_bool(value));
}
static inline int bmds_notification_get_silent(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "silent", out);
}
static inline int bmds_notification_set_transient(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "transient", bmds_gui_bool(value));
}
static inline int bmds_notification_get_transient(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "transient", out);
}
static inline int bmds_notification_show(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "show", NULL, 0, NULL);
}
static inline int bmds_notification_close(bmds_gui_t *gui, uint32_t id) {
        return bmds_gui_call_named(gui, id, "close", NULL, 0, NULL);
}
#define BMDS_GUI_NOTIFICATION_ACTIVATED "activated" /**< Notification's event (Int). */
#define BMDS_GUI_NOTIFICATION_DISMISSED "dismissed" /**< Notification's event (no values). */
#define BMDS_GUI_NOTIFICATION_EXPIRED "expired" /**< Notification's event (no values). */

/* ---- TrayIcon */
/** @brief A new TrayIcon under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_tray_icon_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "TrayIcon", parent);
}
static inline int bmds_tray_icon_set_icon(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "icon", bmds_gui_string(value));
}
static inline int bmds_tray_icon_get_icon(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "icon", out);
}
static inline int bmds_tray_icon_set_icon_image(bmds_gui_t *gui, uint32_t id, uint32_t value) {
        return bmds_gui_set_named(gui, id, "iconImage", bmds_gui_id(value));
}
static inline int bmds_tray_icon_get_icon_image(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "iconImage", out);
}
static inline int bmds_tray_icon_set_tool_tip(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "toolTip", bmds_gui_string(value));
}
static inline int bmds_tray_icon_get_tool_tip(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "toolTip", out);
}
static inline int bmds_tray_icon_set_category(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "category", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_tray_icon_get_category(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "category", out);
}
static inline int bmds_tray_icon_set_visible(bmds_gui_t *gui, uint32_t id, int value) {
        return bmds_gui_set_named(gui, id, "visible", bmds_gui_bool(value));
}
static inline int bmds_tray_icon_get_visible(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "visible", out);
}
static inline int bmds_tray_icon_get_available(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "available", out);
}
#define BMDS_GUI_TRAY_ICON_ACTIVATED "activated" /**< TrayIcon's event (Atom). */
#define BMDS_GUI_TRAY_ICON_SCROLLED "scrolled" /**< TrayIcon's event (Int). */
#define BMDS_GUI_TRAY_ICON_AVAILABILITY_CHANGED "availabilityChanged" /**< TrayIcon's event (Bool). */

/* ---- Services */
/** @brief A new Services under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_services_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Services", parent);
}
static inline int bmds_services_register(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "register", args, 1, NULL);
}
static inline int bmds_services_unregister(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "unregister", args, 1, NULL);
}
static inline int bmds_services_send(bmds_gui_t *gui, uint32_t id, const char *a0, const void *a1, uint32_t a1_size) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0), bmds_gui_bytes(a1, a1_size)};
        return bmds_gui_call_named(gui, id, "send", args, 2, NULL);
}
static inline int bmds_services_has(bmds_gui_t *gui, uint32_t id, const char *a0, bmds_gui_value_t *result) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "has", args, 1, result);
}
static inline int bmds_services_watch(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "watch", args, 1, NULL);
}
static inline int bmds_services_unwatch(bmds_gui_t *gui, uint32_t id, const char *a0) {
        const bmds_gui_value_t args[] = {bmds_gui_string(a0)};
        return bmds_gui_call_named(gui, id, "unwatch", args, 1, NULL);
}
#define BMDS_GUI_SERVICES_MESSAGE "message" /**< Services's event (String, Bytes). */
#define BMDS_GUI_SERVICES_CHANGED "changed" /**< Services's event (String, Bool). */
static inline int bmds_window_set_level(bmds_gui_t *gui, uint32_t id, const char *value) {
        return bmds_gui_set_named(gui, id, "level", bmds_gui_atom_value(bmds_gui_atom(gui, value)));
}
static inline int bmds_window_get_level(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "level", out);
}
static inline int bmds_window_set_reserve(bmds_gui_t *gui, uint32_t id, int32_t value) {
        return bmds_gui_set_named(gui, id, "reserve", bmds_gui_int(value));
}
static inline int bmds_window_get_reserve(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "reserve", out);
}

/* ---- Shell */
/** @brief A new Shell under @p parent (0: none): its id, or 0. */
static inline uint32_t bmds_shell_new(bmds_gui_t *gui, uint32_t parent) {
        return bmds_gui_new(gui, "Shell", parent);
}
static inline int bmds_shell_get_work_area(bmds_gui_t *gui, uint32_t id, bmds_gui_value_t *out) {
        return bmds_gui_get_named(gui, id, "workArea", out);
}
static inline int bmds_shell_act(bmds_gui_t *gui, uint32_t id, int32_t a0, const char *a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_atom_value(bmds_gui_atom(gui, a1))};
        return bmds_gui_call_named(gui, id, "act", args, 2, NULL);
}
static inline int bmds_shell_show_window_menu(bmds_gui_t *gui, uint32_t id, int32_t a0, int32_t a1, int32_t a2) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_int(a1), bmds_gui_int(a2)};
        return bmds_gui_call_named(gui, id, "showWindowMenu", args, 3, NULL);
}
static inline int bmds_shell_copy_icon(bmds_gui_t *gui, uint32_t id, int32_t a0, uint32_t a1, bmds_gui_value_t *result) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_id(a1)};
        return bmds_gui_call_named(gui, id, "copyIcon", args, 2, result);
}
static inline int bmds_shell_grab_key(bmds_gui_t *gui, uint32_t id, int32_t a0, int32_t a1) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0), bmds_gui_int(a1)};
        return bmds_gui_call_named(gui, id, "grabKey", args, 2, NULL);
}
static inline int bmds_shell_program_starting(bmds_gui_t *gui, uint32_t id, int32_t a0) {
        const bmds_gui_value_t args[] = {bmds_gui_int(a0)};
        return bmds_gui_call_named(gui, id, "programStarting", args, 1, NULL);
}
#define BMDS_GUI_SHELL_WINDOW_CHANGED "windowChanged" /**< Shell's event (Int, String, List). */
#define BMDS_GUI_SHELL_WINDOW_CLOSED "windowClosed" /**< Shell's event (Int). */
#define BMDS_GUI_SHELL_WORK_AREA_CHANGED "workAreaChanged" /**< Shell's event (Rect). */
#define BMDS_GUI_SHELL_KEY_GRABBED "keyGrabbed" /**< Shell's event (Int, Int). */

#ifdef __cplusplus
}
#endif

#endif
