# File Browser: Folder Windows, a Folder Tree, One View for the Desktop Too

Status: **design** (2026-10-07, the user's request: "building the GUI file browser now ... the
typical folder per window view, and folder view on the left ... build out our GUI UI library for
this, but this also gives us a real use for drag and drop ... unify this with our desktop ...
only want a single file browser process (even if there are multiple windows) ... generally a
windows 95 like interface ... complete, easy to use, is efficient in both memory and cpu, and fits
our overall aesthetic").

## 1. Goal

- **Folder windows:** each shows one folder and navigates in place (Back, Forward, Up, an address
  bar). A folder can also open in a window of its own.
- **A folder tree** on the left of a folder window: the filesystems, the home, the desktop, their
  folders.
- **The desktop is a folder view:** the same items, menus, keys and operations as in a window.
- **One process:** the panel's. The desktop, the taskbar and every folder window share one icon
  cache, one file-type lookup and one copy of the code.
- **Drag and drop** between views, the tree and the desktop, and onto other programs (a terminal
  pastes the paths).
- **Efficient:** no polling (the VFS tells views of changes), long operations off the event loop,
  memory in proportion to what is shown.
- **The look:** Windows 95's file manager in the toolkit's classic 3D style.

## 2. Facts that shape the design (checked 2026-10-07)

- **The panel** (`/bin/panel`, `bmds_panel`) is the desktop and the taskbar in one process, on the
  toolkit: a `DesktopWindow` (a `GuiWindow` at the desktop level) and a bar.
  - The desktop shows the files of `/home/Desktop` in an `IconGrid` of at most 64 items, with
    places saved in `/home/.config/desktop.conf`.
  - It has these operations, in `panelapp.cpp`:
    - Open (by `mimeapps.list`), Copy (`text/uri-list`), Paste, Delete to the trash;
    - Rename in place (`PopupEdit`), New ▸ Folder, Text Document, Empty File;
    - keys for its icons (arrows, Enter, F2, Delete, Ctrl+C and V, the Menu key, a letter).
  - The pure parts are already shared: copy names (`CopyName`), `TrashInfo`, `Trash` (bmds_fs),
    `FileFormat` and `Vfs::identify()` (file types), `MimeApps`, `DesktopEntry`.
- **The toolkit** (`bmds_gui`) has `Widget`, `Label`, `Button`, `BoxLayout`, `LineEdit`, `Menu`,
  `MessageBox`, `PopupEdit`, `IconTheme`, `GuiClipboard` and `watchConfig()`.
  - It has no scrolling, no list, tree or column views, no menu bar, toolbar, status bar or
    splitter, and no general dialog.
- **The window manager** holds at most 16 windows on the screen, for every application together
  (`WindowStack::MaxWindows`), and 16 connections. Menus and popups are windows too. The limit is
  fixed arrays, not the algorithms (FB17).
- **Drag and drop** is designed (`clipboard.md` §5), not built:
  - the window manager draws the drag;
  - data goes through the clipboard's `Drag` selection;
  - Copy, Move or Link comes from the keys held.
- **The VFS** tells one hook of changes to a file (`setChangeHook`, under the storage lock). It
  doesn't say which directory changed, and nothing can wait for a directory.
- **Directories can be large** on a card (thousands of entries). Reading one is not instant, and
  telling a file's type from its contents reads 512 bytes of it.

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| FB1 | The process | **The panel's** (the user's, 2026-10-07). Folder windows are the panel's windows, as Windows 95's Explorer windows were `explorer.exe`'s. A crash ends the desktop, the taskbar and the folders together; the panel restarts as one program | a `files` process (a second icon cache and copy of the code); a process per window |
| FB2 | Navigation | **In place** (the user's): Back, Forward (Alt+Left and Right), Up (Backspace), an address bar. **Open in New Window** is on a folder's menu and is Shift+double-click | spatial, a window per folder (Windows 95's My Computer) |
| FB3 | Views | **Large Icons and Details first** (the user's); **more planned:** a view is a widget on the folder model's interface, so Small Icons, List and Thumbnails are new widgets, not changes. View ▸ picks one per window, and the choice is remembered per folder (FB16) | every view now |
| FB4 | The folder tree | **On the left of a folder window, shown by default, View ▸ Folders hides it.** Its roots are Desktop (`/home/Desktop`), Home (`/home`) and each mounted filesystem (`/boot`, `/sd0/p0`, `/ram0/p0`, by its mount, named as `df` names them). Folders expand on demand (a + box, or the right arrow); only expanded folders are read. It follows the view: navigating selects and opens the path to the folder shown | no tree (Windows 95's My Computer windows); a tree window of its own (Windows 95's Explorer) |
| FB5 | From the command line (the user's: "a command line way to open a browser for a folder") | **`files [PATH...]`** (a built-in program, `/bin/files`): a window for each PATH (the current directory without one). A file's path opens its folder with the file selected. The running panel opens the windows (FB6); without a panel, `files: the desktop isn't running (start 'panel')` and status 1. Its interface is frozen once shipped. The launcher gets a Files entry (`boot/applications/files.desktop`, `Exec=files /home`) | `panel PATH` (the panel's own arguments); starting a browser process when none runs |
| FB6 | How `files` reaches the panel | **Window manager services:** an application registers a name (`GuiApplication::registerService("files", fn, arg)`); `WindowManager::sendToService(name, bytes)` from any task hands a message of up to 1 KB to that application's loop. `files` sends its absolute paths, NUL-separated. Generic: any single-instance program can use it. Not an ABI yet (the window manager's API is internal until GUI programs get one) | a kernel FIFO (`/dev/...`); a sysconf key the panel watches (settings aren't messages) |
| FB7 | Where the code lives | **A new component `bmds_files`** (portable, host-tested):<br>- `FolderModel`: a folder's entries (name, type, size, modified time, attributes, icon name), sorted, hidden files filtered, filled in batches;<br>- `FileOps`: copy, move, trash, delete, rename, new items, paste, conflict names, moved out of `panelapp.cpp`.<br>**The views are toolkit widgets** (`bmds_gui`): `IconView`, `DetailsView`, `TreeView` on an `ItemModel` interface, so other programs (an open dialog, later) use them too | all in `bmds_panel` (nothing else could use it) |
| FB8 | The desktop | **An `IconView` in free-placement mode** (the places saved, as now), on a `FolderModel` of `/home/Desktop`. Its menus and keys are a folder window's, plus Applications ▸ on the background's menu. Its 64-item limit goes (the model grows; the grid scrolls nothing, it is the screen) | keep `DesktopWindow` beside the new views (two of everything) |
| FB9 | Menus (both the desktop's and a window's) | **An item's:** Open, Open With ▸ (`mimeapps.list`'s applications for its type), Open in New Window (folders), Cut, Copy, Delete, Rename, Properties.<br>**The background's:** View ▸ (a window's), Sort By ▸ (Name, Type, Size, Modified), Paste, New ▸ (Folder, Text Document, Empty File), Properties.<br>**A window's menu bar:** File, Edit, View, Go, Help | Windows 95's exact items (Send To, Quick View: nothing to back them) |
| FB10 | Delete | **Delete moves to the trash** (`files.md` F2, as now). **Shift+Delete deletes outright**, after a confirmation that names the items | always outright; always the trash |
| FB11 | Cut | **Cut then Paste moves.** The clipboard offers `text/uri-list` and `x-special/gnome-copied-files` (`cut` or `copy`, then the URIs: what GNOME and others read). A cut item is drawn dimmed until it is pasted or the clipboard changes | a type of our own; no Cut (drag only) |
| FB12 | Large folders | **The model is filled on the loop in batches** (a timer: a few dozen entries a turn, the view drawn as it fills), never by a blocking read of the whole folder. **Types come from names first;** a type by contents (`Vfs::identify()`) is looked up only for items shown, in the same batches. Entries are in PSRAM and grow as needed | a reader thread per window (a stack each); the contents read for every file |
| FB13 | Long operations | **One worker thread for the panel** runs copies, moves across filesystems, deletes of trees and emptying the trash, one job at a time, the others queued. A progress window shows after a second (items, bytes, the file, Cancel). A name already taken asks: Replace, Keep Both (a copy's name), Skip, each "for all", or Cancel. A move within a filesystem and a trash are renames, done at once on the loop | operations on the loop (the desktop freezes); a thread per operation |
| FB14 | Changes from elsewhere | **VFS directory watches:** a view registers its folder and the VFS tells it when an entry is made, removed or renamed in it, or a file in it is closed after writing. Changes coalesce; the view reads the folder again (FB12). A mount or unmount tells every watch. Generic, in `bmds_fs`; the tree and the desktop use it too, and the desktop's timer that looks for icons on a card mounted later goes | polling (forbidden); reading again only on F5 |
| FB15 | Drag and drop | **As `clipboard.md` §5,** built now:<br>- a drop on a folder of the **same filesystem moves**, on another **copies** (Windows 95's rule);<br>- Ctrl copies, Shift moves, Ctrl+Shift links;<br>- a right drag asks on the drop;<br>- targets are views, folder items, tree folders and the desktop;<br>- a terminal takes a drop as its paths, quoted, as if typed;<br>- within the panel, the transfer is skipped (the paths are at hand), but the window manager still draws it | drag within a window only |
| FB16 | What is remembered | **Per folder** (`/home/.config/folders.conf`: a file, as lists are, `sysconf.md` §10): the view, the sort and the tree's width. **Per window:** nothing; a new window takes its folder's. **sysconf:** `files/show-hidden` (bool), `files/default-view` (enum `icons`, `details`) | everything in sysconf (a key per folder) |
| FB17 | The window limit (the user's: "a better algorithm here and not have any limit (or a limit high enough it wouldn't matter)") | **255 windows and 255 connections, storage that grows.** The drawing already scales: the compose plan works top-down and stops once the change is covered, and too fragmented a plan falls back to a rectangle at a time, then to bottom-up drawing. The 16 is only fixed arrays: window records, the stack's rectangles, ids and levels, an application's slots, connections. These become PSRAM lists that grow as numbers are used. Temporaries sized by the cap go: `planCompose()` takes the stack's rectangles and the desktop as a bottom layer instead of a copy, and `WindowManager::list()` becomes `windows()`, a PSRAM list of exactly the windows open, instead of a caller's `Info[MaxWindows]` (about 80 bytes each) on its stack. A snapshot, not a callback under the lock: the panel's window list calls the window manager for each button's icon. Ids stay `uint8_t`, as every call and `win` use them | raise the constant (the callers' stacks grow 16-fold); 16-bit ids (every call changes, for no use on one screen) |

## 4. Design

### 4a. Toolkit additions (`bmds_gui`)

Each widget is drawn in the classic 3D style, works with the keys and has its logic host-tested.

- **Scrolling:**
  - `ScrollBar`: arrows, a thumb sized to the page, track clicks page, auto-repeat on a timer.
  - `ScrollArea`: a viewport with bars as needed. The wheel scrolls it. It is a widget's
    content, not a copy of it, so only what shows is drawn.
- **Item views** (`ItemView`, the base): a model (`ItemModel`: count, text, icon, a column's
  text, sort, change signals), selection (one, Ctrl, Shift ranges, a rubber band), the focused
  item, keys (arrows, Home, End, Page Up and Down, a letter to jump), double-click, renaming in
  place (a `LineEdit` over the item's name), and drag sources and targets (FB15).
  - **`IconView`:** 32-pixel icons with names under them, laid out in rows, or in saved cells
    (FB8).
  - **`DetailsView`:** rows with a 16-pixel icon and columns under a `HeaderView`. Clicking a
    column sorts, and dragging its edge resizes it.
  - **`TreeView`:** indented rows with + and − boxes, children asked of the model when expanded.
- **Window furniture:**
  - `MenuBar` (on `Menu`, with Alt and mnemonics);
  - `ToolBar` (flat buttons with icons and tooltips later);
  - `StatusBar` (sunken fields);
  - `Splitter` (dragged to resize the tree);
  - `ProgressBar`;
  - `Dialog` (a window with buttons, from which the progress and conflict windows are made).

### 4b. `bmds_files`

- **`FolderModel`:** a folder's path, its entries and their sort.
  - `fillSome(n)` reads the next `n` entries (FB12).
  - `refresh()` re-reads after a watch fires, keeping the selection by name.
  - Hidden entries (a leading `.`) are filtered by `files/show-hidden`.
  - Entries come from one PSRAM array, grown by doubling; names are stored once.
- **`FileOps`:** the operations, each a plain function with a result (host-tested on RAM
  disks):
  - `copy(from, to, Progress&)` and `move(...)` (a rename on one filesystem, else copy then
    delete);
  - `trash`, `remove`, `rename`;
  - `makeNew(kind)`, `pasteName(...)` (`CopyName`).
- **`Job`:** what the worker runs (FB13): a list of sources, a target, an operation, and
  progress counters the window reads.

### 4c. The panel

- **`FolderWindow`:** a `GuiWindow` holding:
  - the menu bar and toolbar (Back, Forward, Up, Cut, Copy, Paste, Delete, Properties, Views);
  - the address bar (a `LineEdit`; Enter goes there);
  - a `Splitter` with the `TreeView` and the view;
  - the status bar (the items, those selected and their size, the free space).
- **The desktop** becomes an `IconView` in the desktop window (FB8). Its code shares the
  windows' menus and handlers (`FolderActions`, one per view), so an operation is written once.
- **Services:** the panel registers `files`. A message opens a `FolderWindow` per path.

### 4d. `files` (the command)

- It normalizes each PATH against the current directory and checks that it exists.
- It sends them to the `files` service (FB6), and its status says whether the service took them.
- It is a built-in program on public APIs. A file's path carries the file's name, so the window
  selects it.

### 4e. Step 1b design: the toolkit's furniture (2026-10-07)

What the widgets need that the toolkit lacks, found reading it:

- **A clip inside a window.** `Painter` clips only to the window's visible pieces, and a
  widget is trusted to keep inside its rectangle. A scrolled view's edge rows would draw over
  its bars and whatever is below. `Painter` gains a *clipped painter*: made from another and a
  rectangle, its clip and damage are the other's cut to the rectangle (into storage the caller
  gives: on the stack for up to 32 pieces each, in PSRAM past that), its tools shared, its
  calls and first error folded back when it goes. `text()`, `cells()` and `image()` take
  signed positions: a row half above the window's top is drawn from its visible rows (the
  strip is rendered whole, then copied from its first visible row and column). Fills stay
  unsigned: a view cuts its rectangles to the viewport first.
- **The wheel.** The window manager sends it and `GuiWindow` queues it, but nothing handled
  it. `Widget::wheelTurned(notches)` is offered to the widget under the pointer, then its
  parents, until one uses it.
- **The pointer's shape per widget.** Shapes are a window's, fixed when it opens. The window
  manager gains `setPointer(id, shape)`, and `Widget::pointerAt(x, y)` (Arrow by default) is
  asked as the pointer moves; the window sets the shape when it changes (a splitter's handle
  shows the double arrow).

The widgets (the classic look, as the buttons and menus):

- **`ScrollModel`** (pure, host-tested): a range, a page, a value kept in range, line and
  page steps; the bar's parts along a length (two arrows, the track either side of the thumb,
  the thumb sized to the page and at least 8 pixels), the part at a position, and the value a
  dragged thumb stands for.
- **`ScrollBar`:** 16 pixels across; arrows with ▲▼◀▶; the track in the light face colour;
  a press on an arrow or the track repeats after 400 ms every 50 ms (a GUI timer); the thumb
  is dragged. Told of changes by a callback. Not focusable.
- **`ScrollArea`** (the base of the item views, as Qt's `QAbstractScrollArea`): a content size,
  a viewport, the bars shown as needed, their corner filled. The content is drawn by
  `paintContent()` through a painter clipped to the viewport, at the scroll offset (x in
  4-pixel steps, as text needs). A scroll redraws only the viewport; moving its pixels with
  `Painter::scroll()` is for the views (step 3). The wheel scrolls three lines a notch.
- **`MenuBar`:** titles (with mnemonics) opening `Menu`s below them. Alt and a title's letter
  opens it, as does a press; with one open, the pointer moving onto another title opens that
  one, and Left and Right go round the bar (`Menu` tells its root's owner of a Left or Right
  it doesn't use). F10 opens the first (`GuiWindow::setMenuBar()`).
- **`ToolBar`:** a row of flat buttons (an icon, or text) and separators.
- **`StatusBar`:** sunken fields (`Label`s with the `Status` frame), each a fixed width or
  stretching.
- **`Splitter`:** two children side by side (or one above the other) with a 4-pixel handle
  between, dragged; each side kept at its least; the first side keeps its size when the
  splitter is resized (the tree stays as wide).
- **`ProgressBar`:** a sunken field with blocks in the highlight colour (Windows 95's), the
  value of a maximum.
- **`Dialog`:** a window with a content area and buttons at the bottom right; the first the
  default, Escape and the close box its cancelling one; told the button's result. Centred in
  the work area. `MessageBox` stays as it is (it already has the same behaviour); the progress
  and conflict windows (step 6) are `Dialog`s.

Pure geometry (`ScrollModel`, `MenuBarLayout`, `SplitterLayout`, `ProgressLayout`, a dialog's
`ButtonRow`) is in host-tested files. The widgets are tried on the board in `/bin/widgets`, a
built-in program on the public API showing each of them.

### 4f. Step 2 design: `bmds_files` (2026-10-07)

A portable component (built for the host tests too) over `Vfs` and `Trash`, no GUI:

- **`FolderModel`:**
  - `open(path)` normalizes the path, opens the directory and empties the model; `fillSome(n)`
    reads up to `n` entries (FB12), and `isComplete()` says when the directory is read.
  - Entries are one PSRAM `List` (size, time, type, attributes, and the name as an offset into
    one PSRAM character list: names stored once). The order shown is a third list of indices.
    Each batch is sorted on its own, then merged into the order from the back (no buffer but
    the batch's), so a view drawn while the folder fills never shows it unsorted.
  - Sorting by name, size, type (the extension) or time, up or down, folders first (Windows'),
    names compared without case and with runs of digits as numbers ("File 2" before "File 10").
  - Hidden entries (a leading `.`, or FAT's Hidden attribute) are kept and left out of the order
    unless shown: `setShowHidden()` needs no new read.
  - `indexOf(name)` (a view keeps its selection across a refresh by name), and the counts and
    bytes of what shows (the status bar's).
- **`FileOps`:**
  - The operations: `copy(from, intoDirectory)`, `move(...)` (a rename on one filesystem,
    else a copy then a removal), `remove(path)` (a tree), `trash(path)`, `rename(path, name)`
    (a new name in the same folder, checked), `makeNew(directory, kind)` (F10's names),
    `copyName(directory, name)` (F1's first free copy name), and `measure(paths)` (items and
    bytes, for the progress window).
  - Each takes a `FileOps::Progress`, the caller's object (the worker's job, FB13): told each
    item and the bytes as they go, asked `isCancelled()` between chunks, and asked what to do
    when a name is taken (Replace, Keep Both, Skip or Cancel; a folder's Replace merges). A
    failed item is told and the rest go on. Copies keep the modification time. A file cut off
    by Cancel or an error is removed. A link inside a tree is copied as a link. Nothing is
    copied into itself.
  - Copies use a 32 KB buffer of DMA-capable internal RAM (PSRAM if there is none), one per
    operation, freed after it.
- **Later, not now:** the shell's `cp -r`, `mv` and `rm -r` walk trees their own way, with the
  shell's messages; moving them onto `FileOps` is a separate change.

### 4g. Step 3 design: the item views (2026-10-07)

- **`ItemModel`** (an interface the owner implements): `count()`, `text(row)` (the name),
  `icon(row, size)` (may be null while icons load), `columns()`, `columnTitle(c)`,
  `columnText(row, c, buffer)` (sizes and dates formatted by the model) and `columnAlign(c)`.
  No listeners: the owner changes its model and tells its views (`modelReset()`), which keeps
  the toolkit free of observer lists. `ListItemModel` is a plain one (names and icons) for the
  desktop and the demo; the folder model's adapter comes with the folder windows (step 4).
- **Pure, host-tested:**
  - `ItemSelection`: a bit per row (PSRAM words), the current row and the anchor; select only,
    toggle (Ctrl), a range from the anchor (Shift, adding with Ctrl), all, none.
  - `IconLayout`: flowing cells (rows left to right, as many columns as fit: a folder window),
    or the desktop's cells from `IconGrid`; the cell of a row, the row at a point, the rows a
    rectangle touches (a rubber band), the neighbour an arrow goes to (flowing: the next cell;
    on the grid, the desktop's nearest-that-way rule, moved here).
  - `HeaderLayout`: columns' widths (on 4-pixel columns, a least each), the column at a point,
    the edge within 3 pixels (resizing).
  - `TreeRows`: the tree's visible rows as one flat list (depth, expanded, has-children,
    loaded); expanding inserts children after a row, collapsing removes its descendants (a
    contiguous run), a row's parent, a row's path (names joined by `/` from its root).
- **`ItemView`** (on `ScrollArea`): the model, the selection and the current row; a press
  selects (Ctrl toggles, Shift extends, a press on a selected row keeps a multiple selection
  until its release, for dragging later); a press on no row starts a rubber band; a double
  click or Enter activates; a right press asks for a menu (the row selected first); F2, a slow
  second click on a selected name or a long press asks for a rename; arrows, Home, End and
  Page keys move the current row (Shift extends, Ctrl moves without selecting, Ctrl+Space
  toggles, Ctrl+A selects all); a letter goes to the next row starting with it; other keys are
  offered to the owner first (Delete, Ctrl+C). Subclasses give the geometry.
- **`IconView`:** 32-pixel icons with names in up to two lines (the desktop's look), flowing
  or on the desktop's grid (its items dragged to cells, as now). Colours from roles: a folder
  window's light face, or the desktop's colours.
- **`DetailsView`:** a header (raised buttons, the sort shown by a small triangle; a click asks
  for that sort, its edge is dragged to resize), rows of 18 pixels with a 16-pixel icon and the
  model's columns; the header scrolls across with the rows, not down.
- **`TreeView`:** indented rows with + and − boxes and folder icons; expanding asks the owner for
  the children (`setChildren()`), Left and Right collapse and expand (or go to the parent and
  the first child), as Windows' tree.
- **The desktop on `IconView`:** `DesktopWindow` keeps its API for the panel and holds an
  `IconView` on its grid, with `ListItemModel`; `win check`'s reference picture is the view's.
  The desktop gains the views' multiple selection, Ctrl, Shift and the rubber band; its Delete
  and Copy act on every item selected.
- **`/bin/widgets`** shows a tree, and the list as icons or details (View menu).

## 5. Steps

1. **The window limit, then the toolkit's furniture:** 255 windows and connections in growing
   storage (FB17), with round times measured on the board with many windows open. Then
   `ScrollBar`, `ScrollArea`, `MenuBar`, `ToolBar`, `StatusBar`, `Splitter`, `ProgressBar`,
   `Dialog`, with host tests for their logic.
   - *As built, the limit (2026-10-07):* `WindowStack` keeps its rectangles and its ids with
     levels in two `List`s (reserved together, 16 then doubling, so an open fails whole with
     `NoMem`); the window manager's records and connections, and an application's slots, are
     `List`s grown as numbers are used (never shrunk); the update bits are 256 bits. `win
     new` keeps its test windows in a PSRAM chain reused once closed, not an array by number.
     Host test: 64 to 255 windows, random changes, each damaging what changed. On the board:
     255 windows open (the 256th `Full`), `win check` 0 pixels wrong, no plan fallbacks;
     `win shuffle 100` 28.4 ms a change with 255 windows against 32.6 ms with 16 (the moved
     pixels cost, not the count); internal RAM free the same with 255 windows as with 2.
   - *As built, the furniture (2026-10-07):* as §4e, with `ScrollModel` and the geometry in
     `furniturelayout.h` host-tested (216 checks: values, steps, parts, a dragged thumb staying
     under the pointer at every place). Arrows are drawn as small filled triangles: unscii's
     ▲▼◀▶ fill a 16-pixel button and their rows' background covered its bevel. `Menu` tells a
     `Moved` callback of Left and Right it doesn't use. Found on the way:
     `GuiApplication::exec()` didn't wait when no timer was set (`WaitForever` and "a timer is
     due" were both 0); every program so far always had a timer, `/bin/widgets` doesn't. On
     the board, `/bin/widgets`: the wheel scrolls three rows a notch; F10, Right and Alt+F open
     the menus; a dialog by Enter, answered by Right and Enter; a progress dialog that ends
     itself; the splitter dragged with its pointer shape; an arrow held repeats; the thumb
     dragged; rows half out of a viewport cut at its edges. Size: 16,688 bytes of code (4,170
     of them `/bin/widgets`), no static RAM.
2. **`bmds_files`:** `FolderModel` and `FileOps` (the desktop's operations moved here), host-tested.
   - *As built (2026-10-07):* as §4f. Host tests on two RAM disks (199 checks, then the trash
     through a link): batches sorted at every step, natural name order, hidden entries,
     every sort up and down (ties by name, always up); copies with each answer to a taken name,
     a folder merged, a copy beside itself, nothing into itself; moves by rename and across
     filesystems; measuring, removing, a cancelled copy leaving nothing; renames (case only
     too), new items' names, copy names, times kept. The panel's Paste (now folders too: they
     were refused before), New, Delete and Rename use `FileOps`. Found on the board: the trash
     didn't follow `/home`'s link (`files.md` §10), so the desktop's Delete failed for every
     icon in `/home/Desktop`; fixed in `Trash::move()`. Size: 3,584 bytes of code.
3. **Item views:** `ItemView`, `IconView`, `DetailsView`, `TreeView`. The desktop moved onto
   `IconView`: no change to see but the 64-item limit gone, which proves the unification.
   - *As built (2026-10-07):* as §4g. The pure parts host-tested (142 checks): selections,
     flowing and grid layout, arrows on both, the header's columns and edges, the tree's rows
     (expanding, collapsing, paths, names compacted after many rounds), the plain model, and
     `IconGrid` placing 1000 items. `IconGrid::place()` lost its 64-item arrays (a PSRAM bitmap
     of the cells taken), and the panel reads its desktop entries into an array of exactly the
     size the folders need: the desktop has no limit now. The header is drawn by `DetailsView`
     itself through a clipped painter (`ScrollArea::drawClipped()`, shared with the content),
     not a separate `HeaderView`. Items are drawn through a small canvas interface, so the
     desktop's reference picture for `win check` is the same code as its painting. On the board:
     the desktop's `win check` 0 pixels wrong; a rubber band selecting two icons, both dragged
     two cells and back (`desktop.conf` following); `/bin/widgets` as a small browser of the
     real filesystems (the tree expanded from the VFS, a folder's files as icons or details,
     sorting by a header's click, Ctrl-clicks, double clicks into folders with the tree
     following, 361 files filled in batches, End to the last). Size: `bmds_gui` 40,702 to
     62,334 bytes of code; 24,080 bytes in all with the panel's and the demo's changes; no
     static RAM. Left for step 4: a sort or refresh keeping the selection by name (the views
     reset; the folder windows restore it), the rubber band scrolling the view at its edge.
   - *After the user tried it (2026-10-07):*
     - The tree has one root, `/`, with every filesystem, `/home` and the rest under it (the
       mounts as roots hid the root folder itself). The folder windows' tree is the same.
     - A link is shown as what it leads to (`FolderModel`: `/home` was a 1 KB file); the entry
       keeps a `link` flag for an overlay later.
     - "A residue of the selection's blue beside the text" was the focus mark: a click on empty
       space left the last row current, and its 1-pixel outline, drawn before the text, kept only
       its sides. Now a click on empty space leaves no row current; a selection in a view without
       the focus is a grey fill (the face), not an outline; the focus mark is drawn after the text,
       in the rows above and below it, in the shadow's grey (all four sides show). An icon view's
       outlines are drawn after the names too.
   - *Then (2026-10-07):*
     - `/bin`'s programs showed "0 KB": they are empty nodes by design (run, not read; their code
       is the firmware's). `FolderModel` marks an empty file that `Vfs::builtinProgram()` knows
       (`builtin`), and views show "Built-in program" and no size. `stat` is unchanged (its
       attributes reach the SDK).
     - The outline of a move or resize vanished on white: it is now a light band between two
       dark lines (seen on any colour; inverting would read the screen back and still vanish on
       a middle grey).
     - Dragging desktop icons redrew the whole screen at each motion, and the labels flickered
       (the screen is drawn on as it shows). A drag now redraws the dragged rows where they were
       and where they are, and a selection's change only the rows whose look changed (the view
       remembers the selection as drawn); the drag's offset is kept on 4-pixel columns (text
       must start on one: misaligned, it was refused, 5 errors in `win stats`). On the board: a
       round of a drag 1.6 ms and 5 DMA calls, no errors.
     - The rubber band flickered: each motion redrew its old and new rectangles whole. Now only
       its edges are redrawn (the rows under it are, as their selection changes). That damage is a
       pixel wide, off the 4-pixel columns, and a text's copy clipped by it was refused (labels
       left without their text, `win check` 867 pixels wrong): `WindowManager::invalidate()` now
       widens a client's damage to 4-pixel columns, as the painters' damage must be, whatever a
       client asks. On the board: a band swept over two icons, no errors, `win check` clean.
     - It still flickered, or vanished while it changed size: a race between a window's state and
       its damage (the application changes the band under its lock and gives the damage after;
       a round between drew the new band into the old damage). Putting a window's redraw off
       while it had damage not yet given stopped that, but lagged badly: under a stream of
       motions there nearly always is some. As the user suggested, the band is now the window
       manager's, as a move's outline is: `WindowManager::setBand(id, rect)` shows it over the
       window's client area each round where the window shows (a dark ring and a light one, seen
       on any colour). It first had what it covered redrawn by damage, as the outline does; that
       left trails, because the window under it is the one busy with the motions and draws late.
       Now it is saved and put back as the pointer is: at a round's start what it covered is put
       back (after the pointer's), the windows draw their damage, then what it will cover is saved
       and it is drawn, in pieces `Surface::patch()` takes (a row's bytes of its top and bottom;
       150 rows of its sides). Moving it asks no window to draw. A window's damage is still given
       in one call (`invalidate(id, rects)`), so a round never takes half of it. On the board: a
       screenshot after each of six steps, the rings whole and no trail pixel; a sweep over the
       icons and back, their labels repainted without a ring left on them; `win check` clean.
4. **Folder windows:** `FolderWindow`, navigation, the menus and keys, window manager services,
   `files`, the launcher's entry, `folders.conf`, `files/*` settings.
5. **Directory watches** in the VFS (FB14): views, the tree and the desktop follow changes.
6. **The worker** (FB13): progress, Cancel, conflicts; Cut (FB11); Shift+Delete (FB10).
7. **Drag and drop** (FB15): the window manager, the toolkit, the views, the terminal's drop.
8. **Properties and a Trash window** (the trash of every filesystem in one view, Restore,
   Empty), and a Trash icon on the desktop.

Board checks at each step; each step is usable alone.

## 6. ABI impact

- **Frozen once shipped:**
  - `files`' interface (FB5);
  - the clipboard's types (`text/uri-list`, and `x-special/gnome-copied-files` as GNOME
    defines it);
  - `folders.conf`'s keys and the `files/*` parameters.
- **Not ABI:** window manager services and the toolkit's classes, until GUI programs get an ABI
  (`windows.md` §6). No new system calls.

## 7. Testing

- **Host:**
  - the widgets' logic (scroll ranges, selection, keys, sorting, the tree's expansion);
  - `FolderModel` on RAM disks (batches, refresh, hidden files);
  - `FileOps` (copies, moves across and within filesystems, conflicts, the trash);
  - directory watches;
  - services (a message to a name, an unknown name).
- **Board:**
  - a folder of a few thousand files on the card, opened while the desktop stays responsive;
  - copies to and from `/boot` with progress and Cancel;
  - a drag from a window to the desktop and to a terminal;
  - `files /sd0/p0` from a shell;
  - memory with three windows open, and after they close (nothing left over).

## 8. Open questions

- **Tooltips** for the toolbar: a popup window each time, or text in the status bar (Windows 95
  did both)? The status bar is free; tooltips cost a window.
- **Thumbnails** (a future view) need a decoder per format and a cache; not before PNG and a
  JPEG decoder are worth it.
