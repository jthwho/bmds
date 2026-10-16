/**
 * @file      dice_rules.c
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck Dice's rules (devplan/luckyduck.md §8c, §18).
 */

#include <stdlib.h>
#include <string.h>
#include "dice_rules.h"

/* ---- Scores */

static void count_faces(const uint8_t *dice, int *counts) {
        for (int f = 0; f <= 6; ++f) counts[f] = 0;
        for (int i = 0; i < DICE_DICE; ++i)
                if (dice[i] >= 1 && dice[i] <= 6) ++counts[dice[i]];
        return;
}

static int sum_of(const uint8_t *dice) {
        int sum = 0;
        for (int i = 0; i < DICE_DICE; ++i) sum += dice[i];
        return sum;
}

/* The longest run of faces present. */
static int longest_run(const int *counts) {
        int best = 0, run = 0;
        for (int f = 1; f <= 6; ++f) {
                run = counts[f] > 0 ? run + 1 : 0;
                if (run > best) best = run;
        }
        return best;
}

static int most_of_a_kind(const int *counts) {
        int most = 0;
        for (int f = 1; f <= 6; ++f)
                if (counts[f] > most) most = counts[f];
        return most;
}

int dice_box_score(const uint8_t *dice, int box, int joker) {
        int counts[7];
        count_faces(dice, counts);
        const int most = most_of_a_kind(counts);
        switch (box) {
                case DICE_ONES:
                case DICE_TWOS:
                case DICE_THREES:
                case DICE_FOURS:
                case DICE_FIVES:
                case DICE_SIXES: return (box + 1) * counts[box + 1];
                case DICE_THREE_KIND: return most >= 3 ? sum_of(dice) : 0;
                case DICE_FOUR_KIND: return most >= 4 ? sum_of(dice) : 0;
                case DICE_FULL_HOUSE: {
                        int three = 0, two = 0;
                        for (int f = 1; f <= 6; ++f) {
                                if (counts[f] == 3) three = 1;
                                if (counts[f] == 2) two = 1;
                        }
                        return (three && two) || joker ? 25 : 0;
                }
                case DICE_SMALL_STRAIGHT: return longest_run(counts) >= 4 || joker ? 30 : 0;
                case DICE_LARGE_STRAIGHT: return longest_run(counts) >= 5 || joker ? 40 : 0;
                case DICE_LUCKY_DUCK: return most == 5 ? 50 : 0;
                case DICE_CHANCE: return sum_of(dice);
                default: return 0;
        }
}

int dice_upper(const dice_seat_t *s) {
        int sum = 0;
        for (int b = DICE_ONES; b <= DICE_SIXES; ++b)
                if (s->box[b] != DICE_OPEN) sum += s->box[b];
        return sum;
}

int dice_total(const dice_seat_t *s) {
        const int upper = dice_upper(s);
        int       sum = upper + (upper >= DICE_UPPER_TARGET ? DICE_UPPER_BONUS : 0) + s->lucky_bonus;
        for (int b = DICE_THREE_KIND; b < DICE_BOXES; ++b)
                if (s->box[b] != DICE_OPEN) sum += s->box[b];
        return sum;
}

/* Whether @p dice are a second Lucky Duck for @p s: five of a kind, its Lucky Duck box written. */
static int is_joker(const dice_seat_t *s, const uint8_t *dice) {
        int counts[7];
        count_faces(dice, counts);
        return most_of_a_kind(counts) == 5 && s->box[DICE_LUCKY_DUCK] != DICE_OPEN;
}

/* Whether @p s may write @p dice into @p box: open, and the joker rule's order (its upper box;
   else any lower box; else any upper one). */
static int allowed(const dice_seat_t *s, const uint8_t *dice, int box) {
        if (box < 0 || box >= DICE_BOXES || s->box[box] != DICE_OPEN) return 0;
        if (!is_joker(s, dice)) return 1;
        const int upper = dice[0] - 1;
        if (s->box[upper] == DICE_OPEN) return box == upper;
        int lower_open = 0;
        for (int b = DICE_THREE_KIND; b < DICE_BOXES; ++b)
                if (s->box[b] == DICE_OPEN) lower_open = 1;
        return lower_open ? box >= DICE_THREE_KIND : box <= DICE_SIXES;
}

static int points_for(const dice_seat_t *s, const uint8_t *dice, int box) {
        if (!allowed(s, dice, box)) return 0;
        return dice_box_score(dice, box, is_joker(s, dice));
}

/* ---- The log */

static void emit(dice_table_t *t, uint8_t type, int seat, int die, int held, int32_t amount) {
        dice_event_t *e = &t->events[t->event_count++ % DICE_EVENTS];
        memset(e, 0, sizeof(*e));
        e->type = type;
        e->seat = (int8_t)seat;
        e->die = (uint8_t)die;
        e->held = (uint8_t)held;
        e->roll = t->roll;
        memcpy(e->dice, t->dice, sizeof(e->dice));
        e->amount = amount;
        return;
}

const dice_event_t *dice_event(const dice_table_t *t, uint32_t i) {
        return &t->events[i % DICE_EVENTS];
}

/* ---- Play */

static void begin_turn(dice_table_t *t, int seat) {
        t->turn_seat = (int8_t)seat;
        t->roll = 0;
        memset(t->held, 0, sizeof(t->held));
        memset(t->dice, 0, sizeof(t->dice));
        emit(t, DICE_EV_TURN, seat, 0, 0, 0);
        t->events[(t->event_count - 1) % DICE_EVENTS].roll = t->round;
        return;
}

void dice_init(dice_table_t *t, uint32_t playing, deck_key_t key) {
        memset(t, 0, sizeof(*t));
        deck_seed_key(&t->rng, key);
        int count = 0, first = -1;
        for (int s = 0; s < DICE_SEATS; ++s) {
                for (int b = 0; b < DICE_BOXES; ++b) t->seat[s].box[b] = DICE_OPEN;
                t->seat[s].playing = (playing >> s) & 1u;
                if (t->seat[s].playing) {
                        ++count;
                        if (first < 0) first = s;
                }
        }
        t->turn_seat = -1;
        emit(t, DICE_EV_START, -1, 0, 0, count);
        if (first < 0) {
                t->phase = DICE_OVER;
                return;
        }
        t->phase = DICE_PLAYING;
        begin_turn(t, first);
        return;
}

static int my_turn(const dice_table_t *t, int seat) {
        return t->phase == DICE_PLAYING && seat == t->turn_seat;
}

int dice_roll(dice_table_t *t, int seat) {
        if (!my_turn(t, seat) || t->roll >= DICE_ROLLS) return -1;
        uint8_t mask = 0;
        for (int i = 0; i < DICE_DICE; ++i) {
                if (t->held[i]) {
                        mask |= (uint8_t)(1u << i);
                        continue;
                }
                t->dice[i] = (uint8_t)(1 + deck_below(&t->rng, 6));
        }
        ++t->roll;
        emit(t, DICE_EV_ROLL, seat, 0, mask, 0);
        return 0;
}

int dice_hold(dice_table_t *t, int seat, int die, int held) {
        if (!my_turn(t, seat) || t->roll == 0 || t->roll >= DICE_ROLLS || die < 0 || die >= DICE_DICE) return -1;
        held = held != 0;
        if (t->held[die] == held) return 0;
        t->held[die] = (uint8_t)held;
        emit(t, DICE_EV_HOLD, seat, die, held, 0);
        return 0;
}

int dice_can_score(const dice_table_t *t, int seat, int box) {
        return my_turn(t, seat) && t->roll > 0 && allowed(&t->seat[seat], t->dice, box);
}

int dice_points(const dice_table_t *t, int seat, int box) {
        if (seat < 0 || seat >= DICE_SEATS) return 0;
        return points_for(&t->seat[seat], t->dice, box);
}

int dice_score(dice_table_t *t, int seat, int box) {
        if (!dice_can_score(t, seat, box)) return -1;
        dice_seat_t *s = &t->seat[seat];
        const int    points = points_for(s, t->dice, box);
        const int    joker = is_joker(s, t->dice), upper_before = dice_upper(s);
        s->box[box] = (int16_t)points;
        emit(t, DICE_EV_SCORE, seat, box, 0, points);
        if (joker && s->box[DICE_LUCKY_DUCK] == 50) {
                s->lucky_bonus = (int16_t)(s->lucky_bonus + DICE_LUCKY_BONUS);
                emit(t, DICE_EV_BONUS, seat, DICE_LUCKY_DUCK, 0, DICE_LUCKY_BONUS);
        }
        if (upper_before < DICE_UPPER_TARGET && dice_upper(s) >= DICE_UPPER_TARGET)
                emit(t, DICE_EV_BONUS, seat, DICE_ONES, 0, DICE_UPPER_BONUS);
        /* The next seat playing; past the last, the next round; past the last round, the end. */
        int next = seat;
        do {
                next = (next + 1) % DICE_SEATS;
                if (next == 0) ++t->round;
        } while (!t->seat[next].playing);
        if (t->round >= DICE_ROUNDS) {
                t->phase = DICE_OVER;
                t->turn_seat = -1;
                int winner = -1, best = -1;
                for (int i = 0; i < DICE_SEATS; ++i) {
                        if (!t->seat[i].playing) continue;
                        const int total = dice_total(&t->seat[i]);
                        if (total > best) {
                                best = total;
                                winner = i;
                        }
                }
                emit(t, DICE_EV_END, winner, 0, 0, best);
                return 0;
        }
        begin_turn(t, next);
        return 0;
}

/* ---- The bots */

/* What each box is worth on average over a game well played: a box's score is judged against it. */
static const float PAR[DICE_BOXES] = {2.1f,  5.3f,  8.6f,  12.2f, 15.7f, 19.2f, 21.7f,
                                      13.1f, 22.6f, 29.5f, 32.7f, 16.9f, 22.0f};

/* Multisets of 0 to 5 dice: each as its faces' counts packed 3 bits a face (face 1 lowest), sorted. */
#define MULTISETS 462
static uint32_t ms_key[MULTISETS];
static uint8_t  ms_size[MULTISETS];
static float    ms_prob[MULTISETS];           /* of rolling it with that many dice */
static uint16_t ms_sized[DICE_DICE + 1][252]; /* each size's, by index */
static uint16_t ms_sized_count[DICE_DICE + 1];
static int      ms_ready;

static int compare_keys(const void *a, const void *b) {
        const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
        return x < y ? -1 : x > y;
}

static int key_count(uint32_t key, int face) {
        return (int)((key >> (3 * (face - 1))) & 7u);
}

static void multisets(void) {
        if (ms_ready) return;
        int n = 0;
        for (uint32_t key = 0; key < (1u << 18); ++key) {
                int size = 0, ok = 1;
                for (int f = 1; f <= 6; ++f) {
                        const int c = key_count(key, f);
                        if (c > 5) ok = 0;
                        size += c;
                }
                if (ok && size <= 5 && n < MULTISETS) ms_key[n++] = key;
        }
        qsort(ms_key, MULTISETS, sizeof(ms_key[0]), compare_keys);
        static const float factorial[6] = {1, 1, 2, 6, 24, 120};
        for (int i = 0; i < MULTISETS; ++i) {
                int   size = 0;
                float ways = 1.0f;
                for (int f = 1; f <= 6; ++f) {
                        const int c = key_count(ms_key[i], f);
                        size += c;
                        ways /= factorial[c];
                }
                float all = 1.0f;
                for (int k = 0; k < size; ++k) all *= 6.0f;
                ms_size[i] = (uint8_t)size;
                ms_prob[i] = ways * factorial[size] / all;
                ms_sized[size][ms_sized_count[size]++] = (uint16_t)i;
        }
        ms_ready = 1;
        return;
}

static int ms_index(uint32_t key) {
        int lo = 0, hi = MULTISETS;
        while (lo < hi) {
                const int mid = (lo + hi) / 2;
                if (ms_key[mid] == key) return mid;
                if (ms_key[mid] < key)
                        lo = mid + 1;
                else
                        hi = mid;
        }
        return -1;
}

static uint32_t key_of(const uint8_t *dice, uint32_t mask) {
        uint32_t key = 0;
        for (int i = 0; i < DICE_DICE; ++i)
                if (mask & (1u << i)) key += 1u << (3 * (dice[i] - 1));
        return key;
}

static void dice_of(uint32_t key, uint8_t *dice) {
        int n = 0;
        for (int f = 1; f <= 6; ++f)
                for (int c = key_count(key, f); c > 0; --c) dice[n++] = (uint8_t)f;
        return;
}

/* Box @p box's worth for @p s with @p dice: its points against its par, credit towards the upper
   bonus while it can still be made, and a second Lucky Duck's 100 (whichever box takes it). */
static float worth(const dice_seat_t *s, const uint8_t *dice, int box) {
        if (!allowed(s, dice, box)) return -1000.0f;
        const int points = dice_box_score(dice, box, is_joker(s, dice));
        float     w = (float)points - PAR[box];
        if (box <= DICE_SIXES) {
                const int upper = dice_upper(s);
                int       reach = upper;
                for (int b = DICE_ONES; b <= DICE_SIXES; ++b)
                        if (s->box[b] == DICE_OPEN) reach += 5 * (b + 1);
                if (upper < DICE_UPPER_TARGET && reach >= DICE_UPPER_TARGET)
                        w += 0.6f * (float)(points - 3 * (box + 1));
        }
        if (is_joker(s, dice) && s->box[DICE_LUCKY_DUCK] == 50) w += DICE_LUCKY_BONUS;
        return w;
}

static int best_box(const dice_seat_t *s, const uint8_t *dice, float *value) {
        int   best = -1;
        float most = -1e9f;
        for (int b = 0; b < DICE_BOXES; ++b) {
                const float w = worth(s, dice, b);
                if (s->box[b] == DICE_OPEN && w > most) {
                        most = w;
                        best = b;
                }
        }
        if (value != NULL) *value = most;
        return best;
}

/* The values by multiset: a hand of five's worth scored now (v0), with one roll to come (v1); a
   keep's expected worth with one roll left (e1), with two (e2). */
static float v0[MULTISETS], v1[MULTISETS], e1[MULTISETS], e2[MULTISETS];

/* @p e[k] for each keep k: the chance-weighted @p v of the hands it can roll to. */
static void expect(const float *v, float *e) {
        for (int k = 0; k < MULTISETS; ++k) {
                const int rolled = DICE_DICE - ms_size[k];
                float     sum = 0.0f;
                for (int j = 0; j < ms_sized_count[rolled]; ++j) {
                        const int o = ms_sized[rolled][j];
                        const int hand = ms_index(ms_key[k] + ms_key[o]);
                        if (hand >= 0) sum += ms_prob[o] * v[hand];
                }
                e[k] = sum;
        }
        return;
}

dice_choice_t dice_bot(const dice_table_t *t, int seat, int ahead) {
        dice_choice_t c = {DICE_BOXES, 0};
        if (!my_turn(t, seat) || t->roll == 0) return c;
        const dice_seat_t *s = &t->seat[seat];
        float              now;
        const int          box = best_box(s, t->dice, &now);
        if (t->roll >= DICE_ROLLS) {
                c.box = (int8_t)box;
                return c;
        }
        multisets();
        for (int i = 0; i < MULTISETS; ++i) {
                if (ms_size[i] != 5) continue;
                uint8_t hand[DICE_DICE];
                dice_of(ms_key[i], hand);
                (void)best_box(s, hand, &v0[i]);
        }
        expect(v0, e1);
        const float *e = e1;
        if (DICE_ROLLS - t->roll >= 2 && ahead >= 2) {
                /* A hand of five with one roll to come: its best keep's (all five kept: scored now). */
                for (int i = 0; i < MULTISETS; ++i) {
                        if (ms_size[i] != 5) continue;
                        uint8_t hand[DICE_DICE];
                        dice_of(ms_key[i], hand);
                        float most = -1e9f;
                        for (uint32_t m = 0; m < 32; ++m) {
                                const int k = ms_index(key_of(hand, m));
                                if (k >= 0 && e1[k] > most) most = e1[k];
                        }
                        v1[i] = most;
                }
                expect(v1, e2);
                e = e2;
        }
        /* Rolling again keeping some (fewer than five), against scoring now. */
        float    most = now;
        uint32_t keep = 0x1fu;
        for (uint32_t m = 0; m < 31; ++m) {
                const int k = ms_index(key_of(t->dice, m));
                if (k >= 0 && e[k] > most + 0.01f) {
                        most = e[k];
                        keep = m;
                }
        }
        if (keep == 0x1fu) {
                c.box = (int8_t)box;
                return c;
        }
        c.keep = (uint8_t)keep;
        return c;
}
