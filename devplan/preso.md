# Preso: Presentations Written in Markdown

Planned 2026-10-30 with the user (the user: a presentation app in the spirit of Marp, decks written
in Markdown with images, a look set apart from the content, the retro aesthetic kept; built as an
SDK program on the card; the talk about BMDS given on BMDS).

The program is `preso` (the user's name, 2026-10-30).

Status: **steps 1 to 5 built** (2026-11-03, §14).

Revised 2026-11-03 with the user: AngelScript in decks for small animations and interactive
pieces in the spirit of Manim, the bulk of a deck still Markdown (§7); both forms of script
(regions and `angel run` blocks), sandboxed, with Previous through a script's beats by re-running
it (§7.6); and written to be **ported to the PC later, probably on SDL3** (§2): Preso is likely
useful beyond BMDS.

## 1. Goal

- **A deck is a Markdown file** with its images beside it. Anyone who knows Marp writes one at
  once, and a deck can be previewed in Marp on a PC.
- **The look lives in a theme**, apart from the content: colours, fonts, spacing, headings,
  bullets, code, backgrounds, effects. Retro themes are built in; a deck can bring its own.
- **Small animations and interactive pieces in script** (AngelScript, angel's language): a sort
  shown step by step, a diagram that builds, a slider to drag. A few lines beside the Markdown,
  never instead of it (§7).
- **Presenting is comfortable:** a keyboard, a USB clicker (a keyboard) or a gamepad moves through
  the deck; the speaker's notes show on a laptop's terminal over SSH.
- **Live demos from the deck:** a slide can start a program, and the deck comes back when it ends.
  The talk steps from its slides into the real system and back in one take.
- **Writing a deck is quick:** the deck reloads when its file, theme or images change, staying on
  its slide, so a deck edited on a laptop over sshfs changes on the screen as it is saved.
- **An SDK program on the card** (`programs/preso`, `dist/preso`), not part of the OS. What it
  needs that would serve other programs too goes in the kernel or the SDK, as ordinary
  additions (§9).
- **Portable:** everything but a thin platform layer builds anywhere, so a PC port (SDL3) is a
  new layer, not a rewrite (§2).

Not goals: editing decks on the board beyond what the text editor already does; Marp's HTML and
CSS (themes are our own format, §4); PDF or PowerPoint output.

## 2. Structure: a portable core and a platform layer

Preso is written for a PC port from the start. The rule: **the core includes no platform header**
(no `bmds/sys.h`, no SDL), as `bmds_core` includes no ESP-IDF header. It builds on the host,
where the tests run it, and that build is the first half of the PC port.

| Part | Where | What |
|---|---|---|
| core | `programs/preso/core/` | the deck parser, themes, layout, drawing into a pixel buffer, transitions, the script host (§7), the presenting state machine (steps, beats, overview, go to), notes text |
| BMDS platform | `programs/preso/bmds/` | the surface (`libbmdsgame`), Controls, the font calls, `bmds_image_decode`, sound, `fs_watch`, running demos, the syntax call |
| headless platform | `programs/preso/headless/` | no screen: `preso check` and `preso export`, on the board and the host; the host tests |
| SDL3 platform | `programs/preso/sdl/` (later) | §12 |

- **The platform interface** (`core/platform.h`, one abstract class of a few calls, the core's
  only way out):
  - **Surface:** open (window or full screen, a size), present a pixel buffer (a dirty
    rectangle), hide and show (§6), the display's frame tick.
  - **Input:** events as the core's own types (key with a name, pointer, wheel, text); actions
    (Next, Previous...) already resolved from bindings where the platform has them (Controls on
    BMDS, a bindings file of Preso's on the PC).
  - **Images:** decode a file to pixels at a size.
  - **Fonts:** a face by name and size; draw a run of text into the buffer; measure it.
  - **Sound:** play a file (an effect), stop it.
  - **Files:** read a file, watch a deck's folder for changes.
  - **Processes:** run a command and be told when it ends (§6).
  - **Syntax:** colour a line of code in a language (§9).
  - **Clock:** monotonic time.
- **What the core uses that is portable already:** md4c; AngelScript and `libangel` (§7.1).
  The canvas is the core's own (`core/canvas.h`, fills and blends in RGB565 or XRGB8888), not
  `libbmdsgame`'s: that one's C is portable, but its header is the BMDS SDK's (`bmds/game.h`
  includes `bmds/sys.h`), which the core can't include (as built, §14). Text is drawn into it by
  the platform.
- **What the PC must bring itself**, where the board has a system call: fonts (the OS's font
  library, `stb_truetype` with its glyph cache, is host-built for the tests and can be compiled
  in), image decoding (`stb_image`), audio decoding (the vendored `dr_flac`, `stb_vorbis`,
  `minimp3`), syntax colouring (`bmds_core`'s `textsyntax.cpp` with its few headers, compiled
  in). Each stays behind the platform interface, so the core never knows.
- **Language rules** are the SDK's on every platform: C++20 without exceptions, RTTI or the
  standard library (small containers of the core's own, as angel's `Text` and `String`). That is
  what the board needs, and it builds the same with any compiler on a PC.
- **Pixels:** the core draws in one format (RGB565 on the board, for memory and the 2D-DMA;
  the PC layer may ask for XRGB8888). The format is a parameter of the core's buffers, not a
  constant spread through the drawing.

## 3. Building blocks

- **`libbmdsgame`** (`games.md`): on BMDS the surface (a window or full screen, Alt+Enter), the
  frame loop that only draws on change, images (PNG, JPEG, BMP), fonts (`bmds_game_font()` over
  the font library), the 2D-DMA for big copies, sound; its canvas everywhere (§2).
- **Controls** (`controls.md`): the deck declares actions; keyboard and pad bindings come from
  `controls.conf` and the user's changes.
- **md4c** (MIT, plain C, CommonMark with tables and strikethrough, a callback parser that keeps
  no tree): vendored in `thirdparty/md4c`. A subset parser of our own would grow into it anyway.
- **`libangel`** (§7.1): angel's runtime as a library, so decks' scripts are angel's language.
- **Fonts on the board:** IBM Plex Sans (regular, bold, italic), Mono (regular, bold), Serif
  (regular), and unscii. A bitmap font has no bold: it is drawn twice, a pixel apart.

## 4. The deck format

Marp's, where Marp has the thing; our extensions are directives Marp ignores.

```markdown
---
theme: vga
paginate: true
footer: BMDS · 2026
transition: wipe
duration: 25m
---

<!-- _class: title -->
# BMDS
## Bare Metal Do Stuff

---

# What's in it

* a DOS-like shell        <!-- `*` lists appear one item at a time (Marp's fragments) -->
* windows and a desktop
* programs from the card

![bg right:40%](images/desktop.png)

<!-- Speaker notes: any comment that isn't a directive. -->

---

# Quicksort

![script w:800 h:450](anim/quicksort.as)

---

<!-- run: luckyduck -->
# Lucky Duck
```

- **Slides** are separated by `---` lines. The front matter (the first `---` block) sets
  directives for the whole deck; `<!-- name: value -->` sets one from that slide on, and
  `<!-- _name: value -->` for that slide only (Marp's local directives).
- **Directives:** `theme`, `class`, `paginate`, `header`, `footer`, `backgroundColor`,
  `backgroundImage`, `color`, `transition`, `size` (Marp has these); ours: `run` (§6), `sound` (a
  sound played as the slide appears), `align` (vertical: top, middle), `interactive` (§7.5),
  `auto` (a time, then the next step by itself: kiosks, §5), `duration` (the talk's length, for
  the pace in the notes).
- **Images** take Marp's keywords in their alt text: `bg` (a background), `bg left` and
  `bg right` with a share (`right:40%`) split the slide, several `bg` images tile, `contain`,
  `cover` and a percentage scale, `w:` and `h:` size an inline image, and Marp's filters
  `grayscale` and `sepia` (applied once at decoding). A missing image draws a marked box and
  `preso check` reports it.
- **Scripts:** an image whose file ends in `.as` and whose alt text starts with `script` is a
  script region (§7.2); a code block whose info string is `angel run` shows its code and runs it
  (§7.3).
- **Markdown:** headings 1-3, paragraphs with bold, italic, code and strikethrough, links (shown,
  not followed), lists with nesting, numbered lists, block quotes, code blocks (coloured by
  language, §9), tables, horizontal rules, inline images. HTML other than comments is shown as
  text.
- **Fitting:** Marp's `<!-- fit -->` on a heading scales it to the width. Overflow is not
  shrunk silently: it is cut at the slide's edge and `preso check` reports it.

## 5. Themes

A theme is an INI file, as our other configuration (`share/preso/themes/NAME.theme` on the card,
or a path beside the deck). Keys are inherited: a section names only what differs.

```ini
[Theme]
Name=VGA
Size=1280x720          ; the slide's size, scaled to the screen
Scale=smooth           ; or nearest: Size=640x480 drawn in chunky pixels
Palette=ega            ; optional: images and colours mapped to it, dithered
Effects=scanlines

[Slide]
Background=#0000aa
Color=#ffffff
Font=unscii 32
Margin=64 80
Pattern=               ; a tiled PNG, as the desktop's

[H1]
Font=unscii 64
Color=#ffff55
Frame=double           ; a box-drawing frame around the heading

[Bullet]
Glyphs=■ ▪ ·

[Code]
Font=IBM Plex Mono 24
Background=#000000
Color=#aaaaaa
Keyword=#ffff55        ; the syntax styles' colours (§9), each optional
String=#55ff55
Comment=#5555ff

[Script]
Accent=#ff55ff         ; colours and fonts scripts may ask for by name (§7.4)

[Class title]          ; <!-- class: title --> slides
Align=middle
H1.Font=unscii 96
```

**Built-in themes** (each one drawn in our own style, nothing copied):
- `vga`: blue and white text mode, unscii, box-drawing frames, the EGA palette;
- `amber` and `green`: phosphor on black, a soft glow, scanlines;
- `classic`: the toolkit's grey 3D look, a title bar on every slide, IBM Plex;
- `bbs`: ANSI art colours, block-shade dividers;
- `paper`: plain, light and readable, for when retro isn't wanted.

**Effects** are drawn after the slide: `scanlines` (darkened alternate rows), `glow` (a blurred
copy of the text added under it), `dither` (images mapped to `Palette` by ordered dithering,
which gives the period look and costs nothing per frame once done).

**`Scale=nearest`** draws the slide at its own small size and scales it up by whole pixels: the
look of a 640×480 screen, and a quarter of the drawing.

## 6. Presenting

- **Rendering:** each slide is laid out and drawn into a buffer of its own (on the board in
  PSRAM, 1280×720 RGB565: 1.8 MB), the current one and the next ahead of time, so a step is a
  copy. A transition blends the two buffers. A script region (§7) is drawn over its slide's
  buffer each frame while it moves, and only its rectangle is presented.
- **Transitions:** `cut`, `wipe` (a direction), `dissolve` (an ordered-dither mask growing),
  `pixelate`, `crt` (the old slide collapses to a line and a dot, as a tube switched off, and the
  new one opens from it), `type` (the text appears as if typed, with an optional teletype sound).
- **Actions** (`controls.conf` on BMDS; the user changes them in Controls):

  | Action | Keys | Pad |
  |---|---|---|
  | Next (or next item, or next beat) | Right, Down, Space, Enter, PageDown | A, right |
  | Previous | Left, Up, Backspace, PageUp | B, left |
  | First, last | Home, End | — |
  | Go to | a number, then Enter | — |
  | Black screen, white screen | B or `.`, W | Select |
  | Overview (a grid of every slide) | Tab | Start |
  | Into a script region, and back (§7.5) | — (a click), Escape | X |
  | Quit | Escape (outside a region) | — |

  USB clickers send PageUp, PageDown and B, so they work as keyboards do.
- **A pointer and ink:** the mouse (or the pad's stick) shows a highlight dot while it moves;
  holding its button draws on the slide in the theme's accent colour. The ink is cleared when the
  slide is left. Inside a script region the pointer is the script's (§7.5).
- **Notes on a laptop:** the deck writes each slide's notes, number and the next slide's title
  and first lines to its standard output. Started over SSH (`ssh root@bmds0 preso talk.md`), the
  notes land in the laptop's terminal while the slides show on the HDMI screen, and keys typed
  there move the deck too. No second display needed, and no new IPC.
- **A timer** in the notes: time since the first slide, and per slide; with `duration`, the pace
  ("2:10 behind" from the slides left and the time left).
- **Starting:** `preso talk.md` at the first slide, `preso talk.md 12` at the twelfth.
- **Kiosk:** `preso --loop talk.md` goes back to the first slide after the last; with `auto`
  directives, a deck runs by itself on a table at a show.
- **Reloading:** the platform watches the deck's folder (`fs_watch` on BMDS). A change re-parses
  the deck and redraws, keeping the slide (by number, or by its title if slides moved). A deck
  that no longer parses keeps the old one and says why in the corner and the notes.

## 6a. Live demos

`<!-- run: COMMAND -->` on a slide: stepping onto it starts `COMMAND` (vfork and exec through the
shell, so arguments and `terminal -e ...` work), and the deck gets out of the way. When the
program ends, the deck comes back on the slide after. The slide's own content shows while the
program starts, so a title card can introduce the demo.

To get out of the way the deck **hides its surface** while the demo runs and shows it again after
(§9): minimizing would leave a button in the panel and a full-screen window behind the demo.
On the PC, the same call is SDL's window hide and show.

## 7. Scripts: animations and interactive pieces

The bulk of a deck is Markdown. A script is for what Markdown can't do: an algorithm shown step
by step, a diagram that builds, a curve that moves, something to drag. The model is Manim's
(objects, and animations played on them), small enough for a few lines.

### 7.1 The runtime: `libangel`

- Angel's runtime is already modular (`registerCore`, `registerFonts`, `registerGraphics`,
  `registerSound`... in `programs/angel`, and `registerCanvasDrawing(engine, type, hooks)` gives
  any canvas type the drawing methods). It becomes a static library, **`libangel`**:
  everything in `programs/angel` but `main.cpp` and `debug.cpp`. `angel` links it, and so does
  `preso`. One language, the same float literals and top-level statements, and the same drawing
  methods the guide teaches.
- `libangel` gains **profiles**: what a host registers is a choice, not everything at once
  (§7.6). Angel registers the whole system as today; Preso the deck profile.
- It builds for the host already (the angel tests do it), so it is portable as far as Preso
  needs; the parts behind `ANGEL_BMDS` (windows' presenting, processes, the network) are outside
  the deck profile.
- One engine for the deck, made when the first script slide is reached; a module for each
  script; line cues **on** (unlike angel), so a script that loops without yielding is stopped
  (§7.6). The 25% they cost is small here: the work per frame is native (§7.4).

### 7.2 Script regions

```markdown
![script w:800 h:450](anim/quicksort.as)
![script bg right:45%](anim/orbit.as)
```

- An image whose alt text starts with `script` and whose file ends in `.as`: a rectangle of the
  slide that the script draws in, placed and sized by the image keywords (`w:`, `h:`, `bg`,
  `left`/`right` with a share). The file is relative to the deck.
- Marp on a PC shows a broken image there; `preso export --posters` writes each script's last
  frame as `anim/quicksort.as.png`, and `![script ...](anim/quicksort.as)` may be written as the
  PNG's name with `script:anim/quicksort.as` in the alt text, so Marp shows the poster. (The
  plain form is the one to teach.)

### 7.3 `angel run` blocks

````markdown
```angel run
for (int i = 1; i <= 5; i++)
        println(i * i);
```
````

- The slide shows the code (coloured, §9) as any code block. The **next step runs it**: its
  printed output appears in a pane beside the code (`angel run`: to the right; `angel run below`:
  under it), and its drawing (a `Canvas` it asks for) in that pane instead.
- The same runtime, sandbox and beats as a region: a block is a region whose script is written
  in the slide.
- On a PC in Marp, it is an ordinary code block: a deck still reads well there.

### 7.4 The scene API (Manim's model)

A script sets up objects and plays animations on them; the objects are drawn and the animations
run **in native code** at the display's rate, so a script's own work is a few calls a beat (about
2.4 million statements a second on the board: plenty for setting up, too few for per-pixel work).

```angelscript
auto title = Text("Quicksort", theme.font("H1"));
auto bars  = Bars({5, 2, 8, 1, 9}, theme.color("Script.Accent"));
play(write(title), 0.5);
pause();                          // a beat: waits for Next
play(bars.swap(0, 3), 0.4);
play(fadeOut(title), bars.highlight(3), 0.3);   // played together
```

- **Objects** (retained, in a scene per region): `Text`, `Rect`, `Circle`, `Line`, `Arrow`,
  `Polygon`, `Image` (a file beside the deck), `Code` (coloured, §9), `Group`; and helpers of
  the subject, as `Bars` (an array as bars) and `Grid`. Properties: `x`, `y`, `scale`,
  `rotation`, `color`, `opacity`, `visible`; `add()`, `remove()`.
- **Animations:** `move(obj, x, y)`, `fadeIn`, `fadeOut`, `grow`, `shrink`, `write` (text
  appears letter by letter), `transform(a, b)` (a into b's shape, colour and place),
  `colorTo`, `scaleTo`, `rotateTo`, and `tween(obj, "property", to)` for anything numeric; each
  with an easing (`Ease::InOut`, the default; `Linear`, `Out`, `Bounce`).
- **Time:** `play(animations..., seconds)` runs them together and returns when they end;
  `wait(seconds)`; `pause()` waits for Next (a beat, §7.6).
- **Immediate drawing** for what objects don't fit: a script may define
  `void draw(Canvas@ c, float t)`, called each frame while the region moves, with angel's
  canvas methods (`line`, `fillCircle`, `text`...), drawn under the scene's objects.
- **The theme:** `theme.color(name)` and `theme.font(name)` (`"H1"`, `"Code"`, `"Script.Accent"`)
  so a script fits every theme; `region.width`, `region.height`.
- **Sound:** `Sound("click.wav").play()` from the deck's folder, as angel's.

### 7.5 Interactive pieces

- A script may define `void event(Event@ e)` (angel's `Event`: pointer, wheel, keys) and
  `void tick(float dt)`.
- **Who gets the keys:** the deck's actions stay the deck's (a clicker must always work).
  A click in a region, or the Into action, **enters** it: its keys and pointer are the script's,
  with a thin frame in the accent colour to show it, and Escape gives them back. A slide marked
  `<!-- interactive -->` enters its region as it appears. The pointer's moves and clicks inside
  a region are always the script's (they don't conflict with stepping).
- **Widgets in the scene** for the common cases: `Slider(min, max, value)`, `Button(label)`,
  `Toggle(label)`, drawn in the theme, with a `changed` callback.

### 7.6 Beats, Previous, and determinism (the user's decision, 2026-11-03)

- **Beats** share the slide's step counter with `*` fragments: Next ends a `pause()`; after the
  script's last beat, Next goes on (to the next fragment or slide). Next during a `play()` ends
  it at once (the animation jumps to its end) and goes on to the next beat: a presenter in a
  hurry never waits.
- **Previous re-runs the script** from its start to the beat before, in **fast-forward**: no
  drawing, every `play()` and `wait()` at once, `draw()` not called; then it draws once. The
  same for going to a slide in the middle of a script, and for drawing the next slide ahead of
  time (beat 0).
- **For that, scripts are deterministic:** time is the region's own (`millis()` counts the
  script's played time, not the clock), `random()` is seeded from the deck, the slide and the
  region, and nothing in the deck profile reads the outside world. Interactive state isn't
  replayed: Previous on an interactive slide restarts it.
- **Runaway scripts:** a script that runs more than 100 ms without yielding (`play`, `wait`,
  `pause`, or returning from `draw`, `event` or `tick`) is stopped (a line callback: why line
  cues are on) and its region shows the error. A talk never hangs on a loop.
- **Errors:** a script that doesn't compile, or throws, draws a marked box in its region with
  `file:line: message`, as a missing image does; the notes say it too. `preso check` compiles
  every script and runs each to its end in fast-forward.

### 7.7 The sandbox (the user's decision, 2026-11-03)

Decks are shared, and a deck someone sends must not be able to do harm when it is opened.

- **The deck profile** (the default): the core (strings, maths, arrays; `print` to the region's
  output; no `exit`, no `input`, no `env`), fonts, the scene and canvas, `Image` and `Sound`
  from files **inside the deck's folder** (paths with `..` or absolute ones refused), read-only
  text files there (`readText`, for data). Nothing else: no files elsewhere, no writing, no
  processes, network, settings, notifications, windows (`Screen`) or GUI.
- **`preso --trust DECK`** registers angel's whole system API for that run: a deck of one's own
  that shows live data (the board's CPU load on a slide, say). A deck can't ask for it: only the
  command line gives it. `preso check` says which scripts would need it (what they name that the
  deck profile lacks).
- `run:` demos (§6a) aren't scripts: they are commands, and a deck that has them says so when it
  opens ("This deck runs programs: luckyduck, sysmon"), once, with the choice to run it without
  them. A trusted deck doesn't ask.

### 7.8 The API is ABI

Decks are files people keep, so the scene API, the deck profile and the beat rules are userspace
ABI once released: append only, with a lock of their own (`tests/preso/api.lock`, from
`preso --api`, as angel's `tests/scripts/api.lock`).

## 8. Also from the same code

- **`preso check DECK`:** parses and lays out every slide without a screen; reports missing
  images, overflowing text, unknown directives and themes, script errors, scripts that need
  `--trust`. Runs on the host as well (a test).
- **`preso export DECK DIR`:** each slide, and each beat and fragment, to a PNG, for the README,
  a web page or a PDF made elsewhere; `--posters` (§7.2).
- **A Markdown viewer later:** the same layout in a scrolling window opens `.md` files from the
  file browser (`mimeapps.list`), our own docs included. Not in this plan's steps.

## 9. Additions outside the program

Kept small, and only what other programs would use:
- **Syntax colouring in the SDK:** `bmds_syntax_scan(language, line, state, spans, max)` over
  `bmds_core`'s `textsyntax` (the editor's languages), and `bmds_syntax_find(name or file
  name)`. Today only the GUI's `CodeEdit` reaches it. Preso's code blocks and `Code` objects use
  it; so would a Markdown viewer, `less`-like pagers, angel scripts. The PC port compiles
  `textsyntax.cpp` in instead (§2).
- **Hiding a surface** for a while, and showing it again (§6a): a call on a surface, the window
  manager taking it off the screen and the panel without destroying it.
- **`libangel`** (§7.1): not a kernel change, but a library in the SDK's tree that any program may
  link to embed scripting.
- **Bold and italic for the bitmap font** in the font library, if drawing twice looks wrong.
- Anything md4c needs from libc that musl's static build lacks (none expected).

## 10. Resource budget

- **Program size:** md4c about 60 KB of code, the core about 120 KB, AngelScript 583-724 KB and
  `libangel`'s deck profile about 150 KB: about 1 MB, loaded from the card into PSRAM, nothing in
  the firmware.
- **PSRAM:** three slide buffers (current, next, the transition's) 5.5 MB; scaled pictures kept up
  to 4 MB (as built, §14: two full-slide backgrounds), a picture's decoding while it is scaled (a
  1280×720 JPEG 2.7 MB); the deck's text a few KB; the script engine about 30 KB, and each script's
  module and scene a few KB more. The board has about 6 MB of PSRAM free with the desktop running
  (§14): this is the budget that binds.
- **CPU:** nothing between steps (the frame loop draws only on change). A slide drawn in tens of
  milliseconds ahead of time; transitions at the display's 60 Hz; a moving region only its
  rectangle. Fast-forwarding a script to a beat: its statements without drawing, milliseconds.

## 11. Open questions

- **Typing transitions with sound:** fun, possibly too much for a talk. Off by default.
- **The poster form of script regions** (§7.2): worth it, or is a broken image in Marp fine?
  Decided when a deck is shared with Marp users.

## 12. The PC port (later)

What an SDL3 platform layer (`programs/preso/sdl/`) needs, so the core is written for it now:
- a window or full screen (`SDL_CreateWindow`, a streaming texture the buffer is copied into,
  presented on vsync); hide and show for demos;
- input from SDL's events, and SDL's gamepads, with Preso's own bindings file where BMDS has
  Controls;
- the font library, `stb_image`, the audio decoders and `textsyntax` compiled in (§2); SDL's
  audio stream for sound;
- watching files: the OS's facility (inotify, `ReadDirectoryChangesW`, FSEvents) or, at the
  least, checking the deck's times twice a second;
- demos through `posix_spawn` or `CreateProcess`;
- a **presenter window** on a second display (the notes, the next slide, the timer), which a PC
  usually has: the same text the terminal gets, drawn.
- Built with CMake beside the SDK's build, sharing the core's source list.

## 13. Steps

1. [x] md4c vendored; the core's layout (`core/`, `headless/`; `bmds/` comes with the screen)
   and the platform interface (files, so far); the deck parser (front matter, slides,
   directives, image keywords, script regions and blocks, notes) into a slide model; host tests
   (§14).
2. [x] Layout and drawing with one theme (`paper`): headings, paragraphs, inline styles, lists,
   code (the syntax call in the SDK, §9), quotes, tables; the frame loop and the step actions;
   reloading; `preso check`.
3. [x] Images: backgrounds, splits, sizes, filters; the dithering to a palette (the palette itself
   comes with the retro themes, step 4).
4. [x] The theme format in full and the retro themes; effects; `Scale=nearest` (§14).
5. [x] Transitions, the overview, black and white screens, go to (`*` fragments came with step 2;
   §14).
6. [ ] `libangel` split from angel, with profiles; script regions; the scene API and
   animations; beats, Previous by fast-forward, determinism, the runaway stop; the deck profile;
   the API lock.
7. [ ] `angel run` blocks; interactive pieces (events, entering a region, the widgets);
   `--trust`, and the question before `run:` demos.
8. [ ] Notes and keys over the terminal, the timer and the pace, starting at a slide; pad
   bindings checked with the user's pad; the pointer and ink.
9. [ ] Live demos, with hiding a surface (§6a, §9).
10. [ ] `preso export` and `--posters`; kiosk mode; `dist/preso` with an example deck; the BMDS
    talk itself.

## 14. As built

### Step 1 (2026-11-03): the deck parser

- **md4c 0.5.2** in `thirdparty/md4c` (`md4c.c` and `md4c.h` unchanged; not its HTML renderer
  or entity table). On the board it is 40 KB of code, not the 60 KB guessed (§10).
- **The core** (`programs/preso/core`):
  - `types.h`: `Error`, `StringView`, word and line helpers.
  - `arena.h`: the deck's memory, freed at once.
  - `buffer.h`: text gathered piece by piece.
  - `deck.h`: the model and `Deck::parse()`, which never fails over a mistake in the deck. A
    mistake is a `Diagnostic` with its line; only memory or a deck past 8 MB fails a parse.
  - `markdown.cpp`: md4c's callbacks building the model.
  - `directives.cpp`: directive names, image keywords, entities, times.
  - `platform.h`, with the one call step 1 needs (`readFile`).

  On the board the core is 24 KB of code so far, and the stripped program 290 KB (musl's stdio
  included).
- **Portability is enforced by the build.** The host builds the core as a library of its own
  (`preso_core`, `tests/host/CMakeLists.txt`), with only its own directory and md4c's on the
  include path, so a BMDS header can't creep in. Its tests are a separate program,
  `preso_tests`.
- **How the model is held:**
  - Blocks and inlines are linked lists in the arena.
  - Text points into the deck's source wherever md4c's does. Adjacent runs of one style are
    merged, so a paragraph is a few inlines.
  - Copies are made only for entities, alt text, image paths, code blocks and HTML.
- **Comments.** md4c gives an HTML comment in pieces: a block one line by line, an inline one
  split at its line ends with the line ends not in the source. They are gathered whole, then:
  - `fit` marks a heading;
  - `interactive` is a directive;
  - a comment every line of which is `name: value` with a known name is directives;
  - anything else is a speaker note, and the note is reported when every line has a
    directive's shape (a lower-case name) but a name is unknown (`transtion: wipe`).
- **Directives:**
  - Marp's global, local and spot (`_name`) rules.
  - `run`, `sound` and `interactive` belong to their slide only.
  - `headingDivider` is read from the front matter only; elsewhere it is reported, since it
    would split slides already made.
  - Values are checked where they are written (size, duration, paginate, align, auto,
    transition names).
  - Marp's metadata (`marp`, `lang`, `image`...) is known, so it isn't reported. `style` and
    `math` are reported as unsupported.
- **What `preso check` reports beyond Marp:**
  - `text` followed by `---` on the next line is CommonMark's setext heading, not a slide
    break: reported, because the author almost always meant the break.
  - Image filters other than `grayscale` and `sepia`.
  - A script region whose script isn't `.as`.
  - `left`/`right` on an image that isn't `bg`.
  - Blocks nested more than 31 deep are flattened into the deepest kept, not lost.
- **Images:**
  - Alt text keeps the first run of words that aren't keywords.
  - Paragraphs left with nothing but `bg` images or comments are dropped.
  - `<br>` is a line break; other HTML is shown as text.
  - Entities: 44 named ones and numeric ones; an unknown entity is kept as written.
- **`preso check [-v] DECK...`** (`main.cpp`, on the headless platform): `file:line: warning:
  ...`; `-v` lists the slides with their titles, steps and demos. Exit status 0 when clean, 1
  when something was reported, 2 when the deck couldn't be read.
- **Tested:**
  - 23 cases in `tests/host/preso/deck.cpp`, also run under ASan and UBSan.
  - Every Markdown file in the repository (101) and md4c's own test suite (the CommonMark
    spec and its extensions) through a sanitized `preso check`, with nothing reported by the
    sanitizers.
  - Pathological decks: 20,000 nested quotes, 50,000 emphasis marks, 20,000 slides in 0.7 s,
    unclosed comments, and 200 KB of random Markdown tokens.
  - On the board, `dist/preso` built by `bmds-dist` checked a four-slide deck, with the same
    output as on the host.

### Step 2 (2026-11-03): drawing slides, presenting, reloading

- **Colouring code in the SDK** (§9):
  - `syntax_find` and `syntax_scan`, slots 133-134, in `components/bmds_loader/src/syntaxcalls.cpp`
    over `bmds_core`'s `textsyntax`. The styles' numbers are the editor's `SyntaxStyle`,
    checked equal at build.
  - A language is found by its id, its name in any case, an extension, a file name or an
    interpreter (`#!`'s), or by `c++`, `console`, `shellscript`, `makefile` or `patch`.
  - A call colours at most 128 spans (1.5 KB of the caller's stack).
  - `progtest syntax` passes on the board.
- **The core:**
  - `canvas.h` and `color.h`: the canvas and colours (`#rgb`, `#rrggbb`, CSS's basic names).
  - `theme.h`: `paper` as a `constexpr` (no global constructor), its fields the ones the INI
    format will fill (step 4).
  - `fonts.h`: a font cache. BMDS gives a program 16 font handles, so at most 12 are kept
    open and the least recently used is closed. Fonts are keyed by family, size and style,
    and named for the platform as the font library reads them ("IBM Plex Sans 30 bold").
  - `layout.h`: a slide into a list of draw operations, each with the step it appears at.
  - `presenter.h`: the steps, the picture ahead, reloading, and `preso check`'s work
    (`checkDeck`).
- **Layout:**
  - **Text:** words placed on lines, a line's pieces on one baseline. A word wider than its
    line is broken at characters, through the platform's `textFit`.
  - **Runs:** words of one style with only spaces between them are drawn as one run. Each
    word's width is rounded up, so the run is measured again and what follows moves by the
    difference. Without that, a bold word after plain text sat a space too far, which was
    seen on the board.
  - **Links and strike-through:** the line continues over the spaces between words.
  - **Inline code:** its box's padding counts in its width, so it doesn't crowd a comma
    after it.
  - **Lists:** bullets by depth are `•`, `–` and `›`. IBM Plex Sans has no `◦` or `▪`; the
    first plan's `◦` drew as a missing glyph. Numbers are right-aligned before the text.
    Nested deep, the indent stops once the text would have too little room.
  - **Code blocks:** a box, lines coloured through the platform; tabs go to a stop every four
    characters. md4c expands a tab in a line's indentation itself.
  - **Tables:** columns as wide as their text, or shared out between each one's widest word
    and its whole text. A header row is bold on its own background; cells keep their
    alignment. At most 16 columns; more are reported.
  - **Classes:** `title` and `lead` are put in the middle and centred (the theme's class
    rules); `align: middle` and `align: top` override that.
  - **`fit`** scales a heading to its line, up to the font library's 255 pixels.
  - **Split slides:** a `bg left` or `bg right` image takes its share of the slide. Its box
    is drawn and named until images are (step 3), and the footer and page number stay on
    the text's side.
  - **Directives:** `backgroundColor` and `color` are applied and checked; `size: 4:3` gives
    a 960×720 slide.
  - **Overflow:** content past the bottom margin is reported and cut at the slide's edge; so
    is a code line wider than the slide.
- **Presenting:**
  - **Steps:** a slide's steps are its fragments. Next and Previous go through them, and back
    onto a slide shows it whole.
  - **The picture ahead:** the next step is drawn into a second buffer once the current one
    is shown, so Next is a swap and a copy.
  - **Pixels:** two buffers of the slide's size from the platform (`bmds_mem_alloc` on the
    board), 3.7 MB with the program.
- **Reloading:** a change in the deck's folder reads the deck again. The slide is kept by its
  title if the slides moved (Code, slide 4, stayed Code at slide 3 when a slide before it was
  removed), else by its number. A deck that can't be read keeps the one shown, with a red
  strip along the bottom saying why.
- **The BMDS platform** (`bmds/bmdsplatform.h`) and its own loop:
  - **Why not libbmdsgame's loop:** it wakes at its tick rate whenever the surface is active,
    even with no `tick` callback, and it can't wait on other descriptors (the watched folder;
    the SSH terminal's keys in step 8).
  - **The loop:** `io_poll` on the surface and the watch with no timeout, so a slide that
    doesn't change costs nothing; `top` showed Preso at 0%. A change is acted on 150 ms after
    the last one, since a save is often several writes.
  - **Presenting:** the slide's buffer is registered as a surface image and copied by the
    2D-DMA into the buffer acquired, by the CPU if that fails.
  - **The pointer** is hidden over the slides.
- **Controls:**
  - The presenter's actions are declared from the program (`BMDS_CONTROLS_DECLARE` with
    text), with the CPU's raw keys as the fallback.
  - An action takes at most four keys; the first declaration gave `next` five and was
    refused (`-BMDS_EINVAL`, "line 7" in the log). `sys.h` now says so.
  - Enter is left a plain key: the presenter takes it as Next for now, and step 5 needs it to
    end a slide's number.
- **`preso check`** lays out every slide as well, on the platform's fonts. On the board that
  is the real ones: six slides in 0.07 s, the program's loading included. On the host it is
  the headless platform's stand-ins, each character half its size wide, so overflow there is
  approximate.
- **Tests:**
  - 14 more host cases (`tests/host/preso/layout.cpp`): lines, styles, fragments drawn by step,
    lists, `fit`, a title slide, code, tables, overflow, splits, sizes and colours, the
    presenter's steps and reloads. They run on a test platform that serves the deck from
    memory.
  - ASan and UBSan over them, over every Markdown file in the repository, and over
    pathological decks (a 100,000-character word, 40-column tables, 2,000 lines of code).
  - On the board, a six-slide deck presented over SSH and seen in screenshots: the title
    slide, fragments, the code's colours, the table and quote, the split slide, `fit`, a
    reload, Space, Left and Escape.

### Step 3 (2026-11-03): pictures

- **The platform decodes; the core does the rest,** the same on every platform:
  - `Platform::imageSize()` and `decodeImage()` give a `Picture` in one of three layouts:
    0xAARRGGBB words, bytes B, G, R, or RGB565 words.
  - The default `imageSize()` only looks for the file. So on the headless platform a picture
    is there or not, and its box is drawn without being reported.
  - The BMDS platform uses `image_info` and `image_decode`, upright (EXIF).
- **Decoding on the board:**
  - An opaque picture is asked for as BGR24, the JPEG codec's own format, in the layout it
    asks for, so it decodes straight into the buffer.
  - **Why not BGRA32:** as BGRA32 a JPEG decodes into the codec's buffer and is converted.
    A 1920×1080 one failed with `-BMDS_ENOMEM`: about 6 MB of PSRAM was free while the desktop
    ran, with a largest block of 4 to 6 MB (`docs/platforms/esp32p4.md` §4).
  - If BGR24 doesn't fit, the picture comes as RGB565. That works, but its five bits of red
    and blue band a sky once scaled, so it's the last resort.
  - A picture with alpha comes as BGRA32 (a PNG, decoded in software).
- **Scaling** (`core/images.h`, `scalePicture()`):
  - Area-averaged down and bilinear up, separable, in 16.16 fixed point. Colours are
    premultiplied by alpha, so clear pixels don't darken the edges around them.
  - **Memory:** a row at a time. Each output row is a weighted sum of a few source rows, already
    scaled across and kept in a ring, so a big picture never needs a second copy of itself.
  - **Windows:** only the part of the scaled picture that is drawn is made. A 100×400 picture
    covering a 1280×720 slide is 1280×5120 scaled; only its middle 720 rows are ever made.
  - **The plain case** (opaque, no filters, no palette, to RGB565) has loops of its own. A
    picture at its own size from RGB565 is a row copy.
  - **On the board:** a 1280×720 JPEG scaled to itself took 314 ms through the general loop
    and 131 ms through the plain one. That's near the speed of PSRAM for 0.9 million pixels
    read and written.
- **Colours:**
  - `grayscale` and `sepia` are applied after scaling.
  - A theme's `Palette` maps pictures to its colours by an 8×8 ordered dither. The nearest
    colour is weighted as the eye sees, green most.
  - Otherwise RGB565 output is dithered by a 4×4 map, which breaks up the bands of a smooth
    sky (seen on the board: without it the 5- and 6-bit steps show).
- **The cache** (`ImageCache`):
  - It knows each picture's size, and keeps its scaled windows up to 4 MB, the least recently
    drawn going first.
  - Out of memory, while decoding or scaling, it lets go of everything it keeps and tries once
    more.
  - A picture that still fails is reported once, `preso: PATH: can't be drawn: WHY`, and drawn
    as its box until the deck reloads. A reload forgets every picture, since the files may
    have changed.
  - Paths are relative to the deck's folder, `%XX` decoded.
- **Layout:**
  - **Inline:** a picture has its own size; `w:` or `h:` keeps it in proportion; a percentage
    scales it. Too wide for its line, it shrinks to fit. Pictures sit on the text's baseline.
  - **Backgrounds:** `cover` by default, `contain`, `auto` (`ImageFit::Natural`, new: its own
    size, which an unmarked background isn't), a percentage, or `w:`/`h:`. A background is
    centred and clipped to its area.
  - **Several backgrounds** share the slide side by side, or with `vertical` one above another.
    With one `left` or `right`, they all share that side's split.
  - **`backgroundImage: url(...)`** covers the whole slide.
  - **Reported:**
    - a picture that isn't there;
    - one that isn't PNG, JPEG or BMP;
    - one on the web (pictures are read, not fetched);
    - one more than twice the pixels it is drawn at and over a megapixel ("a copy that size
      decodes in less memory, and better"). On the board, the 1920×1080 JPEG covering a slide
      fell back to RGB565 for lack of memory and banded; a 1280×720 copy decoded as BGR24 and
      drew smooth.
- **Measured on the board** (the first drawing of a slide, done ahead of time, so Next is
  still a copy at a talk's pace):
  - a slide-sized JPEG background: 267 ms (45 ms of it decoding);
  - a split slide with a 1280×720 PNG: 377 ms (the PNG's decoding 262 ms);
  - three small PNGs: 339 ms;
  - three 1920×1080 tiles with filters: 1.1 s;
  - a fragment step on a slide whose pictures are kept: 65 ms.
- **Tests:** 20 more host cases (`tests/host/preso/images.cpp`):
  - scaling: solid colour, a checkerboard averaged, a ramp enlarged, alpha and its edges,
    windows, own size, the three layouts;
  - filters, the palette dithering, RGB565 dither;
  - the cache, its budget and eviction;
  - layout: sizes, cover, contain, `auto`, tiles, splits, `backgroundImage`, failures, the size
    hint, extreme sizes.

  They run on a platform that makes its pictures, under ASan and UBSan too. On the board, a
  five-slide deck of JPEGs, PNGs with and without alpha, tiles, sepia and grayscale was checked
  in screenshots.

### Step 4 (2026-11-03): themes

- **The format** (`core/theme.h`, `ThemeSet`): INI, read over paper's values, so a theme names
  only what differs.
  - `;` and `#` start comments, as does ` ;` after a value.
  - `[Theme] Extends=NAME` reads a built-in theme first (four deep at most; a circle is
    reported).
  - Every key is checked. A wrong one is reported with the file and line, and why
    (`mine.theme:11: key `Colour` isn't a key of its section`), and the rest is still read.
- **Sections and keys:**
  - `[Theme]`: `Name`, `Extends`, `Size` (to 1280×720), `Scale` (`smooth`, `nearest`),
    `Palette`, `Effects` (`scanlines`, `glow`), `Scanlines` and `Glow` (percentages),
    `GlowRadius`.
  - `[Slide]`: `Background`, `Color`, `Accent`, `Font`, `Pattern`, `Frame`, `Margin` (CSS's
    one to four numbers), `Align`, `Center`, `Gap` (in lines), `Placeholder`.
  - `[H1]`-`[H3]`: `Font`, `Color`, `Background` (a band), `Bar` (the band across the slide: a
    title bar), `Frame`, `FrameColor`, `Underline` (`line`, `double`, `shade`),
    `UnderlineColor`.
  - `[Bullet]`: `Glyphs` (up to three by depth), `Color`. `[List]`: `Indent`.
  - `[Code]`: `Font`, `Color`, `Background`, `InlineBackground`, `Frame`, `Padding`, and a
    colour per style (`Keyword=`, `Comment=`...).
  - `[Quote]`: `Bar`, `Color`, `Indent`. `[Table]`: `Header`, `Rule`. `[Link]`: `Color`.
  - `[Header]` and `[Footer]` (one style): `Font`, `Color`, `Background` (a bar: a status line).
  - `[Script]` is accepted, for step 6.
- **Values:**
  - **Fonts** as the font library names them, read from the end: a size, a style, then the
    family (`unscii 32`, `IBM Plex Sans 30 bold`). Without a size or family, the current one
    is kept.
  - **Frames:** `none`, `single`, `double` (two lines, as box drawing's ═), `raised`, `sunken`
    (the toolkit's bevel).
  - **Palettes:** `ega`, `cga`, `amber`, `green`, `gray`, `mono`, or up to 256 colours written out.
- **`[Class NAME]`** sections: `Key=` is the slide's, `Section.Key=` another section's.
  - The lines are checked as they are read, and applied when a slide of that class is laid
    out (`ThemeSet::forClasses`). Several classes apply in order; each combination is made
    once and kept.
  - Paper's `title` and `lead` are such lines now: the class rules that were C++ in step 2
    are theme text.
- **Where a theme is found:**
  - a path, from the deck's folder (`themes/mine.theme`);
  - else `NAME.theme` beside the deck;
  - then `themes/NAME.theme` in the platform's data folder: on BMDS `pkg/root`'s
    `share/preso`, so a package can bring themes;
  - then the built-in one.

  An unknown name is paper, reported (Marp's `gaia` and `uncover` aren't ours). A theme's
  `Pattern` is named from its own file's folder and kept as the deck's folder names it, which
  is what the picture cache reads from.
- **The built-in themes** are INI text in the program, read the same way:
  - **`paper`**: as before.
  - **`vga`**: 640×360 and sharp, the EGA palette, blue, unscii 16, a double frame round H1,
    a grey status line.
  - **`amber`** and **`green`** (`Extends=amber`): unscii 32 on black, the phosphor's shades as
    the palette, glow and scanlines.
  - **`classic`**: grey, a navy title bar across each slide, a raised window round it, code
    in a sunken box.
  - **`bbs`**: ANSI colours on black, 640×360 sharp, block-shade dividers under headings.

  `preso theme` lists them, and `preso theme NAME` prints one: the start of one's own. A test
  checks that each reads with no complaint.
- **Layout:**
  - **Headings:** a band or frame gives the heading room inside it. The band goes behind the
    text, put before it in the draw list (`insertOp`). A shade divider is a run of `█▓▒░`,
    a quarter each.
  - **The header and footer** get a bar across the slide where either has text.
  - **The slide's frame** is drawn last, over everything.
  - **A pattern** is tiled at its own size behind everything.
  - **Bullets** are in `Bullet.Color`, else the accent.
- **`Scale=nearest`:** `Platform::openScreen()` takes `sharp`. On BMDS that's
  `BMDS_SURFACE_WHOLE`: a 640×360 theme is shown at whole multiples with square pixels, for
  nothing (the compositor scales). Its pictures are scaled to 640×360 and EGA-dithered, which
  looks the part.
- **Effects** (`core/effects.h`), applied after each step is drawn:
  - **Scanlines:** every other row darkened, straight on RGB565's fields.
  - **Glow (a bloom):**
    - **Source:** each pixel's excess over the background, averaged at a quarter size each way.
      In RGB565 the excess comes from three lookup tables.
    - **Blur:** three box blurs each way, close to a Gaussian. Each is a running sum read in
      order: rows directly, columns with a sum per column rather than copying columns out.
    - **Add-back:** interpolated back up. At a quarter's size the weights repeat every four
      pixels, so they are constants.
    - **Halo only:** a pixel gets only the blur's excess over its own light. A big bright
      picture isn't washed out, while the dark around text glows.
    - **Hue:** no channel is brightened past the brightest excess on the slide, so the glow
      keeps the text's hue. Red saturating first had made amber's halo inside a W a pale
      yellow block.
    - **Gain:** the strength is a gain of up to two. A blur spreads a thin stroke's light
      thinly; at four, the inside of a W filled solid.
  - **Speed on the board:** about 1 s a step the first way (half size, columns copied,
    divisions); now 250 to 400 ms, most of it memory (its buffers, 170 KB each, are in PSRAM).
    A step is drawn ahead, so this shows only when someone presses faster than that: four
    presses in a row lagged a second at first.
- **Tested:**
  - 10 more host cases (`tests/host/preso/themes.cpp`): fonts, every built-in theme clean, vga
    and green in detail, a theme file with mistakes and its lines, the data folder and paths,
    palettes, the decorations in layout, the effects, a sharp screen opened.
  - ASan and UBSan over everything, and 40 random theme files.
  - On the board, a four-slide deck in all six themes, seen in screenshots.

### Step 5 (2026-11-03): transitions, the overview, blank screens, going to a slide

- **Transitions** (`core/transitions.h`):
  - **The directive:** `transition: NAME [DIRECTION] [TIME]`, with `none` or `cut`, `wipe`
    (`left`, `right`, `up`, `down`), `dissolve`, `pixelate`, `crt` and `type`. It is a local
    directive, as Marp's, so it carries on to the slides after until changed; `_transition`
    is one slide's.
  - **When they play:** going forward onto another slide. Going back, a fragment step and a
    jump to a number are cuts.
  - **Interruptions:** a key during a transition finishes it at once, then acts. The presenter
    is never kept waiting.
  - **`type`** types the slide's text at 60 characters a second, at most its time (2 s by
    default), over the slide drawn without its text.
- **How a frame is made:** as **pieces**, rectangles copied from the two slides' pictures
  (already drawn) or filled (`Platform::presentPieces()`).
  - **On BMDS the 2D-DMA copies them.** Both pictures are registered as surface images once per
    transition, and each frame is a few BLIT jobs, a run of one source's pieces a job. Other
    platforms (and the host) copy them by the CPU, through the same `applyPieces()`.
  - **Pixelate** is the CPU's, since the DMA doesn't stretch.
- **Measured on the board, in order** (1280×720; frames in the transition's time):
  1. **The CPU composing each frame into the surface's buffer:** a wipe took 53 ms a frame, 15
     frames a second. The CPU copies PSRAM at about 28 MB/s each way (the platform notes).
  2. **Whole frames by the 2D-DMA:** 20 frames a second.
  3. **Only what differs:** with two buffers taking turns, the one being drawn holds the frame
     from two frames back. So a frame copies only the difference since then: a wipe's strip,
     the cells dissolved since, the text typed since (`Platform::frameBuffers()`; a
     platform that can't say gets whole frames). Still about 20 a second.
  4. **The present's damage:** passing no damage meant "all of it", and each present cost 12 ms
     re-sending the whole picture to the display. The pieces' rectangles are now the damage,
     merged into at most 64. Wipes then ran at about 48 frames a second and `type` at 55.
  5. **Bigger dissolve cells:** dissolve still made about 600 cell pieces a frame and ran at 22
     frames a second. Its cells are now 32 pixels at 1280 (16 at 640), the old programs'
     blocky look; 23 a second.
  6. **Pixelate only on change:** pixelate composes a frame only when its block size changes
     (`pixelateStage()`), and otherwise waits for the display's next frame
     (`Platform::waitFrame()`, `BMDS_SURFACE_WAIT`), not spinning.
  - **Where it ended up:** at 1280×720 wipe 48 frames a second, type 55, dissolve 23, crt 16 (a
    piece per row of the squeezed picture: all of it changes), pixelate about 13. At 640×360
    (vga, bbs) all run at about 60.
  - **The first frame** costs about 100 ms: both pictures written back from the cache as they
    are given to the DMA.
  - The test (`present.cpp`) checks every difference frame, in two buffers taking turns,
    against the whole frame composed afresh.
- **Animation in the platform:**
  - `animate(true)` makes the loop call `App::frame()` each display frame.
  - `beginFrame()` gives a screen buffer to draw straight into, and `endFrame()` shows it.
    This saves the third slide buffer the plan had (§10: 1.8 MB the board doesn't have to
    spare).
  - On BMDS, acquiring a buffer waits for the display, which paces the frames. While
    animating, the loop polls without waiting.
- **Black and white screens:** B or `.` (a clicker's blank button), W, and Select on a pad.
  - The same key again brings the slide back, and so does a step, without moving.
- **Going to a slide:** digits typed show "Go to 12" in a box in the corner; Enter goes there
  (to its first step).
  - Backspace (Previous) takes a digit back, and Escape gives up rather than quitting.
  - A number past the deck does nothing.
  - Digits come from the key's character, else from its HID usage (top row, keypad): keys sent
    by `win hid` on the board carry no character.
- **The overview:** Tab, or Start on a pad.
  - **The grid:** four slides across and as many rows as fit, each slide's number and title
    under it, the slide shown in yellow, a frame round the one chosen, a line of help.
  - **Thumbnails:** each slide is drawn whole into the picture ahead and scaled down
    (`scalePicture`), one a frame, so the grid fills as it is looked at. Only those near the
    page are kept (90 KB each at 1280).
  - **Keys:** Controls gets a second set, `overview` (left, right, up, down, choose, close),
    switched by `Platform::setInputMode()`. In the `deck` set the arrows are Next and
    Previous, so the overview needs its own. Enter or Space goes to the chosen slide; Tab or
    Escape goes back.
  - A reload closes it, since its thumbnails were of the deck as it was.
- **Tests:** 11 more host cases (`tests/host/preso/present.cpp`):
  - transitions read and composed (start, end, halfway, differences against whole frames);
  - the presenter's transitions, `type`, blank screens, typed numbers, and the overview.

  They run on a platform with a screen buffer and a clock of the test's, and under ASan and
  UBSan. On the board: a seven-slide deck of every transition (measured, at 1280 and at 640),
  the overview, go-to and black, seen in screenshots.
