/**
 * @file      gui.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgui (devplan/guiprograms.md §4a, §8): a program's side of the GUI protocol. A
 * connection to the GUI service (the board's own, or another machine's through a stream),
 * objects made by id (windows, widgets), their properties set and read by name, methods called,
 * and events read. Requests are buffered and sent in batches: only those that answer (`get`,
 * `call` with a result, `sync`, names) wait for the service.
 *
 * Names (classes, properties, methods, events, enumerations' values) are atoms: numbers the
 * service gives for strings, cached here (`bmds_gui_atom`, `bmds_gui_atoms` for many in one
 * round trip).
 *
 * Errors the service reports for a request come later than the call that made it (requests
 * don't wait): they go to the error handler (`bmds_gui_on_error`; the default prints them to
 * stderr), and `bmds_gui_sync()` makes sure every error so far has come.
 *
 * Not thread-safe: one connection, one thread (or the caller's lock).
 */

#ifndef BMDS_GUI_H
#define BMDS_GUI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A value's tag (the protocol's). */
#define BMDS_GUI_NONE 0u
#define BMDS_GUI_INT 1u    /**< int32 (`number`). */
#define BMDS_GUI_LONG 2u   /**< int64 (`number`). */
#define BMDS_GUI_BOOL 3u   /**< 0 or 1 (`number`). */
#define BMDS_GUI_STRING 4u /**< UTF-8 (`text`, `length`). */
#define BMDS_GUI_ATOM 5u   /**< A name's number (`number`). */
#define BMDS_GUI_ID 6u     /**< An object (`number`). */
#define BMDS_GUI_RECT 7u   /**< `x`, `y`, `width`, `height`. */
#define BMDS_GUI_COLOR 8u  /**< 0xRRGGBB (`number`), or a palette role's atom (`role`). */
#define BMDS_GUI_LIST 9u   /**< `elements` (their tag), `count`, `items` (read with `bmds_gui_list_item`). */
#define BMDS_GUI_BYTES 10u /**< Any bytes (`text`, `length`): a clipboard's data (appended 2026-10-22). */

/** @brief A value: what a property holds, an argument, a result, an event's datum. */
typedef struct bmds_gui_value_t {
                uint32_t    tag;
                int64_t     number;
                uint32_t    role;
                const char *text; /**< Not NUL-terminated on reads: `length` bytes. */
                uint32_t    length;
                int32_t     x, y;
                uint32_t    width, height;
                uint32_t    elements;
                uint32_t    count;
                const void *items;
                uint32_t    items_size;
} bmds_gui_value_t;

/** @brief A property and its value (`create`, `set`). */
typedef struct bmds_gui_prop_t {
                uint32_t         name; /**< An atom. */
                bmds_gui_value_t value;
} bmds_gui_prop_t;

/** @brief Values an event carries at most (more are dropped here). */
#define BMDS_GUI_EVENT_VALUES 8u

/** @brief An event: valid (its strings too) until the next call on the connection. */
typedef struct bmds_gui_event_t {
                uint32_t         name;    /**< The event's atom (`clicked`, `accepted`...). */
                uint32_t         source;  /**< The object's id. */
                uint32_t         time_ms; /**< The service's clock. */
                uint32_t         count;   /**< Values in `values`. */
                bmds_gui_value_t values[BMDS_GUI_EVENT_VALUES];
} bmds_gui_event_t;

/** @brief An error the service reported. */
typedef struct bmds_gui_error_t {
                uint32_t sequence; /**< The request's number (the first after connecting is 1). */
                uint32_t opcode;   /**< The request's. */
                uint32_t code;     /**< `BMDS_GUI_BAD_...`. */
                uint32_t bad;      /**< The id or atom at fault, or 0. */
} bmds_gui_error_t;

/** @brief Error codes (the protocol's). */
#define BMDS_GUI_BAD_REQUEST 1u
#define BMDS_GUI_BAD_LENGTH 2u
#define BMDS_GUI_BAD_ID 3u
#define BMDS_GUI_BAD_NAME 4u
#define BMDS_GUI_BAD_CLASS 5u
#define BMDS_GUI_BAD_VALUE 6u
#define BMDS_GUI_BAD_ACCESS 7u
#define BMDS_GUI_BAD_MATCH 8u
#define BMDS_GUI_NO_MEM 9u
#define BMDS_GUI_BAD_IMPLEMENTATION 10u

/** @brief Told of an error (on the caller's thread, inside whatever call read it). */
typedef void (*bmds_gui_error_fn)(void *arg, const bmds_gui_error_t *error);

/** @brief A connection. */
typedef struct bmds_gui bmds_gui_t;

/**
 * @brief Connects to @p display: NULL for `BMDS_DISPLAY`'s, else "local" (the board's own
 *        service). Other machines' come with step 8 (devplan/guiprograms.md §4g).
 * @return The connection, or NULL with `errno` set.
 */
bmds_gui_t *bmds_gui_connect(const char *display);

/**
 * @brief Speaks the protocol on @p fd, already connected to a service (a socket, a test's
 *        socketpair); the connection closes it at the end.
 * @return The connection, or NULL with `errno` set (`EPROTO`: not a GUI service, or refused).
 */
bmds_gui_t *bmds_gui_connect_fd(int fd);

/** @brief Sends what is buffered, closes the connection; its objects and windows go. */
void bmds_gui_close(bmds_gui_t *gui);

/** @brief Returns the connection's fd, for `poll()` beside others (events are read with `bmds_gui_next_event`). */
int bmds_gui_fd(const bmds_gui_t *gui);

/** @brief Sends what is buffered. @return 0, or -1 with `errno`. */
int bmds_gui_flush(bmds_gui_t *gui);

/** @brief Has @p fn told of errors (NULL: the default, printing to stderr). */
void bmds_gui_on_error(bmds_gui_t *gui, bmds_gui_error_fn fn, void *arg);

/** @brief Returns an error code's name ("BadId"). */
const char *bmds_gui_error_name(uint32_t code);

/** @brief Returns @p name's atom (cached; a round trip the first time), or 0 with `errno`. */
uint32_t bmds_gui_atom(bmds_gui_t *gui, const char *name);

/** @brief Fills @p out with the atoms of @p count names, in one round trip for those not cached. @return 0 or -1. */
int bmds_gui_atoms(bmds_gui_t *gui, const char *const *names, uint32_t count, uint32_t *out);

/**
 * @brief Returns @p atom's name (cached for the connection's life; a round trip the first time),
 *        or NULL with `errno`.
 */
const char *bmds_gui_atom_name(bmds_gui_t *gui, uint32_t atom);

/** @brief Returns a new object id, or 0 (all used). */
uint32_t bmds_gui_new_id(bmds_gui_t *gui);

/**
 * @brief Makes object @p id of class @p cls (an atom) under @p parent (0: none; a window's or a
 *        box's id for a widget), its properties set at once.
 * @return 0, or -1 with `errno` (the connection broke); the service's own errors come later.
 */
int bmds_gui_create(bmds_gui_t *gui, uint32_t id, uint32_t cls, uint32_t parent, const bmds_gui_prop_t *props,
                    uint32_t count);

/** @brief Destroys object @p id and its children. @return 0 or -1. */
int bmds_gui_destroy(bmds_gui_t *gui, uint32_t id);

/** @brief Sets @p count properties of @p id. @return 0 or -1. */
int bmds_gui_set(bmds_gui_t *gui, uint32_t id, const bmds_gui_prop_t *props, uint32_t count);

/**
 * @brief Reads @p count properties of @p id into @p out (strings valid until the next call).
 * @return 0, or -1 with `errno` (`EINVAL`: the service refused, its error told too).
 */
int bmds_gui_get(bmds_gui_t *gui, uint32_t id, const uint32_t *names, uint32_t count, bmds_gui_value_t *out);

/**
 * @brief Calls @p method of @p id with @p count arguments; with @p result, waits for its result
 *        (valid until the next call), else doesn't wait.
 * @return 0, or -1 with `errno`.
 */
int bmds_gui_call(bmds_gui_t *gui, uint32_t id, uint32_t method, const bmds_gui_value_t *args, uint32_t count,
                  bmds_gui_value_t *result);

/** @brief Turns @p count of @p id's events on (@p on) or off. @return 0 or -1. */
int bmds_gui_listen(bmds_gui_t *gui, uint32_t id, const uint32_t *events, uint32_t count, int on);

/** @brief Waits until the service has done every request so far (their errors told). @return 0 or -1. */
int bmds_gui_sync(bmds_gui_t *gui);

/** @brief Returns 1 if the service named extension @p name at setup (`CANVAS-SURFACE`), else 0. */
int bmds_gui_has_extension(const bmds_gui_t *gui, const char *name);

struct bmds_surface_config_t;

/**
 * @brief Gives canvas @p canvas a surface (the `CANVAS-SURFACE` extension, local connections
 *        only): an fd for the surface calls a game uses (`bmds/sys.h`'s `bmds_surface_buffer`,
 *        `bmds_surface_acquire`, `bmds_surface_present`, and `bmds_surface_control`'s images,
 *        blits, frame waits and info). The canvas shows the newest presented buffer at its top
 *        left, at its own size; its events still come here. Waits for the service to run what
 *        was sent before (`bmds_gui_sync`), so a canvas just made has its size.
 *
 * @p config is `bmds_surface_config_t` (NULL: the canvas's size, RGB565, 2 buffers): `width`
 * and `height` (both 0: the canvas's size now), `format` (`BMDS_PIXEL_RGB565` or
 * `BMDS_PIXEL_BGR24`), `buffers` (2 or 3), `flags` (`BMDS_SURFACE_FAST` or 0). On the canvas's
 * `resized` event, a program wanting its new size asks again and closes the old fd.
 * @return The fd (close-on-exec), or -1 with `errno`: `ENOTSUP` (a remote connection),
 *         `ENOENT` (no such object), `EINVAL` (not a canvas, a field), `EAGAIN` (the canvas has
 *         no size yet: its window not shown), `ENOMEM`, `ENOSYS`.
 */
int bmds_gui_canvas_surface_open(bmds_gui_t *gui, uint32_t canvas, const struct bmds_surface_config_t *config);

/**
 * @brief The next event, waiting at most @p timeout_ms (-1: for ever; 0: not at all). Sends what
 *        is buffered first.
 * @return 1 (an event in @p out), 0 (none in time), or -1 with `errno` (`EPIPE`: the service
 *         closed the connection).
 */
int bmds_gui_next_event(bmds_gui_t *gui, bmds_gui_event_t *out, int timeout_ms);

/** @brief Reads the display's property @p name from the setup (`screen`, `workArea`). @return 0, or -1 (`ENOENT`). */
int bmds_gui_display(const bmds_gui_t *gui, const char *name, bmds_gui_value_t *out);

/** @brief Reads element @p index of list @p list (of `list->elements`). @return 0, or -1 (`ERANGE`). */
int bmds_gui_list_item(const bmds_gui_value_t *list, uint32_t index, bmds_gui_value_t *out);

/**
 * @brief Makes an object of class @p cls (its name) under @p parent: a new id, created.
 * @return The id, or 0 with `errno` (no ids left, the connection broke, a name).
 */
uint32_t bmds_gui_new(bmds_gui_t *gui, const char *cls, uint32_t parent);

/** @brief Sets @p id's property @p name (its name, interned once) to @p value. @return 0 or -1. */
int bmds_gui_set_named(bmds_gui_t *gui, uint32_t id, const char *name, bmds_gui_value_t value);

/** @brief Reads @p id's property @p name into @p out (valid until the next call). @return 0 or -1. */
int bmds_gui_get_named(bmds_gui_t *gui, uint32_t id, const char *name, bmds_gui_value_t *out);

/** @brief Calls @p id's method @p name (its name) as `bmds_gui_call`. @return 0 or -1. */
int bmds_gui_call_named(bmds_gui_t *gui, uint32_t id, const char *name, const bmds_gui_value_t *args, uint32_t count,
                        bmds_gui_value_t *result);

/**
 * @brief Sends image object @p id's pixels (an `Image`, made first): @p width × @p height, at most
 *        512 each, ARGB8888 (0xAARRGGBB, row by row), in pieces under the largest message.
 *        Sent again, the image changes; at another size, it is a new picture (the old one stays
 *        where it is shown).
 * @return 0, or -1 with `errno` (the service's own errors come later).
 */
int bmds_gui_put_image(bmds_gui_t *gui, uint32_t id, uint32_t width, uint32_t height, const uint32_t *argb);

/**
 * @brief Draws into canvas @p canvas (a `Canvas`): the @p width × @p height pixels at
 *        @p rgb565 (RGB565, @p stride pixels a row) put at (@p x, @p y) of its picture. Sent
 *        run-length coded when that is smaller, in bands under the largest message. A part
 *        outside the canvas's picture (drawn for a size it no longer has) is dropped: draw again
 *        on its `resized` event.
 * @return 0, or -1 with `errno` (the service's own errors come later).
 */
int bmds_gui_put_pixels(bmds_gui_t *gui, uint32_t canvas, int32_t x, int32_t y, uint32_t width, uint32_t height,
                        const uint16_t *rgb565, uint32_t stride);

/**
 * @brief Asks what class @p cls (an atom) has (`Describe`, devplan/guiprograms.md §10): its
 *        reply's body in @p body (valid until the next call on the connection), little-endian
 *        uint32 fields:
 *        - the parent class's atom (0: none), then 1 if it can be made (0: a base);
 *        - its own properties: a count, then each one's atom, tag and flags (1 readable,
 *          2 writable);
 *        - its methods: a count, then each one's atom, result tag, argument count and tags;
 *        - its events: a count, then each one's atom, 1 if sent by default, value count and tags.
 *        A parent's entries are described by asking for the parent.
 * @return 0, or -1 with `errno` (`EINVAL`: no such class, the error handler told).
 */
int bmds_gui_describe(bmds_gui_t *gui, uint32_t cls, const uint8_t **body, uint32_t *length);

/** @brief Returns true if @p event is the one named @p name (`clicked`). */
int bmds_gui_event_is(bmds_gui_t *gui, const bmds_gui_event_t *event, const char *name);

/**
 * @brief A list of @p count strings as a value: encoded into the connection's scratch, valid
 *        until the next call that takes a list.
 */
bmds_gui_value_t bmds_gui_strings(bmds_gui_t *gui, const char *const *items, uint32_t count);

/** @brief Values to pass: a 64-bit integer, a rectangle, a colour (0xRRGGBB). */
bmds_gui_value_t bmds_gui_long(int64_t v);
bmds_gui_value_t bmds_gui_rect(int32_t x, int32_t y, uint32_t width, uint32_t height);
bmds_gui_value_t bmds_gui_color(uint32_t rgb);

/** @brief Values to pass: an integer, a boolean, a string (NUL-terminated), an atom, an id. */
bmds_gui_value_t bmds_gui_int(int32_t v);
bmds_gui_value_t bmds_gui_bool(int v);
bmds_gui_value_t bmds_gui_string(const char *text);
bmds_gui_value_t bmds_gui_atom_value(uint32_t atom);
bmds_gui_value_t bmds_gui_id(uint32_t id);

/** @brief A value of @p size bytes at @p data (`BMDS_GUI_BYTES`: any bytes, a clipboard's data). */
bmds_gui_value_t bmds_gui_bytes(const void *data, uint32_t size);

#ifdef __cplusplus
}
#endif

#endif
