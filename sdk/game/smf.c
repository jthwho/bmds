/**
 * @file      smf.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Standard MIDI Files walked in time order (devplan/audio.md §25): the tracks
 * merged, the tempo map applied, for the music a game sends through
 * /dev/midi. Format 0 and 1; a division in ticks a quarter note or SMPTE's.
 * Every read is checked against the track's end: a file cut short ends there.
 */

#include <string.h>
#include "game_internal.h"

#define DEFAULT_TEMPO 500000u /* microseconds a quarter note: 120 a minute */

static uint32_t be(const uint8_t *p, unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v = (v << 8) | p[i];
        return v;
}

/* A variable-length quantity at *@p p (at most 4 bytes), before @p end. -1: cut short. */
static int64_t vlq(const uint8_t **p, const uint8_t *end) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
                if (*p >= end) return -1;
                const uint8_t b = *(*p)++;
                v = (v << 7) | (b & 0x7fu);
                if ((b & 0x80u) == 0) return v;
        }
        return -1;
}

/* Track @p t's next delta time read into its tick (done at its end). */
static void advance(bmds_game_smf_track_t *t) {
        if (t->pos >= t->end) {
                t->done = 1;
                return;
        }
        const int64_t delta = vlq(&t->pos, t->end);
        if (delta < 0) {
                t->done = 1;
                return;
        }
        t->tick += (uint64_t)delta;
        return;
}

int bmds_game_smf_open(bmds_game_smf_t *smf, const void *data, size_t size) {
        memset(smf, 0, sizeof(*smf));
        const uint8_t *d = (const uint8_t *)data;
        if (size < 14 || memcmp(d, "MThd", 4) != 0 || be(d + 4, 4) < 6) return -BMDS_EINVAL;
        const uint32_t format = be(d + 8, 2);
        const uint32_t division = be(d + 12, 2);
        if (format > 1 || division == 0) return -BMDS_EINVAL;
        smf->data = d;
        smf->size = size;
        if ((division & 0x8000u) != 0) {
                /* SMPTE: frames a second (negative, in the top byte) times ticks a frame. */
                const int      fps = -(int)(int8_t)(division >> 8);
                const uint32_t per = division & 0xffu;
                if (fps <= 0 || per == 0) return -BMDS_EINVAL;
                smf->smpte = 1;
                smf->division = (uint32_t)fps * per;
        } else {
                smf->division = division;
        }
        /* The tracks: each MTrk chunk, others skipped. */
        size_t at = 8 + be(d + 4, 4);
        while (at + 8 <= size && smf->count < BMDS_GAME_SMF_TRACKS) {
                const uint32_t length = be(d + at + 4, 4);
                const size_t   body = at + 8;
                const size_t   end = length > size - body ? size : body + length;
                if (memcmp(d + at, "MTrk", 4) == 0) {
                        smf->tracks[smf->count].pos = d + body;
                        smf->tracks[smf->count].end = d + end;
                        ++smf->count;
                }
                if (length > size - body) break;
                at = body + length;
        }
        if (smf->count == 0) return -BMDS_EINVAL;
        bmds_game_smf_rewind(smf);
        return 0;
}

void bmds_game_smf_rewind(bmds_game_smf_t *smf) {
        size_t   at = 8 + be(smf->data + 4, 4);
        uint32_t n = 0;
        while (at + 8 <= smf->size && n < smf->count) {
                const uint32_t length = be(smf->data + at + 4, 4);
                const size_t   body = at + 8;
                const size_t   end = length > smf->size - body ? smf->size : body + length;
                if (memcmp(smf->data + at, "MTrk", 4) == 0) {
                        bmds_game_smf_track_t *t = &smf->tracks[n++];
                        t->pos = smf->data + body;
                        t->end = smf->data + end;
                        t->tick = 0;
                        t->status = 0;
                        t->done = 0;
                        advance(t);
                }
                if (length > smf->size - body) break;
                at = body + length;
        }
        smf->tempo = DEFAULT_TEMPO;
        smf->tick = 0;
        smf->ns = 0;
        return;
}

/* @p tick in nanoseconds from the start, the tempo map's place moved there. */
static int64_t time_of(bmds_game_smf_t *smf, uint64_t tick) {
        const uint64_t ticks = tick - smf->tick;
        const int64_t  ns = smf->smpte ? (int64_t)(ticks * 1000000000ull / smf->division)
                                       : (int64_t)(ticks * smf->tempo * 1000ull / smf->division);
        smf->ns += ns;
        smf->tick = tick;
        return smf->ns;
}

int bmds_game_smf_next(bmds_game_smf_t *smf, bmds_game_smf_event_t *event) {
        for (;;) {
                /* The track with the earliest event (the first of equals: format 1's order). */
                bmds_game_smf_track_t *t = NULL;
                for (uint32_t i = 0; i < smf->count; ++i) {
                        bmds_game_smf_track_t *c = &smf->tracks[i];
                        if (!c->done && (t == NULL || c->tick < t->tick)) t = c;
                }
                if (t == NULL) return 0;
                event->ns = time_of(smf, t->tick);
                if (t->pos >= t->end) {
                        t->done = 1;
                        continue;
                }
                uint8_t status = *t->pos;
                if (status & 0x80u) {
                        ++t->pos;
                } else if (t->status != 0) {
                        status = t->status; /* running status */
                } else {
                        t->done = 1;
                        return -BMDS_EINVAL;
                }
                if (status < 0xf0u) {
                        t->status = status;
                        const unsigned n = (status & 0xe0u) == 0xc0u ? 1u : 2u; /* Cx and Dx take one */
                        if ((size_t)(t->end - t->pos) < n) {
                                t->done = 1;
                                return -BMDS_EINVAL;
                        }
                        event->kind = BMDS_GAME_SMF_MESSAGE;
                        event->data[0] = status;
                        event->data[1] = t->pos[0] & 0x7fu;
                        event->data[2] = n == 2 ? (uint8_t)(t->pos[1] & 0x7fu) : 0;
                        event->length = (uint8_t)(1 + n);
                        t->pos += n;
                        advance(t);
                        return 1;
                }
                t->status = 0; /* a SysEx or meta event ends running status */
                if (status == 0xffu) {
                        if (t->pos >= t->end) {
                                t->done = 1;
                                return -BMDS_EINVAL;
                        }
                        const uint8_t type = *t->pos++;
                        const int64_t length = vlq(&t->pos, t->end);
                        if (length < 0 || length > t->end - t->pos) {
                                t->done = 1;
                                return -BMDS_EINVAL;
                        }
                        if (type == 0x51 && length == 3 && !smf->smpte) {
                                const uint32_t tempo = be(t->pos, 3);
                                if (tempo != 0) smf->tempo = tempo;
                        }
                        t->pos += length;
                        if (type == 0x2f) {
                                t->done = 1; /* the end of the track */
                                continue;
                        }
                        advance(t);
                        continue;
                }
                if (status == 0xf0u || status == 0xf7u) {
                        /* F0: a SysEx (F0 sent, then its bytes); F7: bytes as they are. */
                        const int64_t length = vlq(&t->pos, t->end);
                        if (length < 0 || length > t->end - t->pos) {
                                t->done = 1;
                                return -BMDS_EINVAL;
                        }
                        const uint32_t lead = status == 0xf0u ? 1u : 0u;
                        if ((uint32_t)length + lead > sizeof(event->sysex)) {
                                t->pos += length; /* too long for a game's music: skipped */
                                advance(t);
                                continue;
                        }
                        event->kind = BMDS_GAME_SMF_SYSEX;
                        event->sysex[0] = 0xf0u;
                        memcpy(event->sysex + lead, t->pos, (size_t)length);
                        event->bytes = event->sysex;
                        event->count = (uint32_t)length + lead;
                        t->pos += length;
                        advance(t);
                        if (event->count == 0) continue;
                        return 1;
                }
                t->done = 1; /* a status no file may have */
                return -BMDS_EINVAL;
        }
}
