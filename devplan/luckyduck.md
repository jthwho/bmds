# Lucky Duck: the casino

Solitaire grows into a collection of card, dice and casino games under one roof and one theme:
**Lucky Duck**, a casino run by ducks. Planned 2026-10-15 from the user's request ("something
for everyone", "have fun with the animations"). `games.md` keeps the surfaces, `libbmdsgame`,
Blocks, Doom and Solitaire's original design (§21, §22); this document is the casino's.

## 1. Decisions (the user's, 2026-10-15)

- **One application**, "Lucky Duck", replacing `solitaire.app`. Its first games: Solitaire (its
  rules and play unchanged), Blackjack, a Yahtzee-rules dice game, Video Poker, and Poker from
  five cards to seven. More from §12's list later.
- **The theme is ducks** throughout: the card courts, the backs, the chips, the dealer, the
  opponents, and the animations ("ducks doing things").
- **A maximized window** (the user's, 2026-10-15, replacing "1280×720 for everything" and the
  full screen step 1 fell back to): the picture is the maximized window's client area (1272×664
  on the 720p desktop), the layouts placed from it (§15). Scaling with any window size isn't
  asked for yet.
- **Order:** the shell and Solitaire, Blackjack, the dice game, Video Poker, Poker, then the
  network.
- **The mouse first**, the keyboard minimal: every button has a key letter, plus Escape, F1 and M.
  Solitaire keeps its full keyboard play, since it already has it. No pad, and no
  `controls.conf` (raw keys, as Solitaire).
- **Multiplayer later** (local network or an internet server, details to be decided), so the
  table games are built for it from the start (§6).
- **Play money only:** no purchases, nothing worth money.

The name: the user named the dice game "Lucky Duck" and made it the theme. The casino carries the
name, and so does the dice game's five of a kind (Yahtzee's, a trademark we don't use). The dice
game's menu title is "Lucky Duck Dice" until the user says otherwise.

## 2. Shape

- **`programs/luckyduck/`**, one C program on `libbmdsgame`, built by **`dist/luckyduck`** into
  `apps/luckyduck.app` (`X-BMDS-Id=luckyduck`). `dist/solitaire` and `programs/solitaire` go: it
  is unreleased, so nothing is kept for the old name (the card's `solitaire.app` is removed when
  the new one is installed).
- **`luckyduck [GAME]`:** the lobby, or straight to `solitaire`, `blackjack`, `dice`, `video` or
  `poker`. `-W` keeps Solitaire's win demo.
- **One surface for the whole run** (1280×720 RGB565, two buffers). Each game is a **scene**: a
  table of callbacks (enter, leave, event, tick, draw) that the loop's callbacks forward to. The
  lobby is a scene too. A change of scene is a transition (§5) on the same surface: no window
  closing and reopening.
- **Files:**
  - `main.c`: the loop, the scenes, the lobby.
  - `stage.{c,h}`: sprites, the background, damage, motion, particles (§4).
  - `ui.{c,h}`: buttons, panels and boxes, the chip rack, the headline font, messages.
  - `bank.{c,h}`: the bankroll and the statistics (§7).
  - `deck.{c,h}`: cards, decks and shoes, the seeded random numbers (xoshiro128**).
  - Each game: its rules as pure C (`*_rules.c`), host-tested (`tests/host/luckyduck_*.cpp`), and
    its scene (`solitaire.c`, `blackjack.c`, `dice.c`, `video.c`, `poker.c`). Solitaire's
    `rules.c` moves unchanged, with its tests.
  - `hand.{c,h}`: the poker hand evaluator, which Video Poker and Poker share.
  - `assets/`: Python (the standard library only, as now) drawing every picture and sound at build
    time, one module per kind (cards, chips, dice, ducks, tables, font, sounds).
- **The data folder** (`bmds_app_data_dir()`): `settings` (each game's options, "game.name value"
  lines), `bank` (§7), `scores` (the dice game's high scores).

## 3. The art (all ours, drawn by `assets/`)

Cartoon style with dark outlines: keyed sprites on RGB565 have hard edges, and an outline hides
them, so no alpha blending is needed. Supersampled 4×4 like the present cards.

- **Cards, 100×140** (a real card's 5:7). Large corner indices, which matter when cards overlap
  in a stud hand or a column. Standard pips.
  - The courts are duck figures, double-headed: the King with a crown and cape, the Queen with a
    tiara and pearls, the Jack with a feathered cap.
  - The Ace of Spades has a duck in its big spade.
  - A jester duck Joker, for later games.
  - Two backs: green with the Lucky Duck emblem, and red, for a second deck.
  - One sheet of 13×5 tiles (the last row the backs, the places and the Joker): 1300×700 RGB565,
    1.8 MB.
- **Chips:** $1 white, $5 red, $25 green, $100 black, $500 purple, $1000 gold with a duck. Each
  has a top view (48 px) and an edge view for stacks (4 px each).
- **Dice, 56 px:** ivory with black pips, the one-pip a little duck's head.
  - Twelve tumbling frames: a rounded cube ray-traced in Python at random turns, with the faces
    shown.
  - A shadow.
- **Ducks:**
  - **Lucky**, the mascot: yellow, a green visor and a bow tie. Poses: idle with a blink, a
    four-frame waddle, a quack (beak open, a "QUACK!" bubble), a cheer (wings up, a hop), a droop,
    a deal (a wing flick), a think (a wing on the chin), asleep. About 160 px.
  - **The players:** ducks of other kinds, each with a name and a hat: a mallard, a white Pekin, a
    wood duck, a black duck, and a rubber duck.
  - **Ducklings** at 48 px, for the lobby and the celebrations.
- **Tables:** each scene's background is one 1280×720 picture drawn by `assets/` (felt with a
  vignette, a wooden rail on the oval tables, the game's name and the Lucky Duck arc printed on
  the felt), loaded on entering the scene into 1.8 MB.
- **The headline font:** stroked letters with an outline and a shadow, at 48 and 28 px: "BUST",
  "BLACKJACK!", "LUCKY DUCK!", "FULL HOUSE", payouts. A Hershey-style stroke set written into the
  script (A–Z, 0–9, `$!?.,:+-`). Body text stays `libbmdsgame`'s 8×16, at scale 1 or 2.
- **Sounds**, synthesized like Solitaire's: a card's snap and flip, a riffle shuffle, chip clinks
  (several, picked at random), the pot swept, dice shaken in a cup and clattering, quacks (a few
  pitches), a button's click, a win, a big win, a loss's "wah-wah".

## 4. The stage: drawing and motion

Solitaire's drawing generalized: each frame lays out a display list, compares it with the last
frame's, and redraws only the rectangles that differ. A rectangle is repainted from the scene's
background picture (`bmds_canvas_copy()`'s sort of copy) and then the sprites over it, in order.

- **A sprite:** an image tile, a position (1/16 px), a scale across and down (1/256), a turn
  (1/256 of a full turn), a layer, a shadow (a lift in pixels: the shadow drawn offset by it).
- **Motion:** tweens on a sprite's properties, with a start delay, a length in ticks and an
  easing: linear, out (quadratic), in-out, out-back (an overshoot: a pop), a bounce. Tweens chain
  into sequences.
- **Particles** (up to 256): confetti, feathers, sparkles, coins. Each has a speed, gravity, a
  spin and a life. They are small, so their damage stays small.
- **Number roll-ups:** a payout counting up, the bank counting with it.
- **Events drive the animation:** a table game's rules emit events (§6). The scene turns each
  event into motion queued after the last: the rules have moved on, and the picture catches up.
  - Input waits for the queue to empty, or a click hurries it (finishes it), as Solitaire's
    flights do.
- **The shared vocabulary:**
  - **A deal:** an arc from the shoe or deck, spinning flat as it lands, the shadow shrinking.
  - **A flip:** the card's width eases to 0, it changes face and eases back out, lifting slightly
    (a shadow) as it turns.
  - **A slide.**
  - **Chips:** they stack one at a time as they arrive, with clinks, and a pot sweeps to its
    winner.
  - **A shake** (a bust).
  - **A pop** (out-back) for headlines and wins.
  - **A glow pulse** around a winning hand.
- **`libbmdsgame` gains** the drawing these need. It is a static library with no ABI promise
  (`game.h`): a game keeps the version it was built with, so additions aren't an ABI change.
  - `bmds_canvas_blit_scaled()`: an image tile drawn to a destination size, nearest pixel, keyed.
    A negative width mirrors it. A flip is this.
  - `bmds_canvas_blit_rotated()`: a tile turned and scaled about its centre, nearest pixel,
    keyed: spinning cards, confetti, tumbling. Clipped to the rotated bounds.
  - `bmds_canvas_shade()`: the pixels under a keyed tile's shape (or a rectangle) darkened by a
    factor: shadows.
  - `bmds_canvas_fill_blend()`: a translucent fill, to dim the table behind a box.
  - Each is written for RGB565 first (the other formats as the plain C way), and host-tested in
    `tests/host/game.cpp`.

## 5. The lobby

The casino floor, on its own background: each game is a table drawn as a big button (a small
felt table with the game's name and a picture: fanned cards, chips, dice). The bank is at the top
right, and a statistics panel opens from it.

- Lucky waddles between the tables, stops to quack, and sleeps when nothing happens for a minute.
- A click on a table plays a **transition**: a line of ducklings waddles across the screen, and
  the new scene is drawn behind them.
- Escape, or the "Lobby" button on every table, goes back, and that ends the game being played.
  When a game is under way (a Solitaire deal moved, chips on a table, a dice game started), a box
  says so first ("Leave the table? This game ends: the chips bet are lost.") and asks: Leave
  (Enter, L) or Stay (Escape, S). The user's, 2026-10-15.
- Games not built yet show as tables with a "coming soon" sign.

## 6. Tables built for multiplayer

Each table game (Blackjack, the dice game, Poker) is built so a network can replace a seat later
without the rules or the scene changing.

- **The rules** are a pure-C state machine, deterministic from a seed and the actions taken:
  - `*_act(table, seat, action)` checks an action ("seat 2 raises 50") and applies it, or refuses
    it.
  - Each change is appended to an **event log**: a card dealt to a seat (and who may see it), a
    card turned up, a bet, chips moved, a hand's result.
  - The scene reads the log: the events drive the animation (§4), and the log is also what a
    network would send.
- **The dealer** (the "host") holds the random numbers and the whole state.
- **A seat sees only its own view:** `*_view(table, seat, ...)` and the events filtered to what
  that seat may see (a face-down card is a back). The local scene draws only from its own seat's
  view, and bots decide only from theirs. Nobody peeks, so a network client never needs, or gets,
  the other hands.
- **Seats:** the local player, a bot (in-process, from its view), or later a remote player. The
  rules don't know which.
- **The network (step 6, its own design):**
  - The host is the board that opens the table. Clients send actions, and the host sends each
    client its view's events.
  - TCP with a small framed binary protocol, versioned. Tables on the local network are found by
    multicast, which the sockets already have.
  - An internet server, a relay holding no state, is decided later.
  - `libbmdsgame`'s loop watches only the surface's descriptor, so it gains a way to watch others
    (the game's sockets). That's a library addition again.
- Solitaire and Video Poker are one player's games and stay outside this.

## 7. The bank

- **Starting money:** $1,000 shared by Blackjack, Video Poker and Poker (a buy-in for a cash
  table, or a fee for a tournament).
- **Going broke:** below the smallest table minimum, Lucky tops you back up to $1,000, with a
  sad quack. The top-ups are counted.
- **Kept:** the balance, its highest, the top-ups, and per game the hands played, won and lost
  and the biggest win. Written when a hand ends, never mid-animation.
- **Solitaire's Vegas scoring plays from the bank** (the user left it to us, 2026-10-15: it is
  what Vegas scoring means, and it fills §22's "Vegas' cumulative score"): a Vegas deal costs $52
  from the bank and each card sent up pays $5 back (taken back if it comes down). Leaving or
  dealing again keeps what was won. Standard scoring and none don't touch the bank. The rules are
  unchanged: the scene moves the money as the score moves.

## 8. The games

### 8a. Solitaire

`rules.c` moves unchanged, along with its tests. The scene is today's `solitaire.c` re-laid out
at 1280×720 with the 100×140 cards:

- **Layout:** columns 150 apart, the top row at y 24, the columns from y 196, the status line at
  y 690.
- **Steps:** face-down 10, face-up 36 (closer when needed).
- **Unchanged:** the drag, the double click, the right click, the full keyboard, the flights, the
  options, and the win's bouncing trails.
- **Boxes:** drawn in the new style.
- **Settings:** under "solitaire.", in the casino's settings.
- **Vegas** plays from the bank (§7).

### 8b. Blackjack

- **Rules:**
  - Six decks in a shoe, reshuffled at the cut card (about three quarters through).
  - The dealer stands on a soft 17, or hits it (an option). A blackjack pays 3:2.
  - Doubling on any first two cards, and after a split.
  - Splitting to four hands. Split Aces take one card each and can't be split again.
  - Insurance, and even money.
  - Late surrender (an option).
  - Limits: $5 to $500.
- **Seats:** five spots. You take one, and the others are empty or bot ducks playing basic
  strategy. Bots make the table lively, and their spots are the future network's.
- **A hint button:** what basic strategy says, with a duck's whisper.
- **Animation:**
  - Lucky deals from the shoe: cards slide out of the shoe, arc to their spots, and spin flat.
  - The hole card: the dealer peeks under it when showing an Ace or a ten.
  - Betting: a click on a chip in the rack flies it to the betting circle, where it stacks with a
    clink. A right click takes the top one back.
  - Payouts slide from the dealer's rack.
  - A bust: the hand shakes, "BUST" pops, and the cards are swept away.
  - A blackjack: sparkles, "BLACKJACK!", and the duck at that spot cheers.
  - The reshuffle: a riffle animation, with its sound.
- **Host-tested:**
  - Hand values (soft and hard).
  - The dealer's play under both rules.
  - Every action's legality.
  - Splits, doubles, insurance and surrender.
  - Payouts.
  - The shoe and the cut card.
  - Determinism (the same seed and actions give the same game).

### 8c. Lucky Duck Dice (the dice game)

- **Rules:** Yahtzee's 13 boxes:
  - The upper section, ones to sixes, with 35 for 63 or more.
  - Three and four of a kind, a full house (25), a small straight (30), a large straight (40), a
    **Lucky Duck** (five of a kind, 50), and chance.
  - Further Lucky Ducks score 100 each, with the joker rule.
  - Three rolls a turn. A click on a die holds it.
- **Players:** alone (against the high scores) or against one to three bot ducks.
- **Bots:** they choose holds by expected value over the boxes still open. Good, but beatable.
- **The scorecard,** on the right: hovering over a box shows what it would score now.
- **Animation:**
  - Lucky shakes the cup (with its sound) and throws. The dice tumble across the felt: they
    bounce off the rail and each other, spin through their tumbling frames, and settle on what the
    rules rolled.
  - A held die hops up to the hold row.
  - Scoring: the points fly to their box.
  - A **Lucky Duck:** the dice line up, feathers fall as confetti, ducklings march across, and
    "LUCKY DUCK!" pops.
- **High scores** are kept in the data folder.
- **Host-tested:** every box's score, the bonus, the joker rule, and the bot's choices being legal.

### 8d. Video Poker

- **Rules:**
  - Jacks or Better with the full-pay 9/6 table.
  - Bets of one to five coins of $1 or $5, from the bank.
  - Deal, hold (a click on a card), draw. The pay table lights the hand.
  - Double-up later.
- **Animation:** a held card lifts and shows "HELD". The drawn cards flip in a ripple. A win
  counts up coin by coin, with a gold flash for a royal flush.
- **Host-tested:** the evaluator's ranking and the payouts.

### 8e. Poker

- **Variants:**
  - Texas Hold'em, no-limit by default (the user's), fixed-limit as an option.
  - Five-Card Draw, Five-Card Stud and Seven-Card Stud (fixed-limit, with antes and the stud
    bring-in).
- **The table:** 2 to 6 seats. A **cash game** buys in from the bank. A **tournament** is a
  sit-and-go with rising blinds.
- **Rules:**
  - Blinds and antes, the betting rounds, all-ins and side pots.
  - Stud's bring-in and who acts first.
  - The draw.
  - Showdown order, and split pots.
- **`hand.c`:** the best five of up to seven cards as one comparable 32-bit value, from rank masks
  per suit (flushes, straights, sets). Host-tested against brute force over every five-card
  choice.
- **Bots:**
  - **Equity:** a Monte Carlo estimate of their chance of winning against the players still in,
    from 500 to 2000 random deals, spread over ticks.
  - **The decision:** that chance weighed against the pot odds and position.
  - **A personality per duck:** how many hands it plays, how aggressive it is, how often it
    bluffs.
  - **A visible "think":** a wing on the chin, which also covers the time the estimate takes.
- **Animation:**
  - Chips into the pot, and the pot swept to the winner (split pots split).
  - The flop dealt three in a row and flipped in a ripple. The turn and the river each come after
    a pause.
  - A fold: the cards slide in face down.
  - The showdown turns the hands over in order. The winning five cards lift and glow, and the
    hand's name pops ("FULL HOUSE").
- **Host-tested:**
  - Each variant's betting rounds.
  - Side pots.
  - Showdowns and split pots.
  - The bring-in.
  - The bots' actions being legal.

## 9. Input

**Conventions for every game** (the user's, 2026-10-15):

- **System buttons apart from the game's:** Lobby, Rules or Help, Options, New game are stacked at
  the top right (`app_system_button()`); what is played (Hit, Deal, Undo, the chip rack...) is in
  the bottom band.
- **Ending a game under way asks first** (`app_confirm()`: the answer, or "keep playing"): back to
  the lobby, a new game, and closing the window. A game counts as under way when leaving would
  lose it (Solitaire after its first move; Blackjack's round in play; a bet not yet dealt comes
  back, so betting doesn't).
- **Rules and help in one box** (`app_info()`), not printed on the table.


- **The mouse:** everything that can be done is a button or a thing on the table to click, lit
  while the pointer is over it. A right click undoes a bet's last chip.
- **Keys:** each button shows its letter (Blackjack's H, S, D, P, R). Escape is the lobby, F1
  the help, M the sound.
- **Solitaire** keeps its full keyboard play.

## 10. Budgets (estimates, measured as the steps land)

- **PSRAM:** about 10 MB of 32.
  - The two buffers: 3.7 MB.
  - A scene's background: 1.8 MB.
  - The card sheet: 1.8 MB.
  - The ducks: about 1 MB.
  - Chips and dice: 0.5 MB.
  - Sounds: about 1 MB.
- **Code:** about 200 KB.
- **Frames:** 60 a second with a busy deal (up to 8 card-sized sprites moving, some turned). The
  rotated blit is measured first, so the deal's spin can be cut if it costs too much.
- **A background's load** (a 1280×720 PNG from the card): measured. If it is slow, the felt is
  drawn on the board instead and only the prints are loaded.

## 11. Steps

1. **The shell and Solitaire:**
   - `programs/luckyduck`: the scenes, the stage, the UI, the bank's start.
   - `libbmdsgame`'s four calls (§4), host-tested.
   - `assets/`: the cards with duck courts and backs, the felt, Lucky's first poses, the headline
     font, the sounds.
   - The lobby, with Solitaire's table and "coming soon" signs.
   - Solitaire at 1280×720.
   - `dist/luckyduck` replacing `dist/solitaire`, installed on the card when the user asks.
2. **Blackjack:** the chips, the bank, Lucky dealing, the bot ducks.
3. **Lucky Duck Dice:** the cup, the tumbling dice, the bots, the high scores.
4. **Video Poker:** `hand.c`.
5. **Poker:** the four variants, side pots, the bots, tournaments.
6. **Network play:** its design first. Sockets exist, so no new system calls are expected.
7. **More games** (§12), as the user picks them.

## 12. More games (suggested, for the user to pick)

| For | Games |
|---|---|
| Quick, alone | FreeCell, Spider, TriPeaks (Solitaire's engine) |
| Card players (four seats: good for the network) | Hearts, Spades, Euchre |
| Two players | Cribbage, Gin Rummy |
| Dice | Liar's Dice (bluffing ducks), Farkle, Craps |
| The casino floor | Roulette (a showpiece wheel), Baccarat, slots |
| Children | Crazy Eights, Go Fish, War, Memory |

## 13. Open questions

- The dice game's title ("Lucky Duck Dice"?), §1.

Settled 2026-10-15: Vegas plays from the bank (§7); Hold'em is no-limit by default; leaving a
table ends its game, after asking (§5), so nothing is kept across runs but the bank, the
statistics and the settings.

## 14. As built: step 1, the shell and Solitaire (2026-10-15)

- **`programs/luckyduck/`:** `main.c` (the loop, the scenes, the ducklings' transition, the leave
  box, the felt), `lobby.c`, `solitaire.c` with `solitaire_rules.c` (moved unchanged, with
  `tests/host/solitaire.cpp`), `stage.{c,h}` (§4's display list: items by key, compared with the
  last frame's, redrawn from the felt in order of z), `ui.{c,h}` (the headline letters, panels,
  buttons, cards), `data.c` (the settings and the bank, "name value" files in the data folder).
  **`dist/luckyduck`** replaces `dist/solitaire`; `programs/solitaire` is gone.
- **`assets/`** (Python's standard library): shapes as signed distances (an edge's coverage one
  evaluation, an outline the shape grown), the stroke font (A–Z, 0–9, `$!?.,:+-'/%x`, three
  styles: gold 40 px, white 24 and 15, outlined), the ducks (one duck from parts; Lucky's twelve
  poses, ducklings, the mallard, Pekin, wood, black and rubber ducks drawn for later), the cards
  (duck courts: the King a mallard, the Queen a Pekin, the Jack a duckling), the lobby's tables,
  the sounds (quacks through two formants, a sad trombone). `build.py` draws in 8 processes
  (26 s), packs `sprites.png` and `font.png`, and writes `art.h`/`art.c` (each sprite's and
  glyph's place) for the program.
- **`libbmdsgame`** (§4): `bmds_canvas_blit_scaled()`, `_blit_rotated()` (its sine a polynomial:
  no libm), `_shade()`, `_shade_image()`, `_fill_blend()`, host-tested (scaled, mirrored,
  quarter and half turns, an eighth's bounds, shades, blends in each format). The keyed blit's
  RGB565 rows now go a word at a time (it was a call per pixel).
- **The lobby:** the carpet drawn on the board, the tables (lifted with a shadow under the
  pointer; the four not built behind "coming soon", a sad trombone if clicked), the bank, the
  sound button, Lucky walking, stopping, blinking, quacking (out loud when clicked), asleep after
  45 s without input (Zs rising), cheering when woken.
- **Solitaire:** §21's game at 1280×720 (columns 150 apart), a status band with the score, time,
  moves and buttons (Lobby, Deal, Undo, Options, Help), boxes as panels. Vegas from the bank (a
  demo deal neither costs nor pays). Escape lets go of a held run, else leaves.
- **Leaving** asks while a game is under way (the dimmed table, Leave: Enter or L; Stay: Escape
  or S); Stay redraws the table.
- **Checked on the board** (`win hid`, `win pointer`, screenshots): the lobby, the transition, a
  deal, drawing, the leave box (keys and mouse, Stay and Leave), Vegas taking $52, the options
  box, the win's bouncing trails at the new size, the files in `$HOME/.local/share/luckyduck`.
  Sound wasn't heard here (it loads without errors).
- **What drawing costs (measured), and what was done about it:** the CPU moves PSRAM at about
  28 MB/s each way (`docs/platforms/esp32p4.md`): a whole felt copied is 64 ms, a card blitted
  0.8 ms, a redrawn pixel about 230 ns with the felt and a card (~6 bytes of traffic a layer).
  - The ducklings' wipe draws only what it uncovers: the scene's ticks wait for it (a deal
    behind it cost 30-50 ms a frame). It runs at ~45 frames a second (~1.3 s across).
  - Painting the felt from per-pixel arithmetic took 230-320 ms when a scene started; from
    tables of finished pixels (shade × grain-and-dither), 84 ms.
  - A deal's waiting cards are drawn once (the top of those on the stock), and a redraw skips what
    cards over it hide (`ui_uncovered()`: rows or columns a card covers right across), the felt
    too. A deal still costs ~30 ms a frame (5 cards in flight, ~130k pixels): 20-30 frames a
    second, the ticks keeping 60. **Next if it matters:** composing a redraw in internal RAM
    (one PSRAM write a pixel, not one a layer: about a third less).
- **Not yet:** the in-game statistics panel (the bank's highest and top-ups are kept, not shown).

## 15. After the user's first look (2026-10-15)

The user's notes on step 1, and what was done:

- **The old Solitaire** is removed from the card.
- **A maximized window, no full screen.** The surfaces gained (the user approved the ABI):
  - `BMDS_SURFACE_MAXIMIZED` (0x8): the window opens maximized; with `width` and `height` 0 the
    surface is its client area at scale 1 (the work area less the frame; at most 1280×720, the
    width down to a multiple of 4). A size given is kept.
  - `bmds_surface_info_t` appends `picture_width` and `picture_height` (52 to 60 bytes): what a
    0×0 surface got.
  - `libbmdsgame` takes 0×0 and tells the game (`bmds_game_width()`, `bmds_game_height()`).
  - Lucky Duck asks for it and enters its first scene at the first callback (the size known);
    the lobby, Solitaire (columns centred, the band at the bottom), the boxes and the leave box
    are placed from the size. On the board: 1272×664, the window `(maximized)` in `win list`.
  - Not done: following a window resized after (a resizable surface: a resize event, buffers
    made again, layouts that adapt to any size). Restoring the window keeps the picture's size.
- **The quack bubble facing left** read backwards (it was mirrored): a second bubble with its tail
  on the right.
- **The title's sign:** a marquee round "LUCKY DUCK CASINO" on the felt, 44 bulbs chasing in pairs
  for six seconds, then flashing odd and even for three. Each bulb's glow is drawn on the sign's
  frame colour; a change redraws only bulbs.
- **The ducks, redrawn:** lit from the upper left (each part darker toward its far edge, a
  highlight), three tail feathers, primaries fanning back under a smaller wing, the chest's
  feathers, an eye with an iris, a pupil, two highlights and a lid, a beak with a mouth line,
  a shine and a nostril, webbed feet with toes, a bow tie with folds, a ground shadow (a shade).
- **The cards, redrawn:** courts with a latticed ground, a robe with a gold diamond pattern,
  ermine (King, Queen) or a striped tunic (Jack), the suit on a medallion, a jewelled crown with
  pearls, a tiara, a feathered cap, a sceptre, a rose, a halberd, a gold band between the halves;
  pips with a gentle gradient; the back's lattice gold-studded, clovers in its corners, Lucky in a
  gold cartouche.
- **Kerning:** worked out at build time from the glyphs' shapes (each glyph's ink profile row by
  row; a pair moved so its gap, mostly at its narrowest rows, matches "HH"'s), about 1,800 pairs a
  weight, the ones that round to a pixel kept in `art.c` and applied by `ui_text()`. The art's own
  text (plates, signs, the cards' corners) is kerned the same way. Assets now take ~90 s.
- **A system font library** (the user's suggestion, agreed as its own project): TrueType
  rasterized (stb_truetype), antialiased, kerned, glyphs cached, for programs and later the
  desktop. Lucky Duck's headline letters move onto it when it lands. To be planned in its own
  devplan.
- **Better sounds:** kept for the end (polish), as the user asked.
- **Then (the user's):**
  - The status band's letters touched: an outlined style's outline ate the gap between letters.
    Each style now has its own spacing (the small one 2 units more, the medium 1, the big title
    none), and is kerned with it.
  - Solitaire's table lit with the pointer over others: the oval test overflowed (dx² × 76² past
    2³¹ wrapped negative, "inside" the first table); now in 64 bits. Its plate's hit area is the
    plate's middle (it was 220 wide).
  - It stayed lit after the pointer left the window: surfaces now say so (the user approved the
    ABI): `BMDS_POINTER_GONE` (-32768), one `BMDS_EVENT_POINTER` with x and y both it as the
    pointer leaves the window (the window manager's `Leave`, dropped before). Checked on the board:
    over the Dice table Solitaire stays down, over Solitaire it lifts, onto the panel it drops.
  - "BANK" sits over the amount, both to the right, as the amount can grow to many digits.

## 16. Step 2 design: Blackjack (2026-10-15)

§8b's rules, built as §6 asks (rules that run from actions and a seed, seats, per-seat views, an
event log that drives the picture).

- **`deck.{c,h}`:** xoshiro128** seeded (the rules' only randomness), Fisher-Yates shuffles. A
  card is a byte (suit × 13 + rank, as Solitaire's).
- **`blackjack_rules.{c,h}`** (pure C, host-tested in `tests/host/luckyduck_blackjack.cpp`):
  - A table of 5 seats (seat 0 at first base, the screen's right; the player at seat 2), each with
    a stack and up to 4 hands; the dealer's hand; a 6-deck shoe with the cut card at 75%
    (reshuffled before the next round once passed).
  - Phases: betting, insurance (the up card an Ace), play (seat by seat, hand by hand), the
    dealer, settled. Actions: `bj_bet()`, `bj_deal()`, `bj_insure()`, `bj_act()` (hit, stand,
    double, split, surrender), each refused when not legal (`bj_can()`).
  - Rules (options in the table): the dealer stands on soft 17 (or hits it), peeks with an Ace or
    a ten up, blackjack pays 3:2 (rounded down to the dollar), insurance 2:1 (half the bet; with a
    blackjack it is even money), double on any two (after a split too), split equal values to 4
    hands (split Aces one card each, no more), late surrender (an option, on by default), a player
    bust loses even if the dealer busts.
  - **Events** (a round's log): shuffle, bet, deal (a seat's hand or the dealer's, up or down),
    the hole card turned, split, double, insurance, a turn, bust, blackjack, a hand's result and
    payout, the round's end. `bj_event_for(seat)` gives an event as that seat may see it (the hole
    card hidden until turned): the scene reads only those, as a network client would.
  - **Basic strategy** (`bj_basic()`: S17 or H17, doubles, splits after a split, surrender) plays
    the bot ducks and answers the hint button.
- **The scene (`blackjack.c`):**
  - The felt painted on entering: a curved table edge and its padded rail, five gold betting
    circles, and the prints along arcs ("BLACKJACK PAYS 3 TO 2", the soft-17 rule, "INSURANCE PAYS
    2 TO 1") in the headline letters, each glyph turned along the curve (`ui_text_arc()`).
  - Lucky deals from a shoe at the top right; the discards go to a tray at the top left.
  - The player's hands at full size in the middle (smaller cards from 3 hands), the bots' at 72%
    on either side, each bot duck beside its circle with its name and stack.
  - **The picture lags the rules:** the scene replays the log one event at a time into its own
    model of the table, each event starting its animation and holding the next for its length (a
    card dealt 12 ticks, the hole card turned 14, a result 30). Input waits until it has caught
    up. A bot acts after a short "think".
  - **Motion:** every card and chip pile follows its place: when the model moves it (dealt, split,
    swept), it eases from where it shows; a card turned face up narrows and widens
    (`bmds_canvas_blit_scaled()`), dealt cards spin as they fly. Payouts slide from the dealer's
    tray, losses to it; results pop as headlines ("BLACKJACK!", "BUST", "WIN", "PUSH").
  - **Betting:** a chip rack ($1, $5, $25, $100, $500) in the bottom band; a click adds the chip
    to the bet (it flies to the circle), a right click on the circle takes the last back; Deal
    (Enter), Rebet (the last bet), Clear. Then Hit (H), Stand (S), Double (D), Split (P),
    Surrender (R), Insurance or Even money (Y/N), a hint (Lucky whispers basic strategy).
  - Chips: one sprite a denomination (a chip seen from above at an angle), a pile drawn bottom up
    a few pixels apart, its amount made of the fewest chips.
  - The bank is the player's stack: a bet leaves it when placed, payouts return to it, and the
    bank is written at the round's end. Below the table minimum, Lucky tops it up (§7).
  - Options: the soft-17 rule, surrender, bot ducks (0-4, default 2).

## 17. As built: step 2, Blackjack (2026-10-15)

- **`deck.{c,h}`, `blackjack_rules.{c,h}`** as §16, host-tested (`tests/host/luckyduck_blackjack.cpp`,
  11 cases): the random numbers (same seed, same numbers; unbiased ranges) and shuffles, totals
  (soft, hard, hidden cards), bets and their limits, the deal's order, busts and the dealer's
  draws, pushes, both soft-17 rules, blackjacks at 3:2 (an odd bet rounded down), the peek,
  insurance (taken and declined, won and lost), doubling (and its stack), surrender (and when not),
  splits with a double after, split Aces, the cut card's reshuffle, the hole card hidden in the
  seats' log, basic strategy.
- **`blackjack.c`:** the table as §16 planned. The felt has its curved rail, five gold circles,
  "BLACKJACK PAYS 3 TO 2" along an arc (`ui_text_arc()`: each glyph turned with the curve) and the
  two rules printed flat. Lucky deals from the shoe (a flick of the wing a card), cheers at the
  player's blackjack and droops when the dealer busts. Mallory, Pearl, Woody and Squeaky (the
  number in the options, default 2) bet, think, play basic strategy, and look happy or sad.
  Cards spin as they fly from the shoe, land and turn; splits slide apart, a bet pile under each
  hand; results pop up over the hands; payouts slide from the dealer to the bet and home, losses
  to the dealer. A gold arrow bobs over the hand to act. The hint (T) is Lucky whispering basic
  strategy. Keys: Enter deal, B rebet, C clear, H/S/D/P/R, Y/N insurance, T hint, O options.
- **Stage and helpers:** `STAGE_CHIPS` (a pile from the fewest chips, `ui_chips()`), and new art:
  six chips at an angle (they stack 5 pixels apart) and flat for the rack, the shoe, the tray, the
  arrow, the bot ducks in four poses.
- **Checked on the board:** rounds dealt and played to the end (bets flying to circles, the deal's
  spin, a blackjack, a bot's bust, the dealer drawing, results and payouts, the bank moving), a
  split (with a temporary stacked shoe, removed), the hint, the buttons for each phase. Found and
  fixed there: the dealer's draws now wait for their cards to land and turn before what follows
  shows (a WIN showed while the dealer's card was still face down); the band's bank and bet moved
  to two lines and the hint onto the felt (the player's turn has five buttons).
- **Not yet:** insurance and even money seen on the board (host-tested only); the options box
  answers keys only (a click closes it); doubling's chips and surrender's half seen only by logic.

### 17a. After the user's look (2026-10-15)

- The table's prints are gone (the arc, the soft-17 and insurance lines): a Rules button (F1) shows
  them all in `app_info()`'s box. `ui_text_arc()` went with them.
- "BLACKJACK!" and the dealer's headlines are the size of "WIN".
- A chip adds what it says ($1 and $1K were raised to the minimum, cut to the maximum): the rules
  take any bet the stack covers and check the limits at the deal (`bj_bet_ok()`); out of them, the
  deal doesn't happen and the table says "TABLE MINIMUM IS $5" (or the maximum).
- The system buttons (Lobby, Rules, Options) moved to the top right, the shoe left of them; the
  band has only the game's. The shoe is drawn as one (the decks' edges stacked in a smoked box, a
  card at its mouth) and the tray shows a pile of the discards since the shuffle.
- Solitaire follows the same rules: Lobby, New Game (asks while a game is under way), Options and
  Help (now the shared box) at the top right, its columns 40 pixels left for them; Undo in the
  band; a change of draw or scoring waits for the next deal (dealt at once before the first move).
- Closing the window asks too while a game is under way.

### 17b. Cleanups (2026-10-15, the user's)

- Solitaire's columns are centred between the left edge and the system buttons.
- Its status fields each have a column (score, the bank in Vegas, moves, the time last): the time
  ticking no longer nudges what follows it.
- A confirm box's safe answer ("keep playing") is lit from the start and is Enter's, Space's,
  Escape's and N's; only Y or a click on the other button ends the game.
- Blackjack's band: BANK and BET side by side, a small label over each amount (stacked, the amounts
  ran into each other). The tray's discards are card backs lying in it, the pile a little higher
  as it grows (they were squashed upright cards).

## 18. Step 3 design: Lucky Duck Dice (2026-10-15)

§8c's game, built as §6 asks, with §9's conventions. Nothing in a die is hidden, so every seat's
view is the whole table, but the shape is Blackjack's all the same (actions, a seed, a log the
picture replays), so a network can take a seat later.

- **`dice_rules.{c,h}`** (pure C, host-tested in `tests/host/luckyduck_dice.cpp`):
  - Up to 4 seats (the player first, then the bots), 13 rounds, each seat's card of 13 boxes
    (open, or the points written).
  - A turn: `dice_roll()` rolls the dice not held (three rolls at most), `dice_hold()` holds or
    frees a die (after the first roll), `dice_score()` writes the dice into an open box and ends
    the turn. Each is refused when not allowed.
  - **Scores** (`dice_box_score()`): the upper boxes their face's sum; three and four of a kind all
    the dice; a full house 25, a small straight 30, a large 40, a Lucky Duck 50, chance the sum.
    The upper bonus is 35 at 63 or more.
  - **A second Lucky Duck** pays 100 more when the Lucky Duck box holds 50, and plays as a joker
    (the official forced rule) whether the box holds 50 or 0:
    - its upper box if that is open;
    - otherwise any open lower box, at full value (a full house 25, the straights 30 and 40);
    - otherwise a zero in an open upper box.
  - **Events** (a ring of 256, read by a running count): the game's start, a turn, a hold, a roll
    (the five values and the holds), a score, a bonus (upper or Lucky Duck), the game's end (the
    winner).
  - **Bots** by expected value, without searching the whole game:
    - A box's worth is its score less what that box is worth on average (its par), plus credit
      towards the upper bonus.
    - The dice kept are chosen over the distinct multisets (462 of 0 to 5 dice), looking ahead
      to the rolls left (two at most). That is about 100,000 table lookups a decision.
    - Good but beatable: they don't plan across turns.
- **The scene (`dice.c`):**
  - **Layout:** the scorecard on the right, left of the system buttons. Lucky with the cup at
    the top left, the bot ducks beside it with their names. The tray (a felt box in a wooden rail)
    below them. A shelf of five slots under the tray, where held dice stand. Roll in the band.
  - **The scorecard** is painted into the felt: the labels, the grid, the players' names. The
    stage draws only the numbers. Written scores are white. What the dice would score in each
    open box is shown in a new gold small style (`ART_FONT_HINT`) in the column of the player whose
    turn it is. Hovering over a box lights its row, and a click writes it.
  - **A roll:** the dice not held are scooped into the cup. Lucky shakes it (a new rattle sound,
    three poses with the cup) and throws. The dice fly from the cup's mouth and tumble across the
    tray:
    - **Physics:** position, height and spin; bounces off the rail and each other; friction; a
      shadow that drifts away from the die as it rises.
    - **Frames:** in flight they show tumbling frames (the cube drawn at angles), turned in the
      direction they travel. They land on the face the rules rolled (a click of a clack sound).
    - The physics is the picture's, from its own random numbers: the rules decide only the values.
  - **Holds:** a click on a die (or keys 1 to 5) holds it: it hops into its slot on the shelf,
    and a second click hops it back to the tray.
  - **Scoring:** the points fly from the dice to the box, and the box shows them when they land.
    The upper bonus and a second Lucky Duck's 100 pop up beside the card.
  - **A Lucky Duck rolled:** the five dice line up on the shelf, feathers fall as confetti,
    ducklings march across the tray, and "LUCKY DUCK!" pops up with the fanfare.
  - **Bots:** they think, hold dice one by one (each hop seen), roll, and write a box. They look
    happy at a good score and sad at a zero. Options make them fast.
  - **Buttons:**
    - The system buttons (top right): Lobby, Rules, Options, New Game.
    - In the band: Roll (R, Space, Enter) and Hint (T: Lucky whispers what the bots would do,
      "PSST... HOLD 5 5 5" or "PSST... FULL HOUSE!").
  - **The game's end:** a box with the final scores in order, the high scores (the top five; a
    new one marked), and Play Again (Enter).
  - **Under way** from the first roll to the game's end, so leaving asks (§9). Options (bot ducks
    0 to 3, default 1; fast bots) apply to the next game.
- **Kept:** the high scores in `scores` ("dice.0" to "dice.4" lines, the player's only), and
  `dice.played` and `dice.won` (won: beating every bot) in the settings.
- **Sounds:** `rattle.wav` (dice knocking in a leather cup) and `clack.wav` (a die landing).
- **No ABI changes.**

## 19. As built: step 3, Lucky Duck Dice (2026-10-15)

- **`dice_rules.{c,h}`** as §18, host-tested (`tests/host/luckyduck_dice.cpp`, 6 cases):
  - Every box's score; a turn's rolls and holds and what is refused; the upper bonus at 63.
  - Second Lucky Ducks: the 100, the joker rule's three cases, and a scratched box (the joker
    without the 100).
  - Whole bot games: 13 rounds, the winner, the same game from the same seed, seats not starting
    at 0.
  - The bots' choices: they average over 190 in the test.
- **The bots:** each box's worth is its score less its par, plus 0.6 a point above or below the
  upper bonus's pace (while the bonus can still be made), plus a second Lucky Duck's 100.
  - Measured on the host (300 solo games): 237 on average looking two rolls ahead, 233 looking
    one. Well-played solitaire averages about 254.
  - A decision takes 1.1 ms on the host at -O2. The work is about 9,000 lookups by binary search
    over the 462 multisets.
  - Woody looks one roll ahead; Pearl and Mallory look two.
- **The scene (`dice.c`)** as §18 planned. Placed from the picture's size and the players:
  - The scorecard: 190 for the labels, 108 a player.
  - The tray takes what is left, and the bots spread over its width.
  - The throw's speed scales with the tray's width (the dice piled against the far rail in the
    four-player tray).
  - Dice settle on any angle and are drawn turned; the stage redraws only what moved.
  - A die at rest is knocked by one still rolling, and slides on face up.
  - When a turn ends, its held dice go back to the tray, so the next player starts with nothing
    on the shelf.
  - The options box answers clicks as well as keys. Changing the bots between games deals a new
    game at once, keeping the box open.
- **Art:**
  - The dice from above in two sizes: the one is a red pip with a gold duck, the others drilled
    pips.
  - Eight tumbling frames: a rounded cube drawn turned, its faces lit by angle.
  - A die's shadow, two feathers.
  - Lucky with a leather cup in three poses.
  - `ART_FONT_HINT`: the small style in gold.
- **Sounds:** `rattle.wav` (knocks through resonances, dulled) and `clack.wav`.
- **Checked on the board:**
  - Rolls and tumbles, holds, hints, the hint bubble, scoring by click with points flying.
  - Pearl's turn with its holds.
  - A whole game to its end panel and high score.
  - Three bots' layout.
  - A Lucky Duck's party, with a temporary build rolling only fours (not kept).
  - Found and fixed there:
    - Dice were too big in the air and their shadows too dark; dice overlapped at rest.
    - A second key during a hop was dropped; the hint bubble covered Pearl.
    - The label column and the players' columns were too narrow.
    - The end panel's height now fits what is in it.

## 20. Randomness (2026-10-15, the user's ask: "a solid source of randomness")

- **Before:**
  - Every game was seeded from the monotonic clock: nanoseconds since boot, folded to 32 bits.
    That is guessable, and Blackjack's and the dice's 128-bit generator could only reach 2^32 of
    its states (about 4 billion shoes or games). Harmless alone, but over a network a client could
    recognise the shoe from a few cards.
  - Solitaire shuffled with its own xorshift32 and `% n` (a small bias).
  - `rand()` was seeded from the clock too.
- **Now:**
  - **`app_key()`** (main.c) gives each new game 128 bits from `bmds_random()`: the kernel's
    ChaCha20 pool, seeded from the hardware's noise (`random.md`). If that ever failed it falls
    back to the clock, saying so.
  - **`deck_key_t`** carries the 128 bits. `bj_init()`, `dice_init()` and `sol_deal()` take one,
    so a game is still the same from the same key (the tests use `deck_key(n)`; a network host
    would keep the key).
  - **`deck_seed_key()`** mixes each word reversibly (none of the key is lost). It then takes
    eight generator steps, because xoshiro128**'s first outputs come from one state word: the new
    test caught the first number ignoring three of the four.
  - **Solitaire** shuffles with `deck_shuffle()` (Fisher-Yates over the unbiased `deck_below()`).
  - **`rand()`**, seeded from `app_key()`, moves only the picture: bounces, feathers, the bots'
    bet sizes.
- **Tested:**
  - One bit changed anywhere in the key changes the stream.
  - An all-zero key isn't stuck.
  - A card lands in each of the 52 places about equally often over 52,000 shuffles.

### 20a. The dice game's last cleanups (2026-10-15, the user's)

- **Scorecard rows:**
  - Each row has a top and a height (`row_top()`, `row_h()`), and text is centred in its row by
    its font's capital height.
  - The header's gold line sits just above the ones row, which now has the same padding as the
    others (the line used to cut into it).
  - The total's row is 40 high for its bigger letters.
  - The card ends 10 pixels above the band.
- **Centring:** text was centred by how far the pen moves, which counts the letter spacing after
  the last letter. Each font now records that spacing (`art_font_t::tail`, from `build.py`), and
  `ui_text_ink_width()` centres buttons and centred stage text by the ink alone. "NEW GAME" was
  2.5 pixels left; every button is now at its middle, Blackjack's and the lobby's too.
- **The Lucky Bonus row** checked with a temporary build rolling only fours: a second Lucky Duck
  in fours shows 100 there and 170 in the total.
- **Dice leaving the shelf** (a turn's end, or a hold undone) look for a clear place
  (`clear_spot()`): where they last lay if no die is lying, landing or rolling within two radii;
  otherwise the nearest clear point on rings around it. They used to return to where they lay
  before, which later rolls may have filled.

## 21. Step 4 design: Video Poker (2026-10-15)

§8d's game, with §9's conventions. One player and nothing hidden from them, so it stays outside
§6: no seats, no event log. The scene animates what changed in the machine.

- **`hand.{c,h}`: the evaluator Poker will share.**
  - `hand_value(cards, n)` gives the best five of 5 to 7 cards as one 32-bit number. A bigger
    number is a better hand.
    - The category is in bits 20 and up: high card, pair, two pair, three of a kind, straight,
      flush, full house, four of a kind, straight flush.
    - Five ranks follow, four bits each, in the order they count (Ace high): for four of a kind,
      the four's rank and then the kicker; for a pair, the pair, then the kickers high to low.
  - It works from four rank masks, one per suit, and builds "seen once" to "seen four times"
    masks bit by bit. There are no tables beyond a 13-bit popcount.
  - Also `hand_category()`, `hand_rank()` (the n-th rank) and `hand_name()` ("FULL HOUSE").
  - **Host-tested:**
    - Every five-card hand (2,598,960) counted by category against the known numbers (4 royal
      flushes, 36 straight flushes, 624 fours and so on).
    - Random seven-card hands against brute force over their 21 fives.
    - Ties, kickers and the wheel (A-2-3-4-5, the lowest straight).
- **`video_rules.{c,h}`: the machine.** Pure C, host-tested in
  `tests/host/luckyduck_video.cpp`.
  - **Its state:** the deck, shuffled from a `deck_key_t` each hand; the five cards; the holds;
    the coins bet (1 to 5) and their value ($1 or $5); the stack in dollars; the phase (betting,
    dealt, done); the result and the win.
  - **Calls:** `vp_bet()` sets the coins, `vp_deal()` takes the bet and deals five, `vp_hold()`
    toggles a card's hold, and `vp_draw()` replaces the cards not held and pays. Each refuses what
    isn't allowed. Every hand gets a fresh shuffle with a new key, as real machines do.
  - **Three machines,** each a pay table as data: rows of a name, what each row matches, and the
    pay for one to five coins. A royal flush pays 800 a coin at five coins (4,000).
    - **Jacks or Better 9/6** (99.5% with perfect play), the default.
    - **Bonus Poker 8/5** (99.2%): fours pay more by rank (Aces 80; 2s, 3s or 4s 40).
    - **Double Double Bonus 9/6** (99.0%): fours with the right kicker pay up to 400; two pair
      pays 1.
    - `vp_pay(machine, cards)` gives the matched row and its pay.
  - **Perfect play** (`vp_ev_*`):
    - It tries every one of the 32 holds against every draw. Over all holds that is every
      five-card hand once (2,598,960 evaluations), so every hold's expected return is exact.
    - It runs in slices (`vp_ev_step(ev, budget)`), so the scene spreads it over frames while
      Lucky thinks.
    - It answers the hint ("PSST... HOLD J J", "PSST... DRAW FIVE").
- **The scene (`video.c`):** the machine is a cabinet painted on the felt, with a deep blue screen.
  - **The pay table** fills the top of the screen in gold: the rows, and a column for each bet
    of 1 to 5 coins.
    - The column bet is lit.
    - After the deal, the row of the hand already made is lit.
    - After the draw, a winning row flashes.
  - **Lucky** stands left of the screen with a face-down deck and deals:
    - **The deal:** cards fly to their five places face down, then flip left to right in a
      ripple.
    - **Holds:** a click on a card (or keys 1 to 5) holds it. The card lifts and "HELD" shows on
      it; a second click lets it down.
    - **The draw:** the cards not held flip down and fly back to the deck. New ones fly in and
      flip in a ripple.
  - **Winning:**
    - The cards that make the hand lift together with a glow, and the hand's name pops ("FULL
      HOUSE").
    - The win counts up coin by coin into the bank, coins flying from the screen to the bank
      with a clink.
    - Lucky cheers at three of a kind or better.
  - **A royal flush:** gold flashes over the screen, feathers fall, ducklings march under the
    cards, "ROYAL FLUSH!" pops big, and the fanfare plays.
  - **Buttons:**
    - System, at the top right: Lobby, Rules (F1), Options (O).
    - In the band: Hint (T), Bet One (B), Bet Max (X, which also deals), and Deal or Draw (Enter
      or Space).
    - The band shows BANK, BET ("3 × $1") and WIN.
  - **Under way:** from the deal to the draw (the bet is lost by leaving). Leaving or closing
    asks (§9).
  - **Options:** the machine and the coin's value, both kept. The coins bet are kept too.
    Changes apply at the next deal.
- **The bank:** the bet leaves it at the deal and the win returns to it. It is written when the
  count-up ends (or on leaving). Below one coin, Lucky tops it up (§7).
- **Kept:** in `settings`, `video.machine`, `video.coin`, `video.coins`, `video.played` and
  `video.royals`.
- **No new art:** cards, chips (as coins), Lucky's poses, feathers and ducklings exist.
- **No new sounds.**
- **No ABI changes.**
- **Later:** double-up (§8d), and Deuces Wild, which needs wild cards in the evaluator.

## 22. As built: step 4, Video Poker (2026-10-15)

- **`hand.{c,h}`** as §21. Host-tested in `tests/host/luckyduck_video.cpp`:
  - All 2,598,960 five-card hands counted by category.
  - 20,000 random hands of five to seven cards against brute force over their fives.
  - Ties, kickers, the wheel, and seven cards holding two threes or three pairs.
- **`video_rules.{c,h}`** as §21, with the three machines.
  - Host-tested: the deal, holds, the draw and what is refused; each pay table's rows (the
    kickers of Double Double Bonus); the cards that count.
  - **Perfect play** is tested against known answers: a pair of Jacks returns about 1.54 a coin;
    a pat flush is broken for four to the royal; a low pair is given up for four to a flush; a pat
    straight flush stands; nothing draws five. Its fast pays are checked against the pay tables
    hold by hold.
  - **Perfect play is fast:**
    - Each hold's cards and the draw's cards but the last are kept as rank masks, so each of the
      2,598,960 hands adds one card.
    - The pays are looked up by what the masks show: a pair's rank, a four's and its kicker's.
    - One-bit masks become ranks by a de Bruijn multiply, not by counting bits (the chip may have
      no instruction for it).
    - On the board it takes about 70 frames (1.2 s) at 8 ms a frame while Lucky says "HMM...".
      The first version took several seconds.
- **The scene (`video.c`)** as §21.
  - The cabinet sits on the lobby's carpet: a gold frame, a black bezel, a coin slot, and a blue
    screen shading darker downwards.
  - The pay table is painted in the gold small letters. The column bet shows red; the hand made
    shows gold, and its row flashes once it wins.
  - Cards fly in from Lucky's deck with a turn, slowing to land flat, and flip left to right. At the
    draw, the cards not held turn down, fly back, and new ones come.
  - "HELD" sits under a lifted card. A win lifts the cards that make it, with a gold rim pulsing
    behind them.
  - The headline rises under the cards: "ROYAL FLUSH!" in the big letters, the rest in the middle
    size.
  - The win counts up in about 80 frames, coins flying to the bank. The bet's coins fly to the coin
    slot at the deal.
  - The hint bubble is two lines ("PSST..." over "HOLD 10 K 10 K"), so it fits left of the cards.
  - Options: the machine, and coins of $1, $5 or $25.
- **Checked on the board:**
  - Deals, holds by key, the draw, the pay.
  - The hint, its time, and its answer (it held two pair).
  - The options' machine and coin, Double Double Bonus's 13 rows, and the leave box mid-hand.
  - The lobby's table.
  - A royal flush's party, with a temporary build drawing only royals (not kept).
  - Found and fixed there:
    - The glow was a grey box over the blue (now a solid gold rim).
    - "HELD" was too big; the hint's bubble covered the first card.
    - The royal flush's headline ran off the screen; the cards moved up into the gap under the
      table.
- **Seen on the board:** after many test runs and screenshots in `/tmp`, PSRAM's largest free block
  fell to 2.4 MB. The screenshot's 2.8 MB copy failed then, until the test copy closed.
- **Open:** Double Double Bonus's 13 rows are tight at 21 pixels; double-up; Deuces Wild.

## 23. Smoother animation (2026-10-16, the user's: "the video poker animations aren't that smooth")

- **Measured:** ticks run at a fixed 60 a second and a frame follows them, so a frame past 16.7 ms
  makes the next frame run two ticks (the motion jumps).
  - During a deal or a draw, frames took up to 30 ms to draw, and 11 to 18 of each second's 60
    frames skipped a tick. Between hands, under 1 ms.
  - The board's profiler (`tools/bmds-prof`) put the busy time in plain copies (felt and cards,
    42%), the scaled blit (13%: every card at rest went through it at 100%), the turned blit
    (11%), and the double buffer's catch-up copy (22% of that `memcpy`).
  - The slow frames' areas were small (60,000 to 130,000 pixels), so a redrawn pixel cost about
    230 ns. The felt alone cost about 180 ns a pixel, read and written in PSRAM while the scan and
    the window manager use it too (`docs/platforms/esp32p4.md`).
- **Changed (each measured):**
  - **The stage:** a card or sprite at its own size is drawn by the plain blit (rows copied),
    not the scaled one.
  - **Felt under cards:** each redraw restores the felt only where no card's opaque middle will
    cover it (`restore_uncovered()`: card faces, backs and the Joker; the place tiles are hollow).
    That roughly halved the felt.
  - **The catch-up copy:** Lucky Duck opts into `BMDS_GAME_REPAINTS` (`games.md` §14a). The stage
    declares each rectangle it repaints, and the copy happens after drawing, skipping those. A box
    (it dims the picture), the ducklings' transition and Solitaire (its win's trails are never
    erased) call `bmds_game_up_to_date()` first.
  - **Video Poker:**
    - Cards slide from the deck growing (full size halfway) instead of spinning: a turned card's
      bounds are a square up to three times its area.
    - The draw's departures go in turn, as the arrivals do.
    - The winning cards' rim and the royal flush's flash are four thin strips each: a full
      rectangle redrew the whole card, and the screen-wide flash 500,000 pixels.
  - **`libbmdsgame`'s turned and scaled blits** work out each row's span once, step in 32 bits,
    and have an RGB565 path. Host-tested to draw exactly what the old ones did.
- **Result:** the worst frame went from 30 to about 20 ms. The busiest seconds skip 1 to 8 ticks
  (about 5) instead of 11 to 18. Every scene was checked on the board after the change: Video
  Poker, its leave box, the ducklings' transition, the lobby, Blackjack, the dice and Solitaire.
- **Tried, no gain:**
  - Filling the screen's plain gradient rows instead of copying them: the cache reads a line in
    before writing it, so a fill costs about what a copy does.
  - Sending cards 7 ticks apart instead of 5.
- **What is left:** what a frame touches in PSRAM sets its cost. The next step is the 2D-DMA
  doing a program's bulk copies (the felt restore and the catch-up copy). That would be a new
  surface call, an ABI addition, so it waits for the user.
