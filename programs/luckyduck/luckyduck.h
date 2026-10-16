/**
 * @file      luckyduck.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck (devplan/luckyduck.md): what the casino's parts share. One RGB565 surface the size
 * of a maximized window (at most 1280×720) for the whole run; each game a scene the loop forwards to; the pictures, the sounds, the felt
 * under every table, the settings and the bank.
 */

#ifndef LUCKYDUCK_H
#define LUCKYDUCK_H

#include <stdint.h>
#include <bmds/game.h>
#include "art.h"

/* The picture: a maximized window's client area (the system's choice), at most 1280×720. */
#define MAX_WIDTH 1280
#define MAX_HEIGHT 720
#define WIDTH (app.width)
#define HEIGHT (app.height)

/* HID keys (page 7). */
#define KEY(u) (0x00070000u | (u))
#define K_A KEY(0x04)
#define K_B KEY(0x05)
#define K_C KEY(0x06)
#define K_D KEY(0x07)
#define K_F KEY(0x09)
#define K_H KEY(0x0b)
#define K_L KEY(0x0f)
#define K_M KEY(0x10)
#define K_N KEY(0x11)
#define K_O KEY(0x12)
#define K_P KEY(0x13)
#define K_R KEY(0x15)
#define K_S KEY(0x16)
#define K_T KEY(0x17)
#define K_U KEY(0x18)
#define K_Y KEY(0x1c)
#define K_Z KEY(0x1d)
#define K_1 KEY(0x1e)
#define K_3 KEY(0x20)
#define K_ENTER KEY(0x28)
#define K_ESCAPE KEY(0x29)
#define K_BACKSPACE KEY(0x2a)
#define K_SPACE KEY(0x2c)
#define K_F1 KEY(0x3a)
#define K_F2 KEY(0x3b)
#define K_RIGHT KEY(0x4f)
#define K_LEFT KEY(0x50)
#define K_DOWN KEY(0x51)
#define K_UP KEY(0x52)
#define K_KP_ENTER KEY(0x58)
#define MOD_CTRL 0x11u

/* Colours (0xRRGGBB). */
#define C_TEXT 0xfff4dc
#define C_DIM 0xb8d0b8
#define C_GOLD 0xffd040
#define C_CURSOR 0x40e0ff
#define C_PANEL 0x0c2a1a
#define C_PANEL_EDGE 0xe8b830
#define C_SHADOW 0x000000

/** @brief A game, or the lobby: what the loop forwards to. */
typedef struct scene_t {
                const char *name;  /**< As `luckyduck NAME` takes it. */
                const char *title; /**< The window's. */
                /** @brief Starting: paints the felt, deals. @return 0, or -1 (said on stderr). */
                int (*enter)(int argc, char **argv);
                void (*leave)(void);
                void (*event)(const bmds_surface_event_t *e);
                void (*tick)(void);
                /** @brief Draws what changed since the last frame. */
                void (*draw)(bmds_canvas_t *c);
                /** @brief Draws everything inside @p r again (from the felt up). */
                void (*redraw)(bmds_canvas_t *c, bmds_rect_t r);
                /** @brief Whether a game is under way (leaving asks first); NULL: never. */
                int (*busy)(void);
} scene_t;

extern const scene_t lobby_scene;
extern const scene_t solitaire_scene;
extern const scene_t blackjack_scene;

/** @brief The sounds (the folder's WAVs). */
enum {
        SFX_PLACE,
        SFX_FLIP,
        SFX_DEAL,
        SFX_WIN,
        SFX_QUACK,
        SFX_QUACK2,
        SFX_QUACKS,
        SFX_CLICK,
        SFX_SAD,
        SFX_COUNT
};

/** @brief What every scene shares. */
typedef struct {
                bmds_game_t      *game;
                int32_t           width, height; /**< The picture's. */
                bmds_game_image_t cards, sprites, font;
                bmds_game_image_t felt; /**< The table under everything: the scene paints it. */
                bmds_canvas_t     felt_canvas;
                int32_t           px, py; /**< The pointer. */
                uint32_t          buttons;
                int64_t           idle_ns; /**< When the last input came. */
} app_t;

extern app_t app;

/** @brief Plays a sound (unless the sound is off). */
void sfx(int which, float volume);

/** @brief Whether the sound is on, and turning it on or off (kept). */
int  sound_on(void);
void sound_toggle(void);

/** @brief Changes to @p scene, the ducklings waddling across between them. */
void app_switch(const scene_t *scene, int argc, char **argv);

/** @brief Back to the lobby: asks first if a game is under way. */
void app_leave(void);

/** @brief Whether a box is up (the scene's events and ticks don't come while it is). */
int app_asking(void);

/**
 * @brief Asks @p title (with @p line under it): @p yes (Y, or a click) calls @p yes_fn; "keep
 *        playing", the safe answer lit from the start, is Enter's, Space's, Escape's and N's. Every game asks this before it ends a game under way: going to
 *        the lobby, a new game, quitting (devplan/luckyduck.md §9).
 */
void app_confirm(const char *title, const char *line, const char *yes, void (*yes_fn)(void));

/** @brief Shows @p count lines of @p title to read (rules, help) until closed. @p lines must last. */
void app_info(const char *title, const char *const *lines, int count);

/** @brief System buttons' places: stacked at the top right, @p i from the top (gameplay's are in the band). */
bmds_rect_t app_system_button(int i);

/** @brief The felt inside @p r onto @p c (the background of every redraw). */
void felt_restore(bmds_canvas_t *c, bmds_rect_t r);

/** @brief Paints the felt: green baize, @p glow its lighter middle (0xRRGGBB), darker at the edges. */
void felt_baize(uint32_t middle, uint32_t edge);

/** @brief Paints the felt: the lobby's carpet. */
void felt_carpet(void);

/* ---- Rectangles */

bmds_rect_t rect(int32_t x, int32_t y, int32_t w, int32_t h);
int         meets(const bmds_rect_t *a, const bmds_rect_t *b);
bmds_rect_t bounds(const bmds_rect_t *a, const bmds_rect_t *b);
int         contains(const bmds_rect_t *r, int32_t x, int32_t y);
int         same_rect(const bmds_rect_t *a, const bmds_rect_t *b);
bmds_rect_t intersection(bmds_rect_t a, bmds_rect_t b);

/** @brief Adds @p r to @p list (of @p count, at most @p max) to redraw: merged where that costs little. */
void dirty_add(bmds_rect_t *list, uint32_t *count, uint32_t max, bmds_rect_t r);

/* ---- Settings: "name value" lines in a file of the data folder */

int64_t setting(const char *name, int64_t fallback);
void    set_setting(const char *name, int64_t value);

/* ---- The bank (devplan/luckyduck.md §7) */

#define BANK_START 1000

/** @brief The balance, in dollars. */
int64_t bank_balance(void);

/** @brief Adds @p delta (negative: takes) to the balance, kept at once. */
void bank_add(int64_t delta);

/** @brief Tops the balance up to the start if below @p least. @return 1 if it did. */
int bank_ensure(int64_t least);

/** @brief Formats @p dollars as "$1,234" (or "-$52") into @p out. */
void money(char *out, uint32_t size, int64_t dollars);

#endif /* LUCKYDUCK_H */
