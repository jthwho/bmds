/**
 * @file      platform.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Doom's platform on BMDS (devplan/games.md §19): doomgeneric's DG_ calls on a surface (320x200
 * L8 with Doom's palette, its screen copied in), its keys and mouse, its clock and waits, and the
 * program's start (the data folder, the game data). Linked with doomgeneric (GPL-2) by dist/doom;
 * this file is ours.
 */

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <bmds/app.h>
#include <bmds/sys.h>

#include "d_event.h"
#include "doom_bmds.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_video.h"
#include "m_config.h"
#include "m_controls.h"

#define WIDTH 320
#define HEIGHT 200
#define BUFFERS 3
#define QUEUE 64 /* keys: a power of two */
#define TICRATE_HZ 35

/* HID keys (page 7). */
#define KEY(u) (0x00070000u | (u))

/* Doom's, not in its headers. */
extern boolean sendpause;
extern int     usemouse;

/* Doom's "novert" setting (in its list, bound by nothing upstream): up and down don't walk. */
static int novert = 1;

static int32_t  g_fd = -1;
static uint8_t *g_pixels[BUFFERS];
static uint32_t g_stride;
static int32_t  g_held = -1;  /* the buffer Doom draws in (DG_ScreenBuffer), acquired */
static uint8_t *g_own;        /* Doom's buffer when the surface's rows aren't 320 apart */
static int      g_active = 1; /* the surface has the keys */
static int      g_full_screen;
static int      g_paused_by_us;
static uint32_t g_pointer = BMDS_POINTER_NORMAL;

static uint16_t g_queue[QUEUE]; /* pressed << 8 | Doom's key */
static uint32_t g_queue_head, g_queue_tail;
static uint8_t  g_down[256 / 8]; /* Doom's keys held */

static unsigned char g_pad_keys[BMDS_PAD_GUIDE + 1]; /* the key each pad button pressed (0: none) */
static unsigned char g_stick_keys[2];                /* the left stick's: across, then down */

static int      g_mouse_moved;
static int32_t  g_mouse_dx, g_mouse_dy;
static uint32_t g_buttons;

static int     g_clock_started;
static int64_t g_clock_start_ms; /* Doom's clock's start: its first DG_GetTicksMs() */

static int64_t now_ms(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns / 1000000;
}

/* Doom's key for HID @p usage (0: none of Doom's). */
static unsigned char doom_key(uint32_t usage) {
        if (usage >= KEY(0x04) && usage <= KEY(0x1d)) return (unsigned char)('a' + (usage - KEY(0x04)));
        if (usage >= KEY(0x1e) && usage <= KEY(0x26)) return (unsigned char)('1' + (usage - KEY(0x1e)));
        if (usage >= KEY(0x3a) && usage <= KEY(0x45)) {
                static const unsigned char f[] = {KEY_F1, KEY_F2, KEY_F3, KEY_F4,  KEY_F5,  KEY_F6,
                                                  KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12};
                return f[usage - KEY(0x3a)];
        }
        switch (usage) {
                case KEY(0x27): return '0';
                case KEY(0x28):
                case KEY(0x58): return KEY_ENTER;
                case KEY(0x29): return KEY_ESCAPE;
                case KEY(0x2a): return KEY_BACKSPACE;
                case KEY(0x2b): return KEY_TAB;
                case KEY(0x2c): return KEY_USE;
                case KEY(0x2d):
                case KEY(0x56): return KEY_MINUS;
                case KEY(0x2e):
                case KEY(0x57): return KEY_EQUALS;
                case KEY(0x2f): return '[';
                case KEY(0x30): return ']';
                case KEY(0x31): return '\\';
                case KEY(0x33): return ';';
                case KEY(0x34): return '\'';
                case KEY(0x35): return '`';
                case KEY(0x36): return KEY_STRAFE_L; /* , */
                case KEY(0x37): return KEY_STRAFE_R; /* . */
                case KEY(0x38): return '/';
                case KEY(0x39): return KEY_CAPSLOCK;
                case KEY(0x46): return KEY_PRTSCR;
                case KEY(0x47): return KEY_SCRLCK;
                case KEY(0x48): return KEY_PAUSE;
                case KEY(0x49): return KEY_INS;
                case KEY(0x4a): return KEY_HOME;
                case KEY(0x4b): return KEY_PGUP;
                case KEY(0x4c): return KEY_DEL;
                case KEY(0x4d): return KEY_END;
                case KEY(0x4e): return KEY_PGDN;
                case KEY(0x4f): return KEY_RIGHTARROW;
                case KEY(0x50): return KEY_LEFTARROW;
                case KEY(0x51): return KEY_DOWNARROW;
                case KEY(0x52): return KEY_UPARROW;
                case KEY(0xe0):
                case KEY(0xe4): return KEY_FIRE; /* Ctrl */
                case KEY(0xe1):
                case KEY(0xe5): return KEY_RSHIFT;
                case KEY(0xe2):
                case KEY(0xe6): return KEY_RALT;
                default: return 0;
        }
}

static void queue_key(int pressed, unsigned char key) {
        if (g_queue_head - g_queue_tail >= QUEUE) return; /* full: Doom is far behind */
        g_queue[g_queue_head++ % QUEUE] = (uint16_t)(pressed << 8 | key);
        if (pressed)
                g_down[key / 8] |= (uint8_t)(1u << (key % 8));
        else
                g_down[key / 8] &= (uint8_t) ~(1u << (key % 8));
        return;
}

/* A level being played: the mouse is the game's. */
static int playing(void) {
        return gamestate == GS_LEVEL && !menuactive && !paused && !demoplayback;
}

static void set_active(int active) {
        if (active == g_active) return;
        g_active = active;
        if (!active) {
                /* Away: nothing stays held, and a game being played waits. */
                for (unsigned key = 0; key < 256; ++key)
                        if ((g_down[key / 8] & (1u << (key % 8))) != 0) queue_key(0, (unsigned char)key);
                memset(g_pad_keys, 0, sizeof(g_pad_keys));
                memset(g_stick_keys, 0, sizeof(g_stick_keys));
                g_buttons = 0;
                g_mouse_moved = 1;
                if (playing() && !netgame) {
                        sendpause = true;
                        g_paused_by_us = 1;
                }
        } else if (g_paused_by_us) {
                if (paused) sendpause = true;
                g_paused_by_us = 0;
        }
        return;
}

/* ---- Pads (devplan/controls.md §7b), as SDL ports bind them */

/* Doom's key for pad @p button: in the menus South accepts and East goes back. */
static unsigned char pad_key(uint32_t button) {
        switch (button) {
                case BMDS_PAD_UP: return KEY_UPARROW;
                case BMDS_PAD_DOWN: return KEY_DOWNARROW;
                case BMDS_PAD_LEFT: return KEY_LEFTARROW;
                case BMDS_PAD_RIGHT: return KEY_RIGHTARROW;
                case BMDS_PAD_SOUTH: return menuactive ? KEY_ENTER : KEY_FIRE;
                case BMDS_PAD_EAST: return menuactive ? KEY_BACKSPACE : KEY_USE;
                case BMDS_PAD_WEST: return KEY_RSHIFT; /* run */
                case BMDS_PAD_NORTH:
                case BMDS_PAD_R1: return (unsigned char)key_nextweapon;
                case BMDS_PAD_L1: return (unsigned char)key_prevweapon;
                case BMDS_PAD_START: return KEY_ESCAPE;
                case BMDS_PAD_SELECT: return KEY_TAB; /* the map */
                default: return 0;
        }
}

/* The left stick past half way as the arrows: across turns, down walks. */
static void pad_stick(uint32_t axis, int32_t value) {
        const int     i = axis == BMDS_PAD_LEFT_X ? 0 : 1;
        unsigned char key = 0;
        if (value < -16384) key = i == 0 ? KEY_LEFTARROW : KEY_UPARROW;
        if (value > 16384) key = i == 0 ? KEY_RIGHTARROW : KEY_DOWNARROW;
        if (key == g_stick_keys[i]) return;
        if (g_stick_keys[i] != 0) queue_key(0, g_stick_keys[i]);
        if (key != 0) queue_key(1, key);
        g_stick_keys[i] = key;
        return;
}

static void pad(const bmds_surface_event_t *e) {
        if (e->flags == BMDS_PAD_AXIS && (e->data[1] == BMDS_PAD_LEFT_X || e->data[1] == BMDS_PAD_LEFT_Y)) {
                pad_stick(e->data[1], (int32_t)e->data[2]);
                return;
        }
        if (e->flags != BMDS_PAD_BUTTON || e->data[1] > BMDS_PAD_GUIDE) return;
        const uint32_t button = e->data[1];
        if (e->data[2] != 0) {
                const unsigned char key = pad_key(button);
                if (key == 0) return;
                g_pad_keys[button] = key; /* released as pressed, whatever the menu does meanwhile */
                queue_key(1, key);
        } else if (g_pad_keys[button] != 0) {
                queue_key(0, g_pad_keys[button]);
                g_pad_keys[button] = 0;
        }
        return;
}

static void handle(const bmds_surface_event_t *e) {
        switch (e->type) {
                case BMDS_EVENT_KEY: {
                        const unsigned char key = doom_key(e->data[0]);
                        if (key == 0) break;
                        if ((e->flags & BMDS_KEY_UP) != 0) {
                                queue_key(0, key);
                        } else if ((e->flags & BMDS_KEY_DOWN) != 0) {
                                /* Repeats step the menus; pause would flicker. */
                                if ((e->flags & BMDS_KEY_REPEAT) != 0 && key == KEY_PAUSE) break;
                                queue_key(1, key);
                        }
                        break;
                }
                case BMDS_EVENT_POINTER:
                        if (g_pointer != BMDS_POINTER_RELATIVE) break;
                        g_mouse_dx += (int16_t)(e->data[3] & 0xffffu);
                        g_mouse_dy += (int16_t)(e->data[3] >> 16);
                        g_buttons = e->data[2] & 7u; /* left fire, right strafe, middle forward */
                        g_mouse_moved = 1;
                        break;
                case BMDS_EVENT_PAD: pad(e); break;
                case BMDS_EVENT_ACTIVE: set_active(e->data[0] != 0); break;
                case BMDS_EVENT_PRESENTED: g_full_screen = e->data[0] != 0; break;
                case BMDS_EVENT_CLOSE: I_Quit(); break; /* as the menu's quit: the settings saved */
                default: break;
        }
        return;
}

/* The surface's events waiting, handled. */
static void events(void) {
        bmds_surface_event_t e[8];
        for (;;) {
                bmds_pollfd_t p = {sizeof(p), g_fd, BMDS_POLL_IN, 0};
                if (bmds_io_poll(&p, 1, 0, NULL) <= 0 || (p.revents & BMDS_POLL_IN) == 0) return;
                const int32_t n = bmds_read(g_fd, e, sizeof(e));
                if (n <= 0) return;
                for (int32_t i = 0; i < n / (int32_t)sizeof(e[0]); ++i) handle(&e[i]);
        }
}

/* The pointer as the game needs it: held for turning while playing, hidden over a full screen. */
static void update_pointer(void) {
        uint32_t mode = g_full_screen ? BMDS_POINTER_HIDDEN : BMDS_POINTER_NORMAL;
        if (usemouse && g_active && playing()) mode = BMDS_POINTER_RELATIVE;
        if (mode == g_pointer) return;
        if (bmds_surface_control(g_fd, BMDS_SURFACE_POINTER, &mode, sizeof(mode)) == 0) g_pointer = mode;
        if (mode != BMDS_POINTER_RELATIVE) {
                g_buttons = 0;
                g_mouse_moved = 1;
        }
        return;
}

/* Doom's buffer: the next of the surface's, acquired (or its own, copied at each present). */
static void acquire(void) {
        g_held = bmds_surface_acquire(g_fd, -1);
        if (g_held < 0) I_Error("Doom: no buffer to draw in (%d)", (int)g_held);
        DG_ScreenBuffer = g_own != NULL ? g_own : g_pixels[g_held];
        return;
}

void DG_Init(void) {
        /* Weapon cycling, unbound upstream: the pad's North and shoulders use it (saved with the
         * rest, so a binding of the user's own stays). */
        if (key_prevweapon == 0) key_prevweapon = '[';
        if (key_nextweapon == 0) key_nextweapon = ']';
        bmds_surface_config_t c;
        memset(&c, 0, sizeof(c));
        c.size = sizeof(c);
        c.width = WIDTH;
        c.height = HEIGHT;
        c.format = BMDS_PIXEL_L8;
        c.buffers = BUFFERS;
        c.flags = BMDS_SURFACE_FULL_SCREEN;
        c.aspect_x = 4; /* Doom's 320x200 was shown at 4:3 */
        c.aspect_y = 3;
        c.title = "Doom";
        g_fd = bmds_surface_open(&c);
        if (g_fd < 0) I_Error("Doom: no surface (%d)", (int)g_fd);
        for (uint32_t i = 0; i < BUFFERS; ++i) {
                bmds_surface_buffer_t b;
                memset(&b, 0, sizeof(b));
                b.size = sizeof(b);
                b.index = i;
                if (bmds_surface_buffer(g_fd, &b) != 0) I_Error("Doom: no surface buffer %u", (unsigned)i);
                g_pixels[i] = b.pixels;
                g_stride = b.stride;
        }
        /* doomgeneric's own buffer (four bytes a pixel) isn't needed: Doom draws into ours. */
        free(DG_ScreenBuffer);
        DG_ScreenBuffer = NULL;
        if (g_stride != WIDTH) {
                g_own = malloc(WIDTH * HEIGHT);
                if (g_own == NULL) I_Error("Doom: out of memory");
        }
        acquire();

        /* Defaults, before Doom reads its settings: the mouse on, up and down not walking. */
        usemouse = 1;
        M_BindVariable("novert", &novert);
        return;
}

/*
 * Doom's screen, where it draws (doomgeneric patched to ask): internal RAM, as Doom draws a column
 * at a time; its copy into the surface's buffer (PSRAM) at each frame is a fast sequential one.
 */
byte *DG_VideoBuffer(void) {
        return bmds_mem_alloc(WIDTH * HEIGHT, BMDS_MEM_ALIGN, BMDS_MEM_FAST); /* NULL: Doom's zone */
}

void DG_DrawFrame(void) {
        events();
        if (palette_changed) {
                uint32_t rgb[256];
                for (int i = 0; i < 256; ++i)
                        rgb[i] = (uint32_t)colors[i].r << 16 | (uint32_t)colors[i].g << 8 | colors[i].b;
                const bmds_surface_palette_t p = {sizeof(p), 0, 256, rgb};
                (void)bmds_surface_control(g_fd, BMDS_SURFACE_PALETTE, (void *)&p, sizeof(p));
                palette_changed = false;
        }
        if (g_own != NULL)
                for (int y = 0; y < HEIGHT; ++y)
                        memcpy(g_pixels[g_held] + (size_t)y * g_stride, g_own + y * WIDTH, WIDTH);
        bmds_surface_present_t p = {sizeof(p), (uint32_t)g_held, NULL, 0};
        const int32_t          err = bmds_surface_present(g_fd, &p);
        if (err < 0) I_Error("Doom: present failed (%d)", (int)err);
        update_pointer();
        acquire();
        return;
}

/*
 * Every wait in Doom (TryRunTics(), the wipe) is a loop of I_Sleep(1) until its tic count moves,
 * so this sleeps at least @p ms and on to Doom's next tic, handling events meanwhile: a wake a
 * tic, not one a millisecond.
 */
void DG_SleepMs(uint32_t ms) {
        int64_t now = now_ms();
        int64_t until = now + ms;
        if (g_clock_started) {
                const int64_t tic = (now - g_clock_start_ms) * TICRATE_HZ / 1000;
                const int64_t next = g_clock_start_ms + ((tic + 1) * 1000 + TICRATE_HZ - 1) / TICRATE_HZ;
                if (next > until) until = next;
        }
        while (now < until) {
                bmds_pollfd_t p = {sizeof(p), g_fd, BMDS_POLL_IN, 0};
                (void)bmds_io_poll(&p, 1, (int32_t)(until - now), NULL);
                events();
                now = now_ms();
        }
        return;
}

uint32_t DG_GetTicksMs(void) {
        const int64_t now = now_ms();
        /* Doom's clock starts at its first reading (I_GetTime()'s basetime): so does ours. */
        if (!g_clock_started) {
                g_clock_started = 1;
                g_clock_start_ms = now;
        }
        return (uint32_t)now;
}

/* Doom's mouse acceleration (i_video.c's settings), as Chocolate Doom's. */
static int accelerate(int32_t v) {
        if (v < 0) return -accelerate(-v);
        if (v > mouse_threshold) return (int)((v - mouse_threshold) * mouse_acceleration + mouse_threshold);
        return v;
}

int DG_GetKey(int *pressed, unsigned char *key) {
        if (g_queue_head == g_queue_tail) events();
        if (g_queue_head != g_queue_tail) {
                const uint16_t k = g_queue[g_queue_tail++ % QUEUE];
                *pressed = k >> 8;
                *key = (unsigned char)(k & 0xff);
                return 1;
        }
        /* The tic's mouse, summed: Doom keeps only a tic's last motion event. */
        if (g_mouse_moved) {
                event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = ev_mouse;
                ev.data1 = (int)g_buttons;
                ev.data2 = accelerate(g_mouse_dx);
                ev.data3 = novert ? 0 : -accelerate(g_mouse_dy);
                D_PostEvent(&ev);
                g_mouse_dx = g_mouse_dy = 0;
                g_mouse_moved = 0;
        }
        return 0;
}

void DG_SetWindowTitle(const char *title) {
        if (g_fd >= 0 && title != NULL)
                (void)bmds_surface_control(g_fd, BMDS_SURFACE_TITLE, (void *)title, (uint32_t)strlen(title));
        return;
}

/* ---- The program */

/* Doom's own IWADs, in its order (d_iwad.c). */
static const char *const IWADS[] = {"doom2.wad", "plutonia.wad", "tnt.wad",    "doom.wad",      "doom1.wad",
                                    "chex.wad",  "hacx.wad",     "freedm.wad", "freedoom2.wad", "freedoom1.wad"};

static int has_arg(int argc, char **argv, const char *name) {
        for (int i = 1; i < argc; ++i)
                if (strcasecmp(argv[i], name) == 0) return 1;
        return 0;
}

/* The first IWAD in @p dirs' order into @p path, or false. */
static int find_iwad(const char *const *dirs, int ndirs, char *path, size_t size) {
        for (int d = 0; d < ndirs; ++d) {
                if (dirs[d] == NULL) continue;
                for (size_t i = 0; i < sizeof(IWADS) / sizeof(IWADS[0]); ++i) {
                        const int n = snprintf(path, size, "%s/%s", dirs[d], IWADS[i]);
                        if (n > 0 && (size_t)n < size && access(path, R_OK) == 0) return 1;
                }
        }
        return 0;
}

int main(int argc, char **argv) {
        /* The data folder is Doom's current directory: its settings, saves and screenshots. */
        static char data[PATH_MAX];
        const char *data_dir = NULL;
        if (bmds_app_data_dir(data, sizeof(data)) == 0 && chdir(data) == 0) data_dir = data;

        /* Ours after the user's: Doom takes an option's first. */
        static char iwad[PATH_MAX];
        char      **args = calloc((size_t)argc + 3, sizeof(char *));
        if (args == NULL) return 1;
        memcpy(args, argv, sizeof(char *) * (size_t)argc);
        int n = argc;
        if (!has_arg(argc, argv, "-iwad")) {
                const char *const dirs[] = {data_dir, bmds_app_dir()};
                if (!find_iwad(dirs, 2, iwad, sizeof(iwad))) {
                        fprintf(stderr,
                                "doom: no game data: put a WAD (doom.wad, doom2.wad, ...) in %s, or "
                                "install Freedoom\n",
                                data_dir != NULL ? data_dir : "the data folder");
                        return 1;
                }
                args[n++] = "-iwad";
                args[n++] = iwad;
        }
        args[n] = NULL;

        if (!has_arg(argc, argv, "-nosound") && !has_arg(argc, argv, "-nomusic")) doom_bmds_sound_early();
        doomgeneric_Create(n, args);
        for (;;) doomgeneric_Tick();
}
