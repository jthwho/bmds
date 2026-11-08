/**
 * @file      demo1.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * BMDS Demo #1 (devplan/demo.md): a demoscene-style show of the system. The skeleton (step 1): a
 * 640×360 L8 picture full screen, the parts in turn, the show's palette, and the overlay. The music
 * (step 3): the song (song.c) played by the SDK's sequencer, each frame told where the music is at
 * the moment the frame is seen (sync_t), each section's cue choosing the part.
 *
 * The rest (step 6): the boot (a BIOS that talks) before the music, the credits at its end, lines
 * said on the sections' cues (picked at random each time, made ahead and played on the beat), and
 * a transition into each section: the old part veiled, wiped or torn, the new one revealed.
 *
 * Keys: Space the next part; 1-9 a part by number, 0 the boot (until the next section); M the music
 * off and on (from the start); W FM or the wavetable (from the start); P pause the picture; F the
 * overlay (frames a second, the time a frame and its drawing, each core's busy share, the music's
 * place); Escape ends it.
 *
 *     demo1 [-w] [-F] [-n] [-1] [-s]   (-w: in a window; -F: FM, not the wavetable; -n: no music;
 *                                       -1: once, then exit; -s: silent, no speech either)
 *
 * The music is on the wavetable unless its instruments can't be loaded (no SoundFont, or not
 * enough memory): then on FM.
 */

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <bmds/game.h>
#include <bmds/palette.h>
#include "demo.h"
#include "voice.h"

#define KEY_ESCAPE 0x00070029u
#define KEY_SPACE 0x0007002cu
#define KEY_P 0x00070013u
#define KEY_F 0x00070009u
#define KEY_M 0x00070010u
#define KEY_W 0x0007001au
#define KEY_1 0x0007001eu
#define KEY_9 0x00070026u
#define KEY_0 0x00070027u

enum {
        P_COPPER,
        P_PLASMA,
        P_STARS,
        P_TUNNEL,
        P_FIRE,
        P_BALLS,
        P_SOLIDS,
        P_ROTO,
        P_CREDITS,
        P_BOOT
};
static const part_t *const parts[] = {&part_copper, &part_plasma, &part_stars, &part_tunnel,  &part_fire,
                                      &part_balls,  &part_solids, &part_roto,  &part_credits, &part_boot};
#define PARTS ((int)(sizeof(parts) / sizeof(parts[0])))

/* The show's timeline: each section's part (song.h). */
static const int timeline[SONG_SECTIONS] = {
        [SONG_INTRO] = P_COPPER,  [SONG_BUILD] = P_PLASMA, [SONG_DROP_A] = P_STARS,
        [SONG_GROOVE] = P_TUNNEL, [SONG_BREAK] = P_FIRE,   [SONG_BUILD_2] = P_BALLS,
        [SONG_DROP_B] = P_SOLIDS, [SONG_FINALE] = P_ROTO,  [SONG_OUTRO] = P_CREDITS,
};

/* ---- Lines on cues: an exchange picked at random for each section, its first turn said on its
   cue (its first beat or a bar in), the others each after the last ends; the duo take turns
   ('|' between turns; "%u": the cores' MHz), either one starting. */
#define SAY_CUE 8   /* voice.h's slots: each section its own four, above the parts' */
#define CUE_TURNS 4 /* turns at most */
#define CUE_AHEAD 3 /* sections ahead their lines are asked for */

/* A section's lines this time round. */
typedef struct {
                int     asked, played;
                int     turns;
                int64_t at; /* when the first is heard (0: not known yet) */
} cue_state_t;

typedef struct {
                uint32_t           bar; /* into the section */
                const char *const *lines;
                uint32_t           count;
} cue_t;

#define LINES(...)                                                                                                     \
        (const char *const[]){__VA_ARGS__},                                                                            \
                (uint32_t)(sizeof((const char *const[]){__VA_ARGS__}) / sizeof(const char *))
static const cue_t cues[SONG_SECTIONS] = {
        [SONG_INTRO] = {1, LINES("Round two.|Again! Again!", "Back to the start.|Same demo, new numbers.",
                                 "Did we just loop?|We did. Act natural.")},
        [SONG_BUILD] = {2, LINES("Not one pixel of this picture moves.|Only the palette does.",
                                 "What is this, a plasma?|A sum of sines. Nothing more.",
                                 "Two hundred and fifty two colours.|All of them cycling.|Show off.")},
        [SONG_DROP_A] = {0, LINES("Look! It's bare metal, doing stuff!", "Drop it!|Dropping it.",
                                  "Here we go!|Hold on to something.", "Is that bare metal?|Doing stuff. Yes!")},
        [SONG_GROOVE] = {1,
                         LINES("Steer the tunnel with the mouse.|What mouse? You hid it.|Try anyway.",
                               "A tunnel from a lookup table.|Are we there yet?|It's a tunnel. We are never there.")},
        [SONG_BREAK] = {1, LINES("The music is synthesized live, on the same chip.|While it draws all this?|While it "
                                 "draws all this.",
                                 "Fire. In software.|No G P U was harmed.",
                                 "Is it getting hot in here?|That's the fire effect.")},
        [SONG_BUILD_2] = {1, LINES("This is an E S P 32 P 4.|%u megahertz.|And no G P U.",
                                   "Two hundred and sixteen balls.|Sorted every frame.|Every frame?|Every frame.")},
        [SONG_DROP_B] = {1, LINES("Every pixel you see was drawn by software.|Even that one?|Especially that one.",
                                  "Gouraud shading.|On a microcontroller?|On a microcontroller.",
                                  "Six hundred and forty triangles.|And counting.")},
        [SONG_FINALE] = {0, LINES("One more time!|One more time!",
                                  "A texture, turned and zoomed.|Then a cube wearing a plasma.|Fashion.",
                                  "Textures, on a microcontroller.|Why not?")},
        [SONG_OUTRO] = {0, NULL, 0}, /* the duo's chat (below) */
};

/* ---- The credits' chat: two bits picked at random from this pool, and a sign-off, the duo
   taking turns. Retro machines, graphics, films and programmers' jokes. */
#define CHAT_SLOT (SAY_CUE + CUE_TURNS * SONG_OUTRO) /* voice.h's slots: its own eight, the last */
#define CHAT_TURNS 8
_Static_assert(SONG_OUTRO == SONG_SECTIONS - 1 && CHAT_SLOT + CHAT_TURNS <= VOICE_SLOTS, "the slots");

static const char *const chat_bits[] = {
        "Remember the Commodore sixty four?|Sixty four kilobytes. We thought that was a lot.|This board has "
        "five hundred times that.",
        "Ten print, chr dollar, two oh five point five, plus r n d one. Go to ten.|Gesundheit.",
        "Is this running on a Z eighty?|No, a RISC V. But it has the same attitude.",
        "How many programmers does it take to change a light bulb?|None. That's a hardware problem.",
        "Why do programmers prefer dark mode?|Because light attracts bugs.",
        "There are ten kinds of people in the world.|Those who understand binary, and those who don't.",
        "Open the pod bay doors, HAL.|I'm sorry Dave, I'm afraid I can't do that.|Why not?|No floppy drive.",
        "Shall we play a game?|How about global thermonuclear war?|How about Doom? It runs on this, you know.",
        "Enhance!|It's a sixteen by sixteen icon. There is nothing to enhance.",
        "Is this the Matrix?|No. The Matrix had a G P U.",
        "Use the Force!|I'd rather use the D M A controller.",
        "Did you blow on the cartridge?|There is no cartridge.|Then what have I been blowing on?",
        "It's a Unix system! I know this!|It's BMDS, actually.|I don't know this.",
        "Press play on tape.|Searching.|Found demo one.|Loading. Please wait three minutes.",
        "Why did the programmer quit?|He didn't get arrays.",
        "It works on my machine.|This is your machine.",
        "Who needs a G P U, with two cores and a dream?|And a lot of lookup tables.",
        "My first computer had sixteen colours.|This demo has two hundred and fifty six.|Show off.",
        "Beam me up!|Sorry. The transporter needs a G P U.",
        "Have you tried turning it off and on again?|It boots so fast, I do it for fun.",
        "In space, no one can hear you underrun.|We fixed those.|Mostly.",
        "What's a sprite?|A little picture that moves.|Like me?|You're more of a waveform.",
        "Do androids dream of electric sheep?|This one dreams of more internal RAM.",
        "Mode thirteen h!|Three twenty by two hundred. The good old days.",
        "Poke fifty three two eighty, zero.|A black border. Very classy.",
        "Hello, world.|Hello yourself.",
        "I'll be back.|Only if the song loops.",
        "Roads? Where we're going, we don't need roads.|Or a G P U.",
        "What's the best thing about a Boolean?|Even if you're wrong, you're only off by a bit.",
        "Knock knock.|Race condition.|Who's there?",
        "Are those scan lines?|Those are memories.",
        "Was that a blitter?|Two dimensional D M A. Same thing, more letters.",
};
static const char *const chat_ends[] = {
        "Thanks for watching!|See you next frame.",
        "That's all, folks.|Same time next loop?",
        "Thanks for watching!|Don't forget to like and subscribe.|This is a microcontroller.|Fine. Just clap, "
        "then.",
        "Thank you, and good night!|It's daytime.|Not in here.",
        "Greetings to everyone watching!|Especially the ones still reading the credits.",
};
#define COUNT_OF(a) ((uint32_t)(sizeof(a) / sizeof(a[0])))

static uint32_t turns_in(const char *text) {
        uint32_t n = 1;
        for (; *text != '\0'; ++text) n += *text == '|';
        return n;
}

/* The chat into @p text: two bits (one if two don't fit), then a sign-off. */
static void chat(char *text, size_t size) {
        const char *end = chat_ends[demo_rand_below(COUNT_OF(chat_ends))];
        const char *a = chat_bits[demo_rand_below(COUNT_OF(chat_bits))], *b = NULL;
        for (int tries = 0; tries < 8 && b == NULL; ++tries) {
                const char *c = chat_bits[demo_rand_below(COUNT_OF(chat_bits))];
                if (c != a && turns_in(a) + turns_in(c) + turns_in(end) <= CHAT_TURNS) b = c;
        }
        if (b != NULL)
                snprintf(text, size, "%s|%s|%s", a, b, end);
        else
                snprintf(text, size, "%s|%s", a, end);
        return;
}

static struct {
                demo_t  demo;
                int     part, next; /* shown, and asked for (-1: none) */
                float   t;          /* seconds into the part */
                int     paused, overlay;
                int64_t last_ns;
                /* The music. */
                bmds_seq_t *seq;
                int         wavetable;
                int         probe;   /* the wavetable's instruments: 0 being loaded, 1 there, -1 not (LOAD/STORE) */
                int         music;   /* frames until the music starts (0: not to) */
                int32_t     section; /* the last frame's (-1: none yet) */
                int64_t     last_at; /* the last frame's moment seen */
                /* The overlay's figures, a second at a time. */
                int64_t  second_ns, busy_us[2];
                uint32_t frames, fps;
                float    frame_ms, draw_ms, busy[2];
                uint32_t counters[2];
                int      have_counters;
                int      pointer_hidden;
                /* The rest. */
                int         once;               /* -1: the song once, then the end */
                int         silent;             /* -s: no speech */
                uint32_t    frames_drawn;       /* in all */
                cue_state_t cue[SONG_SECTIONS]; /* each section's lines */
                int32_t     cue_now;            /* the section they last saw (-1: none) */
                uint32_t    mhz;
                /* The transition into the next section: its style, and the switch's tear. */
                int   style;
                float reveal; /* 1 at a switch, to 0 */
                int   first_switch;
} show;

static bmds_seq_song_t once_song; /* -1's: the song, ending */

uint32_t demo_rand(void) {
        static uint32_t x;
        if (x == 0) {
                (void)bmds_random(&x, sizeof(x));
                if (x == 0) x = 0x9e3779b9u;
        }
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
}

void demo_shuffle(uint32_t *order, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) order[i] = i;
        for (uint32_t i = count; i > 1; --i) {
                const uint32_t j = demo_rand_below(i), t = order[i - 1];
                order[i - 1] = order[j];
                order[j] = t;
        }
        return;
}

float demo_sines[DEMO_SINES];

void demo_init_sines(void) {
        for (int i = 0; i < DEMO_SINES; ++i) demo_sines[i] = sinf(6.28318531f * (float)i / DEMO_SINES);
        return;
}

static int64_t now_ns(void) {
        int64_t ns = 0;
        bmds_clock_get(BMDS_CLOCK_MONOTONIC, &ns);
        return ns;
}

/* Each core's busy time so far (µs), from the system's counters; 0 if they can't be read. */
static int busy_now(int64_t *us) {
        if (!show.have_counters) {
                for (uint32_t i = 0;; ++i) {
                        bmds_counter_t c;
                        memset(&c, 0, sizeof(c));
                        c.size = sizeof(c);
                        if (bmds_stats_list(i, &c) != 0) break;
                        if (strcmp(c.name, "cpu.core0.busy_us") == 0) show.counters[0] = c.id;
                        if (strcmp(c.name, "cpu.core1.busy_us") == 0) show.counters[1] = c.id;
                }
                show.have_counters = show.counters[0] != 0 && show.counters[1] != 0 ? 1 : -1;
        }
        return show.have_counters > 0 && bmds_stats_read(show.counters, 2, us, NULL, 0) == 2;
}

/* Whether the wavetable can play: a synth of our own opened with it loads its SoundFont (into the
   system's cache, so the music's first note doesn't wait for it either), then closed. Seconds
   from the card: on a thread, during the boot. */
static void *probe_wavetable(void *arg) {
        (void)arg;
        bmds_synth_config_t c;
        memset(&c, 0, sizeof(c));
        c.size = sizeof(c);
        c.format = BMDS_AUDIO_F32LE;
        c.engine = BMDS_SYNTH_ENGINE_WAVETABLE;
        const int32_t fd = bmds_audio_synth(&c);
        if (fd >= 0) {
                bmds_close(fd);
        } else {
                fprintf(stderr, "demo1: the wavetable: %s; FM instead\n", strerror(-fd));
        }
        __atomic_store_n(&show.probe, fd >= 0 ? 1 : -1, __ATOMIC_RELEASE);
        return NULL;
}

/* Plays the song from its start on the FM synth or the wavetable (ports by name); quiet if it can't. */
static void music_start(void) {
        bmds_seq_config_t c;
        memset(&c, 0, sizeof(c));
        c.size = sizeof(c);
        c.port = show.wavetable ? "wavetable" : "fm"; /* by name: /dev/midi is whichever the user set */
        c.latency_us = -1;
        /* The music is all scheduled and the frames drawn for when they're seen: the synth may
         * render 250 ms ahead, so drawing (the presenter above it) doesn't make it miss a period
         * (devplan/audio.md §27). */
        c.allow_latency_us = 250000;
        int err = bmds_seq_open(&c, show.once ? &once_song : song_get(), &show.seq);
        if (err < 0 && show.wavetable) {
                fprintf(stderr, "demo1: music (wavetable): %s; FM instead\n", strerror(-err));
                show.wavetable = 0;
                c.port = "fm";
                err = bmds_seq_open(&c, show.once ? &once_song : song_get(), &show.seq);
        }
        if (err == 0) err = bmds_seq_play(show.seq, 0);
        if (err < 0) {
                fprintf(stderr, "demo1: music (%s): %s\n", c.port, strerror(-err));
                bmds_seq_close(show.seq);
                show.seq = NULL;
        }
        show.section = -1;
        memset(show.cue, 0, sizeof(show.cue));
        show.cue_now = -1;
        show.demo.seq = show.seq;
        show.demo.wavetable = show.wavetable;
        voice_duck(show.seq); /* the music down under each line */
        return;
}

static void music_stop(void) {
        voice_duck(NULL);
        bmds_seq_close(show.seq);
        show.seq = NULL;
        show.demo.seq = NULL;
        return;
}

/* The section after @p section, round the loop (-1: none). */
static int32_t section_after(int32_t section) {
        if (section + 1 < SONG_SECTIONS) return section + 1;
        return show.once ? -1 : 0;
}

/* A section's lines' first slot: each section its own (the chat eight). */
static int cue_base(int32_t section) {
        return section == SONG_OUTRO ? CHAT_SLOT : SAY_CUE + CUE_TURNS * section;
}

/* Picks @p section's lines (an exchange, or the chat) and asks for them to be made. */
static void cue_ask(int32_t section) {
        cue_state_t *q = &show.cue[section];
        memset(q, 0, sizeof(*q));
        q->asked = 1;
        const int chatting = section == SONG_OUTRO;
        if (cues[section].count == 0 && !chatting) {
                q->played = 1; /* nothing to say */
                return;
        }
        char text[1200];
        if (chatting)
                chat(text, sizeof(text));
        else
                snprintf(text, sizeof(text), cues[section].lines[demo_rand_below(cues[section].count)], show.mhz);
        const int most = chatting ? CHAT_TURNS : CUE_TURNS;
        int       who = (int)demo_rand_below(2);
        for (char *turn = text; turn != NULL && q->turns < most; who ^= 1) {
                char *bar = strchr(turn, '|');
                if (bar != NULL) *bar++ = '\0';
                voice_say(cue_base(section) + q->turns++, turn, &show.demo.duo[who]);
                turn = bar;
        }
        return;
}

/* Places @p section's lines on its cue once made (at least half the mixer's latency ahead), the
   first on its beat and the others after it; dropped if made too late. */
static void cue_place(int32_t section, int64_t seen) {
        cue_state_t *q = &show.cue[section];
        if (!q->asked || q->played) return;
        if (q->at == 0 && bmds_seq_time_of(show.seq, (uint32_t)section, cues[section].bar, 0, seen, &q->at) != 0)
                q->at = 0;
        if (q->at == 0) return;
        const int base = cue_base(section);
        /* Its cue passed this time round (the time given is then the next loop's), or too near now:
           not said, and its lines dropped at once, made or not, so the speech goes on to those it
           can still say instead of making these (once behind, it never caught up). */
        if ((section == show.section && q->at - seen > 30000000000ll) ||
            q->at - seen < bmds_game_audio_latency_ns(voice_audio()) / 2) {
                q->played = 1;
                for (int i = 0; i < q->turns; ++i) voice_drop(base + i);
                return;
        }
        for (int i = 0; i < q->turns; ++i)
                if (voice_state(base + i) != VOICE_READY) return;
        /* On its beat, or after the chat still going (round the loop): never over another line. */
        if (voice_said_end() + 200000000 > q->at) {
                for (int i = 0; i < q->turns; ++i) (void)voice_queue(base + i, 0.2f, 1.0f, 0.0f);
                q->played = 1;
                return;
        }
        if (voice_play(base, q->at, 1.0f, 0.0f) < 0) return;
        q->played = 1;
        for (int i = 1; i < q->turns; ++i) (void)voice_queue(base + i, 0.2f, 1.0f, 0.0f);
        return;
}

/* The lines: each section's asked for three sections ahead (the speech is made on the time the
   drawing and the synth leave, SCHED_IDLE: a busy part can starve it for seconds; asked one
   ahead, a line beside the fire part was made after its cue), placed as its cue nears, freed two
   sections after (asked afresh for the next loop: other lines). */
static void cue_lines(int64_t seen) {
        if (show.silent || voice_audio() == NULL || show.section < 0) return;
        if (show.section != show.cue_now) {
                show.cue_now = show.section;
                const int32_t old = (show.section + SONG_SECTIONS - 2) % SONG_SECTIONS;
                if (show.cue[old].asked) {
                        for (int i = 0; i < show.cue[old].turns; ++i) voice_drop(cue_base(old) + i);
                        show.cue[old].asked = 0;
                }
        }
        int32_t ahead = show.section;
        for (int k = 0; k < CUE_AHEAD && (ahead = section_after(ahead)) >= 0; ++k)
                if (!show.cue[ahead].asked) cue_ask(ahead);
        cue_place(show.section, seen);
        const int32_t next = section_after(show.section);
        if (next >= 0) cue_place(next, seen);
        return;
}

/* ---- Transitions: in the last half beat of a section the part goes, as the style says; the next
   is revealed over the first. */
enum {
        STYLE_FADE,
        STYLE_WHITE,
        STYLE_BLINDS,
        STYLE_TEAR,
        STYLES
};
#define BLIND 20 /* rows a blind */

/* Rows of @p c moved sideways by up to @p amount of the width, a band of 8 rows at a time. */
static void tear(bmds_canvas_t *c, float amount) {
        if (amount <= 0.01f) return;
        for (int32_t y = 0; y < c->height; y += 8) {
                const int32_t shift = (int32_t)((demo_frand() - 0.5f) * amount * 0.4f * (float)c->width);
                if (shift == 0 || demo_rand_below(3) == 0) continue;
                for (int32_t r = y; r < y + 8 && r < c->height; ++r) {
                        uint8_t *row = c->pixels + (size_t)r * c->stride;
                        if (shift > 0) {
                                memmove(row + shift, row, (size_t)(c->width - shift));
                                memset(row, PAL_PANEL, (size_t)shift);
                        } else {
                                memmove(row, row - shift, (size_t)(c->width + shift));
                                memset(row + c->width + shift, PAL_PANEL, (size_t)-shift);
                        }
                }
        }
        return;
}

/* The transition over the part drawn: @p out (0..1) how far the old part has gone, the new
   one's reveal (1..0) after the switch. */
static void transition(demo_t *d, bmds_canvas_t *c, float out) {
        const float gone = out > show.reveal ? out : show.reveal;
        switch (show.style) {
                case STYLE_FADE: pal_veil(&d->pal, 0x000000, gone); break;
                case STYLE_WHITE: pal_veil(&d->pal, 0xffffff, out * 0.85f); break;
                case STYLE_BLINDS: {
                        const int32_t rows = (int32_t)(gone * BLIND + 0.5f);
                        if (rows > 0)
                                for (int32_t y = 0; y < c->height; y += BLIND)
                                        bmds_canvas_fill(c, 0, y, c->width, rows, PAL_PANEL);
                        pal_veil(&d->pal, 0x000000, 0.0f);
                        break;
                }
                case STYLE_TEAR:
                        tear(c, gone);
                        pal_veil(&d->pal, 0x000000, out * 0.5f);
                        break;
                default: break;
        }
        return;
}

/* A track's last note at @p at, its loudness falling by e in @p tau seconds. */
static float envelope(int64_t at, uint32_t track, float tau) {
        bmds_seq_hit_t h;
        if (bmds_seq_last_hit(show.seq, at, track, &h) != 0) return 0.0f;
        const float age = (float)(at - h.time_ns) * 1e-9f;
        return age > 6.0f * tau ? 0.0f : (float)h.velocity / 127.0f * expf(-age / tau);
}

/* The music at @p at, the moment this frame is seen. */
static void update_sync(demo_t *d, int64_t at) {
        sync_t *y = &d->sync;
        memset(y, 0, sizeof(*y));
        y->pos.size = sizeof(y->pos);
        const int got = show.seq != NULL && bmds_seq_position(show.seq, at, &y->pos) == 0;
        y->known = got && !y->pos.ended;
        /* -1: the song over and the chat said. */
        if (got && y->pos.ended && show.once && voice_said_end() < at) bmds_game_quit(d->game);
        y->bar_seconds = 240.0f / (y->known ? y->pos.bpm : 140.0f);
        if (y->known) {
                y->kick = envelope(at, SONG_KICK, 0.15f);
                y->clap = envelope(at, SONG_CLAP, 0.2f);
                y->hat = envelope(at, SONG_HATS, 0.06f);
                y->bass = envelope(at, SONG_BASS, 0.1f);
                y->pulse = 1.0f - y->pos.beat_phase;
                bmds_seq_hit_t hits[32];
                const uint32_t n = bmds_seq_hits(show.seq, show.last_at, at,
                                                 1u << SONG_KICK | 1u << SONG_CLAP | 1u << SONG_HATS, hits, 32);
                for (uint32_t i = 0; i < n; ++i) {
                        if (hits[i].track == SONG_KICK) ++y->kicks;
                        if (hits[i].track == SONG_CLAP) ++y->claps;
                        if (hits[i].track == SONG_HATS) ++y->hats;
                }
                /* A new section: its part (a part chosen by key holds until then), revealed as the
                   last transition's style says; the next one's picked. */
                if ((int32_t)y->pos.section != show.section) {
                        show.section = (int32_t)y->pos.section;
                        show.next = timeline[show.section];
                        show.reveal = 1.0f;
                        if (show.style == STYLE_WHITE) pal_flash(&d->pal, 0xffffff, 0.9f, 3.0f);
                }
        }
        show.last_at = at;
        return;
}

static void on_event(bmds_game_t *game, const bmds_surface_event_t *e) {
        demo_t *d = &show.demo;
        if (e->type == BMDS_EVENT_CLOSE) {
                bmds_game_quit(game);
                return;
        }
        if (e->type == BMDS_EVENT_KEY && (e->flags & BMDS_KEY_DOWN) != 0) {
                const uint32_t key = e->data[0];
                if (key == KEY_ESCAPE)
                        bmds_game_quit(game);
                else if (key == KEY_SPACE)
                        show.next = (show.part + 1) % PARTS;
                else if (key == KEY_0)
                        show.next = P_BOOT;
                else if (key >= KEY_1 && key <= KEY_9 && (int)(key - KEY_1) < PARTS)
                        show.next = (int)(key - KEY_1);
                else if (key == KEY_P)
                        show.paused = !show.paused;
                else if (key == KEY_F)
                        show.overlay = !show.overlay;
                else if (key == KEY_M && show.seq != NULL)
                        music_stop();
                else if (key == KEY_M)
                        music_start();
                else if (key == KEY_W) {
                        show.wavetable = !show.wavetable;
                        music_stop();
                        music_start();
                } else if (parts[show.part]->event != NULL)
                        parts[show.part]->event(d, e);
                return;
        }
        if (parts[show.part]->event != NULL) parts[show.part]->event(d, e);
        if (e->type == BMDS_EVENT_POINTER) {
                const int32_t x = (int32_t)e->data[0], y = (int32_t)e->data[1];
                d->pointer_x = x == BMDS_POINTER_GONE ? -1 : x;
                d->pointer_y = x == BMDS_POINTER_GONE ? -1 : y;
                d->button = (e->data[2] & 1u) != 0;
        }
        return;
}

/* A tick a frame (60 a second) is what makes the loop draw; the show's time is the clock's. */
static void on_tick(bmds_game_t *game) {
        (void)game;
        return;
}

static void draw_overlay(bmds_canvas_t *c) {
        char line[160];
        snprintf(line, sizeof(line), " %u fps  frame %.1f ms  draw %.1f ms  core 0 %.0f%%  core 1 %.0f%%  %d/%d %s%s ",
                 (unsigned)show.fps, (double)show.frame_ms, (double)show.draw_ms, (double)show.busy[0],
                 (double)show.busy[1], show.part + 1, PARTS, parts[show.part]->name, show.paused ? "  (paused)" : "");
        (void)bmds_canvas_text_flags(c, BMDS_GAME_FONT_DEFAULT, 0, 0, line, PAL_TEXT, PAL_PANEL, BMDS_TEXT_OPAQUE);
        const sync_t *y = &show.demo.sync;
        if (show.seq == NULL)
                snprintf(line, sizeof(line), " music off ");
        else if (!y->known)
                snprintf(line, sizeof(line), " music starting ");
        else
                snprintf(line, sizeof(line), " %s %s %u.%u.%u  %.0f BPM  %.1f s  latency %.1f ms ",
                         show.wavetable ? "wavetable" : "fm", y->pos.cue, y->pos.bar + 1, y->pos.beat + 1,
                         y->pos.step + 1, (double)y->pos.bpm, y->pos.seconds,
                         (double)bmds_seq_latency_ns(show.seq) / 1e6);
        (void)bmds_canvas_text_flags(c, BMDS_GAME_FONT_DEFAULT, 0, bmds_game_line_height(BMDS_GAME_FONT_DEFAULT), line,
                                     PAL_DIM, PAL_PANEL, BMDS_TEXT_OPAQUE);
        return;
}

static void on_draw(bmds_game_t *game, bmds_canvas_t *c) {
        demo_t       *d = &show.demo;
        const int64_t start = now_ns();
        const float   dt = show.last_ns != 0 ? (float)(start - show.last_ns) / 1e9f : 0.0f;
        d->game = game;
        show.last_ns = start;
        if (!show.pointer_hidden) {
                /* No arrow over the show; the pointer still moves (and steers) and presses. */
                uint32_t mode = BMDS_POINTER_HIDDEN;
                show.pointer_hidden =
                        bmds_surface_control(bmds_game_fd(game), BMDS_SURFACE_POINTER, &mode, sizeof(mode)) == 0;
        }
        ++show.frames_drawn;
        /* The speech a few frames in, the music once the boot is over (or left): not before the
         * loop, since a synth starting allocates internal RAM, which full screen needs too (its
         * scan's descriptors); asked first, it went to a window. */
        if (show.frames_drawn == 3 && !show.silent) {
                const int err = voice_open(250);
                if (err < 0) fprintf(stderr, "demo1: speech: %s\n", strerror(-err));
        }
        /* The wavetable asked whether it can play, a frame after the speech (also a synth's
           internal RAM: after full screen). */
        if (show.frames_drawn == 4 && show.music && show.wavetable) {
                pthread_t t;
                if (pthread_create(&t, NULL, probe_wavetable, NULL) == 0)
                        pthread_detach(t);
                else
                        show.probe = 1; /* not asked: tried as it starts */
        }
        const int probed = !show.wavetable || __atomic_load_n(&show.probe, __ATOMIC_ACQUIRE) != 0;
        if (show.music && show.frames_drawn >= 15 && probed && (d->boot_done || show.part != P_BOOT)) {
                show.music = 0;
                if (show.wavetable && __atomic_load_n(&show.probe, __ATOMIC_ACQUIRE) < 0) show.wavetable = 0;
                music_start();
        }
        if (!show.music && show.seq == NULL && d->boot_done && show.part == P_BOOT && show.next < 0)
                show.next = P_COPPER; /* no music: on at once */
        const int64_t seen = bmds_game_frame_ns(game);
        d->seen_ns = seen != 0 ? seen : start;
        update_sync(d, d->seen_ns);
        if (show.next >= 0) {
                show.part = show.next;
                show.next = -1;
                show.t = 0.0f;
                parts[show.part]->enter(d);
        }
        const float step = show.paused ? 0.0f : (dt > 0.1f ? 0.1f : dt); /* a stall doesn't jump the show */
        show.t += step;
        if (show.seq != NULL) cue_lines(d->seen_ns);

        /* Every pixel repainted: the loop copies nothing forward. */
        const bmds_rect_t whole = {0, 0, (uint32_t)c->width, (uint32_t)c->height};
        bmds_game_repainted(game, &whole);
        parts[show.part]->frame(d, c, show.t, step);

        /* The transition: how far into the last half beat before the next section. */
        float out = 0.0f;
        if (d->sync.known) {
                const int32_t next = section_after(show.section);
                int64_t       at = 0;
                const float   len = 30.0f / d->sync.pos.bpm;
                if (next >= 0 && bmds_seq_time_of(show.seq, (uint32_t)next, 0, 0, d->seen_ns, &at) == 0) {
                        const float left = (float)(at - d->seen_ns) * 1e-9f;
                        if (left < len) out = 1.0f - left / len;
                }
        }
        if (out == 0.0f && show.reveal > 0.0f) {
                show.reveal -= step / 0.35f;
                if (show.reveal <= 0.0f) {
                        show.reveal = 0.0f;
                        show.style = (int)demo_rand_below(STYLES); /* the next one's */
                        if (!show.first_switch) show.first_switch = 1;
                }
        }
        if (!show.first_switch) show.style = STYLE_TEAR; /* the boot breaks into the demo */
        transition(d, c, out);
        if (show.overlay) draw_overlay(c);
        const uint32_t *palette = pal_update(&d->pal, step);
        if (palette != NULL) (void)bmds_game_palette(game, 0, 256, palette);

        /* The figures: smoothed a frame at a time, the cores' a second at a time. */
        const int64_t end = now_ns();
        show.draw_ms += ((float)(end - start) / 1e6f - show.draw_ms) * 0.05f;
        show.frame_ms += (dt * 1000.0f - show.frame_ms) * 0.05f;
        ++show.frames;
        if (end - show.second_ns >= 1000000000) {
                int64_t us[2];
                if (busy_now(us)) {
                        const float span = (float)(end - show.second_ns) / 1000.0f;
                        for (int k = 0; k < 2; ++k) {
                                show.busy[k] = 100.0f * (float)(us[k] - show.busy_us[k]) / span;
                                show.busy_us[k] = us[k];
                        }
                }
                show.fps = show.frames;
                show.frames = 0;
                d->figures.fps = show.fps;
                d->figures.busy[0] = show.busy[0];
                d->figures.busy[1] = show.busy[1];
                d->figures.draw_ms = show.draw_ms;
                show.second_ns = end;
        }
        return;
}

int main(int argc, char **argv) {
        bmds_game_config_t config;
        memset(&config, 0, sizeof(config));
        config.size = sizeof(config);
        config.width = DEMO_WIDTH;
        config.height = DEMO_HEIGHT;
        config.format = BMDS_PIXEL_L8;
        config.flags = BMDS_SURFACE_FULL_SCREEN | BMDS_SURFACE_WHOLE;
        config.title = "BMDS Demo #1";
        config.options = BMDS_GAME_REPAINTS;
        int opt;
        int music = 1;
        show.wavetable = 1;
        while ((opt = getopt(argc, argv, "wFWn1s")) != -1) {
                if (opt == 'w') {
                        config.flags &= ~BMDS_SURFACE_FULL_SCREEN;
                } else if (opt == 'F') {
                        show.wavetable = 0;
                } else if (opt == 'W') {
                        show.wavetable = 1; /* the default now */
                } else if (opt == 'n') {
                        music = 0;
                } else if (opt == '1') {
                        show.once = 1;
                } else if (opt == 's') {
                        show.silent = 1;
                } else {
                        fprintf(stderr, "usage: demo1 [-w] [-F] [-n] [-1] [-s]\n");
                        return 2;
                }
        }
        once_song = *song_get();
        once_song.loop_section = -1;
        bmds_system_info_t info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        show.mhz = bmds_system_info(&info) == 0 ? info.cpu_mhz : 360;
        memset(show.cue, 0, sizeof(show.cue));
        show.cue_now = -1;
        demo_t *d = &show.demo;
        d->pointer_x = d->pointer_y = -1;
        /* The duo: one low and slow, one high and quick, their accents picked (one installed: both
           the system's). */
        static const char *const accents[] = {"en-US", "en-GB"};
        for (int i = 0; i < 2; ++i) {
                bmds_game_speak_t *h = &d->duo[i];
                memset(h, 0, sizeof(*h));
                h->size = sizeof(*h);
                h->voice = accents[demo_rand_below(2)];
                h->pitch = i == 0 ? 55 + demo_rand_below(15) : 115 + demo_rand_below(15); /* higher is hard to follow */
                h->speed = i == 0 ? 85 + demo_rand_below(10) : 100 + demo_rand_below(10);
        }
        pal_init(&d->pal);
        demo_init_sines();
        /* The parts' 3D: a helper on the other core draws a band of the solids. */
        bmds_3d_config_t c3;
        memset(&c3, 0, sizeof(c3));
        c3.size = sizeof(c3);
        c3.cores = 2;
        int e3 = bmds_3d_create(&c3, &d->ctx);
        if (e3 < 0) {
                fprintf(stderr, "demo1: 3D: %s\n", strerror(-e3));
                return 1;
        }
        bmds_mat4_t projection;
        bmds_mat4_perspective(&projection, 1.0472f, (float)DEMO_WIDTH / (float)DEMO_HEIGHT, 0.5f, 120.0f);
        bmds_3d_set_projection(d->ctx, &projection);
        for (int i = 0; i < PARTS; ++i) {
                const int err = parts[i]->init(d);
                if (err < 0) {
                        fprintf(stderr, "demo1: %s: %s\n", parts[i]->name, strerror(-err));
                        return 1;
                }
        }
        show.next = P_BOOT;
        show.second_ns = now_ns();
        int64_t us[2];
        if (busy_now(us)) show.busy_us[0] = us[0], show.busy_us[1] = us[1];

        show.section = -1;
        show.music = music; /* after the boot (and 15 frames: full screen takes its memory first) */

        config.event = on_event;
        config.tick = on_tick;
        config.draw = on_draw;
        const int err = bmds_game_run(&config);
        music_stop();
        voice_close();
        if (err < 0) {
                fprintf(stderr, "demo1: %s\n", strerror(-err));
                return 1;
        }
        return 0;
}
