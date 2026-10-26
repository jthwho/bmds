# Wallpaper: a solid color or a tiled pattern behind the desktop

Status: **done, checked on the board** (steps 1-5; 2026-10-25, planned the same day with the
user). 31 patterns ship; the default is `cubes` (the user's). Since 2026-10-26 the 2D-DMA copies
the pattern straight from its tile (§6e), and the solid color is no longer drawn twice (§6f).
Fills in `gui.md` G9's "A wallpaper, later, is the same window": the panel's desktop window
draws it; the window manager doesn't change.

## 1. Goal

- **The desktop is a solid color or a tiled pattern** (the user's). Patterns are retro and fun:
  geometric ones and sci-fi ones, as some older computers had.
- **Each pattern has its own palette, and its colors can be changed** (the user's); **palettes can
  be saved and recalled, several for each pattern** (the user's).
- **Patterns come from `/rom` and from the user's `/home/Images/DesktopPatterns`** (the user's),
  so anyone can draw more.
- **Every wallpaper setting is in the Settings app** (the user's).

## 2. Decisions (the user's, 2026-10-25)

- **No photographs.** Windows draw directly on the screen (`windows.md` W1), so the desktop
  repaints whatever a window uncovers. A picture would need a scaled copy kept at screen size
  (1.8-2.7 MB at 720p) to repaint from. A pattern repaints from a tile of a few KB.
- Solid color or a tiled pattern; the patterns in `/rom` and in the user's folder.
- **New boards start with a pattern** (which one: chosen when the set exists).
- **Named palettes, saved and recalled, several for each pattern** (§4c).
- **Icon labels on a box of the desktop color** (§4e).

## 3. Facts that shape the design (checked 2026-10-25)

- **The desktop window** (`bmds_panel/src/desktopwindow.cpp`) is an `IconView` filling the
  screen at the desktop level. `paint()` fills it with the palette's `desktop` color, and the
  `IconView` fills its viewport with the same color (`iconview.cpp`, `setColors()`).
- **Icons are blended over a flat color:** `Painter::image(x, y, icon, bg)` blends the icon's
  alpha over `bg` and reads nothing back from the surface. Over a pattern, an icon's soft edges
  would show a fringe of the desktop color, so they need the pattern under them (§4e).
- **`Painter::rows()`** draws a band whose pixels a `RowMaker` produces a piece at a time in the
  window manager's strip, while the 2D-DMA copies the previous piece. That is the tiling path:
  only the decoded tile stays in memory (§4d).
- **The PNG decoder is our own** (`bmds_core/src/pngdecoder.cpp`) and handles palette images (color
  type 3, bit depths 1-8). It looks each index up in a 256-entry table read from `PLTE`, so
  replacing table entries recolors a pattern at no cost (§4b).
- **Settings already have templates** (`${HOME}` in `screenshot/path`, `editor/scripts`) and
  `Paths` lists separated by `;` (`panel/launcher/applications`, `bench/paths`).
- **`/rom/share/...`** comes from `mkrom.py`'s `--dir`/`--file` in `bmds_storage/CMakeLists.txt`.
  PNGs in the repository are kept in Git LFS (`.gitattributes`).
- **The Settings app** (`bmds_settings`) has rows by key, `.choices`, `.when`, a set of editors
  (`Editor::Auto`, `Slider`, `List`, `Folder`, `File`, ...) and previews (`previews.cpp`). Its
  Appearance page's Desktop section has `wm/palette/desktop` and `wm/palette/desktop-text`.

## 4. Design

### 4a. Settings (the panel's, `panel/desktop/...`)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `panel/desktop/wallpaper` | enum `color\|pattern` | `pattern` | What the desktop shows |
| `panel/desktop/pattern` | string | (a shipped pattern) | The pattern's name: its file name without `.png` |
| `panel/desktop/pattern-colors` | string | empty | The pattern's colors: `#rrggbb` a palette entry, `;` between them; empty or short: the file's own for the rest |
| `panel/desktop/pattern-scale` | int 1-4 | 1 | Each pattern pixel drawn as N×N screen pixels (sharp) |
| `panel/desktop/patterns` | Paths | `/home/Images/DesktopPatterns;/rom/share/patterns` | Where patterns are found: the first file of a name wins |
| `panel/desktop/palettes` | Path | `/home/.config/patterns.conf` | The saved palettes (§4c; step 3) |

- **Paths are settings**, and the user's folder comes first, so a user's file replaces a shipped
  pattern of the same name. `Paths` and `Path` settings must be absolute (no `${HOME}`
  templates); `/home` is the home folder at each lookup (`vfs.h`), so it follows `home`.
- **The desktop color stays `wm/palette/desktop`.** It is the solid wallpaper, the window manager's
  fill when there is no panel, and the backing of icon labels (§4e).
- **`pattern-colors` is what shows now.** Switching to another pattern sets it to the colors that
  pattern last had (§4c), or empties it (the pattern's own).
- **The default:** `cubes` (the user's, from the set in §4g).

### 4b. Pattern files

- **A pattern is a PNG of at most 256×256 pixels, tiled from the screen's top-left corner.**
- **Palette PNGs (color type 3) can be recolored:** their `PLTE` gives the default colors, and
  `pattern-colors` replaces entries in order. Settings offers the first 16 entries; patterns use
  2-16 colors.
- **Other PNGs tile as they are**, with no colors to change: a user's drawing in any paint program
  works. Transparent pixels show the desktop color.
- **Larger files are refused** (logged, and the desktop shows its color), as is a missing pattern.
- **The decoder gains a palette override:** `Images::Options` (or the PNG decoder's own options)
  takes an optional table of replacement colors. The tile is decoded once, into the screen's
  B, G, R, with the user's colors already applied. Changing a color decodes again (a few ms for
  256×256). No indexed pixel format is needed.
- **Names shown:** the file name, dashes as spaces (`tron-grid` shows as "Tron grid"). The
  shipped ones carry a PNG `tEXt` `Author` and `License` (ours, MIT).

### 4c. Saved palettes

- **A palette is a name and a color list** (the `pattern-colors` form), belonging to one pattern.
- **Built-in palettes travel in the pattern:** besides `PLTE` (shown as "Original"), a shipped
  pattern may carry more as PNG `tEXt` chunks keyed `Palette:NAME` (`Palette:Amber` =
  `#1a0f00;#ffb000`). So a pattern arrives with a few looks, and a user's PNG can do the same.
  They are read-only.
- **The user's palettes are in one file**, `panel/desktop/palettes` (an INI, a section for each
  pattern by name):

  ```ini
  [tron-grid]
  Current=#000010;#00e0ff;#004060
  Ice=#000010;#c0f0ff;#305070
  Sunset=#200010;#ff6030;#602040
  ```

  `Current` is the colors the pattern last had, so switching away and back keeps them. Every
  other key is a saved palette. A user's palette with a built-in palette's name replaces it.
- **Written only by Settings** (Save, Delete, and a pattern switch's `Current`), all at once
  through a temporary file and a rename. The panel reads only `pattern-colors`. A pattern whose
  file is gone keeps its section, harmlessly.
- **The file is user data, not a setting:** its contents are open-ended (any pattern, any
  number of palettes). Its path is a setting.

### 4d. Drawing (the panel)

- **`DesktopWindow` keeps the decoded tile** in PSRAM (§6e has the layout and sizes), scaled by
  `pattern-scale` when it is decoded.
- **`paint()` has the 2D-DMA copy the damage straight from the tile** (`Painter::tiled()`, §6e):
  no CPU a pixel. The damage is widened to 4-pixel columns (overdrawing the desktop costs
  nothing). Icons are still blended over the pattern in the strip (`Painter::rows()`), and
  `make()` still makes rows for the window check's reference.
- **The solid color stays a `fill()`.**
- **Settings take effect at once:** the panel watches `panel/desktop/` and `wm/palette/desktop`,
  decodes again, and repaints the whole desktop window.
- **No animation.** Redrawing the screen continually breaks "efficiency is correctness"; if ever
  wanted, it belongs in a screensaver.

### 4e. Icons over a pattern

- **The `IconView` gains a background hook:** instead of filling with a color, it asks its owner to
  paint a rectangle. The desktop paints the pattern, and every other view keeps its color.
- **Icons are blended over the pattern:** the icon's cell is made in the toolkit's picture (the
  canvas `IconView` already draws into off screen), pattern first, then the icon blended over it,
  then copied. That gives no fringe. This is the one change to `bmds_gui`.
- **Labels sit on a box of the desktop color** (`desktop-text` on `desktop`; the user's), so they
  are always readable over any pattern, as on the old desktops. A selected label is highlighted as
  now.

### 4f. The Settings app

- **Appearance → Desktop** gains:
  - **Wallpaper:** Solid color | Pattern (`.choices`).
  - **Pattern:** a new editor (`Editor::Pattern`): a grid of swatches, each a small tiled preview
    of a pattern found on `panel/desktop/patterns`, in its own colors, with its name. Shown when
    the wallpaper is `pattern` (`.when`).
  - **Its colors:** a new editor (`Editor::PatternColors`): one color button for each of the
    pattern's palette entries (as many as it has, up to 16). A pattern that can't be recolored
    says so.
  - **Its palettes:** a list of the pattern's palettes (Original, the built-in ones, then the
    user's), each with a strip of its colors; choosing one applies it. **Save as...** asks a
    name and saves the colors shown (an existing name asks to replace it); **Delete** removes a
    user's palette (built-in ones can't be).
  - **Scale:** 1×-4× (`Slider`).
  - **Patterns from:** `panel/desktop/patterns` (`List`).
- **The page's desktop preview** (`previews.cpp`) draws the pattern under its sample icon.
- **Open the patterns folder** in the Pattern editor: a button that opens
  `${HOME}/Images/DesktopPatterns` in Files (made if missing), so adding patterns is obvious.

### 4g. The shipped patterns

- **Made by a script in the repository, `tools/mkpatterns`** (Python and Pillow, fixed seeds), the
  way `tools/render-icons` makes the icons. Its PNGs are committed (Git LFS) in `rom/patterns/`,
  and staged as `/rom/share/patterns` by `mkrom.py --dir`. The procedural ones use larger tiles
  (up to 256×256) so the repeat doesn't show. The board has no generator code.
- **Our own designs, not copies:** the old Mac, Windows 3.1, Amiga and Atari ST patterns are the
  inspiration. Their bitmaps and names (Scottie, Critters, Paisley, ...) are not used.
- **The set (step 2, 2026-10-25; the user looked at each on the board and kept them all):**
  - *Classic two-color (8×8, 16×16):* checks, weave, bricks, diamonds, zigzag, dots, parquet,
    scales, grid.
  - *Geometric:* truchet, cubes (2:1 isometric tumbling blocks), waves, bauhaus, hexagons,
    triangles, arches, maze (dug on a torus), quilt.
  - *Sci-fi:* starfield, circuit, tron, hexarmour, scanlines, glyphs (our own 4×6 glyphs),
    radar, nebula (dithered fractal clouds), hull, invaders (our own aliens).
  - *Extras:* terrazzo, plaid (a twill-woven sett), topo (contours of smooth hills).
  - Names fit Settings' grid (10 characters): herringbone became `parquet`, pixelinvaders
    `invaders`.
- **Each with one to three built-in palettes** (`Palette:` chunks): 56 in all.
- **Size:** 31 files, 28.7 KB of ROM (most under 500 bytes; nebula 7.9 KB, topo 6.5 KB).

## 5. ABI impact

- **No syscalls or SDK changes.** The pattern file convention (a PNG of at most 256×256, palette
  PNGs recolorable, `Palette:NAME` text chunks, in `panel/desktop/patterns`' folders), the saved
  palettes' file format, and the settings' names are user facing, frozen once released (not
  yet).

## 6. Steps

### 6a. Step 1, as built (2026-10-25)

- **`Png::decode(..., colors)`** replaces a palette image's first entries (each keeping its
  `tRNS` alpha); **`Png::palette()`** reads them (for Settings, step 3).
- **`Backdrop`** (`bmds_gui`): `make()` is the owner's; `paint()` and `paintIcon()` draw it with
  `Painter::rows()`, a strip piece at a time while the 2D-DMA copies the last. An icon is blended
  over the backdrop in the strip, so it has no fringe. `IconView::setBackdrop()` uses it for the
  viewport and the icons, and puts the names on boxes of the background color.
- **`Wallpaper`** (`bmds_panel`, pure, host tested): the PNG decoded to 0xAARRGGBB (kept only if it
  has alpha, to blend again over a new desktop color), then a B, G, R tile scaled across, its rows
  repeated to at least 128 pixels so a row is made in a few `memcpy`s; down is scaled when made
  (row `y / scale`). Largest: a 256-wide pattern at 4×, 768 KB; the test patterns 3 KB
  (checks), 24 KB (truchet), 192 KB (starfield).
- **The panel** finds `NAME.png` on `panel/desktop/patterns` (at most 512 KB), and applies the
  settings at start and whenever `panel/` changes. Problems are said and leave the color.
- **`DesktopWindow`** paints the pattern around the view, and blends it again when the palette's
  desktop color changes (checked at each paint).
- **Test patterns** (`tools/mkpatterns`, in `rom/patterns`, staged as `/rom/share/patterns`):
  `checks` (8×8, 2 colors, 216 bytes), `truchet` (64×64, 3 colors, 459 bytes), `starfield`
  (256×256, 4 colors, 963 bytes), each with two `Palette:` chunks.
- **Tests:** `tests/host/wallpaper.cpp` (the palette read and replaced, colors parsed, tiling at
  any place, negative ones, rows past the kept width, scaling, a new background, refusals).

### 6b. Step 3, as built (2026-10-25)

- **Shared by the panel and Settings** (`bmds_gui`): `TiledPattern` (the panel's `Wallpaper`,
  moved and renamed); `PatternPalettes` (pure, host tested: `Palette:` chunks through the new
  `Png::nextText()`, the saved palettes' INI read and edited line by line, other lines kept);
  `PatternFiles` (the keys' names, `NAME.png` found and read, the patterns listed with the first
  of a name winning, the saved palettes' text read, and written beside and renamed over).
- **`panel/desktop/palettes`** (a `Path`, `/home/.config/patterns.conf`) is declared with the
  panel's other keys; only Settings writes the file.
- **Appearance → Desktop:** Wallpaper (Solid color | Pattern), then, while it is Pattern:
  - **Pattern** (`Editor::Pattern`): the current one's name and **Open Folder** (the first
    folder of `panel/desktop/patterns`, made if missing, opened by `files`) in the row; under
    it, in the form, `PatternChooser`: a 64×40 swatch of each pattern in its own colors (the
    current one in its colors now, framed), its name under it; a click, or Enter or Space,
    chooses. Choosing keeps the old pattern's colors as its `Current`, sets the new one's
    `Current` (else its own colors) with it, and saves both at once.
  - **Pattern colors** (`Editor::PatternColors`, `PatternColors`): a swatch for each palette
    entry (up to 16; the color dialog shows it on the desktop as it changes, OK saves, Cancel puts
    it back); the palettes (Original, the built-in ones, the saved ones marked "(saved)", or
    "Unsaved colors" first when no palette matches); **Save As...** (a name dialog) and
    **Delete** (a saved one). Every saved change is also kept as the pattern's `Current`.
  - **Pattern scale** (a slider, 1-4), **Patterns from**, **Saved palettes in**.
- **The preview** draws the pattern under its windows (the "Desktop" label on its box), from the
  preview's own corner.
- **Differences from §4f:** Save As with an existing name replaces it and says "replaced"
  (no second question); Delete doesn't ask either (the status bar says what happened). A
  pattern added to a folder while Settings is open shows when the page is opened again.

### 6c. On the board (2026-10-25)

- **Seen:** truchet across the desktop (no seam under the panel), the icon blended over it and
  its name on a box; `win check`: 0 of 885,532 pixels wrong (the reference draws the pattern
  too). Recolored, scale 3 and 4, starfield, checks; a missing pattern and bad colors leave the
  color. A `truchet.png` in `/home/Images/DesktopPatterns` replaced the shipped one.
- **Settings:** the preview on the pattern; a click on Starfield switched the desktop and saved;
  the Amber palette applied and was kept as `Current`; a swatch's color dialog showed red stars
  as it changed and Cancel put them back; Save As wrote `Late amber=...`; Delete removed it;
  back to Truchet brought its own colors back; Open Folder made the folder and opened it in
  Files. Everything survived a reboot.
- **Measured:** uncovering the whole desktop (1280×692, a window closed): 73-88 ms of drawing
  with the pattern, 53-67 ms with the color (the window manager's totals for those rounds):
  about 20 ms more, the CPU making 2.6 MB of rows from the tile. Done since: §6e.
- **Found and fixed:** Pillow pads a palette to its bit depth's 2^bits entries, so truchet showed
  a fourth, unused swatch (`tools/mkpatterns` now cuts `PLTE` back); a saved palette with a
  built-in one's colors showed as the built-in one, so Delete never applied (the palette last
  chosen or saved is shown when it matches, and choosing reads the palettes again: the same
  colors change no setting, so nothing else would); `Current=` was written empty for a pattern
  never recolored (now only a change is kept).

### 6d. The steps


1. **Drawing:**
   - The decoder's palette override.
   - The panel's settings, the pattern found on the path, decoded and tiled with `rows()`.
   - The `IconView` background hook, icons blended over the pattern, and labels on boxes.
   - Three test patterns.
   - Measured: decoding a tile, and repainting the whole desktop as a pattern against as a
     solid fill.
2. **The shipped patterns:** `tools/mkpatterns`, `rom/patterns/`, and staging into
   `/rom/share/patterns`.
3. **The Settings app:** the rows, the Pattern and PatternColors editors, the saved palettes
   (the file, built-in `Palette:` chunks, Save as, Delete, `Current` on a switch), the preview,
   and the folder button.
4. **Documents:** `gui.md` G9 points here; the roadmap. Done (2026-10-26).
5. **Drawn by the 2D-DMA** (asked 2026-10-26): §6e, and the solid colour's cost found (§6f).

### 6e. Drawn by the 2D-DMA (2026-10-26)

- **`Surface::startTile()`** fills a rectangle with a tile repeated across and down, through the
  clip, from any column and row of the tile. Each clip piece goes as chunks across (from the
  tile's column to its end, then whole widths) and bands down, each one receive descriptor; the
  transmit side is one descriptor for each run of the tile's rows (`BlitChain::addTiled()`, rows
  wrapping), or one a row when the 2D-DMA repeats each row `scale` times. The tile is written
  back from the cache once a call.
- **`Painter::tiled()`** clips to the surface, works out the tile's column and row at the corner,
  and waits for the copy (the tile may change after, as `pixels()`). **`Backdrop::tile()`** says a
  backdrop is such a tile; `paint()` uses it when its columns fall on the window's, else makes rows.
  Settings' preview forwards it, shifted to its corner.
- **`TiledPattern` keeps the tile as the 2D-DMA copies it:** rows of whole repeats on 4-pixel
  columns (`unit` = the least multiple of the scaled width that is one), at least 256 pixels
  (`MinRow`); scaled down too and repeated to at least 32 rows (`MinTall`) while that stays under
  256,000 bytes (`MaxScaledBytes`), else kept once and repeated by the 2D-DMA. Allocated on
  128 bytes (the L2 line). Sizes: checks 24 KB, cubes 36 KB, starfield 192 KB; topo at 2x is
  past the limit (each row repeated): 384 KB.
- **On the board:** `win check` 0 wrong for topo 1x and 2x, nebula 4x, scanlines 3x (36 wide),
  hexarmour 3x, hexagons, dots 4x, cubes; the preview drew. Uncovering the whole desktop, the
  window manager's drawing over the rounds that redrew it: **52-58 ms with a pattern, against
  87-89 ms before** (the CPU making rows), and 66 ms with the solid color. That was put down
  to its wide fills sending a 1280-pixel line a descriptor a row; wrongly (§6f). Rounds where
  the panel was busy (`Deferred` counted) left the desktop for later and were left out.

### 6f. The solid color, measured (2026-10-26)

- **`display.fillcost`** (a bench, `BenchQuick`): 1280×692 of an off-screen surface, the scan
  reading surface 0 meanwhile. `Surface::fill()` batched: **15.1 ms**; `startTile()` from a
  24 KB tile in PSRAM: 24.0 ms; raw chains from a colour's rows in PSRAM: 24.0 ms whether
  1280 wide by 12 rows a descriptor or 256-pixel chunks; one internal row a descriptor: 15.1 ms.
  The 2D-DMA's PSRAM reads cost; its descriptors don't. Solid colour rows in PSRAM (tried first,
  4 slots of 48 KB) made the desktop slower: 100 ms.
- **The 66 ms was the desktop drawn twice:** `DesktopWindow::paint()` filled its whole window
  with the colour, and the icon view then filled its viewport (all but the panel's strip) again.
  With a pattern it already painted only around the view. Now the colour is filled around the
  view too: uncovering the desktop **35 ms with the solid colour** (from 66), 53 ms with a
  pattern; `win check` 0 wrong, a colour change repainted the whole desktop.
