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
 * - Sound (`bmds_game_audio_open()`): effects mixed on a real-time audio thread of the library's,
 *   started by the game's thread without waiting; music decoded and played on a thread of its own.
 *
 * Pixel values are the canvas's own (`bmds_canvas_pixel()` makes one from 0xRRGGBB, once): an
 * RGB565 value, B, G, R bytes as 0xRRGGBB, or an L8 index.
 *
 * Not thread-safe: a canvas, a game and an audio are one thread's (the library's own threads
 * aside).
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

/**
 * @brief Draws @p source of @p image (NULL: all of it; cut to the image) stretched to
 *        @p width × @p height with its top-left at (@p x, @p y), each pixel the nearest of the
 *        source's, leaving out its key's pixels if it has one. A negative @p width mirrors it
 *        across (a negative @p height, down). Nothing if the formats differ. A card's turn is
 *        this, its width eased to 0 and back.
 */
void bmds_canvas_blit_scaled(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height,
                             const bmds_game_image_t *image, const bmds_rect_t *source);

/** @brief `bmds_canvas_blit_rotated()`'s turn: a whole one. */
#define BMDS_GAME_TURN 65536u

/** @brief `bmds_canvas_blit_rotated()`'s scale: the image's own size. */
#define BMDS_GAME_SCALE 256u

/**
 * @brief Draws @p source of @p image (NULL: all of it) turned clockwise by @p turn (of
 *        `BMDS_GAME_TURN`) and scaled by @p scale (of `BMDS_GAME_SCALE`) with its centre at
 *        (@p cx, @p cy), each pixel the nearest of the source's, leaving out its key's pixels if
 *        it has one. The damage is the turned picture's bounds. Nothing if the formats differ.
 */
void bmds_canvas_blit_rotated(bmds_canvas_t *canvas, int32_t cx, int32_t cy, uint32_t turn, uint32_t scale,
                              const bmds_game_image_t *image, const bmds_rect_t *source);

/** @brief A shade's or a blend's whole: `bmds_canvas_shade()`'s level that leaves pixels as they are. */
#define BMDS_GAME_OPAQUE 256u

/**
 * @brief Darkens a rectangle: each pixel's colour times @p level (of `BMDS_GAME_OPAQUE`, in 32
 *        steps). L8 pixels are left as they are (an index has no brightness).
 */
void bmds_canvas_shade(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t level);

/**
 * @brief Darkens, as `bmds_canvas_shade()`, the pixels @p source of @p image would cover drawn at
 *        (@p x, @p y): its key's pixels left out, so a keyed sprite casts its own shape (a
 *        shadow). Nothing if the formats differ.
 */
void bmds_canvas_shade_image(bmds_canvas_t *canvas, int32_t x, int32_t y, const bmds_game_image_t *image,
                             const bmds_rect_t *source, uint32_t level);

/**
 * @brief Fills a rectangle with 0xRRGGBB @p rgb over what is there, @p alpha of
 *        `BMDS_GAME_OPAQUE` of it (in 32 steps): a translucent fill. L8 fills with index @p rgb
 *        where @p alpha is at least half, and leaves the pixels otherwise.
 */
void bmds_canvas_fill_blend(bmds_canvas_t *canvas, int32_t x, int32_t y, int32_t width, int32_t height, uint32_t rgb,
                            uint32_t alpha);

/**
 * @brief Draws an ellipse's outline, one pixel wide, centred on (@p cx, @p cy) with radii @p rx
 *        across and @p ry down (a circle: both the same; 0 and 0: one pixel).
 */
void bmds_canvas_ellipse(bmds_canvas_t *canvas, int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t pixel);

/** @brief Fills an ellipse, as bmds_canvas_ellipse() outlines it. */
void bmds_canvas_fill_ellipse(bmds_canvas_t *canvas, int32_t cx, int32_t cy, int32_t rx, int32_t ry, uint32_t pixel);

/**
 * @brief Fills the polygon of @p points corners at @p xy (x0, y0, x1, y1...; closed from the last
 *        to the first), even-odd: each pixel whose centre is inside.
 * @return 0, or -1 (no memory for the crossings).
 */
int bmds_canvas_fill_polygon(bmds_canvas_t *canvas, const int32_t *xy, uint32_t points, uint32_t pixel);

/**
 * @brief Fills the area around (@p x, @p y) of the pixel value there, connected across and down
 *        (QBasic's PAINT), inside the clip.
 * @return 0, or -1 (no memory: the fill stopped part way).
 */
int bmds_canvas_flood_fill(bmds_canvas_t *canvas, int32_t x, int32_t y, uint32_t pixel);

/** @brief Returns the pixel value at (@p x, @p y), or 0 outside the canvas. */
uint32_t bmds_canvas_get(const bmds_canvas_t *canvas, int32_t x, int32_t y);

/** @brief Returns a pixel value as 0xRRGGBB (bmds_canvas_pixel()'s inverse; L8: the index). */
uint32_t bmds_canvas_rgb(const bmds_canvas_t *canvas, uint32_t pixel);

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
                uint32_t    size;     /**< `sizeof(bmds_game_config_t)`. */
                uint32_t    width;    /**< The picture's (a multiple of 4); 0 and 0 with `BMDS_SURFACE_MAXIMIZED`: */
                uint32_t    height;   /**< the maximized window's (`bmds_game_width()`, `bmds_game_height()`). */
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
                uint32_t options; /**< `BMDS_GAME_...`. */
} bmds_game_config_t;

/**
 * @brief `bmds_game_config_t::options`: the draw declares what it repaints. Before drawing
 *        anything it calls either bmds_game_repainted() for a rectangle it then paints every pixel
 *        of, and only there, or bmds_game_up_to_date() (after which it may draw anything, blends
 *        too). The canvas is brought up to date after drawing, but not where it was repainted: with
 *        double buffers, the last frame's changes are mostly drawn again, so not copied first.
 */
#define BMDS_GAME_REPAINTS 0x1u

/**
 * @brief Runs a game: opens its surface and loops (events, ticks, drawing, presents) until
 *        `bmds_game_quit()`. While the surface isn't active, no ticks run and nothing is drawn
 *        unless asked (`bmds_game_redraw()`).
 * @return 0, or a negative error (`-BMDS_EINVAL`, `-BMDS_ENODEV`: no desktop, or the surface
 *         calls' errors).
 */
int bmds_game_run(const bmds_game_config_t *config);

/** @brief Returns the config's `arg`. */
void *bmds_game_arg(const bmds_game_t *game);

/** @brief Ends the loop after this callback. */
void bmds_game_quit(bmds_game_t *game);

/** @brief Asks for a frame (`draw`) without a tick, inactive too (a pause screen drawn as it goes). */
void bmds_game_redraw(bmds_game_t *game);

/** @brief Returns the surface's descriptor (for `BMDS_SURFACE_...` controls: the palette, the pointer). */
int32_t bmds_game_fd(const bmds_game_t *game);

/** @brief From `draw`, with `BMDS_GAME_REPAINTS`: brings the canvas up to date now (none of what
 *         was repainted so far). Then anything may be drawn. Otherwise nothing. */
void bmds_game_up_to_date(bmds_game_t *game);

/**
 * @brief From `draw`, with `BMDS_GAME_REPAINTS`: every pixel of @p rect is about to be painted
 *        (none depends on what is there). Past `BMDS_GAME_REPAINTED_MAX` a frame, the canvas is
 *        brought up to date instead (bmds_game_up_to_date()). Otherwise nothing.
 */
void bmds_game_repainted(bmds_game_t *game, const bmds_rect_t *rect);

#define BMDS_GAME_REPAINTED_MAX 32u

/**
 * @brief Gives the 2D-DMA @p image (in the game's format, unkeyed, its pixels from
 *        bmds_mem_alloc()) as slot @p slot (0 to `BMDS_SURFACE_IMAGES - 1`) for
 *        bmds_game_dma_copy(); NULL forgets it. Change its pixels, and give it again
 *        (`devplan/games.md` §25). Without the 2D-DMA, copies go by the CPU.
 * @return 0, or `-BMDS_EINVAL` (the slot, a key, another format).
 */
int32_t bmds_game_dma_image(bmds_game_t *game, uint32_t slot, const bmds_game_image_t *image);

/**
 * @brief From `draw`: copies slot @p slot's image into the canvas at the same places, @p count
 *        rectangles cut to the canvas's clip, as damage: by the 2D-DMA in one job for each 64
 *        (`BMDS_SURFACE_BLIT`), or by the CPU. Returns when they are written.
 */
void bmds_game_dma_copy(bmds_game_t *game, uint32_t slot, const bmds_rect_t *rects, uint32_t count);

/** @brief From `draw`: fills @p count rectangles with @p pixel (the canvas's value), as bmds_game_dma_copy(). */
void bmds_game_dma_fill(bmds_game_t *game, const bmds_rect_t *rects, uint32_t count, uint32_t pixel);

/** @brief Returns the ticks run so far. */
uint64_t bmds_game_ticks(const bmds_game_t *game);

/** @brief Returns true while the surface is active (focused). */
int bmds_game_is_active(const bmds_game_t *game);

/** @brief Returns the picture's width (the config's, or what the system chose for 0). */
uint32_t bmds_game_width(const bmds_game_t *game);

/** @brief Returns the picture's height. */
uint32_t bmds_game_height(const bmds_game_t *game);

/** @brief Effects playing at once (`bmds_game_play()`). */
#define BMDS_GAME_VOICES 16u

/** @brief The sound (effects and music) of a game. */
typedef struct bmds_game_audio_t bmds_game_audio_t;

/** @brief How `bmds_game_audio_open()` plays. */
typedef struct bmds_game_audio_config_t {
                uint32_t size;          /**< `sizeof(bmds_game_audio_config_t)`. */
                uint32_t period_frames; /**< The mixer's period (its latency's step); 0: 256. */
                uint32_t flags;         /**< 0. */
} bmds_game_audio_config_t;

/**
 * @brief Opens the sound device (`/dev/dsp`) as a real-time client and starts the mixer's thread
 *        (@p config NULL: the defaults).
 * @return 0 and @p audio, or a negative error (`-BMDS_ENOMEM`, the device's: `-BMDS_ENOENT` no
 *         sound device, `-BMDS_EBUSY`).
 */
int bmds_game_audio_open(const bmds_game_audio_config_t *config, bmds_game_audio_t **audio);

/** @brief Stops the music and the effects and closes @p audio (NULL: nothing). */
void bmds_game_audio_close(bmds_game_audio_t *audio);

/** @brief Returns the device's rate, which loaded sounds are at. */
uint32_t bmds_game_audio_rate(const bmds_game_audio_t *audio);

/** @brief Pauses (@p paused not 0) or resumes the effects and the music: while inactive, say. */
void bmds_game_audio_pause(bmds_game_audio_t *audio, int paused);

/** @brief Sets the effects' volume, 0 to 1 (the start: 1). */
void bmds_game_audio_volume(bmds_game_audio_t *audio, float volume);

/** @brief A sound loaded to play: 16-bit samples at the device's rate, 1 or 2 channels. */
typedef struct bmds_game_sound_t {
                const int16_t *samples; /**< Interleaved, left first. */
                uint32_t       frames;
                uint32_t       channels;
} bmds_game_sound_t;

/**
 * @brief Loads the sound file at @p path (`bmds_audio_decode()`'s: WAV, MP3, Ogg Vorbis, FLAC)
 *        whole into @p sound, resampled to @p audio's rate.
 * @return 0, or a negative error (`-BMDS_ERANGE`: longer than 10 minutes; `-BMDS_ENOMEM`, the
 *         open's and the decoder's). Free it with `bmds_game_sound_free()`, its voices stopped.
 */
int bmds_game_sound_load(bmds_game_audio_t *audio, const char *path, bmds_game_sound_t *sound);

/** @brief Loads a sound from @p length bytes at @p data (one built in), as `bmds_game_sound_load()`. */
int bmds_game_sound_load_memory(bmds_game_audio_t *audio, const void *data, uint32_t length, bmds_game_sound_t *sound);

/** @brief Frees a loaded sound and empties it (an empty one: nothing). No voice may be playing it. */
void bmds_game_sound_free(bmds_game_sound_t *sound);

/** @brief `bmds_game_play()`'s flags: it plays until stopped, from its start again at its end. */
#define BMDS_GAME_PLAY_LOOP 1u

/**
 * @brief Starts @p sound at @p volume (0 to 1) and @p pan (-1 left to 1 right), from the
 *        mixer's next period. With every voice busy, the oldest that doesn't loop is taken.
 * @return The voice (0 or more), or `-BMDS_EBUSY` (every voice loops), `-BMDS_EAGAIN` (more
 *         asked since the mixer's last period than it keeps), `-BMDS_EINVAL`.
 */
int32_t bmds_game_play(bmds_game_audio_t *audio, const bmds_game_sound_t *sound, float volume, float pan,
                       uint32_t flags);

/** @brief Changes a voice's volume and pan (over a period: no click). Ended: nothing. */
void bmds_game_voice_set(bmds_game_audio_t *audio, int32_t voice, float volume, float pan);

/** @brief Stops a voice (faded over a period). Ended: nothing. */
void bmds_game_voice_stop(bmds_game_audio_t *audio, int32_t voice);

/** @brief Returns true while @p voice plays (as far as the mixer has said). */
int bmds_game_voice_is_playing(const bmds_game_audio_t *audio, int32_t voice);

/**
 * @brief Plays the sound file at @p path (`bmds_audio_decode()`'s: MIDI through the synth, MP3,
 *        Ogg Vorbis, FLAC, WAV) as the music at @p volume, from its start again at its end with
 *        `BMDS_GAME_PLAY_LOOP`, decoded on a thread of its own (real-time, `SCHED_FIFO` at 1: above
 *        the game's threads); the music playing stops first. With `BMDS_GAME_MUSIC_MIDI_OUT`, a
 *        MIDI file's events go to `/dev/midi` instead (the system port: the built-in synth, or
 *        wherever it is routed), sent ahead with their times so they keep them; the volume
 *        scales its channels' volumes. Other files are decoded as without it.
 * @return 0, or a negative error (the open's and the decoder's, the device's, `-BMDS_ENOMEM`).
 */
int bmds_game_music_play(bmds_game_audio_t *audio, const char *path, float volume, uint32_t flags);

/** @brief `bmds_game_music_play()`'s flags: a MIDI file through `/dev/midi` (devplan/audio.md §25). */
#define BMDS_GAME_MUSIC_MIDI_OUT 2u

/** @brief Stops the music (none: nothing). */
void bmds_game_music_stop(bmds_game_audio_t *audio);

/** @brief Sets the music's volume, 0 to 1. */
void bmds_game_music_volume(bmds_game_audio_t *audio, float volume);

#ifdef __cplusplus
}
#endif

#endif /* BMDS_GAME_H */
