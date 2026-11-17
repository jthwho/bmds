/**
 * @file      screencap.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Watching the screen as a VNC server would (devplan/vnc.md §8, §14): a ScreenShare object's watch and
 * changed, gui_screen_capture's rectangles, image_encode into memory, and injected keys and
 * pointer. While it watches, the panel's tray shows the system's screen-shared icon.
 *
 *     screencap [-o FILE] [-w SECONDS] [-q QUALITY] [-5] [-i]
 *
 *   -o FILE      the screen as a picture: .png, or .jpg (encoded into memory first)
 *   -w SECONDS   follows the changes for that long, putting them on a copy of the screen; then
 *                checks the copy against a whole capture, pixel for pixel, and says what it took
 *   -q QUALITY   the JPEG's (1-100, 80)
 *   -5           captures as RGB565 (the copy and the check in it too)
 *   -i           types "echo screencap" and Enter, then clicks the screen's middle
 */

#include <bmds/guiclasses.h>
#include <bmds/sys.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MaxRects 64

typedef struct Screen {
                bmds_gui_t *gui;
                uint32_t    screen;
                int         rgb565;
                uint32_t    width, height, bpp;
                uint8_t    *picture; // the screen as followed
                uint8_t    *buffer;  // a capture's pixels
                uint32_t    capacity;
                bmds_rect_t rects[MaxRects];
                uint64_t    rectsTaken, bytesTaken, captures;
} Screen;

static uint64_t nowMs(void) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

// One capture (all of it with @p all) put on @p into (the picture's size): 0, or the call's error.
static int capture(Screen *s, int all, uint8_t *into) {
        int more = 1;
        while (more) {
                bmds_screen_capture_t c;
                memset(&c, 0, sizeof(c));
                c.size = sizeof(c);
                c.format = s->rgb565 ? BMDS_PIXEL_RGB565 : BMDS_PIXEL_BGR24;
                c.flags = all ? BMDS_CAPTURE_ALL : 0;
                c.buffer = s->buffer;
                c.capacity = s->capacity;
                c.rects = s->rects;
                c.max_rects = MaxRects;
                const int32_t r = bmds_gui_screen_capture(bmds_gui_fd(s->gui), &c);
                if (r < 0) return (int)r;
                all = 0; // what didn't fit is still marked
                more = (int)c.more;
                if (c.width != s->width || c.height != s->height) {
                        fprintf(stderr, "screencap: the picture is %ux%u now (a full screen?): not followed\n",
                                (unsigned)c.width, (unsigned)c.height);
                        continue;
                }
                const uint8_t *from = s->buffer;
                for (uint32_t i = 0; i < c.rect_count; ++i) {
                        const bmds_rect_t *r2 = &s->rects[i];
                        const size_t       row = (size_t)r2->width * s->bpp;
                        for (uint32_t y = 0; y < r2->height; ++y) {
                                memcpy(into + ((size_t)(r2->y + y) * s->width + (size_t)r2->x) * s->bpp, from, row);
                                from += row;
                        }
                }
                s->rectsTaken += c.rect_count;
                s->bytesTaken += c.bytes;
                ++s->captures;
        }
        return 0;
}

// The picture written as @p path says (.jpg: encoded into memory, then written).
static int save(Screen *s, const char *path, int quality) {
        const size_t n = strlen(path);
        const int    jpeg = n > 4 && (strcmp(path + n - 4, ".jpg") == 0 || strcmp(path + n - 5, ".jpeg") == 0);
        const int    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
                perror(path);
                return 1;
        }
        bmds_image_encode_t e;
        memset(&e, 0, sizeof(e));
        e.size = sizeof(e);
        e.codec = jpeg ? BMDS_IMAGE_JPEG : BMDS_IMAGE_PNG;
        e.format = s->rgb565 ? BMDS_PIXEL_RGB565 : BMDS_PIXEL_BGR24;
        e.width = s->width;
        e.height = s->height;
        e.pixels = s->picture;
        e.quality = (uint32_t)quality;
        int32_t r;
        if (jpeg) {
                // Into memory, as a VNC server's rectangles go (a fifth of the raw size is plenty).
                e.out = s->buffer;
                e.out_capacity = s->capacity;
                const uint64_t t0 = nowMs();
                r = bmds_image_encode(-1, &e);
                if (r >= 0) {
                        printf("screencap: JPEG of %ux%u, %ld bytes, in %llu ms\n", (unsigned)s->width,
                               (unsigned)s->height, (long)r, (unsigned long long)(nowMs() - t0));
                        if (write(fd, s->buffer, (size_t)r) != r) r = -1;
                }
        } else {
                r = bmds_image_encode(fd, &e);
        }
        close(fd);
        if (r < 0) {
                fprintf(stderr, "screencap: %s: encode failed (%ld)\n", path, (long)r);
                return 1;
        }
        return 0;
}

// Types @p text (lower case, digits, spaces) and Enter as HID usages, then clicks the middle.
static void inject(Screen *s, const char *text) {
        for (const char *p = text; *p != '\0'; ++p) {
                int usage = 0;
                if (*p >= 'a' && *p <= 'z') usage = 0x04 + (*p - 'a');
                if (*p >= '1' && *p <= '9') usage = 0x1e + (*p - '1');
                if (*p == '0') usage = 0x27;
                if (*p == ' ') usage = 0x2c;
                if (usage == 0) continue;
                bmds_screen_share_inject_key(s->gui, s->screen, usage, 0, 1);
                bmds_screen_share_inject_key(s->gui, s->screen, usage, 0, 0);
        }
        bmds_screen_share_inject_key(s->gui, s->screen, 0x28, 0, 1); // Enter
        bmds_screen_share_inject_key(s->gui, s->screen, 0x28, 0, 0);
        const int32_t x = (int32_t)s->width / 2, y = (int32_t)s->height / 2;
        bmds_screen_share_inject_pointer(s->gui, s->screen, x, y, 0, 0);
        bmds_screen_share_inject_pointer(s->gui, s->screen, x, y, 1, 0); // the left button down
        bmds_screen_share_inject_pointer(s->gui, s->screen, x, y, 0, 0); // and up
        return;
}

int main(int argc, char **argv) {
        const char *out = NULL;
        int         seconds = 0, quality = 80, typing = 0, opt;
        Screen      s;
        memset(&s, 0, sizeof(s));
        while ((opt = getopt(argc, argv, "o:w:q:5i")) != -1) {
                if (opt == 'o') {
                        out = optarg;
                } else if (opt == 'w') {
                        seconds = atoi(optarg);
                } else if (opt == 'q') {
                        quality = atoi(optarg);
                } else if (opt == '5') {
                        s.rgb565 = 1;
                } else if (opt == 'i') {
                        typing = 1;
                } else {
                        fprintf(stderr, "usage: screencap [-o FILE] [-w SECONDS] [-q QUALITY] [-5] [-i]\n");
                        return 2;
                }
        }
        s.gui = bmds_gui_connect(NULL);
        if (s.gui == NULL) {
                perror("screencap: no GUI");
                return 1;
        }
        if (!bmds_sys_has(BMDS_SYS_GUI_SCREEN_CAPTURE)) {
                fprintf(stderr, "screencap: this system has no screen capture\n");
                return 1;
        }
        bmds_gui_value_t screen;
        if (bmds_gui_display(s.gui, "screen", &screen) != 0) return 1;
        s.width = screen.width;
        s.height = screen.height;
        s.bpp = s.rgb565 ? 2 : 3;
        const size_t bytes = (size_t)s.width * s.height * s.bpp;
        s.picture = malloc(bytes);
        s.capacity = (uint32_t)(bytes / 4); // a quarter a call: big changes take several
        s.buffer = malloc(s.capacity);
        uint8_t *check = seconds > 0 ? malloc(bytes) : NULL;
        if (s.picture == NULL || s.buffer == NULL || (seconds > 0 && check == NULL)) {
                fprintf(stderr, "screencap: out of memory\n");
                return 1;
        }
        s.screen = bmds_screen_share_new(s.gui, 0);
        bmds_screen_share_watch(s.gui, s.screen, 1);
        // The service takes the request in its own time; its first changed (sent at once)
        // says the watch is there, and captures may begin.
        bmds_gui_event_t first;
        int              got;
        while ((got = bmds_gui_next_event(s.gui, &first, 2000)) == 1)
                if (first.source == s.screen && bmds_gui_event_is(s.gui, &first, BMDS_GUI_SCREEN_SHARE_CHANGED)) break;
        if (got != 1) {
                fprintf(stderr, "screencap: the screen can't be watched (no display?)\n");
                return 1;
        }

        uint64_t t0 = nowMs();
        int      r = capture(&s, 1, s.picture);
        if (r < 0) {
                fprintf(stderr, "screencap: capture failed (%d)\n", r);
                return 1;
        }
        printf("screencap: %ux%u whole in %llu ms (%llu calls)\n", (unsigned)s.width, (unsigned)s.height,
               (unsigned long long)(nowMs() - t0), (unsigned long long)s.captures);
        if (typing) inject(&s, "echo screencap");

        if (seconds > 0) {
                s.rectsTaken = s.bytesTaken = s.captures = 0;
                const uint64_t   end = nowMs() + (uint64_t)seconds * 1000u;
                uint64_t         events = 0;
                bmds_gui_event_t e;
                for (uint64_t now = nowMs(); now < end; now = nowMs()) {
                        const int n = bmds_gui_next_event(s.gui, &e, (int)(end - now));
                        if (n < 0) break;
                        if (n == 0) continue;
                        if (e.source != s.screen || !bmds_gui_event_is(s.gui, &e, BMDS_GUI_SCREEN_SHARE_CHANGED))
                                continue;
                        ++events;
                        if (capture(&s, 0, s.picture) < 0) break;
                }
                printf("screencap: %llu changes, %llu captures, %llu rectangles, %llu bytes in %d s\n",
                       (unsigned long long)events, (unsigned long long)s.captures, (unsigned long long)s.rectsTaken,
                       (unsigned long long)s.bytesTaken, seconds);
                // The copy followed by changes alone against a whole capture now. Something can
                // change between the two (a cursor blinking, the tray's CPU meter): a difference
                // that a further round clears was such a race; one that stays is a lost change.
                for (int round = 1; round <= 3; ++round) {
                        (void)capture(&s, 0, s.picture); // the last changes
                        if (capture(&s, 1, check) != 0) break;
                        size_t   differ = 0;
                        uint32_t x0 = s.width, y0 = s.height, x1 = 0, y1 = 0;
                        for (size_t i = 0; i < bytes; ++i) {
                                if (s.picture[i] == check[i]) continue;
                                ++differ;
                                const uint32_t px = (uint32_t)(i / s.bpp);
                                const uint32_t x = px % s.width, y = px / s.width;
                                if (x < x0) x0 = x;
                                if (y < y0) y0 = y;
                                if (x > x1) x1 = x;
                                if (y > y1) y1 = y;
                        }
                        if (differ == 0) {
                                printf("screencap: the followed copy matches (round %d)\n", round);
                                break;
                        }
                        printf("screencap: round %d: %zu bytes differ, in %u,%u-%u,%u\n", round, differ, (unsigned)x0,
                               (unsigned)y0, (unsigned)x1, (unsigned)y1);
                        memcpy(s.picture, check, bytes); // follow on from the whole picture
                }
        }
        int status = 0;
        if (out != NULL) status = save(&s, out, quality);
        bmds_screen_share_watch(s.gui, s.screen, 0);
        bmds_gui_close(s.gui);
        free(check);
        free(s.buffer);
        free(s.picture);
        return status;
}
