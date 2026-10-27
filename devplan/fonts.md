# Fonts: one font library for the desktop, programs and angel

Status: **steps 1-8 done** (2026-10-26/27: §6a, §6c-§6e, §6g-§6j; fixes after, §6k; other fonts tested, §6f); planned the same day with the user (§8). Fills the
roadmap's "A system font library (planned with the user 2026-10-15)".

## 1. Goal

- **TrueType text that looks good:** smoothed (antialiased), kerned, any size, through
  stb_truetype, for the desktop, the toolkit and programs.
- **The bitmap font stays, and stays the fastest** (the user's): unscii-16 becomes one font in the
  library, drawn by today's PIE kernel.
- **One glyph cache for the whole system** (the user's): every glyph is rendered once per font
  and size, whatever its color and whichever program draws it.
- **Smoothing is optional** (the user's): a system setting, and a per-call override.
- **The whole API is in the SDK** (the user's), and **angel has all of it** (the user's). Nothing is
  released, so today's text calls may change (the user's).
- **A font dialog** in the toolkit, a GUI class like `ColorDialog`, so programs (through the SDK)
  and angel scripts can let the user pick a font (the user's).

## 2. Decisions (the user's, 2026-10-26)

- **Keep the bitmap font as a kind of font**, not a separate path. Callers name a font; "unscii
  16" is a bitmap one and takes the 1-bit kernel. The terminal, the boot console and the fallback
  when no font file loads stay on it. stb_truetype has no hinting, so at 16 px a hand-drawn
  bitmap is sharper than any TrueType font.
- **Cache coverage, not colors.** A cached glyph says how much of each pixel the glyph covers; the
  color is applied as it is drawn. Smooth edges have to be blended at draw time anyway, so a
  cache per color would only multiply memory. Key: face, pixel size, smoothing, glyph.
- **The OS draws the text.** Programs ask for text to be drawn into their own pixels; they never
  hold a pointer into the cache, so the cache can evict under a fixed budget without stale glyphs.
  Programs share the OS's address space (`docs/ARCHITECTURE.md`, memory protection), so one
  cache serves all of them.
- **Smoothing on: 4 bits a pixel** (16 levels: as good as 256 for text, half the memory).
  **Smoothing off: 1 bit a pixel**, the bitmap font's format, through the same PIE kernel, so a
  TrueType font drawn sharp costs what unscii costs.
- **No sub-pixel (LCD) smoothing:** it depends on the monitor's pixel order, which HDMI doesn't
  tell us, triples the cache, and needs the color known when the glyph is rendered.
- **The whole API in the SDK and in angel**, breaking today's text calls where needed.
- **IBM Plex ships** (Sans, Mono, Serif; OFL).
- **The desktop switches to TrueType by default** once step 7 looks right on the board; unscii
  stays the terminal's font and a choice in Settings.
- **The cache's budget is 512 KB of PSRAM.**
- **The user's fonts are in `${HOME}/Fonts`.**
- **A font dialog** (§4i), in the SDK and angel like the other GUI classes.

## 3. Facts that shape the design (checked 2026-10-26)

- **One font exists, twice:** unscii-16 (8×16, public domain, `thirdparty/unscii/`) is built into
  the OS by `tools/fontconv.py` (`bmds_ui`, `Fonts::unscii16`, plus `Fonts::icons16`) and into
  libbmdsgame (`sdk/game/font16.c`, by `tools/gamefont`, ASCII and Latin-1 only).
- **`GlyphRenderer`** (`bmds_ui/include/bmds/glyphrenderer.h`) expands 1-bit glyphs into RGB888,
  Scalar or PIE. Each pixel is the foreground or the background color; it never reads the
  surface back. Glyphs land on 8-pixel multiples in the strip, at most 32 rows tall.
- **`Painter::text(x, y, utf8, font, fg, bg, maxWidth)`** puts x on a 4-pixel column and expands
  glyphs into a two-half strip in internal DMA RAM while the 2D-DMA copies the previous half
  (`windows.md` §4d). Reading back from a PSRAM surface is what costs (`docs/platforms/esp32p4.md`,
  the 2D-DMA note).
- **The toolkit hardcodes the font:** about 30 files name `Fonts::unscii16`. Layout uses
  `GuiStyle::TextHeight = 16`, `ButtonHeight = 22` and `Column = 4`
  (`bmds_gui/include/bmds/widget.h`), measuring with `textWidth()` rounded up to a column.
- **Programs' text today** is libbmdsgame's: `bmds_canvas_text(canvas, x, y, text, pixel, scale)`
  and `bmds_game_text_width(text, scale)` (`sdk/include/bmds/game.h`), a static library with no
  ABI promise. angel wraps them: `Screen/Image/Canvas::text(x, y, value, color, scale)`,
  `textWidth(value, scale)` and `textHeight(scale)` (`tests/scripts/api.lock`).
- **The GUI protocol has no font:** no property, no size, no measuring (`sdk/gui/classes.def`).
  The server draws every widget in unscii. Clients draw a Canvas's text themselves.
- **System calls:** slots in `sys.h`, the last being 126 (`FS_WATCH`); the kernel's table is
  `bmds_loader/src/systable.c`, locked by `abilock.c`. Image decoding (93-95) already writes
  straight into a program's buffer, which is the pattern for drawing text.
- **The SoundFonts are the model for shared data:** loaded once, shared by every synth, kept
  while unused until memory runs short (`synth/keep-fonts`, `audio.md`).
- **`/rom` is mapped in place** (`rom.md`, `Vfs::map()`): a font file there is read where it lies
  in flash, costing no RAM. The ROM image is 6.5 MB of the 10 MB partition.
- **Pixel formats** programs draw in: `BMDS_PIXEL_BGR24`, `RGB24`, `BGRA32`, `RGB565`, `GRAY8`,
  `L8` (`sys.h`); surfaces are `L8`, `RGB565` or `BGR24`.
- **stb_truetype checks nothing** in a file (found reading it, 2026-10-26): a damaged glyph makes
  it read past the file, a compound glyph made of itself recurses forever, a last contour of one
  off-curve point reads past its own allocation, and a part placed by matching points is misread.
  Its kerning takes the first GPOS lookup covering the left glyph instead of adding up the `kern`
  feature's lookups: IBM Plex Sans has three, and 410 pairs in Latin and Cyrillic come out wrong
  (To, Ta, Yo and VA unkerned). So `FontFace` reads the character map, names and kerning itself,
  and checks the tables and each glyph before stb_truetype sees them (§6a).
- **IBM Plex's license reserves the name "Plex"**: a modified copy (subsetting counts) may not be
  called Plex. So the files ship as released, 173-221 KB each.
- **Lucky Duck** kerns its own drawn font at build time (`luckyduck.md`, `assets/font.py`) and
  says its headline letters move onto this library when it lands.

## 4. Design

### 4a. Faces, fonts and names

- **A face** is one font file's style: "IBM Plex Sans Bold" (TrueType), or a built-in bitmap font
  ("unscii", 16 px only, and `icons16`). Faces are found in the folders of `font/paths` and listed
  by the family and style names in their files. A TrueType file in `/rom` is read in place; one
  elsewhere is read into PSRAM once and shared, and stays loaded while unused until memory runs
  short, as the SoundFonts do.
- **A font** is a face at a pixel size: what callers open and draw with. Opening one is cheap
  (the face's scale and metrics); its glyphs fill the cache as they are drawn.
- **Names:** a family, a pixel size and a style (bold, italic), written as one string where a
  string is wanted (settings, the GUI protocol, angel): `IBM Plex Sans 13 bold`, or a role:
  `ui`. One parser, in `bmds_core`, is used by all of them. A missing style is made up where
  it can be: a bold drawn by smearing one pixel (as `TextCell::Bold` does), no made-up italic. A
  missing family falls back to the role's font (§4b).
- **Roles:** `font/ui`, `font/mono`, `font/title`, `font/terminal`, each a name such as
  `IBM Plex Sans 13`. Programs and the toolkit ask for a role, so the user changes the look in one
  place. Until the toolkit is moved over (§6, step 7) they all say `unscii 16`; then the defaults
  become IBM Plex (§4h).
- **Missing glyphs** come from the next face in `font/fallback`, then from unscii (it covers much
  of Unicode), then the replacement glyph.
- **Bitmap sizes:** a bitmap font opens at its size and at whole multiples of it (unscii at 32 or
  48, pixels doubled or tripled, as libbmdsgame's `scale` does today). It is never smoothed.

### 4b. Laying out a line

- UTF-8 in, left to right, one codepoint to one glyph (stb_truetype's cmap). No shaping, no
  ligatures, no right-to-left.
- **Kerning:** stb_truetype's pair adjustments (the `kern` table and GPOS pair positioning).
- **Positions:** advances and kerning add up in 1/64 pixels so the spacing is right. Smoothed, each
  glyph lands at the nearest quarter pixel (`FontFace::Phases`: up to four cached renders of a
  glyph, the phase in the cache's key); sharp, at the nearest whole pixel. Snapping smoothed glyphs
  to whole pixels, as first built, spaced 13-pixel Plex unevenly (§6k).
- **Metrics** per font: ascent, descent, line height, the widest advance, monospace or not.
  Measuring a string gives its width, how many bytes fit in a width (for eliding and wrapping)
  and its ink box.
- `\n` starts a new line one line height down, as `bmds_canvas_text` does today. Wrapping and
  eliding are the caller's, using the measuring calls.

### 4c. The glyph cache

- **One cache, in the OS, in PSRAM,** with a byte budget (`font/cache`, default 512 KB: about
  8,000 glyphs at 13-16 px). The bitmap fonts' glyphs are in rodata and are never cached.
- **Entries:** key (face, size, smoothing, glyph index), the glyph's box and offset from the
  pen, and its rows: 1 or 4 bits a pixel, each row starting on a byte.
- **Eviction:** least recently used. A draw call pins the glyphs it uses until it returns, so an
  entry is never freed while being drawn; the cache may pass its budget by one call's glyphs.
- **Concurrency:** a mutex around lookups and inserts. Rendering happens outside it, and if two
  tasks render the same glyph at once, the second copy is dropped.
- **Rendering:** stb_truetype's rasterizer gives 8-bit coverage, reduced to 4 bits, or to 1 bit
  against a threshold. Its scratch memory comes from a named `MemSpace` (`STBTT_malloc`), not the
  general heap.
- **Per face:** a table of the first 256 codepoints' glyph indices, so ASCII skips the cmap search.
- **Kerning pairs are cached too** (found on the board, §6a: 7.7 µs a pair from the font's
  tables): a two-way table of 4,096 (face, left, right) pairs to the value in font units (48 KB,
  PSRAM), read without a lock (§6c), so text drawn again costs a hash and a read a pair.
- **Stats** for the `font` command (§4h): entries, bytes, hits, misses, renders, evictions.

### 4d. Drawing

There are three ways to draw a run, chosen by what the caller gives:

| Mode | When | Cost |
|---|---|---|
| **Cells** | a bitmap font in fixed cells, fg on bg (the terminal) | today's kernel, unchanged |
| **Opaque** | a background color is given (labels, menus, edit boxes) | the box filled with bg, glyphs blended between bg and fg; nothing read back |
| **Over** | no background (text on a picture) | the pixels under the text read, blended, written back |

- **How a line is drawn** (`TextDraw::line()`, §6d): laid out, its glyphs fetched from the cache
  32 at a time (one lock for a run of one face, misses rendered). For each run, bands of rows
  across its width are composed a glyph at a time into alpha, each pixel the larger of the
  glyphs covering it (kerned glyphs overlap). Then each row is drawn by one call:
  - **Over:** `blend()`, `(color × a + pixel × (255 − a) + 127) / 255`, PIE for B, G, R, R, G, B
    and grey.
  - **Opaque:** the run's part of the box is filled first, then the same blend (PIE formats), or
    a 16-color ramp from background to color written where alpha isn't 0 (the rest). Nothing
    is read from the screen.
- **Coverage is linear in the kernels** (alpha = level × 17). The contrast curve
  (`font/contrast`) is applied as a glyph is rendered into the cache, before it is reduced to 4
  bits; a change empties the cache.
- **Target formats:** BGR24 (the surfaces), RGB24, RGB565, BGRA32 (alpha becomes
  a + A × (255 − a) / 255), GRAY8, and palette indexes (`L8`) sharp only: indices can't be mixed.
- **In the OS (the Painter):** the strip works as today, filled with bg (Opaque) or loaded from
  the surface by the 2D-DMA (Over), then the glyphs are blended in at any x. The run's box keeps
  the 4-pixel column rule at its edges, which the Opaque fill covers. Over is the costly one and
  is rare: icon captions already sit on a box of the desktop color (`wallpaper.md` §4e).
- **In programs:** the call draws into the program's own buffer with the CPU, in the program's
  task, as image decoding writes into its buffer.

### 4e. The program ABI (`sdk/include/bmds/sys.h`, new system calls)

The calls are in `sys.h` with every other (its structs, function types, inline wrappers); there is
no separate `font.h`. A font is a **font handle**, not an fd: a program has 16 fds in all
(`FdTable::MaxFds`), and a game holds a font for each size it uses. So fonts are a table of the
program's own (`Process::fontAdd()`, as its directories are), 16 of them, closed by `font_close`
and when the program ends or execs (§6e). Structs start with `uint32_t size`.

| Slot | Call | Does |
|---|---|---|
| 127 | `font_open(const bmds_font_spec_t *)` | Opens a font by a name string (§4a), or a family or role with a pixel size and style; a handle |
| 128 | `font_info(int32_t font, bmds_font_info_t *)` | Its size, ascent, descent, line height, widest advance, flags (bitmap, fixed, made-up bold), style, face, and its name |
| 129 | `text_measure(int32_t font, text, length, max_width, bmds_text_extent_t *)` | To a newline: width, bytes that fit, line height |
| 130 | `text_draw(const bmds_text_draw_t *)` | Draws a line into the program's pixels (pixels, format, size, stride, a clip; x and y the line's top; color; a background box or not; smoothed, sharp or the system's; a width limit): the width drawn |
| 131 | `font_list(uint32_t index, bmds_font_face_t *)` | The faces installed: family, style, weight, flags, glyphs, a bitmap font's size, file |
| 132 | `font_close(int32_t font)` | Closes a handle |

**libbmdsgame:** `bmds_canvas_text(canvas, font, x, y, text, pixel)` and
`bmds_game_text_width(font, text)` take a font handle and go through these calls, a line at a
time (they keep newlines). `sdk/game/font16.c` and `tools/gamefont` are gone, since unscii comes
from the OS like any other font. A negative font is unscii 16 (`BMDS_GAME_FONT_DEFAULT`), not the
`ui` role: games lay out on its 8×16 cells, and `ui` becomes IBM Plex in step 7.
`bmds_game_font(name)` opens a font once and keeps it; `bmds_game_font_scaled(n)` is unscii at
n × 16, so callers of the old `scale` keep their look.

### 4f. angel

Everything in §4e, as script calls registered with help lines and locked in `api.lock`:

- `class Font`: `Font(const string &in family, int size, int style = 0)`, `Font(const string &in
  name)` (a name string or a role, §4a), `string name` (the string back),
  `Font@ systemFont(FontRole role)`; `family`, `size`, `ascent`, `descent`, `lineHeight`,
  `maxAdvance`, `bitmap`, `monospace`; `int width(const ?&in value)`,
  `int fit(const string &in text, int maxWidth)`.
- `enum FontRole { Ui, Mono, Title, Terminal }`, `enum FontStyle { Bold = 1, Italic = 2 }`,
  `enum TextSmoothing { System, On, Off }`.
- `Screen`, `Image` and `Canvas` each get `text(x, y, value, color, const Font@ font,
  TextSmoothing smooth = System)` (Over) and `text(x, y, value, color, uint bg, const Font@ font,
  ...)` (Opaque).
- `array<string>@ fontFamilies()`.
- The GUI widgets' `font` property (§4g) and `FontDialog`/`FontButton` (§4i), generated with
  the other GUI classes; a widget's font as a `Font` from its name.
- **Kept:** today's `text(..., scale)`, `textWidth(value, scale)` and `textHeight(scale)`, now
  unscii at that multiple through the library. They're a short form games like.

### 4g. The GUI protocol and the toolkit

- **Widget property `font`** (appended in `classes.def`, a string): a role (`ui`, `mono`, `title`)
  or a name (`IBM Plex Sans 14 bold`); empty means the class's default. Remote clients get the
  same look as local ones, since the server draws.
- **The toolkit** asks for its fonts by role. `GuiStyle`'s heights come from the `ui` font's
  metrics instead of constants, and widgets measure with their font. The terminal widget stays in
  cells (`font/terminal`, a bitmap or monospace font).
- **Settings app:** an Appearance > Fonts section: each role a `FontButton` (§4i), then
  smoothing, contrast and a preview.
- Clients that draw a Canvas's text themselves use §4e in-process. A measuring request for remote
  Canvas clients waits until something needs it.

### 4h. Settings and the `font` command

| Key | Type | Default | Meaning |
|---|---|---|---|
| `font/paths` | String (templates aren't paths) | `/rom/share/fonts;${HOME}/Fonts` | Where faces are found |
| `font/ui` | string | `IBM Plex Sans 13` | The toolkit's text (§4a) |
| `font/mono` | string | `IBM Plex Mono 13` | Fixed-width text outside the terminal (editors, code) |
| `font/title` | string | `IBM Plex Sans 13 bold` | Window titles, headings |
| `font/terminal` | string | `unscii 16` | The terminal's cells |
| `font/fallback` | string list | `IBM Plex Sans;IBM Plex Mono` | Faces tried for a missing glyph (then unscii) |
| `font/smooth` | bool | `true` | Smoothing, unless a call says otherwise |
| `font/contrast` | int 0-100 | 30 (chosen on the board, §6d) | Coverage c drawn as 255 × (c / 255)^(1 / (1 + k / 100)): darker edges |
| `font/cache` | int, KB | 512 | The glyph cache's budget |

The sizes are first guesses, settled on the board in step 7; until then the first three say
`unscii 16`.

`font`: `font list`, `font info NAME`, `font stats`, `font draw NAME TEXT FILE.png` (renders to a
picture, for tests and for looking).

### 4i. The font dialog

- **`FontDialog`**, a GUI class beside `FileDialog` and `ColorDialog` (`classes.def`), drawn by
  the server like them:
  - Properties: `title`, `font` (a name string, §4a), `owner`, `sample` (the preview's text),
    `filter` (`all`, `monospace`, `scalable`).
  - Method: `open`. Events: `finished` (Bool: chosen, String: the font) and `changed` (off unless
    asked: the font as the user moves through the lists, for a live preview).
- **Layout:** a family list with a search box over it, a style list (the family's faces, plus a
  made-up bold), a size list with a spin box (a bitmap face offers only its sizes), and a preview
  drawn in the chosen font with the system's smoothing. Each family's name is drawn in its own
  face.
- **`FontButton`**, a widget like `ColorSwatch`: shows a font's name drawn in that font, and opens
  a `FontDialog` when clicked; its `font` property and a `changed` event. The Settings app's
  font rows are these.
- **Everywhere at once:** the server side in `bmds_gui` (`fontdialog.h`), the classes in
  `classes.def`, so `guiclasses.h` (the SDK), the protocol doc and the lock come from `guigen.py`;
  angel's `gui.cpp`; `guitest.c`, the `guigallery` example and the Widgets app show them.

## 5. Budget

- **Flash:** the faces and stb_truetype, 20.7 KB (step 1); the cache and kernels to come.
- **ROM:** the shipped faces (IBM Plex Sans regular, bold, italic; Mono regular, bold; Serif
  regular), as released (§3): 1.17 MB. The ROM image is 7.7 MB of the 10 MB partition.
- **PSRAM:** the cache's budget (512 KB), plus any face loaded from outside `/rom`.
- **Internal RAM:** the rasterizer's scratch, a few KB while a glyph renders. The Painter's strip
  is unchanged.
- **CPU:** a cached glyph costs about what an unscii glyph costs (sharp) or a few PIE
  instructions more a pixel (smooth, Opaque). Rendering a new glyph is milliseconds at most and
  happens once.

## 6. Steps

1. **stb_truetype:** vendor it (`thirdparty/stb_truetype/`, the stb_vorbis README as the
   template), and IBM Plex (`thirdparty/plex/`, subset into `/rom/share/fonts` by the build).
   `FontFace`/`Font` in `bmds_core` (host tested): open, metrics, kerning, rendering at
   1 and 4 bits. A bench (`font.render`): microseconds per glyph per size on the board.
2. **The cache and the face list** (`bmds_core`, host tested): keys, budget, LRU, pinning, the
   mutex, stats, `font/paths`, roles, fallback, the bitmap fonts as faces. The `font` command.
3. **The kernels:** 1 and 4 bits at any x into BGR24, RGB565, BGRA32 and GRAY8, Opaque and Over;
   Scalar and PIE writing the same bytes (host test plus a board check); a bench (`font.draw`).
4. **The SDK:** slots 127-132 in `sys.h`, the kernel side, libbmdsgame onto it (`font16.c`
   gone), an example, `progtest`, `ABI.md`, `abilock.c`.
5. **angel:** §4f, help, `api.lock`, an example script.
6. **The Painter:** Opaque and Over runs through the strip; the window manager's titles and menus
   follow the `title` and `ui` roles.
7. **The toolkit:** fonts by role, metrics from the font, the `font` property in `classes.def`
   (guigen, docs). Then the roles' defaults switch to IBM Plex, the sizes settled on the board.
8. **The font dialog** (§4i): `FontDialog` and `FontButton` in `bmds_gui`, `classes.def`, angel,
   `guitest.c`, `guigallery`, the Widgets app, and the Settings app's Fonts section.
9. **Lucky Duck's headline letters** (`luckyduck.md`), then the docs: `gui-protocol.md`,
   `sdk/README.md`, the roadmap.

### 6a. Step 1, as built (2026-10-26)

- **`thirdparty/stb_truetype/`** (1.26, unchanged) and **`thirdparty/plex/`** (six faces as
  released, Git LFS, OFL), staged as `/rom/share/fonts` and `/rom/share/licenses/ibm-plex/`.
- **`bmds_fonts`**, a new component, portable (host tests, fuzzer): **`FontFace`** over a font in
  memory.
  - `open()` checks the directory, every table it reads, loca in order inside glyf, the metrics'
    sizes and a Unicode character map (format 12 over 4). CFF and collections: `NotSupported`.
  - Names (typographic family and style first, UTF-16 to UTF-8), metrics, fixed pitch (`post`),
    `glyphIndex()` (the first 256 from a table), `advance()`.
  - `kerning()`: every lookup the `kern` features name (types 2 and 9, formats 1 and 2), each
    adding its first applying subtable; else the `kern` table's format 0 pairs.
  - `box()` and `render()` check the glyph first (every byte stb_truetype will read, contours in
    order, parts placed by offset, at most 8 deep, at most 16,384 points). `render()` rasterizes
    a byte a pixel into the caller's buffer and packs it there to 1 or 4 bits. stb_truetype's
    memory comes from `Internal` (else `Psram`), and a failure is told as `NoMem`.
- **Single precision:** stb_truetype's math calls are mapped to the `float` ones. The only doubles
  left are in its signed-distance-field code, which the linker drops.
- **Tests:** `tests/host/fonts.cpp` covers names, metrics, the character map, kerning against
  fontTools' figures (all 8,836 ASCII pairs add up to theirs), sharp and smooth agreeing, every
  glyph of all six faces drawn, and damaged files refused. **`fontface_fuzz`** has seeds from
  `tests/fuzz/make-ttf-seeds.py`. In its first 6 minutes it tripped a rasterizer assert, on an
  edge above a glyph's box at offset 0. stb_truetype is patched (`bmds-first-row.diff`), and the
  input is kept in `tests/host/fonts/fuzz/`. Then 30 minutes (1.7 million inputs) were clean.
- **`font.render`**, a bench: a face opened, ASCII rendered at 13-48 px, kerning, lookups, then
  the same from a copy in PSRAM. On the board (IBM Plex Sans, read in place from `/rom`):

  | Metric | Flash | PSRAM copy |
  |---|---|---|
  | open | 217 µs | |
  | render, a glyph at 13 px (smooth or sharp) | 101 µs | 101 µs |
  | render, a glyph at 16 / 24 / 48 px | 105 / 127 / 217 µs | |
  | kerning, a pair | 7.7 µs | 7.7 µs |
  | character map, past the first 256 | 0.55-0.66 µs | |

  Reading the font in place from flash costs nothing measurable: it is the CPU's work. Kerning
  was 14.5 µs at `-Os`; `fontface.cpp` is built `-O2`. Its dozen binary searches a pair (three
  lookups of four subtables in Plex) are still too slow to repeat for every pair drawn, so the
  cache keeps pairs as well as glyphs (§4c). A first render is about 100 µs a glyph, all of it
  once.
- **Seen on the host** (a line drawn to a picture): smooth text is clean at 11 px and up. Sharp
  TrueType at 11-13 px is ragged, because stb_truetype has no hinting. Smoothing on by default
  and unscii for sharp small text (§2) were the right calls.

### 6b. Step 2, the design (2026-10-26)

- **The bitmap fonts move into `bmds_fonts`**: bmds_ui's `Font` becomes `BitmapFont`
  (`<bmds/bitmapfont.h>`, 28 uses renamed), with `Fonts::unscii16`, `Fonts::icons16`, the
  generated unscii table and `IconGlyph`. bmds_ui then requires bmds_fonts. That frees the name
  `Font` for the library's font, which the Painter takes in step 6.
- **`FontName`**: `IBM Plex Sans 13 bold italic` or a role with overrides (`ui`, `title 20`).
  Read from the end: a number is the size, `bold`/`italic`/`regular` the style; the rest is the
  family. Formatted back the same way.
- **`GlyphCache`** (one per system, thread-safe, one `Mutex` held only for lookups, inserts and
  pins):
  - Key: face, pixel size, depth, made-up bold, glyph. Each entry is allocated from `Psram` on a
    miss. Allocation happens only on misses, which already cost a render (about 100 µs), never on
    a hit.
  - LRU over a 4,096-bucket hash. The budget (`font/cache`) counts entries and their rows. An
    entry is pinned while it is drawn (`acquire()` / `release()`).
  - A miss renders outside the lock. Two tasks rendering the same glyph keep the first copy.
  - It is a `Memory::Reclaimer`: when PSRAM runs short, unpinned glyphs are freed.
  - Kerning: a direct-mapped table of 4,096 pairs (32 KB, `Psram`, made on first use), looked up
    a run at a time under one lock.
  - Stats: entries, bytes, hits, misses, evictions, kerning hits and misses.
- **`FontLibrary`**:
  - **Faces:** a table of at most 64, appended to and never reordered, so a face's index is its
    cache id. The bitmap fonts are faces too (unscii: 16 px and whole multiples). TrueType files
    come from `font/paths` through a `FontFiles` interface (list a folder, map a file, load one),
    the VFS on the board and the host's files in tests. `/rom` files are mapped. Other files are
    read for their names at scan time, then loaded into `Psram` when first opened and kept until
    memory runs short.
  - **Style matching:** weight and italic from `OS/2` (added to `FontFace`). A missing bold is
    made up; italic is never made up.
  - **Roles, fallback, default size:** roles give a name (§4a); the fallback faces are tried for a
    missing glyph, then unscii; a name without a size takes 13 px (or a bitmap font's own size).
- **`Font`**, a handle: a face at a size, style, a bitmap font's scale. Copying it keeps its face
  loaded.
  - Pixel metrics (ascent, descent, line height, widest advance), from the face scaled and
    rounded out.
  - `layout()`: UTF-8 to positioned glyphs, with fallback faces and kerning, positions in 1/64
    px snapped per glyph.
  - `textWidth()` and `fitting()` built on `layout()`.
- **On the board** (`SystemFonts`): the system's library and cache. The settings of §4h
  (`font/paths` a String, since templates aren't paths: `/rom/share/fonts;${HOME}/Fonts`, each
  expanded) are watched and applied. The `font` command (in bmds_ui, which has the PNG writer)
  offers `list`, `info`, `stats`, `draw` and `rescan`. A `font.cache` bench measures a hit, a
  miss and laying out a line.

### 6c. Step 2, as built (2026-10-26)

- **As designed (§6b)**, with these details:
  - **The bitmap fonts moved:** `BitmapFont` is in bmds_fonts; bmds_ui requires it, and
    `tools/fontconv.py` writes the new name.
  - **`FontFace`** gained `weight()` and `isItalic()` (`OS/2`, else `head`).
  - **`Font::Placed`** carries the face or bitmap font drawing each glyph, its cache id, a
    bitmap font's scale and baseline, and the pen in 1/64 px. The drawing kernels of step 3 take
    these.
  - **Faces are found by style:** the family's faces by italic, then by the weight nearest 400 or
    700.
- **The cache's hot paths, measured and reworked on the board** (`font.cache`, IBM Plex Sans 13
  from `/rom`):

  | Metric | First version | As built |
  |---|---|---|
  | miss (box, render, allocate), a glyph | 112 µs | 117 µs |
  | hit, `acquire()` + `release()` | 1,295 ns | 768 ns |
  | a run's hit (`lookup()`, one lock a run) | | 260 ns |
  | kerning, a cached pair | 403 ns (mostly misses) | 334 ns |
  | layout, a character (`textWidth()`) | 1,386 ns | 947 ns |

  - **Hits:** a hit no longer moves its entry. It is marked used, and the eviction sweep gives a
    used entry a second chance (CLOCK).
  - **`release()`:** one atomic decrement, no lock.
  - **Kerning:** read without the lock (a sequence number per pair), two ways per set. The first
    bench warmed 8,836 pairs into 4,096 slots and so measured misses.
  - **Layout:** 16.16 fixed point for font units to 1/64 px instead of float. `font.cpp`,
    `glyphcache.cpp` and `fontface.cpp` are built `-O2`.
  - **Still to watch:** a 15-character label lays out in about 14 µs. If the toolkit's repaints
    show it (step 7), the next step is a per-face table of the ASCII advances.
- **On the board:** the `font` command lists the six Plex faces and unscii, opens the roles (all
  unscii until step 7), makes up Serif's bold, and draws lines to PNGs, smooth and sharp, with
  fallback glyphs (∑ → from the Plex fallback faces, ░ from unscii) and unscii doubled.
- **Size:** bmds_fonts is 84.4 KB of flash, including unscii's 46.4 KB table, which moved from
  bmds_ui. Step 2's code is about 16 KB, with 1.5 KB of internal RAM for its static objects. The
  cache's buckets (16 KB) and kerning table (48 KB) are in PSRAM, made on first use.
- **Tests:** `tests/host/fontlibrary.cpp`: names, the cache (hits, budget, pins, made-up bold,
  kerning, a run, threads, no memory, no leaks), and the library (scan, styles, roles, fallback,
  bitmap sizes, layout, loading and letting go).

### 6d. Step 3, as built (2026-10-26)

- **`TextBlend`** (bmds_fonts): `compose()` (coverage into alpha, the larger of two), `blend()`
  (one color by alpha; PIE for B, G, R, R, G, B and grey, 16 pixels a step: three planes, two QACC
  passes a channel, as the icons' blend), `fill()` (B, G, R four pixels to three words),
  `makeRamp()` and `paint()`. `simd test text` compares PIE with scalar on 2,000 random cases on
  the board: the same.
- **`TextDraw::line()`**: a `TextTarget` (pixels, format, size, stride, clip) and a `TextPaint`
  (color, a background box or not, smooth or sharp). It draws as far as a width allows and
  returns the width drawn. `font draw` now draws through it.
- **`GlyphCache::setCurve()`** and the curve argument of `FontFace::render()`. `font/contrast`
  defaults to 30, chosen from 0, 30, 60 and 100 drawn on the board: at 0, 13 px text is a little
  grey and soft; 60 and up start to look bold.
- **Found on the board:** `SystemFonts` watched the prefix `font/`, which matches no key (a
  prefix names whole parts: `font`), so no `font/` setting was followed. It watches `font` now,
  and a change of contrast or budget takes effect at once.
- **`font.draw`, measured as the design went** (a 57-character line of IBM Plex Sans 13 into
  640×24 B, G, R; µs a line; internal RAM and PSRAM alike):

  | Design | Over, PIE | On a box | Sharp |
  |---|---|---|---|
  | Each glyph's rows blended on their own (10 pixels a call) | 420 | 543 | 364 |
  | A run's rows composed, a row a call, levels then alpha | 396 | 522 | 411 |
  | Alpha composed directly, the box filled a run at a time | 318 | 483 | 320 |
  | Bands composed a glyph at a time, word fills | 272 | 387 | 286 |
  | On a box: fill, then the PIE blend | 268 | 310 | 285 |

  For comparison, unscii draws 2.2 µs a cell (`simd.glyphs`, PIE); TrueType is now 4.7-5.4 µs a
  character, laying out included (about 45 µs of the line). `simd.text`, the blend alone over
  16 rows of 1,024 pixels: B, G, R 680 µs scalar, 178 PIE; grey 522 and 56.
- **Tests:** `fontlibrary.cpp` covers compose, the ramp, every format's blend against the
  formula, fills, and lines drawn: over, cached, clipped, off the picture, width-limited, on a
  box, sharp, indexed, and unscii doubled.

### 6e. Step 4, as built (2026-10-26)

- **The calls** (§4e): slots 127-132 in `sys.h` (structs, function types, inline wrappers;
  `bmds_font_open_name()` for the common case), the kernel's side in
  `bmds_loader/src/fontcalls.cpp` over `SystemFonts`, `TextDraw` and the program's font table,
  locked in `abilock.c`. `text_draw` takes about 5 KB of the caller's stack (the band shrank to
  2 KB for it); program stacks are 32 KB unless a program asks for less.
- **Found on the board:** fonts as fds, as first designed (a `FontFile` stream like the synth's),
  ran the `fonts` example out of fds. A program has 16 fds in all; the example held 12 fonts and
  the 13th failed with `EMFILE`. So fonts became a table of the program's own
  (`Process::fontAdd()`, `fontOf()`, `fontClose()`, `MaxFonts` 16, allocated on the first
  `font_open`, freed with the program's other state), and `font_close` took slot 132.
- **libbmdsgame:** §4e. `bmds_game_font_scaled()` reaches unscii at 15 × 16 = 240 px (the
  library's bitmap multiple went from 8 to 15, the most a 255 px size allows); angel's `scale`
  is 1-15 now (it was 1-64).
- **Tests:**
  - **The host tests** run libbmdsgame's text on the real `FontLibrary` and `TextDraw` behind fake
    font calls in the fake system call table (`tests/host/fakefonts.cpp`, from `oss.cpp`). They
    check unscii's bits in each format and clip, scaled, beyond Latin-1, and smoothed TrueType
    into B, G, R and sharp into L8.
  - **The host `angel`** gets the same fake font calls (`angelsys.cpp`), so the script tests'
    text (`graphics.as`: widths and heights) runs on the real library.
  - **`progtest fonts` on the board** covers the faces, opening by name, family and role, info,
    the errors, measuring (kerned, to a newline, within a width), drawing into all six formats
    and clipped, a box, sharp, 200 opens and closes, and 16 at once and no more with the fds
    untouched.
- **On the board:** Blocks (built, not installed: run from `build-dist`) draws as it did. The
  `fonts` example (`sdk/examples/fonts`) shows each face in itself, sizes from 11 to 32, smoothed
  and sharp, white over blue, black on a yellow box, unscii.

### 6f. Other fonts tested (2026-10-26)

- **A sweep of every font on the development machine:** 2,577 files (Noto, DejaVu, Inter,
  Liberation, Roboto, URW, STIX, Thai and Korean faces, and decorative fonts from the user's
  `~/.fonts`). Each was opened by `FontFace` and every glyph rendered at 13 and 40 px, smooth and
  sharp, built with AddressSanitizer, UBSan and stb_truetype's asserts.
  - **First run:** no fault, no sanitizer report, no assert. But 92 of DejaVu Sans' glyphs were
    refused (`u` among them), and there were problems in five other files.
  - **After the fixes below:** 2,135 files open, all 2,046,406 of their glyphs render, and none is
    refused. Refused files: 435 CFF `.otf` (and one CFF font named `.ttf`), 5 collections, and
    NotoColorEmoji (colour bitmaps, no outlines).
- **Kerning against fontTools**, every printable ASCII pair, for ten fonts: DejaVu Sans,
  Liberation Serif, Noto Sans, Ubuntu and Lato (kerning in GPOS extension lookups: 1,314 and
  1,908 pairs), EB Garamond, Go Mono (none), Open Sans (a `kern` table only), Roboto, FreeSerif.
  Every pair the same.
- **Fixed:**
  - **A lone last point:** refused only when it is off-curve, the case where stb_truetype reads
    past its allocation. DejaVu's are on-curve hinting anchors, drawn safely; refusing them lost
    92 glyphs.
  - **Empty contours** (an end the same as the one before; Milea): accepted, and stb_truetype
    patched to skip them (`bmds-empty-contours.diff`). Unpatched, it joined every later contour
    into one shape.
  - **A box turned inside out** (Gemelli's compounds of a space): an empty glyph, not a damaged
    one.
  - **Glyphs of up to 65,536 points** (Toony's and Daubmark's hand-traced 15,000-21,000): was
    16,384. A first render takes up to about 1 MB for stb_truetype, `Internal` else `Psram`,
    once.
  - **A font with only a Windows symbol map** (Seized X-S): opens, its 0x20-0xFF also looked up
    at U+F020-F0FF, as Windows does (`bmds-symbol-cmap.diff` lets stb_truetype open it).
- **The fuzzer on the fixes** (15 minutes): one find, UBSan's. A compound part scaled past a
  vertex's 16 bits made stb_truetype's float-to-`short` conversion undefined; it is clamped now
  (`bmds-compound-range.diff`, the fourth patch). A second 15 minutes was clean.
- **Tests:** `tests/host/fonts/edge.ttf` and `symbol.ttf`, our own (MIT), made by
  `make-edge-fonts.py`, hold each case. The empty-contour test fails with the stb patch undone.
- **On the board** (the fonts copied to `/sd0/p0/claude/fonts`, `font/paths` pointed there for
  the test and set back): DejaVu Sans, EB Garamond, FreeSerif, Go Mono, Lato, Liberation Serif,
  Noto Sans, Open Sans, Roboto, Ubuntu, Seized X-S, Toony, Milea, Gemelli and Daubmark draw
  right, kerned, with accents and Cyrillic, ∑ and → from the fallback where a font lacks them.
  These are loaded into PSRAM, not read in place.
- **Names:** EB Garamond's file names its family "EB Garamond" and its style "12 Regular" (an
  optical size), so `EB Garamond 12 16` names no family and gives the `ui` role's font. The
  name's form can't end a family in a number (§4a).
- **Not covered:** CFF outlines (`.otf`) and collections (`.ttc`). They are a large part of what
  people have: 442 of the 2,577 here. stb_truetype reads CFF, through a charstring interpreter
  that would need checking and fuzzing like the rest. A question for later.

### 6g. Step 5, as built (2026-10-26)

- **`Font`** (`programs/angel/fonts.cpp`): `Font(name)` (a name or a role, "ui", "mono 20"),
  `Font(family, size, style = 0)`; `name`, `family`, `style` (the face's style name), `size`,
  `ascent`, `descent`, `lineHeight`, `maxAdvance`, `bold`, `italic`, `bitmap`, `monospace`;
  `width(value)` (the widest line) and `fit(text, maxWidth)` (bytes of the first line that fit).
  `enum FontStyle { Regular, Bold, Italic }`, `array<string>@ fontFamilies()` (sorted).
- **Changed from §4f:**
  - **No `FontRole` enum or `systemFont()`:** roles are names (`Font("title")`), as in the
    settings and the GUI's `font` property. `Terminal` and `System` are already classes in the
    script API, and enum values aren't scoped here, so `FontRole::Terminal` would clash.
  - **`TextSmoothing { Default, Smooth, Sharp }`,** the system's words, for the same reason
    (`System` is a class).
- **Sharing:** a program has 16 fonts, so fonts that come back with the same name share one
  object (`Font("IBM Plex Sans 14")` is `Font("IBM Plex Sans", 14)`), and a font is closed when
  the script lets go of it. A 17th at once raises an exception naming the limit; 90 opened one at
  a time are fine (checked on the board).
- **Drawing:** `Screen`, `Image` and `Canvas` get `text(x, y, value, color, font, smooth)` and
  `text(x, y, value, color, background, font, smooth)` (each line on a box); a null font is
  unscii 16. They go through libbmdsgame's new `bmds_canvas_text_flags()` (a background and
  `text_draw`'s flags). The `scale` forms stay.
- **Tests:** `tests/scripts/fonts.as` (names, sharing, metrics, measuring, drawing over, on a box
  sharp, smoothed, a null font, the errors) gives the same output on the host and on the board.
  The host's fake font calls gained opening by family and role, the face's style and `font_list`.
  `api.lock` appended. The `fonts` example (each family in itself; size, smoothing, bold, italic
  from the keys), and lesson 10 writes a line in IBM Plex.

### 6h. Step 6, the design and as built (2026-10-26)

- **`Painter::text(x, y, utf8, const Font &, fg, bg, maxWidth)` (Opaque):** a bitmap font at its
  own size (unscii 16) keeps `GlyphRenderer`'s path, the same pixels as before. Anything else is
  drawn through the strip by `run()` as before, a piece at a time: each piece filled with bg,
  then `TextDraw::line()` blends the line into it at minus the piece's column (clipped to it),
  which is what `TextDraw`'s Opaque writes on PIE. A line taller than the strip's 16 rows goes
  in bands of 16. Nothing is read back. The box is the advance of what fits in `maxWidth`.
- **Over in the Painter, left for its first caller:** each clip piece of the line's box read into
  internal RAM by `Surface::patch()` (a surface line's bytes at most: about 64 × 19 pixels), the
  line blended over it, written back: two 2D-DMA jobs a tile. Nothing in the window manager or
  the toolkit draws text on a picture, so it was written, then taken out rather than shipped
  untested.
- **Smoothing:** `Painter::Tools::smooth`, `font/smooth` as the window manager last heard it.
- **Roles followed:** `SystemFonts::watch()` (four listeners) is told after a `font/` change is
  applied. The window manager keeps `title` and `ui` fonts, opens them again then, and redraws
  everything.
- **Titles** in the `title` font, the ink box (ascent + descent) centred in the 20-pixel bar and
  cut to it (a clipped painter). The bar stays 20 pixels (`WindowFrame::TitleHeight` is a
  constant a dozen files use); its height from the font is a question for step 7, when the
  sizes are settled.
- **Menus** (the window manager's and the toolkit's, `drawMenu()`): `MenuLayout::layout()` takes
  a `Font` (the `ui` role) and keeps it; an item is at least 20 pixels, else the ink box and 4.
  Mnemonics underline under the baseline, half the descent down (unscii's last row, as before).
  The Alt+Tab switcher's title is in `ui` too.
- **Found on the board:**
  - **The window manager's task overflowed its stack** (8 KB, 2.5 KB free with unscii) on the
    first TrueType title: `TextDraw::line()` takes about 5 KB. The board reset. Now
    `TextDraw::Scratch` (the batch, the pieces and the band) can be the caller's: the window
    manager keeps one (`TextDraw::scratchBytes()`, internal RAM) in `Painter::Tools`, and a call
    without one puts it on its stack in a function of its own, so a caller with one never pays
    for it. The task's free stack stayed at 2.5 KB, rendering misses included.
  - **A title that wasn't a multiple of 4 pixels wide vanished:** the strip's copies are in
    4-pixel columns and `startBlit()` refused the piece (`Invalid`, noted and not shown).
    unscii's widths are multiples of 8, so the old path never met it. The box now ends on a
    column, bg past the advance.
- **Checked on the board:** with `font/title` IBM Plex Sans 14 bold and `font/ui` IBM Plex Sans
  14, titles (active and inactive), the window menu (mnemonics, shortcuts, separators) and the
  switcher change as the settings do, sharp with `font/smooth` off (two colours, sixteen
  smoothed). With the defaults (unscii 16) every pixel is as before (the bitmap path, 20-pixel
  items).

### 6i. Step 7, the design and as built (2026-10-26)

- **`GuiStyle`'s fonts at run time** (`bmds_gui`): `GuiStyle::font(role)` is the role's font as
  the settings last said (a copy: a count taken), opened from `SystemFonts` and opened again
  when `SystemFonts::watch()` tells of a change. `textHeight(font)` is its ink box (ascent and
  descent: 16 for unscii), `buttonHeight(font)` that and 6, 22 at least (as now). The constants
  `TextHeight` and `ButtonHeight` go; `Column`, `ButtonPad`, `IconSize` stay.
- **A restyle:** each `GuiApplication` is told (posted to its loop) and restyles its windows: a
  widget's own font opened again (a role follows the settings), `Widget::styleChanged()` for
  widgets that keep measurements, every layout run again at the window's size, all redrawn.
  Windows keep their sizes; a dialog sized from its hints is the next one opened.
- **A widget's font:** `Widget::font()` is its own (`setFont(name)`: a role or a name, kept in
  PSRAM only for widgets that have one) or its class's role (`fontRole()`: `ui`). Widgets
  measure and draw with it. The GUI protocol's `font` property (a string, `Widget`, appended to
  `classes.def`): "" is the class's.
- **Runs:** `Painter::text(x, y, utf8, font, runs, maxWidth)` draws one line in runs of colours
  (bytes, fg, bg), the box from x (a column) as before: each run's columns filled with its bg and
  the line blended in its fg, clipped to them. A line edit's selection is three runs. TrueType
  advances aren't multiples of 4, so a run can't start a call of its own.
- **Left in cells:** `TextEdit`, `CodeEdit` and `Terminal` draw an 8 × 16 grid of unscii cells
  (syntax colours, a cursor a cell wide). A TrueType mono font there needs the cell width from
  the font and runs at any column: a step of its own. They stay unscii.
- **The OS's own windows** (the panel's notifications and tray, the patch bay, the Settings
  app's previews, the updater, the image viewer) measure and draw with the `ui` role.
- **Then the defaults:** `font/ui` IBM Plex Sans, `font/title` IBM Plex Sans bold, sizes settled
  on the board (the 20-pixel title bar and 22-pixel rows as the limit). `font/mono` IBM Plex
  Mono (programs and angel). `font/terminal` stays unscii 16.

- **As built:**
  - **The defaults:** `font/ui` IBM Plex Sans 13, `font/title` IBM Plex Sans 13 bold,
    `font/mono` IBM Plex Mono 13 (2026-10-27, on the board). 13 px has an 18-pixel ink box: a
    title in the 20-pixel bar, two names' lines in the desktop's 76-pixel icon cells, buttons
    24 high and the panel 30. 14 px (19) is larger and still fits the bar, but the icon cells'
    second line runs 2 pixels over, and the panel is crowded.
  - **Measured, not counted:** `TextWrap::wrap()` takes a measure (a font's per-character
    widths; the cells version is one), and `GuiStyle::wrap()` wraps in a font: line edits, a
    form's help, message boxes, icon names. Row heights (tree, details, tabs, the patch bay, the
    notification list, the panel's dialogs and the panel itself) come from the font's ink box.
  - **Left in unscii:** the editors and the terminal (cells, above), the Settings app's palette
    preview (a miniature desktop on a 16-pixel grid), the updater's release notes (wrapped in
    cells) and a drag's count badge.
  - **The window manager's stack** went from 8 to 10 KB: widgets paint on its task and a glyph
    is rendered there the first time, which left 1.5 KB free; 3.8 KB now.
  - **Checked on the board:** the whole desktop restyles live when `font/ui` and `font/title`
    change (the Widgets app's three tabs, menus, the tree, icon names, the status bar, the
    panel and its clock); a line edit's selection starts mid-letter-pair; an angel script sets
    `font` on a label (serif 20, `mono`), a button (bold 18, taller) and a line edit; a bad name
    is `BadValue`; `progtest fonts` passes with the new defaults; Settings and the terminal look
    right.
  - **Not followed live:** the panel's height is set when it starts (a font change shows at
    the next start), and a dialog sized from its hints is the next one opened.

### 6j. Step 8, as built (2026-10-27)

- **`FontDialog`** (`bmds_gui/fontdialog.h`): a search box over the families (each drawn in
  itself: `ItemModel::rowFont()`, which a details or list view draws a row's name in, cut to the
  row), the family's styles in order (regular, bold, italic, bold italic: its faces' names, and
  "Bold" made up where it has none), sizes (21 from 8 to 72, or a bitmap face's multiples) with a
  field, and a preview in the font. Opened on a name or a role (shown as the font it is now);
  `fontName()` is what was chosen; `Filter` offers all families, fixed-width ones or TrueType
  ones; `Changed` for a live preview.
- **`FontButton`**: a button showing a font's name in that font (its own font); a click opens a
  `FontDialog`, a font chosen becomes its own and is told to `Changed`.
- **The GUI protocol:** `FontDialog` (`title`, `font`, `owner`, `sample`, `filter`; `open`;
  `finished`, `changed`) and `FontButton` (`filter`, `changed`; its font is `Widget.font`), so
  the SDK's wrappers, the reference, angel and both locks follow. `guitest` covers a widget's
  font (its height grows in a bigger one, a role, a bad name), a font button and a font dialog
  opened and destroyed; `guigallery`'s Dialogs tab has Font... and a font button, the answer
  written in the font chosen.
- **The Widgets app** (Controls: a font button) and **Settings** (Appearance > Fonts: text,
  window titles and fixed-width as font buttons, the last offering fixed-width families; smooth
  text, contrast, the folders, the fallback families, the cache). `font/terminal` isn't placed:
  the terminal still draws in unscii cells.
- **Checked on the board:** the dialog from the Widgets app (families in themselves, a family's
  styles, sizes, the preview following, OK applying it); from Settings, a fixed-width font
  chosen, saved and shown, then put back to its default; `guitest` passes.

### 6k. Fixes after step 8 (2026-10-27)

- **Glyphs at quarter pixels:** the pens were right but each glyph was drawn at its pen rounded to
  a pixel, so letters a pixel apart in one place touched in another ("pps" in the panel's Apps).
  `FontFace::box()` and `render()` take a phase (stb_truetype's x shift), `GlyphCache` keys on
  it, and `TextDraw` places a smoothed glyph at its pen's nearest quarter; sharp text keeps whole
  pixels (and one render).
- **Text at any pixel:** `Painter::text(Font)` may start off a 4-pixel column: the box starts on
  the column left of it, its background in the pixels between. The panel's clock uses it.
- **The tray's room:** the same at both ends (the 3 pixels inside the edge the icons had), the
  icons' 4-pixel gap between them and the clock, the time as wide as it gets (each digit the
  widest), measured when asked so a font change lays the panel out right. The width's rounding
  to a column goes between the icons and the time.
- **Settings:** the palette preview draws the window manager's frames (`WindowFrame`'s parts, the
  title buttons' glyphs) with the title in the title font and the rest in the toolkit's, its
  height from them, so it follows the fonts as it follows the colors. A slider's Save and Revert
  wait for the drag to end (shown at once they narrowed it, which moved the value under the
  pointer, which could take them away again). A click's focus no longer scrolls a form to show
  what was clicked (`GuiWindow`: the pattern grid moved under the pointer before the press was
  placed, choosing another pattern). A wheel notch moves a slider three steps, a page at most
  (`SliderModel::wheel()`): one step of contrast's 100 didn't show.

## 7. ABI impact

New slots 127-132 and their structs in `sys.h` (step 4, locked in `abilock.c`); libbmdsgame's two
text calls change their arguments (no ABI promise there, and nothing is released); angel gains calls and keeps its old
ones; `classes.def` gains a property and two classes (appended). On-disk: none (fonts are
standard TrueType files).

## 8. Questions answered (the user's, 2026-10-26)

1. **Which faces ship?** IBM Plex (Sans, Mono, Serif).
2. **TrueType by default?** Yes, once it looks right on the board (step 7).
3. **The cache's budget?** 512 KB of PSRAM.
4. **The user's font folder?** `${HOME}/Fonts`.
5. **Added by the user:** a font dialog, available through the SDK and angel like the other GUI
   classes (§4i).
