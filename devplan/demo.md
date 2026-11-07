# BMDS Demo #1 (`demo1`): a demoscene-style demo of the system

Status: **planned** (2026-11-06, with the user). Built on the SDK's 3D library (`3d.md`) and
sequencer (`sequencer.md`), which it is the first user of.

## 1. Goal

- **Show what BMDS can do, in a fun and entertaining way** (the user's): a demo in the old
  demoscene's spirit, fun graphics over fast techno.
- **The music:** a fast techno soundtrack on MIDI, through FM by default, **the wavetable as an
  option** (the user's).
- **Speech:** the speech engine says interesting things now and then (the user's).
- **Graphics:** a mix of 2D and 3D, text, palette shifting (the user's).
- **Interactive:** mouse and keyboard change things in interesting ways (the user's).
- **Everything reusable goes in the SDK or the kernel** (the user's): the demo is the effects and
  the show; the tools are for everyone.

## 2. Decisions (the user's, 2026-11-06)

- **Name:** "BMDS Demo #1", the command and folder `demo1` (`apps/demo1.app`).
- **Picture:** 640×360 L8, scaled 2× to 1280×720, full screen suggested (`BMDS_SURFACE_FULL_SCREEN
  | BMDS_SURFACE_WHOLE`), Alt+Enter to a window. 60 fps.
- **3D:** our own API in the SDK (`3d.md`), not OpenGL.
- **Music:** the SDK's in-program sequencer with beat sync (`sequencer.md`), an original track.

## 3. The show

Timed by the song's sections (cues), about 3½ minutes at 140 BPM, then it loops (or ends with
`-1`). Each part names what it demonstrates; the bracket is the SDK piece it uses or proves.

| # | Part | What's on screen | Demonstrates |
|---|------|------------------|--------------|
| 0 | **Boot** | A fake BIOS text screen types itself, "BMDS" in block letters, the screen "breaks" into the demo on the first kick | text, palette flash [libbmdsgame text, palette] |
| 1 | **Copper and scroller** | Raster bars sliding behind a sine-wave scroller in a big font, bars pulsing on the beat | palette per frame, beat sync [seq hits, palette] |
| 2 | **Plasma** | Classic sum-of-sines plasma, the colour comes only from cycling the palette; the drop changes the palette's whole mood | palette cycling [palette helpers] |
| 3 | **Starfield to wireframe** | 3D stars rushing; they slow and gather into a wireframe BMDS logo that spins | 3D points and lines, depth cue [3D wire] |
| 4 | **Tunnel** | Textured tunnel flying on a curve, turning with the mouse, its texture's palette cycling | lookup-table mapping [canvas remap] |
| 5 | **Solids** | A Gouraud torus knot and flat-shaded polyhedra orbiting, the light following the pointer, flashing on the snare | 3D lighting and ramps [3D flat, Gouraud] |
| 6 | **Vector balls** | Hundreds of shaded ball sprites forming shapes (cube, sphere, the "B"), morphing on each bar | projection and depth sort, keyed blits [3D project, sort] |
| 7 | **Fire and glow** | Fire rising from the bottom, additive glowing wireframes above it | saturating add in L8 [3D `ADD`, PIE] |
| 8 | **Rotozoom and the cube** | A rotozoomer of the BMDS logo, then the same picture (and the plasma, live) on a spinning textured cube | affine mapping, render to texture [canvas texture, 3D textured] |
| 9 | **Credits** | A starfield and a typed list of what the demo used (the sequencer, the 3D library, the FM synth, PIE…), with the system's live numbers (fps, CPU per core, memory) | text, the stats calls |

The parts may change as they're built; this list is the plan to start from.

### 3a. Speech

Lines rendered ahead into mixer sounds (`bmds_game_sound_speak()`) and played on a beat, at a
section's cue, pitched and paced by markup. Examples:

- "Bare metal. Do stuff." on the first drop;
- "This is an E S P 32 P 4. Four hundred megahertz. No G P U." before the 3D part;
- "Every pixel you see was drawn by software." during the solids;
- "Sixteen voices of F M, synthesized live." at the breakdown;
- the credits read their headings.

## 4. The music

An original track, written for the demo as sequencer data (C structs in step 3; the text form once
it exists, `sequencer.md` §4c):

- **140 BPM, A minor**, four-on-the-floor kick, off-beat open hats, sixteenth closed hats with
  `CHANCE` variation, a clap on 2 and 4;
- an **acid bassline** (Synth Bass 2 or Lead 2 sawtooth) with slides and a filter lane on 74;
- an **arpeggiated lead** and a **pad** for the breakdowns;
- sections: intro (8 bars), build (8), drop A (16), breakdown (8), build (8), drop B (16),
  outro (8): ~3½ minutes.
- **FM by default** (`/dev/midi`, the system synth), **`-w` or W** for the wavetable port,
  switched live at the next bar (notes cut on the old one, the song carries on).

## 5. Interaction

- **Mouse:** steers the camera or the light in the 3D parts and the tunnel's turn; moving it fast
  adds glow; **a click** sends a shockwave (a ripple in the 2D parts, a jolt in the 3D) and a
  palette burst; **the wheel** sweeps the acid's filter (controller 74).
- **Keys:**
  - Space: the next part (the music jumps to its section at the next bar);
  - 0-9: a part by number;
  - letters: the drum pads, quantized to the next sixteenth (kick, snare, clap, hats, a stab);
  - Enter, then typed text and Enter: it's spoken on the next bar and joins the scroller;
  - W: FM ⇄ wavetable; M: music on/off; F: the overlay (fps, ms per frame per core, triangles,
    the synth's voices and CPU); P: pause (music and picture); Esc: quit.
- **Actions** declared in `controls.conf` (`controls.md`) so pads and rebinding work.

## 6. What goes where

- **The kernel:** K1 (frame times), K2 (a port's latency), K3 (the synths' sound controllers)
  (`sequencer.md` §7).
- **The SDK:** the 3D library (`3d.md`), the sequencer (`sequencer.md`), palette helpers and
  canvas mapping (`3d.md` §8), `bmds_game_play_at()` and `bmds_game_sound_speak()`.
- **Effects that graduate to the SDK** when a second program could want them: the sine scroller
  (as `bmds_canvas_text_wave()`), plasma and fire (as small reusable generators in a
  `bmds/effects.h`, if more than the demo wants them; decided when built). The parts' choreography
  stays in the demo.
- **The demo:** `programs/demo1` (the parts, the show's timeline, the song, its assets) and
  `dist/demo1` (the app folder: the program, `app.desktop`, `controls.conf`, textures and the
  font made by an `assets.py`, as Blocks does).

## 7. Structure of the program

- `demo1.c`: the game loop (`bmds_game_run`, 60 Hz), the sequencer, the part switching on cues.
- `part.h`: a part is `{name, init, enter, frame(time_ns, pos), event, leave}`; parts share the
  canvas, the palette manager, a 3D context and the hit list for the frame.
- `palette.c`: the show's palette manager: each part's base palette, crossfades on part changes,
  flashes and bursts from hits, applied once a frame (`bmds_game_palette()`).
- `song.c`: the track as data.
- `parts/*.c`: one file a part.
- `overlay.c`: the F overlay.
- Host-testable pieces (the palette manager, the timeline) are separate from drawing.

## 8. Budgets

- **Frame:** 16.7 ms; each part measured on the board with the overlay, aiming at ≤ 12 ms on one
  core so the drop's extra effects fit. If a part needs it, the second core draws half the bands
  (`3d.md` §7), measured beside the display's own work first.
- **Memory:** 3 L8 buffers (690 KB PSRAM), tables (sine, tunnel's u/v 2 × 230 KB in PSRAM, or a
  half-size table mirrored), textures in internal RAM where small; under 4 MB total.
- **Size:** the program ~150-250 KB; assets ~200 KB.
- **Audio:** the synth's voices at most 24 at once in the drops (FM is ~8 µs a voice-block).

### 8a. Found: whole frames present at under 40 a second (2026-11-06)

`wire -e` (`3d.md` §13: a 640×360 L8 surface cleared and damaged whole every frame, nothing else
drawn) runs at 38-39 fps, windowed or full screen. Full screen, the presenter expands each L8
frame to 1280×360 RGB888 (1.4 MB written to PSRAM, the scan doubling the rows), and that, not
the program's drawing, is the limit. A demo changes every pixel every frame, so **before the
demo's parts, the kernel's whole-frame L8 present must reach 60** (step 0 below). Options, to be
measured and chosen with the user:

- **The PPA's CLUT** (L8 in, RGB888 out, in hardware: 1.65 ms for 320×200 in `games.md`'s
  measurements, ~6 ms for 640×360), then the doubling across by the 2D-DMA or the scan.
- **Doubling across in the scan** (as rows already are): the presenter writes 640×360 RGB888
  (0.7 MB, half the traffic) if the DW-GDMA or the DSI path can repeat pixels.
- **The presenter on the other core** from the program's drawing, with PIE for the expansion.
- **A narrower format the scan reads** (RGB565 widened by the 2D-DMA), if the scan can take it.

### 8b. Done: L8 full screen through the PPA's CLUT (2026-11-06)

**Measured first** (core 1 profiled under `wire -e -F`, `tools/bmds-prof`): the presenter (the
`surface` task, core 1) was 63% of the core, about half of it the CPU's palette lookups
(`expandWords<2>`, `expandFour<2>`: ~12 ms a frame against the ~6 ms `display.expand`'s rate
predicts, the source read from PSRAM beside the scan, the 2D-DMA and the program), and ~13% in
the 2D-DMA driver's critical sections. A conversion longer than a frame costs two (the scan
surface's next drawing waits for the last swap), so frames came at 30 and 60 in turn: 35-39 fps.
The bench's own case (`surface.present`, `demo_l8_640x360_full`, added) reached 54 because its
program draws nothing.

**Chosen: the PPA's CLUT, the CPU only repeating indices across** (games.md §27): the presenter
writes each row's indices doubled (`Pixels::repeat8`, PIE's `vzip.8`, 16 pixels a step) into a
half of its internal strip while the PPA expands the other half through the palette straight
into the scan surface's back buffer (`Surface::beginWrite()`). The PPA isn't the CPU's cache
traffic, and it only has to write 1280×360 (the scan repeats rows).

| Full screen (`surface.present`, the board) | fps before | after | system CPU a frame before | after |
|---|---|---|---|---|
| `demo_l8_640x360_full` (whole frames) | 54 | 60 | 16.8 ms | 6.6 ms |
| `doom_l8_320x200_full` (3×) | 60 | 60 | 5.6 ms (2026-10-26) | 3.6 ms |
| `quake_l8_320x240_full` | 60 | 60 | 6.8 ms (2026-10-26) | 4.4 ms |
| `wire -F` (drawing 7-10 ms on core 0) | 35 | 60 | | |

- **Later, if core 1 is wanted for drawing:** `repeat8` is still about a third of core 1 at
  640×360, nearly all of it reading the program's frame from PSRAM (~43 MB/s with the scan, the
  PPA and the program all at it): fetching rows into internal RAM by the 2D-DMA first would
  hide it. Bigger strip halves would cut the ~30 PPA jobs a frame and their fixed cost, and the
  strip's cache write-backs run with interrupts off.

## 9. Steps

The demo is built alongside the libraries, each step using what has just landed:

0. **Whole frames at 60** (§8a): the kernel's L8 present path, measured with `wire -e` and
   `surface.present`'s cases (a 640×360 L8 whole-frame case added). **Done** (§8b, 2026-11-06).
1. **Skeleton:** `programs/demo1`, `dist/demo1`, the loop, palette manager, overlay, part
   switching by keys; parts 1 and 2 (copper and scroller, plasma): 2D and palettes only.
2. With **3D step 1** (wire): part 3 (starfield to the wireframe logo).
3. With **sequencer step 1**: the song (first sections), sync for parts 1-3, the cue timeline.
4. With **3D steps 2-3** (solids, PIE): parts 5 and 6; the overlay's 3D counters.
5. With **sequencer steps 2-3**: live control, the wavetable switch, drum pads, speech lines,
   typed text spoken.
6. With **3D step 4** (textures): parts 4, 7, 8; the boot screen and credits.
7. **Polish:** the whole song, transitions, the loop, timings on the board, the `dist` recipe and
   icons, `RELEASE_NOTES.md`.

## 10. As built

### Step 1: the skeleton, copper and plasma (2026-11-06)

- **`programs/demo1`:** `demo1.c` (the frame loop at 60, part switching, pause, the overlay),
  `pal.c` (the show's palette: a part's base, crossfades between parts, flashes, cycling; entries
  252-255 the overlay's; host-tested in `tests/host/demo1.cpp`), `demo.h` (the parts' interface, a
  4,096-entry sine table), `parts/copper.c`, `parts/plasma.c`; `app.desktop` (full screen
  suggested). **`dist/demo1`** builds `apps/demo1.app` (259 KB), checked with `bmds-dist build`.
- **The picture:** 640×360 L8, `BMDS_GAME_REPAINTS` (every pixel repainted, nothing copied
  forward). The loop draws on its ticks, so the show has an empty tick; its time is the clock's
  (a stall over 0.1 s doesn't jump it). Keys: Space the next part, 1-9 a part, P pause, F the
  overlay (frames a second, the time a frame and its drawing, each core's busy share from
  `cpu.coreN.busy_us`), Escape. Raw keys for now; actions in `controls.conf` at the polish step.
- **Part 1, copper bars and a sine scroller:** the bars are the copper way, all palette: every pair
  of rows is its own entry, the picture of those rows is still (copied in by the 2D-DMA each frame,
  `bmds_game_dma_copy`), and the sky and eight dark-bright-dark bars are 180 entries worked out
  each frame. The message is drawn once into a strip in unscii at 4× (64 pixels high, sharp: the
  look), and each column of the screen copies a column of it moved by a wave, its rows a band of
  64 entries that cycles. The pointer across bends the wave; a click flashes white.
- **Part 2, plasma:** three waves summed (across, a table a frame; down, a value a row; a turning
  wave stepped along each row in fixed point) through a table to 252 colours, the palette turned
  two steps a frame; one value for each pair of pixels, four pixels a 32-bit store.
- **Measured on the board** (full screen, the overlay):

  | | first build | now |
  |---|---|---|
  | copper | 19.6 ms drawing, 50 fps | 6.7 ms, 60 fps (core 0 35%) |
  | plasma | 16.5 ms, 57 fps | 11.1 ms, 60 fps (core 0 69%) |

  What it took: built as a display program (`BMDS_DISPLAY=1` for `bmds-cc`, `display = true` in the
  recipe), or it shares the presenter's core (the first run: 25 ms, core 1 at 100%); musl's
  `sinf` does its work in doubles, which this core does in software (a quarter of the copper's
  time at 640 calls a frame), hence the table; the bars moved from ~300 KB of row fills into the
  palette; the plasma's pairs and word stores.

### Step 2: stars into letters (2026-11-06)

- **Part 3, `parts/stars.c`:** 900 stars rush at the viewer as 3D points (depth-cued along a
  32-entry ramp, fog 2-55); from 4 s to 8 s they slow and gather onto the letters BMDS, each star
  given a place along the letters' edges (as many a unit of edge as there are stars to go); at
  9.5 s they let go and rush on. The letters are outlines extruded by libbmds3d
  (`bmds_3d_mesh_extrude()`, B's and D's holes extruded on their own, which in wire are the
  letters' holes), drawn in wire, back faces culled, faded in through their own ramp's colours as
  the stars arrive, and swinging (`0.8 sin`, always readable). Dragging turns them; a click
  flashes. Points are 2 pixels while they draw the letters.
- **Measured:** about 9 ms a frame, 60 fps full screen.
