/**
 * @file      game.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame (devplan/games.md §4, §13): what games share, on the surface calls. A static
 * library (libbmdsgame.a, linked by bmds_add_program and bmds-cc): no system calls of its own and
 * no ABI promise; a game keeps the version it was built with.
 *
 * - A canvas (`bmds_canvas_t`) is a picture to draw on: one of a surface's buffers, or any
 *   pixels. Drawing is clipped, and records what it changed (its damage).
 * - The frame loop (`bmds_game_run()`) opens the surface, calls the game's callbacks (events, a
 *   fixed-rate tick, drawing) and presents what was drawn. A frame starts as the last one shown,
 *   so a game draws only what changes, and only that is presented.
 *
 * Pixel values are the canvas's own (`bmds_canvas_pixel()` makes one from 0xRRGGBB, once): an
 * RGB565 value, B, G, R bytes as 0xRRGGBB, or an L8 index.
 *
 * Not thread-safe: a canvas and a game are one thread's.
 */

#ifndef BMDS_GAME_H
#define BMDS_GAME_H

#include <stddef.h>
#include <stdint.h>
#include <bmds/sys.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Rectangles of damage a canvas keeps (merged where they meet; more: their bounds). */
#define BMDS_CANVAS_MAX_DAMAGE 16u

/** @brief A glyph's size, at scale 1. */
#define BMDS_GAME_GLYPH_WIDTH 8
#define BMDS_GAME_GLYPH_HEIGHT 16

/** @brief A picture to draw on. Its fields are read freely; changed only through the calls. */
typedef struct bmds_canvas_t {
                uint8_t    *pixels; /**< The top-left pixel. */
                uint32_t    stride; /**< Bytes from a row to the next. */
                int32_t     width;
                int32_t     height;
                uint32_t    format;                         /**< `BMDS_PIXEL_L8`, `_RGB565` or `_BGR24`. */
                bmds_rect_t clip;                           /**< Drawing stays inside it. */
                uint32_t    damage_count;                   /**< Rectangles in `damage`. */
                bmds_rect_t damage[BMDS_CANVAS_MAX_DAMAGE]; /**< What drawing changed. */
} bmds_canvas_t;

/** @brief `bmds_game_image_t::flags`: pixels equal to `key` aren't drawn. */
#define BMDS_GAME_IMAGE_KEYED 1u

/** @brief An image to blit: its pixels in a canvas's format (a sprite sheet, tiles, a picture). */
typedef struct bmds_game_image_t {
                const uint8_t *pixels;
                uint32_t       stride; /**< Bytes from a row to the next. */
                int32_t        width;
                int32_t        height;
                uint32_t       format; /**< As the canvas it is drawn on. */
                uint32_t       flags;  /**< `BMDS_GAME_IMAGE_KEYED`. */
                uint32_t       key;    /**< The transparent pixel value, with `BMDS_GAME_IMAGE_KEYED`. */
} bmds_game_image_t;

/** @brief `bmds_game_load_t::flags`: pixels of `key_rgb` in the file are transparent too. */
#define BMDS_GAME_LOAD_KEY_RGB 1u

/**
 * @brief How to load an image (`bmds_game_image_load()`): into which format, what is transparent.
 *        Pixels whose alpha is below one half are transparent; if any are, the image is keyed.
 */
typedef struct bmds_game_load_t {
                uint32_t        size;    /**< `sizeof(bmds_game_load_t)`. */
                uint32_t        format;  /**< The canvas's: `BMDS_PIXEL_L8`, `_RGB565` or `_BGR24`. */
                uint32_t        flags;   /**< `BMDS_GAME_LOAD_KEY_RGB`. */
                uint32_t        key_rgb; /**< 0xRRGGBB: with `BMDS_GAME_LOAD_KEY_RGB`, transparent. */
                const uint32_t *palette; /**< L8: the colours (0xRRGGBB) of indices 0 on: each pixel the nearest. */
                uint32_t        palette_count; /**< L8: 1-256. */
                uint32_t        key;           /**< L8: the index transparent pixels become (no colour becomes it). */
                uint32_t        mem_flags; /**< `bmds_mem_alloc()`'s (`BMDS_MEM_FAST`: internal RAM, faster blits). */
} bmds_game_load_t;

/**
 * @brief Loads the picture at @p path (PNG, JPEG or BMP: `bmds_image_decode()`'s) into @p image,
 *        in @p how's format. RGB565 and BGR24 key their transparent pixels with a value no opaque
 *        pixel keeps (magenta's; such a pixel is moved a step of green from it).
 * @return 0, or a negative error (`-BMDS_EINVAL`: @p how; `-BMDS_ENOMEM`; the open's and the
 *         decode's errors). Free @p image with `bmds_game_image_free()`.
 */
int bmds_game_image_load(const char *path, const bmds_game_load_t *how, bmds_game_image_t *image);

/** @brief Loads a picture from @p length bytes at @p data (one built in), as `bmds_game_image_load()`. */
int bmds_game_image_load_memory(const void *data, uint32_t length, const bmds_game_load_t *how,
                                bmds_game_image_t *image);

/** @brief Frees a loaded image's pixels and empties it (an empty one: nothing). */
void bmds_game_image_free(bmds_game_image_t *image);

/**
 * @brief Makes @p canvas a picture of @p width × @p height pixels of @p format at @p pixels,
 *        @p stride bytes a row: clipped to all of it, no damage.
 * @return 0, or -1 (a format or size it can't draw).
 */
int bmds_canvas_init(bmds_canvas_t *canvas, void *pixels, uint32_t stride, int32_t width, int32_t height,
                     uint32_t format);

/** @brief Returns the canvas's pixel value for 0xRRGGBB (L8: @p rgb is the index). */
uint32_t bmds_canvas_pixel(const bmds_canvas_t *canvas, uint32_t rgb);

/** @brief Clips drawing to @p rect (cut to the canvas), or to all of it (NULL). */
void bmds_canvas_set_clip(bmds_canvas_t *canvas, const bmds_rect_t *rect);

/** @brief Fills a rectangle. */
void bmds_canvas_fill(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t pixel);

/** @brief Fills the whole clip. */
void bmds_canvas_clear(bmds_canvas_t *canvas, uint32_t pixel);

/** @brief Draws a rectangle's outline, one pixel wide. */
void bmds_canvas_rect(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t pixel);

/** @brief Draws a line from (@p x0, @p y0) to (@p x1, @p y1), both ends included. */
void bmds_canvas_line(bmds_canvas_t *canvas, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t pixel);

/** @brief Sets one pixel. */
void bmds_canvas_plot(bmds_canvas_t *canvas, int32_t x, int32_t y, uint32_t pixel);

/**
 * @brief Draws @p source of @p image (NULL: all of it) with its top-left at (@p x, @p y),
 *        leaving out its key's pixels if it has one. Nothing if the formats differ.
 */
void bmds_canvas_blit(bmds_canvas_t *canvas, int32_t x, int32_t y, const bmds_game_image_t *image,
                      const bmds_rect_t *source);

/**
 * @brief Draws UTF-8 @p text's characters in @p pixel (the background left as it is) from
 *        (@p x, @p y), each glyph @p scale times its size (1 or more): ASCII and Latin-1, `?`
 *        for others. A newline starts a line below at @p x.
 * @return The widest line's width in pixels.
 */
int32_t bmds_canvas_text(bmds_canvas_t *canvas, int32_t x, int32_t y, const char *text, uint32_t pixel, int32_t scale);

/** @brief Returns the widest line of @p text's width at @p scale, in pixels. */
int32_t bmds_game_text_width(const char *text, int32_t scale);

/** @brief Adds @p rect (cut to the canvas) to the damage, as drawing does. */
void bmds_canvas_damage(bmds_canvas_t *canvas, const bmds_rect_t *rect);

/** @brief Makes the whole canvas the damage. */
void bmds_canvas_damage_all(bmds_canvas_t *canvas);

/** @brief Forgets the damage. */
void bmds_canvas_damage_clear(bmds_canvas_t *canvas);

/**
 * @brief Copies @p count rectangles of @p from into the same places of @p to (the same size and
 *        format), recording no damage: how the frame loop brings a buffer up to date.
 */
void bmds_canvas_copy(bmds_canvas_t *to, const bmds_canvas_t *from, const bmds_rect_t *rects, uint32_t count);

/** @brief A running game (`bmds_game_run()`'s, given to its callbacks). */
typedef struct bmds_game_t bmds_game_t;

/** @brief What `bmds_game_run()` runs. */
typedef struct bmds_game_config_t {
                uint32_t    size;  /**< `sizeof(bmds_game_config_t)`. */
                uint32_t    width; /**< The picture's (a multiple of 4). */
                uint32_t    height;
                uint32_t    format;   /**< `BMDS_PIXEL_L8`, `_RGB565` or `_BGR24`. */
                uint32_t    buffers;  /**< 2 or 3 (0: 2). */
                uint32_t    flags;    /**< `BMDS_SURFACE_...` (FULL_SCREEN: suggested). */
                uint32_t    aspect_x; /**< The picture's shape (0, 0: square pixels). */
                uint32_t    aspect_y;
                const char *title;   /**< NULL: the program's name. */
                uint32_t    tick_hz; /**< Ticks a second (0: 60). */
                void       *arg;     /**< The callbacks' (`bmds_game_arg()`). */
                /** @brief An event (keys, the pointer, ACTIVE, CLOSE...); NULL: CLOSE quits. */
                void (*event)(bmds_game_t *game, const bmds_surface_event_t *event);
                /** @brief The game's state a tick on (while active). */
                void (*tick)(bmds_game_t *game);
                /** @brief Draws on @p canvas, which holds the last frame shown: only what changed. */
                void (*draw)(bmds_game_t *game, bmds_canvas_t *canvas);
} bmds_game_config_t;

/**
 * @brief Runs a game: opens its surface and loops (events, ticks, drawing, presents) until
 *        `bmds_game_quit()`. While the surface isn't active, no ticks run and nothing is drawn.
 * @return 0, or a negative error (`-BMDS_EINVAL`, `-BMDS_ENODEV`: no desktop, or the surface
 *         calls' errors).
 */
int bmds_game_run(const bmds_game_config_t *config);

/** @brief Returns the config's `arg`. */
void *bmds_game_arg(const bmds_game_t *game);

/** @brief Ends the loop after this callback. */
void bmds_game_quit(bmds_game_t *game);

/** @brief Asks for a frame (`draw`) without a tick. */
void bmds_game_redraw(bmds_game_t *game);

/** @brief Returns the surface's descriptor (for `BMDS_SURFACE_...` controls: the palette, the pointer). */
int32_t bmds_game_fd(const bmds_game_t *game);

/** @brief Returns the ticks run so far. */
uint64_t bmds_game_ticks(const bmds_game_t *game);

/** @brief Returns true while the surface is active (focused). */
int bmds_game_is_active(const bmds_game_t *game);

#ifdef __cplusplus
}
#endif

#endif /* BMDS_GAME_H */
