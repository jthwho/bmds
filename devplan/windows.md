# Windows: a Desktop, a Window Manager, the Terminal as the First Program

Status: **steps 1 and 2 done** (surface 0 runs; `display.md` §5c) (Phase 4, 2026-10-05). After measuring three ways
to put windows on the screen (§4b, §4c, below), the user chose the simple one: **surface 0**,
a double-buffered RGB888 framebuffer in PSRAM that the DW-GDMA scans directly (no ring), windows
moved as an outline until placed. The PSRAM bandwidth is constant (166 MB/s of scan at 720p60) and
accepted. Separate surfaces that the DMA reads as rectangles of their own (§4c) may come later.

## 0. Decisions (2026-10-05)

- **Surface 0** (the display's first surface): RGB888 in PSRAM (the scan has no 565 conversion: the DW-GDMA converts
  nothing, nor does this chip revision's DSI bridge), scanned by the DW-GDMA straight into the DSI
  bridge (no ring). Soaked 35 minutes under load with 0 underruns (§4b). It frees the ring's 276 KB
  of internal RAM and can't show stale lines. The PSRAM bandwidth is constant (166 MB/s of scan at
  720p60) and accepted.
- **The drawing API is the same for every surface and every buffering:** drawing goes to the right
  buffer, a double-buffered surface swaps at the vertical (a frame's start) after `present()`, and
  keeping the other buffer up to date is the surface's business (the last frame's changes copied
  across by 2D-DMA, queued ahead of the next drawing). Nothing waits, except for now drawing on a
  double-buffered surface while its swap is pending (at most a frame); a **mailbox** mode (a third
  buffer, opt-in per surface: a newer `present()` replaces a pending one) removes that later. An
  application that wants to sync uses separate calls: `present()` returns the frame's number,
  `waitShown(frame, timeoutMs)` waits until it is on the screen, `frames()` counts them.
- **Efficiency is critical: every trick in the book.** The P4's SIMD (the PIE vector extension:
  128-bit registers) and hand-written assembly for the inner loops (glyph expansion, fills, colour
  conversion, blending), measured against plain C++; DMA for every PSRAM write; hot paths in IRAM
  where a measurement says so; nothing done twice that can be done once.
- **Surface 0 is nothing special:** just the first surface; surfaces are numbered per display (no
  singletons), and display 0's first is surface 0.
- **Every surface single-buffered by default, double-buffered on request** (surface 0 too): what
  suits the application. A single buffer is drawn while it is shown (a draw can tear: the scan may
  pass it half done); a double one is drawn behind and swapped at a frame's start by `present()`
  (2.76 MB more for surface 0 at 720p, and what changed copied forward after each swap).
- **Window contents, from simple to fast, the application's choice** (RAM is scarce, so the
  default costs none):
  1. **Drawn directly on surface 0 (the default, now):** a window has no picture of its own; it
     draws, clipped to its visible part, straight into surface 0, and redraws what the window manager
     reports uncovered (as on the Mac and GEM).
  2. **A backing surface, copied by the window manager (later):** the window draws into its own
     surface and the window manager copies the visible parts into surface 0 (chained 2D-DMA copies,
     §4b; a 565 surface widened on the way): nothing to redraw when uncovered.
  3. **A secondary surface the DMA reads (later):** the surface is a rectangle of its own in the
     scan's list of runs (the gather design, §4c): no copy at all; e.g. a cursor, a video window.

  One API: a window asks for one of these when it is made; the drawing calls are the same.
- **Moves:** an outline while dragging; one redraw (or copy) when the window is placed.
- **Superseded:** the RGB565 framebuffer widened into a ring (`display.md` §5b) and the scan-time
  compositor prototype (§4b), once surface 0 runs.

## 1. Goal

- **A GUI from day one, bare bones:** a flat desktop, rectangular windows with a title bar and a
  close box, stacked front to back, one with the keyboard focus. No menus, icons, resizing by
  handle or transparency in this plan.
- **The terminal is a program,** not a mode of the display: a window showing a `TerminalScreen`
  (`terminal.md` §3a) with a shell on it.
- **Any resolution:** nothing is sized for one mode; sizes come from the mode the display runs.
  The test target is **1280×720**, the heaviest mode on PSRAM bandwidth run so far.
- **Later, not now:** an ABI for GUI programs (§6). Then GUI applications become separate programs;
  some may stay built in (the terminal perhaps).
- The **serial console stays the system console** (boot log, recovery): a fault in the GUI never
  costs the shell.

## 2. Facts that shape the design

- **The display path** (`display.md` §5b): an RGB565 double buffer in PSRAM, `present()` swaps at a
  frame's start, the back buffer then holds the picture of two presents ago. Drawing reaches the
  back buffer by 2D-DMA rectangle copies (`Framebuffer::blit()`, `RectCopier`).
- **The CPU doesn't write PSRAM pictures** (`display.md` V9): its cache write-backs can't be ranked
  below the scan's 2D-DMA reads. It draws in internal RAM and the 2D-DMA copies.
- **PSRAM bandwidth is the budget.** At 720p60 the scan reads 110 MB/s of the framebuffer. Every
  composed pixel costs a read of a window's picture and a write into the back buffer; every pixel a
  program draws costs a write into its picture. A full-screen change is 3.7 MB of copying.
- **A 2D-DMA job has a fixed cost** (about 13,400 cycles through IDF's job queue, `display.md` §8):
  few large copies beat many small ones.
- **No input until Phase 5** (USB host): no mouse, no keyboard on the board yet.

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| W1 | Window contents | **Drawn directly on surface 0 by default** (§0): no picture per window; uncovered parts are redrawn by the window on request. Backing surfaces (copied) and secondary surfaces (read by the DMA) are later options a window asks for | a backing store for every window (Amiga smart refresh): memory per window whether it needs it or not |
| W2 | Clipping | **Visible regions:** each window's visible part (its rectangle minus the windows above, `Region`) clips its drawing; a change of the stack reports the newly uncovered rectangles to the windows under them as damage to redraw | drawing everything bottom to top (overdraw, flicker) |
| W3 | Buffers | **Single by default, double on request**, per surface (§0); a double surface 0 is swapped at a frame's start, and what changed is copied into the other buffer afterwards (or redrawn) | double always (2.76 MB more at 720p) |
| W4 | Redraws | **Asked for by damage, merged per frame:** the window manager gathers the uncovered rectangles per window and asks each window once to redraw them | a redraw per change |
| W5 | Frames (chrome) | **Drawn by the window manager on surface 0**, clipped like any window: a 1-pixel border, a title bar one font row high with the title and a close box; the focused window's title bar highlighted | — |
| W6 | Desktop | **A flat color, `CONFIG_BMDS_DESKTOP_COLOR`** (RGB, default a dark grey), changeable at run time (`win desktop RRGGBB`), filled by the window manager where no window is (no surface) | a desktop surface (2.76 MB; a pattern or picture later can be one) |
| W7 | Font | **unscii-16** (Viznut, public domain): 8×16, the look wanted. Converted at build time from its `.hex` file to a rodata glyph table (16 bytes a glyph) with a sorted codepoint index; a missing character draws a replacement glyph. Which ranges to keep is decided by size (ASCII, Latin-1, box drawing and block elements at least) | Spleen (BSD-2), Terminus (OFL) |
| W8 | Drawing | **A `Painter` over a surface (surface 0, clipped to a window's visible region), drawing through a strip in internal RAM:** fill, text runs (glyphs with two colors), copies. Each operation draws into the strip and copies it into the surface by 2D-DMA (RGB888 rows into PSRAM: x and width in multiples of 4 pixels, or the edges drawn by the CPU) | the CPU writing PSRAM directly (V9) |
| W9 | Where | **A new `bmds_ui` component:** the pure parts (rectangle regions, the compose plan, the font lookup, the window stack) host-tested; the surfaces, the window manager task and the drawing on the target, over `Framebuffer`. The terminal program beside it | in `bmds_drivers` (the display drivers stay drivers) |
| W10 | API | **Internal only** (C++ in the kernel) until the terminal and one or two more clients have proven its shape; then a C ABI under `sdk/` (§6) | an ABI now (frozen before it has users) |
| W11 | Control without input | **A `win` command** on the serial shell: list, new terminal, move, raise, close, focus, type text into a window, desktop color. It stays as the scripting and test tool | waiting for Phase 5 |

## 4. Design

### Surfaces and windows

- **`Surface`:** a picture in PSRAM: width, height, format (RGB888 for surface 0), one or two buffers;
  `present()` swaps a double one at the next frame's start. Surface 0 is the display's first.
- **`Window`:** a number (no singletons: `win 1`, `win 2`), a screen rectangle, a title, flags
  (frame or not, closable), how its contents are kept (§0: drawn on surface 0 for now), and a redraw
  callback the window manager calls with the damage to redraw.
- **The stack:** windows in z-order over the desktop (a flat color, drawn by the window manager);
  focus is the top window unless set otherwise. Each window's visible region is kept up to date as
  the stack changes.

### Changes of the stack

- **Opening or raising:** the window's newly visible parts are damage to it; it draws them.
- **Closing or moving away:** the parts it uncovers are damage to the windows (and the desktop)
  under it; each redraws its part.
- **Moving:** an outline drawn while dragging (XOR-free: the outline's rectangles drawn and the
  pixels under them redrawn from their owners when it moves); at the drop, the window is redrawn at
  its new place and the uncovered parts are damage, as above.
- **Pace:** damage is gathered and each window asked once a frame (W4); with a double-buffered surface 0
  the window manager presents after the redraws.

### Drawing

- The `Painter` draws a band of the target rectangle at a time into its strip (internal RAM: one text
  row at the full screen width is 1280×16×3 = 60 KB at 720p), copies it into the surface by 2D-DMA,
  and moves on; it clips to the window's visible region (each visible rectangle its own band).
- **Text:** a run of cells, each a codepoint and two colors, drawn from the glyph table. Bold,
  underline and reverse are drawn by the terminal's renderer from the cell's attributes.

### The terminal program

- A window whose client area is `columns × rows` cells of 8×16; `TerminalScreen` holds the cells,
  so a redraw of any damage is drawn from them (the cells are the terminal's own backing store, 12
  bytes a cell instead of 384 bytes of pixels).
- **A `Console` on it:** writes go to the screen; each frame (not each write) the program draws the
  dirty rows on surface 0, and the cursor (an inverted cell) where it moved. A burst of output is
  thus drawn once per frame however many lines it scrolled.
- **A shell runs on it.** Until Phase 5 its input comes from `win type N TEXT` on the serial shell;
  then from the keyboard when it has the focus.

## 4b. Composing at scan time (the user's proposal, 2026-10-05; prototype)

Windows as sprites: no framebuffer. Per block, the scanout's refill puts the block's visible pieces
of each window straight into the ring: a window is shown where its position says, and the
position (and which of a window's buffers is shown) is taken once a frame, at the frame's start.
Dragging copies nothing.

- **Per-window formats:** RGB565 (the 2D-DMA's converter widens it), RGB888 (copied as is), and
  **8-bit with a palette** (planned: the PPA blender's CLUT, which `PaletteExpander` drives by hand
  since IDF's blend leaves L4/L8 out; a palette per window).
- **The PPA:** a fill per block for a flat desktop (no surface, no PSRAM reads, no desktop pieces
  around the windows); blends with a color key or alpha for sprites that aren't rectangles (the
  mouse cursor); scaling for low-resolution program modes. Its measured rate (§8 of `display.md`:
  about 52 Mpixel/s, 18,000 cycles a job) is below 720p60's 55 Mpixel/s, so it does parts of the
  screen, not all of it.
- **Bandwidth:** each visible pixel read once a frame (as the framebuffer's scan now), nothing
  written to PSRAM; no compose copies and no 3.7 MB double buffer. A program drawing into a shown
  surface can tear unless the window has two buffers (swapped at a frame's start).
- **The risk:** every piece is a job inside the block's deadline (about 195,000 cycles a 24-line
  block at 720p60, of which the one widen job takes about 107,000 now, and IDF's queue costs about
  13,400 cycles a job). A missed block shows stale lines (garbage with windows), where a framebuffer
  compose falling behind only lowers the frame rate.
- **Chained pieces:** a 2D-DMA job is a chain of descriptors, each with its own picture, block and
  offsets. If one job carries all of a block's pieces of one format (a chain of transmit and of
  receive descriptors), the fixed cost is paid once a block, not once a piece.

**The prototype** (`win proto`, the scanout taking a `BlockSource` that composes each block):
1. N test windows (565 and 888 surfaces) at fixed or moving positions, latched per frame.
2. The pieces of each block by `planCompose()` over the windows clipped to the block's band.
3. Copies one job a piece, or chained (a job per format a block).
4. The desktop as a surface (its pieces copied) or a PPA fill per block (windows copied over it).
5. A PPA color-key blend of a sprite on top.

Measured at 720p60, idle and under `memtest`: cycles per block and per piece, where blocks start
being skipped. Decides: full scan-time composition, or a framebuffer base layer with a few sprites
(the cursor, the window being dragged) on top.

**Measured** (2026-10-05, 720p60, 200 MHz PSRAM, 3 blocks of 24 lines: 533 µs a block; test
windows 400×300, cascaded, RGB565 and RGB888 alternating; averages over 5 s, µs a block, average/most):

| Layers | Desktop | Jobs | Pieces a block | Block | Skipped |
|---|---|---|---|---|---|
| 0 | surface | — | 1 | 279/287 | 0 |
| 0 | PPA fill | — | 0 | 219/226 | 0 |
| 4 | surface | chained | 3.8 | 336/422 | 0 |
| 4 | surface | one a piece | 3.8 | 415/621 | 0 |
| 8 | surface | chained | 6.3 (13 at most) | 366/410 | 0 |
| 8 | PPA fill | chained | 3.9 | 450/538 | 0 |
| 8 | PPA fill | one a piece | 3.8 | 522/776 | one block a frame |
| 8 RGB565 | surface | chained | 6.3 | 309/339 | 0 |
| 8 RGB888 | surface | chained | 6.3 | 392/436 | 0 |

- **Chaining works** (one 2D-DMA job carries many descriptors, each its own picture and block):
  a job a piece costs about 36 µs (13,000 cycles) more each, as IDF's queue measured before.
  `check` compares a whole block with what it should show: 0 wrong, also while windows move.
- **The PPA fill is not cheap:** 214 µs for a block (1280×24), more than half of copying it from a
  surface (273 µs); it saves PSRAM reads, not time.
- **The PPA sprite blend** (32×32, color key) costs about 90 µs a block it touches: nearly all the
  PPA's fixed cost (about 32,000 cycles a job).
- **Dragging is free:** 8 windows, 4 of them moving every frame, and the sprite: 378/500 µs, 0
  skipped.
- **Under load** (8 windows, 4 moving, the sprite): a `memtest` pass 0.2% of blocks skipped (0.7%
  with the PPA fill); a core writing PSRAM flat out (`display stress write`) 45%, its copies 27%.
  The per-block CPU work in flash slows several times through the shared L2 cache: with the
  compositor, `Region` and `BlitChain` in IRAM (linker fragments) and IDF's 2D-DMA enqueue in IRAM
  (`CONFIG_DMA2D_OPERATION_FUNC_IN_IRAM`), still 31% and 12%: the 2D-DMA's PSRAM reads of many short
  rows slow down too. (`CONFIG_DMA2D_ISR_IRAM_SAFE` can't be used: the PPA's callbacks are in flash,
  and IDF then refuses its jobs.) A skipped block shows lines of three blocks earlier: garbage, with
  windows.

**The 2D-DMA can't feed the DSI bridge** (the user's question): its transmit side serves JPEG and the
PPA's three inputs ("display-1..3" in the register descriptions, the PPA in IDF), its receive side
JPEG and the PPA's two outputs. The bridge takes pixels into a FIFO paced by a hardware handshake
that only the DW-GDMA has. **But the DW-GDMA can scan the frame straight from PSRAM** (no ring), in
RGB888 (this chip revision's bridge converts no formats): `display test 720 fb`, which fell behind
at 80 MHz. At 200 MHz: 0 underruns idle, under `display stress write` and `psram` (3 × 2 s each)
and a `memtest` pass (48 s in all); the CPU's PSRAM writes reach 94 MB/s beside it (132 beside the
ring's scan). It frees the ring's 276 KB of internal RAM and can't show stale lines; it reads 166
MB/s of PSRAM (against 110), and an underrun, if one comes, glitches the link. **The soak** (35
minutes, a shell loop of `memtest` passes and `display stress write`/`psram`/`write`): 126,675
frames, **0 underruns, 0 stalls, 0 resyncs**; 134 `memtest` passes, 0 errors. (The loop never
yields core 0: a task-watchdog warning each round, harmless.)

## 4c. Gathering at scan time (the user's proposal, 2026-10-05; prototype)

No framebuffer and no ring: the DW-GDMA (the TRM's VDMA, the only DMA wired to the DSI bridge)
walks, each frame, a list of runs in scan order, each a contiguous piece of a window's surface or of
the desktop, straight into the bridge's FIFO. Moving a window is a different list.

- **Lists:** two, alternating. The first descriptor of each interrupts (a frame starts: the refill
  task builds the other list from the positions now); one about 85% of the way through interrupts
  too, and links the list's last descriptor to the other list if it is ready, else back to its own
  start (the same picture again: a replay). The DMA clears each descriptor's valid bit as it
  finishes (no way to turn the write-back off), so a replay re-validates the list: the part done at
  that interrupt, the rest at the next frame's start.
- **Runs:** RGB888 only (the DW-GDMA converts nothing, nor does this revision's bridge); every run
  starts on 8 bytes and is a multiple of 8 long (the DMA's 64-bit transfers): window x and widths
  in multiples of 8 pixels. A window costs a run a line it crosses, and so does the desktop gap
  after it; runs contiguous in memory merge. The flat desktop is a constant buffer of 8 lines in
  internal RAM, so a desktop gap at a line's end and the next line's start are one run, and rows
  without windows are a run per 8 lines: the desktop reads no PSRAM.
- **The cursor:** a patch of 40×32 (x on 8 pixels, the cursor at any x inside it) per list, composed
  by the CPU from what is under it with the cursor's alpha; the cursor's rows point there.
- **Descriptors:** 64 bytes each, in internal RAM (IDF's lists): 2,560 a list by default, 320 KB for
  the two; the ring's 276 KB isn't needed.
- **The risk:** the DMA's cost per run (fetching the descriptor, writing it back, starting the read)
  against 240 MB/s into the bridge at 720p60, whose FIFO holds about a line and a half; an underrun
  glitches the link.

Designed, not built: the user chose surface 0 alone first (§0); this is the plan for later
surfaces.

## 5. Budgets (estimates; measured as the steps land)

At 720p:

| What | PSRAM | Internal RAM |
|---|---|---|
| Surface 0, single-buffered (double: twice) | 2.76 MB (5.53 MB) | |
| Windows drawn on surface 0 (the default) | none | |
| A terminal of 100×40 cells: `TerminalScreen`'s cells | 47 KB | |
| The painter's strip (a text row at the screen's width, RGB888) | | 60 KB |
| Glyph table (unscii-16, measured) | 44,368 bytes of bitmaps and 247 runs (about 2 KB), rodata in flash | |

- **The scan:** 166 MB/s of PSRAM reads at 720p60, constant (§0).
- **A typed character** redraws one text row of the window: about 38 KB written for the 100-column
  terminal (800×16×3).
- **The hard case is a scrolling terminal:** every row changes each frame: about 1.5 MB drawn a
  frame for the 100×40 terminal, 92 MB/s at 60 frames a second, beside the scan. The terminal draws
  at most once a frame and may drop to every other one under load; scrolling the window's part of
  surface 0 by a 2D-DMA copy (instead of redrawing its rows) is the next lever, measured in step 5.

## 6. ABI (later)

The goal is GUI programs as separate executables. Not before the internal API has settled. Expected
shape: a window handle per program, the surface mapped into the program (it draws with the CPU into
internal RAM and asks for copies, or calls drawing system calls), damage reported by a call, events
(keys, mouse, close, focus) read from a handle that works with `poll`. Every boundary struct starts
with `uint32_t size` (`docs/ABI.md`). Built-in programs (the terminal) use the same API as external
ones once it exists.

## 7. Open questions

- ~~The first surface's name~~: surface 0 (the user, 2026-10-05).
- ~~Double buffering without waits~~: drawing waits while a swap is pending for now; the mailbox
  (an opt-in third buffer) is planned (§0).

- The PPA's fill (a solid rectangle into PSRAM without a source picture): would make the desktop a
  color instead of a surface, saving its 1.84 MB and its reads.
- Driving the 2D-DMA directly (`display.md` §7): the per-job cost matters more here, with a copy per
  visible piece.
- Window moves without a mouse are `win move`; with one, an outline drag (the frame drawn while
  dragging, one compose at release) keeps a drag from costing a full compose per frame.
- Bold glyphs: if unscii has no bold face, drawn by smearing one pixel right, or a second font.
- ~~Which unscii ranges to keep~~: all but the controls and the private use area (§9).

## 8. Steps

1. **Pure foundations** (host-tested; done): rectangle regions and the compose plan; the font
   converter (a host tool over unscii's `.hex`, vendored in `thirdparty/unscii` with its notice) and
   the glyph lookup.
2. **Surface 0** (`display.md` §5c; done): RGB888 in PSRAM scanned directly by the DW-GDMA,
   single or double buffered (`present()` at a frame's start), the default display path; the ring
   paths and their bring-up commands removed.
3. **The window manager on surface 0:** windows, the stack, visible regions, damage and redraw
   requests, the desktop color, frames with titles, outline moves; test windows that redraw a
   pattern; `win list/new/move/raise/close/desktop`.
4. **Drawing:** the `Painter` (fills, text from the glyph table, copies), clipped to visible regions.
5. **The terminal program:** `TerminalScreen` drawn per frame, a `Console` on it, a shell in it,
   `win type`.
6. **At boot:** the desktop and a terminal start with the display.
7. **Later:** backing surfaces (copied by the window manager) and secondary surfaces (read by the
   DMA, §4c), chosen per window.

## 9. As built

- **Step 1** (2026-10-05):
  - `Rect` moved to `bmds_core` (`bmds/rect.h`) with intersection, union, containment and
    `alignedX()`; `Framebuffer` uses it.
  - `bmds_ui` (a new component; `main` requires it):
    - `Region`: up to 32 disjoint rectangles; neighbors that make a rectangle merge, and past 32 it
      collapses to the bounds.
    - `subtract()` and `planCompose()`: layers top first, damage in, disjoint pieces out, `Full`
      past the caller's space or 128 rectangles of working space (2 KB of stack).
    - `Font`: runs of consecutive codepoints binary-searched, narrow and wide glyphs in separate
      tables, a replacement glyph (□, U+25A1: unscii has no U+FFFD).
  - The glyph table is generated at build time by `tools/fontconv.py` from
    `thirdparty/unscii/unscii-16.hex` (Unscii 2.0; the public-domain variant, not `-full`, which is
    GPL). 2,530 glyphs, 243 of them two cells wide (fullwidth forms, emoji, the Yijing hexagrams).
  - Host-tested (`tests/host/ui.cpp`): `subtract()` and `Region` against pixel maps over random
    rectangles, merging and collapsing, `planCompose()` against a topmost-layer reference (each
    damaged pixel exactly once, even inputs giving even pieces), `Full`, and glyphs checked against
    the `.hex` file's bits.
