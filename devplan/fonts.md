# Fonts: one font library for the desktop, programs and angel

Status: **steps 1 and 2 done** (2026-10-26: §6a, §6c); planned the same day with the user (§8). Fills the
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
- **Positions:** advances and kerning add up in 1/64 pixels so the spacing is right, and each glyph
  is snapped to a whole pixel. That means one cached render per glyph, not one per sub-pixel offset.
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

- **Kernels** (PIE, with Scalar twins, written for every format below):
  - 1 bit: a mask selecting fg, at any x (bit shifts across bytes).
  - 4 bits: per pixel `dst + (fg - dst) × c / 15`, with a 16-entry table for contrast (a gamma
    correction setting, `font/contrast`, to try on the board).
  - Glyphs may overlap (negative kerning), so even the Opaque mode blends glyphs onto the filled
    box rather than writing whole cells.
- **Target formats:** BGR24 (the surfaces), RGB565, BGRA32 and GRAY8. L8 (indices) only sharp:
  indices can't be mixed.
- **In the OS (the Painter):** the strip works as today, filled with bg (Opaque) or loaded from
  the surface by the 2D-DMA (Over), then the glyphs are blended in at any x. The run's box keeps
  the 4-pixel column rule at its edges, which the Opaque fill covers. Over is the costly one and
  is rare: icon captions already sit on a box of the desktop color (`wallpaper.md` §4e).
- **In programs:** the call draws into the program's own buffer with the CPU, in the program's
  task, as image decoding writes into its buffer.

### 4e. The program ABI (`sdk/include/bmds/font.h`, new system calls)

Fonts are fds, like the synth's, so a program's exit closes them. Structs start with
`uint32_t size`. Slots from 127 (exact layout in step 4):

| Slot | Call | Does |
|---|---|---|
| 127 | `bmds_font_open(const bmds_font_spec_t *)` | Opens a font by family, pixel size and style, by role, or by a name string (§4a); returns an fd |
| 128 | `bmds_font_info(int fd, bmds_font_info_t *)` | The face chosen, its size, ascent, descent, line height, widest advance, flags (bitmap, monospace, made-up bold) |
| 129 | `bmds_text_measure(int fd, const char *utf8, size_t len, int32_t max_width, bmds_text_extent_t *)` | Width, bytes that fit, ink box |
| 130 | `bmds_text_draw(const bmds_text_draw_t *)` | Draws into a buffer (pixels, format, width, height, stride, a clip rectangle): font, x and y (the top of the line), fg, bg or none, smoothing (system, on, off), max width; returns the width drawn |
| 131 | `bmds_font_list(uint32_t index, bmds_font_face_t *)` | The faces installed: family, style, file, bitmap sizes |

`close()` closes a font. **libbmdsgame:** `bmds_canvas_text()` and `bmds_game_text_width()` take
a font fd and go through these calls. `sdk/game/font16.c` and `tools/gamefont` go away, since
unscii comes from the OS like any other font. A null font (-1) means the `ui` role.

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
| `font/contrast` | int | (found on the board, step 3) | The 4-bit coverage table's gamma |
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
4. **The SDK:** slots 127-131, `font.h`, the kernel side, libbmdsgame onto it (`font16.c` gone),
   an example, `progtest`, `ABI.md`, `abilock.c`.
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

## 7. ABI impact

New slots 127-131 and `sdk/include/bmds/font.h`; libbmdsgame's two text calls change their
arguments (no ABI promise there, and nothing is released); angel gains calls and keeps its old
ones; `classes.def` gains a property and two classes (appended). On-disk: none (fonts are
standard TrueType files).

## 8. Questions answered (the user's, 2026-10-26)

1. **Which faces ship?** IBM Plex (Sans, Mono, Serif).
2. **TrueType by default?** Yes, once it looks right on the board (step 7).
3. **The cache's budget?** 512 KB of PSRAM.
4. **The user's font folder?** `${HOME}/Fonts`.
5. **Added by the user:** a font dialog, available through the SDK and angel like the other GUI
   classes (§4i).
