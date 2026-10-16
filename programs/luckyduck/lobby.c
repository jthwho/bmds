/**
 * @file      lobby.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's lobby (devplan/luckyduck.md §5): the casino floor, each game a table to click (lit
 * and lifted under the pointer; the ones not built yet behind a "coming soon" sign), the bank, and
 * Lucky waddling about between them: stopping, blinking, quacking (a click on Lucky quacks out
 * loud), and asleep when nobody has touched anything for a while. The title is a sign whose
 * bulbs chase around it. Laid out for the picture's size (a maximized window's).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stage.h"
#include "ui.h"

#define LANE_Y (HEIGHT - 130)
#define SLEEP_NS 45000000000LL
#define TITLE "LUCKY DUCK CASINO"
#define BULBS 96
#define BULB_STEP 24 /* pixels between bulbs */

typedef struct {
                int            sprite;
                int32_t        x, y;
                const scene_t *scene; /* NULL: coming soon */
} table_t;

/* Two rows across the middle (their places from the picture's size: lobby_enter()). */
static table_t TABLES[] = {
        {ART_TABLE_SOLITAIRE, 0, 0, &solitaire_scene},
        {ART_TABLE_BLACKJACK, 0, 0, &blackjack_scene},
        {ART_TABLE_DICE, 0, 0, &dice_scene},
        {ART_TABLE_VIDEO, 0, 0, &video_scene},
        {ART_TABLE_POKER, 0, 0, &poker_scene},
};
#define TABLE_COUNT (int)(sizeof(TABLES) / sizeof(TABLES[0]))

enum {
        L_WALK,
        L_IDLE,
        L_QUACK,
        L_SLEEP,
        L_CHEER
};

/* Keys on the stage. */
enum {
        KEY_SOUND = 1,
        KEY_BANK,
        KEY_BANK_LABEL,
        KEY_HINT,
        KEY_TABLE = 10, /* + the table */
        KEY_GREY = 20,  /* + the table */
        KEY_SIGN = 30,  /* + the table */
        KEY_LUCKY = 40,
        KEY_BUBBLE,
        KEY_SHADOW,
        KEY_Z = 50,    /* + 3 */
        KEY_BULB = 100 /* + the bulb */
};

static struct {
                stage_t stage;
                /* The sign's bulbs: where (their middles), how many, the pattern's step. */
                int16_t  bulb_x[BULBS], bulb_y[BULBS];
                int      bulbs;
                uint32_t lights;
                int      hover; /* the table under the pointer, or -1 */
                int      sound_hover;
                /* Lucky. */
                int      state;
                int32_t  x, target;
                int      dir;
                uint32_t ticks, wait, bubble;
} l;

static int32_t random_between(int32_t a, int32_t b) {
        return a + (int32_t)((uint32_t)rand() % (uint32_t)(b - a + 1));
}

static void walk_somewhere(void) {
        int32_t t = random_between(40, WIDTH - 170);
        if (t > l.x - 80 && t < l.x + 80) t = l.x < WIDTH / 2 ? l.x + 300 : l.x - 300;
        l.target = t;
        l.dir = t > l.x ? 1 : -1;
        l.state = L_WALK;
        l.ticks = 0;
        return;
}

static void lucky_quack(int loud) {
        l.state = L_QUACK;
        l.ticks = 0;
        l.bubble = 70;
        if (loud) sfx(rand() % 2 ? SFX_QUACK : SFX_QUACK2, 0.7f);
        return;
}

static void lucky_tick(void) {
        int64_t now = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &now);
        const int idle = now - app.idle_ns > SLEEP_NS;
        ++l.ticks;
        if (l.bubble > 0) --l.bubble;
        switch (l.state) {
                case L_WALK:
                        l.x += 2 * l.dir;
                        if ((l.dir > 0 && l.x >= l.target) || (l.dir < 0 && l.x <= l.target)) {
                                l.state = L_IDLE;
                                l.ticks = 0;
                                l.wait = (uint32_t)random_between(120, 330);
                        }
                        break;
                case L_IDLE:
                        if (idle) {
                                l.state = L_SLEEP;
                                l.ticks = 0;
                        } else if (l.ticks >= l.wait) {
                                if (rand() % 4 == 0)
                                        lucky_quack(0);
                                else
                                        walk_somewhere();
                        }
                        break;
                case L_QUACK:
                        if (l.ticks >= 40) {
                                l.state = L_IDLE;
                                l.ticks = 0;
                                l.wait = (uint32_t)random_between(60, 180);
                        }
                        break;
                case L_SLEEP:
                        if (!idle) { /* woken */
                                l.state = L_CHEER;
                                l.ticks = 0;
                                sfx(SFX_QUACK2, 0.4f);
                        }
                        break;
                case L_CHEER:
                        if (l.ticks >= 36) walk_somewhere();
                        break;
                default: break;
        }
        return;
}

static int lucky_sprite(void) {
        switch (l.state) {
                case L_WALK: return ART_LUCKY_WALK0 + (int)((l.ticks / 7) % 4);
                case L_QUACK: return l.ticks < 30 ? ART_LUCKY_QUACK : ART_LUCKY_IDLE;
                case L_SLEEP: return ART_LUCKY_SLEEP;
                case L_CHEER: return ART_LUCKY_CHEER;
                default: return (l.ticks % 200) < 8 ? ART_LUCKY_BLINK : ART_LUCKY_IDLE;
        }
}

static bmds_rect_t lucky_rect(void) {
        return rect(l.x, LANE_Y, 128, 128);
}

static bmds_rect_t sound_rect(void) {
        return rect(24, 39, 170, 38);
}

static int table_at(int32_t x, int32_t y) {
        for (int i = 0; i < TABLE_COUNT; ++i) {
                /* The oval and the middle of its plate, not the picture's empty corners and sides. */
                /* In 64 bits: far away, dx² × 76² passes 2³¹ (it wrapped negative: inside the first table). */
                const int64_t dx = x - (TABLES[i].x + 150), dy = y - (TABLES[i].y + 82);
                if (dx * dx * 76 * 76 + dy * dy * 142 * 142 <= (int64_t)142 * 142 * 76 * 76) return i;
                const bmds_rect_t plate = rect(TABLES[i].x + 80, TABLES[i].y + 166, 140, 34);
                if (contains(&plate, x, y)) return i;
        }
        return -1;
}

/* ---- The sign: bulbs chasing around the title */

/* Whether bulb @p i is lit now: a chase (pairs running round) for six seconds, then the odd and
   even ones flashing in turn for three, then round again. */
static int bulb_lit(int i) {
        const uint32_t t = l.lights % 540; /* ticks */
        if (t < 360) return (uint32_t)(i + 1000 - (int)(t / 4)) % 4 < 2;
        return ((uint32_t)i + (t - 360) / 15) % 2 == 0;
}

/* The sign around the title painted on the felt, and its bulbs' places along a path round it. */
static void paint_sign(void) {
        bmds_canvas_t    *c = &app.felt_canvas;
        const int32_t     tw = ui_text_width(ART_FONT_BIG, TITLE);
        const bmds_rect_t sign = rect(WIDTH / 2 - tw / 2 - 48, 6, tw + 96, 104);
        ui_round_fill(c, sign, 16, 0x1a0606);
        ui_round_fill(c, rect(sign.x + 2, sign.y + 2, (int32_t)sign.width - 4, (int32_t)sign.height - 4), 14, 0x4a0c14);
        ui_round_fill(c, rect(sign.x + 22, sign.y + 22, (int32_t)sign.width - 44, (int32_t)sign.height - 44), 6,
                      0xe8b830);
        ui_round_fill(c, rect(sign.x + 25, sign.y + 25, (int32_t)sign.width - 50, (int32_t)sign.height - 50), 4,
                      0x16040a);
        ui_text_centred(c, ART_FONT_BIG, WIDTH / 2, sign.y + 32, TITLE);
        bmds_canvas_damage_clear(c);
        /* The bulbs on a rectangle 12 in from the edge, evenly round it from the top-left. */
        const int32_t x0 = sign.x + 12, y0 = sign.y + 12;
        const int32_t w = (int32_t)sign.width - 24, h = (int32_t)sign.height - 24, around = 2 * (w + h);
        l.bulbs = around / BULB_STEP < BULBS ? around / BULB_STEP : BULBS;
        for (int i = 0; i < l.bulbs; ++i) {
                int32_t d = (int32_t)((int64_t)around * i / l.bulbs), x, y;
                if (d < w) {
                        x = x0 + d, y = y0;
                } else if ((d -= w) < h) {
                        x = x0 + w, y = y0 + d;
                } else if ((d -= h) < w) {
                        x = x0 + w - d, y = y0 + h;
                } else {
                        x = x0, y = y0 + h - (d - w);
                }
                l.bulb_x[i] = (int16_t)x;
                l.bulb_y[i] = (int16_t)y;
        }
        return;
}

/* ---- The frame's layout */

static void layout(void) {
        stage_t *s = &l.stage;
        stage_begin(s);
        stage_item_t *it = stage_put(s, KEY_SOUND, STAGE_BUTTON, 5, 24, 39);
        it->w = 170;
        it->h = 38;
        it->font = l.sound_hover ? UI_HOVER : UI_NORMAL;
        snprintf(it->text, sizeof(it->text), "SOUND: %s", sound_on() ? "ON" : "OFF");
        char amount[24];
        money(amount, sizeof(amount), bank_balance());
        /* "BANK" over the amount, both to the right: the amount can grow to many digits. */
        it = stage_put(s, KEY_BANK_LABEL, STAGE_TEXT, 5, 0, 30);
        it->font = ART_FONT_SMALL;
        snprintf(it->text, sizeof(it->text), "BANK");
        it->x = (int16_t)(WIDTH - 28 - ui_text_width(ART_FONT_SMALL, it->text));
        it = stage_put(s, KEY_BANK, STAGE_TEXT, 5, 0, 56);
        it->font = ART_FONT_MED;
        snprintf(it->text, sizeof(it->text), "%s", amount);
        it->x = (int16_t)(WIDTH - 28 - ui_text_width(ART_FONT_MED, it->text));
        for (int i = 0; i < TABLE_COUNT; ++i) {
                const table_t *t = &TABLES[i];
                const int      lift = l.hover == i && t->scene != NULL ? 6 : 0;
                if (lift) { /* its shadow, left where it was */
                        it = stage_put(s, KEY_GREY + i, STAGE_SHADE, 9, t->x, t->y + 4);
                        it->tile = (uint16_t)t->sprite;
                        it->alpha = 110;
                }
                it = stage_put(s, KEY_TABLE + i, STAGE_SPRITE, 10, t->x, t->y - lift);
                it->tile = (uint16_t)t->sprite;
                if (t->scene == NULL) {
                        it = stage_put(s, KEY_GREY + i, STAGE_SHADE, 11, t->x, t->y);
                        it->tile = (uint16_t)t->sprite;
                        it->alpha = 110;
                        it = stage_put(s, KEY_SIGN + i, STAGE_SPRITE, 12, t->x + 140, t->y + 96);
                        it->tile = ART_SIGN_SOON;
                }
        }
        it = stage_put(s, KEY_HINT, STAGE_LABEL, 5, WIDTH / 2, 114);
        it->flags = STAGE_CENTRE;
        it->rgb = 0xf0d8a0;
        snprintf(it->text, sizeof(it->text), "Click a table to play");
        for (int i = 0; i < l.bulbs; ++i) {
                it = stage_put(s, KEY_BULB + (uint32_t)i, STAGE_SPRITE, 6, l.bulb_x[i] - 9, l.bulb_y[i] - 9);
                it->tile = bulb_lit(i) ? ART_BULB_ON : ART_BULB_OFF;
        }
        it = stage_put(s, KEY_SHADOW, STAGE_SHADE, 29, l.x + (l.dir < 0 ? 18 : 14), LANE_Y + 106);
        it->tile = ART_SHADOW;
        it->alpha = 150;
        it = stage_put(s, KEY_LUCKY, STAGE_SPRITE, 30, l.x, LANE_Y);
        it->tile = (uint16_t)lucky_sprite();
        if (l.dir < 0) it->flags = STAGE_MIRROR;
        if (l.bubble > 0) { /* its tail towards the beak, its words the right way round */
                it = stage_put(s, KEY_BUBBLE, STAGE_SPRITE, 31, l.dir < 0 ? l.x - 70 : l.x + 70, LANE_Y - 52);
                it->tile = l.dir < 0 ? ART_BUBBLE_QUACK_LEFT : ART_BUBBLE_QUACK;
        }
        if (l.state == L_SLEEP) {
                for (int k = 0; k < 3; ++k) {
                        const uint32_t phase = (l.ticks + (uint32_t)k * 40) % 120;
                        it = stage_put(s, KEY_Z + (uint32_t)k, STAGE_SPRITE, 31,
                                       l.x + (l.dir < 0 ? 10 : 90) + (int32_t)(phase / 8),
                                       LANE_Y + 10 - (int32_t)phase / 2);
                        it->tile = ART_SLEEP_Z;
                        it->scale = (uint16_t)(128 + phase);
                }
        }
        return;
}

/* ---- The scene */

static int lobby_enter(int argc, char **argv) {
        (void)argc;
        (void)argv;
        felt_carpet();
        paint_sign();
        /* The tables: three across, two under them between, the rows as close as the height asks. */
        const int32_t row2 = HEIGHT >= 700 ? 364 : 350;
        for (int i = 0; i < TABLE_COUNT; ++i) {
                TABLES[i].x = i < 3 ? WIDTH / 2 - 500 + i * 350 : WIDTH / 2 - 325 + (i - 3) * 350;
                TABLES[i].y = i < 3 ? 134 : row2;
        }
        l.hover = -1;
        l.sound_hover = 0;
        if (l.x == 0 || l.x > WIDTH - 140) l.x = WIDTH / 2 - 80;
        l.dir = 1;
        l.state = L_IDLE;
        l.ticks = 0;
        l.wait = 90;
        layout();
        memcpy(l.stage.shown, l.stage.now, sizeof(l.stage.now));
        l.stage.dirty_count = 0;
        return 0;
}

static void lobby_event(const bmds_surface_event_t *e) {
        if (e->type == BMDS_EVENT_POINTER) {
                const int32_t     x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                const uint32_t    pressed = e->data[2] & ~app.buttons;
                const bmds_rect_t sr = sound_rect();
                l.hover = table_at(x, y);
                l.sound_hover = contains(&sr, x, y);
                if ((pressed & 1u) == 0) return;
                const bmds_rect_t lr = lucky_rect();
                if (l.sound_hover) {
                        sound_toggle();
                        sfx(SFX_CLICK, 0.6f);
                } else if (contains(&lr, x, y)) {
                        lucky_quack(1);
                } else if (l.hover >= 0 && TABLES[l.hover].scene != NULL) {
                        sfx(SFX_CLICK, 0.6f);
                        app_switch(TABLES[l.hover].scene, 0, NULL);
                } else if (l.hover >= 0) {
                        sfx(SFX_SAD, 0.35f);
                }
        } else if (e->type == BMDS_EVENT_ACTIVE && e->data[0] == 0) {
                l.hover = -1; /* the pointer gone elsewhere: nothing lit */
                l.sound_hover = 0;
        } else if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0) {
                if (e->data[0] == K_M) sound_toggle();
        }
        return;
}

static void lobby_tick(void) {
        ++l.lights;
        lucky_tick();
        return;
}

static void lobby_draw(bmds_canvas_t *c) {
        layout();
        stage_draw(&l.stage, c);
        return;
}

static void lobby_redraw(bmds_canvas_t *c, bmds_rect_t r) {
        layout();
        stage_redraw(&l.stage, c, r);
        return;
}

const scene_t lobby_scene = {
        "lobby", "Lucky Duck", lobby_enter, NULL, lobby_event, lobby_tick, lobby_draw, lobby_redraw, NULL,
};
