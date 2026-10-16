/**
 * @file      poker_rules.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Lucky Duck's Poker rules (devplan/luckyduck.md §8e, §25): Texas Hold'em (no-limit or fixed),
 * Five-Card Draw, Five-Card Stud and Seven-Card Stud at a table of six seats, run from actions and
 * a seed, its events logged for the picture (and one day a network), each readable as a seat may
 * see it; each seat's view for the bots. Side pots, split pots, all-ins, tournaments' places. No
 * drawing, no system calls: tested on the host.
 */

#ifndef LUCKYDUCK_POKER_RULES_H
#define LUCKYDUCK_POKER_RULES_H

#include <stdint.h>
#include "deck.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PK_SEATS 6
#define PK_CARDS 7 /* a seat's at most (Seven-Card Stud) */
#define PK_BOARD 5 /* Hold'em's */
#define PK_POTS 6  /* the main pot and the side pots: at most one a seat */
#define PK_EVENTS 384
#define PK_HIDDEN 0xffu /* a card a seat may not see */
#define PK_NOBODY (-1)

/** @brief The games. */
enum {
        PK_HOLDEM,
        PK_DRAW,
        PK_STUD5,
        PK_STUD7,
        PK_GAMES
};

/** @brief The phases of a hand. */
enum {
        PK_WAITING, /* between hands: pk_start_hand() deals the next */
        PK_BETTING, /* `turn` to act */
        PK_DRAWING  /* Draw's: `turn` to discard */
};

/** @brief Actions (pk_act()). */
enum {
        PK_FOLD,
        PK_CHECK,
        PK_CALL,
        PK_RAISE, /* a bet when nothing is bet; the amount is the total raised to */
        PK_ACTIONS
};

/** @brief What a post is (`pk_event_t::action` of `PK_EV_POST`). */
enum {
        PK_POST_SMALL,
        PK_POST_BIG,
        PK_POST_ANTE,
        PK_POST_BRING_IN
};

/** @brief Events (`pk_event_t::type`). */
enum {
        PK_EV_START,   /* seat: the button; amount: the hand's number */
        PK_EV_POST,    /* seat, action (PK_POST_...), amount: what was posted */
        PK_EV_DEAL,    /* seat, index (its card's place), card, up */
        PK_EV_BOARD,   /* index, card */
        PK_EV_TURN,    /* seat: to act (or, drawing, to draw) */
        PK_EV_ACT,     /* seat, action, amount: its bet this round after it; up: all in */
        PK_EV_COLLECT, /* the round's bets into the pot; amount: the pot after */
        PK_EV_RETURN,  /* seat, amount: an uncalled bet back */
        PK_EV_DISCARD, /* seat, value: the mask of its cards thrown (their places dealt again after) */
        PK_EV_SHOW,    /* seat, index, card: a card turned up at the showdown (or an all-in's) */
        PK_EV_HAND,    /* seat, value: its hand's value (hand.h), mask: the five cards making it */
        PK_EV_POT,     /* index, amount, value: the seats it is for (a mask) */
        PK_EV_WIN,     /* seat, index: the pot, amount */
        PK_EV_OUT,     /* seat, index: its tournament place (1 the winner), 0 in a cash game */
        PK_EV_END
};

/** @brief An event of a hand. */
typedef struct {
                uint8_t  type;
                int8_t   seat;
                uint8_t  index;
                uint8_t  card;
                uint8_t  up;
                uint8_t  action;
                int32_t  amount;
                uint32_t value;
                uint16_t mask; /* PK_EV_HAND: bit i the seat's card i, bit PK_CARDS + i the board's */
} pk_event_t;

/** @brief The table's game and stakes (pk_stakes()). */
typedef struct {
                uint8_t game;
                uint8_t no_limit;               /* Hold'em only */
                int32_t small_blind, big_blind; /* Hold'em, Draw */
                int32_t ante, bring_in;         /* the studs */
                int32_t small_bet;              /* fixed limit's (the big bet twice it) */
} pk_rules_t;

/** @brief A seat. */
typedef struct {
                uint8_t sitting; /* at the table (with chips, or out of them between hands) */
                uint8_t in_hand; /* dealt this hand and not folded */
                uint8_t all_in;
                uint8_t acted; /* since the last full raise: may not raise again */
                uint8_t drawn; /* has drawn this hand */
                uint8_t shown; /* its cards turned up */
                uint8_t count; /* cards */
                uint8_t up;    /* bit i: card i dealt face up */
                uint8_t cards[PK_CARDS];
                uint8_t place;   /* a tournament's: where it finished (0: still in) */
                int32_t stack;   /* chips not in the pot */
                int32_t bet;     /* this round's */
                int32_t put;     /* this hand's, every round's */
                int32_t started; /* the stack the hand started with */
} pk_seat_t;

/** @brief A table. Read freely; changed by the calls. */
typedef struct {
                pk_rules_t rules;
                deck_rng_t rng;
                uint8_t    deck[DECK_CARDS];
                uint8_t    next; /* the next card's place */
                uint8_t    muck[DECK_CARDS];
                uint8_t    muck_count; /* discards and folded cards, for the draw's reshuffle */
                uint8_t    fixed[DECK_CARDS];
                uint8_t    fixed_count; /* the next hand's deck starts with these (pk_fix_deck()) */
                pk_seat_t  seat[PK_SEATS];
                uint8_t    board[PK_BOARD];
                uint8_t    board_count;
                uint8_t    phase;
                uint8_t    round;      /* the betting round: 0 the first */
                uint8_t    raises;     /* fixed limit: bets this round */
                uint8_t    tournament; /* a seat out of chips is out */
                int8_t     button;
                int8_t     turn;
                int8_t     aggressor;  /* the last to bet or raise (this round, or the last that had one) */
                int8_t     bring_in;   /* stud: who brought it in this hand */
                int32_t    cur_bet;    /* the bet to match this round */
                int32_t    last_raise; /* no-limit: the last full raise's size */
                int32_t    pot;        /* collected from the rounds before */
                uint32_t   hands;      /* dealt so far */
                uint8_t    out;        /* tournament seats out so far */
                pk_event_t events[PK_EVENTS];
                uint32_t   event_count; /* this hand's */
} pk_table_t;

/** @brief A game's stakes from its big blind (Stud: antes a quarter, the bring-in half, the small bet it). */
pk_rules_t pk_stakes(int game, int no_limit, int32_t big_blind);

/** @brief Makes a table with @p rules, its random numbers from @p key, no one sitting. */
void pk_init(pk_table_t *t, const pk_rules_t *rules, deck_key_t key, int tournament);

/** @brief Changes the stakes (between hands: a tournament's rising blinds). @return 0, or -1. */
int pk_set_rules(pk_table_t *t, const pk_rules_t *rules);

/** @brief Sits a player at @p seat with @p stack (between hands; @p sitting 0 stands one up). */
void pk_sit(pk_table_t *t, int seat, int sitting, int32_t stack);

/** @brief Adds @p amount to @p seat's stack (between hands: a rebuy). */
void pk_add_chips(pk_table_t *t, int seat, int32_t amount);

/** @brief The next hand's deck starts with @p cards in order (the tests' fixed hands). */
void pk_fix_deck(pk_table_t *t, const uint8_t *cards, int count);

/** @brief The seats with chips. */
int pk_players(const pk_table_t *t);

/**
 * @brief Deals the next hand (between hands, two seats with chips): the button moved, the deck
 *        shuffled, the blinds or antes posted, the cards dealt. @return 0, or -1.
 */
int pk_start_hand(pk_table_t *t);

/** @brief What @p seat must add to call (0: it may check). */
int32_t pk_to_call(const pk_table_t *t, int seat);

/** @brief The least and the most @p seat may raise to now (its bet's total; 0 when it may not raise). */
int32_t pk_min_raise(const pk_table_t *t, int seat);
int32_t pk_max_raise(const pk_table_t *t, int seat);

/** @brief Whether @p seat may take @p action now (its turn). */
int pk_can(const pk_table_t *t, int seat, int action);

/**
 * @brief Takes @p action for @p seat (its turn): @p amount, for a raise, the total raised to (fixed
 *        limit: ignored, the round's size). A call or raise beyond the stack is all in.
 * @return 0, or -1 (not allowed).
 */
int pk_act(pk_table_t *t, int seat, int action, int32_t amount);

/** @brief Draw: @p seat throws the cards in @p mask (bit i: card i) and is dealt as many. @return 0, or -1. */
int pk_discard(pk_table_t *t, int seat, uint32_t mask);

/** @brief The chips in the middle: the pot and this round's bets. */
int32_t pk_pot_total(const pk_table_t *t);

/** @brief The bet size of fixed limit's round @p round. */
int32_t pk_bet_size(const pk_table_t *t, int round);

/** @brief The cards a seat ends with (its hand and the board). */
int pk_final_cards(int game);

/** @brief Event @p i as @p seat may see it (another seat's down cards hidden until shown). */
pk_event_t pk_event_for(const pk_table_t *t, uint32_t i, int seat);

/** @brief A seat in a view. */
typedef struct {
                uint8_t sitting, in_hand, all_in;
                uint8_t count;
                uint8_t cards[PK_CARDS]; /* PK_HIDDEN where it may not see */
                int32_t stack, bet, put;
} pk_view_seat_t;

/** @brief What a seat may know of the table: everything public and its own cards. */
typedef struct {
                pk_rules_t     rules;
                int8_t         me, button, turn;
                uint8_t        phase, round, board_count;
                uint8_t        board[PK_BOARD];
                pk_view_seat_t seat[PK_SEATS];
                uint8_t        dead[DECK_CARDS]; /* up cards seen and folded */
                uint8_t        dead_count;
                int32_t        pot; /* the middle, this round's bets in */
                int32_t        to_call, min_raise, max_raise;
                uint8_t        can_raise;
} pk_view_t;

/** @brief @p seat's view. */
void pk_view(const pk_table_t *t, int seat, pk_view_t *v);

#ifdef __cplusplus
}
#endif

#endif /* LUCKYDUCK_POKER_RULES_H */
