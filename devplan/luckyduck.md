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
