/**
 * @file      guisurface.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A canvas drawn through its surface (the CANVAS-SURFACE extension, devplan/guiprograms.md §11
 * 3c): a plasma drawn by the CPU into the canvas's buffers and presented each frame, as a game
 * draws, inside an ordinary window beside a label. The canvas follows the window's size (a new
 * surface on each `resized`). The label shows the frames a second and what drawing and
 * presenting took. With -n FRAMES it draws that many as fast as it can (with -p presenting only,
 * not drawing), prints the averages, and ends.
 *
 *     guisurface [-n FRAMES] [-p] [-s WIDTHxHEIGHT]
 */

#include <bmds/guiclasses.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/sys.h>

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

static uint16_t palette[256];
static uint8_t  wave[1024];

static void make_tables(void) {
        for (int i = 0; i < 256; ++i) {
                /* Around the colour wheel, RGB565. */
                const int r = i < 85 ? 255 - i * 3 : i < 170 ? 0 : (i - 170) * 3;
                const int g = i < 85 ? i * 3 : i < 170 ? 255 - (i - 85) * 3 : 0;
                const int b = i < 85 ? 0 : i < 170 ? (i - 85) * 3 : 255 - (i - 170) * 3;
                palette[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
        /* A triangle wave: 0..127..0 over 1024 steps, no floating point. */
        for (int i = 0; i < 1024; ++i) wave[i] = (uint8_t)(i < 512 ? i / 4 : (1023 - i) / 4);
        return;
}

static void draw(uint8_t *pixels, uint32_t stride, uint32_t width, uint32_t height, uint32_t t) {
        for (uint32_t y = 0; y < height; ++y) {
                uint16_t     *row = (uint16_t *)(pixels + y * stride);
                const uint8_t wy = wave[(y * 3 + t * 5) & 1023];
                for (uint32_t x = 0; x < width; ++x) row[x] = palette[(uint8_t)(wave[(x * 2 + t * 7) & 1023] + wy + t)];
        }
        return;
}

int main(int argc, char **argv) {
        int      frames = 0, present_only = 0;
        unsigned want_w = 640, want_h = 480;
        int      opt;
        while ((opt = getopt(argc, argv, "n:ps:")) != -1) {
                if (opt == 'n')
                        frames = atoi(optarg);
                else if (opt == 'p')
                        present_only = 1;
                else if (opt == 's' && sscanf(optarg, "%ux%u", &want_w, &want_h) == 2)
                        continue;
                else {
                        fprintf(stderr, "usage: guisurface [-n FRAMES] [-p] [-s WIDTHxHEIGHT]\n");
                        return 2;
                }
        }
        bmds_gui_t *gui = bmds_gui_connect(NULL);
        if (gui == NULL) {
                perror("guisurface: no GUI");
                return 1;
        }
        if (!bmds_gui_has_extension(gui, "CANVAS-SURFACE")) {
                fprintf(stderr, "guisurface: the service gives canvases no surfaces (a remote one?)\n");
                return 1;
        }
        make_tables();
        const uint32_t window = bmds_window_new(gui, 0);
        bmds_window_set_title(gui, window, "Canvas surface");
        const uint32_t canvas = bmds_canvas_new(gui, window);
        bmds_canvas_set_preferred_width(gui, canvas, (int32_t)want_w);
        bmds_canvas_set_preferred_height(gui, canvas, (int32_t)want_h);
        const uint32_t label = bmds_label_new(gui, window);
        bmds_label_set_text(gui, label, "Starting...");
        bmds_window_show(gui, window);

        int      fd = -1, running = 1;
        uint32_t width = 0, height = 0, t = 0, count = 0, total = 0;
        int64_t  drawing = 0, presenting = 0, since = now_ns(), started = 0;
        while (running) {
                bmds_gui_event_t e;
                int              r;
                /* Events first; without a surface, wait for them. */
                while ((r = bmds_gui_next_event(gui, &e, fd < 0 ? -1 : 0)) == 1) {
                        if (e.source == window && bmds_gui_event_is(gui, &e, BMDS_GUI_WINDOW_CLOSED)) running = 0;
                        if (e.source == canvas && bmds_gui_event_is(gui, &e, BMDS_GUI_CANVAS_RESIZED) && e.count == 2) {
                                if (fd >= 0) close(fd);
                                fd = bmds_gui_canvas_surface_open(gui, canvas, NULL); /* its new size */
                                width = (uint32_t)e.values[0].number;
                                height = (uint32_t)e.values[1].number;
                                if (fd < 0) perror("guisurface: a surface");
                                if (started == 0) started = now_ns();
                        }
                        if (fd < 0) break;
                }
                if (r < 0) break;
                if (!running || fd < 0) continue;
                const int64_t t0 = now_ns();
                const int32_t index = bmds_surface_acquire(fd, 1000);
                if (index < 0) continue;
                bmds_surface_buffer_t b = {sizeof(b), (uint32_t)index, NULL, 0};
                if (bmds_surface_buffer(fd, &b) < 0) break;
                if (!present_only) draw(b.pixels, b.stride, width, height, t++);
                const int64_t          t1 = now_ns();
                bmds_surface_present_t p = {sizeof(p), (uint32_t)index, NULL, 0};
                const int32_t          frame = bmds_surface_present(fd, &p);
                const int64_t          t2 = now_ns();
                drawing += t1 - t0;
                presenting += t2 - t1;
                ++count;
                ++total;
                if (frames > 0 && total >= (uint32_t)frames) break;
                if (frames == 0 && frame >= 0) {
                        /* At the display's pace: the next frame begun before drawing again. */
                        bmds_surface_wait_t w = {sizeof(w), (uint32_t)frame, 100};
                        (void)bmds_surface_control(fd, BMDS_SURFACE_WAIT, &w, sizeof(w));
                }
                const int64_t now = now_ns();
                if (now - since >= 1000000000) {
                        char text[96];
                        snprintf(text, sizeof(text), "%ux%u: %u fps, drawing %.1f ms, presenting %.2f ms", width,
                                 height, (unsigned)(count * 1000000000ll / (now - since)),
                                 (double)drawing / count / 1e6, (double)presenting / count / 1e6);
                        bmds_label_set_text(gui, label, text);
                        bmds_gui_flush(gui);
                        count = 0;
                        drawing = presenting = 0;
                        since = now;
                }
        }
        if (frames > 0 && total > 0) {
                bmds_surface_info_t info = {sizeof(info)};
                (void)bmds_surface_control(fd, BMDS_SURFACE_INFO, &info, sizeof(info));
                const int64_t elapsed = now_ns() - started;
                printf("guisurface: %ux%u, %u frames in %lld ms: %.1f ms a frame (%s), %u dropped\n", width, height,
                       total, (long long)(elapsed / 1000000), (double)elapsed / total / 1e6,
                       present_only ? "presents only" : "drawn and presented", (unsigned)info.dropped);
        }
        if (fd >= 0) close(fd);
        bmds_gui_close(gui);
        return 0;
}
