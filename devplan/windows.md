# Windows: a Desktop, a Window Manager, the Terminal as the First Program

Status: **steps 1 to 5 done** (surface 0, `display.md` §5c; the window manager on it; the Painter;
the terminal program, §9)
(Phase 4, 2026-10-05). After measuring three ways
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
- **Built-in applications are ordinary applications** (the user, 2026-10-05): the terminal, and
  every application built into the OS, is written against the GUI API exactly as a separately
  built program would be. Building it in is packaging (always there, smaller, faster), never a
  reason for access others don't have; pulling it out into its own executable must need no
  change to the OS. So no window manager or OS code knows a particular application: no
  application's settings in the window manager's palette, no application in `win`, no hooks for
  one in `main`. Test aids go through generic interfaces. Step 9 (§8) removes what breaks this
  today.

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
| W6 | Desktop | **A flat color, `CONFIG_BMDS_DESKTOP_COLOR`** (RGB, default a dark grey), changeable at run time (`win color desktop RRGGBB`, W12), filled by the window manager where no window is (no surface) | a desktop surface (2.76 MB; a pattern or picture later can be one) |
| W7 | Font | **unscii-16** (Viznut, public domain): 8×16, the look wanted. Converted at build time from its `.hex` file to a rodata glyph table (16 bytes a glyph) with a sorted codepoint index; a missing character draws a replacement glyph. Which ranges to keep is decided by size (ASCII, Latin-1, box drawing and block elements at least) | Spleen (BSD-2), Terminus (OFL) |
| W8 | Drawing | **A `Painter` over a surface (surface 0, clipped to a window's visible region), drawing through a strip in internal RAM:** fill, text runs (glyphs with two colors), copies. Each operation draws into the strip and copies it into the surface by 2D-DMA (RGB888 rows into PSRAM: x and width in multiples of 4 pixels, or the edges drawn by the CPU) | the CPU writing PSRAM directly (V9) |
| W9 | Where | **A new `bmds_ui` component:** the pure parts (rectangle regions, the compose plan, the font lookup, the window stack) host-tested; the surfaces, the window manager task and the drawing on the target, over `Framebuffer`. The terminal program beside it | in `bmds_drivers` (the display drivers stay drivers) |
| W10 | API | **Internal only** (C++ in the kernel) until the terminal and one or two more clients have proven its shape; then a C ABI under `sdk/` (§6) | an ABI now (frozen before it has users) |
| W11 | Control without input | **A `win` command** on the serial shell: list, new terminal, move, raise, close, focus, type text into a window, desktop color. It stays as the scripting and test tool | waiting for Phase 5 |
| W12 | Colors | **A palette by role** (`WindowPalette`): desktop, active and inactive frame, active and inactive title text, close box and its mark, an undrawn client area, the move outline. Each role has a name (`win color ROLE RRGGBB`; a settings file later); a change redraws the desktop alone or, for any other role, the screen. The desktop's default is `CONFIG_BMDS_DESKTOP_COLOR` | colors fixed in the code |

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

## 4a. Step 3 design: the window manager on surface 0 (2026-10-05; built)

- **Pure and host-tested (`bmds_ui`):** `WindowStack` holds up to 16 windows (255 since 2026-10-07, `filebrowser.md` FB17; numbered from 1,
  the lowest free number reused), their rectangles in z-order, and one screen `Region` of damage.
  Its changes add exactly what needs redrawing: opening adds the window's rectangle; closing and
  moving add what it covered (and, moving, its new place); raising adds only the parts that were
  covered (a compose of the old stack over its rectangle). A window's own invalidation is clipped
  to its visible part before it is added, so the windows above aren't asked to redraw for it.
  `WindowFrame` gives the frame's geometry.
- **A round** (the window manager's task, at most once a frame): `planCompose()` over the stack
  (top first, the desktop last) and the damage gives disjoint pieces, each owned by the topmost
  layer; the pieces are grouped by layer; the desktop's are filled in one call; each window draws
  its pieces once: the frame by the window manager, then its client area by its redraw callback
  through a `Painter` clipped to the pieces inside it. Past the plan's space, each damage rectangle
  is planned alone, and past that the rectangle is painted bottom to top (overdraw, never a hole).
  With a double-buffered surface 0 the round ends with `present()`; the next round waits for the
  next frame either way, so a burst of changes is drawn once.
- **Alignment:** RGB888 rows into PSRAM start and end on 4 bytes, so window rectangles are kept on
  4-pixel columns (x and width; the frame's sides are 4 pixels), which keeps every piece of every
  plan aligned. A `Painter` fill snaps its x edges outward to the 4-pixel grid.
- **Frames:** the whole frame in one color (by default the focused window's blue, the others grey; W12): a title
  bar 20 pixels high (a font row and 2 pixels above and below), 4-pixel sides and bottom, a 12×12
  close box at the left of the title bar, the title in white after it. Focus is the top window.
  Windows are kept wholly on the screen.
- **Outline moves:** `drag(id, x, y)` shows a 4-pixel outline of the window at the new place, drawn
  after each round's redraws; moving it damages the pixels it covered (their owners redraw them);
  `drop()` moves the window there. Without a mouse, `win drag` walks the outline over 30 frames.
- **Drawing (until the step 4 `Painter`):** fills of many rectangles of one color as one 2D-DMA job
  (`Surface::fill(Span<const Rect>)`: the first fixed-cost lever); text through a strip in internal
  DMA RAM (the screen's width × 16 rows: 60 KB at 720p), each glyph row expanded by two 12-byte
  lookups (4 pixels each) in a table built per pair of colors, then copied by 2D-DMA per piece. SIMD
  glyph expansion, unaligned edges and copies are step 4.
- **Threads:** the window manager's state is behind a mutex; the API (the `win` command now, the
  terminal later) changes the stack and wakes its task, which draws the round with the mutex held
  (an API call waits at most a round). Redraw callbacks run on the window manager's task.
- **Testing:** the test windows draw a checker of 32-pixel squares in their own color; `win check`
  reads back the shown picture and compares every desktop and client-area pixel with what the stack
  says should be there.

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

## 4d. Step 4 design: the Painter (2026-10-05; built)

What step 5's terminal needs, made fast: rows of cells (each its own colors), scrolling, and the
odd 1-pixel line.

- **Cells:** `Painter::cells(x, y, cells, font)` draws a run of `TextCell`s (a codepoint, fg, bg,
  underline and bold flags; bold is the glyph smeared one pixel right); `text()` is a run of one
  pair of colors. Glyphs are expanded into the strip by a kernel, then copied by 2D-DMA piece by
  piece (the clip).
- **Kernels, measured against each other** (`win bench`, which also checks they agree byte for
  byte):
  - *Scalar* (pure, host-tested against the font's bits): a 16-entry table of 4 pixels (12 bytes,
    three words) per nibble, rebuilt only when a cell's colors differ from the last cell's; a glyph
    row is two lookups and six word stores.
  - *PIE*: the glyph row's byte broadcast into a 128-bit register (`esp.vldbc.8.ip`), masked with
    the bit of each pixel's 3 bytes and compared (`esp.andq`, `esp.vcmp.eq.u8`) into a 24-byte
    mask, blended `bg ^ (mask & (fg ^ bg))`, stored as three 64-bit halves; no table, colors in
    registers, a hardware loop (`esp.lp.setup`) over the rows.
  - The font lookup gets a fast path for its first run (ASCII), and a cell repeating the last
    codepoint reuses its glyph.
- **Overlap:** the strip is used in two halves: while the 2D-DMA copies one half's glyphs into the
  surface, the CPU expands the next cells into the other. `Surface` gains a started copy that
  doesn't wait (`startBlit()`; the next drawing call, or `wait()`, waits for it).
- **Scrolling:** `Painter::scroll(area, dy)` moves what of `area` is visible by `dy` rows with
  2D-DMA copies inside the surface (`Surface::copy()`, in an order that never reads a row already
  overwritten: top first moving up; bottom first moving down, in bands no taller than `dy`). Only
  pixels whose source is visible and not damaged are copied; the rest is returned as a region the
  window redraws.
- **Unaligned edges:** a fill whose sides aren't on 4-pixel columns writes its whole groups of 4
  in the middle as now, and the edge groups by read-modify-write: the 2D-DMA reads them into
  internal RAM, the CPU sets the pixels inside the fill, the 2D-DMA writes them back (two jobs a
  fill call, for all its rectangles' edges together). Text still starts on a 4-pixel column.
- **Window updates:** a window may draw outside of damage (a terminal's new rows, a scroll):
  `WindowManager::update(id)` asks for a call at the next round, with the painter's clip all of its
  visible client area. The redraw callback always gets the damage to redraw (`Painter::damage()`),
  and may draw anything else in the clip.
- **Tests:** the test windows add a label of cells in two colors, 1-pixel grid lines at unaligned
  columns, and `win scroll N DY` (the pattern moves; the exposed rows are redrawn); `win check`
  models all of it.

## 4e. Step 5 design: the terminal program (2026-10-05; built)

- **`ScreenConsole`** (`bmds_console`, pure, host-tested): a `Console` whose output is applied to
  a `TerminalScreen` and whose input is a ring the owner fills (`pushInput()`: `win type` now, the
  keyboard in Phase 5). Writes translate `\n` to CR LF as the mode says; a Ctrl-C pushed raises
  the console's interrupt when signals are on; the screen's replies (DSR, DA, the size report) go
  into the input. A mutex serializes the writers and the renderer (`Locked` access to the screen).
  After a write that changed the screen it calls the owner's `changed` hook (at most once until
  the owner says it drew: a burst of output asks for one redraw).
- **`TerminalView`** (`bmds_ui`, pure, host-tested): a row of `TerminalCell`s into the painter's
  `TextCell`s: default colors from the palette (new roles, appended: `terminal-text`,
  `terminal-background`, `terminal-cursor`), indexed and RGB colors through `Color::toRgb()`,
  bold, underline, reverse, dim (half the foreground), invisible, the tail of a wide character
  dropped (its glyph is two cells wide), the cursor as a cell in the cursor color.
- **`Terminal`** (`bmds_ui`, target): a numbered session (`MaxTerminals` = 4): a `ScreenConsole`,
  its screen in PSRAM, and a shell spawned on it the first time it opens. A shell's task never
  ends, so **a session outlives its window**: closing the window detaches it (output still goes to
  the screen), and `win term N` opens a window on it again. The window's client area sets the
  size (columns × rows of 8×16 cells; `setSize()` tells the shell). Its redraw, on the window
  manager's task, draws the dirty rows, the rows under the damage, the rows where the cursor was
  and is, and the margin past the last cell; the console's `changed` hook asks for an update.
  Rows are redrawn, not scrolled (step 4: a redraw is twice as fast as a PSRAM copy).
- **The window manager** gains a `closed` callback per window (so a terminal knows its window
  went); the shell gets the program launcher from `main` (`Terminal::setLauncher()`).
- **Commands:** `win term [N]` (a new or detached session, or session N, in a new window);
  `win type N TEXT` (window N's terminal; `\n`, `\r`, `\t`, `\e`, `\xHH` and `^C` style escapes);
  `win list` marks terminals.
- **Tests:** host tests for `ScreenConsole` and `TerminalView`; on the board, a shell in a window:
  commands typed with `win type`, output checked by reading the screen model back
  (`win dump N`), and every pixel of the window against the model (`win check` learns terminals:
  the cells rendered by the scalar kernel as the reference).

## 4f. Step 8a design: decorations, title-bar buttons, minimize and maximize (2026-10-05; built)

The first of step 8's affordances. Resize handles, pointer shapes, the wheel and menus follow.

- **Decorations are flags the window chooses** (`WindowFrame::Decoration`, a byte): `Border`
  (4-pixel sides, a bottom, and a top when there is no title bar), `TitleBar`, and four buttons
  that need the title bar: `MenuButton`, `MinimizeButton`, `MaximizeButton`, `CloseButton`.
  `Standard` is all of them; `None` is a bare client area (the window is its client). The flags are
  set in `WindowConfig` and changed by `setDecorations()`. The outer rectangle stays put and the
  client area grows or shrinks.
- **`WindowFrame` becomes a value**: the outer rectangle and the flags, with `client()`,
  `titleBar()`, `title()`, `button()`, `edges()` and `partAt()` (which part a point is on, so the
  pointer code and, later, resize handles and pointer shapes use the same geometry). Static
  `outerWidth()`/`outerHeight()` give the outer size for a client size (the terminal sizes itself
  in cells), and `minWidth()`/`minHeight()` the smallest window for a set of flags.
- **Buttons:** 16 × 16, 2 pixels below the title bar's top, 4 pixels apart and 4 in from its ends,
  so every edge stays on a 4-pixel column. The menu button is at the left; minimize, maximize and
  close are at the right in that order. Each is one 16 × 16 glyph from a small icon font of BMDS's
  own (`Fonts::icons16`, drawn as pictures in its source, in the private use area): menu (≡),
  minimize, maximize, restore (when maximized) and close (×). A button is then one `cells()` call,
  and `win check` models it from the same bits. (unscii's □ was tried first: its squares are 8 × 8
  cells doubled, so they draw as tall bars.) The close button keeps the `close-box` and
  `close-mark` roles; the others use two roles appended to the palette, `button` and
  `button-mark`. A button pressed
  with the pointer still on it is drawn with its two colors swapped. It acts on release over it,
  as a close does now. The menu button does nothing until there are context menus.
- **Minimize** hides the window: `WindowStack` keeps hidden windows after the shown ones (they keep
  their number, are in no layer, and are never drawn), `hide()` damages what the window showed,
  and `show()` puts it back on top like an open. Focus goes to the new top window. With no panel
  yet, `raise()`/`win raise N` restores a hidden window, and `list()` reports the hidden ones last,
  marked minimized.
- **Maximize** moves the window to the whole screen (the work area, once a panel takes the top)
  and keeps its old rectangle; the button again, a double click on the title bar (400 ms), or
  `restore()` puts it back. A maximized window isn't moved (`move()`/`drag()` give `InvalidState`,
  and its title bar doesn't grab).
- **A window is told its client size** when it changes (maximize, restore, decorations; resize
  handles later): a `Resized` callback, like `Closed`, called with the lock held. The terminal
  resizes its screen (`ScreenConsole::resize()`, which raises the console's resize interrupt, the
  shell's `SIGWINCH` path); the test windows take the new size. The window's whole rectangle is
  already damage from the change.
- **Commands** (appended): `win decorate N [FLAG...]` with `all`, `none`, `+name` and `-name`
  (`border title menu minimize maximize close`), printing the flags; `win minimize N`,
  `win maximize N`, `win restore N`. `win list` appends ` (minimized)` or ` (maximized)` after
  the existing marks. `win check` models the new frames: buttons and their glyphs, frames without
  a title bar or border, and hidden windows (not drawn).
- **Cost:** no new allocation; a window's state grows by its decorations, flags and restore
  rectangle (about 12 bytes each, 16 windows). Drawing a frame adds up to four small fills and
  four one-glyph texts, only where damaged.

## 4g. Step 8b design: resize handles and pointer shapes (2026-10-05; built)

- **`Resizable` is a decoration** (a seventh flag; it needs `Border`, and `Standard` now includes
  it). A resizable window's handles are the outer 4-pixel ring, its title bar's ends and top
  included, so a frame with a title bar can be sized from the top too. Within 16 pixels of a corner
  the ring is that corner's handle. The title-bar buttons win where they overlap the ring.
  `partAt()` names the eight handles (parts appended after `Close`).
- **Outline resizes**, like outline moves: pressing a handle raises the window and remembers the
  rectangle and the press. Motion moves the edges the handle holds, shown as the outline. Release
  puts the window there and tells it its client size (`Resized`, from §4f) once, not on every
  motion; a terminal reallocates its screen once per resize. The computation is pure and
  host-tested (`WindowFrame::resizedRect()`): the edges not held stay put, the window stays on the
  screen, no smaller than its decorations allow, x and width on 4-pixel columns.
- **Size steps:** a window may ask for its client size in steps (`WindowConfig::widthStep`,
  `heightStep`; X11's size increments). The terminal asks for 8 × 16, so a hand resize leaves no
  margin and the shell gets whole cells. Width steps are rounded up to a multiple of 4.
  `maximize()` and `resize()` (programmatic) don't snap.
- **Pointer shapes:** the arrow, a horizontal and a vertical double arrow, and the two diagonals.
  Each is a picture with a hot spot. The pointer takes the shape of the handle under it, and keeps
  it while a resize is under way. Applications choosing a shape for their client area (an I-beam
  in the terminal) comes with pointer control (the next item).
- **Commands** (appended): `win resize N W H` (the top-left corner stays, no snapping), and
  `resize` in `win decorate`'s names.
- **Cost:** shapes are rodata pictures; the state is a resize's start (a rectangle, edges and the
  press) and two step sizes per window. The same outline drawing as a move.

## 4h. Step 8c design: pointer control (2026-10-05; built)

- **Shown or hidden on request:** `setPointerVisible()`. A hidden pointer still moves and still
  presses (its place is kept); only its drawing stops, and what it covered is redrawn. `win
  pointer on|off`.
- **Hidden while typing:** a window opened with `WindowConfig::hidePointerWhileTyping` hides the
  pointer when a key goes to it (a press or repeat of anything but a modifier or a lock key), and
  the pointer comes back at the next motion or button change. The terminal asks for it.
- **A window's pointer shape:** `WindowConfig::pointer` sets the shape over the client area. The
  shapes become a public enum, `WindowManager::Pointer` (append only): the arrow, the four resize
  shapes, and a text I-beam. The terminal shows the I-beam. `None` (2026-10-14, `games.md` §12)
  draws nothing over the client area: a game's hidden pointer.
- **The pointer's lock** (2026-10-14, `games.md` §12): `setPointerLock(id, on)`. While the
  window is focused, the pointer is hidden and held still, and every pointer event goes to it
  (motion as `MouseEvent::dx`, `dy`, which every motion event now carries); presses never reach
  frames. Focus elsewhere (Alt+Tab, the Windows key, a new window) lets go until it comes back.
  One window at a time. A full-screen window gets the same capture, the pointer moving unless locked.
- **Getting the pointer back from a window** (2026-10-15, the user's report: in a window, it wasn't
  clear how). Ctrl+Alt lets the lock go (as virtual machines do): the pointer shows, and the window
  gets no mouse at all (moving over it doesn't turn a game's view); a left click in its client area
  takes the pointer again (raised and focused; the click goes no further). Focus leaving and
  coming back takes it again too. While it holds the pointer its title says so ("- Ctrl+Alt frees
  the mouse"); let go, "- click it for the mouse". Ctrl reaches the window first (Doom fires once):
  a chord's first key can't be held back. Full screen keeps the pointer (below).
- **Full screen is exclusive** (2026-10-15, the user's: the Windows key was too easy to hit). While
  a window is full screen and focused, Alt+Enter is the window manager's only key: every other one,
  the Windows key's tap, Alt+Tab, Alt+F4 and Alt+Space included, is the game's. Before, those left
  full screen and did their own thing.
- **`win key TEXT`** (a test aid like `win pointer`): each character sent through the window
  manager as a keyboard would send it (press, then release, with the usage and modifiers a US
  keyboard needs, and the same escapes as `win type`), so the keyboard path (focus, the terminal's
  key encoding, the hiding) is testable from a script.
- **`win check`** already leaves the pointer's area out; a hidden pointer has none, so every pixel
  is checked.

## 4i. Step 8d design: the mouse wheel and a terminal's scrollback (2026-10-05; built)

- **The wheel goes to the window under the pointer**, not the focus (`WindowConfig::wheel`, a
  callback with the notches: positive away from the user). It also brings back a pointer hidden
  by typing. `win pointer DX DY [BUTTONS [WHEEL]]` gains the wheel (an optional argument appended).
- **Scrollback lives in `TerminalScreen`** (`bmds_console`, host-tested): `setHistory(lines)`
  gives it a second allocation in its `MemSpace`. Rows that scroll off the top of the primary
  screen (a scroll region starting at row 0), and rows a resize drops from its top, are copied in
  **without their trailing blank cells**: a ring of cells written in turn (a line in one piece,
  from the start when it doesn't fit before the end) and a ring of lines (where each starts, how
  long). It holds `lines` lines or 64 times as many cells (1,024 at least, so a line is never
  cut), the oldest lost first. A line keeps its length across resizes; the view pads or cuts it.
  The alternate screen (full-screen programs) keeps none, as in xterm. `ED 3` (`CSI 3 J`) empties
  it. `historyCount()`, `historyRow(i, length)`, and `historyAdded()` (lines added ever, for a
  view that stays on the same lines).
- **Why compact, measured:** the screen's rows (36 KB at 100 × 30, both screens 72 KB) stay in
  the cache while they rotate; a history that copies or swaps whole rows touches a new 1.2 KB of
  PSRAM every line. Copying whole rows into a ring cost 15 µs a line in the screen model (45 ms
  for 3,000 lines over a 69 ms model); swapping rows into the ring by pointer instead wasn't
  better end to end (`time seq 1 3000` into a shown terminal: 0.70 s against HEAD's 0.50 s).
  Even finding where a line ends cost the cache misses: the scan reads the whole row from its
  end. So the screen keeps, per stored row, where its written cells end (an upper bound kept by
  every write and erase: colored blanks are written cells), and a push reads only those cells.
- **The view:** the terminal keeps how far it is scrolled back. `TerminalView::viewRow()` (pure,
  host-tested) maps a shown row to a history line or a screen row, and `cells()` pads a line to
  the width and blanks a wide character cut at the edge. While scrolled back, new output keeps
  the view on the same lines (moved by `historyAdded()`), and every row is redrawn on any change;
  at the bottom, drawing is as before (dirty rows only). The cursor shows only where its row is in
  view. A key that sends anything returns to the bottom; Shift+Page Up and Shift+Page Down scroll
  a page and aren't sent.
- **On the alternate screen** the wheel sends Up and Down arrow keys instead (xterm's alternate
  scroll), so `less` and `vi` scroll with it. Three lines a notch either way.
- **Memory:** 1,000 lines a terminal (64,000 cells: 768 KB, and 8 KB of line records, in PSRAM),
  allocated when its window first opens. `win check` draws a terminal from the same view,
  scrolled back or not; `win dump` adds a line with the history's size and the view's place.

## 4j. Window titles and icons from the application; `exit` closes a terminal (2026-10-05; built)

Asked for by the user after 8d.

- **`setTitle(id, text)`** changes a window's title (copied, `MaxTitle` bytes) and redraws its
  title bar; **`setIcon(id, icon)`** gives it an icon (`WindowManager::Icon`: a size and 0xRRGGBB
  pixels, kept by the caller). The icon is a skeleton: kept, not drawn yet (the window menu's
  button and the panel's window list will). `win title N TEXT` sets a title from the shell.
- **The terminal's title from the program:** `TerminalScreen` takes `OSC 0` and `OSC 2` (xterm's
  "set the window title"; `OSC 1`, the icon's name, is ignored) and keeps the title (up to 63
  bytes, cut on a UTF-8 boundary; the parser keeps 62 of an OSC string, cutting anywhere, so an
  incomplete last character is left out), cleared by `RIS`. `ScreenConsole` calls a title hook, on the
  writer's task with no lock held, when it changes. The terminal's window is then titled
  "TITLE - Terminal", or "Terminal" when the title is empty. The session number leaves the title.
- **`exit` ends a terminal's shell:** `Shell::run()` returns at `exit` (at the prompt) with its
  status. `Shell::Startup` gains an `exited` callback; a shell spawned without one (the serial
  console's) starts a new prompt instead, so the serial console never loses its shell. A finished
  session can't free its own task (a task doesn't end by returning; `Task::stop()` is called from
  another task): it is put on a list and suspends itself, and the next `Shell::spawn()` stops and
  frees the ones on the list. The terminal's callback closes its window and marks the session
  not started, so `win term N` opens a fresh one (the screen reset, the history emptied).

## 4k. A 2D-DMA job that never finishes (2026-10-15; built)

- **What happened:** the desktop froze during a Lucky Duck game: the panel's clock stopped, the CPU
  stayed 96% idle, and `screenshot`, `win pointer`, and for a while `win list`, hung.
  - `win stats` showed 5,540 drawing errors, climbing about 2 a round, with each round taking
    0.9 s where it takes a few ms.
  - Closing the game didn't help; a reboot did.
- **Why it froze:** one of the display surface's `BlitChain` jobs never reached its end of frame.
  - `_busy` then stayed set, and every later `add()`, `run()` and `wait()` waited its 100 ms and
    failed. That is about nine calls a round, all with the window manager's `_lock` held.
  - Every caller of that lock waits forever. The shell sits at priority 2, last in the
    priority-ordered queue, so its commands starved; the panel's and the sampler's priorities
    were lifted to 5 by inheritance.
  - `docs/platforms/esp32p4.md` already recorded the cause class: a 2D-DMA job that never
    completes stays in the pool, and only a restart cleared it.
- **The trigger is not known.** The freeze came during the first real Lucky Duck celebration,
  minutes after a second copy of the game ran out of memory while starting. Neither reproduced
  afterwards: about 20 celebrations (5,000 rounds) and an out-of-memory third copy beside two
  others all gave 0 errors.
- **Recovery (`BlitChain`):**
  - A job older than `StuckMs` (250 ms; a whole 1080p RGB888 copy takes about 60) is ended with
    `dma2d_force_end()`, its completion drained, and the chain freed. It costs about 300 ms of
    drawing instead of the desktop.
  - It is ended only if its channels were given to it. `on_job_picked` now sets `_picked`; until
    then, IDF's `trans->rx_chan` still names the previous job's channel, which may by then be
    another job's.
  - A job never given channels can't be taken out of IDF's queue, so it waits, and the log says
    others hold the channels.
  - `wait()` acts only when the job is old, not merely when the caller's time runs out (callers
    pass what remains of their own deadlines).
  - Each forced end is logged (the first three, then at powers of two): the job's pieces and rows,
    its age, its first piece's rectangle and picture, and whether IDF's `on_desc_empty` (data
    past the receive descriptors: the two sides' bytes disagreeing) fired. That is what to read
    in `log show` the next time, to find the trigger.
  - `stop()` waits up to a second (a stuck job ended within it) before freeing descriptors the
    hardware might still read. A job still queued after that keeps its memory, and the log says
    so.
- **Left as it is:** the window manager waits on hardware with `_lock` held (every draw call up to
  `DrawTimeoutMs`), so any slow driver starves every lock caller. Worth its own design if
  anything else ever stalls.



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
  at most once a frame and may drop to every other one under load. Scrolling by a 2D-DMA copy
  inside surface 0 was measured in step 4 and loses to redrawing the rows (§9).

## 4l. The starting pointer: feedback while a program starts (2026-10-16)

The user's report: launching Lucky Duck gives no sign that anything is happening. It reads its
program, pictures and sounds (several seconds from the card) before its window opens.

- **The picture:** `Pointer::Starting`, the arrow with a small hourglass whose sand runs (three
  frames, a quarter second each). It is appended after `None`. It replaces the arrow wherever the
  arrow would show; the other shapes (a resize handle, an I-beam, a game's `None`) win, so the
  pointer still says what a click there does. The other windows keep working meanwhile.
- **Who says a program is starting:** the panel, after `ProgramLauncher::start()` gives it the pid
  (`FolderActions::launch()`: the desktop, the Apps menu, folders, notifications' actions). It
  calls `WindowManager::programStarting(pid, launcher, timeoutMs)`. The panel starts each program
  as the leader of a new session, so the pid is also the session.
- **What ends it, for each program starting** (up to 4 at once; more replace the oldest):
  - **A window opened by its session:** an application-level window whose opener's task belongs to
    a process of that session. `TaskOwner` gains `sessionId()` (0 for kernel tasks), and the loader
    answers from its process table. `WindowManager::open()` asks before it takes its lock, so a
    child of the program opening the window counts too (`terminal -e`).
  - **The program gone:** `launcher->sendSignal(pid, 0)` says `NotExist`. It is checked at each
    frame of the animation, so a program that fails, or hands off to one already running, stops
    the hourglass within a quarter second.
  - **The time running out:** 20 s.
- **No polling when idle:** the window manager's task waits for its wake as before. Only while a
  program is starting does it wake each quarter second, to turn the sand and run the checks.
- **Not done:** a starting button on the taskbar (Windows' and GNOME's), if the pointer isn't
  enough.
- **Built and checked on the board (2026-10-16):** Lucky Duck from the Apps menu showed the
  hourglass, its sand turning between frames, until its window opened about 1.5 s after the click.
  The arrow then came back, and the program was its own session's leader as expected. The
  program-gone path (`sendSignal(pid, 0)`) and the timeout are untested on the board.

## 4m. A window manager that starved its callers (2026-10-16, the user's report)

Starting the system monitor with Doom in a window froze the desktop for many seconds, then left it
laggy. Reproduced: with windowed Doom alone `win stats` (one lock of the window manager) took
~660 ms; the system monitor's start stalled a shell loop 16 s, then 1-4 s at a time. CPU wasn't
short (Doom ~23%, the monitor ~0%).

- **The cause:** `run()` read the frame number before the round and then waited for the frame
  after it. A round that ran past its frame's end (windowed Doom's grew to ~15 ms; the monitor's
  first drawing more) found that frame already begun, didn't wait, and took the lock again at
  once: a task woken by the unlock never ran before the lock was taken back. Every caller of the
  window manager waited (programs' invalidates and presents, the shell's commands), and the
  monitor, its own lock held meanwhile, deferred its redraws round after round.
- **The fix:** the frame number is read after the round, so the window manager always waits for
  the next frame's start with the lock free. A round shorter than a frame still runs every frame;
  a longer one runs every other.
- **Measured** (Doom windowed, the monitor open over it, seven minutes): `win stats` 4-40 ms
  (30 in 0.1-1.2 s), the monitor's start one 0.24 s gap, ~33 rounds a second (Doom's rate), 5-9 ms
  each.

## 4n. Batched drawing: a round in few 2D-DMA jobs (2026-10-16, the user's report)

The System Monitor felt laggy: slow to draw, its window's outline trailing when moved. Measured
(`prof`, timing logs since removed): its once-a-second refresh took the window manager ~27 ms to
draw, in ~95 2D-DMA jobs a round (~175 a refresh), each paying IDF's set-up (the queue, channel
connects, callbacks, cache syncs: 44% of the window manager's CPU) and a wait.

- **Holding the channels between jobs failed** (the first try): a job without an end of frame keeps
  IDF from giving its channels back, but the receive channel then raises "descriptors empty" at the
  chain's end and IDF frees them anyway (`docs/platforms/esp32p4.md` §4); every later job timed out.
  Removed.
- **`Surface::beginBatch()`/`endBatch()`** (after `enableBatching()`: a 96-piece, 512-row chain and
  12 KB of colour lines in internal DMA RAM, the desktop's surface only): fills and copies from
  pictures (`startBlit()`) are added to one job; it runs when full, at `present()`, `wait()`,
  `endBatch()`, and before anything that reads the surface (`patch()`, `copy()`: a job's transmit
  side can read ahead of its receive side's writes) or a widening copy (the other chain). The
  window manager batches each round.
- **Fills' colour lines** come from the 12 KB, one per colour and width (a line of the colour at
  least as wide is reused), up to 32 rows of it (≤1.5 KB) so a tall fill takes a descriptor per 32
  rows, not per row.
- **Uneven edges** (a fill's sides off the 4-pixel columns: nearly every widget fill) were 2 jobs
  each plus a flush. Now they wait in the batch (64 groups, a colour per pixel, neighbours on the
  same rows merged, overlapping rows split and merged) and are read, patched and written for all at
  once only when something must see them. A later write of whole groups over a pending one (text on
  its background) takes those rows from it instead (`coverEdges()`): it sets those pixels anyway.
- **The strip** (`Painter::Tools`) is a column allocator now: pieces side by side, half after half,
  reused only after a wait (a batch keeps its sources until it runs); the window manager starts it
  over after each round.
- **The System Monitor's tables** were cleared and refilled every second and their views reset (a
  whole repaint) even when nothing changed: `TableModel::takeChanged()` (a hash of the rows) skips
  the reset then.
- **Measured** (the System Monitor's overview, idle): jobs ~95 → ~5 a round; the refresh's drawing
  27 → 7-8 ms (and 290,000 → ~100,000 pixels a second); the window manager's CPU while drawing
  ~40% less. `win stats` gains `Jobs:` (2D-DMA jobs on the desktop's surface). `win check` 0 wrong.
- **Thin strips** (2026-10-16, after): a text, cells, icon or row band is made only for the clip's
  spans in its rows (`Painter::run()`); what lies before a span is passed over by measuring
  (`GlyphRenderer` with no output: the widths, nothing drawn) and nothing is made past the last.
  The window manager's CPU during a drag ~22% less again; the rounds no faster (~16 ms): they wait
  for the 2D-DMA now, not the CPU (busy ~14% of a core).
- **`Full` from a batch's rows:** a batch runs out of transmit descriptors (a fill's rows) before
  its pieces, and `copyForward()` and `copy()` ran the job only at a full count of pieces: their
  `add()` failed with `Full`, a window's drawing counted an error (`wm0.errors`, the user's report).
  Both run the job and add again now; 0 errors in 28 drags since.

## 4o. Queries that don't wait for a round (2026-10-16)

The outline's trail: a GUI application handles a pointer event holding its own lock and asks the
window manager for its client area (`clientRect()`), which waited for the window manager's lock,
held for a whole round; that round meanwhile found the application busy (`tryLock`) and put off
its redraw, leaving the old outline's strip unrestored. The System Monitor's timer did the same
through `isMinimized()`.

- **A snapshot** (each window's client area and minimized state, under a seqlock) is taken before
  each round; `clientRect()` and `isMinimized()` read it when the lock is held (nothing moves during
  a round), and take the lock otherwise (or when no snapshot is kept: a window opened since).
- **`setPointer()`** called during a round records the shape and wakes the window manager, which
  applies it before the next round (`Ok` returned).
- **Measured:** deferred redraws during a drag of the System Monitor 5 → 0; no event held the
  application's lock past 5 ms.

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
3. **The window manager on surface 0** (done; design in §4a, as built in §9): windows, the stack, visible
   regions, damage and redraw requests, the desktop color, frames with titles, outline moves; test
   windows that redraw a pattern; `win start/stop/list/new/move/drag/raise/close/color/check/stats/shuffle`.
4. **Drawing** (done; design in §4d, as built in §9): the `Painter` (cells with a PIE kernel, overlapped
   with the 2D-DMA, scrolling, unaligned edges), window updates, `win bench`, `win scroll`.
5. **The terminal program** (done; design in §4e, as built in §9): `TerminalScreen` drawn per frame, a
   `Console` on it, a shell in it, `win type`.
6. **At boot** (done 2026-10-05, decided with the user): BMDS boots minimal, the display and
   the desktop off; `/sd0/p0/autoexec.sh` brings up whatever environment it wants. The card's
   script now holds `display start 720 double`, `win start`, `win term`: the desktop with a
   focused terminal, verified with a screenshot after a reset. The display boot stage and
   `CONFIG_BMDS_DISPLAY_AT_BOOT` are gone (the test picture stays: `display test`).
7. **Later:** backing surfaces (copied by the window manager) and secondary surfaces (read by the
   DMA, §4c), chosen per window.
8. **Window manager affordances** (asked for 2026-10-05, after the keyboard and mouse landed,
   `input.md`); roughly in this order:
   - **Decorations under the application's control** (done, §4f): a window says which it has (a
     frame at all, a title bar, each title-bar button; resize handles with them), set at open and
     changeable.
   - **Title-bar buttons** (done, §4f): minimize, maximize and close on the right; the button on
     the left becomes the window's menu once there are context menus.
   - **Resize handles** (done, §4g) on the frame's edges and corners, with the pointer's shape
     changing over them (and over other hot spots); an outline resize like the outline move; a
     window told its new size (a terminal resizes its screen, `SIGWINCH`).
   - **Pointer control** (done, §4h): shown or hidden on request; a window can ask for it to
     hide while it is typed into and to come back on the next move (the terminal does).
   - **Mouse wheel** (done, §4i): delivered to the window under the pointer; the terminal
     scrolls back through its history with it (needs a scrollback in `TerminalScreen`).
   - **Context menus** (done, `gui.md`): a right click on the desktop opens one that starts
     programs (the launcher's entries), the title bar's left button (or a right click on its
     title bar or its panel button) opens the window's.
   - **A panel along the top** (done, `gui.md`): a program launcher button and a list of the
     windows (minimized ones restored from it), and a clock; `/bin/panel`, on a GUI toolkit
     (`bmds_gui`), its layout and its launcher's entries from configuration files. The window
     manager gained window levels (desktop, normal, panel, popup), mouse events for client areas,
     popups, a work area, and `watch()` for the window list. Then (`gui.md` §4e-§4k): bevelled
     title-bar buttons in the toolkit's 3D look, new windows that ask are cascaded, Alt+Tab,
     Alt+F4, Alt+Space, Alt+drag, Always on Top, window icons on the title bar, global keys
     (the Windows key), snapping, and rolling up.

9. **Built-in applications as ordinary applications** (the decision in §0; designed 2026-10-05
   with the user; with `programs.md` §17, `/bin`, and `pty.md`, pseudo-terminals):
   - **Connecting first:** a GUI application starts with `WindowManager::connect(display)`
     (display 0 for now; a parameter, not an environment variable: one user, no `DISPLAY`). It
     fails (`NotRunning`) when no window manager runs, and every window belongs to a connection.
     When the window manager stops, each connection is told (its windows close, and further calls
     report it gone); when an application disconnects, or ends without closing its windows, they
     close with it. Run from the serial shell, `terminal` opens its window on the desktop; the shell
     waits for it as for any program (`terminal &` doesn't), and Ctrl-C there ends it.
   - **The terminal is `/bin/terminal`:** a program (`BMDS_PROGRAM`) in its own component, depending
     only on the GUI API, the console's pty and the program launcher, as a program built
     separately would. One process per window: it connects, opens a window and a pty, starts
     `/bin/sh` on the pty (`ProgramLauncher::start()`), and applies what comes out of the pty to
     its screen model, which its window draws. The shell exiting closes the window and ends the
     process; the window closing hangs up the pty, so the shell ends too. The numbered sessions
     that outlive their windows go.
   - **Its colors are its own:** the `terminal-*` roles leave the window manager's palette; `win
     color` keeps accepting their names (an interface), passed on to the terminal's settings.
   - **`win` knows no application:**
     - `win term` runs `/bin/terminal` in the background (kept: shell commands are frozen).
     - `win type N TEXT` becomes key events for window N (as `win key`, to a chosen window).
     - `win dump N` asks window N for its text through an optional callback any window may
       offer; the terminal offers its screen.
     - `win check` compares each window against a reference picture that the window may draw
       through another optional callback; the test windows and the terminal offer theirs.
     - `win list` drops the "(terminal)" mark.
   - **What goes:** `Terminal::setLauncher()` in `main`, `Terminal::byWindow()`, the terminal in
     `bmds_ui`, `ScreenConsole` as the terminal's console (kept for a local console without the
     window manager, roadmap Phase 5).
   - **Order:** `/bin` (`programs.md` §17), the pty (`pty.md` steps 1-2), `connect()` and window
     ownership, then the terminal as `/bin/terminal` with its colors and the `win` changes.

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
- **Step 3** (2026-10-05): the window manager on surface 0 (§4a).
  - `bmds_ui`, pure and host-tested: `WindowStack` and `WindowFrame`. Random opens, closes, moves,
    raises and invalidations against pixel maps of who shows each pixel: every change of owner is
    damaged, nothing outside the rectangles involved is, a raise damages exactly what was covered,
    an invalidation exactly what of it shows; the plan gives each piece to the window showing it.
  - On the target: `Painter` (fills cut to the clip and batched 32 to a job, text through the strip
    with the nibble table), `WindowManager` (display 0's, claims the display as "desktop", a task
    on core 0 at priority 5 with 6 KB of stack), and the `win` command.
  - `Surface` gained `fill(Span<const Rect>)` (one line of the color, the widest's length; up to 32
    rectangles a job; the line written 12 bytes at a time) and `waitFrame()` (each frame wakes the
    surface's waiters when it has any).
  - Memory: the strip is 60 KB of internal DMA RAM at 720p (the screen's width × 16 rows), a
    round's plan and clips 6.5 KB of internal RAM, all freed by `win stop`; `bmds_ui` is 1,640 bytes
    of `.bss` and 13 KB of code.
  - Measured at 1280×720, 16 test windows: `win check` (every pixel of the shown picture: the
    desktop, the frames and their titles' glyphs, the clients' patterns) found 0 wrong after
    opening, raising, moving, dragging, closing, a desktop color change and 300-change shuffles,
    single- and double-buffered; 0 underruns. A round averaged 2.2 to 2.7 ms single-buffered and
    5.0 ms double-buffered (the copy forward included; 26 ms at most, the first full screen); a
    full-screen desktop fill is 8.4 ms. Each change was drawn in one frame (16.6 ms a change
    through `sync`, the frame's pace). No plan needed the fallback.
  - Not yet: the SIMD glyph expansion and unaligned edges (step 4), a mouse (Phase 5).
- **The palette** (W12, 2026-10-05, the user's request): `WindowPalette` (pure, host-tested names)
  replaces the colors fixed in the window manager; `win color` lists the roles and sets one.
  Measured: 0 wrong pixels after changing the desktop, a frame, the inactive title and the client
  colors; repainting the whole screen for a role took 20 ms.
- **Step 4** (2026-10-05): the Painter (§4d).
  - `GlyphRenderer` (`bmds_ui`; the scalar kernel host-tested against the font's bits, bold and
    underline, and runs against single glyphs): `drawCells()` and `drawText()` keep the PIE
    registers loaded over a run and reload the colors only when they change. Measured with
    `win bench` (160 cells, a color change every 7, some styled; PIE and scalar give the same
    bytes), in cycles a cell:

    | Version | Scalar | PIE |
    |---|---|---|
    | First (a glyph a call, a table rebuilt per color) | 1,398 | 479 |
    | PIE with a 256-entry mask table, two rows interleaved | | 503 |
    | Colors set with word operations | 860 | 462 |
    | Runs (registers kept), the font's ASCII fast path inline | 818 | 360 |

    The PIE kernel alone is 236 cycles a cell (16 rows of 13 instructions, near one a cycle); the
    mask table was slower (326 alone) and costs 8 KB, so it went. Raw stores into internal RAM:
    `esp.vst.128` 7 to 13 bytes a cycle, `memset` 2.6, so the kernels aren't store-bound.
  - `Surface`: `fill()` takes any rectangle (sides off the 4-pixel columns: their groups read into
    the fill's line buffer, patched, written back: two jobs a call, no extra memory);
    `startBlit()`/`wait()` (a copy cut to a clip as one job, not waited for); `copy()` (in order).
    `BlitChain` gained `submit()`/`wait()`.
  - `Painter`: exact fills, `cells()` and `text()` through two strip halves (the strip is still
    60 KB at 720p: 2 × 640 × 16 pixels), `scroll()` (only visible, undamaged sources; the rest
    returned to redraw), the damage (`damageAt()`, `damageBounds()`).
  - `WindowManager::update()`: a redraw without damage, clipped to all of the visible client
    area. The `wm` task's stack is 8 KB (2.9 KB left at worst in the tests).
  - Test windows: 1-pixel grid lines off the 4-pixel columns, a label of cells (bold, underline,
    colors, ░▒▓☺Ä), `win scroll N DY`; `win check` models them (the label drawn by the scalar
    kernel as the reference): 0 wrong pixels after shuffles, scrolls both ways (partly covered
    windows among them), text, drags and palette changes, single- and double-buffered, 0
    underruns.
  - Measured at 720p, a full-screen window (`win textbench`): 43 rows of 159 cells in 11.7 ms,
    18.4 ms with the overlap turned off. Scrolling the same window by 16 rows (a 1272 × 664 copy
    inside PSRAM) takes 23.0 ms: PSRAM to PSRAM runs at the 110 MB/s each way of step 2. **A
    terminal redraws its rows rather than scrolling them;** `scroll()` is for content that costs
    more to draw than to copy.
  - Size: `bmds_ui` 20 KB of code, 2.5 KB of `.bss` (the test windows' state among it).
  - Next levers, not taken: the 2D-DMA job's fixed cost (about 86 jobs in the full-screen text,
    one per strip half), by driving the 2D-DMA directly (§7).
- **Step 5** (2026-10-05): the terminal program (§4e).
  - `ScreenConsole` (`bmds_console`, host-tested: output with and without CR LF, input, Ctrl-C
    dropping what came before it, replies as input, one redraw asked per burst) and `TerminalView`
    (`bmds_ui`, host-tested: defaults, bold, underline, reverse, dim, invisible, wide characters,
    the cursor, also on a wide character's tail). Three palette roles appended: `terminal-text`,
    `terminal-background`, `terminal-cursor`. `TerminalScreen` gained `clearRowDamage()`.
  - `Terminal` (`bmds_ui`): four numbered sessions; a session keeps its shell (task `termN`, its
    64 KB stack in PSRAM) and screen when its window closes, and `win term N` shows it again.
    `main` gives the terminals the program launcher.
  - The redraw locks the screen a row at a time (output goes on while rows are drawn; a row
    written meanwhile is dirty again), and `WindowManager::update()` no longer takes the window
    manager's lock (an atomic mask taken by the next round): a writer never waits for a round.
  - Commands: `win term [N]`, `win type N TEXT` (escapes and `^C`), `win dump N`; `win list` marks
    terminals; `win check` draws each terminal's client area from its screen with the scalar
    kernel and compares every pixel.
  - Measured at 720p: a shell in a 100 × 30 window runs commands typed with `win type` (`ls`,
    `help`, `printf` with colors, reverse, underline and a wide character, `clear`, `seq`);
    `$COLUMNS x $LINES` is `100 x 30`; Ctrl-C stops `seq 1 10000000`. `win check` found 0 wrong
    pixels with output scrolled, two overlapping terminals, a window closed and its session shown
    again, terminal colors changed, drags and shuffles, single- and double-buffered; 0 underruns.
  - Output speed, `time seq 1 3000`: 0.31 s to `/dev/null`, 0.41 s on the serial console, 0.43 s
    into a terminal whose window is covered (the screen model and the console: 0.12 s), 0.55 s
    into a shown window (the window redrawn each frame: all 30 rows change while it scrolls, a
    round of about 7.3 ms). The two lock changes above took it from 0.58 s to 0.55 s.
  - The `wm` task kept at least 3.9 KB of its 8 KB stack in these tests.
- **Step 8a** (2026-10-05): decorations, title-bar buttons, minimize and maximize (§4f).
  - `WindowFrame` is a value (the outer rectangle and the flags) with `partAt()`; host-tested for
    every valid set of flags (34 of the 256 bytes): the parts tile the window without overlap,
    the buttons sit on the title bar apart from each other and the title, on 4-pixel columns, and
    `partAt()` agrees with the rectangles. `WindowStack` gained hidden windows (`hide()`, `show()`,
    `rectOf()`), added to the random damage test (a hide damages what the window showed, a show
    what it covers).
  - `Fonts::icons16`: five 16 × 16 icons (216 bytes of rodata). Palette roles appended: `button`,
    `button-mark`; the close button is now red with a white ×.
  - The terminal resizes its screen when its window does: maximized at 720p, `$COLUMNS x $LINES`
    is `159 x 43` and `stty size` says `43 159`; restored, `100 x 30` again.
  - `win check` found 0 wrong pixels with windows with every decoration, none, the border alone,
    and the title bar with only a close button; after maximizing and restoring the terminal
    and a test window, minimizing and raising, and with the pointer pressing buttons. By the
    pointer: a button is drawn with its colors swapped while held and acts on release over it
    (moving off first cancels), minimize hides, maximize fills the screen, a double click on the
    title bar restores, close closes.
  - Rounds measured with `win stats` at 720p, double-buffered: maximizing the terminal 26.8 ms
    (the whole screen of cells), restoring it 60.0 ms (four test windows and the desktop under it
    redrawn); maximizing a test window 135.0 ms, all of it its pattern's hundreds of fills and
    one-pixel lines (a test window draws slowly on purpose; a terminal is the real case).
  - Size: the window manager's code grew by 3,357 bytes and its `.bss` by 256 (the restore
    rectangle, flags and resize callback of 16 windows).
- **Step 8b** (2026-10-05): resize handles and pointer shapes (§4g).
  - `WindowFrame`: `Resizable` (in `Standard`), the eight handles from `partAt()`, `edgesOf()` and
    `resizedRect()`. Host-tested: the handles around the ring and near the corners, buttons first;
    20,000 random resizes (three sets of decorations, four pairs of steps, every handle) stay on
    the screen, on 4-pixel columns, no smaller than allowed, in steps on the axes sized, with the
    edges not held kept and a held edge within a step of the pointer. The first run caught a bug:
    a handle holding one axis snapped the other one to its steps too, so dragging a terminal's
    top edge moved its right edge.
  - On the board, by the pointer: the terminal's right edge dragged 80 pixels gave 110 columns
    (`$COLUMNS`), its bottom-right corner 120 × 26 (whole cells, no margin); a test window's
    top-left corner moved its left and top edges only, and shrinking it past the least stopped at
    100 × 32 with its far edges kept. `win resize 2 333 211` gave 336 × 211, moved up to stay on
    the screen; too small is `Invalid`. `win check` found 0 wrong pixels after each. Each shape
    was checked in screenshots over its handle, and the outline and the diagonal pointer
    mid-resize.
  - Size: the window manager's code and rodata grew by 2,634 bytes (the five pictures among
    them) and its `.bss` by 80.
- **Step 8c** (2026-10-05): pointer control (§4h).
  - `WindowManager::Pointer` is public (the five shapes of 8b and an I-beam, `Text`);
    `setPointerVisible()`; `WindowConfig::pointer` and `hidePointerWhileTyping`, both taken by the
    terminal. `win pointer on|off` and `win key TEXT` (a US keyboard's usages and modifiers for
    each byte, a press and a release, through the window manager's queue, synced every 8 keys so
    the 32-event queue doesn't overflow); `win type`'s escapes are shared with it.
  - On the board: over the terminal's text the pointer is the I-beam; `win key 'echo hi from
    keys\r'` ran the command through the keyboard path and hid the pointer (`win check` then
    compared all 921,600 pixels: 0 wrong); a 1-pixel move brought it back. Hidden with `win
    pointer off`, it stayed hidden while moved (0 wrong of all pixels) and came back with `on`.
    `^C` typed with `win key` stopped `sleep 30`.
  - Size: the window manager's code and rodata grew by 486 bytes (the I-beam among them) and
    its `.bss` by 16.
- **Step 8d** (2026-10-05): the mouse wheel and a terminal's scrollback (§4i).
  - Host-tested: the history's rules (what goes in, the alternate screen and lower scroll regions
    keeping none, resizes, `ED 3`, colored blanks kept); 20,000 random steps (scrolling, resizes,
    clears, alternate-screen excursions, history sizes, a quarter of the lines empty, lines up to
    145 cells so the cells run out before the lines do) against a model of every line written; 500
    random screens (printing, wide characters, erasing in and out of color, inserting and deleting
    cells) whose rows must reach the history whole. Each rule was checked by breaking it on
    purpose: letting an empty line stop the eviction scan, skipping the wrap's eviction, not
    counting a colored erase or an insertion in a row's end all fail the tests. One run caught a
    real bug: an empty line (no cells) at the write point was dropped by the next line.
  - On the board: the wheel over the terminal scrolled back three lines a notch and forward again;
    output arriving while scrolled back kept the view on the same lines (6 back became 8 back as
    two lines came); maximized while scrolled back, the history lines were padded to 159 columns;
    a key returned to the bottom. In `vi` the wheel moved the cursor (arrow keys), and `vi` quit
    back to the shell's screen with its history. `win check` found 0 wrong pixels in each state.
  - `time seq 1 3000` into a shown 100 × 30 terminal: 0.51 s (eight runs of ten; two 0.45 s)
    against 0.50 s before (ten of ten). The screen model alone: 73.9 ms for 3,000 lines with the
    history full against 70.6 ms without (1.1 µs a line).
  - A bug found on the way, and fixed: `win key` lost characters past the 24th now and then. A
    queued key wasn't counted as a change, so `sync()` (which `win key` waits on every 8 keys)
    returned before the keys were delivered and the 32-event queue overflowed. Keys now count, and
    a round takes the count before it delivers them.
  - Size (with step 8c): `bmds_ui` 1,806 bytes more flash and 272 more `.bss`; `bmds_console`
    1,072 more code.
- **Titles, icons, `exit`** (2026-10-05, §4j): `WindowManager::setTitle()`, `setIcon()` (kept,
  not drawn), `win title N TEXT`; `TerminalScreen::title()` from `OSC 0`/`OSC 2`;
  `ScreenConsole::setTitleHook()`; `Shell::run()` returns at `exit`, `Startup::exited`, ended
  sessions reaped by the next `spawn()`. Host-tested: titles by BEL and ST, `OSC 1` and others
  ignored, a repeat not reported, a long title cut whole, the empty title, `RIS`. On the board:
  `printf '\033]2;hello there\007'` titled the window "hello there - Terminal" and an empty one
  "Terminal" again (`win check` 0 wrong); `exit` closed the window, its task waited suspended,
  and `win term` reaped it and opened a fresh session (row 0, no history); two exit-and-reopen
  cycles left free PSRAM at 26,885,076, 26,885,324 and 26,885,332 bytes.
- **Step 9: built-in applications as ordinary applications** (2026-10-06, §8 step 9, `pty.md`):
  - `WindowManager::connect(gone, arg)` (`InvalidState` when no window manager runs) and
    `disconnect(client)`, which closes the client's windows; `stop()` tells each client it is
    gone. A window's config names its client, a kind (`win list` shows it, as "(terminal)" was),
    and two optional callbacks: `dump` (its text, for `win dump`) and `reference` (its client area
    as it should look, for `win check`). `keyTo(id, event)` sends a key to a chosen window.
  - The terminal is `/bin/terminal` (`components/bmds_terminal`, `BMDS_PROGRAM`, linked whole
    because `main` requires it; 3,928 bytes of code, no `.bss`). It uses only the GUI API, a
    `Pty` and `Shell::spawn()`: the session's shell runs on the pty's console in its own task,
    and the program's task reads the pty into its `TerminalScreen`. The shell exiting ends the
    pty's reads, so the program closes its window and ends; the close button hangs up the pty, so
    the shell ends. The old `Terminal` in `bmds_ui`, `Terminal::setLauncher()` and
    `Terminal::byWindow()` are gone. `win term`, `type`, `dump`, `check` and `list` use the
    generic calls above; `win term` starts the program with `ProgramLauncher::start()`.
  - The terminal's colors became its own later the same day (`gui.md` §4l): `/boot/config/
    terminal.conf`, and the window manager's resources for `win color terminal-*`.
  - On the board: `terminal` without the window manager printed "terminal: the window system
    isn't running ('win start')"; run from the serial shell it held the shell until Ctrl-C closed
    it; `exit` in a window closed it; `win stop` ended every terminal process. `win type`,
    `win dump` and `win check` (0 wrong of 921,600) worked on a pty terminal. `time seq 1 3000`
    into a shown 100 × 30 terminal: 0.45 s (0.51 s before). Five open-and-exit cycles: internal
    RAM free the same (451,295), PSRAM 8 bytes less.
- **The pointer saves what it covers** (2026-10-06, the user's report: the pointer trailed a little
  when moved over the desktop): a move used to damage the area it left, which the window under it
  redrew; over the desktop that is the panel, which the same move had just woken with a motion
  event, so its redraw often found it busy and waited a round (`Painter::defer()`), leaving the
  pointer drawn where it was. Now `drawCursor()` saves the area's pixels as it draws the pointer
  (in the same `patch()`), and each round that draws starts by putting them back
  (`restoreUnder()`), before any window draws: a move asks no window for anything, and nothing a
  window draws or scrolls (`Painter::scroll()` copies) can carry the pointer's picture. 1.5 KB of
  PSRAM (`UnderBytes`). Measured: 60 moves over the desktop, 60 rounds of 0.47 ms on average, no
  redraws and none put off; `win check` 0 wrong, with a terminal scrolling 400 lines under the
  pointer, with typing hiding it, over a window's edge (another shape) and after a resize.
