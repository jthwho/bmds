# GUI: a Toolkit, Menus, and the Panel

Status: **steps 1-7 built** (2026-10-06; Phase 4, `windows.md` §8 step 8: context menus and the
panel), then, asked for by the user: the 3D look (§4e), cascading windows (§4f), window
management keys and Always on Top (§4g), icons (§4h, §4j), the Windows key, snapping and rolling
up (§4k). As built and measured: §9.

## 1. Goal

- **A small GUI toolkit** that applications are written with: widgets (labels, buttons, menus,
  layout boxes), an event loop, timers. The panel is its first real user; the terminal and the
  window manager's own menu share its pieces. It is written so that it can ship in the SDK once
  GUI programs can be built separately (`windows.md` §6): it reaches the window system only
  through the same public calls any program has.
- **Context menus:** the window menu (the title bar's left button, or a right click on a window's
  panel button) and the desktop's menu (a right click on the desktop: the launcher's entries).
- **The panel** (`/bin/panel`), an ordinary program on the toolkit, along the top:

      | Launcher | Window list                                          | Clock |

  The launcher's entries come from **configuration files**, one per application, with defaults
  kept in the repo and installed on `/boot`; each names a command, an icon (kept now, drawn once
  there are icons), and whether it runs in a terminal. The panel's own layout is a configuration
  file too: which items, in which order, each item's settings, so that new kinds of items (a
  system tray, a CPU meter) are added without changing the others.

## 2. Facts that shape the design (checked 2026-10-06)

- **The window manager's API is C++ in the kernel** (`WindowManager`), with callbacks run on its
  task with its lock held, which must not call it back (`windowmanager.h`). Only the redraw has
  to run there (it draws on surface 0 during the round); the others only report something.
- **Windows get keys, the wheel, resizes and closes, but no pointer events:** a press in a client
  area raises the window and goes no further. Menus and buttons need presses, releases and motion.
- **Every window is in one stack:** no layers, no window that stays on top, none that never takes
  the focus, no work area (maximize fills the screen).
- **Nobody is told when windows change:** `list()` exists; a window list would have to poll it.
- **The window manager draws its frames itself** (`windowmanager.cpp`): a title bar, four
  16 × 16 buttons from `Fonts::icons16`, colors from `WindowPalette`. The menu button does
  nothing yet.
- **Text is unscii-16:** 8 × 16 cells, wide characters 16 pixels: a string's width is its cells
  × 8, computed without drawing.
- **`/boot` is FAT on flash, always mounted;** reading is cheap, a write about 0.35 s
  (`storage.md` §13). `HOME=/boot`. Its defaults (`/boot/profile`, `/boot/autoexec.sh`) are
  string constants written only when it is first formatted, so a board formatted before a new
  default existed never gets it.
- **Packages install under `/sd0/p0`** (`bin/`, `share/licenses/`; `dist/README.md`).
- **Programs:** a built-in program is a `BMDS_PROGRAM` entry in `/bin` with a `Context`
  (arguments, environment, descriptors, `ProgramLauncher`); `ProgramLauncher::start()` starts a
  child without waiting; `ProcessControl::disownChild()` lets it go.
- **The SDK takes C++ without exceptions, RTTI or the standard library** (`sdk/README.md`). The
  toolkit keeps to that (no `std::` containers or strings; `bmds` types and fixed arrays).
- **`formatTime()`** (bmds_core) formats a `LocalTime` with `strftime`'s conversions.

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| G1 | Where the toolkit lives | **A new component, `bmds_gui`**, above `bmds_ui`: it uses only `WindowManager`'s public calls, the `Painter`, the font and the palette. Its pure parts (menu geometry, layout, the INI reader, desktop entries) are host-tested. When GUI programs get an ABI (`windows.md` §6), the same sources move into the SDK as a library over it | in `bmds_ui` beside the window manager (then nothing keeps it to the public API) |
| G2 | Threads | **The application's own thread runs everything but drawing:** window-manager callbacks only queue an event (a fixed ring, motion merged) and wake it; `GuiApplication::exec()` dispatches them with the application's lock held, and handlers may call the window manager. Drawing runs on the window manager's task (it must: the round) and takes the application's lock **without waiting** (`tryLock()`): when a handler holds it, the window asks for an update at the next round (`update()`, which takes no lock) and draws then. So there is no lock order to keep. (Designed first: handlers' window-manager calls gathered and made unlocked after each handler; the non-blocking lock made that unneeded.) Redraws a handler asks for (`Widget::update()`) are gathered per window and sent once after each round of events | handlers on the window manager's task (they couldn't call it) |
| G3 | Pointer events for windows | **A `mouse` callback per window** (`WindowConfig::mouse`): press, release, motion and leave, in client coordinates, with the button and the buttons down. A press in a client area keeps sending that window motion and the release until every button is up, even outside it (an implicit grab). Without the callback, as now | — |
| G4 | Levels | **Each window is on a level** (`WindowStack::Level`; "layer" was taken: it is a place in the stack): `Desktop` (below everything), `Normal`, `Above` (applications' windows kept on top, §4g), `Panel` (always above applications' windows), `Popup` (above everything). Raising stays within the level. Only applications' windows (`Normal`, `Above`: `isApplication()`) take the focus, are minimized or maximized, or are in a window list | — |
| G5 | Popups | **A popup layer window closes when a press lands outside every popup** (that press is used up, as in Windows and GTK), and gets the keys while it is open (the focus's frame stays as it was). Submenus are popups over popups | a grab call (X11): more API, same effect |
| G6 | Work area | **A panel window reserves the strip it covers** (`WindowConfig::reserve`): maximizing fills the screen less the strips, maximized windows follow when a strip changes, and windows placed or moved keep their title bars out of it | maximize under the panel |
| G7 | Window list | **`WindowManager::watch(client, fn)`:** called once a round in which the list changed (opened, closed, titled, focused, minimized, restored, maximized, an icon set); the panel then calls `list()`. `Info` gains the layer and whether an icon is set | the panel polling `list()` |
| G8 | The window menu | **Drawn and run by the window manager** (it is the window manager's, as in every desktop), with the toolkit's pure menu geometry and its drawing function, so it looks like every other menu: Restore, Minimize, Maximize, Close (Move and Size later). The panel opens it for a window with `showWindowMenu(id, x, y)` | the application drawing it (then a window without the toolkit has none) |
| G9 | The desktop | **The panel opens a desktop-layer window** filling the screen, drawn in the palette's `desktop` color (`win color desktop` still works), whose right click opens the launcher's menu. Without the panel the window manager fills the desktop as now. A wallpaper, later, is the same window | a call to claim the desktop's clicks (and nothing to grow into) |
| G10 | Launcher entries | **freedesktop.org Desktop Entry files** (`NAME.desktop`, INI): `Name`, `Comment`, `Exec`, `Icon`, `Terminal`, `Categories`, `NoDisplay`, `Hidden` of the `[Desktop Entry]` group; other keys and groups ignored. A standard every desktop reads, easy to write by hand, and packages can bring their own | a format of our own |
| G11 | Where entries are | **A search path, first match of a file name wins:** `/boot/applications` (the defaults, the user's to change), then `/sd0/p0/share/applications` (what packages install). A user's file of the same name in `/boot` replaces a package's; `Hidden=true` there hides it. The panel's configuration can name other directories | one directory |
| G12 | The panel's configuration | **`/boot/config/panel.conf`, INI:** `[Panel]` with `Edge` and `Items` (a list of section names, in order); each item a section whose `Type` (default: its name in lowercase) picks its kind and whose other keys are that kind's settings. Unknown kinds and keys are reported and skipped. Without the file, the built-in default (the same as the installed one) | a fixed layout |
| G13 | Item kinds | **A table of kinds in the panel** (`launcher`, `windows`, `clock`, `spacer`; append only): each kind makes a widget from its section. A tray is a new kind (and, for other programs to put things in it, a protocol: later) | — |
| G14 | Defaults on `/boot` | **The files under `boot/` in the repo are embedded in the firmware** (a generated table, as the font is) and installed: every default file missing from `/boot` and not listed in `/boot/.defaults` is written, and its name added there. So a new release's new defaults reach boards formatted before it, and a default the user deleted stays deleted (to have it back: remove its line, or the whole `.defaults`). `/boot/profile` and `/boot/autoexec.sh` move there from `bootdisk.cpp`'s constants. Reading `.defaults` each boot costs a small read; writing happens once per new default | written only at the first format (as now) |
| G15 | Run in a terminal | **`Terminal=true` runs `terminal -e COMMAND [ARG...]`:** the terminal gains `-e` (appended to its interface), which runs the command (through `PATH`) on its pty instead of an interactive shell, and closes when it ends. | `/bin/sh -c` in a terminal (a shell process more) |
| G16 | `Exec` | **Split into arguments by the specification's quoting** (double quotes, backslash escapes), not run by a shell; field codes (`%f`, `%u`, ...) dropped (nothing passes files yet); the program found through `PATH`. A shell line is `Exec=sh -c "..."` | every entry through `/bin/sh -c` |
| G17 | Icons | **16 and 32 pixel PNG files in a theme on the card**, rendered from SVG by a script and kept in the repository (§4h), and **icons built into programs** (§4j). (First: names kept, not drawn) | a multi-size container of our own |
| G18 | Colors | **The toolkit's colors are palette roles,** appended: `face` (menus, the panel, buttons), `face-text`, the face's four shades for its bevels (`face-highlight`, `face-light`, `face-shadow`, `face-dark`: §4e), `highlight` (the menu item under the pointer), `highlight-text`, `disabled-text`. Every application's widgets look alike and `win color` restyles them. The defaults are the classic grey (`c0c0c0`) with a navy highlight | colors per application |

## 4. Design

### 4a. The window manager (additions; `windows.md` §4k when built)

- **`WindowConfig` gains** (appended): `mouse` (a `Mouse` callback, `MouseEvent`), `level`
  (`WindowManager::Level`: `Desktop`, `Normal`, `Panel`, `Popup`; default `Normal`), `reserve`
  (pixels of the screen's edge the window covers and reserves, for a panel: top or bottom by
  where it is), `focusable` (default true; desktop and panel windows set it false).
- **`MouseEvent`:** `type` (`Press`, `Release`, `Motion`, `Leave`), `x`, `y` (`int16_t`, client
  coordinates: negative or past the size during a grab), `button` (the one pressed or released),
  `buttons` (down after the event). Motion is sent once a round at most (the last place), and
  only when it moved within the client area or during a grab; `Leave` when the pointer leaves the
  client area (onto the frame, another window, or away during no grab).
- **`WindowStack`** keeps each window's level: an open or a raise puts the window at the top of
  its level, under every window of a higher one. Host-tested as the stack is now (the damage
  rules unchanged, random levels added).
- **Focus** skips windows that aren't focusable: pressing one doesn't raise it above its level
  or take the focus; keys keep going to the focused normal window. A popup takes the keys while
  open (the top popup), without changing which frame shows as focused.
- **Popups:** a press outside every popup closes all of them (their `closed` callbacks run) and is
  used up (its release too). Opening a popup from a press (a menu button) works because the
  closing press came first. A popup opened while a client area holds a press takes the pointer:
  the pressed window is told `Leave`, and the motion and the release go to the window under the
  pointer (the menu), so a press on a button, a drag onto an item and a release there choose it.
- **Reserved strips:** the work area is the screen less the open panels' strips. `maximize()`
  uses it; when it changes, maximized windows are resized to it. A normal window's place keeps
  its top inside it (`placed()`), as it now keeps windows on the screen.
- **`watch(client, fn, arg)`** (one per connection; null to stop): called at the end of a round
  in which the list of normal windows changed. `Info` gains `level` and `hasIcon`. `list()` is
  unchanged otherwise. Also new: `screen()`, `workArea()`, `rect(id)`.
- **The window menu** (G8): a press on a title bar's menu button opens it under the button;
  `showWindowMenu(id, x, y)` opens it anywhere (the panel's right click). It is a popup window the
  window manager owns: drawn by the toolkit's menu drawing, run by the window manager's pointer
  and key handling (Up, Down, Return, Escape), its choice acted on directly. Items not possible
  are disabled: Restore when neither minimized nor maximized, Maximize when maximized or the
  window has no maximize button, Minimize when it has no minimize button.

### 4b. The toolkit (`bmds_gui`)

- **`GuiApplication`:** connects (`WindowManager::manager(display)`, `connect()`), owns the event
  queue (64 events in a ring: motion for the same window merged; an overflow redraws every
  window), the lock, timers (up to 16, `startTimer()`: each a deadline; the loop waits until the
  nearest), posted calls (`post(fn, arg)`: from any thread) and up to 255 windows (16 until 2026-10-07). The window furniture (scroll areas, a menu bar, tool and status bars, a splitter, a
  progress bar, dialogs) is `filebrowser.md` §4e's. `exec()` runs
  until `quit()`, the window manager stops (0), or a signal raises the program's interrupt (it
  waits with `Interrupt::waitCurrent()`; 130). One per program.
- **`Widget`:** a rectangle in its window (window coordinates), a parent and children (a list
  through the widgets themselves), shown or hidden, enabled or not; virtual `paint(Painter &)`,
  `mousePressed`, `mouseReleased`, `mouseMoved`, `mouseLeft`, `keyPressed`, `sizeHint()`,
  `resized()`; a stretch factor and a widest size for layouts. `update()` asks for its rectangle
  to be redrawn. `Widget::create<T>(parent, ...)` makes one in PSRAM owned by its parent, which
  destroys it (Qt's model; the codebase has no `UniquePtr` yet); `destroy()` takes one out.
- **`GuiWindow`:** the root widget of a window-manager window: its config (title, decorations,
  level, reserve, focusable, kind, pointer shape, whether it draws a reference picture for
  `win check`), `show()`, `close()`, `screenRect()`, a central widget that fills it. Painting
  walks the widgets the painter's clip shows. Mouse events go to the deepest widget under the
  pointer; after a press, to the pressed widget until release (the grab, mirrored). Keys go to
  the focus widget, then the window.
- **`Label`** (text, alignment), **`Button`** (text, icon name, `setClicked()` for the left
  button's click, `setPressed()` for any press at once (a menu opens on it, a right click), an
  outline under the pointer, checkable: the panel's window buttons are checked while their window
  has the focus), **`BoxLayout`** (horizontal or vertical: children's size hints, widest sizes
  and stretch factors through `LineLayout`, spacing; recomputed on resize; the gaps filled),
  **`Menu`** (below).
- **`Menu`:** items (text, a shortcut's text, a number, disabled, checked, a submenu, a
  separator; icons later), one `Triggered` callback for the menu and its submenus.
  `popup(anchor)` opens it as a popup window
  sized to fit (moved to stay on the screen; above its point if it doesn't fit below). The
  pointer highlights; a release on an item chooses it (so press-drag-release works, as does
  click-then-click); Up, Down, Return, Escape; Right and Left for submenus. Its geometry is the
  pure **`MenuLayout`** (item rectangles from the texts, the hit test, host-tested) and its
  drawing **`drawMenu()`**, which the window manager's window menu uses too: both live in
  `bmds_ui` beside the painter (the window manager can't depend on the toolkit).
- **Styling** in one place (`GuiStyle`, and `MenuLayout`'s sizes): a menu item 20 pixels high,
  separators 8, a button's padding, the panel's 28-pixel height (§4e); the palette roles of G18.
- **Not now:** text entry, scroll bars, lists, dialogs, check boxes. They come with the programs
  that need them.

### 4c. The panel (`/bin/panel`)

- **One process, two windows:** the panel (level `Panel`, no decorations, the screen's width, 28
  pixels (§4e), reserving its strip, not focusable) and the desktop (layer `Desktop`, the whole screen,
  not focusable). `panel` is started from `autoexec.sh` after `win start` (`panel &`); a second
  `panel` finds one running (a window of kind `panel`) and says so.
- **Items** from `panel.conf` (G12, G13), in a horizontal `BoxLayout`:
  - **`launcher`:** a button (`Label`, default "Apps"; an icon later) opening the menu of entries,
    sorted by name; `Categories` make submenus once there are more than `Submenus` entries
    (default 12; the main categories: `System`, `Utility`, `Development`, `Graphics`,
    `AudioVideo`, `Game`, `Other`). `Applications` names the search path. The desktop's menu is the same menu.
  - **`windows`:** a button per normal window (stretching, at most `MaxWidth` pixels, default
    200), in the order they opened: the title (an icon later), checked when focused, the title
    in brackets when minimized. A click raises it, or minimizes it if it has the focus; a right
    click opens its window menu.
  - **`clock`:** the local time (`Format`, default `%a %d %b %H:%M`; `Zone` for another zone),
    redrawn at each minute (or each second when the format has seconds), its width fixed by
    formatting a wide time. A click: a calendar later.
  - **`spacer`:** empty, `Width` pixels or stretching.
- **Starting an entry:** its `Exec` split (G16), the program found on the panel's `PATH`
  (`Vfs::findProgram()`: the shell's search, moved there from the shell so that the shell, the
  terminal's `-e` and the panel agree), run with `ProgramLauncher::start()` in the panel's
  environment and directory, in a session of its own, and disowned; with `Terminal=true`, as
  `terminal -e ...`. A failure (not found) is reported on the panel's standard error.
- **Configuration read at start;** `panel --reload` later (or a change watched). Errors go to
  standard error with the file and line.

### 4d. Configuration files

- **`IniReader`** (bmds_core, pure): walks a buffer's lines: groups (`[Name]`), `key=value`
  (spaces around both trimmed), comments (`#`, `;` at a line's start), blank lines; reports each
  with its line number; a line that is none of them is an error the caller reports and skips.
  Lists are `;`-separated (the Desktop Entry convention; `Items=launcher;windows;clock`).
  Booleans `true`/`false`. Localized keys (`Name[de]`) are skipped for now.
- **`DesktopEntry`** (bmds_core, pure): the `[Desktop Entry]` group's keys of G10 into a value
  (fixed sizes: a name of 63 bytes, a command of 255), and `Exec` split into arguments.
- **The files** (installed by G14; under `boot/` in the repo):

      boot/profile                         (moved from bootdisk.cpp)
      boot/autoexec.sh                     (moved from bootdisk.cpp)
      boot/config/panel.conf               the panel's layout
      boot/config/terminal.conf            the terminal's colors (§4l)
      boot/desktop/terminal.desktop        the desktop's icons (§4m)
      boot/config/wm.conf                  the window manager's desktop menu (§4o)
      boot/applications/terminal.desktop   Terminal: terminal

  Packages bring their own in `share/applications/` (BusyBox: `vi.desktop`, a text editor in a
  terminal).

### 4e. Depth: the classic 3D look (the user's request, 2026-10-06)

Modelled on the desktops of the 1990s (Windows 95's look; Motif's shadows): faces lit from the top
left, their edges bevelled.

- **`Bevel`** (`bmds_ui`, pure, host-tested): a style's edges as rectangles and shades. Two-pixel
  bevels are two rings, each a lit side (top, left) and a shaded side (bottom, right, which takes
  the corners where the sides meet), in the face's four shades: `Raised` (a button: highlight and
  dark outside, light and shadow inside), `Sunken` (the same reversed: pressed or checked),
  `Window` (a window, a menu, a panel: its outer lit line the softer light), `Field` (a text
  field, sunk); one-pixel: `Status` (a status field, sunk), `Thin` (a flat button under the
  pointer). `drawBevel()` draws one with a fill a shade.
- **Buttons** are raised; pressed with the pointer on them, or checked, they sink, the text a
  pixel lower; checked, their face is the lighter shade (the classic taskbar's active window).
  A flat look (toolbars) shows a thin edge only under the pointer. No bold anywhere: the first
  build made checked buttons and "Apps" bold, and the user found the smeared glyphs too hard
  to read (2026-10-06).
- **Labels** may have a frame (`setFrame()`: the panel's clock sits in a `Status` field).
  **`BoxLayout`** gains margins and a frame drawn in them (the panel's row: a `Window` bevel).
- **Menus** (`drawMenu()`, so the window menu too) have a `Window` bevel (`MenuLayout::Edge` 2),
  etched separators (a shadow line over a highlight line) and the navy highlight.
- **The panel** is 28 pixels: the bevel, a pixel, buttons of 22, a pixel, the bevel. The launcher
  stays pressed in while its menu is open (`Menu::setDismissed()` tells it when the menu closes).
- **The title bar's buttons** (the window manager's) are raised with the same bevel around their
  glyphs (which leave the outer two pixels blank), and sink while pressed (they swapped colors
  before). `win check` models the bevel. The frames themselves stay flat for now.
- Text still starts on a 4-pixel column, so a pressed button's text moves down a pixel, not right.

### 4f. Cascading new windows (the user's request, 2026-10-06)

- **`WindowConfig::cascade`:** a normal window that asks is placed by the window manager; its
  size is kept. **`cascadeRect()`** (pure, beside `WindowStack`, host-tested): down a diagonal
  from 16 pixels inside the work area's top-left corner, `CascadeStep` (24: a title bar and its
  border) a step, the first place where no shown normal window has its top-left corner. A
  diagonal that would leave the work area starts again at the top two steps to the right; with
  every place taken, or a window too big for the steps, the first place (as far up and left as
  it fits). A window closed frees its place for the next one.
- **The terminal** asks for it unless given `X Y`; so terminals from the panel's launcher, from
  `terminal &` and from `terminal -e` cascade. `win term N` still places its own (the tests'
  places).

### 4g. Window management keys and "Always on Top" (the user's request, 2026-10-06)

- **Alt+Tab** (the user's design): the window manager keeps the key and shows, in the screen's
  middle, a box (320 pixels wide at least, drawn as a menu is) listing the applications' windows:
  the focused one first, then the others top to bottom, then the minimized ones (`[title]`).
  Each Tab moves the choice down, Shift+Tab up, around the ends, redrawing only the two rows;
  nothing is raised until Alt is released, which raises the choice (restoring a minimized
  one). Escape cancels; a click on a row chooses it. So Alt+Tab once flips between the last two
  windows. A window closing meanwhile cancels the list. The list lives in PSRAM while it shows
  (about 1.3 KB).
- **Alt+F4** closes the focused window (when it has a close button); **Alt+Space** opens its
  window menu under its title bar. **Alt and a left press anywhere in a window** moves it, as by
  its title bar (as X window managers do). The keys the window manager keeps are not delivered;
  a key sent to a chosen window (`keyTo()`, `win type`) bypasses them.
- **Always on Top:** a level of its own (`Above`, between `Normal` and `Panel`), set by
  `setAbove(id, on)` or the window menu's checkable "Always on Top" (`WindowStack::setLevel()`;
  the window is redrawn whole, as the change is rare). Such windows still take the focus, are
  minimized, maximized and listed like any.
- **Focus is the focusable window raised last** (it was the top focusable one): a window raised
  under an always-on-top one has the focus and the keys. When it closes or is minimized, the
  top focusable window has it.
- **Test aids** (appended to `win`): `win above N on|off`; `win hid press|release|repeat USAGE
  [MODIFIERS]` sends a raw key event (HID usage and modifier bits) through the window manager as
  a keyboard would, for keys `win key` can't type (Alt+Tab, arrows: the menus' arrow keys are now
  tested); `win list` marks `(above)`.
- **Not yet:** a key to open the panel's launcher (the Windows key) needs a way for a program to
  claim a global key (`WindowManager::grabKey()`, a later step).

### 4h. Icons (decided by the user, 2026-10-06; built)

The user's set: icons8's **Flat Color Icons** (github.com/icons8/flat-color-icons; MIT, or
icons8's "Good Boy License"): 329 icons, SVG only, on a 48 × 48 grid, flat shapes with alpha.
Rendered with `rsvg-convert` they read well at both 32 and 16 pixels.

- **Sizes: 16 and 32** (the user's choice), the two the classic desktops used: 16 for menus,
  panel buttons, title bars and Alt+Tab's rows; 32 for desktop icons and a bigger switcher later.
- **In the repository** (the user's: rendered by a script, not by the build, both kept, PNGs in
  Git LFS): `thirdparty/flat-color-icons/` holds upstream's `svg/`, license and readme at a pinned
  commit (`BMDS.md`: version, commit, how to update) and the rendered `png/16x16/`, `png/32x32/`.
  `tools/render-icons` renders them (rsvg-convert; the output is the same bytes run to run) and
  removes PNGs left without an SVG. `.gitattributes` puts every `*.png` in LFS.
- **On the card:** the `icons` distribution package (`dist/icons/`: `source_tree`, `sdk = false`)
  installs the theme as `/sd0/p0/share/icons/flat-color/16x16/NAME.png` and `32x32/`, with
  `index.theme` and a link per alias (`dist/icons/aliases`: the freedesktop.org names Desktop
  Entries use, `utilities-terminal` → `command_line`, `start-here` → `services`,
  `application-x-executable` → `template`, ...). The build refuses LFS pointers ("git lfs pull").
- **PNG decoding** (`bmds_core`, pure, host-tested): `Inflate` (RFC 1951 and the zlib wrapper,
  Adler-32 checked; canonical Huffman codes decoded a bit at a time, the output its own window,
  small rather than fast) and `Png` (8-bit grey, RGB, palette of 1 to 8 bits with `tRNS`, grey
  with alpha, RGBA, every filter, chunk CRCs checked; 16-bit and interlaced refused) into
  0xAARRGGBB.
- **Drawing:** `Icon` (`bmds/icon.h`: a size and 0xAARRGGBB pixels its owner keeps) and
  `Painter::image()`, which blends the alpha over the background color the caller names (the
  face, a menu row's highlight, the title bar) while filling the strip: nothing is read back from
  the surface. Any height, in bands of the strip's. The old glyph namespace `Icon` became
  `IconGlyph`.
- **Where they show:** menu items (`MenuLayout::Item::icon`, the toolkit's `Menu::addItem(...,
  icon)`), buttons (`Button::setIcon()`, before the text), the title bar (a window's 16 × 16 icon
  replaces its menu button's ≡, as the classic title bars showed theirs; it still opens the menu),
  Alt+Tab's rows (copied when the list opens), the panel's window buttons (copied with
  `WindowManager::copyIcon()`: another program's pixels can't be kept). `win check` models the
  title bar's icon.
- **`IconTheme`** (`bmds_gui`; one in each `GuiApplication`): `find(name, side)` looks for
  `SIDExSIDE/NAME.png` in `/boot/icons` (the user's), the theme on the card, then `hicolor`; an
  absolute path as it is. `program(path, side)` is a program's own (§4j). `forProgram()` is a
  launcher entry's: its `Icon=`, else its program's own, else `application-x-executable`. Icons
  found are decoded once and kept, each in one allocation sized to it (a 24-byte header, its
  name, its pixels: about 1.1 KB for 16 × 16) on a list, nothing before the first (the user's
  suggestion: a table of 32 fixed slots, each with a 96-byte name, was mostly empty). Past
  `MaxBytes` (64 KB) the icon used longest ago goes. One not found isn't kept, so it is found
  once the card mounts.

- **A second theme: Glyphs** (the user's choice, 2026-10-06, after looking at Streamline's
  set: CC BY 4.0 and 30,547 icons, mostly thin line or flat ones that wash out at 16 pixels).
  gorango's Glyphs (MIT) keeps its icons as JSON exported from Figma; `tools/import-glyphs`
  (a pinned commit, fetched into a temporary directory) writes the core set's coloured variant
  (`poly`, 860 icons; the other four are one light grey) as `thirdparty/glyphs/svg/`, and
  `tools/render-icons` renders it. The `icons-glyphs` package installs the `glyphs` theme with
  its own aliases (`start-here` → `venn`, `utilities-terminal` → `laptop-code`, ...) and
  `Inherits=flat-color`; both packages share `tools/install-icon-theme`. **Flat Color stays
  the default** (the user's); `IconTheme=` in `[Panel]` picks another.
- **Theme lookup, as built with the second theme:** `/boot/icons`, the theme, the theme it
  inherits (its `index.theme`'s `Inherits=`, the first; read once the card is there), the
  default theme, `hicolor`. A file of the wrong size for a name is passed over (the first
  build kept it, and returned it the second time it was asked for).

### 4j. Icons built into programs (the user's question, 2026-10-06; built)

As a Windows `.exe` carries its icons, a BMDS program does: launchers and title bars show it with
no icon file installed and no `Icon=` written.

- **The format** (`sdk/include/bmds/program.h`, appended; frozen once shipped): notes with the
  name "BMDS" and type `BMDS_NOTE_ICON` (2), in a section `.note.bmds.icon` that isn't allocated:
  not loaded with the program, read from the file only when something shows it. Each note's
  descriptor is a `bmds_icon_note_t` (`size` of the header, `width`, `height`, `format`:
  `BMDS_ICON_FORMAT_PNG`, `length`), then the image. One note a size. The ABI lock checks the
  layout.
- **The SDK:** `bmds_add_program(name SOURCES ... ICONS a.png b.png)` reads each PNG's size from
  its header and writes an assembler file of the notes (`.incbin` of the images): no other tool.
  The linker keeps a non-allocated section as it is; `strip --strip-unneeded` keeps it. `hello`
  has one (16 and 32: the theme's `idea`).
- **Built-in programs:** `BMDS_PROGRAM_ICONS(name, main, stack, icons)` (an array of
  `ProgramIcon`: size and PNG bytes in rodata, made from PNGs by `bmds_embed_icons()` in CMake,
  `tools/embedicons.py`). The terminal has `command_line`, the panel `services`.
- **Reading one:** `ProgramImage::findIcon()` (pure, host-tested on the SDK's `hello.elf`) finds
  the note nearest a size (that size, else the next bigger, else the biggest) without reading the
  images; `ProgramIcons::read(path, ...)` returns the PNG of a built-in (`/bin/NAME`, links
  followed) or a file.
- **Used by:** `IconTheme::program()`; the panel's launcher (an entry without a usable `Icon=`
  shows its program's); the terminal sets its window's icon from its own (`/bin/terminal`), so
  its title bar, panel button and Alt+Tab row show it.

### 4k. The Windows key, snapping, rolling up (the user's request, 2026-10-06; built)

- **Global keys:** `WindowManager::grabKey(client, usage, modifiers, fn, arg)` (8 at most,
  released with the connection): a key with exactly those modifiers (left and right alike) is
  told on its press and not delivered. A modifier key with no modifiers is a **tap**: told on its
  release when no other key came while it was down, its press and release kept from windows (the
  Windows key; Win+E is still E with Gui to the window). The window manager's own keys come
  first. The toolkit's `GuiApplication::grabKey(usage, modifiers, fn, arg)` posts the call to
  the loop.
- **The panel** claims both Windows keys (`0xe3`, `0xe7`): a tap opens the launcher's menu at its
  button with the first item highlighted (the arrows, Return and Escape work), the button pressed
  in; another tap closes it.
- **Snapping:** a window dragged by its title bar with the pointer at or above the work area's
  top (on the panel) is maximized when dropped; within 2 pixels of a side, it takes that half of
  the work area. The outline shows where it will go. A snapped window keeps its size and place:
  dragged by its title bar again it takes its size back under the pointer (the same place across
  its title bar), and Restore (the window menu, `restore()`) puts it back. Maximizing a snapped
  window keeps the size from before it snapped. Only windows that can be maximized snap to the
  top, and resizable ones to the sides.
- **Rolling up** ("shading", as X window managers call it): `shade(id, on)`, the window menu's
  "Roll Up" / "Roll Down", or the wheel over the title bar (away from the user rolls up). The
  window becomes its frame's top and bottom; its application isn't told (its client area keeps
  its size, hidden), and it keeps the focus and its keys. A rolled-up window isn't resized by its
  handles; maximizing, restoring or snapping rolls it down first. `win shade N on|off`, and `win
  list` marks `(shaded)`.

### 4l. The terminal's own colors; resources (roadmap, 2026-10-06)

(Since `sysconf.md` step 3 the terminal's colors are parameters, and §13 removed the resources
below, which then had no user.)

The last of `windows.md` §8 step 9: the window manager draws no terminal, so the palette's
`terminal-text`, `terminal-background` and `terminal-cursor` roles leave it.

- **Resources** (as X's resource database): the window manager keeps strings by name for its
  applications, knowing none of them. `setResource(name, value)` (a name of 31 bytes at most, a
  value of 63; an empty value removes it), `resource(name, out)`, and `watchResources(client, fn,
  arg)`: the connection is told each change (its name and value) on the task that set it, the
  window manager locked (as a redraw is: it must not call the window manager); then every window
  is redrawn, as for a palette change. They last until `stop()`. One PSRAM allocation each,
  nothing until the first; a connection's watcher costs 8 bytes of `.bss`.
- **`TerminalColors`** (bmds_ui, with `TerminalView`, which takes it instead of the palette): the
  default text, background and cursor colors and the 16 ANSI colors (xterm's unless set); the 256
  palette's others and RGB as before.
- **The terminal** reads them when it starts: the defaults, then `/boot/config/terminal.conf`'s
  `[Colors]` group (`Text`, `Background`, `Cursor`, `Color0` to `Color15`; `RRGGBB` or
  `#RRGGBB`), then the resources `terminal-text`, `terminal-background` and `terminal-cursor`. It
  watches those, so a change shows in every open terminal. The file is a new default on `/boot`.
- **`win color`** (removed 2026-10-07 for `sysconf`, `sysconf.md` §13) still took the three names: `win color terminal-text RRGGBB` sets
  the resource. Its list shows the palette's roles, then the three, `-` where not set (the
  terminal's own setting applies).

### 4m. Desktop icons (roadmap, 2026-10-06)

- **What they are:** the Desktop Entry files (G10) of `[Desktop] Icons` in `panel.conf`
  (directories, `;`-separated; `/boot/desktop` unless set, empty: none), the first file of a name
  winning as the launcher's do, hidden ones left out, by name. The default `/boot/desktop` has
  the terminal's entry. At most 32.
- **Drawn by the panel's desktop window** (it stays a view: the panel gives it each item's name
  and icon and is told when one is opened): a 32 × 32 icon (the entry's `Icon=`, else its
  program's own, else `application-x-executable`; copied, so the theme's cache may drop it), and
  its name under it in up to two lines of 72 pixels (broken at a space; a second line that
  doesn't fit is cut and ends in an ellipsis, `…`). Cells of
  80 × 76 pixels from the work area's top-left corner, down then across, as the classic desktops
  do. The name is in `desktop-text` (a palette role, appended: white) on the desktop's color.
- **The mouse:** a left press on an icon or its name selects it (its name in `highlight` and
  `highlight-text`), anywhere else clears the selection; a second press on it within the double
  click time (`WindowManager::DoubleClickMs`) opens it as the launcher would. A right press
  anywhere still opens the launcher's menu.
- **The card later:** an icon not found (the theme is on the card, mounted after the panel
  started) is looked for again every 2 seconds, for a minute at most.
- **`win check`:** the desktop's reference picture draws the icons and names too.
- **Not yet:** keys, and files other than Desktop Entry files.
- **Moving them** (2026-10-06): an icon pressed and dragged more than 4 pixels follows the
  pointer (drawn above the others); dropped, it takes the grid cell under its middle (kept on
  the grid: no free placement, as "align to grid" desktops), and an icon already there takes the
  dropped one's old cell. A drag ends no double click. **`IconGrid`** (bmds_gui, pure,
  host-tested): items placed in cells, those with a cell wanted (inside the grid, not taken by
  an earlier one) there, the rest in the first free cells down then across; the cell under a
  point.
- **Kept:** `/boot/config/desktop.conf`, written by the panel after each move (one small file,
  rewritten whole: about 0.35 s on `/boot`, after the drop is drawn), `[Icons]` with a line a
  file name: `terminal.desktop=COLUMN,ROW`. Read when the icons are, so a screen of another size
  keeps the places that fit; an icon without one, or whose place is off the grid, is placed as
  before. Not a default on `/boot`: absent until an icon is moved.
- **An icon's menu** (2026-10-07): a right click on an icon selects it and opens Open, Copy and
  Delete. Copy puts its `.desktop` file on the clipboard as a URI list and its path
  (`clipboard.md` §4f). Delete moves it to its filesystem's trash (`files.md` F8).
- **The desktop's menu** (2026-10-07, `files.md` F7): Applications (the launcher's menu, as a
  submenu), a separator, Paste. Paste copies the clipboard's files into the desktop's first
  directory, under a copy's name if theirs is taken. Before this, the right click opened the
  launcher's menu itself.
- **`WindowManager::showMenu()`** (2026-10-07, `files.md` §4c): a menu of a program's own items,
  drawn and run as the window manager's, for programs not on the toolkit (the terminal's right
  click).

### 4n. Alt+Tab with 32-pixel icons (roadmap, 2026-10-06)

- **The look** (the classic one): a raised box in the screen's middle with the windows' icons
  in a grid, 32 × 32 each in a 48-pixel cell (8 a row at most), the chosen one's cell in the
  highlight color, and under them, in a sunken field, the chosen window's title (`[title]` for a
  minimized one, cut to fit). At least 256 pixels wide, so a short list still shows a title.
  Tab and Shift+Tab move, Alt's release chooses, Escape cancels, the mouse points and clicks, as
  before.
- **Icons:** a window has a large icon besides its small one: `WindowManager::setLargeIcon(id,
  icon)` (32 × 32 at most, kept by the owner as the small one is), `GuiWindow::setLargeIcon()`.
  Without one, the small icon is drawn doubled; without either, the cell is empty. The terminal
  sets both from its built-in icons.
- **No copies:** the list keeps the windows' numbers and looks their icons up as it draws, with
  the window manager's lock held, which `setIcon()` and `setLargeIcon()` take too (the owner
  keeps an icon until it sets another). The list was 16 KB of copied pixels; now one 4 KB buffer
  for a doubled icon.
- **`SwitcherLayout`** (bmds_ui, pure, host-tested): the grid's columns and rows, each cell's and
  icon's rectangle, the title's field, the cell under a point; 4-pixel columns throughout.

### 4o. The window manager's desktop menu (the user's request, 2026-10-06)

**Goal:** a window system without the panel (or any program) still gives a terminal: a right
click on the bare desktop opens a menu of the window manager's own; a program that wants its
own desktop menu (the panel) provides it.

- **Overriding it:** a right press where **no window** is opens it. A program that covers the
  desktop with a desktop-level window (the panel's, which draws the icons) takes the press and
  shows its own menu; with the panel stopped, or its desktop window off (`[Desktop] Menu=false`
  and no `Icons`), the window manager's shows. Nothing more to claim or release.
- **The window manager knows no program:** `setDesktopMenu(labels, fn, arg)` gives it the items'
  labels (10 at most, 31 bytes each, copied) and a function told which was chosen, on its task
  with its lock released. It reuses the window menu's popup (`MenuLayout`, the keys, the mouse).
- **`win start`** gives it the items of `/boot/config/wm.conf`'s `[Menu]` group, a line an item
  (`Label=command`, in order; a new default with `Terminal=terminal`; that one item if the file
  isn't there), and runs the chosen command as the panel runs a launcher entry: split as a
  Desktop Entry's `Exec`, found on `PATH`, started in a session of its own. What a launch needs
  outlives the command: `win start` keeps a copy of its exported environment and its directory,
  and a file table on `/dev/null` (the started program's standard files; its window is where it
  speaks). A failure is logged.

### 4p. Window keys, the terminal key, minimizing only with a window list (the user's, 2026-10-06)

- **The standard keys** (Windows'; the window manager's, so with or without a panel), on the
  focused application window:
  - Win+Up maximizes; Win+Down restores a maximized or snapped window, else minimizes.
  - Win+Left and Win+Right snap it to that half of the work area (as dragging to a side does);
    pressed again on a window snapped there, they restore it.
  - Alt+F10 maximizes or restores.
  - A key with the Windows key held makes its release no tap (the panel's launcher stays shut).
- **Ctrl+Shift+Windows runs a terminal:** the three held together (in any order, either side) run
  `wm.conf`'s `[Keys] Terminal` command (`terminal` by default): a terminal from the keyboard when
  no program gives one, hard to press by chance. The window manager tells `win start`'s function
  (`setDesktopMenu()`'s) the index `DesktopKey`. It wins over a program's claim of the Windows key
  (no launcher opens with it).
- **Minimizing needs somewhere to go:** a window can be minimized only while a program watches the
  window list (`watch()`: the panel's buttons bring windows back). Without one, the minimize button
  is drawn disabled (its mark in `disabled-text`) and does nothing, the window menu's item is
  disabled, Win+Down only restores, and `minimize()` reports `InvalidState`. When the last watcher
  goes (the panel ends), the minimized windows are shown again, so none is lost. `canMinimize()`
  says which (`win check` draws the button as the screen should show it).

### 4q. Names from file names, renaming in place, message boxes, a line editor (the user's, 2026-10-07)

The user noticed two desktop icons named alike with different file names, and asked for names
from the file name, renaming in the icon's label, and a message box in the GUI API.

- **A desktop icon is named by its file:** `terminal (copy).desktop` is "terminal (copy)".
  Copies, renames and the file stay in step. `Name=` still names the launcher's entries, which
  come from the applications' own files.
- **`LineEdit`** (bmds_gui), a line of text to edit:
  - Up to 255 bytes.
  - Keys:
    - characters insert; Backspace and Delete;
    - Left, Right, Home and End move, with Shift selecting;
    - Ctrl+A selects all; Ctrl+C, Ctrl+X and Ctrl+V use the clipboard (`GuiClipboard`, text);
    - Enter accepts; Esc cancels.
  - Mouse: a press puts the cursor, a drag selects, a double click selects all.
  - It scrolls to keep the cursor in view, drawn in a field's bevel.
  - Its model (`LineEditModel`: text, cursor, selection, the editing operations on whole UTF-8
    characters) is pure and host-tested.
- **`MessageBox`** (bmds_gui):
  - `MessageBox::show(app, kind, title, text, buttons, fn, arg)`.
  - Kinds: information, warning, error, question. Buttons: OK; OK and Cancel; Yes and No; Yes,
    No and Cancel.
  - A window with a title bar, centred in the work area and above other windows while shown.
  - A 32-pixel icon: the theme's `dialog-information`, `dialog-warning`, `dialog-error` or
    `dialog-question` if it has one, else a drawn badge.
  - The text wrapped to 50 columns at most, 12 lines (`TextWrap`, pure, host-tested). The
    buttons at the bottom right, the first one the default.
  - Enter is the default, Esc or the close box Cancel (or No, or OK when it's alone).
  - The answer is told on the loop's thread, then the box goes. It doesn't stop the program's
    other windows (the toolkit has no modality): the caller waits for the answer.
- **Renaming a desktop icon:**
  - It starts from Rename in the icon's menu; a click on a selected icon's name, released
    without moving, once the double-click time has passed (a double click still opens it); or
    a press held still on an icon for 0.8 s.
  - An editor (a `LineEdit` in a popup window) covers its name, the name all selected. It is
    as wide as the cell, wider for a long name, up to three cells.
  - Enter, or a press elsewhere, renames it; Esc keeps the old name.
  - The name is checked first:
    - unchanged: nothing;
    - empty, holding `/ \ : * ? " < > |`, only dots and spaces, or longer than 39 bytes: a
      message box, then editing goes on;
    - another icon's: "There is already an icon named NAME here.", then editing goes on.
  - Then the file is renamed (its `.desktop` kept), and it keeps its place on the grid.
- **As built** (2026-10-07):
  - **Pure** (host-tested in `tests/host/gui.cpp`): `LineEditModel` (whole characters, the
    selection, the 255-byte limit, controls left out) and `TextWrap` (spaces, LF, long words,
    wide characters, the line limit).
  - **`LineEdit`:**
    - It scrolls a character at a time. It has nothing to scroll until it has a size
      (`resized()`): text set before its window opened showed empty.
    - A paste still coming when it's destroyed is dropped (`GuiClipboard::cancel()`).
  - **`PopupEdit`** (bmds_gui): the in-place editor, a `LineEdit` in a popup window. A press
    elsewhere accepts, as Enter does.
  - **`MessageBox`:**
    - Its icon is copied from the theme: the theme's cache may drop its own.
    - Flat Color has only `dialog-information`, so the others are drawn badges.
    - Its buttons centre their text (`Button::setCentered()`).
  - **The panel:**
    - Names from file names.
    - Rename in the icon menu.
    - `DesktopWindow` asks for a rename (`RenameAsked`) after a long press (`LongPressMs`,
      800 ms) or a slow second click on the name (released, then 450 ms without a press).
    - On a problem, the editor is closed before the message box opens: a press on the box
      would otherwise count as a press elsewhere, which accepts. When the box is answered,
      editing goes on.
    - The renamed entry's file and path are updated, then the places are written, so it keeps
      its cell under the new name.
  - **On the board:**
    - The icon read "terminal" (its file).
    - Rename, then typing "My Shell" and Enter, gave `My Shell.desktop` and
      `My Shell.desktop=0,0` in `desktop.conf`, with the label changed.
    - Copy and Paste made `My Shell (copy).desktop`. A slow second click on its name opened
      the editor. "My Shell" brought "There is already an icon named "My Shell" here.", and
      OK put "My Shell" back in the editor; Esc left the files as they were.
    - A long press opened the editor; "a/b" brought the forbidden characters' message.
  - **Size:** the image grew 8,208 bytes to 851,456.

### 4r. Editing like the label, mnemonics, Tab (the user's, 2026-10-07)

The user asked for three things:

- "The inline rename edit box shouldn't have a border and should replace the label text when
  visible... look as much like the label as possible (except with the feedback to the user
  they're editing it)."
- "Proper hotkey support... have the Ok button be "&Ok" in the software and have it respond to
  the keypress."
- "Tab/Shift-Tab support in the desktop/panel."

**The editor looks like the label:**

- **`LineEdit` gains a look:**
  - `setFramed(false)`: no bevel.
  - `setColors(background, text)`: palette roles; a field's white and text by default.
  - `setCentered(true)`: each line centred.
  - `setWrapping(true)`: lines broken as `TextWrap` breaks them, on the width's columns; Up and
    Down move between lines. Wrapped text doesn't scroll; the edit asks to grow instead
    (`setLinesChanged()`).
  - `setMaxLength(bytes)`: typing stops at the limit, as Windows' rename does. The panel's
    message for too long a name stays for a paste.
- **`PopupEdit::open()` takes a `Look`:**
  - `Field`: as before.
  - `IconName`: a field's white and text (the user's, after trying the desktop's colors),
    centred, wrapped, no frame. The window is as tall as the
    lines, and grows and shrinks as they change (8 lines at most).
- **The desktop:**
  - Labels break with `TextWrap` too (9 columns), so a name wraps the same in both.
  - `DesktopWindow::setEditing(item)` hides that item's label while the editor is over it.
  - The editor sits exactly where the label's lines are: the cell's left, its width, the
    first line's top.
- **The feedback:** the cursor, and the selection in a selected label's colors. The rest is the
  label's look.
- **The selection when editing starts:** the whole name. For a file with an extension
  (`filetypes.md`), only the part before it, as file managers do.

**Mnemonics:**

- **`Mnemonic` (bmds_ui, pure, host-tested):**
  - `&` marks the next character: "&Ok" shows "Ok" with the O underlined, and its key is `o`.
  - `&&` is a literal `&`; a `&` at the end is dropped.
  - `strip(text, out)` gives the shown text, the underlined character's byte offset and the
    key, lowercased (ASCII letters and digits; other characters by codepoint).
  - `matches(key, event)` takes the event's character, or with Alt the letter or digit of its
    usage (the keymap may give no character with Alt).
- **Drawn:** a line under the marked character, on the font's last row, in the text's color.
  Always drawn: no hiding until Alt is pressed.
- **Who has them:**
  - `Button` and `Label` (`setText("&Name")`). A label's mnemonic focuses its buddy
    (`setBuddy()`), as Qt's.
  - `Menu` items.
  - `MenuLayout::Item` gets the underline offset, so the window manager's menus underline too,
    and its `showMenu()` items take `&`.
  - `MessageBox`'s buttons: &OK, &Cancel, &Yes, &No.
- **The keys:**
  - In a window: Alt+key, or the key alone when the focus widget doesn't use it (a button has
    the focus; a line edit takes the letters).
  - One widget with the key: it is activated (a button clicked, a label's buddy focused).
  - Several: the focus goes to the next of them, without activating, as Windows does.
  - In a menu (the toolkit's and the window manager's): the key alone chooses the item, or opens
    its submenu; several with the key highlight the next.
- **The window manager's Alt keys** (Alt+Tab, Alt+F4, Alt+Space, Alt+F10) come first, as now.
  A program's mnemonics can't use Tab, F4, Space or F10.
- **Text that isn't a label:** the panel doubles `&` in window titles and launcher entries' names,
  so "Tom & Jerry" shows as written. The window manager's desktop menu labels (`wm.conf`) take
  `&` as a mark.

**Focus and Tab:**

- **Widgets:**
  - `setFocusable(bool)`: `Button` and `LineEdit` are focusable by default.
  - Tab and Shift+Tab move the window's focus through its visible, enabled, focusable widgets in
    tree order, around the ends, unless the window has `setTabOut()`: then it is told instead
    at either end.
  - A press on a focusable widget gives it the focus.
  - `GuiWindow::setFocusWidget()` redraws the old and the new focus.
- **Active windows:**
  - `WindowConfig::activated` (new) tells a window when it gains or loses the keys.
  - `GuiWindow::isActive()`; the focus widget is redrawn on a change.
  - A focused `Button` draws a ring in the highlight's color in place of its bevel's inner line
    while its window is active. A rectangle in the text's color drawn over the face crossed the
    text and made it hard to read (the user's, 2026-10-07); the ring is outside the text's rows
    and another color.
  - Space and Enter click a focused button.
- **The default button:**
  - `Button::setDefault()`: a dark outline.
  - Enter in a window whose focus widget doesn't use it clicks the default button.
  - `MessageBox`'s first button is the default and has the focus; Left and Right move between
    its buttons too.
- **Keys for the desktop and the panel (window manager):**
  - `focusable` now counts for desktop- and panel-level windows. They never join Alt+Tab or the
    window list.
  - `WindowConfig::focusOnPress` (default true) says whether a press focuses one. The desktop's
    does. The panel's doesn't: a press on a window's button would take the focus from the window
    that the button minimizes.
  - `raise(id)` still focuses any focusable window.
  - When no application window is left, the focus falls to a focusable desktop window, so keys
    reach the desktop after the last window closes.
  - Alt+Space opens the window menu only for an application window, not for the desktop.
- **The desktop's keys:**
  - The arrows move the selection on the grid, to the nearest icon that way. Home and End go to
    the first and last.
  - A letter selects the next icon whose name starts with it.
  - Enter opens; F2 renames; Delete moves to the trash.
  - The Menu key or Shift+F10 opens the icon's menu, or the desktop's without a selection.
  - Ctrl+C copies; Ctrl+V pastes.
  - Tab goes to the panel's first item, Shift+Tab to its last.
  - The selected icon's name is filled with the highlight while the desktop is active, and only
    outlined in it while not.
- **The panel's keys:**
  - Tab and Shift+Tab move through the launcher and the window buttons; past either end, they go
    to the desktop. Left and Right do the same as Tab and Shift+Tab.
  - Space or Enter presses: the launcher opens its menu with the first item highlighted; a
    window's button raises the window.
  - The Menu key or Shift+F10 on a window's button opens the window's menu.
  - Esc goes back to the desktop.
- **As built** (2026-10-07):
  - **Pure** (host-tested in `tests/host/gui.cpp`): `Mnemonic` (marks, `&&`, a trailing `&`,
    the first mark, multi-byte characters, cutting on a character; keys with and without Alt,
    Ctrl and releases giving none) and `LineEditModel::setMaxBytes()`.
  - **The editor:**
    - `LineEdit` lays its lines out as `TextWrap` does and keeps the cursor on the line it
      belongs to (a space dropped at a break counts as the line before's).
    - Unframed, a line selected to an end is lit 4 pixels past it, as a selected label's
      highlight is, so a name all selected looks the same in the editor and the label.
    - The desktop's labels use `TextWrap` (9 columns) too, so the editor's lines break where
      the label's did.
    - `PopupEdit` counts the lines at its width before it opens, so it opens at its full
      height, and resizes its window with `WindowManager::resize()` as they change.
  - **Mnemonics:**
    - `MenuLayout::Item::underline` carries the offset, so `drawMenu()` underlines for both the
      toolkit's menus and the window manager's.
    - The window menu: &Restore, Mi&nimize, Ma&ximize, Roll &Up/&Down, Always on &Top, &Close.
      The terminal's: &Copy, &Paste, Select &All. The desktop's: &Applications, &Paste. An
      icon's: &Open, &Copy, Re&name, &Delete.
  - **Focus:** `GuiWindow` handles keys in one place (`key()`) in the order given above. The
    window manager tells `activated` from `changed()`, where it already redraws the frames of
    the windows losing and gaining the focus.
  - **On the board:**
    - Down selected the first icon and F2 opened the editor in the label's place, the name
      selected with the label's padding.
    - A three-line name gave a three-line editor; typing "x" shrank it to one line.
    - Renaming to a taken name brought the message box: &OK underlined, outlined as the
      default, with the focus rectangle. The O key alone answered it, and editing went on.
    - Tab from the desktop focused the panel's launcher (the desktop's selection then only
      outlined); Enter opened the launcher with Terminal highlighted, Enter started it.
    - Tab moved to the Terminal's button and Space raised the window.
    - Alt+Space opened the window menu with its marks underlined; X maximized and R
      restored.
    - Closing the terminal left the keys with the desktop (`win list`: "Desktop (focus)").
    - `win check` found 0 wrong pixels, the selection's outline included.
  - **Size:** this section and `filetypes.md` together grew the image 15,680 bytes to 867,136.
  - **Alt stuck after a mnemonic (the user's report, 2026-10-07):** the title-bar buttons
    stopped working until something was typed. The keyboard decoder reports a modifier's own
    release with the modifiers as they were, and the window manager kept those as the ones
    down. So after Alt+O, every press was an Alt+drag (§4g) until the next key. The window
    manager now takes a modifier key's own bit off at its release, and puts it on at its
    press. On the board: an Alt tap, then a click on a terminal's close button, closed it
    (before the fix, nothing happened). An Alt+drag with Alt held still moved the window.
  - **The editor on white (the user's, 2026-10-07):** drawn in the desktop's colors, the
    editor couldn't be told from the label. It is now on a field's white with a field's text
    (still no frame, centred and wrapped as the label).
  - **A long name's editor showing the old label (the user's report, 2026-10-07):** to count
    its lines, `PopupEdit::open()` makes the editor a line tall before the window opens. Opening
    puts it back to the window's size only when the window's size changes. Opened again at the
    same size (the same name after a message, say), the editor stayed a line tall, and the rest
    of the window, never drawn, showed what was under it: the elided label. `open()` now fits
    the editor to the window after showing it. A desktop reload while editing (the icons'
    retries in the panel's first minute) keeps the name hidden too. On the board, a long hold
    on "long name goes here this is long.txt" opened the white editor with the whole name on
    four lines, "long name goes here this is long" selected, covering the icon below. After
    Esc, a second long hold and F2 each opened it whole again.
  - **A window number reused:** a panel started again got the old panel's number, still the
    window manager's focus, and so had the keys without being given them. Opening a desktop or
    panel window under the focus's number now clears it.

## 5. Budget (estimates)

- **Firmware:** the toolkit 10 to 15 KB, the panel 5 KB, the window manager's additions 4 KB, the
  defaults' table 2 KB.
- **Memory while the panel runs:** its process (a task's stack, 8 KB), widgets and entries in
  PSRAM (a few KB: an entry is about 400 bytes, a widget under 100), the event ring (64 × 12
  bytes); the window manager's `.bss` grows by a byte or two a window and a watcher a connection.
- **Drawing:** the panel is 1280 × 28 pixels: redrawn whole in under a millisecond (measured
  since: a button about 2 ms with its bevels, §9); the clock's
  cells each minute; the desktop window costs what the desktop fill costs now.
- **Boot:** reading `/boot/.defaults` (under a millisecond); writes only when a release adds a
  default (0.35 s each, once).

## 6. ABI impact

None frozen yet: the GUI API is internal (`windows.md` §6). Interfaces that become frozen:
`terminal -e`, `panel` and its options, the configuration files' keys (append only, as the
palette's role names), `/boot/.defaults`' format (a file name a line).

## 7. Steps

1. **Pure parts** (host-tested; done): `IniReader`, `DesktopEntry` (with `Exec` splitting),
   `MenuLayout`, `LineLayout` (`BoxLayout`'s arithmetic).
2. **The window manager** (done): levels, mouse events, popups, focusable, reserved strips,
   `watch()`; `WindowStack` levels host-tested; `win pointer` drives the real path.
3. **The toolkit** (done): `GuiApplication`, `Widget`, `GuiWindow`, `Label`, `Button`, `BoxLayout`,
   `Menu`, timers; the palette roles.
4. **The window menu** by the window manager, with `drawMenu()` (done).
5. **The defaults on `/boot`** (G14) and `terminal -e` (done).
6. **`/bin/panel`** (done): the panel and desktop windows, the three items and the spacer, the
   desktop menu. Starting it from the card's `autoexec.sh` (`panel &` after `win start`) is the
   user's to add: the card's script isn't in the repository.
7. **On the board** (done, §9).

## 8. Open questions

- Icons' format and sizes (G17): PNG needs an inflate (the screenshot code only deflates).
- A tray's protocol: how another program puts an item on the panel (a window the panel embeds,
  as XEmbed, or a small message protocol through the window manager).
- Keyboard: a key for the launcher's menu (the Windows key) and Alt-Tab, once the window manager
  has global shortcuts.

## 9. As built (2026-10-06)

- **Host-tested:** `IniReader` (groups, entries, comments, invalid lines and their numbers, CR
  LF, lists with `\;`, escapes, booleans); `DesktopEntry` (the keys used, localized and unknown
  ones ignored, a `[Desktop Action]` group not read, what isn't an entry, `Type=Link` refused, a
  name too long refused, `Hidden=true` alone); `Exec` splitting (spaces and tabs, quotes and their
  escapes, field codes dropped alone or inside an argument, `%%`, an empty quoted argument, an
  unclosed quote, buffers too small); `MenuLayout` (rows touching, widths on 4-pixel columns with
  the longest text, shortcut and arrow, hits on every row's edges, keys skipping disabled items
  and separators around the ends, placing at a point, under a button, off each edge, too tall,
  submenus right and left); `LineLayout` (the panel's row, equal shares up to a widest, stretch
  factors in proportion, too little room, every length from 0 to 300 inside the line);
  `WindowStack` levels (the order kept through opens, raises, hides and shows; a raise under a
  panel damaging nothing the panel covers; the random damage test with random levels); the two
  icons and the six roles.
- **On the board, by the pointer (`win pointer`) and screenshots:**
  - The window menu opens on the menu button's press; a release on a disabled item does nothing;
    press, drag and release on Minimize minimizes; two clicks on Maximize maximize; a press off the
    menu closes it and goes no further (the focus stays); Escape closes it.
  - The panel: Apps, a button per window (checked with the focus, `[title]` when minimized),
    the clock at the right. A click on a window's button raises it, again minimizes it; a right
    click opens its window menu (Maximize from it restored and maximized a minimized window under
    the panel: 0, 24, 1280 × 696). `win maximize` fills the same work area.
  - The launcher: press Apps, drag onto Terminal, release: a new terminal. The desktop's right
    click opens the same menu at the pointer. An entry in `/sd0/p0/share/applications` shows next
    to `/boot`'s, sorted; `Text Editor` (`vi`, `Terminal=true`) opened a terminal running `vi`,
    and `:q` closed it; an entry whose command doesn't exist opened a terminal saying
    `terminal: nosuchprogram: Does not exist` until it was closed.
  - `win check`: 0 wrong pixels with the panel and its desktop (the desktop window draws a
    reference; the panel's strip isn't checked: 30,720 pixels), maximized and restored windows,
    after `win color desktop` (a desktop color change now repaints desktop windows: the first run
    showed the old color) and `win color face`.
  - A second `panel` says one is running; `kill` ends it (`Terminated`: the loop waits on the
    program's interrupt; the first build waited on a semaphore alone and ignored signals);
    `win stop` ends it (`Done`).
- **Bugs found on the board:** `win pointer` timed out now and then: a pointer event wasn't
  counted as a change, so `sync()` could take a count from after the round that applied it and
  wait for a round nobody woke (now counted when queued, as keys are). The window menu's item
  array was one short (the separator), cutting Close off.
- **Memory:** the panel running costs 28,892 bytes of PSRAM (its stack, the application with its
  event ring, widgets, entries kept in as little as they need, the menu) and 372 of internal RAM;
  72 KB of PSRAM at most while it starts (the configuration's text and a table of 48 entries,
  freed). Five start-and-kill cycles: internal RAM back exactly, PSRAM within 12 bytes (232 kept
  after the first run, once).
- **Boot:** the first boot with the new firmware installed `config/panel.conf` and
  `applications/terminal.desktop` and listed all four defaults in `/boot/.defaults` (the `boot`
  stage 1.0 s, once); later boots read the list (the stage 4.3 ms, 3.9 before; 376 ms since
  reset).
- **Size:** the image is 760,614 bytes (730,740 before: +29,874): `bmds_gui` 10,144, `bmds_panel`
  7,229, the rest the window manager's additions, the menu drawing, the INI and Desktop Entry
  readers, the defaults' table and `terminal -e`. `.bss` +592 (29,876).
- **Not yet:** icons (G17), the tray (G13's protocol), reloading the configuration, a calendar on
  the clock; `win key` can't send arrow keys (it types bytes), so menus' arrow keys were checked
  only by reading the code.
- **The 3D look and cascading** (§4e, §4f; 2026-10-06):
  - Host-tested: every bevel style on every rectangle up to 9 × 9 covers its ring(s) exactly once
    and nothing inside, the shaded side owns the top-right and bottom-left corners, raised and
    sunken are each other's reverse; `cascadeRect()` places down the diagonal, reuses a place
    freed, starts the next diagonal two steps right, keeps every place inside the work area and on
    a 4-pixel column, and gives the first place when all are taken or the window is too big.
  - On the board (screenshots): two `terminal &` opened at 16, 44 and 40, 68 under the panel; the
    panel's raised buttons, the focused window's sunk and lighter, the clock sunk in the
    bar, Apps pressed in while its menu is open, the menu's bevel and navy highlight, title-bar
    buttons raised and sunk while held (moved off and released: not minimized). `win check`: 0
    wrong pixels, title-bar bevels included.
  - Cost: a window's title changed redraws its title bar and its panel button: two rounds, 30 DMA
    calls, 5.9 ms of drawing (3.6 to 4.1 ms without the panel). A bevel is up to four fills,
    each with read-modify-write edges off the 4-pixel columns: the lever if it matters (one
    patch per edge group). The first build redrew the whole panel at every window-list change
    (`BoxLayout::relayout()` asked for all of itself): children that move ask for their places,
    so it no longer does.
  - Size: +2,698 bytes (763,312); `.bss` +16.
- **Window management** (§4g; 2026-10-06), on the board with `win hid`:
  - Alt+Tab, Tab, release: the third window raised; Shift+Alt+Tab, Escape: nothing changed;
    with an always-on-top window and a minimized one, Alt+Tab flips between the last two windows
    and Shift+Alt+Tab reaches the minimized one, restored. The box in the screen's middle
    (screenshot). Twenty openings: PSRAM unchanged.
  - Alt+Space opened the window menu under the title bar; Down three times skipped the disabled
    Restore and the separator to "Always on Top", Return set it. Alt+F4 closed the focused window.
    Alt and a press in a client area, moved 40, 30: the window moved by that.
  - `win raise` of a terminal under an always-on-top window: the terminal has the focus and
    takes typing (the first build left the focus on the window above: focus was the top
    focusable window). `win check`: 0 wrong pixels throughout.
  - Host-tested: `setLevel()` (up to `Above`, a raise under it, back down on top of the normal
    windows, a hidden window shown on top of its new level).
- **Icons, the Windows key, snapping, rolling up** (§4h-§4k; 2026-10-06):
  - Host-tested: `Png` on 12 images written by `tests/host/png/make.py` (every kind and depth,
    each row a different filter, stored, fixed and dynamic blocks, IDAT split, a colour key) and
    two real icons, each against its pixels (Python's own encoding, ImageMagick's decoding of the
    icons): all equal; a bad CRC, a cut file, interlacing and 16 bits refused; `Inflate`'s
    checks; CRC-32 and Adler-32 against their check values. `findIcon()` on `hello.elf` (16, 32,
    24 → 32, 48 → 32, 8 → 16; the bytes are the PNGs it was built with, and they decode); `cat`
    has none; a text file isn't ELF. The SDK test (`bmds-elfcheck`) still passes `hello.elf` with
    its notes.
  - `tools/render-icons`: 658 PNGs (2.6 MB on disk) in 21 s, the same bytes run to run.
    `bmds-dist build icons`: 706 files and the alias links; installed on the card in 49 s with
    BusyBox (now with `vi.desktop`) and the time zones.
  - On the board (screenshots): icons on the launcher button, the menu's entries, the window
    buttons, the title bars and Alt+Tab's rows; `win check` 0 wrong with icons on title bars and
    with a rolled-up window. The Windows key: a tap opens the launcher at its button, Escape
    closes it, Win+E opens nothing, a tap of either key closes it. Snapping: left half (0, 28,
    640 × 692), dragged away its size back under the pointer (808 × 504), the top maximized under
    the panel with the outline shown there while dragging, the right half, `restore` back each
    time. Rolling up by the wheel (808 × 24) and down, by the window menu from the keyboard; a
    rolled-up terminal still took typing.
  - Found on the board: the user's `/boot/autoexec.sh` starts the panel before the card mounts,
    so its entries and the card's icons weren't there yet. Now `IconTheme` keeps no misses, and
    the panel reads its entries again when the launcher opens if a directory was missing.
  - Memory: the panel running, 41.7 KB of PSRAM with icons and a table of 64 slots (28.9
    before); with the cache an allocation an icon, 34.5 KB. Start-and-kill cycles (with and
    without the launcher opened, one at a time and in a shell loop): unchanged.
  - The drop seen after a boot (about 40 KB once, then steady), tracked down with `mem mark`
    and `mem new` (new: the heap's blocks in use now that weren't at the mark, from IDF's heap
    walk): 33,464 bytes were the shell's idle pipeline worker (a `pipe` thread: a 32 KB stack
    in PSRAM, 496 bytes internal, a 200-byte record), which the first pipeline after boot
    (`mem | grep Psram` in the test loop) starts and the shell's `WorkerPool` keeps for the
    next one (`maxIdle` 2: a three-stage pipeline leaves a second; no more). The user chose to
    free idle workers after a few seconds: `WorkerPool::Config::idleTimeoutMs` (the shell's
    5 s; `Thread::detach()` lets a worker end itself, the timer task freeing its stack). Host-
    tested (a worker gone after its timeout with its memory; 300 rounds of jobs started around
    the timeouts, each run once), and under ThreadSanitizer, which caught a real race on the
    way: the pool's destructor could destroy its lock while a worker that had just left was
    still unlocking it (now counted and waited for). On the board, ten pipelines and 6 quiet
    seconds left none of their 67 KB. The
    rest is the shell's own (command-line buffers, the job table grown once from 288 to 432
    bytes, a 992-byte copy of the environment). A panel run under a mark leaves 0 new blocks.
    A further 6,144 bytes seen once in an earlier session didn't come back in three replays
    of that sequence with the mark set.
  - Size: the image 781,960 bytes (765,482 before: +16,478: the decoder, the icons built into the
    terminal and the panel, the theme lookup, the key grabs, snapping and rolling up); `.bss`
    +224.
- **The terminal's own colors** (§4l, 2026-10-06):
  - Host-tested: `TerminalColors` (each name in any case, `Color0` to `Color15` and nothing
    past them, `RRGGBB` and `#RRGGBB`, a bad value leaving the color as it was), and the view's
    cells in a set's colors (an ANSI color set, the 256 palette's others fixed).
  - On the board: the default `terminal.conf` installed on `/boot`; `win color` lists 22 roles,
    then the three terminal names (`-` unset). A file with `Background=102030`, `Color1=ff8800`
    and two bad lines: the terminal reported lines 4 and 5 and drew the file's colors (red
    printed orange); `win color terminal-text ffff00` turned the open terminal's text yellow,
    `win check` 0 wrong; a terminal opened after the file was restored drew its black with the
    yellow text. Three open-and-close cycles: internal RAM the same to the byte.
  - Size: the image 788,624 bytes.
- **Desktop icons** (§4m, 2026-10-06):
  - The panel's desktop window moved to its own file (`desktopwindow.cpp`, a view), and reading
    Desktop Entry directories to `panelentries.cpp`, shared by the launcher and the desktop.
  - On the board, after a reset: `/boot/desktop/terminal.desktop` installed, the Terminal icon
    (32 × 32, the theme's) and its name at the work area's corner, `win check` 0 wrong; a click
    selected it (its name in the highlight colors), `win check` 0 wrong again; a double click
    opened a terminal. Killing and starting the panel twice: free internal and PSRAM the same to
    the byte after each.
  - Size: the image 792,736 bytes (788,624 before).
- **Moving desktop icons** (§4m, 2026-10-06):
  - Host-tested: `IconGrid` (wanted cells kept, taken or off-grid ones placed as the rest, down
    then across; more items than cells; the cell under a point, the nearest outside the grid).
  - On the board, with a second entry (`Text Editor Test`) added for the test: the Terminal
    dragged from (0, 0) to (2, 1) by `win pointer`; `win check` 0 wrong after the drop, and
    `desktop.conf` written (`terminal.desktop=2,1`, `zz-test.desktop=0,1`). The second dropped
    on the Terminal's cell took it and the Terminal took the second's (`0,1` and `2,1`). The
    panel stopped and started again: both drawn where they were dropped, `win check` 0 wrong.
    A `win check` in the middle of a drag reported 236 pixels: the panel hadn't drawn the
    latest motion yet when it compared (its reference is the panel's state, ahead of the
    screen); after the drop it was 0. The test entry and `desktop.conf` were removed after.
  - Size: the image 795,200 bytes (792,736 before).
- **Dragging made smooth** (the user's report, 2026-10-06: dragging the terminal's icon lagged,
  the pointer leaving a trail; and the test entry's second line, "Editor Te", looked wrong):
  - The cause: the toolkit gave its windows' damage to the window manager with the
    application's lock held, so it waited for the window manager's lock through a round, whose
    redraw of its window then found the application busy (`tryLock`) and asked `update()`: the
    whole window again at the next round, for the desktop the whole screen (20 to 45 ms), and the
    pointer's old place was left drawn until then.
  - The fixes: `GuiApplication` takes each window's damage under its lock and gives it to the
    window manager after (`GuiWindow::takeDirty()`); a redraw that finds its application busy
    calls `Painter::defer()`, and the window manager asks for the same damage again at the next
    round, no more. `win stats` counts them ("Deferred:").
  - Measured, 60 steps of a scripted drag (`win pointer` in a shell loop): before, 309 rounds of
    19.5 ms on average; after, 103 rounds of 0.4 ms (43 redraws put off, each a frame). A
    screenshot in the middle of a drag showed no trail; `win check` 0 wrong after the drop.
  - A cut second line ends in an ellipsis: "Text Editor Test" is "Text" and "Editor T…".
  - Size: the image 795,696 bytes.
- **Alt+Tab with 32-pixel icons** (§4n, 2026-10-06): `SwitcherLayout` host-tested (one window
  at the least width, ten in two rows of eight, every icon on a 4-pixel column, the cell under a
  point, a narrow screen). On the board: two terminals and a test window; Alt+Tab drew the box
  with the terminals' own 32-pixel icons, the chosen cell lit and its title in the field, and the
  test window (no icon) as a small drawn window; Tab moved, Alt's release raised the choice, a
  click on a cell chose it; `win check` 0 wrong with the box open. The list keeps no copies.
- **The window manager's desktop menu** (§4o, 2026-10-06): with the panel stopped, a right press
  on the bare desktop opened "Terminal" at the pointer, and choosing it started `/bin/terminal`
  (pid 3, a session of its own, its window cascaded); with the panel started again, the same
  press opened the panel's menu. The first try crashed: `Thread::detach()` is the thread's own
  call (an assert), so the launch thread now detaches itself, under a lock that keeps one launch
  at a time. Size: the image 801,296 bytes (797,936 before both).
- **Window keys, Ctrl+Shift+Windows, minimizing** (§4p, 2026-10-06), driven by `win hid` on the
  board: Win+Up maximized a window (to the work area under the panel), Win+Down restored it,
  Win+Left snapped it to the left half and again restored it, Alt+F10 maximized it; no launcher
  opened from those Windows presses. With the panel, Win+Down minimized it; the panel killed,
  it showed again, its minimize button drawn disabled, `win minimize` refused (`InvalidState`),
  `win check` 0 wrong over the whole screen. Ctrl+Shift+Windows (Ctrl, Shift, then Windows; and
  Windows, Ctrl, then Shift with the panel running) started a terminal, and the panel's launcher
  didn't open; a plain Windows tap still opened it. Size: the image 802,672 bytes.
