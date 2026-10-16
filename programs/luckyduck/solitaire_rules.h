/**
 * @file      solitaire_rules.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Solitaire's rules (devplan/games.md §21): Klondike, drawing one or three, Windows' standard and
 * Vegas scoring, undo back to the deal. No drawing, no system calls: tested on the host.
 */

#ifndef SOLITAIRE_RULES_H
#define SOLITAIRE_RULES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The piles. */
enum {
        SOL_STOCK,
        SOL_WASTE,
        SOL_FOUNDATION,                   /* 4 of them */
        SOL_TABLEAU = SOL_FOUNDATION + 4, /* 7 columns */
        SOL_PILES = SOL_TABLEAU + 7
};

#define SOL_CARDS 52
#define SOL_RANKS 13
#define SOL_UP 0x80u /* a card's face-up bit; the rest is its suit × 13 + its rank (0 the Ace) */

/** @brief The suits, in the sheet's order: clubs and spades black, diamonds and hearts red. */
enum {
        SOL_CLUBS,
        SOL_DIAMONDS,
        SOL_HEARTS,
        SOL_SPADES
};

/** @brief Scorings. */
enum {
        SOL_SCORE_STANDARD,
        SOL_SCORE_VEGAS,
        SOL_SCORE_NONE,
        SOL_SCORINGS
};

#define SOL_HISTORY 1024 /* moves kept to undo (more: the oldest forgotten) */

/** @brief A pile: its cards, the top last. */
typedef struct sol_pile_t {
                uint8_t count;
                uint8_t cards[SOL_CARDS];
} sol_pile_t;

/** @brief A move as kept for undo: what it did and what it replaced. */
typedef struct sol_move_t {
                uint8_t  kind; /* SOL_MOVED, SOL_DREW, SOL_TURNED_AGAIN */
                uint8_t  from, to, count;
                uint8_t  flipped; /* the column's new top turned over */
                uint8_t  fan;     /* the waste's fan before */
                uint16_t passes;  /* before */
                int32_t  score;   /* before */
} sol_move_t;

/** @brief `sol_move_t::kind`. */
enum {
        SOL_MOVED,
        SOL_DREW,
        SOL_TURNED_AGAIN /* the waste back onto the stock */
};

/** @brief A game. Read freely; changed by the calls. */
typedef struct solitaire_t {
                sol_pile_t pile[SOL_PILES];
                uint32_t   draw;    /* 1 or 3 */
                uint32_t   scoring; /* SOL_SCORE_... */
                int32_t    score;
                uint32_t   passes;  /* times the stock was turned again */
                uint32_t   fan;     /* the waste's top cards shown fanned (drawing three) */
                uint32_t   seconds; /* played */
                uint32_t   moves;
                int        started; /* a move made: the timer runs */
                int        won;
                sol_move_t history[SOL_HISTORY];
                uint32_t   history_first, history_count; /* a ring */
} solitaire_t;

/** @brief Returns @p card's suit, rank (0 the Ace), and whether it is red. */
static inline uint32_t sol_suit(uint8_t card) {
        return (uint32_t)(card & ~SOL_UP) / SOL_RANKS;
}
static inline uint32_t sol_rank(uint8_t card) {
        return (uint32_t)(card & ~SOL_UP) % SOL_RANKS;
}
static inline int sol_red(uint8_t card) {
        return sol_suit(card) == SOL_DIAMONDS || sol_suit(card) == SOL_HEARTS;
}

/** @brief Deals a game from a deck shuffled by @p seed (not 0), drawing @p draw (1 or 3). */
void sol_deal(solitaire_t *s, uint32_t draw, uint32_t scoring, uint32_t seed);

/**
 * @brief Deals a game already played out: every card face up, in four columns from King to Ace,
 *        nothing in the stock, so it finishes by `sol_auto_step()` (a demo of the win).
 */
void sol_deal_finished(solitaire_t *s, uint32_t draw, uint32_t scoring);

/** @brief Returns true if the top @p count cards of pile @p from can be picked up. */
int sol_can_take(const solitaire_t *s, uint32_t from, uint32_t count);

/** @brief Returns true if the top @p count cards of @p from may go onto @p to. */
int sol_can_move(const solitaire_t *s, uint32_t from, uint32_t count, uint32_t to);

/**
 * @brief Moves the top @p count cards of @p from onto @p to, turning over a column's new top,
 *        scoring, and kept for undo.
 * @return 0, or -1 (not allowed).
 */
int sol_move(solitaire_t *s, uint32_t from, uint32_t count, uint32_t to);

/** @brief Returns true if the stock can be drawn from or turned again. */
int sol_can_draw(const solitaire_t *s);

/**
 * @brief Draws `draw` cards from the stock to the waste, or with the stock empty turns the waste
 *        over onto it (another pass, if the scoring allows one).
 * @return The cards drawn (0: turned again), or -1 (neither).
 */
int sol_draw(solitaire_t *s);

/** @brief Undoes the last move. @return 0, or -1 (none left). */
int sol_undo(solitaire_t *s);

/** @brief Returns the foundation pile @p from's top card can go to, or -1. */
int sol_foundation_for(const solitaire_t *s, uint32_t from);

/**
 * @brief Moves one card that can go up (the waste's first, then the columns' left to right).
 * @return The pile it came from (its foundation in `to`), or -1 (none).
 */
int sol_auto_step(solitaire_t *s, uint32_t *to);

/** @brief A second of play passed (a started game not won): the time and its penalty. */
void sol_second(solitaire_t *s);

/** @brief Returns the cards on the foundations. */
uint32_t sol_home(const solitaire_t *s);

#ifdef __cplusplus
}
#endif

#endif /* SOLITAIRE_RULES_H */
