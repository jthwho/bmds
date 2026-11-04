# Preso: Presentations Written in Markdown

Planned 2026-10-30 with the user (the user: a presentation app in the spirit of Marp, decks written
in Markdown with images, a look set apart from the content, the retro aesthetic kept; built as an
SDK program on the card; the talk about BMDS given on BMDS).

The program is `preso` (the user's name, 2026-10-30).

Status: **steps 1 to 8 built** (2026-11-03, §14); the pad bindings wait for the user's pad.

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

### 6b. Step 8's design (2026-11-03)

- **Notes** (`core/notes`): on each slide or step shown, the presenter writes to its standard
  output a block for that moment:
  - a rule with the slide's number of the deck's, its title, and the timer;
  - its step of its steps, then its notes as written;
  - the next slide's number and title, and its first lines as plain text.

  On a terminal the block replaces the screen (cleared first), and the timer's line is written
  again each second in place. Into a file or a pipe it is appended, without the timer's updates.
- **The timer:** the talk's time from the first slide shown, and the slide's own. With `duration`
  the pace: where the talk should be by now (the duration shared evenly over the slides), as
  "1:10 behind", "0:45 ahead" or "on time" (within 15 seconds).
- **Keys from the terminal** (`core/terminal`): the platform puts its standard input in raw mode
  when it is a terminal and gives the presenter its bytes. A decoder turns them into the keys
  they are (arrows, Home, End, PageUp and PageDown as their escape sequences, letters, digits,
  Enter, Backspace), and the presenter maps those as the platform's keys are mapped:
  - `n`, `j`, Space, Enter, Right, Down and PageDown are Next;
  - `p`, `k`, Backspace, Left, Up and PageUp are Previous;
  - `g` and Home are First, `G` and End Last;
  - digits then Enter go to a slide;
  - `b`, `.` and `w` blank; `o` and Tab the overview; `i` enters a region;
  - `q` quits. Escape alone does nothing, so a stray key on the laptop can't end the talk.

  The BMDS platform's fallback key map moves to the core, shared by both.
- **The pointer and ink:**
  - **The dot:** the mouse, or a pad's left stick, moves a dot of the accent colour, shown while it
    moves and hidden two seconds after it stops.
  - **Ink:** holding the mouse's button draws in the accent colour, as strokes kept for the slide.
  - **Clearing:** leaving the slide clears its ink, and so does `c` (Clear ink: C on a keyboard,
    Y on a pad).
  - **Drawing them:** the dot and the ink are drawn over every redraw of the shown slide (a step, a
    script region's frame), only the parts that change presented.
  - **Script regions:** on a slide with an interactive region the system's pointer stays (§7.10),
    and the dot isn't drawn.
- **Ticking:** a timer refreshed on a terminal and a dot hidden after a while need the loop to
  wake now and then. The platform calls `App::second()` about once a second while the app asks
  for it (a terminal's notes, a dot shown), and not otherwise.
- **Pads:** the bindings exist since step 2. Checking them needs the user's pad, so they are
  listed for the user to try.

## 6a. Live demos

`<!-- run: COMMAND -->` on a slide: stepping onto it starts `COMMAND` (vfork and exec through the
shell, so arguments and `terminal -e ...` work), and the deck gets out of the way. When the
program ends, the deck comes back on the slide after. The slide's notes are on the terminal
while it runs.

To get out of the way the deck **closes its screen** while the demo runs and opens it again after.
Its pictures, the screen's buffers, scaled images and thumbnails go too: they are drawn again from
the deck. That is 13 MB at 1280×720 (measured, §14), and a demo needs it. Lucky Duck alone takes 16
MB, and the board has 24 MB free with the desktop, so the two don't fit side by side.
- **From the start:** the screen goes as the demo starts, not once its window takes the focus. A
  program may need the memory before it opens a window (Lucky Duck decodes its sprites first), so
  the desktop shows while the demo loads.
- **Hiding instead (rejected, step 9):** a hidden surface keeps its memory. A window manager call
  that took a window off the screen and the panel was built and worked, but Lucky Duck still ran
  out of memory, so it was taken out again. An unused call would be ABI kept forever.
- **On the PC:** the window closed and opened again, or simply hidden (memory isn't short).

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

### 7.9 Step 6's design (2026-11-03)

What step 6 settles that the sections above leave open:

- **`libangel` is a build split, not a move.** Its files stay in `programs/angel`. The host
  build makes `bmds_libangel` of them all but `main.cpp` and `debug.cpp`, and the board's
  makefiles make `libangel.a` the same way. Both `angel` and `preso` link it, and a static
  library brings in only the files a program reaches: Preso reaches the string, the API's
  registration, the core and the colours, never the graphics, GUI or network files.
  - **The engine's settings** (the language's ABI, angel's table) move into
    `libangel`'s `createEngine()`, so a deck's scripts are the same language as angel's. Only
    the line cues differ (on in Preso, §7.6).
  - **The core splits in two.** `registerLanguage` has print, maths, random numbers,
    `millis`, `assert` and `throw`. `registerProgram` has `input`, `args`, `env`, `exit` and
    `sleep`. `registerCore` is both, as before, so angel's API is unchanged.
  - **What the language reaches outside is a host's:** where `print` writes, what `millis`
    counts, and whose random state is drawn from. Angel's are stdout, the clock and one state;
    Preso's are the region's.
  - **The colours** (`Color::`, `rgb`, `hsv`, `mix`) move out of the graphics into their own
    file, shared.
- **Where the code lives:**
  - The core knows scripts only through an interface (`core/scripts.h`: `Scripts`,
    `ScriptRegion`), so it still has nothing but itself and md4c on its include path. A build
    without scripting passes no scripts, and regions are boxes.
  - The runtime is `programs/preso/script/`: AngelScript and `libangel` on its path, portable,
    no platform header. The antialiased shapes it draws with are the core's (`core/raster`),
    on the core's `Canvas`.
- **Steps:** a region's beats are steps of its slide, in the document's order with its
  fragments. A region met in the layout adds its beats there, and the fragments after it move
  up. A region knows its beats from a run to its end in fast-forward when it is laid out, so
  `preso check` runs every script just by laying out every slide.
- **Positions are centres** (Manim's). An object's `x` and `y` are its centre in the region's
  pixels, `rotation` is in degrees, clockwise. Objects appear when `add`ed or when an
  animation brings them in (`fadeIn`, `grow`, `write`), and go with `remove` or an animation
  that takes them out (`fadeOut`, `shrink`).
- **Drawing:**
  - Shapes are polygons filled with exact coverage, so they are smooth as they move.
  - Text is rendered once into a coverage mask, and drawn from it moved, scaled, turned and
    faded.
  - Each frame redraws only the region's part that changed: what the objects covered before,
    and what they cover now. That part of the slide is redrawn under them, clipped, the theme's
    effects included, and given to the platform as the damage. (As built, a moving frame leaves
    a theme's glow out, and the region gets it when it comes to rest: §14.)
- **Timing:**
  - A region's clock runs only while its slide is shown and still. A transition shows its
    first frame.
  - `play` and `wait` suspend the script's context; `pause` suspends it until Next.
  - Next while a region is still moving finishes its beat at once, passes the pause, and plays
    the next beat. Previous and every jump re-run the script in fast-forward: they run `$top`
    (or `main`) again on a fresh context, which re-initializes the globals, since the top-level
    patch initializes each one where it is declared.
- **`--trust` (step 7)** will add angel's system API without its windows and drawing (no
  `Screen`, `Image`, GUI): the deck's own names (`Rect`, `Image`, `Canvas`, `Slider`) are the
  scene's, so they don't clash.
- **Sound** comes with step 7's interactive pieces: it needs the platform's audio, which the
  core doesn't have yet.

### 7.10 Step 7's design (2026-11-03)

- **`angel run` blocks:** the code box takes the slide's width less a pane (beside it, 45%, at
  least six code lines tall; `below`: under it, the width, six lines). The pane is a script region
  whose script is the block's text.
  - **The gate:** a block's region has a beat before its first statement, so the step after the
    code appears runs it. Its beats are that and its `pause()`s.
  - **Output:** printed lines fill the pane in the theme's code font on the code's background;
    its scene and `draw()` are in the pane too.
  - **Errors** name the deck's line: the block's first line is the script's line 1.
- **Interactive pieces:**
  - **A script's handlers:** `void event(Event@ e)` (angel's names: `EventType::KeyDown`,
    `MouseDown`..., `e.key` as angel names keys, `x` and `y` in the region's pixels) and
    `void tick(float dt)`. Either makes its region interactive, and so do widgets.
  - **Entering a region:** the Into action (I, a pad's West button), a click in it, or
    `<!-- interactive -->` as its slide appears. While a region is entered, keys go to the
    script, `tick()` runs each frame, and the region has a thin accent frame. Escape (the
    `script` set's Leave) gives the keys back. Leaving the slide leaves the region.
  - **The pointer:** on a slide with an interactive region the system's pointer shows; its moves
    and clicks over a region are that region's. Elsewhere it stays hidden (the presenter's
    pointer and ink are step 8's).
  - **In handlers:** `play()`, `wait()` and `pause()` belong to the script's statements, so a
    handler starts animations that run alongside with `start(animation, seconds)`.
  - **Widgets** (objects of the scene, drawn in the theme): `Slider(min, max, value)`,
    `Button(label)`, `Toggle(label)`, with `value`/`on` and `onChange(callback)`. While entered,
    Tab moves among them, arrows move a slider, Space or Enter presses; the pointer drags and
    clicks.
  - **What isn't replayed:** Previous on an interactive slide runs it from its start (§7.6).
- **Sound:** `Sound("click.wav").play(volume)` from the deck's folder, and the slide directive
  `sound: FILE` as a slide appears going forward. Played only live: never in fast-forward or
  `preso check`. The platform plays sounds (BMDS: libbmdsgame's mixer; the headless one none).
- **`preso --trust DECK`:** angel's system API without its windows, drawing, sound or GUI
  (files, the system, the network; as built not its processes, which answer through angel's GUI
  loop: §14), for that run. Trusted scripts' runaway limit is
  a second, since a system call may block. `preso check` builds a script that fails the deck
  profile again with the trusted API: if it builds then, it says the script needs `--trust`.
- **The question before `run:` demos:** a deck with `run:` slides, opened without `--trust`,
  asks once before its first slide (Y runs them, N or Escape presents without them). The demos
  themselves are step 9; until then the answer is kept for them.

## 8. Also from the same code

- **`preso check DECK`:** parses and lays out every slide without a screen; reports missing
  images, overflowing text, unknown directives and themes, script errors, scripts that need
  `--trust`. Runs on the host as well (a test).
- **`preso export DECK DIR`:** each slide, and each beat and fragment, to a PNG, for the README,
  a web page or a PDF made elsewhere; `--posters` (§7.2).
- **A Markdown viewer later:** the same layout in a scrolling window opens `.md` files from the
  file browser (`mimeapps.list`), our own docs included. Not in this plan's steps.

### 8a. Step 10's design (2026-11-03)

- **`preso export [--trust] [--posters] DECK DIR`** draws every step of every slide to a PNG
  in DIR, which it makes if needed:
  - **Names:** `007.png` for a slide of one step, `007-1.png` to `007-4.png` for one of four.
    They sort in order, and the last of a slide is the slide built up.
  - **What is drawn:** what the screen shows once each step has played: script regions at the
    end of their beat (`seek(beat, true)`, as Previous does). Demos don't run, sounds don't
    play, ink and the dot aren't there.
  - **Size:** the deck's (the theme's), with its pictures.
  - **Writing:** `Platform::writePicture()`. On BMDS it is `image_encode`'s PNG. The headless
    platform writes a plain uncompressed PNG, enough for the tests (its fonts are stand-ins).
  - **`--posters`** also writes each script region's frame at its slide's last step, cut to its
    rectangle, beside the script as `SCRIPT.png` (§7.2). Marp then shows a deck that names the
    PNG with `script:SCRIPT` in the alt text, which Preso already reads.
  - **Exit status:** as `check`'s: 0 clean, 1 reported problems, 2 couldn't.
- **Kiosk mode:**
  - **`preso --loop DECK`:** Next on the last step goes to the first slide.
  - **`auto: 10s`:** each step of the slide goes on to the next by itself after that time. It
    can be the front matter's for every slide, or `_auto` for one.
  - **The clock** starts again with every step shown, so a key pressed in between gives the
    full time again. It stops while the overview, a typed number, a blank screen, the question,
    an entered region or a demo is up.
  - **Timing:** `Platform::setAlarm(at)` calls `App::alarm()` at `now()` ≥ `at`. On BMDS it is
    the loop's poll timeout, so nothing wakes in between.
- **`dist/preso`'s examples:** `programs/preso/examples/` installs to `share/preso/examples/`:
  - **`tour.md`:** what a deck can do, one feature a slide, in `paper`.
  - **`bmds/bmds.md`:** the BMDS talk, in `vga`, with its diagrams drawn by scripts (no
    screenshots kept in the repository) and Lucky Duck as its live demo.

## 9. Additions outside the program

Kept small, and only what other programs would use:
- **Syntax colouring in the SDK:** `bmds_syntax_scan(language, line, state, spans, max)` over
  `bmds_core`'s `textsyntax` (the editor's languages), and `bmds_syntax_find(name or file
  name)`. Today only the GUI's `CodeEdit` reaches it. Preso's code blocks and `Code` objects use
  it; so would a Markdown viewer, `less`-like pagers, angel scripts. The PC port compiles
  `textsyntax.cpp` in instead (§2).
- ~~**Hiding a surface**~~: not needed. The deck closes its screen for a demo (§6a).
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
  presented on vsync); the window closed or hidden for demos (§6a);
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
6. [x] `libangel` split from angel, with profiles; script regions; the scene API and
   animations; beats, Previous by fast-forward, determinism, the runaway stop; the deck profile;
   the API lock (§7.9, §14). Sound moved to step 7.
7. [x] `angel run` blocks; interactive pieces (events, entering a region, the widgets); sound;
   `--trust`, and the question before `run:` demos (§7.10, §14).
8. [x] Notes and keys over the terminal, the timer and the pace, starting at a slide; pad
   bindings checked with the user's pad (still to do: the user's pad); the pointer and ink
   (§6b, §14).
9. [x] Live demos: the deck's screen closed while they run, opened again after (§6a, §14).
10. [x] `preso export` and `--posters`; kiosk mode; `dist/preso` with an example deck; the BMDS
    talk itself (§8a, §14).

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

### Step 6 (2026-11-03): libangel, script regions, the scene, beats

- **libangel** (§7.9): angel's runtime as a library, with nothing moved.
  - **The host build** makes `bmds_libangel` of `programs/angel` minus `main.cpp` and
    `debug.cpp`.
  - **The board** builds `libangel.a` in `dist/preso`'s makefile, the same way.
  - **New files:**
    - `engine.cpp` (`createEngine()`) holds angel's settings table, so the language is shared.
    - `program.cpp` holds `input`, `args`, `env`, `exit` and `sleep`.
    - `colors.cpp` holds `Color::`, `rgb`, `hsv` and `mix`, out of the graphics.
  - **`core.cpp`** is now the language part (`registerLanguage`). `registerCore` is both parts,
    so angel's API and its lock are unchanged.
  - **A host's hooks:** a `LanguageHost` says where `print` writes, what `millis` counts, and
    which random state is drawn from. `forgetHelp()` frees the help kept for an engine.
  - **Caches reset per engine:** the string array type and the string type id were cached for
    the process's first engine. They are now reset as each engine registers, since a reload
    makes a new one.
- **Where the code is:**
  - **The core** knows scripts through `core/scripts.h` only (`Scripts`, `ScriptRegion`), and
    still includes nothing but itself and md4c.
  - **`programs/preso/script/`:**
    - `scene` holds objects and their drawing;
    - `animation` holds properties eased from their values to where they end;
    - `region` holds a script's run, beats and fast-forward;
    - `api` holds the deck profile.
  - **`core/raster`** draws smooth shapes on the core's `Canvas`.
  - **The board's program** is 2.1 MB with AngelScript in it.
- **Steps:** the layout numbers steps in document order. A fragment's step is the next free one
  when it is first met, and a region takes its beats' steps where it stands, so fragments after
  it come after its beats (`SlideLayout::steps`, `DrawOp::beat`). A region counts its beats by
  running to its end in fast-forward as the slide is laid out, so `preso check` runs every
  script and reports script errors with the deck's line: `slide 2: sort.as:12: message`.
- **Running a region** (`region.cpp`):
  - **Suspending:** the script's context is suspended in `play()` and `wait()`, where its clock
    runs, and in `pause()`, which is a beat.
  - **Next:**
    - in a beat still playing, it ends that beat at once (fast-forward to its pause), then
      plays the next one;
    - at a pause, it plays the next beat;
    - after the region's last beat, it goes on to the slide's next step.
  - **Previous, jumps, coming back:** each re-runs the script from its start in fast-forward.
    `$top` is run again on a fresh context, and the top-level patch initializes each global
    where it stands. Back onto a slide shows its beats played; onto a slide going forward (Next,
    a jump by number, the overview, starting) plays its first beat. A reload keeps them played.
  - **What a frame redraws:**
    - It compares each object's signature (all its fields) and box with what was last drawn,
      and gives the old and new boxes of what changed, at most 8, joined where they overlap.
    - A `draw()` function, or new printed lines, redraws the whole region.
    - That part of the slide is drawn again under the region and over it: the background, the
      region's script, and anything laid out on top.
    - The platform presents only those rectangles (`presentAreas`; on BMDS one BLIT of pieces,
      with the damage, and a stale rectangle per buffer for the buffer that missed a frame).
  - **The runaway stop:** a line callback reads the clock every 256 lines and aborts a script
    that has run 100 ms since its last `play`, `wait` or `pause`. The region shows
    `sort.as: ran 100 ms without play(), wait() or pause(): stopped (a loop that never ends?)`.
  - **Endless loops that yield** (`while (true) play(...)`) run on: fast-forward gives up after
    600 s of script time or 20,000 yields, and plays from there.
  - **Errors** show as a red box in the region with `file:line: message`, said once in the
    deck's diagnostics. A global's initializer that throws is reported with what it threw, not
    AngelScript's "Failed to initialize".
  - **Deterministic:** `millis()` is the region's own clock. `random()` is seeded from the
    deck's file name, the slide's number and the region's index.
- **The deck profile** (`api.cpp`, 541 declarations locked in `tests/preso/api.lock`, checked by
  `preso_api_lock`):
  - **From libangel:** the language, strings, arrays and colours.
  - **Objects:** `Text`, `Rect`, `Circle`, `Line`, `Arrow`, `Polygon`, `Image`, `Code`,
    `Group`, `Bars` and `Grid`.
    - Each has `x`, `y` (its centre), `scale`, `rotation` (degrees, clockwise), `opacity`,
      `visible`, `color`, `fill` (setting it fills), `fillOpacity` and `lineWidth`, and
      `at(x, y)` for chaining.
    - `Shape@` is every kind's; `cast<Text>(shape)` goes back.
  - **Animations:** `move`, `shift`, `fadeIn`, `fadeOut`, `grow`, `shrink`, `write`,
    `transform`, `colorTo`, `fillTo`, `scaleTo`, `rotateTo`, `tween`, and Bars' `swap`,
    `highlight`, `unhighlight` and `set`. Each has `.ease(Ease::...)` (`Linear`, `In`, `Out`,
    `InOut`, the default, and `Bounce`), `.delay(s)` and `.duration(s)`.
  - **Time and the scene:** `play()` takes one to six animations or an array, then the seconds;
    also `wait()`, `pause()`, `add()` and `remove()`.
  - **The theme and the region:**
    - `theme.color("Text" | "H1" | "Accent" | ... | "Script.NAME")`, from a `[Script]` section
      of any names. Script.Accent, Highlight, Muted, Line and Fill always exist, and vga,
      amber, green and bbs set their own.
    - `theme.font(...)` and `Font("IBM Plex Sans 40 bold")`.
    - `region.width` and `region.height`.
  - **Files:** `readText()`, and `Image` files, from the deck's folder only. An absolute path or
    `..` is refused.
  - **`void draw(Canvas@ c, float t)`:** angel's method names, in floats, antialiased.
  - **Not in this step:** sound (step 7, with the platform's audio), `angel run` blocks,
    interactive pieces and `--trust`.
- **Drawing** (`core/raster`, `script/scene`):
  - **Shapes:** paths filled with exact coverage (signed areas accumulated along each row from
    an active edge list). Between the cells the edges touch, the coverage is constant, and a run
    is filled or blended at once. Rectangles' and circles' outlines are rings; polylines get
    round joins only where they turn enough to show a gap.
  - **Text:** rendered once into a coverage mask (white on black, the green read back), and
    drawn moved, scaled, turned and faded from it. Unscaled and unturned, it snaps to whole
    pixels, since sampled between them it goes soft. `write()` shows the mask a line at a time
    as far as it has got, with no new render per letter.
  - **Code:** a sprite of its colours and their coverage.
  - **Transform:** outlines into outlines, resampled point to point; anything else crossfades as
    both move to where it ends.
- **Effects on a part of the slide:**
  - **The grid:** an area under effects is drawn on the slide's grid of four, so the scanlines
    fall on the same rows and the glow's samples on the same pixels.
  - **The glow:**
    - It is drawn with as much round the area as it reaches, then the pixels outside the area
      are put back.
    - What changed is grown by that reach first (`effectReach()`), since a glow changes the
      slide round what changed.
    - It is capped at the theme's brightest colours rather than the canvas's, so a part glows
      as the whole slide does. The test compares a part redrawn with the whole slide, exactly.
- **Measured on the board** (1280×720, `paper`; a frame's drawing, then the present, which also
  waits for the display):
  - **First:** writing a title took 40 ms a frame, and three shapes growing 55–60 ms (16–25
    frames a second, CPU-bound). In turn:
    1. Text sampled between pixels, measured several times a frame, and rendered again for each
       letter written: now 3–6 ms (60 frames a second).
    2. The fill walked every edge for every row, and a circle's outline was 125 quads with a
       join disc at every corner: now an active edge list, rings, and spans.
    3. Each pixel was mixed through `Canvas::blend`'s calls and colour mix: now RGB565's three
       channels spread so one multiply mixes them, 32 levels. A filled circle went from 5.6 ms
       to 2.3. Per-point `sinf`/`cosf` gave way to turning a step (the platform notes).
    4. The union of everything that moved, with the static title, was redrawn: now each
       object's old and new boxes.
  - **Now:** three shapes growing and turning redraw about 100,000 pixels in 12–21 ms (30–45
    frames a second). A moving dot or a written title runs at 60.
  - **A `draw()` region** redraws all of itself each frame: half the slide, 460,000 pixels, took
    60 ms plus 20 to present (12 frames a second). Keep `draw()` regions small.
  - **Amber (glow):** a part's glow with its reach costs about 100 ms (5–9 frames a second).
    Moving frames leave the glow out and keep the scanlines (4–30 ms a frame); when a region
    comes to rest, what its frames drew is drawn again with the glow, once.
  - **`preso check`** of the four-script demo deck took 0.6 s on the board, building and running
    every script.
- **Found and fixed:**
  - **Leak:** in AngelScript's generic call mode 1 a function owns the handles it is given, and
    every animation passed to `play()` leaked. Handle arguments are now released (`Arg<T>`, as
    angel's `ImageArg`).
  - **Dangling font names:** `FontCache` kept the family pointer of the key it was asked for,
    and a script's `Font` may be freed first. Entries now keep their own copy.
- **Tests:** 10 more host cases (`tests/host/preso/scripts.cpp`, 87 in all):
  - the raster in XRGB8888 and RGB565;
  - beats numbered among fragments;
  - errors and the sandbox;
  - the runaway stop and an endless loop that yields;
  - frames, Next, Previous and coming back;
  - determinism;
  - damage limited to what moved;
  - a part redrawn under effects equal to the whole slide;
  - a glowing region at rest equal to the slide drawn afresh.

  They pass under ASan and UBSan; UBSan's only reports are AngelScript's own unaligned stack
  accesses on a 64-bit host. On the board: the demo deck in `paper` and `amber`, stepped through
  with screenshots, the Previous replay checked against the algorithm, and the frame times
  measured. The last two fixes (whether a slide plays its first beat, and the glow's cap) were
  checked on the host only.

### Step 7 (2026-11-03): angel run blocks, interactive pieces, sound, --trust, the demos question

- **`angel run` blocks** (`Layouter::code`): the code takes 55% of the width and a pane the rest
  (`below`: the pane under it, six code lines tall), on the code's background, as tall as the
  code or six lines.
  - **The pane** is a script region whose script is the block's text (`ScriptPlace::source`,
    `gated`, `console`).
  - **The gate:** a gated region waits at a beat before its first statement, so the step after the
    code runs it. Its beats are that and its `pause()`s, and Previous back to the gate empties the
    pane.
  - **Output:** printed lines fill the pane in the theme's code font, the last that fit.
  - **Errors** give the deck's line: `deck.md:16: error: slide 2: line 16: No matching symbol 'y'`.
- **Interactive pieces** (`ScriptRegion::isInteractive`, `setEntered`, `input`):
  - **Handlers:** a script may define `void event(Event@)` and `void tick(float)` beside its
    statements, or instead of them (a script of handlers only, or only `draw()`, is no longer
    "nothing to run").
  - **`Event`** is angel's shape: `EventType::KeyDown`, `KeyUp`, `MouseMove`, `MouseDown`,
    `MouseUp`, `Wheel` with angel's values; `key` names keys as angel does (the names moved to
    libangel's `keys.cpp`); `text`, `x` and `y` in the region's pixels; `button`, `wheel`,
    `repeat`.
  - **Handlers run** on the region's second context (`draw()`'s), with the runaway stop. In them
    `play()`, `wait()` and `pause()` are refused; `start(animation, seconds)` runs one alongside.
  - **Entering a region:** the Into action (I, a pad's West button), a click in it, or
    `<!-- interactive -->` as its slide appears. While entered:
    - Controls' `script` set has Leave (Escape) and the clicker's PageDown and PageUp as Next and
      Previous, so every other key is the script's;
    - `tick()` runs each frame;
    - a 2-pixel accent frame shows inside its edge.

    Stepping, or another slide, leaves it.
  - **The pointer:** the system's arrow shows on a slide with an interactive region and is hidden
    elsewhere. Moves and clicks over a region are its own; a button held carries on to the region
    it went down in.
  - **Widgets:** `Slider(min, max, value, width)`, `Button(label)` and `Toggle(label, on)`, each
    with `onChange(Callback@)` (`funcdef void Callback()`: a `function() {...}` does).
    - Drawn in the theme: Script.Accent, Muted, Fill and the text colour.
    - While entered, Tab moves the focus (a ring), arrows, Home and End move a slider, and Space or
      Enter presses.
    - The pointer drags a slider and clicks a button (let go over it) or a toggle.
    - Callbacks run after the input or handler that changed them, a few rounds at most.
- **Sound:** `Sound("ding.wav").play(volume)` (the deck's folder only) and the slide directive
  `sound: FILE` as a slide appears going forward.
  - **The platform plays them** (`openSound`, `playSound`, `closeSound`). On BMDS that is
    libbmdsgame's mixer, opened with the first sound; a sound closed while it plays is freed once
    it has stopped. The headless platform has none.
  - **When:** never in fast-forward or `preso check`. A region's sounds asked for while its slide
    is only drawn ahead are kept until it is shown (`setAudible`).
- **`preso --trust DECK [SLIDE]`:** the deck's engine also has angel's files, system and network
  (`AngelScripts::registerAll`). Not its processes, which answer through angel's GUI loop, nor its
  windows, drawing, sound or GUI, whose names are the deck's. The runaway limit is a second.
  - **`preso check`:** a script that doesn't build in the deck profile is built again with that
    API; if it builds then, its error says `needs --trust: `.
- **The question before `run:` demos:** a deck with `run:` slides, opened untrusted, asks first.
  It shows "This deck runs programs: luckyduck, sysmon" (each program once, by its name), Y to run
  them, N or Escape not. The answer is kept for step 9 (`demosAllowed()`); trusted, it isn't
  asked.
- **Reloading:** the presenter also watches the folders of the deck's scripts and pictures
  (`anim/`), each once; the BMDS platform keeps up to 8 watches. An edit to `anim/x.as` hadn't
  reloaded the deck.
- **Found on the board:**
  - **A kernel panic** (programs run unprotected: `docs/ARCHITECTURE.md`): saving the deck while a
    region had the keys reloaded it, and `leave()` then used the old deck's freed region. Found
    from the recovered panic lines (`log show`): the fault and return addresses, fitted against
    the unstripped ELF at the load base where they named `Presenter::leave()` called from
    `arrived()`. The presenter now lets go of the entered region and the pointer's as their deck
    is dropped, and a host test reproduces it under ASan.
  - **`forEachLine` gives each line's offset, not its number.** Step 6 had used it as a number for
    printed output and for multi-line `Text` and `Code`, which put their second lines far down.
    Now counted, and the run block's test checks the lines stack.
  - The crash left the deck file empty mid-write; an empty deck shows a blank slide.
- **Tests:** 8 more host cases (94 in all), and the API lock appended (109 lines):
  - a run block's gate, output and error lines;
  - entering and leaving by Into, a click and Next, with keys, `tick()` and the pointer's arrow;
  - widgets by keys and the pointer, with callbacks;
  - sounds heard only when shown, never in fast-forward or check;
  - `--trust` and check's `needs --trust`;
  - the question, answered and trusted;
  - reloading while a region has the keys.

  They pass under ASan and UBSan. On the board: the three-slide demo deck, with screenshots:
  - the block's pane before and after its two beats;
  - the widgets by keys (Right, Tab, Space) and by the pointer (`win pointer`: a drag, a click
    that started a 45° turn);
  - the sound slide, with libbmdsgame's mixer thread running (whether it was heard isn't known
    from here);
  - the question, answered N.

### Step 8 (2026-11-03): notes and keys over the terminal, the timer and pace, the pointer and ink

- **Notes** (`core/notes`): each slide and step shown writes its block to standard output:
  - a rule, `── 2/12  Title ──────── 4:12  slide 0:42  1:10 behind`;
  - `step 2 of 4`;
  - the slide's notes, or `(no notes)`;
  - `Next: 3  Title` with up to three of its first lines, or `Next: the end`;
  - a line when the deck has problems (`preso check` says what).

  On a terminal (`Platform::outputIsTerminal()`) the screen is cleared for each block, and the
  rule is rewritten in place each second (cursor saved, line 1, restored). Into a file the
  blocks follow one another.
- **The pace** (`paceSeconds`): with `duration`, slide N is due at duration × (N − 1) / slides.
  Within 15 seconds is "on time". On the board, 2:16 into a five-minute talk on slide 3 of 5 read
  "1:04 ahead".
- **Keys from the terminal** (`core/terminal`):
  - **Decoding** (`TerminalKeys::push`) is a byte at a time. Arrows, Home and End come in their
    `ESC [`, `ESC O` and `~` forms, as do PageUp, PageDown and Delete; Shift+Tab is Tab. UTF-8
    gives characters; letters and digits get their HID usages. Escape twice is Escape, since a
    lone one can't be told from a sequence's start.
  - **Mapping** (`terminalAction`): §6b's letters, then the shared `defaultAction` (the BMDS
    platform's fallback map, moved to the core).
  - **Escape** does nothing outside a region. Ctrl-C quits as `q` does: the BMDS platform turns
    off ISIG with ICANON and ECHO, so the terminal is put back on the way out.
  - **When:** the platform reads standard input only when it is a terminal. Started from the
    desktop, nothing changes.
- **Ticking:** `Platform::setTicking()` and `App::second()`. The BMDS loop's poll times out at
  the next second while ticking (and isn't otherwise), so the board stays idle between slides
  unless a terminal shows the timer or a dot is up.
- **The dot and the ink:**
  - The mouse moves a dot (11 pixels, the accent at 200 of 255), hidden 2 seconds after it
    stops.
  - The first button draws ink: strokes 4 pixels wide (2.5 at 640), redrawn as each segment
    comes.
  - Leaving the slide, or Clear ink (C, a pad's North; `c` on the terminal), wipes it.
  - Every redraw of the shown slide (a step, a region's frame, its glow at rest) is followed by
    `decorate()`, which draws the ink and the dot over it, only the parts changed presented.
  - On a slide with an interactive region the system's arrow shows and the dot isn't drawn; the
    ink still is, outside the region.
  - **A pad's left stick** (`Event::Type::Stick`, from `BMDS_PAD_AXIS` while no action takes it)
    moves the dot from the middle, across the slide in about 1.5 s at full tilt, frames carrying
    it while it leans past a dead zone.
- **Controls:** Clear ink (Id 17) joins the deck set. The script set's stepping keys moved to
  Ids 32 and 33, past the actions.
- **Starting at a slide** (`preso talk.md 12`) has been there since step 2.
- **Not checked:** the pad bindings (Next on South, Right and R1; Previous on East, Left and L1;
  Into on West; Clear ink on North; the overview on Start; black on Select; the stick for the
  dot) need the user's pad.
- **On the board:**
  - **Over a real terminal:** a driver opened `ssh -tt`, started `preso talk.md`, and typed `n`,
    the right arrow, `3` Enter and `q`. The notes, each step's block, the timer counting each
    second and the pace all came through, and it quit with status 0.
  - **The ink:** a stroke drawn with `win pointer`, step by step.
  - **Merged moves:** a press and moves sent at once reached the program as one pointer state, so
    only the last point inked: the system merges pointer motion it can't hand over in time. A
    mouse's reports come over time, so a quick stroke can still lose its corners.
- **Tests:** 5 more host cases (`tests/host/preso/notes.cpp`, 99 in all): decoding, the
  terminal's key map, the notes and the pace, the presenter's notes and terminal keys, the dot,
  the ink and the stick.

### Step 9 (2026-11-03): live demos

- **Starting:** stepping forward onto a `run:` slide marks its demo. `finishTransition()` starts
  it once the slide is the shown one, so after a transition into it. The command runs as
  `sh -c "cd 'FOLDER' && COMMAND"` from the deck's folder, through `posix_spawnp`
  (`Platform::startDemo()`). Going back onto the slide, or reaching it by number, doesn't start
  it. It doesn't start in an untrusted deck whose question was answered no either.
- **The screen given back** (§6a): `releaseScreen()` runs before the demo is started.
  - It closes the screen (`Platform::closeScreen()`).
  - It frees the two slide pictures, the scaled images and the thumbnails.
  - On the board, free PSRAM went from 8.9 to 22.0 MB while a demo ran, and Preso itself fell
    from 3.8 MB to 100 KB. Lucky Duck needs about 16 MB.
  - If the demo can't be started, the screen opens again at once and the slide is drawn.
- **While it runs:** actions are ignored except Quit (q, Ctrl-C on the terminal), which ends
  the deck and leaves the demo running. A save that reloads the deck draws nothing until the
  screen is back.
- **Its end:** SIGCHLD writes a byte to a pipe the loop polls, and `waitpid` (WNOHANG) takes the
  status. The presenter opens the screen again, then goes to the next slide, or draws the last
  one again. Without a terminal, the status goes to standard error.
- **The notes** say `Demo: COMMAND (...)`: running, ended, couldn't be started, declined, or
  "runs stepping onto the slide".
- **Two bugs the board showed:**
  - **Signal handlers and the raw poll:** the loop called `bmds_io_poll` directly. The SDK runs
    a program's signal handlers only on the way out of a libc call (`__bmds_syscall`). So
    SIGCHLD made the raw poll return EINTR, but its handler never ran and the demo's end was
    never seen. The loop now uses POSIX `poll()`, with the same descriptor numbers.
  - **Inherited descriptors:** the surface was opened without close-on-exec, so the demo's
    shell kept it. Closing it in Preso then freed nothing and the window stayed. The surface
    (`bmds_fd_flags`) and the pipe (`pipe2`) are now close-on-exec, as the watches already
    were.
- **Hiding, built and removed:** a window manager `withdraw()` was built first. It took a
  window off the screen, out of the panel and out of Alt+Tab, through `BMDS_SURFACE_HIDE`
  (surface_control 8). It worked, but a hidden surface keeps its memory, and Lucky Duck still
  ran out. With the deck closing its screen, nothing uses it, so it was taken out before it
  shipped. The ABI lock gained only the missing check that `BMDS_SURFACE_IMAGE` is 6 and
  `BMDS_SURFACE_BLIT` is 7.
- **On the board:** a three-slide deck with `run: luckyduck`:
  - Lucky Duck started maximized, the panel listing only it.
  - Closing its window brought the deck back on the third slide, with the keys working.
  - Going back onto the demo's slide didn't start it again.
  - With `run: sleep 8`, the deck returned when it ended (status 0).
- **Tests:** 3 more host cases (102 in all):
  - the demo's command and folder;
  - the screen released and opened again;
  - actions waiting while it runs, and Next after it;
  - not run again going back, and declined;
  - a demo on the last slide;
  - one that can't be started.

### Step 10 (2026-11-03): export, posters, kiosk mode, the examples and the talk

- **`preso export [--trust] [--posters] DECK DIR`** (`Presenter::exportSlides()`, §8a):
  - It uses the presenter's own drawing: for each step, the regions are sought to the end of
    their beat, then the slide is drawn into one picture.
  - `Platform::makeFolder()` makes DIR, and `writePicture()` writes each PNG.
  - On the board, PNGs come from `image_encode`, straight from the RGB565 picture. Elsewhere the
    core writes an uncompressed PNG through stdio, with stored deflate blocks, its own CRC-32
    and Adler-32: no compressor needed.
  - **Posters** are cut from the slide's last picture at each region's rectangle, and written
    as `SCRIPT.png` beside the script.
  - **On the board:** the talk took 6.8 s for 47 pictures, with its two posters. The tour took
    17.7 s for 26: its orbit script never ends, so each seek runs it until fast-forward gives up.
- **Kiosk mode:**
  - `--loop` (`Presenter::setLoop()`) makes Next on the last step go to the first slide;
    Previous doesn't wrap.
  - `auto` (parsed since step 1) works through `Platform::setAlarm()` and `App::alarm()`. On
    BMDS the alarm is the loop's poll timeout.
  - `act()` now wraps `actNow()`: every action gives the step the whole time again.
    `finishTransition()` and other events re-arm the alarm without restarting it.
  - The alarm is off while something holds the deck (§8a), and where there is no step after
    the shown one.
  - **On the board:** a three-slide deck with `auto: 2s` under `--loop` went round twice in
    10 s, a slide every 2 s, and Escape quit it.
- **The examples** are in `programs/preso/examples/`, and `dist/preso`'s build installs them to
  `share/preso/examples/`:
  - **`tour.md`** (13 slides, `paper`): the format, text, code, tables, a moving script, a
    script in steps, a slider, an `angel run` block, transitions, and how to run a deck.
  - **`bmds/bmds.md`** (15 slides, `vga`, `duration: 25m`): the board, the idea, the goals, the
    layers (a script, one layer each Next), drivers, programs and the system call table, the
    ABI lock, efficiency, the desktop, Lucky Duck as a live demo, angel, a quicksort drawn by a
    script, self-hosting next. Each slide has speaker notes. It keeps no pictures in the
    repository.
  - `preso check` on the board reports nothing for either.
  - The host's stand-in fonts made overflow reports that the board's fonts didn't: the board
    is where a deck is checked.
- **Tests:** 5 more host cases (107 in all):
  - export's names, sizes and a failed write;
  - a region at each beat, and its poster (`export.cpp`);
  - the core's PNG, each chunk's CRC and the stored rows;
  - `auto` with a key, a blank screen, a slide's own time, the end, and `--loop`;
  - `--loop` without `auto` (`present.cpp`).
