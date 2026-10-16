/**
 * @file      loop.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmdsgame's frame loop (devplan/games.md §13): events, fixed-rate ticks, a frame drawn on a
 * buffer brought up to the last frame shown, its damage presented, paced on the display.
 */

#include <string.h>
#include "game_internal.h"

#define MAX_BUFFERS 3
#define HISTORY 4      /* presents whose damage is kept: a buffer further behind is copied whole */
#define MAX_CATCH_UP 5 /* ticks run at once after a stall; more are dropped */
#define WAIT_MS 100    /* the longest a present waits for the last one's frame */

typedef struct {
                uint32_t    count; /* 0 with all: the whole picture */
                int         all;
                bmds_rect_t rects[BMDS_CANVAS_MAX_DAMAGE];
} damage_t;

struct bmds_game_t {
                bmds_game_config_t config;
                int32_t            fd;
                uint32_t           buffers;
                bmds_canvas_t      canvas[MAX_BUFFERS];
                uint32_t           shown_as[MAX_BUFFERS]; /* the present its pixels are (0: the black start) */
                uint32_t           presents;
                int32_t            newest; /* the last present's buffer, or -1 */
                int32_t            held;   /* acquired, nothing drawn on it yet: kept for the next frame */
                damage_t           history[HISTORY];
                int32_t            last_frame;
                int                presented;
                uint64_t           ticks;
                int64_t            origin_ns; /* when tick 0 was due */
                int64_t            period_ns;
                int                active;
                int                quit;
                int                redraw;
                int32_t            drawing;                            /* the buffer being drawn, or -1 */
                bmds_rect_t        repainted[BMDS_GAME_REPAINTED_MAX]; /* this frame's, BMDS_GAME_REPAINTS */
                uint32_t           repainted_count;
                bmds_game_image_t  images[BMDS_SURFACE_IMAGES]; /* registered (pixels NULL: none) */
                int                no_dma;                      /* the kernel said no once: the CPU from then on */
};

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

void *bmds_game_arg(const bmds_game_t *game) {
        return game->config.arg;
}

void bmds_game_quit(bmds_game_t *game) {
        game->quit = 1;
}

void bmds_game_redraw(bmds_game_t *game) {
        game->redraw = 1;
}

int32_t bmds_game_fd(const bmds_game_t *game) {
        return game->fd;
}

uint64_t bmds_game_ticks(const bmds_game_t *game) {
        return game->ticks;
}

int bmds_game_is_active(const bmds_game_t *game) {
        return game->active;
}

uint32_t bmds_game_width(const bmds_game_t *game) {
        return game->config.width;
}

uint32_t bmds_game_height(const bmds_game_t *game) {
        return game->config.height;
}

/* Buffer @p b brought up to the last present: what the presents since its own changed, copied from
   the newest buffer. */
/* ---- The 2D-DMA (devplan/games.md §25): pieces gathered, one BLIT for each 64, the CPU when
   the kernel has none */

typedef struct {
                bmds_game_t         *g;
                int32_t              buffer; /* written */
                uint32_t             source; /* BMDS_BLIT_... */
                uint32_t             from;
                bmds_surface_piece_t pieces[BMDS_SURFACE_BLIT_MAX];
                uint32_t             count;
                int                  damage; /* recorded on the canvas (a copy or fill the program asked for) */
} batch_t;

static void batch_cpu(batch_t *t) {
        bmds_game_t   *g = t->g;
        bmds_canvas_t *c = &g->canvas[t->buffer];
        for (uint32_t i = 0; i < t->count; ++i) {
                const bmds_surface_piece_t *p = &t->pieces[i];
                const bmds_rect_t           r = {p->x, p->y, p->width, p->height};
                if (t->source == BMDS_BLIT_BUFFER) {
                        bmds_canvas_copy(c, &g->canvas[t->from], &r, 1);
                } else if (t->source == BMDS_BLIT_IMAGE) {
                        bmds_canvas_blit(c, r.x, r.y, &g->images[t->from], &r);
                } else {
                        bmds_canvas_fill(c, r.x, r.y, r.width, r.height, t->from);
                }
        }
        return;
}

static void batch_flush(batch_t *t) {
        if (t->count == 0) return;
        bmds_game_t *g = t->g;
        if (!g->no_dma) {
                const bmds_surface_blit_t bl = {sizeof(bl), (uint32_t)t->buffer, t->source, t->from, t->count,
                                                t->pieces};
                if (bmds_surface_control(g->fd, BMDS_SURFACE_BLIT, (void *)&bl, sizeof(bl)) == 0) {
                        if (t->damage)
                                for (uint32_t i = 0; i < t->count; ++i) {
                                        const bmds_rect_t r = {t->pieces[i].x, t->pieces[i].y, t->pieces[i].width,
                                                               t->pieces[i].height};
                                        bmds_game_damage_in_clip(&g->canvas[t->buffer], &r);
                                }
                        t->count = 0;
                        return;
                }
                g->no_dma = 1; /* an older kernel, or none to be had: the CPU */
        }
        batch_cpu(t);
        t->count = 0;
        return;
}

static void batch_add(void *arg, const bmds_rect_t *r) {
        batch_t *t = (batch_t *)arg;
        if (r->width == 0 || r->height == 0) return;
        bmds_surface_piece_t *p = &t->pieces[t->count++];
        p->x = r->x;
        p->y = r->y;
        p->width = r->width;
        p->height = r->height;
        p->from_x = r->x;
        p->from_y = r->y;
        if (t->count == BMDS_SURFACE_BLIT_MAX) batch_flush(t);
        return;
}

/* Copies into buffer @p b what the presents since its own changed (from the newest), but none of
   @p except (what this frame drew, when it repaints: `BMDS_GAME_REPAINTS`). */
static void bring_up_to_date_except(bmds_game_t *g, int32_t b, const bmds_rect_t *except, uint32_t excepts) {
        if (g->newest < 0 || g->shown_as[b] == g->presents) return;
        const bmds_canvas_t *from = &g->canvas[g->newest];
        bmds_canvas_t       *to = &g->canvas[b];
        const bmds_rect_t    all = {0, 0, (uint32_t)to->width, (uint32_t)to->height};
        bmds_rect_t          rects[BMDS_CANVAS_MAX_DAMAGE];
        uint32_t             count = 0;
        int                  whole = g->presents - g->shown_as[b] > HISTORY;
        for (uint32_t n = g->shown_as[b] + 1; n <= g->presents && !whole; ++n) {
                const damage_t *d = &g->history[n % HISTORY];
                whole = d->all;
                for (uint32_t i = 0; i < d->count; ++i)
                        bmds_game_rects_add(rects, &count, BMDS_CANVAS_MAX_DAMAGE, &d->rects[i]);
        }
        if (whole) {
                rects[0] = all;
                count = 1;
        }
        (void)from;
        batch_t t;
        t.g = g;
        t.buffer = b;
        t.source = BMDS_BLIT_BUFFER;
        t.from = (uint32_t)g->newest;
        t.count = 0;
        t.damage = 0;
        bmds_game_spans_except(to, rects, count, except, excepts, batch_add, &t);
        batch_flush(&t);
        g->shown_as[b] = g->presents;
}

static void bring_up_to_date(bmds_game_t *g, int32_t b) {
        bring_up_to_date_except(g, b, NULL, 0);
}

void bmds_game_up_to_date(bmds_game_t *game) {
        if (game->drawing >= 0) bring_up_to_date_except(game, game->drawing, game->repainted, game->repainted_count);
        return;
}

int32_t bmds_game_dma_image(bmds_game_t *game, uint32_t slot, const bmds_game_image_t *image) {
        if (slot >= BMDS_SURFACE_IMAGES) return -BMDS_EINVAL;
        bmds_game_image_t none;
        memset(&none, 0, sizeof(none));
        if (image != NULL && (image->format != game->config.format || (image->flags & BMDS_GAME_IMAGE_KEYED) != 0))
                return -BMDS_EINVAL; /* the 2D-DMA copies; it has no key */
        game->images[slot] = image != NULL ? *image : none;
        if (game->no_dma) return 0;
        const bmds_surface_image_t im = {sizeof(im),
                                         slot,
                                         image != NULL ? image->pixels : NULL,
                                         image != NULL ? image->width : 0,
                                         image != NULL ? image->height : 0,
                                         image != NULL ? image->stride : 0};
        if (bmds_surface_control(game->fd, BMDS_SURFACE_IMAGE, (void *)&im, sizeof(im)) != 0) game->no_dma = 1;
        return 0;
}

/* A program's copy or fill of the canvas being drawn: cut to its clip, recorded as damage. */
static void dma_pieces(bmds_game_t *game, uint32_t source, uint32_t from, const bmds_rect_t *rects, uint32_t count) {
        if (game->drawing < 0) return;
        bmds_canvas_t *c = &game->canvas[game->drawing];
        batch_t        t;
        t.g = game;
        t.buffer = game->drawing;
        t.source = source;
        t.from = from;
        t.count = 0;
        t.damage = 1;
        const int32_t kx1 = c->clip.x + (int32_t)c->clip.width, ky1 = c->clip.y + (int32_t)c->clip.height;
        for (uint32_t i = 0; i < count; ++i) {
                int32_t x0 = rects[i].x, y0 = rects[i].y;
                int32_t x1 = rects[i].x + (int32_t)rects[i].width, y1 = rects[i].y + (int32_t)rects[i].height;
                if (x0 < c->clip.x) x0 = c->clip.x;
                if (y0 < c->clip.y) y0 = c->clip.y;
                if (x1 > kx1) x1 = kx1;
                if (y1 > ky1) y1 = ky1;
                if (x1 <= x0 || y1 <= y0) continue;
                const bmds_rect_t r = {x0, y0, (uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
                batch_add(&t, &r);
        }
        batch_flush(&t);
        return;
}

void bmds_game_dma_copy(bmds_game_t *game, uint32_t slot, const bmds_rect_t *rects, uint32_t count) {
        if (slot >= BMDS_SURFACE_IMAGES || game->images[slot].pixels == NULL) return;
        dma_pieces(game, BMDS_BLIT_IMAGE, slot, rects, count);
        return;
}

void bmds_game_dma_fill(bmds_game_t *game, const bmds_rect_t *rects, uint32_t count, uint32_t pixel) {
        dma_pieces(game, BMDS_BLIT_FILL, pixel, rects, count);
        return;
}

void bmds_game_repainted(bmds_game_t *game, const bmds_rect_t *rect) {
        const int32_t b = game->drawing;
        if (b < 0 || game->newest < 0 || game->shown_as[b] == game->presents) return; /* up to date */
        if (game->repainted_count == BMDS_GAME_REPAINTED_MAX) {
                bmds_game_up_to_date(game);
                return;
        }
        game->repainted[game->repainted_count++] = *rect;
        return;
}

/* A frame: a buffer, the program's drawing on it, its damage presented. */
static int frame(bmds_game_t *g) {
        int32_t b = g->held;
        if (b < 0) {
                b = bmds_surface_acquire(g->fd, -1);
                if (b < 0) return b;
        }
        g->held = -1;
        const int repaints = (g->config.options & BMDS_GAME_REPAINTS) != 0;
        if (!repaints) bring_up_to_date(g, b);
        bmds_canvas_t *c = &g->canvas[b];
        bmds_canvas_set_clip(c, NULL);
        bmds_canvas_damage_clear(c);
        g->drawing = b;
        g->repainted_count = 0;
        g->config.draw(g, c);
        /* Up to date but for what was repainted (nothing left to do if the draw asked first). */
        if (repaints) bring_up_to_date_except(g, b, g->repainted, g->repainted_count);
        g->drawing = -1;
        if (c->damage_count == 0) {
                g->held = b; /* nothing drawn: the same buffer next time */
                return 0;
        }
        /* After the last present's frame has begun: this one follows it, none is dropped. */
        if (g->presented) {
                bmds_surface_wait_t wait = {sizeof(wait), (uint32_t)g->last_frame, WAIT_MS};
                (void)bmds_surface_control(g->fd, BMDS_SURFACE_WAIT, &wait, sizeof(wait));
        }
        const int whole = c->damage_count == 1 && c->damage[0].x == 0 && c->damage[0].y == 0 &&
                          c->damage[0].width == (uint32_t)c->width && c->damage[0].height == (uint32_t)c->height;
        bmds_surface_present_t present = {sizeof(present), (uint32_t)b, whole ? NULL : c->damage,
                                          whole ? 0 : c->damage_count};
        const int32_t          shown = bmds_surface_present(g->fd, &present);
        if (shown < 0) return shown;
        g->last_frame = shown;
        g->presented = 1;
        ++g->presents;
        damage_t *d = &g->history[g->presents % HISTORY];
        d->all = whole;
        d->count = whole ? 0 : c->damage_count;
        if (!whole) memcpy(d->rects, c->damage, sizeof(bmds_rect_t) * c->damage_count);
        g->shown_as[b] = g->presents;
        g->newest = b;
        return 0;
}

/* The events waiting, to the program (an inactive surface: ticks stop; CLOSE without a handler: quit). */
static int events(bmds_game_t *g) {
        bmds_surface_event_t e[8];
        for (;;) {
                bmds_pollfd_t p = {sizeof(p), g->fd, BMDS_POLL_IN, 0};
                if (bmds_io_poll(&p, 1, 0, NULL) <= 0 || (p.revents & BMDS_POLL_IN) == 0) return 0;
                const int32_t n = bmds_read(g->fd, e, sizeof(e));
                if (n < 0) return n;
                if (n == 0) return 0;
                for (int32_t i = 0; i < n / (int32_t)sizeof(e[0]) && !g->quit; ++i) {
                        if (e[i].type == BMDS_EVENT_ACTIVE) {
                                const int active = e[i].data[0] != 0;
                                /* Back: ticks go on from now, the time away not made up. */
                                if (active && !g->active) g->origin_ns = now_ns() - (int64_t)g->ticks * g->period_ns;
                                g->active = active;
                        }
                        if (g->config.event != NULL)
                                g->config.event(g, &e[i]);
                        else if (e[i].type == BMDS_EVENT_CLOSE)
                                g->quit = 1;
                }
        }
}

int bmds_game_run(const bmds_game_config_t *config) {
        if (config == NULL || config->size < sizeof(bmds_game_config_t) || config->draw == NULL ||
            config->buffers > MAX_BUFFERS)
                return -BMDS_EINVAL;
        bmds_game_t g;
        memset(&g, 0, sizeof(g));
        g.config = *config;
        g.buffers = config->buffers != 0 ? config->buffers : 2;
        g.newest = -1;
        g.held = -1;
        g.drawing = -1;
        g.period_ns = 1000000000 / (config->tick_hz != 0 ? config->tick_hz : 60);
        g.active = 1; /* until told otherwise: a window opens focused */

        bmds_surface_config_t sc;
        memset(&sc, 0, sizeof(sc));
        sc.size = sizeof(sc);
        sc.width = config->width;
        sc.height = config->height;
        sc.format = config->format;
        sc.buffers = g.buffers;
        sc.flags = config->flags;
        sc.aspect_x = config->aspect_x;
        sc.aspect_y = config->aspect_y;
        sc.title = config->title;
        g.fd = bmds_surface_open(&sc);
        if (g.fd < 0) return g.fd;
        int32_t err = 0;
        if (config->width == 0 || config->height == 0) { /* the system chose (a maximized window's) */
                bmds_surface_info_t info;
                memset(&info, 0, sizeof(info));
                info.size = sizeof(info);
                err = bmds_surface_control(g.fd, BMDS_SURFACE_INFO, &info, sizeof(info));
                if (err == 0 && (info.picture_width == 0 || info.picture_height == 0)) err = -BMDS_ENOSYS;
                g.config.width = info.picture_width;
                g.config.height = info.picture_height;
        }
        config = &g.config;
        for (uint32_t i = 0; i < g.buffers && err == 0; ++i) {
                bmds_surface_buffer_t b;
                memset(&b, 0, sizeof(b));
                b.size = sizeof(b);
                b.index = i;
                err = bmds_surface_buffer(g.fd, &b);
                if (err == 0 && bmds_canvas_init(&g.canvas[i], b.pixels, b.stride, (int32_t)config->width,
                                                 (int32_t)config->height, config->format) != 0)
                        err = -BMDS_EINVAL;
        }

        g.origin_ns = now_ns();
        g.redraw = 1; /* the first frame */
        while (err == 0 && !g.quit) {
                /* Until the next tick is due, or an event; inactive, until an event. */
                int32_t timeout = -1;
                if (g.redraw) {
                        timeout = 0;
                } else if (g.active) {
                        const int64_t due = g.origin_ns + (int64_t)(g.ticks + 1) * g.period_ns - now_ns();
                        timeout = due <= 0 ? 0 : (int32_t)((due + 999999) / 1000000);
                }
                bmds_pollfd_t p = {sizeof(p), g.fd, BMDS_POLL_IN, 0};
                (void)bmds_io_poll(&p, 1, timeout, NULL);
                err = events(&g);
                if (err != 0 || g.quit) break;
                int ran = 0;
                if (g.active && g.config.tick != NULL) {
                        const int64_t  elapsed = now_ns() - g.origin_ns;
                        const uint64_t due = elapsed > 0 ? (uint64_t)(elapsed / g.period_ns) : 0;
                        while (g.ticks < due && ran < MAX_CATCH_UP && !g.quit) {
                                g.config.tick(&g);
                                ++g.ticks;
                                ++ran;
                        }
                        /* Too far behind (a stall): the rest dropped, the game slowed, not rushed. */
                        if (g.ticks < due) g.origin_ns = now_ns() - (int64_t)g.ticks * g.period_ns;
                }
                if (g.quit) break;
                /* Inactive, a frame only when asked (a pause screen, say). */
                if ((g.active && ran > 0) || g.redraw) {
                        g.redraw = 0;
                        err = frame(&g);
                }
        }
        bmds_close(g.fd);
        return err < 0 ? err : 0;
}
