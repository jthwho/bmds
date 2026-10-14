# Games: surfaces for programs, a game library, and the first games

Status: **planned** (2026-10-14, with the user). Needs application folders (`apps.md`) for
installing.

## 1. Goal

Games, for two reasons (the user's): to have fun with the system and show what it can do, and to
demo it. They are the first programs that need graphics, so they bring the first graphics ABI.

- **The games:** Blocks (Tetris's rules, §6a), Solitaire
  (Klondike), and **Doom**.
- **What they need from the system:** drawing at full speed (the user: an exclusive mode, as
  audio has one), responsive audio beside the picture, and raw input.
- **Controls** (the user's): the keyboard first-class, gamepads where they apply, bindings changed
  one standard way (`controls.md`).
- **Where code goes** (the user's): what games share is in the SDK, built out as they need it;
  what is one game's stays in that game.

## 2. Decisions (the user's, 2026-10-14)

- An **exclusive mode for graphics**, as audio has: the most performance for whoever needs it.
- **Windowed and full screen through one API** (§3): the program draws its own buffers; the
  system decides where they go. Which one is a **per-application setting**, its default
  **suggested by the application**; **Alt+Enter toggles** it while running.
- **Pull out all the stops** (the user's): the surfaces' work may rework any of the display's and
  the window manager's paths for speed (PIE, assembly, DMA, the PPA, IRAM where measured), to make
  the system as graphically capable as possible.
- Games are **application folders** (`apps.md`) loaded from the card, not built in (the firmware
  partition has 1.375 MB, and Doom is GPL).

## 3. Surfaces for programs (the ABI)

The first graphics ABI. Smaller than `windows.md` §6's full one (no widgets, no damage reports,
no redrawing when uncovered): a window whose contents are one surface the program draws itself.
Solitaire, the media player's video and a screensaver fit it as well as games do; the toolkit's
ABI comes later and is separate.

### 3a. The model: a swap chain

- The program **opens a surface**: its size and format, which never change while it is open
  (`BMDS_PIXEL_L8` with a palette of 256, `RGB565`, `RGB888`), and 2 or 3 buffers. A game draws
  at its own small size: the blocks game at 640×360 RGB565 has 460 KB buffers; only one that asks
  for 1280×720 RGB888 pays for full-size (2.76 MB) ones.
- **Buffers are the program's memory** (tracked and freed with it): PSRAM, or internal RAM
  (`BMDS_MEM_FAST`, DMA-capable) for a small one (Doom's 320×200 is 64 KB). The program
  **acquires** a free buffer (waiting if all are queued, or `tryAcquire` style with
  `BMDS_EAGAIN`), draws into it with the CPU, and **presents** it. The kernel writes the cache
  back (the program doesn't), queues it, and returns the buffer when it has been used.
- **Presenting returns the frame number** it will be shown in; a wait for a frame and a frame
  counter are the surface API's planned `present()`/`waitShown()`/`frames()` (`windows.md` §0).
  Pacing is by the display's 60 Hz; a game with its own tick (Doom's 35 Hz) presents when it has
  a frame. With 3 buffers, a newer present replaces a pending one (mailbox).
- **The palette** (L8) is set by a call and takes effect with the next presented buffer, so
  palette effects (Doom's damage flash) land on the right frame.

### 3b. Presentation

The same buffers go one of two places; only the destination changes:

| | What a presented buffer becomes |
|---|---|
| **Full screen** | Scaled into the scan's buffer (the window manager and the panel draw nothing). A surface at the display's size and format (1280×720 RGB888) needs no scaling: the scan reads the buffer itself, a **zero-copy flip** at a frame's start. |
| **Window** | Scaled into the window's area of surface 0 by the window manager, only the visible parts (2D-DMA, PPA): `windows.md` §0's "backing surface" kind. |

- **Scaling** keeps the aspect ratio (letterboxed), filling the screen or the window; a surface
  can ask for **whole multiples only** (sharp pixels). Pixels may be non-square: a surface says
  its aspect (Doom's 320×200 was shown at 4:3, so 960×720 is right).
- **Who scales:** the PPA (scale, colour conversion, and the blender's CLUT for L8:
  `display.md` §8 measured 52-60 Mpixel/s and ~32,000 cycles a job). Its factors are 16ths
  (`docs/platforms/esp32p4.md`): Doom's 3.6 isn't one (3.5625 gives 712 lines, a 4-line border).
  A PIE scaler in the kernel where the PPA can't (exact factors, or too slow): measured first
  (step 1).
- **Tearing in a window:** surface 0 is single-buffered, so a copy can race the scan. The copy is
  started just after the scan passes the window's lines, if the scan's per-block progress allows;
  measured in step 1.
- **The display's mode doesn't change:** 1280×720 stays on the link; a mode change blanks the
  monitor for seconds.

### 3c. Window or full screen

- **The setting** is per application (its id, `apps.md` §6), kept by the window manager in
  `$HOME/.config/bmds/apps.conf`. Until the user chooses, the application's suggestion
  (`X-BMDS-Display` in `app.desktop`, or a flag when the surface is opened) decides.
- **Alt+Enter** toggles while the surface's window has the focus, and the choice is saved as the
  setting. The program gets a "presentation changed" event (its new scale and area) but needn't
  act on it: its buffers don't change.
- **Full screen, while in front:** the panel is hidden, the window manager draws nothing, and
  the system's keys are limited to the ones that leave (Alt+Tab, the Windows key: the escape
  hatch that always works, so a hung game never needs the power switch). Leaving shows the
  desktop again and the game becomes inactive (§3d); coming back restores full screen.
- **Audio** isn't taken over: a game uses a shared real-time stream (64 frames is 1.3 ms, far
  under a frame's 16.7 ms), so the volume tray and notifications keep working. Exclusive audio
  (`audio.md` §20) stays the program's own choice.

### 3d. Events

The surface's descriptor gives events (`read`, and `poll` says when there are some):

- **Keys:** down, up and repeat, with the HID usage, the modifiers and the layout's character
  (games want keys, text fields want characters; both come). Only while the surface has the
  focus.
- **Pointer:** position in the **surface's own pixels** (the system undoes the scaling, so a
  click means the same in a window and full screen), buttons, the wheel; relative motion with the
  pointer hidden and held (mouse look) on request.
- **State:** active and inactive (focus lost, minimized, left full screen: games pause),
  presentation changed, close asked (the window's close button, Alt+F4).
- **Actions and gamepads** (`controls.md`): the application's declared actions arrive as
  action events (what was bound to them, keyboard or pad, already matched by the system), and
  pads' raw events too, numbered per pad. Every action has a keyboard binding.

### 3e. System calls (sketch; numbers appended when designed)

- `surface_open(const bmds_surface_config_t *)` → a descriptor: size, format, buffers, aspect,
  whole multiples, suggested presentation, title.
- `surface_acquire(fd, timeout_ms)` → a buffer's index and address (`bmds_surface_buffer_t`:
  pointer, stride).
- `surface_present(fd, index)` → the frame number.
- `surface_control(fd, op, ...)`: the palette, the title, the pointer's mode, waiting for a frame.
- Events by `read` on the descriptor; closing it ends the surface. Every struct begins with
  `uint32_t size` (`docs/ABI.md`).

## 4. The SDK's game library

A **static library in the SDK** (linked into each program, so it improves without ABI promises
and an old game keeps the version it was built with), drawing on the system calls above and the
existing ones. Built out as the games need it; nothing one game alone uses.

- **Drawing:** fills, blits with a colour key and with alpha, sprites and sprite sheets, tiles,
  lines, bitmap text (unscii, as the terminal's), palette helpers; for L8, RGB565 and RGB888. The
  inner loops in PIE (`simd.md`), measured against plain C.
- **Images:** PNG sprite sheets through `image_decode` (the system's decoder).
- **Sound:** a mixer of sound effects (voices, volume and pan, one-shots and loops) into a shared
  real-time stream, on its own thread; WAV loading. Music through the synth descriptor
  (`audio.md` §22: a MIDI file played by TinySoundFont and the GM font). Doom brings its own mixer
  (§6b) and uses only the synth.
- **A frame loop helper:** acquire, update at a fixed tick, draw, present, events; pausing when
  inactive.
- **Name:** `libbmdsgame` (`bmds/game.h`), unless the drawing half proves useful to non-games, then
  split.

## 5. Audio with the picture

- **Latency:** a sound effect started at a key press is heard 2-4 ms later at a 64-frame period
  (`audio.md` §20 measured), well inside the frame it is drawn in.
- **The synth's cost is the risk:** ~54 µs a voice a 64-frame block (`audio.md` §22, after the
  float fix), so 16 voices are ~65% of a core. Music on the second core, a voice limit per game,
  and measured with Doom's busiest tracks before promising anything.

## 6. The games

### 6a. Blocks (Tetris's rules)

- **Ours, MIT, in `programs/`,** named Blocks (the user's, 2026-10-14; "Tetris" is a
  trademark).
- **Rules:** a 10×20 field, the seven pieces from a shuffled bag, the standard rotation system
  with wall kicks, a ghost piece, hold, hard and soft drop, levels speeding up, line clear
  animations, a high-score table (`bmds_app_data_dir()`).
- **The first program on everything:** surfaces in both presentations, raw keys, sprites and
  text from the library, sound effects, music (Korobeiniki is a public-domain folk song: our own
  MIDI arrangement in the repository, played by the synth). Its size: 640×360 RGB565, scaled 2×.
- Suggests a window (it is a demo on the desktop); Alt+Enter for full screen.

### 6b. Doom

- **[doomgeneric](https://github.com/ozkl/doomgeneric)** (Chocolate Doom made portable: a
  platform supplies about six functions: init, draw a frame, sleep, ticks, keys, title). GPL-2,
  so a **`dist/` recipe** (`distribution.md`), installed as `apps/doom.app`, like BusyBox's
  sources: fetched and pinned, never in the repository.
- **The game data:** **Freedoom** (BSD-licensed: shipped in the folder), and the user's own WADs
  (`doom1.wad`, `doom.wad`, `doom2.wad`) found in the folder or the data directory.
- **The picture:** Doom draws 320×200 in 8-bit with a palette: an L8 surface of 64 KB in
  internal RAM, aspect 4:3, scaled by the PPA through its CLUT. Suggests full screen.
- **Sound effects:** Doom's own 8-channel mixing of its 11 kHz samples into a shared real-time
  stream. **Music:** Doom's MUS tracks converted to MIDI (Chocolate Doom's converter) and played
  by the synth: the GM font through TinySoundFont, a showcase of `audio.md`.
- **Fits easily:** Espressif ran Doom on the original ESP32 (240 MHz, 4 MB of PSRAM); here two
  360 MHz cores and ~27 MB of free PSRAM with the desktop up (`audio.md` §22).

### 6c. Solitaire (Klondike)

- **Ours, MIT, in `programs/`.** In a window from the start (the classic desktop game, the GUI's
  demo): cards dragged with the mouse, a double click sends one to the foundations, draw one or
  three, undo, a timer and score (Windows' rules as an option), and the bouncing cards when won
  (a blitting demo).
- **Card art:** our own, or a public-domain set (Byron Knoll's vector playing cards; the licence
  checked first); rendered to PNG sprite sheets at build time.
- RGB565 at its window's natural size (about 640×480), whole multiples for a large window.

## 7. Steps

1. **Surfaces in the kernel** (§10): the swap chain, full screen (flip and PPA scaling), a window's
   copy, Alt+Enter, events from the window manager's input, the escape keys. A demo program
   (moving sprites, a palette cycle) measures the PPA's scaling at the games' sizes, the PIE
   fallback, and tearing in a window.
2. **The system calls and the SDK's headers** (an `ABI:` commit), brought to the user before
   building.
3. **Application folders** (`apps.md`), if not done before.
4. **`libbmdsgame`:** drawing, images, the sound mixer, music, the frame loop.
5. **Blocks,** with its actions bound to keys and a pad (`controls.md`).
6. **Doom** (`dist/doom`), with music; the synth's cost measured.
7. **Solitaire.**
8. Later: an SDL backend (§9).

Gamepads and the bindings editor are `controls.md`'s steps, alongside these.

## 8. Budgets (estimates; measured as the steps land)

- **The kernel's part:** a surface's state is small (a few hundred bytes); the buffers are the
  program's. Full screen at a scaled size: one PPA job (or a few: its fixed cost) a presented
  frame; Doom at 35 frames is ~684,000 scaled pixels a frame, ~24 Mpixel/s, inside the PPA's
  measured rate. A window: the copy of its visible part a frame.
- **PSRAM bandwidth:** the scan's 166 MB/s stays; a full-screen scaled frame writes up to
  2.76 MB (165 MB/s at 60 frames: the most demanding case, measured in step 1).
- **The library:** linked only into games; no firmware cost.

## 9. Open questions

- Solitaire's card art (licence).
- **An SDL2 or SDL3 backend** on these calls, as a `dist/` package: many games and ports would
  follow. Too big for now; the calls are designed so its video, audio and events map cleanly.
- Whether a full-screen game may take the display at another mode (a real 640×480 for a retro
  look), accepting the monitor's resync.

## 10. Step 1 design: surfaces in the kernel (2026-10-14)

Kernel only (no ABI yet: step 2 brings the calls to the user). Measurements first, since they
choose between paths.

### 10a. Facts (checked 2026-10-14)

- **The scan** reads surface 0 (RGB888, PSRAM) through a circular list of up to 16 DW-GDMA
  descriptors; a double-buffered swap repoints them at a frame's start (`Scanout`). Surface 0 is
  double-buffered by default (`display/buffering`): 5.5 MB.
- **Nothing writes surface 0 with the CPU:** drawing is 2D-DMA from internal RAM (`Surface`,
  `BlitChain`), so no cache write-backs hold the scan up (`display.md` V9).
- **The PPA's SRM** (scale, rotate, mirror) takes ARGB8888, RGB888, RGB565, YUV and GRAY8 in and
  out, a block of a picture (offset, size) to an offset of another, factors in 16ths (from 1/16
  to under 16), on the 2D-DMA. **No L8 in SRM:** the palette is the blender's CLUT (L8 in,
  RGB888 out, 1:1), which IDF leaves out; BMDS drove it before (`PaletteExpander`, removed with
  the ring in `803709b0`): ~52-60 Mpixel/s, ~18,000 cycles a job.
- **The window manager** draws windows straight on surface 0 through each window's redraw
  callback, clipped to what shows, on its own task, a round at most a frame; `update(id)` asks
  for a window's redraw without blocking.

### 10b. Measurements (the spike, `display test ...` on the board)

1. **SRM's rate** at the games' sizes, to RGB888 in PSRAM: RGB565 640×360 at 2× (Blocks);
   RGB888 320×200 at 3× by 3.5625× (Doom after the CLUT); RGB565 640×480 at 1× (Solitaire in a
   window: conversion only). Cycles a job (its fixed cost) and pixels a second.
2. **The CLUT** (blend, L8 to RGB888) at 320×200 again, chained before SRM.
3. **GRAY8 through SRM:** whether it scales by nearest neighbour or interpolates. If nearest,
   indices can be scaled first and expanded after (or the reverse, whichever is cheaper).
4. **The scan under load:** underruns and skipped blocks with the PPA writing a full-screen
   frame every frame (up to 165 MB/s), with and without the CPU's PSRAM traffic.
5. **Tearing in a window:** whether starting a window's copy just behind the scan (from the
   block the DMA is on, `Scanout::Stats::dmaItem`) keeps it clean.

A path that misses the budget (a frame's 16.7 ms, leaving the PPA half of it for the window
manager and video) gets a fallback measured beside it: PIE code expanding and scaling into
internal-RAM strips copied by the 2D-DMA, as the painter's glyphs are.

### 10b'. Measured (2026-10-14, `display ppa`, the desktop running)

| Path | A frame | Rate |
|---|---|---|
| SRM 640×360 RGB565 at 2× | 30.9 ms | 29.8 Mpixel/s |
| SRM 320×200 RGB888 at 3×, 3.5625× (Doom) | 20.3 ms | 33.6 |
| SRM 640×480 RGB565 at 1× | 18.7 ms | 16.4 |
| SRM 1280×720 RGB888 at 1× | 62.1 ms | 14.8 |
| CLUT 320×200 L8 to RGB888 | 1.65 ms | 38.8 (exact: 0 of 64,000 wrong) |
| CLUT, then SRM to 960×712 (Doom) | 22.0 ms | 31.1 |
| CPU: 640×360 RGB565 doubled across, C / PIE | 10.5 / 6.6 ms | 44 / 70 |
| the same, the source cached (PIE) | 1.07 ms | 431 |
| 2D-DMA: 1280-pixel RGB565 rows, each twice, widened to RGB888 | 18.3 ms | 50.4 |
| CPU and 2D-DMA overlapped (a whole Blocks frame) | 18.8 ms | 49.1 |

The scan had no underruns through any of it.

- **The SRM interpolates** (a checkerboard at 2× comes out gray; no nearest-neighbour mode) and
  is slow: it isn't the games' scaler. It stays for pictures and video, where smoothing is wanted.
- **The CLUT is the palette path** (Doom's L8).
- **Whole frames are too many bytes:** a full 1280×720 RGB888 frame takes 18.3 ms of PSRAM writes
  however it is made, more than a frame. The CPU reading PSRAM is slow too (the source's reads
  cost 5.5 of the 6.6 ms).
- So full-screen scaling (§10c, revised) writes as few bytes as it can:
  - **Rows repeated by the scan:** the scan's DMA reads each source row as many times as the
    vertical factor (a descriptor per output line), so only the source's rows are written
    (Blocks: 1280×360, half; Doom: 960×200, a third with its 3.6× nearly a quarter).
  - **Across by the CPU,** with PIE, into strips in internal RAM, widened into PSRAM by the
    2D-DMA; the source's rows brought into internal RAM by the 2D-DMA if the CPU's reads cost
    more (measured next).
  - **Damage:** a present may say which rows changed; only those go through.

### 10b''. Rows repeated by the scan (2026-10-14, `display rows N`)

`Scanout` scans a surface shorter than the mode with a descriptor a line (`show()` switches,
rebuilding the link so the picture stays in step; a surface that stalls in its first second is
given up for the one before). On the board, 1280×360, ×240 and ×180 surfaces at 2×, 3× and 4×:
120 frames in 2 s each, no underruns, stalls or restarts; a switch takes ~62 ms (the link's
rebuild). The cost is the descriptors' re-validation: ~175,000 cycles a frame (~2.9% of a core)
against ~4,200 in block mode, ~240 cycles a descriptor in uncached stores; to cut later (cached
writes and a write-back of the range) if it matters. 46 KB of internal DMA RAM for the list.

### 10b'''. A full-screen Blocks frame on the line-mode scan (2026-10-14, `display ppa path`)

640×360 RGB565 doubled across into 1280×360 RGB888, which the scan shows at 2× (§10b''):

| Path | A frame |
|---|---|
| 2D-DMA alone: strips widened into 1280×360 (rows once) | 9.2 ms (against 18.3 with rows twice) |
| CPU (PIE, source read from PSRAM) and 2D-DMA overlapped | 12.0 ms |
| 2D-DMA fetching the source into internal RAM first, alone | 4.1 ms |
| fetch, CPU and widening overlapped | 12.1 ms |

- **A whole frame fits a 60 Hz frame** (12 ms of 16.7), at the cost of ~9 ms of the 2D-DMA and ~7
  ms of a core. The 2D-DMA is now the limit: PSRAM writes at ~150 MB/s with its write regulator
  at level 6 (`display.md` §5c: lower underran the scan once).
- **Fetching by DMA doesn't pay** here: it moves the CPU's slow reads onto the 2D-DMA, which is the
  bottleneck already. The CPU reads its source from PSRAM.
- **Damage is what makes it cheap:** most Blocks frames change a few cells; a present's changed
  rows only go through. Whole frames (a line clear's animation, a new level) stay within budget.
- **Doom** (320×200 L8, 3× across into 960×200 RGB888, the scan's 3.6× down): its frame is 64 KB
  in internal RAM, the CPU expands each index to three RGB888 pixels into strips (no 2D-DMA
  conversion), the 2D-DMA copies 576 KB (~4 ms): far inside its 35 Hz tick.

### 10c. Design

- **`Ppa`** (`bmds_drivers`): our own driver on the PPA's LL and the 2D-DMA, as `BlitChain` and
  `PaletteExpander` were: `scale()` (SRM, a block to an offset), `expand()` (the CLUT), a
  palette of 256; jobs started without blocking, finished by a done callback with a timeout
  (`docs/platforms/esp32p4.md`: the blocking mode can wait forever). One instance, numbered as
  the hardware is, shared by its users through a queue (the window manager, presenters,
  `bmds_images`, video later).
- **`ProgramSurface`** (`bmds_ui`, beside the window manager): what step 2 exposes. The swap
  chain (2 or 3 buffers, owned and accounted to the program that opened it; PSRAM, or internal
  DMA-capable RAM when small and asked), the palette, the queue of presented buffers, the
  frame counter, events, and a presentation, one of:
  - **Window:** a window-manager window whose redraw scales the newest presented buffer into
    the pieces that show (one SRM job a piece; integer factors keep pieces exact). A present
    calls `update(id)`; the buffer is released when the round's jobs end.
  - **Full screen:** the window manager hands the display over (`WindowManager::
    setFullScreen()`: no rounds drawn, the pointer hidden, keys and the mouse to the surface,
    the escape keys kept). Two cases:
    - **The display's size and RGB888:** the scan reads the program's buffers themselves
      (`Scanout` shows a given buffer at a frame's start, as a swap does): no copy at all. The
      buffers are allocated to the scan's constraints (alignment, the block division).
    - **Otherwise:** the PPA scales each presented buffer into surface 0's back buffer, and
      surface 0 is presented: no memory beyond surface 0's (the window manager draws nothing
      meanwhile). The letterbox is filled once, at the switch. A single-buffered surface 0 gets
      its second buffer for the time.
    - **Leaving** (the escape keys, Alt+Enter, the program's end): the window manager takes the
      display back and redraws everything.
- **The demo** (`display test surface ...`, kernel-side until step 2): a moving sprite field and
  a palette cycle, at each size and format, windowed and full screen, with Alt+Enter; it reports
  presents, frames, PPA time and the scan's counts.
- **What it may change on the way** (the user's: pull out all the stops): the window manager's
  rounds and copies, the 2D-DMA paths' fixed costs (driving the channels directly, `display.md`
  §7), the scan's descriptors.

## 11. Step 2: the system calls (proposal, 2026-10-14; for the user's review)

Five calls, appended at slots 117-121, in `sdk/include/bmds/sys.h`. Every struct begins with
`uint32_t size`; unknown flag bits are refused (`docs/ABI.md`). A surface is a descriptor:
`read` gives its events, `io_poll` says when there are some, `close` ends it.

```c
/* Formats (bmds_surface_config_t::format): the SDK's BMDS_PIXEL_* (the image calls'), with
 * BMDS_PIXEL_L8 (5) appended: L8, RGB565 (3) or BGR24 (0, "RGB888" in this proposal). */

/* bmds_surface_config_t::flags */
#define BMDS_SURFACE_FULL_SCREEN 0x1u /* suggest full screen (else a window); the user's setting wins */
#define BMDS_SURFACE_WHOLE       0x2u /* scale by whole multiples only, both ways */
#define BMDS_SURFACE_FAST        0x4u /* buffers in internal RAM (small ones; else BMDS_ENOMEM) */

typedef struct bmds_surface_config_t {
        uint32_t    size;        /* sizeof(bmds_surface_config_t) */
        uint32_t    width;       /* pixels; a multiple of 4 */
        uint32_t    height;      /* pixels */
        uint32_t    format;      /* BMDS_PIXEL_... */
        uint32_t    buffers;     /* 2 or 3 (3: a newer present replaces a pending one) */
        uint32_t    flags;       /* BMDS_SURFACE_... */
        uint32_t    aspect_x;    /* the picture's shape, e.g. 4 and 3 for Doom's 320x200 (0, 0: */
        uint32_t    aspect_y;    /*   square pixels) */
        const char *title;       /* the window's title (copied), or NULL: the program's name */
} bmds_surface_config_t;

typedef struct bmds_surface_buffer_t {
        uint32_t size;           /* sizeof(bmds_surface_buffer_t) */
        uint32_t index;          /* which buffer */
        void    *pixels;         /* its first pixel, in the program's memory */
        uint32_t stride;         /* bytes from a row to the next */
} bmds_surface_buffer_t;

typedef struct bmds_rect_t {
        int32_t  x, y;
        uint32_t width, height;
} bmds_rect_t;

typedef struct bmds_surface_present_t {
        uint32_t           size;         /* sizeof(bmds_surface_present_t) */
        uint32_t           index;        /* the buffer drawn */
        const bmds_rect_t *damage;       /* what differs from the last present (any buffer's)... */
        uint32_t           damage_count; /* ...0: all of it */
} bmds_surface_present_t;

/* 117: a new surface; its buffers allocated (the program's memory, freed with it). */
int32_t bmds_surface_open(const bmds_surface_config_t *config);
/* 118: where buffer info->index is (fills info). */
int32_t bmds_surface_buffer(int32_t fd, bmds_surface_buffer_t *info);
/* 119: a free buffer's index, waiting up to timeout_ms (negative: forever; 0: only checks,
 *      BMDS_EAGAIN). */
int32_t bmds_surface_acquire(int32_t fd, int32_t timeout_ms);
/* 120: shows a drawn buffer (the kernel writes the cache back); the frame it will be shown in. */
int32_t bmds_surface_present(int32_t fd, const bmds_surface_present_t *present);
/* 121: everything else, by op (below). */
int32_t bmds_surface_control(int32_t fd, uint32_t op, void *arg, uint32_t arg_size);
```

**`surface_control`'s ops** (append-only, like the slots):

| Op | `arg` | What |
|---|---|---|
| `BMDS_SURFACE_PALETTE` | `bmds_surface_palette_t` (size, first, count, `const uint32_t *rgb` 0xRRGGBB) | L8's palette, from the next present on |
| `BMDS_SURFACE_TITLE` | `const char *` | the window's title |
| `BMDS_SURFACE_POINTER` | `uint32_t`: `_NORMAL`, `_HIDDEN`, `_RELATIVE` (hidden, held, motion as deltas) | the pointer over it |
| `BMDS_SURFACE_WAIT` | `uint32_t` frame number | waits until that frame is shown (with `io_poll`'s timeout rules, a second word) |
| `BMDS_SURFACE_INFO` | `bmds_surface_info_t` | its state: window or full screen, the scale and area on the screen, active, frames shown, presents dropped (mailbox) |

**Events: fixed 32-byte records**, forever (new kinds take new `type`s; a program skips types
it doesn't know):

```c
typedef struct bmds_surface_event_t {
        uint32_t type;       /* BMDS_EVENT_... */
        uint32_t flags;      /* per type: a key's DOWN, UP, REPEAT... */
        int64_t  time_ns;    /* BMDS_CLOCK_MONOTONIC, when it happened */
        uint32_t data[4];    /* per type, below */
} bmds_surface_event_t;
```

| Type | `data` |
|---|---|
| `KEY` | HID extended usage ((page << 16) \| usage, `hotkeys.md` §3.1), modifiers, the layout's character (0: none); flags `DOWN`/`UP`/`REPEAT` |
| `POINTER` | x, y (the surface's pixels; may be outside it while a button is held), buttons, dx and dy (int16 each, packed: relative mode) |
| `WHEEL` | notches (int32, positive away from the user) |
| `ACTIVE` | 1 active, 0 inactive (focus lost, minimized, left full screen: pause) |
| `PRESENTED` | window 0 / full screen 1, the scale (16ths across, 16ths down), the area's size on screen |
| `CLOSE` | (none): the user asked it to close (close button, Alt+F4); the program decides |
| `ACTION` | reserved for `controls.md` (action, value, device, pad) |
| `PAD` | reserved for `controls.md` (pad, control, value) |

**What the system does with it** (not the ABI, for the review): the window or full screen is the
per-application setting (`apps.md` §6) with the flag as the default; Alt+Enter toggles. Full
screen: across by whole multiples, down by rows the scan repeats to keep `aspect` (nearest rows),
letterboxed, the pointer hidden unless asked; in a window: whole multiples both ways (1× to begin,
the window resized in steps). Only damaged rows go through.

**Decided (the user's, 2026-10-14):** a surface is its window (or full screen) from
`surface_open`; events on the surface's own descriptor; `ACTION` and `PAD` reserved now, delivered
with `controls.md`. The proposal stands as written.

## 12. As built (2026-10-14): step 1 and step 2's calls

- **The ABI** (an `ABI:` commit): slots 117-121 as §11, with the formats the image calls' own
  (`BMDS_PIXEL_BGR24` 0, `BMDS_PIXEL_RGB565` 3, `BMDS_PIXEL_L8` 5 appended), locked in
  `abilock.c` (pointer-bearing structs on 32-bit builds only, as the others).
- **`ProgramSurface`** (`bmds_ui`): the swap chain (2 or 3 buffers, PSRAM or with `FAST` internal
  RAM), events as 32-byte records with `poll`, a window (whole multiples, the window manager's
  resize steps, the picture centered and the pointer mapped back), full screen (§10c: a
  1280-wide scan surface of the picture's rows, the scan repeating them; the window manager hands
  the pointer over in screen coordinates and asks it to leave before an escape key), Alt+Enter
  both ways, the program's `FULL_SCREEN` suggestion. A presenter task (core 1, priority 4)
  switches and converts full-screen presents; frames are the scanout's.
- **The window manager:** `WindowConfig::fullScreen`, `setFullScreen()`, Alt+Enter; Alt+Tab,
  Alt+F4, Alt+Space and the Windows keys first ask a full-screen window to leave.
- **Surfaces:** RGB565 sources widened by the 2D-DMA (`Surface::Source`, `Painter::pixels565()`);
  **lazy copy-forward** for double buffering: a present's changes are copied into the next
  buffer only where the next drawing reads them or covers them in part, at present at the
  latest; covered whole, never (`Surface::willDraw()`, `Painter::cover()`): a window redrawn every
  frame isn't copied twice. The desktop's own drawing gains the same.
- **`Pixels`:** `repeat16` (PIE for 2: `simd test repeat16`), `repeat24`, `expandIndexed`, host-tested.
- **The demo:** `sdk/examples/surfdemo` (formats, sizes, buffers, `-F`, `-d` damage; Escape or
  close ends it).
- **Damage** (step 2): a present's rectangles are what differs from the last present, whichever
  buffer it was (§11's first wording, "since this buffer was last presented", was the program's
  buffer age: the system can't rebuild the change from it). They are widened to 8-pixel columns
  and merged where they meet (16 kept, more: their bounds). A window invalidates only them,
  scaled; its redraw expands only the rows and columns its clip reaches. Full screen adds them to
  what the presenter hasn't converted yet (a skipped present's included), and converts only those,
  the scan surface's lazy copy-forward bringing the rest. A palette change makes the next present
  whole. The program still keeps each buffer whole: an uncovered window is redrawn from it.

Measured with `surfdemo` (the demo drawing every pixel each frame, overlapped with the display):

| Case | Presents a second | The window manager's redraw |
|---|---|---|
| window, 640×360 RGB565 at 1× | 57 | 7.5 ms |
| window, 640×360 BGR24 at 1× | 42 | (the program writes 690 KB a frame) |
| window, 320×180 RGB565 at 2× | 60 | 16 ms (45 strips of 145 µs) |
| window, 320×180 L8 at 2× | 60 | |
| full screen, 640×360 RGB565 at 2× | 39 | (the presenter: ~12 ms a frame) |

With damage (`surfdemo -d`: 12 squares of 24 pixels moved, the rest kept):

| Case | Presents a second (whole pictures) |
|---|---|
| window, 640×360 RGB565 at 1× | 59 (55) |
| window, 640×360 BGR24 at 1× | 57 (42) |
| window, 320×180 RGB565 at 2× | 58 (58) |
| full screen, 640×360 RGB565 at 2× | 60 (37) |
| full screen, 640×360 L8 at 2× | 59 |
| full screen, 320×180 RGB565 at 4× | 59 |

- Before lazy forwarding a 2× window ran at 30: half-tile strips copied each tile forward
  before overwriting it (670 µs a strip against 145).
- Full screen's 39: the presenter's conversion and the program's drawing share core 1 at times.
- Not yet: the pointer modes (hidden and relative), the
  per-application setting (with `apps.md`), a close the program can refuse (the window goes at
  once; the program gets `CLOSE`).
