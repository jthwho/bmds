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

- ~~Solitaire's card art (licence).~~ Our own, drawn at build time (§21).
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
rebuild; since 2026-10-16 ~15 ms with the link kept up, `display.md` §5f). The cost is the
descriptors' re-validation: ~175,000 cycles a frame (~2.9% of a core) against ~4,200 in block
mode, ~240 cycles a descriptor in uncached stores; to cut later (cached writes and a write-back
of the range) if it matters. 46 KB of internal DMA RAM for the list.

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
    the escape keys kept; since 2026-10-15 none but Alt+Enter: `windows.md` §4h). Two cases:
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
| `POINTER` | x, y (the surface's pixels; may be outside it while a button is held), buttons, dx and dy (int16 each, packed: the mouse's motion, in every mode) |
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
- **Pointer modes** (step 2): `HIDDEN` sets the window's pointer shape to the window manager's
  new `None`; `RELATIVE` is the window manager's pointer lock (`windows.md` §4h): while the
  surface is focused (in a window or full screen) the pointer is hidden and held, and every
  motion comes with its deltas; focus elsewhere shows it again. Every `POINTER` event carries
  `dx | dy << 16` (the mouse's own motion in screen pixels). `surfdemo -p relative` steers its
  first square with the mouse.
- Not yet: the
  per-application setting (with `apps.md`), a close the program can refuse (the window goes at
  once; the program gets `CLOSE`).

## 13. Step 4 design: `libbmdsgame` (2026-10-14)

A static library in the SDK (`sdk/game/`, `bmds/game.h`), linked into the programs that use it
(`--gc-sections` drops the rest): no system calls of its own and no ABI promise. A game built
against one version keeps it. C99, `bmds_` names, as the SDK's headers. Built in three parts,
each tested before the next; Blocks (step 5) is its first user.

**Part 1: the canvas, drawing, text and the frame loop**

- **`bmds_canvas_t`:** pixels, stride, width, height, format (`BMDS_PIXEL_L8`, `RGB565`, `BGR24`),
  a clip rectangle, and its damage (below). `bmds_canvas_pixel(c, 0xRRGGBB)` gives a format's
  pixel value once (L8: the value is the index), so drawing calls take pixel values and convert
  nothing per call.
- **Drawing:** fill, outline rectangle, horizontal and vertical lines, any line (Bresenham), a
  pixel; images (`bmds_game_image_t`: an image's own pixels in the canvas's format, optionally a
  transparent key) blitted whole or a source rectangle of them (sprite sheets, tiles), with or
  without the key. Everything clipped. Plain C first; PIE inner loops where measurement shows a
  need (fills and keyed blits are the candidates).
- **Text:** unscii's 8×16 glyphs (as the terminal's), ASCII and Latin-1 (UTF-8 in, `?` for the
  rest), drawn in one colour (transparent background) at a whole scale; `bmds_canvas_text_width()`.
  The glyphs (3 KB) were generated from `thirdparty/unscii/unscii-16.hex` by `tools/gamefont`.
  Since 2026-10-26 (`fonts.md` §6e) text is the system's fonts, drawn by the OS (`text_draw`):
  `bmds_canvas_text(canvas, font, ...)`, unscii 16 by default, `bmds_game_font()` for any other,
  `bmds_game_font_scaled()` for the old scales, `bmds_canvas_text_flags()` for a box or a
  smoothing of its own; the copy of unscii is gone.
- **Damage, kept by the canvas:** every drawing call adds what it changed (merged rectangles, 16
  kept, past that their bounds). The frame loop presents it, so a program that draws only what
  changes gets the cheap presents of §12 without saying anything; one that redraws everything
  (or calls `bmds_canvas_damage_all()`) presents it all.
- **Buffer age, handled by the loop:** the buffer a frame is drawn in was last shown two (or three)
  presents ago. Before handing it over, the loop copies into it what the presents since then
  changed, from the newest buffer, so a frame always starts as the last one shown; the program
  draws only its changes. The cost is the last frames' damage, not the picture.
- **The frame loop (`bmds_game_run()`):** opens the surface (`bmds_game_config_t`: size, format,
  buffers, flags, aspect, title, tick rate), then until the program quits: reads the events (to the
  program's `event` callback), runs `tick` at a fixed rate (default 60 a second, catching up at
  most a few ticks after a stall), calls `draw` with the canvas when ticks ran or it asked
  (`bmds_game_redraw()`), presents its damage and paces itself on the display (the previous
  present's frame, as `surfdemo`). Not active (`ACTIVE` 0): no ticks, no frames, the loop
  sleeps in `poll()` until an event; a `PRESENTED` event (scale, full screen) redraws all. The
  close event and Escape are the program's (its `event` decides; `bmds_game_quit()`).

**Part 2: images.** PNG (and the system's other formats) through `image_decode` into an image in
the canvas's format; a transparent key or the PNG's alpha (an alpha below one half: the key).
Alpha blending later, if a game needs it. Built: §15.

**Part 3: sound.** A mixer on a real-time client (`audio_cycle`, its own thread): voices with
volume and pan, one-shots and loops, WAV samples loaded whole (16-bit PCM, any rate: resampled at
load). Music: a file played by `audio_decode` (MIDI through the synth, or MP3, Ogg, FLAC), read
into the mixer's stream on its thread, looping. The music's cost (the synth's) is measured with
Blocks' tune. Built (§16) with the music on its own thread and stream instead.

**Testing:** the drawing, text, damage and buffer-age code is pure C and runs in the host tests
against a fake surface; the loop and the sound on the board with a demo.

## 14. As built: step 4 part 1, the canvas and the frame loop (2026-10-14)

- **`libbmdsgame.a`** (`sdk/game/`, `bmds/game.h`): built with the SDK's libraries (`bmds_libs`),
  in every program's link group and `bmds-cc`'s (an archive: unused, nothing is linked).
  `canvas.c` (fills, outlines, lines, plots, blits with a key and a source rectangle, copies,
  damage), `text.c` (the system's fonts through `text_draw` since `fonts.md` §6e; it had its own unscii
  8×16, `font16.c`), `loop.c` (`bmds_game_run()`). Plain C; no PIE yet (not needed so far).
- **Damage:** each drawing call adds what it changed. A rectangle merges with one it meets only
  when their bounds cover at most a quarter more than the two (merging on any touch chained, one
  rectangle ending up around everything: 64 squares became 4 rectangles over half the picture);
  past 16, the pair whose bounds add the least merge, left overlapping others. The kernel's
  `ProgramSurface::Damage` does the same now (it collapsed to its bounds when full).
- **The loop:** a buffer is brought up to the last frame shown from the newest buffer (the
  presents since its own, their damage kept for 4; further behind, all of it), the program draws,
  and only its damage is presented, after the last present's frame has begun. A frame that drew
  nothing presents nothing (the buffer is kept for the next). Ticks at a fixed rate (default 60),
  up to 5 caught up after a stall (more dropped); while inactive, no ticks or frames, the loop
  sleeps in `poll()`.
- **Host-tested:** formats, clipping, outlines, lines, keyed and clipped blits, copies, damage
  merging (spread changes stay small), glyphs, widths, Latin-1 and `?`.
- **`bounce`** (`sdk/examples/bounce`): squares erased and drawn where they move, the rate in text
  once a second. On the board:

| Case | Frames a second |
|---|---|
| window, 320×180 at 2×, 12 squares | 60 |
| window, 640×360 at 1×, 64 squares | 59 (43 with the old merging) |
| full screen, 640×360, 12 squares | 60 |
| full screen, 640×360, 64 squares | 46 |
| full screen, 320×180, 64 squares | 60 |

- **Found and fixed: a crash entering full screen.** `Scanout::switchSurface()` changed the
  descriptors' plan (`layOut()`: 720 items, line mode) and the surface before the old link came
  down, then allocated the new list; an interrupt in between walked the old one-chunk list with the
  new count and stored into a chunk it doesn't have (a store fault in `validate()`, the first full
  screen after a flash, about one time in three). The plan (`planFor()`) and the surface are now
  taken in `rebuild()` after `stopHardware()`; `fallBack()` the same.
- **Open: fewer line-mode interrupts.** One interrupt every line (as committed: about 4% of a
  core) is kept. With one in 45 (the design's "about 16 a frame", restored and measured), frames
  went on at 60 a second but swaps stopped: no interrupt saw the DMA on the last line, where a swap
  begins. Why isn't known (the last two lines interrupt either way).
- **Next:** part 2 (images), part 3 (sound), then Blocks.


### 14a. Repainting games, and faster turned and scaled blits (2026-10-16, `luckyduck.md` §23)

- **`bmds_game_config_t::options`:** `BMDS_GAME_REPAINTS` is appended to the config (a static
  library, so not an ABI change).
  - With it, the loop doesn't bring the buffer up to date before `draw`. The draw declares each
    rectangle it is about to repaint whole (`bmds_game_repainted()`), or calls
    `bmds_game_up_to_date()` before drawing anything else.
  - After the draw, the loop copies what the last presents changed, but not where the draw
    repainted (`bmds_game_copy_except()`, in row bands, so it needs no memory).
  - With double buffers and smooth motion, most of the last frame's change is drawn again, so it
    no longer gets copied first.
  - It can't go wrong silently: past `BMDS_GAME_REPAINTED_MAX` (32) rectangles a frame, the call
    brings the buffer up to date itself.
- **Turned blits** work out each row's span inside the source once (no pixel is tested), step in
  32 bits (the core's width), and have an RGB565 path. **Scaled blits** gained the same RGB565
  path. Host-tested against the old pixel-at-a-time code on random turns, sizes, mirrors and
  clips.

## 15. As built: step 4 part 2, images (2026-10-14)

- **`bmds_game_image_load(path, how, image)`**, **`bmds_game_image_load_memory(data, length, how,
  image)`** (a picture built into the program), **`bmds_game_image_free()`** (`image.c`). A file
  is read into memory once (`image_info` and `image_decode` each read an fd to its end), decoded
  as BGRA32 into a `bmds_mem_alloc()` buffer (`how->mem_flags`: `BMDS_MEM_FAST` for internal RAM),
  converted in place row by row (the output is never wider than its input), then cut to size.
  EXIF orientation is undone. Any format `image_decode` knows: PNG, JPEG, BMP.
- **`bmds_game_load_t`:** the canvas's format; `BMDS_GAME_LOAD_KEY_RGB` with `key_rgb` (a colour
  in the file that is transparent: BMP sprites); L8's palette (each pixel the nearest colour,
  weighted 3:4:2, the last lookup cached) and `key`, the index transparent pixels become (no colour
  is mapped to it).
- **Transparency:** an alpha below 128 (or the key colour) is transparent. If any pixel is, the
  image comes keyed. RGB565 and BGR24 use magenta (0xF81F, 0xFF00FF) as the key; an opaque pixel
  that would equal it is moved a step of green (0xF83F, 0xFF01FF), so the game never chooses a
  key. Alpha blending: still later, if a game needs it.
- **Host-tested** (`convert.c`, no system calls): RGB565 and BGR24 in place, alpha and a key
  colour, an opaque magenta moved off the key, L8's nearest colour never the key, refused requests.
- **`bounce -i PICTURE`** draws each square as the picture; L8 gets a palette (a 6×6×6 cube), so
  the L8 path is exercised too. A 24×24 PNG with soft edges, 32 of them at 640×360: 58-60 frames a
  second in RGB565, BGR24 and L8, windowed or full screen.
- **Found and fixed: L8 (and scaled) windows were slow.** A window whose picture is expanded by
  the CPU (L8, or scaled) converted the bounds of all the clip into the strip, 8 rows a band, and
  each band was written once a clip rectangle: 32 spread squares in L8 drew at 2-7 frames a second.
  `ProgramSurface::drawScaled()` now converts and writes each clip rectangle on its own
  (`Painter::clipCount()`, `clipAt()`): 58-60. Dense changes at 2× (32 squares at 320×180) draw
  at 45-47, as RGB565.
- **Seen, open:** a `screenshot` taken while a program is full screen shows the desktop's
  buffer, which holds what the full-screen picture left there (RGB565 read as BGR24: noise). The
  screen and the desktop after it are right; the capture should take what is shown.

## 16. As built: step 4 part 3, sound (2026-10-14)

- **Effects (`mix.c`, `audio.c`):** `bmds_game_audio_open()` opens `/dev/dsp` as a real-time
  client (F32, stereo, a 256-frame period: 5.3 ms at 48 kHz) and starts a thread that runs the
  mixer each `audio_cycle`. `bmds_game_play(sound, volume, pan, flags)` returns at once: the game's
  thread queues a command (64 kept between periods) and hands out the voice itself, 16 slots
  numbered with a generation, so a voice that ended or was taken is never touched by a late
  command. The mixer says which generation of each slot ended; with every slot busy, the oldest
  voice that doesn't loop is taken (all looping: `-BMDS_EBUSY`). Each voice's gains move to their
  target (volume × pan × the effects' volume) over a period, so changes and stops don't click; a
  stopped voice fades over one period and ends. Pause: silence, nothing moves.
- **Sounds:** `bmds_game_sound_load()` decodes any file `audio_decode` knows (WAV, MP3, Ogg
  Vorbis, FLAC; MIDI through the synth too) whole as 16-bit samples, 1 or 2 channels, resampled
  linearly to the device's rate at load (10 minutes at most), so the mixer only adds.
- **Music, changed from §13:** not through the mixer. `bmds_game_music_play()` opens the decoder
  (and reports its errors) on the game's thread, then a thread of its own reads it and writes to a
  second, ordinary stream (`S16LE` at the file's rate, a 250 ms queue): the system resamples it
  with its own resampler and mixes it, the decoding (the synth's, for MIDI) stays off the
  real-time thread, and the blocking writes pace it, with no ring or wakeups of the library's.
  Looping seeks the decoder to 0. Its volume scales the samples; pause is the stream's
  `BMDS_AUDIO_PAUSE`; stop drops the queue (a waiting write returns) and joins.
- **Host-tested:** voices panned, mono and stereo out, ends, loops, a stop's fade, stealing the
  oldest, refusing when all loop, pause, a full queue, resampling (lengths, interpolation, the
  last frame held).
- **`bounce -a SOUND -m MUSIC`:** a sound each time a square meets a wall, panned to where it is,
  music looping; paused while the window isn't focused (the `ACTIVE` event). On the board, 32
  squares and a 22.05 kHz WAV (resampled at load to 48 kHz): 60 frames a second, 0 late periods
  and 0 music underruns in a run; one late period seen once, as the focus changed. `top -T`
  (share of the CPU, 2 s): the mixer thread 4.5-7%, the music thread 12.5% for a MIDI tune
  (the synth) and 22.5% for an MP3, the kernel's audio task 6-20% (the MP3 at 44.1 kHz is
  resampled there).
- **Next:** Blocks (step 5), the library's first user.

## 17. Step 5 design: Blocks (2026-10-14)

`programs/blocks/`, MIT, installed by the `dist/blocks` recipe as `apps/blocks.app`. The first
user of all of `libbmdsgame`. Keyboard only until `controls.md` lands (its actions then declared
and bound there; a pad with them).

- **Rules (`rules.c`, pure C, host-tested):** the guideline's. A 10×20 field (two hidden rows
  above), the seven pieces from a shuffled bag of seven, SRS rotation with its wall kicks (the I's
  own table), spawning at columns 3-6 and dropping a row at once. Gravity by level (the
  guideline's (0.8 - (level - 1) × 0.007)^(level - 1) seconds a row, levels every 10 lines), soft
  drop 20× (1 point a row), hard drop (2 a row), a 0.5 s lock delay reset by a move or turn (15
  times, again on a new lowest row), hold once a piece, five next pieces, a ghost. Clears score
  100, 300, 500, 800 × the level, after a 0.4 s flash. Auto-repeat is the game's, from key downs
  and ups (170 ms, then every 33 ms), so a tick's input is deterministic and testable. Game over:
  a piece that can't spawn, or locked wholly above the field. T-spins and back-to-back: later.
- **The picture:** 640×360 RGB565, 16-pixel cells: the field in the middle, hold and the score on
  the left, the next five on the right, our 8×16 text. Cells are tiles of a PNG sheet built with
  the folder (bevelled colours, an empty cell, a flash, ghost outlines with transparency: the
  keyed blit). Each frame draws only the cells and figures that changed from the last, so most
  frames present a few rectangles.
- **Sound:** effects (move, turn, lock, hard drop, hold, clears 1-3 and 4, level, game over) as
  WAVs built with the folder; Korobeiniki (a public-domain folk song: our own arrangement, lead,
  bass and drums) as a MIDI file played by the synth, looping. M turns the music off and on.
- **Screens:** the title (the high scores, Enter to start), playing, paused (Escape or P: the
  field hidden), game over (a name typed in for a high score). Inactive: paused. Q quits from the
  title, pause or game over; closing the window quits.
- **High scores:** the top ten (score, level, lines, name) in `scores` in the data folder
  (`bmds_app_data_dir()`), a line each.
- **Assets:** `programs/blocks/assets.py` (Python, zlib only) writes `tiles.png`, the WAVs and
  `music.mid` into the folder at build time; nothing binary in the repository.
- **Keys:** Left and Right move, Down or Space drop all the way (the user's, 2026-10-14: soft drop
  stays in the rules, unbound), Up or X turn right, Z or Ctrl
  turn left, C or Shift hold, Escape or P pause, M music, Enter start, Q quit.

## 18. As built: step 5, Blocks (2026-10-14)

- **`programs/blocks/`:** `rules.c` (the rules, §17, pure C), `blocks.c` (the program),
  `assets.py` (the tiles, the effects and the music, written into the folder at build time),
  `app.desktop` (`X-BMDS-Display=Window`, `Icon=grid`). **`dist/blocks`** builds
  `apps/blocks.app`; installed on the board, it shows in the launcher and as `blocks` on the PATH.
- **Host-tested** (`tests/host/blocks.cpp`): the bag (each piece once in seven), turns and SRS
  kicks (an I kicked off the wall), the O not turning, gravity (level 1: a row in 60 ticks; the
  rate is rounded up, as truncated it took 61), soft drop and its points, the lock delay and its
  15 resets, auto-repeat, a four-line clear with its score, level and the rows above moved down,
  hold once a piece, the game's end.
- **On the board:** the title with the high scores, a game played from `win hid` keys (moves,
  turns, hold, hard drops, the ghost, the next five), pause, Alt+Enter to full screen and back,
  game over with a name typed in, saved to `~/.local/share/blocks/scores` and shown on the title.
  Effects and Korobeiniki played with 0 late periods and 0 underruns.
- **Cost while playing** (`top -T`): the game's thread 0.9%, the mixer's 0.8%, the music's 8%
  (the synth); on the title, under 1% in all. The first draft found each cell's ghost by dropping
  the piece again for each of the 200 cells, every tick; the cells are now worked out once a
  frame, then only the changed ones drawn.
- **Not yet:** the line-clear flash seen on the board (the rules are tested; a scripted clear
  needs a fixed bag), a starting level, T-spins and back-to-back, the pad and rebinding
  (`controls.md`), and the application's own icon (a theme's `grid` for now).

### 18a. After the user's first game (2026-10-14)

- **Faster at the start:** gravity is the guideline's a level on (level 1 falls a row in 0.79 s,
  not 1 s; level 20 in 0.24 ms, past a row a tick anyway).
- **Down drops all the way**, as Space does (the user's).
- **Clears:** the full rows blink three times (0.1 s lit, 0.1 s not), are emptied, then the rows
  above drop onto them a row at a time (the lowest first, 83 ms apart, a soft thud each), then the
  next piece comes. In the rules (`clearing_tick()`, `blocks_flashing()`), host-tested.
- **Paused while away, shown:** losing the focus paused the game, but the loop draws nothing while
  inactive, so the PAUSED box never showed and the game looked frozen until a key (Escape)
  resumed it. `bmds_game_redraw()` now draws a frame while inactive, and Blocks asks for one.
- **Open: a stall reported by the user** (the falling stopping after moves, and after a clear's
  flash, until a key). Not reproduced with `win hid` keys in long sessions, and no wait in the
  loop, the surface's buffers, its events or the window manager's wakes is unbounded or misses a
  wake on reading. Seen instead, twice: a 16 ms `poll()` returning after ~240 ms. A build that
  logs slow waits, the events and each second's ticks, frames and presents to
  `/tmp/blocks-trace.txt` was put on the board to catch it. The user's next session (7.7 minutes,
  with the fixes above) had no stall, no focus change and steady ticks (60 a second); one more
  `poll()` came back ~210 ms late. The tracing build was then replaced. If the stall comes back,
  rebuild that build from this section's description. The late `poll()` is still unexplained.

## 19. Step 6 design: Doom (2026-10-14)

Two recipes, nothing of Doom's or Freedoom's in the repository:

- **`dist/doom`:** doomgeneric (GPL-2), pinned to a commit (its GitHub archive and sha256: it
  has no releases), with our platform layer, `programs/doom/` (MIT, ours; the linked program is
  GPL-2 as a whole and ships with doomgeneric's licence). Built with `$CC` as `apps/doom.app/doom`
  with `app.desktop` (`X-BMDS-Display=FullScreen`). Patches: only what can't be done from the
  platform layer (the SDL_mixer include in `i_sound.c`; `I_WaitVBL()`, empty, so the quit sound
  plays out; the screen from the platform, `i_video.c`; tics run once made, `d_loop.c`: §20a).
- **`dist/freedoom`:** Freedoom 0.13.0 (BSD), `freedoom1.wad` and `freedoom2.wad` (28.8 MB
  each) into the same folder. Its release is a zip: `bmds-dist` learns to unpack one.

**The platform layer** (doomgeneric's six `DG_` calls, and its sound and music modules):

- **The picture:** doomgeneric built for 8 bits (`CMAP256`) at 320×200, so `I_FinishUpdate()`
  copies Doom's screen straight into ours: an L8 surface, 320×200, three buffers in PSRAM, aspect
  4:3, full screen suggested. Doom draws its screen in internal RAM (`DG_VideoBuffer()`, §20);
  `I_FinishUpdate()` copies it into the buffer we hold acquired (`DG_ScreenBuffer` points at it),
  `DG_DrawFrame()` presents it whole and acquires the next. Palette changes (the damage and pickup
  flashes, the gamma) go to the surface before the present that shows them.
- **Time:** Doom runs at 35 tics a second and every wait in it (`TryRunTics()`, the wipe) is a
  loop of `I_Sleep(1)` until the tic count changes. `DG_SleepMs()` therefore sleeps at least
  what it is asked and on to Doom's next tic (its clock's start is ours: our first
  `DG_GetTicksMs()`), waiting on the surface's events meanwhile: a wake a tic, no 1 ms polling.
- **Keys:** the surface's key events, by HID usage, into Doom's codes (arrows, Ctrl fire, Space
  use, Alt strafe, Shift run, `,` `.` strafe, Tab map, Escape menu, Enter, F1-F12, digits,
  letters, `-` `=`, Backspace, Pause) in a queue `DG_GetKey()` empties. Losing the focus lets go
  of every key and button held, and pauses a game being played (Doom's own pause, its PAUSE
  shown), resumed on the way back if it was ours.
- **The mouse:** relative while playing a level (not in the menu, paused or a demo), hidden
  otherwise. Its motion is summed and given to Doom once a tic (Doom keeps only the last motion
  event of a tic); buttons as Doom's (fire, strafe, forward). Up and down don't walk (`novert`
  on by default; the setting is Doom's own).
- **Closing the window** (Alt+F4) quits as the menu's quit does, the settings saved.
- **Files:** the data folder (`bmds_app_data_dir()`) is Doom's current directory: its settings
  (`default.cfg`, `doomgeneric.cfg`), saves and screenshots. The game data: the first of Doom's
  own list (`doom2.wad`, `plutonia.wad`, `tnt.wad`, `doom.wad`, `doom1.wad`, ..., `freedoom2.wad`,
  `freedoom1.wad`) found in the data folder, then in the application folder, given as `-iwad`
  unless the command line names one. The user's own WADs go in the data folder.
- **Memory:** Doom's zone its default 6 MB, from PSRAM (§20: 8 MB didn't always find room).

**Sound effects through `libbmdsgame`'s mixer**, not Doom's own (the plan in §6b): its sound
module maps directly onto voices (a channel is a voice; volume 0-127 and separation 0-254 are
volume and pan). A lump (DMX: 8-bit unsigned at 11025-44100 Hz, 16 bytes padding each end) is
wrapped as a WAV in memory and loaded with `bmds_game_sound_load_memory()` the first time it is
played. Resampled to 48 kHz, Freedoom 2's 109 sounds would take ~12 MB, so they are kept up to a
budget (4 MB), the least recently played not playing freed first.

**Music through the synth:** `RegisterSong()` takes a MIDI lump as it is (Freedoom's all are)
or converts a MUS one (id's WADs) with doomgeneric's `mus2mid`; `PlaySong()` writes it to a
file in `/tmp` and plays it with `bmds_game_music_play()`, looping. Doom's music volume is the
music's; its pause mutes it, as Chocolate Doom's does. Measured: the synth's cost with the
busiest tracks, a song's start, and sound memory after a level.

## 20. Doom on the board: cost, and full screen in the monitor's own mode (2026-10-14)

**Measured** (`prof`, 10 s, MAP01 standing; percent of one core):

- First build: Doom's process ~74% (rendering ~30%, the synth ~10%, a 64 KB `memcpy` of Doom's
  screen into the surface's buffer 7%); the kernel's conversion task 45-60%, nearly all
  `Pixels::expandIndexed()` (L8 to RGB888, 3× across) at ~12 cycles an output pixel, byte stores
  in a loop over the factor. The system saturated: the synth's thread, an ordinary one, starved,
  and the user heard the music break up in full screen.
- **Fixes:** the music's thread is real-time at the lowest priority (`SCHED_FIFO` 1: above games
  and the display's work; 0 underruns since, its queue full); `expandIndexed()` packs four pixels
  into 3 × factor words (factors 1-4; host-tested against the byte loop at every alignment):
  ~7 cycles a pixel; Doom draws into internal RAM (a patch lets the platform give its screen),
  the surface's buffers in PSRAM. After: the conversion ~25% of a core, Doom's process ~80% with
  the music, idle ~40% of the two cores.
- **What stays:** a CPU copy into PSRAM costs a line miss at a time (~2 ms for Doom's 64 KB);
  the conversion is bound by memory, not arithmetic (word stores gained a third, not three
  times).
- Doom's zone is its default 6 MB: PSRAM is in pieces (22 MB free, the largest 8 MB), so 8 MB
  failed beside the synth's font (~6 MB in one piece, loaded before the zone is taken).

**Design: the monitor's own mode for a picture that fills one.** Scaling 320×200 to 720p costs
192,000 converted pixels a frame, and 35 frames a second can't be paced evenly into 60. VGA's
mode 13h was 320×200 doubled to 640×400 at 70 Hz: Doom's 35 frames are exactly every other
refresh, and the conversion is 128,000 pixels (2× across; rows repeated by the scan).

- **When:** entering full screen, a mode the picture fills exactly by whole factors both ways at
  its own shape (its aspect, or square pixels), the mode shown at 4:3 (VGA's modes) or with
  square pixels (the desktop's), which the monitor takes: listed in its EDID (established,
  standard, CEA or detailed) or inside its range limits (vertical and line rates, pixel clock).
  The candidates: 640×400 at 70 Hz (mode 13h's timing, 25.175 MHz), 640×480 at 60 Hz, 720×400 at
  70 Hz. None: scaled on the desktop's mode, as before. Doom (320×200 at 4:3) gets 640×400@70;
  320×240 gets 640×480; Blocks (640×360, 16:9) fills 1280×720 already: no switch.
- **Setting:** `display/fullscreen-modes`: false keeps every full screen on the desktop's mode.
  The monitor blanks for a moment at each switch (Alt+Enter both ways). False by default after
  the board's trial (§20a).
- **The kernel:** `Scanout::show()` takes the new surface with a timing; the switch rebuilds the
  DSI path already, now at the new clock, and a surface that stalls falls back with its own
  timing. `Display::show()` fits the mode to the PLLs and tells the bridge; the bridge's restarts
  use the timing being sent. `Display::monitor()` reads the EDID (moved from the `display`
  command). `VideoModes::fullScreen()` chooses (pure, host-tested). `ProgramSurface` lays out on
  the mode's size and maps the pointer from the desktop's coordinates to it. Screenshots take
  the shown surface at its own size.

### 20a. On the board (2026-10-14)

- **The mode switch works end to end, and the bridge doesn't hold the mode.** Doom went full
  screen in 640×400 at 70 Hz (the scan at 400 lines from 200 rows, 70.2 frames a second, 0
  underruns, screenshots at 640×400), but the user's monitor showed black with a picture for a
  moment now and then. `display try MODE [lanes= seconds= settle= dds= min= vsync= hsync= hback=]`
  (a test picture in any mode, then the desktop's) narrowed it down with the user watching:
  - The LT8912B **takes the DSI input in every mode**: it measures the frame's lines exactly as
    sent (main 0x9e-0x9f, low byte first: 750 at 720p, 525 at 640×480, 449 at 640×400).
  - **Shown:** 1280×720@60 (80 MHz), 800×600@60 (40 MHz, standard timing), 1024×768@60 (80 MHz,
    the line stretched). **Black:** 640×480@60, 640×400@70, 720×400@70 at 30 MHz (VGA's
    25.175 MHz isn't one the PLLs make), with one DSI lane (720 Mbps), with the receiver's settle
    count 4-12 (fixed at 0x10), with three DDS frequency words, at 40 and 80 MHz, and with
    4-6-line vertical syncs.
  - **What moved it:** a longer horizontal back porch at 31.5 kHz lines. 640×480@60 at 40 MHz
    with a 160-pixel back porch (VGA's is 48) was steady for 10 s; 640×400@70 at 40 MHz glitched
    every few seconds with 160, now and then with 300, often with 480 (the front porch cut to
    55); at 80 MHz with 300, every few seconds. No setting held for a minute.
  - So `display/fullscreen-modes` is **false by default**: full screen scales on the desktop's
    mode as before. The code stays (`Scanout::show()` with a timing and lanes, `Display::show()`,
    `Display::monitor()`, `VideoModes::fullScreen()`, `Edid::takes()`, a minimum clock for
    `fitToClock()`), and `display try` for coming back to it. The bridge's datasheet (its
    timing-dependent registers) is the next lead.
- **A kernel bug behind every Doom hang:** `poll()` checked its deadline, then turned what was
  left into milliseconds rounded up; expired in between, that was 0 ms, `WaitForever`. A timed
  `poll` then waited for an event: Doom's 1-2 ms sleeps froze the game for good (found with a
  watchdog thread marking the main thread's calls: stuck in `io_poll` with a 2 ms timeout, freed
  by a signal). It also explains Blocks' occasional ~210 ms late `poll()` (§18a): the next event
  woke it. Fixed in `Process::poll()`; Doom ran 200 s since without a stall.
- **Each tic was drawn twice:** doomgeneric's `TryRunTics()` made the tic it waited for, saw the
  clock had moved and returned "to update the screen" without running it: 63 presents a second
  at 35 tics, a third never shown. A patch runs the tics once they are made: 35 presents, 0
  dropped.
- **A keyboard that fell silent** (the user's, in Doom's options in full screen; the Num Lock
  light dead, so no reports reached the USB task). TinyUSB's buffer-DMA channel handlers leave a
  channel halted for a cause they don't cover, its endpoint busy for good. They now retry it as a
  transaction error and count it; `usb stats` shows the count and each HID interface's state.
  Unconfirmed as the cause: the counter has stayed 0 since.
- **Open:** all of Doom's threads, the conversion task and the scanout run on core 1 while core 0
  is mostly idle; entering and leaving full screen leaves a window manager drawing error and a
  ~100 ms surface wait among the system's problems (seen after Doom's run, not looked into).

### 20b. Full Screen in the window menu (2026-10-14, the user's request)

- A window that can show full screen (a program's surface: `WindowConfig::fullScreen`) has
  **Full Screen** (Alt+Enter) in its window menu, after Maximize, with BMDS's own
  `window-fullscreen` icon (in the ROM's pack); it does what Alt+Enter does. Other windows'
  menus are as before. Tried on the board with Blocks: chosen from the menu, it went full screen;
  Alt+Enter brought it back.
- **Seen on the way:** screenshots taken while Doom runs in a window come out with the colours
  scrambled (the layout right; the PNG filter ~60 times slower than usual), and clean once Doom
  stops: the open "screenshot noise" item, now reproducible. Found later: PIE's CFG racing between
  tasks (`simd.md` §6).

### 20c. The system monitor after a game (2026-10-14, the user's report)

- **Fixed:** full screen in and out raised `display0.scanout.rebuilds` (it stayed open) and
  `display0.surface.wait_us_max` (critical while Doom ran): `display.md`, "Switches aren't
  problems".
- **Real, and left:** `cpu.core1.busy_us` while Doom runs: all its threads end up on core 1 (ESP-IDF
  pins a task to its core the first time it uses the FPU, and Doom's start there), with the
  conversion and the scanout, while core 0 idles. Spreading program threads over the cores is
  the fix to make. `mem.internal_dma.free` warns while Doom's screen (64 KB of internal RAM) is
  held; it closes when Doom ends.
- **Open:** a real-time client is sometimes one period late as a program starts (1 start in 3
  with a sound only, `bounce -a`; none from the full-screen switches: six Alt+Enters added
  none). The cause isn't found; the warning closes by itself.

## 21. Step 7 design: Solitaire (2026-10-14)

`programs/solitaire/`, MIT, installed by the `dist/solitaire` recipe as `apps/solitaire.app`, in a
window from the start (`X-BMDS-Display=Window`; Alt+Enter as any surface). Blocks' layout: the
rules pure C and host-tested, the program on `libbmdsgame`, the art and sounds written by
`assets.py` at build time.

- **Card art: our own** (§9's open question closed): drawn by `assets.py` (Python's standard
  library, as Blocks' tiles), so there is no licence to check and no build dependency (Byron
  Knoll's set is SVG: rendering it needs a rasterizer). 71×96 cards (Windows' size, so seven
  columns fit 640 across with room between), rounded corners keyed, the pips' curves
  supersampled 4×4. Ranks in the corners from a small bitmap font of the script's own; pips laid
  out as printed cards (the lower half turned); the courts double-headed figures in the suit's
  colours with a crown, a tiara or a cap; a back of a blue lattice. One sheet, `cards.png`: the
  52 faces in 4 rows of 13, then the back, an empty place, a foundation's place and the stock's
  two (turn again, and done). 923×480 RGB565 is 886 KB: in PSRAM.
- **Rules (`rules.c`):** Klondike. 13 piles (the stock, the waste, 4 foundations, 7 columns), a
  card a byte (suit × 13 + rank, the top bit face up). Draw one or three; a column takes a King
  when empty, else one lower of the other colour; a foundation an Ace when empty, else the next
  of its suit; any face-up run moves; a column's new top turns over by itself. Scoring, Windows':
  **standard** (waste to column 5, to a foundation 10, a card turned 5, back from a foundation
  −15, −2 each 10 seconds, the stock turned again −100 drawing one and −20 after three passes
  drawing three, never below 0, and 700,000 ÷ seconds at the win after 30 seconds), **Vegas**
  (−52 to start, 5 a card up, −5 back down, one pass drawing one and three drawing three) or
  none. **Undo** as far back as the game goes (each move keeps the score, passes and the waste's
  fan it replaced); `sol_auto_step()` finds a card that can go up (a double click's, a right
  click's). Host-tested: the deal, every legal and refused move, draws and passes, turning over,
  both scorings, undo back to the deal, the win.
- **The picture:** 640×480 RGB565 (whole multiples in a large window, as every surface; 1.5× full
  screen at 720p). Green felt; the top row (stock, waste fanned three across when drawing three,
  four foundations), seven columns under it (face-down cards 6 pixels apart, face-up 18, closer
  when a column would reach the status line), a status line (score, time, moves). Each frame
  works out where every card is (a display list: position, face, order) and redraws, clipped,
  only the places where something differs from the last frame: a dragged run costs its old and
  new rectangles.
- **Motion:** cards fly where the game sends them (a draw from the stock, a double click to a
  foundation, the right click's moves, the keyboard's): their place eases over 8 ticks, the
  rules already moved on, so input finishes the flights first. The deal flies out card by card.
  **The win:** the foundations' cards leave one by one, the top of each in turn, bouncing on the
  bottom and losing a quarter of their speed each time, drawn every tick without erasing: Windows'
  trails, and the loop's damage is one card a frame.
- **Mouse:** press on a movable card and drag (the run above it comes along); released over a
  pile that takes it (the most overlap among those that do), it moves there, else it goes back.
  A click on the stock draws; a double click (400 ms, the window manager's) sends a card up; a
  right click sends up everything that can go.
- **Keyboard** (first-class): a cursor over the piles, Left and Right across a row, Up and Down
  between the rows and along a column's face-up cards; Space or Enter picks up and puts down (a
  picked-up run is outlined), Escape lets go. D or the stock: draw. F: this card up; A: all
  that can go up. Ctrl+Z, Backspace or U: undo. F2 or N: deal again. O: options (draw one or
  three, the scoring, sound: changing the first two deals again). F1 or H: the keys. The cursor
  shows from the first key and hides when the mouse moves.
- **Sound:** card sounds made by `assets.py` (a snap, a turn, a shuffle for the deal, a short
  fanfare for the win), M on and off. No music.
- **Kept:** the options, and games played and won, in `settings` in the data folder.
- **The timer** counts ticks, so it stops while the window isn't active (the loop runs no ticks)
  and when the game is won; it starts with the first move.

## 22. As built: step 7, Solitaire (2026-10-14)

- **`programs/solitaire/`:** `rules.c` (§21's rules, pure C), `solitaire.c` (the program),
  `assets.py` (the cards, the sounds and the icons), `app.desktop`; **`dist/solitaire`** builds
  `apps/solitaire.app`. The art as §21 planned: 52 faces (pips laid out as printed cards, the
  courts double-headed: a King with a crown, beard and sword, a Queen with a tiara and a flower, a
  Jack with a cap, a feather and a halberd), a lattice back, the places; the icons two cards
  fanned. `solitaire -W` deals a game already played out, which finishes by itself: the win's
  demo (not counted in the games won).
- **Host-tested** (`tests/host/solitaire.cpp`, 10 cases): the deal, columns and foundations (taken
  and refused), turning over, drawing three and the fan, passes and their cost, Vegas, the time's
  cost, undo back to the deal from random play (39 seeds, drawing one and three), the history's
  limit, the win and its bonus, the played-out deal.
- **On the board** (`win hid` keys, `win pointer`): the deal flying out, drawing, Aces sent up
  with F and by double click, a drag from the waste to a foundation, A and right click sending all
  up, undo turning a column's card back down, drawing three fanned, the options (draw three, a new
  deal), the keyboard's pick-up outline, and the win: the cards bouncing off the foundations with
  their trails, then the box, a key dealing again. At rest Solitaire doesn't show in `top`;
  bouncing, its thread takes about 17% (two keyed card blits a tick from PSRAM, and the loop's
  copy forward), the window manager 7%.
- **Full screen at any ratio** (found here): 640×480 is 4:3, 1.5 times across at 720p, and full
  screen only widened by whole multiples, so it showed 2 times across: a third too wide. Now a
  whole multiple is used when one is within 3% of the picture's shape, else exactly the shape's
  width, each column the nearest of the picture's (`Pixels::stretch()`, host-tested), as the scan
  already repeats rows. The pointer's mapping, `BMDS_SURFACE_INFO`'s and `PRESENTED`'s scales
  follow. Solitaire full screen is 960×720, centred; `bounce -s 640x480 -F` runs at 61 frames.
  Blocks (2×) and Doom (3× at 4:3) are unchanged.
- **Seen again: scrambled screenshots** (§20b's open item). Twice, the first PNG after a program
  went full screen came out with its colours scrambled in columns (the layout right) and slow:
  the row swap (`Pixels::swap24()`, PIE) 8 times its usual time, the filter 60. JPEGs and later
  PNGs of the same picture were clean, and it didn't happen again on demand. **Found and fixed**
  (2026-10-15, `simd.md` §6): PIE's CFG (misaligned access) is the core's, not saved per task,
  and the kernels set and restored it, racing each other.
- **Not yet:** the options box answers keys only (a click closes it); Vegas' cumulative score;
  dealing by number.
- **Moved (2026-10-15)** into Lucky Duck, the casino (`luckyduck.md` §8a, §14): the same rules
  and play at 1280×720 with duck cards; Vegas' score now plays from the casino's bank.

## 23. Which core a game's threads run on (2026-10-15)

- **Measured:** each thread of Doom stayed wherever it was at its first float (ESP-IDF pins it
  then): the game's thread (21-27%) and the synth's (31-41%) on core 0 in most runs, with the
  display's conversion (`surface`, 27%, core 1 by design) on the other; before, all of them on
  core 1 at times (`cpu.core1.busy_us` critical, §20c).
- **Now** (`processes.md` §3.12a): the loader pins a program's first task to the less busy core
  (ties to core 1, the display's), and libbmdsgame's mixer and music threads pin themselves to the
  other (`BMDS_SCHED_PIN`). Doom: the game's thread on core 1 with the conversion, the synth and
  mixer on core 0 with the kernel's audio, about 45% and 64%, the same in four runs out of four,
  0 late periods, 59-61 frames a second.
- **Seen on the way:** Solitaire couldn't start while Doom ran (`Out of memory`). Fixed: §24.
- **Since (2026-10-18, `bench.md` §16-17):** the game's thread sharing core 1 with the conversion
  turned out to cost Doom's timedemo a quarter (44 fps against 53-58). The games now set
  `BMDS_PROGRAM_DISPLAY` and start on core 0; their mixer and music go to core 1.

## 24. Two games at once, and what scaled windows cost (2026-10-15)

- **The failure:** every `ProgramSurface` took a 30 KB strip of internal DMA RAM when it opened
  (1280 × 8 rows of RGB888, for scaled rows), whether it ever scaled or not. With Doom full screen
  the largest free internal DMA block was 18 KB, so Solitaire's `bmds_surface_open()` failed.
- **Now:** a window draws its scaled rows in the window manager's strip (`Painter::rows()`: the
  caller makes each piece in a half while the 2D-DMA copies the other; RGB565 or BGR), and full
  screen allocates its strip on entering and frees it on leaving. A windowed program costs no
  strip; Doom full screen keeps its 30 KB. Doom and Solitaire run together (Doom leaves full
  screen when Solitaire's window takes the focus).
- **Measured** (`surfdemo`, 640x360 at 2x): BGR24 from 33 presents a second (one run of the old
  code, which waited for each 8-row band's copy) to 57-58 (three runs); RGB565 and L8 at 58 both
  ways. At 3x (960 wide, bands crossing the strip's halves) the rates fall on 60's steps (30, 40,
  55-58) both ways: no change seen. Pictures checked at 2x and 3x in all three formats.
- **Scaled windows' load** (the user's ask, Doom maximized at 3x: core 0 at 100%, the window
  manager 42%): the CPU expanded each row across, copied it twice down and wrote all of it back
  from the cache; the 2D-DMA read it all. Now each strip row is expanded once and the 2D-DMA
  sends it `scale` times (`Surface::startBlit()`'s repeat; a TX descriptor a row, one RX block),
  and the palette's indices load four at a time. A frame from 26 ms to 14 (most of it waiting
  for PSRAM: 1.7 MB written), the window manager's CPU from ~16 ms a frame to ~10, Doom's frames
  shown from ~22 a second to ~29, core 0 at 77%. The expansion alone (320x200 at 3x, idle):
  4.3 ms to 3.6 with Doom's frame in PSRAM, 2.6 from internal RAM. Further cuts would cost
  colour: an RGB565 strip (the 2D-DMA widening) would halve the CPU's stores; not done.
- **Seen meanwhile, solved (2026-10-15):** 2 PNG screenshots of about 16 taken with Doom drawing
  came out streaked down the columns, both filtering for ~3 s instead of ~0.1. Full-screen
  Solitaire made it happen every time: with the L2 RAM short, the PNG writer's rows came from the
  LP RAM, where PIE's 128-bit loads are slow and wrong (`docs/platforms/esp32p4.md` §1). The
  internal spaces now ask for DMA-capable memory, which only the L2 RAM is; the rows fall back to
  PSRAM when the L2 RAM can't hold them.
- **The pointer full screen (2026-10-15, the user's report):** the desktop isn't shown, so its
  arrow wasn't either. A full-screen surface whose pointer mode is `NORMAL` now draws the window
  manager's arrow (`arrowPicture()`) on the scan surface itself: the presenter puts back what it
  covered, converts the present, draws it where the pointer is (a scan row takes the arrow's row
  its first screen line shows, so the scan's row repeat keeps its size), then presents; a move
  alone wakes the presenter for that. `HIDDEN` and `RELATIVE` (Doom's mouse look) draw none.
- **What else holds internal RAM:** a program's thread stacks go there while the largest block
  keeps 64 KB to spare (`stackSpaceFor()`), and the games' 32 KB stacks use 2-8 KB; windowed Doom
  costs 37 KB this way, full screen 65 KB more (its strip and the screen).

## 25. The 2D-DMA for programs (design, 2026-10-16; the ABI for the user's review)

The user's "having a 2D-DMA access would be great", after `luckyduck.md` §23 showed what a frame
touches in PSRAM sets its cost.
- **The problem:** a program's CPU copies PSRAM at about 28 MB/s each way, 70 to 180 ns a pixel
  with the scan and the window manager competing.
- **What the 2D-DMA does:** it writes about 150 MB/s (`docs/platforms/esp32p4.md`) without the
  CPU. The window manager already copies with it (`BlitChain`).
- **What programs would use it for:** their bulk copies: a background put back under what moved,
  and the double buffer's catch-up copy (`libbmdsgame`'s loop). Today those are half of a busy
  frame.

### 25a. The ABI: two surface controls (appended: ops 6 and 7)

No new system call: `bmds_surface_control()` takes them, so a kernel without them answers
`-BMDS_EINVAL` and `libbmdsgame` falls back to the CPU.

```c
#define BMDS_SURFACE_IMAGE 6u /**< `const bmds_surface_image_t *`: a picture BLIT may copy from. */
#define BMDS_SURFACE_BLIT 7u  /**< `const bmds_surface_blit_t *`: copies or fills by the 2D-DMA. */

#define BMDS_SURFACE_IMAGES 4u     /**< Image slots a surface has. */
#define BMDS_SURFACE_BLIT_MAX 64u  /**< Pieces a BLIT takes. */

/** @brief A picture of the program's, for BLIT: in the surface's format. */
typedef struct {
        uint32_t    size;
        uint32_t    slot;   /**< 0 to BMDS_SURFACE_IMAGES - 1. */
        const void *pixels; /**< Inside one allocation from bmds_mem_alloc(); NULL: the slot forgotten. */
        uint32_t    width;  /**< Pixels. */
        uint32_t    height;
        uint32_t    stride; /**< Bytes a row. */
} bmds_surface_image_t;

/** @brief One rectangle of a BLIT. */
typedef struct {
        int32_t  x, y;           /**< In the buffer written. */
        uint32_t width, height;
        int32_t  from_x, from_y; /**< In the source (a fill: unused). */
} bmds_surface_piece_t;

#define BMDS_BLIT_BUFFER 0u /**< From another of the surface's buffers: `from` its index. */
#define BMDS_BLIT_IMAGE 1u  /**< From an image: `from` its slot. */
#define BMDS_BLIT_FILL 2u   /**< A fill: `from` the pixel (the format's value). */

/** @brief Copies or fills pieces of a buffer the program holds. */
typedef struct {
        uint32_t                    size;
        uint32_t                    buffer; /**< Acquired and not yet presented. */
        uint32_t                    source; /**< BMDS_BLIT_... */
        uint32_t                    from;
        uint32_t                    count;  /**< 1 to BMDS_SURFACE_BLIT_MAX. */
        const bmds_surface_piece_t *pieces;
} bmds_surface_blit_t;
```

- **IMAGE** registers a picture.
  - The kernel checks that it lies inside one of the program's own allocations, in memory the
    2D-DMA can read (PSRAM, or internal RAM given `BMDS_MEM_DMA`), and that its row is at least
    `width` pixels.
  - It writes the picture back from the CPU's cache once. A program that changes the picture
    afterwards registers it again.
  - Freeing it while registered is the program's mistake: the 2D-DMA only reads it, into the
    program's own buffer.
- **BLIT** copies or fills the pieces in one 2D-DMA job and returns when they are written.
  - The pieces are clipped to the buffer and the source.
  - The cache is kept honest around the copy: each piece's rows in the buffer are written back
    and invalidated before the copy, one cache operation a piece (4-5 µs each, so not one a row).
    A copy from another buffer reads what its last present wrote back.
  - It returns 0, or `-BMDS_EINVAL`: a buffer not held, a source that is the buffer itself, an
    empty slot, a format without a pixel size the 2D-DMA takes, too many pieces.
  - Thread safety: while it runs, no other thread of the program draws in the rows its pieces
    cover.
- **Why images are registered, not passed each time:** one check of the memory and one cache
  write-back, instead of both on every frame.

### 25b. The kernel (`bmds_ui`)

- **One `BlitChain` for every program surface,** under a mutex. It starts on the first BLIT and
  sizes its descriptors for 64 pieces and 256 fill rows (about 5 KB of internal DMA RAM). A bigger
  fill becomes more jobs.
- **`ProgramSurface`** keeps the four image slots and answers the two controls. The buffer written
  must be in the program's state (acquired); the source buffer any of the others.
- **`Process`** gains `owns(ptr, bytes)`, a walk of its allocations, for IMAGE.

### 25c. `libbmdsgame` (a static library: no ABI)

- `bmds_game_dma_image(game, slot, image)` registers an image.
- `bmds_game_dma_copy(game, slot, rects, count)` copies the image into the canvas being drawn,
  rectangle for rectangle at the same place (a background).
- `bmds_game_dma_fill(game, rects, count, pixel)` fills.
- Each cuts the rectangles to the canvas's clip, records them as damage, and goes by the CPU when
  the kernel says no.
- **The loop's catch-up copy** (`bring_up_to_date_except()`) goes by BLIT from the newest buffer,
  its row bands as pieces (more than 64 make more calls).

### 25d. Lucky Duck

- **The felt** is registered as image 0 when a scene paints it, and again after a repaint.
- **The stage** restores the felt for all of a frame's dirty rectangles in one BLIT (the felt's
  pieces of every rectangle, not under cards), then draws the items rectangle by rectangle.
  - Batching needs dirty rectangles that don't overlap: a blend drawn twice where two overlap
    would darken. `stage_draw()` falls back to one rectangle at a time when they do.
- **Expected:** the felt restore from 5-9 ms to well under 1 ms, and the catch-up copy off the
  CPU, so no frame of a deal should skip a tick.
- **Measured first on the board:** a BLIT's fixed cost (the cache operations, the job, the call).
  If small rectangles cost more by DMA than by the CPU, `libbmdsgame` copies those by the CPU
  (below a measured threshold).

### 25e. Later, not now

- The PPA (the pixel processing accelerator) can blend with a colour key and scale, which would
  take keyed sprite and card blits too. That needs a design of its own.
- An asynchronous BLIT (returns at once; the next call or present waits), so the CPU draws other
  rectangles meanwhile.

### 25f. As built (2026-10-16)

- **The ABI** as §25a: `BMDS_SURFACE_IMAGE` (6), `BMDS_SURFACE_BLIT` (7), `bmds_surface_image_t`,
  `bmds_surface_piece_t`, `bmds_surface_blit_t`, `BMDS_BLIT_BUFFER`/`_IMAGE`/`_FILL`,
  `BMDS_SURFACE_IMAGES` (4), `BMDS_SURFACE_BLIT_MAX` (64).
  - IMAGE checks the whole `stride × height` against the program's allocations
    (`Process::owns()`).
  - A fill's pixel is the format's value, its bytes low first.
- **The kernel** (`ProgramSurface::setImage()`, `blit()`):
  - One `BlitChain` (64 pieces, 256 fill rows) and a 3,840-byte fill line in internal DMA RAM,
    made on the first blit, shared by every program surface under a mutex. It is created through an
    atomic pointer, not as a global object.
  - **The cache:** each piece's rows are a byte range, and ranges within 16 KB of each other are
    merged. Before the copy, each range is written back and invalidated (`C2M | INVALIDATE |
    UNALIGNED`); after it, invalidated again, rounded out to the L2's 128-byte lines. M2C refuses
    unaligned ranges; the buffers are 128-aligned and padded, so rounding stays inside them.
  - A source buffer the program holds is written back first too.
  - **A full chain** runs and goes on. A fill is cut into strips of at most 256 rows and 1,280
    pixels.
- **`libbmdsgame`:**
  - `bmds_game_dma_image()`, `bmds_game_dma_copy()`, `bmds_game_dma_fill()`.
  - A batch of up to 64 pieces is one BLIT; the CPU takes over for good once the kernel says no.
  - The loop's catch-up copy goes through the same batch: `bmds_game_spans_except()` gives the
    row bands, and `bmds_game_copy_except()` is now its CPU user.
- **Lucky Duck:**
  - The felt is given to the 2D-DMA at the first draw after `felt_baize()` or `felt_carpet()`.
  - `felt_restore()` goes by it.
  - The stage restores a frame's felt in one batch when its dirty rectangles are disjoint
    (`luckyduck.md` §24).
- **Checked on the board:**
  - **The old kernel** (ops 6 and 7 refused): everything drew by the CPU, correctly.
  - **The new one**, Video Poker's five deals and draws: one skipped tick in the 25 busiest
    seconds. That was about 5 a second with the CPU's copies, and 11 to 18 before §23.
  - The window manager kept 0 errors.
  - Every scene drew correctly: Video Poker, the transition, the lobby, Blackjack, the dice and
    Solitaire.
- **Not yet:** a BLIT's fixed cost wasn't measured on its own (the frames made it moot). Small
  pieces go by the DMA too; a CPU threshold waits for a measurement that needs one.

### 25g. A lost 64-byte stretch, and `blittest` (2026-10-16, the user's: "a single line sometimes looks garbled")

- **Seen:** now and then, part of one row of an animated sprite garbled.
- **Found with `blittest`** (`sdk/tests/blittest.c`, built with the SDK examples, kept as BLIT's
  board test). It does random CPU fills, then BLITs of each kind (image, fill, another buffer)
  with random pieces, among them narrow neighbours a few pixels apart. After every blit it checks
  every pixel against a model and tells each bad run: where it is, its offset in its 128-byte
  line, and the piece it falls in.
  - **Image and buffer copies:** 60-80 bad runs every 300 rounds, nearly all 32 pixels (64 bytes)
    at line offset 0 or 64, and mostly outside every piece: what the CPU had drawn just before was
    lost.
  - A full read of the buffer before each blit made them go: dirty lines were being dropped.
- **The cause:** the P4's L1 lines are 64 bytes and its L2's 128.
  - The write-back before a copy covered the pieces' exact bytes, so the L1 wrote back only the
    64-byte halves they touch.
  - The invalidate after rounded out to whole 128-byte lines (M2C takes only aligned ranges), so it
    dropped the other half too, dirty in the L1 with the CPU's drawing.
  - **Fixed:** both round out to the same whole 128-byte lines, so nothing is invalidated unless
    it was written back first.
  - IMAGE now only writes the picture back. It also invalidated, unaligned, which reached the
    program's memory beside it.
- **Also found:** fills read stale memory. Their line in internal RAM is cached too, and wasn't
  written back after being filled. Fixed.
- **After the fixes:** image, fill, buffer and mixed runs, 18,000 blits in all: 0 bad pixels.

**Benchmarks moved (2026-10-17):** `display ppa`'s timings, `ppa path`, `ppa soak` and `ppa
blitcost` became the `display.ppa.scale`, `display.ppa.path`, `display.ppa.soak` and
`display.blitcost` benchmarks (`devplan/bench.md` §11); `display ppa` keeps the checks. The figures
above were taken with the old commands.

## 26. Full screen out of internal RAM after another game (2026-10-20, the user's report)

- **The report:** Doom full screen after a fresh boot; then Lucky Duck; then Doom again came up in
  a window and wouldn't go full screen. The log: `ProgramSurface: full screen: Out of memory`, in
  the scan-out's switch (its descriptor list).
- **What full screen takes of internal DMA RAM:** about 89 KB while it lasts. The scan's line mode
  needs a descriptor a screen line (720 × 64 bytes, 46 KB, in 4 KB chunks), the strip 30 KB
  (8 rows of 1280 at 3 bytes), L8's triple table 8 KB. Doom full screen after a fresh boot leaves
  20 KB of internal RAM.
- **Not a leak:** Lucky Duck run three times kept 19 KB after the first and nothing more. Two
  things made on first use were kept for good: the program blitter (`BMDS_SURFACE_BLIT`'s chain
  and fill line, about 19 KB) and the audio device's mix buffers and I2S channel (about 20 KB, kept
  after its idle stop). Free internal RAM went 149 → 129.5 (Doom) → 110.5 KB (Lucky Duck), and the
  second Doom's switch didn't fit.
- **Fixed:** the blitter is counted by the surfaces that blitted and freed with the last;
  the audio device frees its buffers and channel when it stops (not on a restart), made again by
  the next start. The same sequence: 149 → 144 → 139 KB, and the second Doom full screen.
- **The audit (idle, after boot; `mem blocks`, attributed once with ESP-IDF's heap hooks):** a
  553 KB heap, 149 KB free; 141 KB static besides (`.bss` 63.5 KB in small pieces, IRAM code
  62 KB, `.data` 16 KB).

  | Holder | Bytes | |
  |---|---:|---|
  | Ethernet rings and buffers (32 receive, 16 transmit) | 75,904 | the user's call (network.md §29) |
  | lwIP's heap (`BMDS_NET_HEAP_KB`) | 65,792 | the user's call: frames from PSRAM vanished |
  | dropbear's stack (64 KB asked, ~10 KB used) | 65,536 | `stackSpaceFor()`: room at boot |
  | the window manager's strip (16 rows × 1280 × 3) | 61,568 | |
  | PSRAM's reserved DMA pool (`SPIRAM_MALLOC_RESERVE_INTERNAL`) | 32,768 | a region of its own, still usable |
  | surface 0's batching lines and 2D-DMA chain | 27,136 | |
  | the window manager's pieces and clips | 8,704 | |
  | task stacks, ~25 tasks (2-8 KB) | ~40,000 | |
  | everything else (144 blocks under 1 KB) | ~16,000 | |

- **Done:** a program's stack went to internal RAM while the largest block kept 64 KB to spare,
  so the first long-running program (dropbear, at boot) held 64 KB of it for good. Stacks over
  32 KB (the deep-recursion budgets, mostly untouched) now go to PSRAM (`stackSpaceFor()`); the
  games' 32 KB stacks still go to internal RAM when there is room.

## 27. L8 full screen through the PPA's CLUT (2026-11-06, `demo.md` §8)

BMDS Demo #1 changes every pixel of a 640×360 L8 picture every frame, and full screen that
presented at 35-39 fps: the presenter's palette lookups on core 1 took ~12 ms of a ~16 ms
conversion (`demo.md` §8b has the profile). Now, for L8 at a whole scale:

- **The CPU repeats indices, the PPA looks them up:** each changed row's indices go into a half
  of the presenter's strip repeated `scaleX` times (`Pixels::repeat8()`: PIE `vzip.8` at 2×, word
  stores at 3× and 4×, a scalar twin in `simd test repeat8` and the host tests), the half is
  written back from the cache, and `Ppa::expand()` (the blender's CLUT, with its invisible
  foreground: bypass never completes, `docs/platforms/esp32p4.md`) writes it as RGB888 into the
  scan surface's back buffer while the CPU fills the other half.
- **`Surface::beginWrite(rects)`:** what `startBlit()` does before its own DMA (the pending swap
  waited for, the last present's changes copied forward where the rectangles don't cover them,
  the surface's own jobs finished, the rectangles marked changed), then the back buffer for
  another engine to write; the caller finishes before its next call on the surface.
- **The CLUT is loaded when the palette changes** (`_clutStale`, set by `setPalette()` beside the
  triple table's flag), after the last job, so a palette lands on the present it came with.
- **The PPA is one user's at a time:** `Ppa::start()` claims it (`Busy` for a second instance), so
  a `display.ppa.*` bench run during a full-screen game is refused, not mixed into its frames.
  The presenter holds it from entering full screen to leaving; without it (`Busy`, no memory) the
  CPU expands as before, said in the log.
- **Measured** (`benchprog-display surface`, `demo.md` §8b): every L8 case full screen at 60 with
  less CPU (the demo's 16.8 → 6.6 ms a frame, Doom's 5.6 → 3.6, Quake's 6.8 → 4.4). The first
  build's `repeat8` wrote a byte at a time at 3× and cost Doom 3 ms more than the old path:
  word stores fixed it. Colours checked against the CPU path (`wire`), a cycling palette
  (`surfdemo -f l8 -F`), and full screen entered and left over and over (the bench's cases).

## 28. Sounds ahead, on time, and speech (2026-11-07, `demo.md` step 6)

What BMDS Demo #1 needed from libbmdsgame's sound, for any game whose sounds are planned (a music
game, a cutscene, a demo):

- **`BMDS_GAME_AUDIO_AHEAD`** (`bmds_game_audio_config_t::flags`, with `latency_ms` appended; an
  older `size` still opens real time): the mixer writes an ordinary buffered stream, a period at a
  time, instead of a real-time client's period on its deadline, so it never preempts drawing for
  a deadline (the synth's lesson, `audio.md` §27). Its thread runs at the game's priority on the
  game's core (below); the heard time of each period is the synth's estimate (the mixer's next take, the ring,
  two DMA buffers), eased and found again after an underrun.
- **`bmds_game_play_at(..., at_ns)`:** a sound heard at a moment, to the frame: the voice is taken
  at once and waits its frames in the mixer (a stop before then ends it unheard). Real time, the
  mixer knows each period's `play_ns` from `audio_cycle`; ahead, its estimate. A time already past
  starts it at once. `bmds_game_audio_latency_ns()` says how far ahead to ask.
- **`bmds_game_sound_speak(audio, text, how, sound)`:** text to a sound at the mixer's rate
  (`audio_speak`, 16 kHz, resampled), voice, speed, pitch, volume and markup as `say` takes them;
  made in the caller's thread (about a sixth of a core while speaking), so a game makes its lines
  on a thread of its own ahead of time. Any thread: it reads only the mixer's rate.
- **`bmds_game_speaker_t`** (`_open`, `_say`, `_close`): a voice kept open for line after line;
  opening one is ~0.2 s and 1 MB, so `bmds_game_sound_speak()` is now a speaker opened, used once
  and closed.
- Tested on the host: the mixer's scheduled start (a frame's accuracy, a past time, no time, a stop
  before the start). On the board: demo1's boot and cues.
- **Found on the board** (`tasks` during demo1's credits): the ahead mixer at `SCHED_FIFO` 1 (kernel
  priority 7, above the presenter at 4) took 13 % of core 1 doing nothing, writing silence each
  5 ms; with speech making on the drawing's core besides, the fire part fell from 48 to 25 fps.
  Now it runs at the game's priority, writes four periods a time, and sleeps while no voice plays
  and nothing is asked (a condition the play calls signal): 145 ms of CPU in a whole run. The speech
  thread a game makes lines on belongs below its drawing: `SCHED_IDLE` now means that
  (`processes.md` §3.12; it was the normal priority).
- **Found by ear** (the user: "glitching when speaking"), counted with a log of the stream's
  underruns: on the other core, below the presenter (4) and FM rendering ahead (6), the mixer went
  without for the ring's whole 170 ms mid-line (queued 0-960 frames at each count), and a line after
  silence started with 5 ms queued. Now it keeps to the game's core, time-sliced with the drawing at
  its priority (back within a tick); starting dry it writes half the ring of silence first (the
  timeline measured after, so `play_at` still lands); it stays awake 0.5 s after the last sound, so
  lines close together share a stream; and it is woken once per four-period write, not each period.
  A whole run: every underrun left is the stream running dry while idle (counted by the device as
  one), and the demo's rates rose (fire 48 → 58 fps, solids 40 → 47, the cube 38 → ~48: core 1 no
  longer wakes it each 5 ms).
