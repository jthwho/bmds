/**
 * @file      boot.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Part 0 (devplan/demo.md §3): the boot. A BIOS screen of the 1990s, its facts this board's own
 * (the chip, the cores, the memory counted up, the serial number, the host name) and a handful of
 * checks picked at random from a long list, so each run reads differently; and it talks, in a
 * voice picked at random, each line made by the speech thread while the screen types
 * (voice.h). DEL asks for SETUP. Its script ended, the show starts the music, and the first beat
 * breaks the screen into the demo.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bmds/palette.h>
#include "../demo.h"
#include "../voice.h"

#define COLS 80
#define ROWS 22
#define LINES 32      /* the script's at most */
#define SCREEN_SLOT 2 /* the 2D-DMA's image slot (the copper's 0, the balls' 1) */

/* The palette: the BIOS's text colours, and the logo's cycling entries. */
enum {
        INK_BLACK,
        INK_GREY,
        INK_WHITE,
        INK_YELLOW,
        INK_CYAN,
        INK_GREEN,
        INK_DIM,
        INK_RED,
        LOGO_FIRST = 16
};
#define LOGO_COUNT 32

/* The speech's slots (voice.h): the show's others are above. */
enum {
        SAY_HELLO,
        SAY_MEMORY,
        SAY_QUIP_A,
        SAY_QUIP_B,
        SAY_END,
        SAY_SETUP,
        SAY_COUNT
};

/* A check of the list: what it says, its answer, and what the BIOS remarks aloud (NULL: nothing). */
typedef struct {
                const char *what;
                const char *answer;
                const char *remark;
} check_t;

static const check_t checks[] = {
        {"Checking for a GPU", "none found", "No G P U detected. Every pixel will be drawn by hand."},
        {"Reticulating splines", "done", NULL},
        {"Calibrating the flux capacitor", "1.21 GW", "One point twenty one jigga watts. Great Scott!"},
        {"Polishing the pixels", "shiny", NULL},
        {"Teaching both cores to share", "mostly OK", "Both cores agree to share. Mostly."},
        {"Looking for a floppy drive", "not found", "No floppy drive. Not since nineteen ninety eight."},
        {"Bribing the DMA controller", "accepted", NULL},
        {"Herding the interrupts", "all present", NULL},
        {"Asking the cache nicely", "it said yes", NULL},
        {"Downloading more RAM", "failed", "Downloading more RAM failed. We will make do."},
        {"Searching for the any key", "not found", "I can't find the any key."},
        {"Loading Windows", "just kidding", "Loading Windows. Just kidding."},
        {"Spinning up the hard disk", "no disk, no spin", NULL},
        {"Tuning the FM operators", "in tune", "The F M synthesizer is in tune. Probably."},
        {"Warming up the palette", "256 colours", NULL},
        {"Counting the sheep", NULL, NULL}, /* a number, made up */
        {"Checking the turbo button", "always on", NULL},
        {"Negotiating with the PPA", "agreed", NULL},
        {"Measuring the beat", "140 BPM", "The beat is one hundred and forty per minute. Get ready."},
        {"Untangling the sine tables", "4096 entries", NULL},
        {"Hiding the mouse pointer", "done", "I have hidden the mouse. You will not need it."},
        {"Reversing the polarity", "reversed", NULL},
        {"Compiling the shaders", "no shaders", "There are no shaders. There is only software."},
        {"Checking the coffee level", "low", "Coffee level low. Proceeding anyway."},
        {"Locating the 3D accelerator", "it's you", NULL},
        {"Feeding the speech synthesizer", "talking", NULL},
};
#define CHECKS ((uint32_t)(sizeof(checks) / sizeof(checks[0])))
#define PICKED 6

static const char *const hellos[] = {
        "Power on self test. Stand by.", "Good day. I am the BIOS.", "Hello. Waking up.",
        "Boot sequence engaged.",        "Ah. You're back.",
};
static const char *const endings[] = {
        "All systems nominal. Here we go.",  "Starting demo one. Hold on to your pixels.",
        "Booting the demo. Turn it up.",     "Demo loading. Please enjoy responsibly.",
        "That's enough checking. Let's go.",
};
#define COUNT_OF(a) ((uint32_t)(sizeof(a) / sizeof(a[0])))

/* A line of the screen: its text (the dots and answer typed after), when it appears, a line said. */
typedef struct {
                char     text[COLS + 1];
                char     answer[24];
                uint8_t  ink, answer_ink;
                float    at;       /* the script's seconds */
                float    dots;     /* seconds its dots take (0: none) */
                int      say;      /* SAY_..., or -1 */
                uint32_t count_kb; /* the memory line: counted up to this */
} line_t;

static struct {
                line_t            lines[LINES];
                int               count;
                int               shown; /* lines on the screen */
                float             clock; /* the script's seconds (held while a line waits for its voice) */
                float             wait;  /* how long the line due has waited */
                int               setup; /* DEL pressed: SETUP refused */
                bmds_game_speak_t how;
                int               asked; /* the lines given to the speech thread */
                uint32_t          palette[PAL_PARTS];
                uint8_t          *screen; /* the text screen, drawn as it changes, copied by the 2D-DMA */
                bmds_game_image_t screen_image;
                int               changed; /* drawn since given to the 2D-DMA */
                bmds_canvas_t     canvas;
                int               top;         /* the line at its top */
                int               look[LINES]; /* what each line shows as drawn (-1: not drawn) */
                char              said[SAY_COUNT][160];
                float             end; /* the script's end */
} s;

static int init(demo_t *demo) {
        (void)demo;
        s.palette[INK_BLACK] = 0x000000;
        s.palette[INK_GREY] = 0xaaaaaa;
        s.palette[INK_WHITE] = 0xffffff;
        s.palette[INK_YELLOW] = 0xffff55;
        s.palette[INK_CYAN] = 0x55ffff;
        s.palette[INK_GREEN] = 0x55ff55;
        s.palette[INK_DIM] = 0x555555;
        s.palette[INK_RED] = 0xff5555;
        for (uint32_t i = 0; i < LOGO_COUNT; ++i)
                s.palette[LOGO_FIRST + i] = bmds_palette_hsv((float)i / LOGO_COUNT, 0.8f, 1.0f);
        s.screen = bmds_mem_alloc(DEMO_WIDTH * DEMO_HEIGHT, 64, 0); /* the 2D-DMA reads it */
        if (s.screen == NULL) return -BMDS_ENOMEM;
        const int err = bmds_canvas_init(&s.canvas, s.screen, DEMO_WIDTH, DEMO_WIDTH, DEMO_HEIGHT, BMDS_PIXEL_L8);
        return err != 0 ? err : bmds_canvas_image(&s.canvas, NULL, &s.screen_image);
}

#define LOGO_X (DEMO_WIDTH - 4 * 24 - 16)

/* The logo, top right: BMDS in letters of the cycling rainbow, an energy star of sorts. */
static void draw_logo(void) {
        static const char logo[] = "BMDS";
        const int32_t     big = bmds_game_font_scaled(3);
        if (big >= 0)
                for (int i = 0; i < 4; ++i) {
                        const char letter[2] = {logo[i], '\0'};
                        (void)bmds_canvas_text(&s.canvas, big, LOGO_X + 24 * i, 8, letter,
                                               LOGO_FIRST + (uint32_t)(i * 6) % LOGO_COUNT);
                }
        (void)bmds_canvas_text(&s.canvas, BMDS_GAME_FONT_DEFAULT, LOGO_X + 34, 56, "energy", INK_GREEN);
        return;
}

static line_t *add(float *at, float gap, uint8_t ink, const char *text) {
        if (s.count == LINES) return NULL;
        line_t *l = &s.lines[s.count++];
        memset(l, 0, sizeof(*l));
        *at += gap;
        l->at = *at;
        l->ink = ink;
        l->answer_ink = INK_WHITE;
        l->say = -1;
        snprintf(l->text, sizeof(l->text), "%s", text);
        return l;
}

/* A check's line: its words, dots to column 40, the answer typed after them. */
static line_t *add_check(float *at, float gap, const char *what, const char *answer, float dots) {
        char text[COLS + 1];
        snprintf(text, sizeof(text), "%-34s", what);
        line_t *l = add(at, gap, INK_GREY, text);
        if (l == NULL) return NULL;
        snprintf(l->answer, sizeof(l->answer), "%s", answer);
        l->dots = dots;
        *at += dots;
        return l;
}

/* The script, made afresh each time: the facts read now, the checks and the lines picked. */
static void enter(demo_t *demo) {
        pal_set_base(&demo->pal, s.palette, 0.0f);
        s.count = s.shown = 0;
        s.clock = s.wait = 0.0f;
        s.setup = 0;
        s.asked = 0;
        s.top = -1; /* drawn afresh at the first frame */
        demo->boot_done = 0;
        bmds_system_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (bmds_system_info(&info) != 0) {
                info.cores = 2;
                info.cpu_mhz = 360;
                snprintf(info.board, sizeof(info.board), "ESP32-P4");
        }
        bmds_mem_info_t psram, fast;
        memset(&psram, 0, sizeof(psram));
        memset(&fast, 0, sizeof(fast));
        psram.size = fast.size = sizeof(psram);
        (void)bmds_mem_info(0, &psram);
        (void)bmds_mem_info(BMDS_MEM_FAST, &fast);
        const uint32_t psram_kb = psram.total_bytes / 1024, fast_kb = fast.total_bytes / 1024;

        /* The voice: one of two accents, a little lower and quicker or slower each run. */
        static const char *const voices[] = {"en-US", "en-GB"};
        memset(&s.how, 0, sizeof(s.how));
        s.how.size = sizeof(s.how);
        s.how.voice = voices[demo_rand_below(2)];
        s.how.pitch = 72 + demo_rand_below(25);
        s.how.speed = 95 + demo_rand_below(20);

        char  text[COLS + 1];
        float at = 0.0f;
        snprintf(text, sizeof(text), "BMDS Modular BIOS v%s, An Energy Star Ally",
                 info.version[0] != '\0' ? info.version : "1.0");
        add(&at, 0.6f, INK_WHITE, text);
        add(&at, 0.0f, INK_GREY, "Copyright (C) 2026, Bare Metal Do Stuff Inc.");
        line_t *l = add(&at, 0.5f, INK_GREY, "");
        snprintf(s.said[SAY_HELLO], sizeof(s.said[0]), "%s", hellos[demo_rand_below(COUNT_OF(hellos))]);
        if (l != NULL) l->say = SAY_HELLO;
        snprintf(text, sizeof(text), "%s, chip rev v%u.%u", info.board, info.chip_revision / 100,
                 info.chip_revision % 100);
        add(&at, 0.3f, INK_YELLOW, text);
        snprintf(text, sizeof(text), "Main Processor : RISC-V, %u cores @ %u MHz, no GPU", info.cores, info.cpu_mhz);
        add(&at, 0.4f, INK_GREY, text);
        l = add(&at, 0.4f, INK_GREY, "Memory Testing : ");
        if (l != NULL) {
                l->count_kb = psram_kb;
                l->dots = 1.4f;
                snprintf(l->answer, sizeof(l->answer), "%uK OK", psram_kb);
                at += 1.4f;
        }
        snprintf(s.said[SAY_MEMORY], sizeof(s.said[0]), "Memory test passed. %u megabytes.", psram_kb / 1024);
        snprintf(text, sizeof(text), "Internal SRAM  : %uK", fast_kb);
        l = add(&at, 0.3f, INK_GREY, text); /* said once the count is done */
        if (l != NULL) l->say = SAY_MEMORY;
        add(&at, 0.4f, INK_GREY, "");

        /* The checks: PICKED of them, shuffled; two of those with a remark said. */
        uint32_t order[CHECKS];
        demo_shuffle(order, CHECKS);
        int remarks = 0;
        for (uint32_t i = 0; i < PICKED; ++i) {
                const check_t *c = &checks[order[i]];
                char           answer[24];
                if (c->answer == NULL)
                        snprintf(answer, sizeof(answer), "%u found", 1000 + demo_rand_below(9000));
                else
                        snprintf(answer, sizeof(answer), "%s", c->answer);
                l = add_check(&at, 0.25f, c->what, answer, 0.4f + 0.5f * demo_frand());
                if (l != NULL && c->remark != NULL && remarks < 2) {
                        const int say = remarks == 0 ? SAY_QUIP_A : SAY_QUIP_B;
                        snprintf(s.said[say], sizeof(s.said[0]), "%s", c->remark);
                        l->say = say;
                        ++remarks;
                }
        }
        for (int say = SAY_QUIP_A + remarks; say <= SAY_QUIP_B; ++say) s.said[say][0] = '\0';
        add(&at, 0.4f, INK_GREY, "");
        snprintf(text, sizeof(text), "Serial Number  : %s", info.serial[0] != '\0' ? info.serial : "000000000000");
        add(&at, 0.2f, INK_GREY, text);
        snprintf(text, sizeof(text), "Host Name      : %s", info.hostname[0] != '\0' ? info.hostname : "bmds");
        add(&at, 0.1f, INK_GREY, text);
        add(&at, 0.4f, INK_GREY, "");
        add(&at, 0.3f, INK_GREY, "Press DEL to enter SETUP, ESC to leave");
        l = add_check(&at, 0.6f, "Verifying DMI Pool Data", "OK", 0.8f);
        l = add(&at, 0.4f, INK_WHITE, "Booting BMDS Demo #1 from /sd0/p0/apps/demo1.app");
        snprintf(s.said[SAY_END], sizeof(s.said[0]), "%s", endings[demo_rand_below(COUNT_OF(endings))]);
        if (l != NULL) l->say = SAY_END;
        s.end = at + 0.3f;
        snprintf(s.said[SAY_SETUP], sizeof(s.said[0]),
                 "Setup is not available. This BIOS is only a demo. But nice try.");
        return;
}

static void event(demo_t *demo, const bmds_surface_event_t *e) {
        (void)demo;
        if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0 && e->data[0] == 0x0007004cu && !s.setup) {
                s.setup = 1; /* DEL */
                (void)voice_queue(SAY_SETUP, 0.1f, 0.9f, 0.0f);
        }
        return;
}

static void frame(demo_t *demo, bmds_canvas_t *c, float t, float dt) {
        (void)t;
        /* The lines to say, once the speech is there (the show opens it a few frames in). */
        if (!s.asked && voice_audio() != NULL) {
                for (int say = 0; say < SAY_COUNT; ++say)
                        if (s.said[say][0] != '\0') voice_say(say, s.said[say], &s.how);
                s.asked = 1;
        }
        /* The script moves on, but a line that talks waits (a second and a half at most) for its voice. */
        if (s.shown < s.count && s.clock + dt >= s.lines[s.shown].at) {
                line_t   *l = &s.lines[s.shown];
                const int state = l->say >= 0 && voice_audio() != NULL ? voice_state(l->say) : VOICE_READY;
                if (state == VOICE_QUEUED && s.wait < 1.5f) {
                        s.wait += dt;
                } else {
                        /* Said after the line before it ends (the screen goes on meanwhile). */
                        const float ends = l->say >= 0 ? voice_queue(l->say, 0.25f, 0.9f, 0.0f) : -1.0f;
                        if (ends >= 0.0f && l->say == SAY_END) {
                                /* The music starts as the last line ends (its own start takes ~0.3 s). */
                                const float until = s.clock + ends - 0.1f;
                                if (until > s.end) s.end = until;
                        }
                        s.wait = 0.0f;
                        ++s.shown;
                        s.clock += dt;
                }
        } else {
                s.clock += dt;
        }
        while (s.shown < s.count && s.lines[s.shown].say < 0 && s.clock >= s.lines[s.shown].at) ++s.shown;
        if (s.shown == s.count && s.clock >= s.end) demo->boot_done = 1;

        /* The screen: each line drawn when what it shows changes (text is a system call a string),
           the picture copied out each frame. */
        const int first = s.shown > ROWS - 1 ? s.shown - (ROWS - 1) : 0;
        if (first != s.top) {
                s.top = first; /* scrolled: every line again */
                bmds_canvas_clear(&s.canvas, INK_BLACK);
                draw_logo();
                for (int i = 0; i < LINES; ++i) s.look[i] = -1;
                s.changed = 1;
        }
        int32_t y = 0;
        for (int i = first; i < s.shown; ++i, y += 16) {
                line_t     *l = &s.lines[i];
                const float into = s.clock - l->at;
                const int   done = l->dots == 0.0f || into >= l->dots || i + 1 < s.shown;
                const float f = into < 0.0f ? 0.0f : into / l->dots;
                const int   look = done               ? 1000
                                   : l->count_kb != 0 ? (int)(f * (float)(l->count_kb / 1024))
                                                      : (int)(6.0f * f);
                if (look == s.look[i]) continue;
                s.look[i] = look;
                s.changed = 1;
                bmds_canvas_fill(&s.canvas, 0, y, LOGO_X - 8, 16, INK_BLACK);
                (void)bmds_canvas_text(&s.canvas, BMDS_GAME_FONT_DEFAULT, 0, y, l->text, l->ink);
                if (l->count_kb != 0) {
                        /* The memory, counted up in megabytes as it is "tested". */
                        char n[24];
                        if (done)
                                snprintf(n, sizeof(n), "%s", l->answer);
                        else
                                snprintf(n, sizeof(n), "%uK", (uint32_t)look * 1024);
                        (void)bmds_canvas_text(&s.canvas, BMDS_GAME_FONT_DEFAULT, 17 * 8, y, n,
                                               done ? INK_WHITE : INK_GREY);
                } else if (l->dots != 0.0f) {
                        const int dots = done ? 6 : look;
                        (void)bmds_canvas_text(&s.canvas, BMDS_GAME_FONT_DEFAULT, 34 * 8, y, &"......"[6 - dots],
                                               INK_DIM);
                        if (done)
                                (void)bmds_canvas_text(&s.canvas, BMDS_GAME_FONT_DEFAULT, 41 * 8, y, l->answer,
                                                       l->answer_ink);
                }
        }
        if (s.changed) {
                (void)bmds_game_dma_image(demo->game, SCREEN_SLOT, &s.screen_image); /* given again: written back */
                s.changed = 0;
        }
        const bmds_rect_t whole = {0, 0, DEMO_WIDTH, DEMO_HEIGHT};
        bmds_game_dma_copy(demo->game, SCREEN_SLOT, &whole, 1);
        pal_rotate(&demo->pal, LOGO_FIRST, LOGO_COUNT, 1);
        if (s.setup) {
                bmds_canvas_fill(c, 120, DEMO_HEIGHT - 60, DEMO_WIDTH - 240, 36, INK_RED);
                (void)bmds_canvas_text(c, BMDS_GAME_FONT_DEFAULT, 136, DEMO_HEIGHT - 50,
                                       "SETUP is locked: this BIOS is only a demo", INK_WHITE);
        }
        /* The cursor, blinking under the last line. */
        if (((int)(s.clock * 3.0f) & 1) == 0) bmds_canvas_fill(c, 0, y + 12, 8, 2, INK_GREY);
        bmds_canvas_damage_all(c);
        return;
}

const part_t part_boot = {"Boot", init, enter, frame, event};
