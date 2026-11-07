/**
 * @file      cores.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libbmds3d on two cores (devplan/3d.md §7, §12b): a helper thread on the other core draws the
 * sorted queue's rows from a split down while the caller draws the rows above it. Both read the
 * queue (sorted once) and write different rows of the canvas: the P4's cores share one L1 data
 * cache, so nothing is written back between them. The split moves each frame towards equal times,
 * since the other core has its own work (a full-screen presenter, the window manager).
 */

#include <pthread.h>
#include <string.h>
#include <time.h>
#include "3d_internal.h"

struct bmds_3d_helper_t {
                pthread_t       thread;
                pthread_mutex_t lock;
                pthread_cond_t  wake, done;
                uint32_t        asked, finished; /* jobs: asked by the caller, finished by the helper */
                int             quit;
                bmds_3d_t      *ctx;
                /* The job: its band, the queue's order, and what it drew. */
                bmds_3d_area_t   area;
                const uint16_t  *order;
                uint32_t         n;
                bmds_3d_bounds_t bounds;
                uint32_t         pixels, drawn;
                int64_t          ns; /* its time */
};

static int64_t now_ns(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* Keeps the calling thread to @p core, if the kernel can. */
static void keep_to(int32_t core) {
        if (core < 0 || !bmds_sys_has(BMDS_SYS_SCHED)) return;
        bmds_sched_t s;
        memset(&s, 0, sizeof(s));
        s.size = sizeof(s);
        s.cores = 1u << core;
        (void)bmds_sched(BMDS_SCHED_PIN, 0, &s);
        return;
}

static int32_t current_core(void) {
        if (!bmds_sys_has(BMDS_SYS_SCHED)) return -1;
        bmds_sched_t s;
        memset(&s, 0, sizeof(s));
        s.size = sizeof(s);
        if (bmds_sched(BMDS_SCHED_GET, 0, &s) != 0 || s.core < 0 || s.core > 1) return -1;
        return s.core;
}

typedef struct {
                bmds_3d_helper_t *h;
                int32_t           core;
} start_t;

static void *helper_main(void *arg) {
        bmds_3d_helper_t *h = (bmds_3d_helper_t *)arg;
        pthread_mutex_lock(&h->lock);
        for (;;) {
                while (h->asked == h->finished && !h->quit) pthread_cond_wait(&h->wake, &h->lock);
                if (h->quit) break;
                pthread_mutex_unlock(&h->lock);
                const int64_t t0 = now_ns();
                h->bounds = (bmds_3d_bounds_t){0, 0, 0, 0};
                h->drawn = 0;
                h->pixels = bmds_3d_raster_queue(h->ctx, &h->area, h->order, h->n, &h->bounds, &h->drawn);
                h->ns = now_ns() - t0;
                pthread_mutex_lock(&h->lock);
                ++h->finished;
                pthread_cond_signal(&h->done);
        }
        pthread_mutex_unlock(&h->lock);
        return NULL;
}

static void *helper_entry(void *arg) {
        start_t s = *(start_t *)arg;
        keep_to(s.core);
        return helper_main(s.h);
}

int bmds_3d_helper_start(bmds_3d_t *ctx) {
        bmds_3d_helper_t *h = (bmds_3d_helper_t *)bmds_mem_alloc(sizeof(*h), 0, 0);
        if (h == NULL) return -BMDS_ENOMEM;
        memset(h, 0, sizeof(*h));
        h->ctx = ctx;
        pthread_mutex_init(&h->lock, NULL);
        pthread_cond_init(&h->wake, NULL);
        pthread_cond_init(&h->done, NULL);
        /* The caller keeps to its core, the helper takes the other. */
        const int32_t core = current_core();
        keep_to(core);
        start_t s = {h, core >= 0 ? 1 - core : -1};
        /* A small stack (3 KB used rasterizing bands on the board, 2026-11-06; at least 8 KB, for system calls):
         * a thread's default 32 KB goes in internal RAM while there is room, which full screen and
         * the synths need too. */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 12u * 1024u);
        const int made = pthread_create(&h->thread, &attr, helper_entry, &s);
        pthread_attr_destroy(&attr);
        if (made != 0) {
                pthread_cond_destroy(&h->done);
                pthread_cond_destroy(&h->wake);
                pthread_mutex_destroy(&h->lock);
                bmds_mem_free(h);
                return -BMDS_ENOMEM;
        }
        /* Its start block read before it goes: the first job waits for nothing else. */
        pthread_mutex_lock(&h->lock);
        ++h->asked;
        h->n = 0;
        h->order = NULL;
        pthread_cond_signal(&h->wake);
        while (h->finished != h->asked) pthread_cond_wait(&h->done, &h->lock);
        pthread_mutex_unlock(&h->lock);
        ctx->helper = h;
        ctx->share = 0.5f;
        return 0;
}

void bmds_3d_helper_stop(bmds_3d_t *ctx) {
        bmds_3d_helper_t *h = ctx->helper;
        if (h == NULL) return;
        pthread_mutex_lock(&h->lock);
        h->quit = 1;
        pthread_cond_signal(&h->wake);
        pthread_mutex_unlock(&h->lock);
        pthread_join(h->thread, NULL);
        pthread_cond_destroy(&h->done);
        pthread_cond_destroy(&h->wake);
        pthread_mutex_destroy(&h->lock);
        bmds_mem_free(h);
        ctx->helper = NULL;
        return;
}

uint32_t bmds_3d_helper_raster(bmds_3d_t *ctx, const bmds_3d_area_t *area, const uint16_t *order, uint32_t n,
                               bmds_3d_bounds_t *bounds, uint32_t *drawn) {
        bmds_3d_helper_t *h = ctx->helper;
        const int32_t     rows = area->iy1 - area->iy0;
        if (rows < 2) return bmds_3d_raster_queue(ctx, area, order, n, bounds, drawn);
        int32_t split = area->iy0 + (int32_t)(ctx->share * (float)rows + 0.5f);
        if (split <= area->iy0) split = area->iy0 + 1;
        if (split >= area->iy1) split = area->iy1 - 1;
        bmds_3d_area_t top = *area, bottom = *area;
        top.iy1 = split, top.y1 = (float)split;
        bottom.iy0 = split, bottom.y0 = (float)split;

        pthread_mutex_lock(&h->lock);
        h->area = bottom;
        h->order = order;
        h->n = n;
        ++h->asked;
        pthread_cond_signal(&h->wake);
        pthread_mutex_unlock(&h->lock);

        const int64_t t0 = now_ns();
        uint32_t      mine = 0;
        uint32_t      pixels = bmds_3d_raster_queue(ctx, &top, order, n, bounds, &mine);
        const int64_t own = now_ns() - t0;

        pthread_mutex_lock(&h->lock);
        while (h->finished != h->asked) pthread_cond_wait(&h->done, &h->lock);
        pthread_mutex_unlock(&h->lock);
        pixels += h->pixels;
        /* A triangle across the split counts on both sides: the larger count is nearer the truth. */
        *drawn += mine > h->drawn ? mine : h->drawn;
        if (h->bounds.x1 > h->bounds.x0)
                bmds_3d_include(bounds, h->bounds.x0, h->bounds.y0, h->bounds.x1, h->bounds.y1);

        /* Towards equal times: the top band's rows scaled by how much faster it was, half way. */
        if (own > 0 && h->ns > 0) {
                const float rate_top = ctx->share / (float)own, rate_bottom = (1.0f - ctx->share) / (float)h->ns;
                const float want = rate_top / (rate_top + rate_bottom);
                float       share = ctx->share + 0.5f * (want - ctx->share);
                ctx->share = share < 0.1f ? 0.1f : share > 0.9f ? 0.9f : share;
        }
        return pixels;
}
