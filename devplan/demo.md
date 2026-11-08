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
| 4½ | **Bestagons** (added 2026-11-07, the user's) | A honeycomb of random colours, one flaring on each beat, moved only by its palette; pillars rising out of it in waves; a ball of hexagon tiles flying together and popping on the beat | palette, 2D-DMA, many small draws ordered on the ground [3D flat] |
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
- sections: intro (8 bars), build (8), drop A (16), groove (8), hive (16), breakdown (8), build
  (8), drop B (16), finale (8), outro (8): 104 bars, about 3 minutes.
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
  hide it. **Done in §8c.** Bigger strip halves would cut the ~30 PPA jobs a frame and their fixed cost, and the
  strip's cache write-backs run with interrupts off.

### 8c. Done: the rows fetched by the 2D-DMA (2026-11-07, the user's go-ahead)

**Measured first** (`prof -c 1`, demo1 with the wavetable, 60-90 s in): core 1 92% busy, the
presenter 57% of it and the wavetable 32%. Of the presenter: `repeat8` 64% (37% of the core),
nearly all of it waiting on PSRAM for the program's frame; critical sections ~19% (cache
write-backs, the 2D-DMA driver's connects, semaphores).

**Built:** the rows a strip half takes are fetched by the 2D-DMA (`BlitChain`, one piece a job)
into the half's end while the PPA expands the other half; the CPU repeats them forward in place
from internal RAM (PIE's reads stay ahead of its writes when a row is whole 16-byte steps; other
rectangles are read from PSRAM as before). The lines a fetch fills are invalidated just before it
(the half is written back by then, so rounding out to whole lines loses nothing). Kept from the
first try's lesson: wait for the PPA's last job before the fetch's. Waiting on the fetch first made
two blocks a job, each two task switches and their critical sections, which ate most of the gain
(the presenter 57 → 55% of core 1); after the PPA's ~300 µs job the fetch is done and its wait
doesn't block.

| | before | fetch, two waits | one wait | lines invalidated per fetch |
|---|---|---|---|---|
| `surface.present` demo_l8_640x360_full, system CPU a frame | 6,388 µs | 4,110 | 3,804 | 3,718 |
| demo1, core 1 busy (presenter's share) | 92% (57%) | 89% (55%) | 80% (41%) | |

Doom's whole-frame case 3.6 → 3.0 ms a frame, Quake's 4.4 → 3.5; all at 60. Screenshots of the
boot's text and the tunnel pixel-exact. What's left of the presenter is per job (~30 PPA jobs and
30 fetches a frame): the semaphores' blocks, the cache write-backs (interrupts off), the 2D-DMA
driver's connects (~17 µs a job). Bigger halves would cut them, at internal RAM's cost.

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

### Step 3: the song, the sync, the cue timeline (2026-11-06)

- **`song.c`:** the track as sequencer data, 140 BPM in A minor, Am-F-C-G a bar each, nine tracks:
  kick, clap, hats (sixteenths under off-beat open ones), an acid bass following the chords (Synth
  Bass 2, its filter on 74 opening and closing over four bars: heard once the synths take 74, K3),
  a sawtooth arpeggio, a warm pad in three voices, a crash every eight bars. Seven sections, 72 bars:
  intro 8, build 8 (the kick from quarters to sixteenths, a beat of silence), drop 16, break 8,
  build 8, drop 16 (a busier bass, the pad back), outro 8, then round again: 2 minutes 3, not the
  3½ of §4 (whose bar counts make 2 minutes; longer sections when the other parts exist). The
  patterns are written out by code at the first call (the rules are shorter than the steps).
  Host-tested: the song valid, its events in order, no note started over itself, its sections
  where they should be round the loop (`tests/host/demo1.cpp`).
- **The sync (`sync_t` in `demo.h`):** each frame, for the moment it is seen
  (`bmds_game_frame_ns()`): where the song is, each of kick, clap, hats and bass as its last note's
  loudness decaying (τ 0.15, 0.2, 0.06, 0.1 s), the notes since the last frame, a pulse falling
  through each beat. Parts read it; none keeps the music's state.
  - **Copper:** the kick lights the bars and the sky and widens the bars' swing, the clap lifts the
    scroller's wave, each kick jolts the text's colours on.
  - **Plasma:** the cue chooses the mood (deep blues and greens at the break), the kick turns the
    palette faster, a clap flashes it.
  - **Stars:** round eight bars of the music, not seconds: they gather over bars 3-4, hold, let go
    at 6½; the letters stay, swinging, and fade over the last bar; the kick surges them, the clap
    brightens the letters.
- **The cue timeline:** each section entered chooses its part (intro copper, build plasma, drop
  stars, break plasma, build copper, drop stars, outro copper); a part chosen by key holds until
  the next section. M stops and starts the music, W switches FM and the wavetable (both from the
  song's start until live control, sequencer step 2), `-n` none, `-W` the wavetable.
- **FM by its name:** `/dev/midi` is whichever synth the user set (the wavetable on this board), so
  the demo asks for the port named `fm` (the sequencer takes names now); the overlay's second line
  says which, and where the song is.
- **Internal RAM:** music and full screen didn't fit together at first: whichever started second
  failed ("full screen: Out of memory" into a window, or "fm: real-time audio: Out of memory" and
  silence). Program thread stacks take internal RAM first; with libbmds3d's helper at 12 KB, the
  player at 8 KB and demo1's own at 20 KB (`stack =` in the recipe) both fit, with 18 KB to spare
  (docs/platforms/esp32p4.md §4c). The music still starts a quarter second in, after full screen.
- **A kernel panic found on the way:** with several full-screen instances started over each other
  and killed (a test script's mistake), the scanout's interrupt read a freed surface: a switch
  request left pending by a `show()` that timed out, or the probation fallback, still pointed at a
  full-screen surface its program had freed. `Scanout::forget()` now drops both under a lock the
  switches take, and refuses while the surface is the one sent; `ProgramSurface` calls it (showing
  the desktop first if needed) before freeing, and keeps the surface rather than free it under the
  scan. The same start-and-kill stress ran clean before and after the fix, so it isn't the original
  sequence; the fix is from reading the code.
- **Measured with music** (full screen, FM, the overlay): copper 58 fps (drawing 11.1 ms), plasma
  54-60, the stars at the drop 55. The synth is a real-time thread that takes 19-25% of a core and
  lands on either core (above); when it lands on core 0 it takes its time from the drawing.
- **The synths placed** (the user's choice): their real-time threads on core 1, the mixer on core
  0 (it too landed by chance). The wavetable then costs plasma almost nothing (57-58 fps), FM more
  than before (41-45: the presenter misses frames with core 1 at 62-64%, not yet explained; on core
  0, 49-52). Table in docs/platforms/esp32p4.md §4c.

### Step 4 (part): four more parts, the hook, a longer song (2026-11-06)

The user's ask: more scenes, 2D and 3D, and a catchy melody. Built ahead of their libraries' steps
where the SDK already had what they need (textures, step 6, not needed yet: the tunnel is a table).

- **Tunnel (`parts/tunnel.c`, 2D):** a half-size table (448×252 cells, 16 bits each: u round, v
  along; the shade a 256-entry table of v), larger than the screen so the window onto it swings
  the far end about (the pointer steers it); a 256×64 texture of 32 values (XOR bricks, dark
  mortar) in seven palette rows near to far, each row cycled. The kick pushes the flight and turns
  the colours, the clap twists. 2×2 pixels, two cells a pair of word stores.
- **Fire (`parts/fire.c`, 2D and wire):** a half-size heat field, each cell the average of the
  four below less a cooling read from a moving noise table (flicker), fed at the bottom (hotter on
  the kick); libbmds3d draws a wire icosahedron, then the torus knot, *into the heat field* at its
  hottest (the field is a canvas), so the shape burns. A click throws a fireball. Two heat fields, each
  frame's made from the last's; rows with nothing below stay empty without being worked out.
- **Vector balls (`parts/balls.c`, 3D):** 216 balls easing between a cube of 6³, a sphere, a torus,
  a double helix and a rippling sheet every two bars; `bmds_3d_project()` and
  `bmds_3d_sort_depth()` (from last frame's order: insertion sort, nearly sorted), sprites in 16
  sizes drawn a span a row, four colour ramps; a checkered floor stepped across in fixed point,
  four pixels a store, and a sky the 2D-DMA copies. A click blows them apart.
- **Solids (`parts/solids.c`, 3D):** a Gouraud torus knot (64×5: 640 triangles) pumping on the
  kick, its colour going round the wheel a bar at a time; four flat polyhedra orbiting, spun up on
  each eight-bar phrase or a click; a wire floor (6×6); the light from the pointer, the clap
  lifting the ambient.
- **The hook (`song.c`):** an eight-bar melody over the chords in 3-3-2 (dotted eighths, an eighth),
  its second half answering and climbing to C6, ending on A held over G; a square lead (GM 80) in
  the drops and the finale, a vibraphone (GM 11) an octave up from the break on. Eleven tracks,
  nine sections, 88 bars (2 minutes 31): intro (copper), build (plasma), drop (stars), groove
  (tunnel), break (fire), build (balls), drop (solids), finale (stars), outro (copper). The host
  test counts the hook five times and the vibraphone six.
- **Measured** (full screen, the overlay, drawing / frames a second):

  | | no music | music, intro | music, its own section (FM; the wavetable the same) |
  |---|---|---|---|
  | tunnel | 10.3 ms / 60 | 14.2 / 58 | 13.0 / 41 |
  | fire | | 17.3 / 52 | 14.2 / 40 |
  | balls | | | 14.6 / 43-45 |
  | solids | | 17.3 / 53 | 19.1-19.5 / 37-38 |

  Profiled (`prof`, `addr2line` on a `-g` build of the same code), then: the tunnel's lookups,
  the fire's two passes, the balls' per-pixel tests and sky `memset`, the solids' floor lines (as
  much as half the knot). Fixed what was cheap (16-bit table, the 16 KB texture, spans, DMA sky,
  word stores, a lighter knot and floor); worth 1-3 ms each.
- **What holds them at 40: the synth beside the presenter, not the parts.** Without music the
  tunnel draws in 10.3 ms and runs at 60; with it, ~40. Measured after (docs/platforms/esp32p4.md
  §4c has the numbers): the PPA's CLUT needs ~9 ms of each frame at 1280×360 (its own rate); the
  synth's real-time thread preempts the presenter between PPA jobs and the PPA idles, so the
  conversion takes 13.5-15.5 ms instead of 10.5 and misses vsync on every other frame. Tried: the
  presenter above the synth (60 fps, but the synth missed its periods: silence), rows fetched by
  the 2D-DMA (less CPU, the same wall time), internal RAM's uncached address (PIE faults there), the
  synth on core 0 (the new parts +5-9 fps, plasma -8), the parts' rows on both cores (nothing:
  memory-bound). Kept: the PPA bench cases, the parts' row loops as functions, the fire's two
  fields. The synths stay on core 1 (the user's choice). What is left is the synth's own cost
  (35-40% of a core, mostly memory stalls): the next step, with an internal-RAM budget.
- **FM's waveforms in internal RAM (2026-11-07, the user's choice: 16 KB while FM plays):** eight
  2 KB blocks copied at open, given back at close (5 s after the last note); any without room read
  from flash, said in the log, host-tested (the same samples either way). To make room, a display
  program's stacks keep 128 KB more of internal RAM free (the loader), so demo1's go to PSRAM.
  Result: 35 KB of internal RAM free during the show (lowest 32 KB, from 5.6); but FM still takes
  42% of core 1 in drop B, so the flash reads weren't its cost; frame rates within noise (plasma
  57, stars 55, tunnel 44, fire 40, balls 45, solids 40). Next: FM's voice loop profiled per voice.
- **The synth renders ahead (2026-11-07, the user's idea; `audio.md` §27):** the music is all
  scheduled, so demo1 allows the port 250 ms (`bmds_seq_config_t.allow_latency_us`) and FM writes a
  PCM ring below the presenter instead of meeting each period at 19, raising itself above it only
  while the ring is under a quarter full (5 times in the whole show). Frame rates with FM: plasma
  59, stars 60, tunnel 59, fire 51, balls 56, solids 41; no underrun, no late period or event. Fire
  and solids are their drawing's now (18.4 and 23.2 ms on core 0); FM's own cost (31-42 % of core
  1) still matters, since at 60 fps the presenter takes 57 % of it: profiled per voice next.

### Step 6 (part): the boot, the credits, speech on cues, transitions (2026-11-07)

The user's ask: every remaining piece, the BIOS screen talking, random phrases and details so each
run differs, and the APIs grown where the demo needs them (it's what a game would need too).

- **The boot (`parts/boot.c`, part 0):** a 1990s BIOS screen whose facts are the board's own
  (`bmds_system_info()`: board, chip revision, cores and clock, serial number, host name;
  `bmds_mem_info()`: PSRAM counted up as it is "tested", internal RAM), six checks of 26 picked
  at random ("Calibrating the flux capacitor ...... 1.21 GW"), and a voice picked each run (en-US
  or en-GB, pitch and speed varied) that greets, reports the memory, remarks on two of the checks
  and signs off, each from a pool. A line that talks waits (1.5 s at most) for its speech. DEL
  asks for SETUP, which it refuses aloud. Its script over, the show starts the music, and the
  song's first beat tears the screen into the intro.
- **The credits (`parts/credits.c`, part 9, the outro):** a starfield, the roll (who, the music and
  the synth playing it, the libraries, the chip's parts, four greetings of eleven at random), each
  heading read out as it reaches the middle (two speech slots in turn, the next made while one
  plays), and the system's numbers live under it.
- **Lines on cues (`demo1.c`):** a pool for each section ("Not one pixel of this picture moves.
  Only the palette does.", "This is an E S P 32 P 4. 360 megahertz. No G P U."); one picked as the
  section before starts, made by the speech thread, placed on its beat with `bmds_game_play_at()`
  at the time `bmds_seq_time_of()` gives, or not said if made too late.
- **Transitions:** in the last half beat before a section (`bmds_seq_time_of()` again) the part
  goes, and the next is revealed over 0.35 s: a fade through black, a white flash, blinds, or a
  tear (rows thrown sideways), picked at random each time; the boot always tears.
- **Speech (`voice.c`):** libbmdsgame's mixer opened ahead (250 ms, `BMDS_GAME_AUDIO_AHEAD`: no
  deadline beside the drawing, as the synth's), a thread that says lines into sounds
  (`bmds_game_sound_speak()`), slots the show owns; a sound a voice may still read is kept until
  the mixer lets it go.
- **Options:** `-1` plays the song once and exits; `-s` is silent (no speech); key 0 is the boot.
- **The SDK grown for it** (static libraries, no ABI): `BMDS_GAME_AUDIO_AHEAD` and `latency_ms`,
  `bmds_game_audio_latency_ns()`, `bmds_game_play_at()` (to the frame, host-tested),
  `bmds_game_sound_speak()` (`games.md`); `bmds_seq_time_of()` (`sequencer.md`). In the kernel:
  a cancel (`BMDS_MIDI_CANCEL`) by a program that allowed latency now drops what the synth made
  ahead for it and silences its voices (`audio.md` §27), so M and W stop the music at once.
- **The rotozoomer and the cube (`parts/roto.c`, part 8, the finale):** a 128×128 picture (the
  logo on a checker of two ramps) turned and zoomed, a texel for each 2×2 pixels as the tunnel
  draws (9.5 ms; full-screen `bmds_canvas_texture_affine()` was 16 ms, its 256×256 texture 21);
  from bar 4 it dims and a cube flies in, `TEXTURED_FLAT` with perspective through a light table,
  its faces a 64×64 plasma made each frame (`bmds_canvas_image()`), a small cube of the logo going
  round it.
- **Found on the board:**
  - The boot drew all its text each frame (a system call a string: 10.5 ms); now each line is drawn
    once into its own picture when it changes, which the 2D-DMA copies out (5.0 ms, core 0 20 %).
    The credits likewise: their lines drawn once into keyed images (37 → 7.5 ms).
  - The game mixer ahead at `SCHED_FIFO` 1 took 13 % of core 1 above the presenter while silent,
    and speech made beside the drawing took half its core: fire fell 48 → 25 fps. Fixed in
    libbmdsgame (`games.md` §28) and with `SCHED_IDLE` (`processes.md`); speakers stay open.
  - Only en-US is on this board (the others are the speech-voices package): a voice not there
    falls back to the system's.
- **Measured** (the whole song with FM and speech, the overlay on; fps): boot 59, copper 60, plasma
  56, stars 58, tunnel 57, fire 48, balls 58, solids 40, rotozoomer 60, the cube 38, credits 60.
  The solids and the cube wait for their band on core 1, which the presenter and FM fill (93-99 %):
  FM's own cost is next, then the 3D split.
- **After the user's first watch** (speech that glitched and talked over itself; ideas):
  - The glitches were the speech stream's underruns mid-line and at each line's start: its mixer
    now keeps to the game's core, leads with silence and stays awake between lines (`games.md`
    §28). Lines said in turn no longer overlap: `voice_queue()` plays a line after the last one
    played ends (the boot's lines, its SETUP refusal, the credits' headings); the screen goes on.
  - The music ducks under every line (to 0.5, -6 dB, over 120 ms before it; back up over 0.6 s,
    0.3 s after it unless another line follows; 0.3 was too much, the user's second watch): `bmds_seq_fade()` (`sequencer.md`). The
    limiter that had caught 5 frames in a run now catches none.
  - Two voices talk over the show: one low and slow (pitch 55-70 %, speed 85-95 %), one higher and
    quicker (115-130 %, 100-110 %; 150-180 % was hard to follow), picked each run. Each cue is now an exchange of up to four turns
    from a pool ("Two hundred and sixteen balls." "Sorted every frame." "Every frame?" "Every
    frame."), either one starting.
  - The credits' headings were cut off: the next heading was asked for in the other slot while the
    line there still waited in the queue, and asking stops a slot's line. The headings are no
    longer read; the duo chat over the credits instead (the outro's cue, made during the finale):
    two bits of 32 at random (8-bit machines, retro computing, graphics, films, programmers'
    jokes: "Did you blow on the cartridge?" "There is no cartridge." "Then what have I been
    blowing on?") and one sign-off of five, eight turns at most in slots of their own. A cue
    whose beat comes while the chat still talks (round the loop) is queued after it; `-1` exits
    once the song is over and the chat said.
  - The flux capacitor's remark says "jigga watts", as the film does.
  - The music plays on the wavetable by default (`-F` for FM): a thread during the boot opens a
    synth fd of the wavetable's own (`audio_synth`), which loads its SoundFont into the system's
    cache, and the music waits for its answer; if it can't load (no font, no memory), or the port
    can't be opened, FM plays instead.
  - The user's wavetable run lost its speech halfway: lines are made at `SCHED_IDLE`, and beside
    the wavetable (raised above the presenter 200-1000 times a run) and the fire part, a section's
    three lines took 12 s, after their cue. Each section now has its own slots (48 in all), asked
    three sections ahead and freed two after, made oldest request first; a whole run then places
    every cue, round the loop too. PSRAM free with the wavetable's font and the lines: 2.6 MB at its
    lowest. The user's next run still went quiet halfway: the speech thread was on core 1, which
    the presenter (52 %) and the wavetable (38 %) fill, and got 10 % of it; made in the order asked,
    once behind it made lines whose cue had passed, and never caught up. Now a section whose cue
    passes unsaid has its lines dropped at once (made or not), and the thread keeps to the game's
    core, where the drawing waits each frame (38 % idle there, 11 % on core 1, in the same run):
    over two loops it kept up and waited for work.
  - Stuck notes on the wavetable: `bmds_seq_fade()` sent events out of time order, which the port
    refused, and the player dropped the note-offs after them (`sequencer.md`).
  - BMDS is said "Bum Doss" (the text is respelled for speech only), and "Bare metal. Do stuff."
    became "Look! It's bare metal, doing stuff!".
  - A voice not installed is remembered and not tried again; a speaker is opened before the oldest
    is closed, so a failed open keeps it.
  - Measured again (the whole song; fps, a second's average): boot 60, copper 60, plasma 60, stars
    60, tunnel 59, fire 58, balls 59, solids 47, rotozoomer and cube 55 (47-61), credits 59.

### The bestagons (2026-11-07, the user's request)

- **`parts/hive.c`, "Bestagons"**, on a section of its own: `hive`, 16 bars after the groove (the
  song 88 → 104 bars: kick, clap, hats, the plain bass, the lead, the pads and the hook; no bells).
  Three phases by the bar:
  - **0-4, a honeycomb all palette:** a 640×360 picture made at init (pointy-topped cells 22 pixels
    to a corner, faces, bevels lit from the top left or shaded, gaps), each cell one of 8 colours at
    random (a hash of its place and the run's seed; each colour three entries: face, lit bevel,
    shaded bevel), copied in by the 2D-DMA (slot 3). Only the palette moves (the user's design): on
    each beat one colour, never the last, flares bright and pale and eases back over about a second;
    the 8 colours are spread round the wheel, shuffled and varied each run. The first version (a
    spiral arm of 48 phases, cells popped by the CPU) gave way to it.
  - **4-12, pillars:** 37 hexagonal prisms rising out of it (seen from straight above first: their
    tops are the honeycomb), the camera tilting and circling, heights in waves, the bass lifting the
    middle, a ring a beat on the kick, a ramp a ring. Each prism one draw, its bounds centred on
    its base: the draws' order by centres is then by place on the ground, back to front for things
    standing on a plane whatever their heights. Each draws only its top and the three sides that can
    face the camera (one of six meshes, chosen by the side most towards it): 18 vertices and 10
    triangles, nothing culled.
  - **12-16, a Goldberg ball:** the dual of a twice-split icosahedron made at init (each vertex a
    tile, each triangle's middle a corner; corners sorted round, pulled in for gaps, put on the
    tile's plane): 150 hexagons and 12 pentagons, coloured by distance from the pentagons (a
    breadth-first walk), three meshes whose positions are rewritten each frame: tiles fly in late
    by a random delay, pop out in a band that turns round it on the beat, burst on a click, fly off
    in the last half bar. A coarse core (80 triangles) shows through the gaps.
- **Measured** (the part alone, the wavetable playing): the honeycomb 4.4 ms (the 2D-DMA's copy 2.7
  of it: a fill costs the same, and the CPU waits for either), the pillars 9-11 ms, the ball 11-12;
  57-60 fps with the overlay. Found: 61 pillars cost 13-16 ms. Not the draws (host: 61 draws and one
  mesh of the same triangles cost the same) nor the vertices (18 instead of 30 a pillar saved 0.6
  ms): in the show a triangle costs about twice the 3D bench's (~5 µs queued, the drawing by
  rows), with core 1 full of the synth and the presenter and the PSRAM shared with the scanout.
- **`-p PART`** holds the show on one part (numbered as the keys), the music playing and no boot:
  for watching and timing one part. The keys 1-9 now reach the parts before the credits.
- Speech slots 48 → 52 (four for the new section).
- **The teapot burns in the fire instead** (the user's, after seeing it on the pillars): bars 0-1
  of each eight the icosahedron, 2-3 the knot, 4-7 Newell's teapot, upright, turning and rocking,
  drawn wire into the heat field at its hottest so flames rise off its edges. `teapot.bm3d` in the
  app folder, made by `dist/demo1` with `bmds-mesh3d` (built by `$HOSTCC`) at detail 3 (648
  triangles, 18 KB), loaded at init by `bmds_3d_model_load()`; no file, the knot burns on. Its
  wire is the patches' grid lines: `bmds-mesh3d` now leaves out the sides inside a polygon (a
  patch's quad's diagonal, an OBJ polygon's spokes), which the edge finder kept wherever the two
  triangles weren't in one plane (all over a curved teapot). Same cost as the knot on the board.
