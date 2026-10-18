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
| FB4 | The folder tree | **On the left of a folder window, shown by default, View ▸ Folders hides it.** Its roots are Desktop (`/home/Desktop`), Home (`/home`) and each mounted filesystem (`/boot`, `/sd0/p0`, `/tmp`, by its mount, named as `df` names them). Folders expand on demand (a + box, or the right arrow); only expanded folders are read. It follows the view: navigating selects and opens the path to the folder shown | no tree (Windows 95's My Computer windows); a tree window of its own (Windows 95's Explorer) |
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
  - Copies use a buffer of DMA-capable internal RAM (PSRAM if there is none), one per file while
    its bytes move, its size in whole sectors up to 32 KB (one per operation until 2026-10-08).
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

### 4h. Step 4 design: folder windows (2026-10-08)

- **Window manager services (FB6):**
  - `WindowManager::registerService(client, name, fn, arg)` names a connection's service (31
    bytes at most; `Exists` if another has it; released with the connection).
  - `sendToService(name, bytes)`, from any task, queues a message of up to `MaxServiceMessage`
    (1 KB) for it and calls `fn(arg)` under the window manager's lock. `fn` only wakes its loop,
    as a key grab's does. `NotFound` if no one has the name; `Full` past `MaxServicePending`
    (4 KB) not yet taken.
  - `takeServiceMessage(client, name, buffer)` hands the oldest one over (0 bytes: none).
  - The queue is a pure `ServiceQueue` (length-prefixed messages in one PSRAM list),
    host-tested.
  - `GuiApplication::registerService(name, fn, arg)` calls `fn(arg, message)` on the loop, the
    lock held, for each message.
- **`files [PATH...]` (FB5),** a shell command in `bmds_panel` (`/bin/files` too):
  - each PATH is made absolute against the current directory and checked with `stat`;
  - the paths go to the `files` service in one message, NUL-separated;
  - status 0 if the panel took them;
  - status 1 with `files: PATH: not found` for a missing path (the others are still sent);
  - status 1 with `files: the desktop isn't running (start 'panel')` when there's no service;
  - status 2 for usage.
- **Pure, in `bmds_files`, host-tested:**
  - **`NavigationHistory`:** the folders visited and where in them the window is.
    - `visit(path)` drops the forward ones, and a path equal to the current one isn't added
      again.
    - `back()`/`forward()` return the path to go to.
    - At most 32 kept; the oldest goes.
    - `parentOf(path)` gives the parent, `/` being its own.
  - **`FolderSettings`:** `folders.conf` (FB16), an INI file of a section per folder:

    ```
    [/sd0/p0/photos]
    View=details
    Sort=size
    Descending=true
    Tree=200
    ```

    - `parse(text)` reads it and `find(path)` looks a folder up.
    - `set(path, values)` moves it to the end, and the first ones go past `MaxFolders` (256):
      the least recently changed.
    - `write(out)` writes it back. Unknown keys and sections that aren't absolute paths are
      dropped when written.
- **Toolkit:**
  - `ItemView::resetTo(rows, current)`: as `modelReset()`, then those rows selected and
    `current` current, told once. A sort, a batch read or a refresh keeps the selection by name:
    the window takes the selected names before and finds them after.
  - `GuiWindow::setTitle()`: a folder window's title follows its folder.
- **The panel:**
  - **`FileIcons`:** one per panel (in `bmds_gui` since `devplan/mediaplayer.md` step 2, for the
    file dialog too). It copies each type's icon at 16 and 32 once (the theme's
    cache may drop its own), keyed by the type: its icon name, then its generic one, then
    `unknown`. Folders get `folder`, links to folders get it too, built-ins get
    `application-x-executable`.
  - **`FolderItems`** (`FolderItemModel` in `bmds_gui` since `devplan/mediaplayer.md` step 2):
    a folder window's `ItemModel` over a `FolderModel`, with each row's type.
    - Types come from the name (`FileFormat::byName`).
    - A file whose name says nothing is identified by its contents (`Vfs::identify`, 512
      bytes), 4 a turn after the folder is read.
    - Columns: Name, Size (KB, none for folders and built-ins), Type (`Folder`, `Built-in
      program`, else the extension in capitals and ` File` — the Type sort's order), Modified
      (`YYYY-MM-DD HH:MM`, local).
  - **`FolderActions`:** what a folder window and the desktop both do, moved out of `PanelApp`:
    - open a file by its type (`mimeapps.list`), or run a launcher (`.desktop`);
    - copy paths to the clipboard (`text/uri-list` and text);
    - paste a URI list into a folder (`FileOps::copy`, Keep Both);
    - move paths to the trash;
    - make a new item;
    - check and apply a rename (the messages the desktop shows now).

    It tells `PanelApp` to open a folder window (`openFolder(path, select)`), so a folder on
    the desktop opens in a window.
  - **`FolderWindow`:** 640 × 440, cascaded by 24 pixels from the last one opened.
    - **Menu bar:**
      - File: Open, Open in New Window, New ▸, Delete, Rename, Close.
      - Edit: Copy, Paste, Select All, Invert Selection.
      - View: Large Icons, Details, Arrange Icons ▸ (Name, Type, Size, Modified), Folders,
        Status Bar, Refresh.
      - Go: Back, Forward, Up One Level, Home, Desktop.
      - Help: About.
    - **Toolbar:** Back, Forward, Up | Copy, Paste, Delete | Large Icons, Details.
    - **The address bar:** a `LineEdit`. Enter goes to a folder, or opens a file's folder
      with the file selected. A path that isn't there gets a message.
    - **The splitter:** the tree (one root `/`, following the folder shown: its path expanded
      and selected), then the view.
    - **The status bar:**
      - the items (and the hidden ones not shown);
      - what's selected and its size;
      - the free space of the folder's filesystem.
    - **Keys:**
      - Backspace and Alt+Up go up; Alt+Left and Alt+Right go back and forward.
      - F5 refreshes; Ctrl+L, Alt+D and F4 go to the address bar.
      - Delete trashes; Ctrl+C copies and Ctrl+V pastes.
      - Enter opens, Shift+Enter or Shift+double-click opens a folder in a new window.
      - Ctrl+W closes.
      - The views' own keys still work.
    - **Menus:**
      - An item's: Open, Open in New Window (folders), Copy, Delete, Rename.
      - The background's: View ▸, Arrange Icons ▸, Paste, New ▸.
    - **Rename** is in place (`PopupEdit` over the name).
    - **Changes:** after an operation the window reads its folder again; other windows on the
      same folder do too. The VFS watches (step 5) replace this.
  - **Remembered:**
    - A folder's view and sort are written to `folders.conf` when changed in a window (a
      second after, at most once a second). A window opened or navigated to a folder takes
      them, else `files/default-view` and Name.
    - The tree's width is taken when a window opens (it isn't moved under the pointer on
      navigation).
  - **Settings:** `files/show-hidden` (bool, false) and `files/default-view` (enum
    `icons|details`, `icons`) are watched: every window applies them.
  - **The service:** the panel registers `files`. Each path in a message opens a window: a
    folder on itself, a file on its folder with it selected.
- **Launcher:** `boot/applications/files.desktop`, Name=Files, `Exec=files /home`,
  Icon=`system-file-manager` (the theme's `folder` if missing: `forProgram` falls back).
- **Left for later steps:**
  - Cut (6), Shift+Delete (6), Properties (8), drag and drop (7).
  - The tree reads a folder whole when it is expanded (its subfolders' names only). A folder
    of thousands of entries pauses it; the step 5 watches will read it in batches as the
    views do.

### 4i. Step 5 design: directory watches (2026-10-08)

**Facts (checked in `bmds_fs`):**

- **Every change already knows the entry's path.** `remove()`, `rmdir()`, `mkdir()`,
  `symlink()`, `rename()` and `setModified()` work on `storage`: the normalized path with
  `/home` and the links along the way followed. It is the real path of the entry changed.
- **A file's id is FNV-1a over that path**, with ASCII letters folded (`pathId()`). A
  directory's id is its children's prefix. So a folder's id is what `stat()` gives it, and its
  parent's id needs only the path.
- **A file written reports at close.** `FatFile::handleClosed()` and `LfsFile::handleClosed()`
  call `Fs::changedLocked(device, id)` when the file was open for writing. They know the
  file's id, not its folder's. Every file made (`Create`) is open for writing, so its close
  also covers "made". An empty file made and closed is told too.
- **Mounts, unmounts and `setHome()`** call `changedLocked(0, 0)`, "every file".
- **Not reported before this step:** `mkdir()`, `rmdir()` and `symlink()`. Their entries change
  and no file's contents do, so the change hook (the loader's caches) doesn't need them; they
  tell only the watches.
- **`GuiApplication::post()`** can be called from any task under any lock. It is how a
  change, told under the storage lock, reaches a loop.
- **`TreeRows::setChildren()` collapses a row before filling it.** A folder re-read in the
  tree loses everything expanded below it. That is fine on a first expand, but wrong once a
  change re-reads an expanded folder: the path to the folder shown would close.

**Design:**

- **`DirectoryWatch`** (bmds_fs, `bmds/directorywatch.h`): one folder, the caller's object.
  - **Its parts:** `setNotify(fn, arg)`, `watch(path, context)`, `stop()`, `takeChanges()`;
    the destructor stops it.
  - **Storage:** the watches are an intrusive list under the storage lock. The VFS allocates
    nothing and has no limit. A change walks the list: a few dozen compares of 64-bit ids.
  - **`watch()`** resolves the path as `realPath()` does (links and `/home` followed), and
    keeps the folder's id.
    - Not a folder: `NotDir`.
    - Not there: `NotExist`, and the watch stays listed with no folder. It is still told of
      mounts, after which the folder may be there: its owner calls `watch()` again then.
    - Called again, it moves the watch to the new folder. Start a watch before reading the
      folder, so that no change falls between the two.
  - **Changes, as bits** that accumulate until `takeChanges()` returns and clears them:
    - `Entries`: an entry made, removed or renamed in the folder, or the folder itself
      removed or renamed.
    - `Contents`: a file in it closed after writing, or touched (`setModified()`).
    - `Mounts`: a mount, an unmount, or `/home` leading elsewhere. Every watch is told,
      because anything may have changed.
  - **The notify** is called at each change, under the storage lock, from the task that made
    it. It must not call the VFS or block, and it only wakes its owner. The owner coalesces:
    the bits wait for it however many changes come.
  - **What is told where:**

    | Change | Watches told |
    |---|---|
    | `remove()`, `mkdir()`, `symlink()` | the entry's folder |
    | `rmdir()` | the entry's folder; a watch on the folder removed |
    | `rename()` | both folders; a watch on the folder renamed |
    | a file closed after writing, `setModified()` | the file's folder (`Contents`) |
    | mount, unmount, `setHome()` | all (`Mounts`) |

    A file keeps its folder's id from its open, to tell at close.
  - **Not told:**
    - The folders below one renamed or removed: their watches don't know their ancestors.
      Their windows find out at their next read.
    - `/dev` gaining a device: nothing watches it yet.
- **`FolderWatches`** (bmds_files): a process's watched folders, shared. Two windows and a
  tree row on one folder use one watch.
  - **Its parts:**
    - `add(path)` counts a use; `remove(path)` drops one, and the last stops the watch.
    - `take(fn, arg)` calls `fn(path, changes)` once for each folder changed. On `Mounts` it
      first resolves every folder again (`/home` may lead elsewhere now).
    - `forget(path)` drops a folder's news: the owner has already read the folder after its
      own change.
    - One notify for all, given at construction.
  - **Memory:** PSRAM, a record and the path for each folder.
- **The panel:**
  - **Coalescing:** the notify posts to the loop once until the loop takes it. The loop
    starts a 200 ms single-shot timer, then takes every change. A copy of a hundred files
    into a watched folder is one read, not a hundred. The 200 ms is a choice: a delay the eye
    barely sees.
  - **The panel's own changes** (New, Rename, Delete, Paste) still refresh at once, through
    `folderChanged()`. That call also `forget()`s the folder, so the watch doesn't read it a
    second time.
  - **Folder windows:**
    - Each watches its folder, moving the watch on each `go()`.
    - A change re-reads the folder (`refresh()`, the selection kept).
    - While a name is edited in place, the read waits until the edit closes. The rows would
      move under the editor.
  - **The tree:**
    - A window watches each expanded row's folder. `TreeView` tells of any change to its rows
      (`setRowsChanged()`), and the window compares the expanded paths with its own list.
    - An `Entries` change re-reads that row's folders; `Contents` alone is ignored.
    - `TreeRows::setChildren()` keeps the rows below the children that are still there, so a
      re-read keeps the path open.
  - **The desktop:**
    - It watches its directories, and a change reloads it.
    - On `Mounts`, it reloads, and `FileIcons` forgets the icons it didn't find. The 2-second
      retry timer for a card mounted later goes.

### 4j. Step 6 design: the worker, Cut, Shift+Delete (2026-10-08)

**Facts (checked):**

- **Paste runs on the loop.** `FolderActions::pasted()` calls `FileOps::copy()` for each URI,
  with a `Progress` that always keeps both. A large copy stops the desktop, the taskbar and
  every window until it ends.
- **`FileOps` already has the hooks** a worker needs: `isCancelled()` between chunks, `began()`
  and `advanced()`, `failed()`, `conflict()`. A file cut off is removed, and `measure()`
  counts items and bytes.
- **A thread's stack may be in PSRAM** while it writes to `/boot`: flash is written by
  `flash0`'s task, on its own internal stack. The shell's `cp -r` runs on a PSRAM stack.
- **`FileInfo::device`** names a path's filesystem, so whether a move is a rename can be known
  before trying it.
- **The toolkit has `Dialog`** (four buttons at most), `ProgressBar`, and `MessageBox` (Yes and
  No, text wrapped). It has no check box. `GuiClipboard` can't empty a selection it owns, and
  its `setLost()` (another program's copy replaced ours) is unused.
- **Icons are 0xAARRGGBB**, blended over the background as they are drawn. An icon with its
  alpha halved is a dimmed one, on any background.

**Design:**

- **`FileWorker`** (bmds_files, `bmds/fileworker.h`): one thread running jobs, the caller's
  object.
  - **A job:** a kind (`Copy`, `Move`, `Remove`), its paths and a target folder.
    - `add()` queues one and returns its number. Jobs run one at a time, in order.
    - `cancel(job)` drops a queued one, or stops the running one at its next chunk.
  - **The thread** starts with the first job and ends when the queue is empty. Its 32 KB stack
    is PSRAM and is held only while there is work. The loop joins it after it ends, when it
    takes the news, so the join never waits. It runs at priority 1, below the loop.
  - **Counting first:** a job measures its items and bytes before it starts (`measure()`). A
    move counts only the items on another filesystem; the rest are renames.
  - **News for the loop:** a `Notify` called on the thread, which only wakes the loop. The loop
    reads a `Status` (the job, its kind, counting or not, items and bytes done and in all, the
    item, the jobs waiting) and takes each finished job's result (`takeDone()`: the error, the
    failures and the first one's path). Progress and the item are told at most every 250 ms; a
    job begun or done, its counting done, and a question at once.
  - **Questions:** a name taken (`conflict()`) is put in the status as a question, and the
    thread waits for `answer(resolution, forAll)`. Cancelling also answers. With "for all",
    the job's later conflicts take the same answer without asking.
  - **Memory:** jobs and their paths in PSRAM; the copy buffer is `FileOps`' (internal DMA RAM,
    while a file's bytes move).
- **`CopiedFiles`** (bmds_files, `bmds/copiedfiles.h`): the files on the clipboard, pure and
  host-tested.
  - It keeps the paths and whether they were cut. It renders them as `text/uri-list`,
    `x-special/gnome-copied-files` (`copy` or `cut`, then a URI a line: GNOME's format) and
    plain text.
  - `parse(type, data)` reads either list back: the operation, and each `file://` path
    decoded.
  - `isCut(path)` is a binary search over the cut paths, sorted once. A view asks it for each
    row it draws.
- **The panel:**
  - **`FileJobs`** owns the worker and its windows:
    - **Progress:** a `Dialog` titled Copying, Moving or Deleting, opened once a job has run
      for a second. It shows the item's name, "From FOLDER to FOLDER", a `ProgressBar` (bytes,
      or items when there are none), "N of M items, X of Y", the jobs waiting, and Cancel. It
      closes when the queue is empty.
    - **A name taken:** a `Dialog` saying what is there and asking Replace (Merge for a
      folder), Keep Both, Skip or Cancel. A **`CheckBox`** (new in the toolkit: a box, its
      tick, text with a mnemonic, Space toggles) offers "Do the same for the others".
    - **Failures:** a message box once the job is done, naming the first failure and how many
      there were.
  - **Paste** reads `x-special/gnome-copied-files` when it is offered, else `text/uri-list`
    (a copy).
    - A **copy** is a worker job. A name taken asks now: it was a copy's name before.
    - A **cut** is a move. When every item is on the target's filesystem and no name is
      taken, the renames are done at once on the loop, so they never wait behind a long
      copy. Otherwise the move is a worker job.
    - A cut pasted empties the clipboard, if it is still ours, and the marks go.
  - **Cut** (Ctrl+X, Edit ▸ Cut, an item's menu, a desktop icon's menu) puts the paths on the
    clipboard as a cut. The cut items are drawn dimmed until the clipboard changes:
    - in windows, `FolderItemModel::icon()` gives `FileIcons::dimmed()`, a copy of the icon with
      its alpha halved, made once per icon;
    - on the desktop, `DesktopWindow::setItemIcon()` dims its own copy.
  - **Shift+Delete** (in a window and on the desktop) asks first, in a message box naming up to
    three items and how many more: "Delete "a.txt" for good? It won't go to the trash." Yes
    queues a `Remove` job.
  - **Changes** reach the views through the step 5 watches. The worker tells no window.
- **Quitting the panel** cancels the running job (the file cut off is removed) and drops the
  queue.
- **Left for step 8:** emptying the trash, which will be a `Remove` job.

### 4k. Step 8 design: `.bmds`, `/trash`, the Trash window, Properties (2026-10-17)

The user's design (2026-10-17): "Each mounted filesystem should get a .bmds/ folder at the root
of that filesystem ... to track bmds specific metadata, the trash files being one of them, in
.bmds/trash ... a pseudo filesystem called /trash that unifies all the .bmds/trash metadata into a
single filesystem ... The file browser on /trash provides the undelete options." Their choices:
`/trash` is flat, with names made unique where needed; an entry's original place and date come
from a call, not from files; `/trash` is a fixed VFS path; `mv myfile /trash` moves the file into
its own filesystem's `.bmds/trash`; `.bmds` is made when needed, and not on `/rom` or `/bin`.

**Facts (checked):**

- **The trash is `MOUNT/.trash/{files,info}`** (`files.md` F2, F3), in `Trash` (bmds_fs, above
  the VFS: it calls `Vfs::` like any program). `move()` writes the info first, then renames.
  `list()`, `restore()` and `empty()` walk every mount (`listMounts()`, eight at most).
- **The info's `Path=` is absolute** (`/sd0/p0/notes.txt`). A card mounted elsewhere later
  restores to the wrong place. freedesktop.org's rule for a trash at a volume's top is a path
  relative to that top.
- **The VFS already rewrites paths before resolving them:** `homeRewriteLocked()` turns `/home`,
  `/cfg` and `/apps/NAME` into where they lead, for every call. Virtual trees (`/etc`, `/bin`,
  `/dev`) are `Kind`s in `resolveLocked()`, listed by `Directory::next()`.
- **Every change passes two functions:** `Fs::tellEntryLocked()` (an entry made, removed or
  renamed) and `Fs::writtenLocked()` (a file closed after writing), under the storage lock.
  `tellEntryLocked()` returns at once when nothing watches.
- **`rename()` across volumes is `NotSupported`**; BusyBox `mv` then copies and removes.
- **The folder tree's root is `/`**, so a virtual `/trash` shows in it with no change.
- **Icons:** the Flat Color theme has `user-trash` and `user-trash-full`.
- **`FileWorker` has three kinds** (`Copy`, `Move`, `Remove`), one job at a time.

**Decisions:**

| # | Question | Choice | Alternatives |
|---|---|---|---|
| FB18 | Where the system keeps its data on a filesystem | **`MOUNT/.bmds/`** (the user's), one folder for everything BMDS keeps per filesystem: `trash/` now, perhaps `thumbs/` later. **Made when first needed** (the first trash there), so a card plugged in only to be read is never written. None on read-only filesystems (`/rom`) or virtual trees (`/bin`); added later if a need comes. Hidden by its dot. `Trash`'s refusals widen: nothing in `.bmds` can be trashed, nor `.bmds` itself | a folder made at every mount; `.trash` as now |
| FB19 | The trash's layout | **`.bmds/trash/files/NAME` and `.bmds/trash/info/NAME.trashinfo`**, freedesktop.org's as before. **`Path=` is relative to the filesystem's root** (`notes.txt`, `Documents/a.txt`), freedesktop.org's rule for a volume's trash, so a card restores right wherever it is mounted. An absolute `Path=` is still read (the specification allows both). The `.trash` folders already on the card and `/flash` are moved by hand: nothing is released, so no code migrates them | absolute paths (as now: wrong once a card moves) |
| FB20 | `/trash` | **A fixed virtual directory in the VFS** (the user's), one flat listing of every mounted filesystem's trash. An entry stands for its file in `.bmds/trash/files`: the VFS rewrites `/trash/NAME[/REST]` to `MOUNT/.bmds/trash/files/REAL[/REST]`, as it rewrites `/home`, so reading, `stat()`, listing a trashed folder, copying out and watching work unchanged | a mountable filesystem (a volume of volumes, more code); a folder per filesystem |
| FB21 | Names in `/trash` | **Flat, unique where needed** (the user's). An entry's name is its name in its own trash. When two filesystems' trashes hold the same name, the first filesystem by its mount path keeps it and the others get ` (LABEL)` before the extension: `notes (flash).txt`, the label being the mount path without its slashes (`flash`, `sd0p0`, `tmp`). **`Trash::move()` also picks a name free in `/trash`**, so a suffix shows only for a card's trash brought in with a name already there | names prefixed by the filesystem always |
| FB22 | Where it was and when | **A call** (the user's): `Trash::find(name)` gives an entry's original path (its mount joined to `Path=`), its deletion date, its filesystem and its real name. `Trash::list()` gives every entry with the same in one pass. No info files in `/trash` | `/trash/.info/NAME` files |
| FB23 | Changes through `/trash` | **At the top of `/trash`:**<br>- **remove** (`rm`, `rmdir`, `rm -r`) deletes the entry and its info file: `rm -r /trash/*` empties the trash;<br>- **rename out** within the filesystem renames and drops the info; to another filesystem it is `NotSupported`, so `mv` and the worker copy then remove;<br>- **rename in** (`mv myfile /trash`, `mv myfile /trash/NAME`) is `Trash::move()` (the user's): the file goes into its own filesystem's trash, by a rename, named as asked if free;<br>- **making** anything new there (`cp`, `mkdir`, `>`) is `PermissionDenied`.<br>**Below an entry** (inside a trashed folder), everything is as in any folder | a read-only `/trash` (no `rm`, no `mv` in); copies in as trashing |
| FB24 | Restore | **`Trash::restore(name, mode)`** puts an entry back at its original path by a rename (always the same filesystem). Folders on the way that are gone are made again (Windows' rule). A name taken there: `Exists` with `NoReplace`; `Replace`; or `KeepBoth` (a copy's name, `CopyName`). Elsewhere is a rename (or a move) out of `/trash` | refusing when the folder is gone (as now) |
| FB25 | The Trash window | **A folder window on `/trash`** (the tree's Trash, the desktop's, `files /trash`):<br>- Details by default, with columns Name, Original Location, Date Deleted, Size, Type; sorting by each;<br>- an item's menu: **Restore**, Cut, Delete (for good, asking first), Properties;<br>- the background's: **Empty Trash** (asking first), View ▸, Sort By ▸, Properties;<br>- File ▸ Empty Trash; the toolbar's Paste and New disabled, a Restore button;<br>- a drag out **moves** (restores to where it is dropped), whatever the filesystem;<br>- Delete and Shift+Delete both delete for good (a `Remove` job); Empty Trash is a `Remove` job of every entry, with progress and Cancel | a dialog listing the trash (no drag, no views) |
| FB26 | The Trash on the desktop | **A link `Trash` → `/trash` in `/home/Desktop`**, made by the panel when it makes the Desktop folder (first run), never again: the user may delete it and drag `/trash` back from the tree as a link (Ctrl+Shift). Its icon is `user-trash` or `user-trash-full` (`/trash` empty or not, from its listing, followed by a watch), wherever `/trash` is shown: the desktop, the tree, a view. A drop on it, or on `/trash` anywhere, **only moves** (it offers Move; the pointer says no with Ctrl), each item trashed at once on the loop. Its menu adds Empty Trash | a fixed item the desktop draws (special in the desktop's model; can't be removed) |
| FB27 | Properties | **A dialog per selection** (Alt+Enter, an item's menu, the background's for the folder itself, the toolbar's button):<br>- **one item:** its icon and name (editable: a rename at OK), type, location, size (folders: files and folders in it, counted live), modified, and Read-only and Hidden shown; a link's target; a trashed item's original location and deletion date;<br>- **several:** "N items", their types if one, the total size and counts, the location if they share one;<br>- **a filesystem** (its root: a tree root, `/sd0/p0`): its type, capacity, used and free (`statfs`), with a bar.<br>Counting runs on a **second `FileWorker`** of the new kind `Measure`, so it never waits behind a long copy; the dialog shows the totals as they grow and stops counting when it closes. Only OK, Cancel and a rename; attributes are shown, not changed (the VFS has no call to change them) | Windows 95's tabs; editing attributes (a new VFS call first) |

**Design:**

- **`.bmds`** (bmds_fs): `Fs::MetaDirectory` (`.bmds`) and `Trash::Directory` becomes
  `.bmds/trash`. `Trash::move()` makes `.bmds`, `trash`, `files` and `info` as needed.
- **The trash index** (bmds_fs, `trashindex.cpp`, under the storage lock): what `/trash` needs
  to name and find entries.
  - **Per mount:** its trash's entry names (the info folder's, `.trashinfo` taken off), sorted,
    in PSRAM, read straight from the volume. Each mount's list is read the first time `/trash`
    is used after a mount, and again after it is marked stale.
  - **Stale:** `tellEntryLocked()` and `writtenLocked()` mark a mount's list stale when the path
    is in its `.bmds/trash` (before the early return for no watches); an unmount drops it. So
    every change made through the VFS keeps it right; nothing polls.
  - **The flat names (FB21)** come from the lists in mount-path order. Looking one up finds its
    mount and real name. A `/trash` name never in any list is `NotExist`.
  - **Memory:** about the names' bytes plus 8 per entry; nothing while `/trash` isn't used.
- **The VFS:**
  - `resolveLocked()`: `/trash` is `Kind::TrashDir`. The rewrite step turns `/trash/NAME...` into
    the real path (FB20), so the rest is ordinary `Mounted` work.
  - `Directory::next()` for `TrashDir`: each mount's `files` folder in turn, read from its
    volume, entries without an info skipped, each named by its flat name, its id the real
    path's (so watches on a trashed folder and the views' identities agree).
  - `remove()` and `rmdir()` of a top entry also remove its info. `rename()` from a top entry
    drops the info after; to `/trash` or `/trash/NAME` it calls `Trash::move()` without the lock.
    `open()` with `Create`, and `mkdir()`, at the top: `PermissionDenied`.
  - **Watches:** a change in a mount's `.bmds/trash/files` or `info` also tells watches on
    `/trash` (`Entries`); a mount or unmount tells all, as now.
- **`Trash`** (bmds_fs): `Entry` gains `real` (its name in its own trash); `name` is the flat
  name. `find(name, Entry &)` reads one info file. `restore(name, mode, String *restoredTo)`
  (FB24). `move(path, context, name)` writes a relative `Path=` and picks a name free in
  `/trash`. `empty(mount)` stays (orphan info files go with it).
- **`trash`** (the command, F5, not shipped): `-l` lists flat names; `-r NAME...` restores by
  them (`Exists` an error, as now); `-e [MOUNT]` as now. `trash PATH...` is
  `Trash::move()`. Plain `mv PATH /trash` now does the same.
- **The panel:**
  - **`FolderModel` on `/trash`** reads `Trash::list()` once per refresh, in the same batches, and
    joins it by name: the items' original location and date. `FolderItemModel` adds the two
    columns when the folder is `/trash`.
  - **`FolderActions`:** Restore (with the worker's name-taken dialog: Replace, Keep Both, Skip,
    each for all), Empty Trash, deletes for good in `/trash`, a drop's actions on `/trash`
    (FB26), and Paste and New refused there.
  - **`FileIcons`:** `/trash` and links to it get `user-trash` or `user-trash-full`; one watch on
    `/trash` keeps which.
  - **Properties:** a `PropertiesDialog` (bmds_panel) on the toolkit's `Dialog` and form layout,
    and `FileWorker::Kind::Measure` (bmds_files): counts and bytes reported as a copy's are.
- **Size:** about 6 KB of code (the index and VFS cases 2 KB, the panel 4 KB). No static RAM.

**ABI:** no new system calls; programs get the trash through paths. Frozen once shipped: the
layout on disk (`.bmds/trash`, FB19), `/trash`'s rules (FB20, FB21, FB23), and `trash`'s
interface.

**Tests:**

- **Host** (RAM disks and a tmpfs):
  - the index: names from two filesystems, a clash and its label, a name made free by
    `move()`, staleness after changes made directly in `.bmds/trash`, an unmount;
  - `/trash` through the VFS: list, `stat`, read, a trashed folder listed and watched, remove
    with its info, rename out within a filesystem and refused across, `rename` in as a trash,
    creating refused;
  - `Trash`: relative `Path=` written, absolute read, `find`, restore with missing folders and
    with each mode, a card's trash restored after mounting it elsewhere;
  - `FileWorker`'s `Measure`.
- **Board:**
  - Delete in a window and on the desktop, then the Trash window: columns right, Restore puts
    it back, a drag out restores elsewhere, Empty Trash with progress;
  - `mv x /trash`, `ls /trash`, `rm -r /trash/*` in a shell; `/flash` and the card at once with
    a clashing name;
  - the desktop's Trash icon full and empty; a drop on it;
  - Properties of a file, a folder of `share` (counting while a copy runs), several items, a
    filesystem;
  - `win check`, `win stats`, memory before and after.

**Order:** 8a the VFS and `Trash` (host-tested; `trash` and the shell work alone), 8b the Trash
window and the desktop's Trash, 8c Properties.

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
     - Shift on the desktop selected the rows between in the model's order (the names'), not
       the places': from the top-left icon to the one under it took the terminal at the bottom
       too. A view now gives its range rule (`ItemView::inRange()`): the rows' order for flowing
       icons and details, where it is the order shown; on the grid, the rectangle of cells
       between the anchor and the row (`IconLayout::inGridRange()`), for clicks and Shift+arrows
       alike. Host-tested on a full 4 × 3 grid in no order of names; on the board with 23 icons:
       the two cells, then a 2 × 3 rectangle, then Shift+Right to 3 × 3.
4. **Folder windows:** `FolderWindow`, navigation, the menus and keys, window manager services,
   `files`, the launcher's entry, `folders.conf`, `files/*` settings.
   - *As built (2026-10-08):* as §4h, with these differences:
     - **Shared menus:** the folder windows share one set of menus (10 `Menu`s, about 5 KB each,
       made with the first window). `MenuBar` sets a menu's callbacks as it opens it, so bars
       can share menus, and `setAboutToShow()` fills a menu for the window whose bar opens it.
     - **Hiding the tree:** a `Splitter` side hidden leaves all the room to the other, with no
       handle (View ▸ Folders).
     - **Shift on an activation:** `ItemView::activationModifiers()` tells it, for Shift+double-click
       and Shift+Enter. A Shift double click on a row activates now, rather than extending the
       range a second time.
     - **`FolderModel`:** entries carry a `format` (a MIME type by name when read; `setFormat()`
       after contents), plus `hiddenCount()`.
     - **Toolbar icons:** these are theme names. The flat-color theme's aliases gained
       `go-previous`, `go-next`, `go-up`, `edit-delete`, `view-list-icons` and
       `view-list-details`. A card's theme shows them once reinstalled; until then the buttons
       are text only.
     - **Host tests** (473 checks in the five cases): the service queue (order, refusals, full
       and room again), the history (back, forward, a visit dropping what was ahead, the oldest
       going, parents), `folders.conf` (read, bad keys and groups dropped, the newest written
       last, 256 kept, read back the same), formats by name.
     - **On the board:**
       - `files /sd0/p0` and `files` opened windows; `files /nonexistent` failed with status 1.
       - Moving around: arrows and Enter into `/home`, the tree following; Backspace back up
         with `home` selected; Alt+Left back with Forward enabled.
       - The address: Ctrl+L, then typing `/bin` (59 built-ins, the program icon) and
         `/sd0/p0`.
       - Views: Details from the toolbar, the selection kept, `/` remembered as details in
         `folders.conf` while `/bin` kept icons.
       - Files: File ▸ New ▸ Folder named in place (`fbtest`, re-sorted, still selected); Delete
         to the trash (restored and removed after).
       - Windows: Shift+Enter opened a folder in a new window; Ctrl+W closed windows.
       - The desktop: double-clicking a folder opened a window; Apps ▸ Files opened `/home`; an
         icon's Copy pasted into `/home` by Ctrl+V in a window, which showed it.
       - Settings: `files/show-hidden` applied live.
       - Checks: no drawing errors in `win stats`, and `win check` 0 pixels wrong.
       - Memory: three windows took 24,476 bytes of PSRAM, all given back when they closed (16
         bytes left: a list's room); internal RAM didn't change.
     - **Not checked on the board:** a letter jumping to a name. `win hid` sends no character,
       so only Alt+letters reach mnemonics; a keyboard's keys carry one.
     - **Seen once, not chased:** the first double click on a desktop folder after closing every
       window didn't open it; the second one did.
     - **Size:** `bmds_panel` 41,184 bytes, `bmds_gui` 64,416, `bmds_files` 11,578 and `bmds_ui`
       118,226 in flash (`idf.py size-components`). No new static RAM.
5. **Directory watches** in the VFS (FB14): views, the tree and the desktop follow changes.
   - *As built (2026-10-08):* as §4i, with these differences and findings:
     - **A file made is told as `Contents`,** at its close (every file made is open for
       writing). `Entries` is for folders and links made, and for anything removed or renamed.
       An open file carries a `FileIdentity` (its id, its folder's id, its device) from
       `Vfs::open()` to the volume, so its close tells both the change hook and its folder.
     - **A window whose folder vanished crashed the panel.** That was a step 4 bug the watches
       exposed: `refresh()` emptied the model and went to the parent. After an unmount the
       parent (`/ram0`) is gone too, so `go()` failed and showed a message. Its repaint drew the
       old rows from the empty model (a load fault in `IconView::drawRow()`). Now:
       - a failed open tells the views at once;
       - `refresh()` walks up to the nearest folder that is there;
       - `go()` reads the folder shown again when it can't open the one asked for;
       - `startReading()` fills the first batch at once, so the views never see a model
         emptied by its open.
     - **`TreeRows::setChildren()`** keeps the rows below the children that are still there
       (looked for from after the last one found, so a re-read in the same order is linear).
     - **Host tests:**
       - the watches on FAT (made, written, touched, removed, linked, moved between folders, a
         watched folder renamed and removed, failures telling nothing, a watch moved and
         stopped, `NotDir`, `NotExist` watched for mounts, an unmount and a mount) and on
         littlefs;
       - `FolderWatches` (one watch for two uses, changes merged into one call per folder,
         `forget()`, the last use stopping it, a folder looked for again after a mount, a call
         removing what it is told of);
       - the tree keeping open rows across re-reads, reorders and removals.
     - **On the board:**
       - Desktop: `mkdir` and a file written from the shell appeared on it.
       - A window on `/home/Desktop`: a removal, a rename, and a folder made in `/home`
         followed, the tree gaining it with the path kept open.
       - A burst: 20 files written into a window's folder by one shell loop arrived together.
       - Rename in place: a file written while a name was edited waited for the edit; Escape
         showed it with the selection kept.
       - Mounts: `umount /ram0/p0` with a window on it went to `/`, and `ram0` left the view
         and the tree; `mount` brought it back. `umount /sd0/p0` left the desktop showing
         the fallback home; mounting it again brought the icons back.
       - Checks: `win stats` 0 errors, `win check` 0 pixels wrong.
       - Memory: three windows took 22,632 bytes of PSRAM and gave all of it back on closing
         (the second cycle; the first kept 13,680 bytes of type icons cached for `/boot`'s
         files).
     - **Size:** `bmds_panel` 42,892 bytes (+1,708), `bmds_gui` 65,302 (+886), `bmds_files`
       12,644 (+1,066), `bmds_fs` 88,122 in flash. Static RAM: the watches' list head in
       `bmds_fs` (one pointer).
     - **Still left:** the tree reads a folder whole when it is expanded or re-read. The
       folders below one renamed or removed aren't told; their windows find out at their next
       read.
6. **The worker** (FB13): progress, Cancel, conflicts; Cut (FB11); Shift+Delete (FB10).
   - *As built (2026-10-08):* as §4j, with these differences and findings:
     - **Views drew on a small stack and read files there.** A view's `paint()` runs on the
       window manager's task (8 KB of internal stack). `FolderItems::icon()` loaded a type's icon
       from the theme on a cache miss, decoding a PNG there. On the board, the first try
       overflowed that stack in `inflate()` as a window opened on `/sd0/p0`. A path buffer this
       step had put in `icon()` was the last kilobyte, but the reading was step 4's. Now:
       - drawing asks `FileIcons::loadedForType()` and `loadedNamed()`, which never read a file:
         an icon not loaded is asked for, the loop loads it, and the windows draw again;
       - a cut row is looked up by folder and name (`CopiedFiles::isCut(folder, name)`), with no
         path put together.
     - **Lookups by path in a large folder are slow on the card.** About 10 ms each in a
       361-entry exFAT folder, since each one scans the folder (`du` of it takes 3.8 s).
       `FileOps` looked up every child after reading its folder, so counting `share` took about
       90 s. It now uses the type, size and time the listing gives (`entriesIn()`): the same
       count takes about 3 s. A copy still opens each file by path. On the board it copied 2,508
       items in 119 s, about 47 ms an item, the same rate as BusyBox's `cp -r`. Faster lookups in
       the FAT and exFAT drivers are a separate change.
     - **Toolkit fixes:**
       - `Dialog` keeps its title as its window's, so `setTitle()` changes it, open or not.
       - `TextWrap::wrap()` drew an empty line as the line feed itself (a box in the message box).
       - `GuiClipboard::clear()` empties a selection the application owns.
     - **The progress window** opens over the screen's middle, where a question already is. Once
       open, it raises the question above itself.
     - **The copy buffer** (32 KB of internal DMA RAM) stays held while a question waits for an
       answer (per file since, below).
     - **Host tests:**
       - `CopiedFiles`: each type rendered, a cut looked up by path and by folder and name, GNOME's
         list and URI lists read back.
       - `FileWorker` on two RAM disks: a tree copied; the same again asking for the folder
         (merged) and a file (Keep Both for all, six names after); a job queued behind a
         question dropped by its number and the asking one cancelled; a move across filesystems,
         one within and a missing path (one failure, the rest done); a move asking Replace for
         all; a removal.
       - `TextWrap` keeping an empty line empty.
     - **On the board:**
       - A copy of `share` (4.1 MB, 2,508 items) from the card to `ram0`. The progress window
         came up after a second, counting first, then the item, the folders, the bar and the
         counts. Escape cancelled it: the window closed and the worker thread ended. Done
         again, it arrived whole (`du` the same both sides).
       - A paste onto a folder of the same name asked Merge. The next question, with "Do the same
         for the others" ticked (Alt+O), took Keep Both for the four subfolders.
       - Cut: the folder dimmed. Pasted on the same disk, it moved at once and the clipboard
         emptied. Cut from `ram0` and pasted on the card, it was moved by the worker.
       - Shift+Delete asked, naming the folder, then deleted it without the trash.
       - A desktop icon cut by Ctrl+X dimmed. A copy from the shell (`clip copy`) undimmed it.
       - Two pastes in a row: "1 more waiting", both done.
       - Checks: `win stats` 0 errors, `win check` 0 pixels wrong.
       - Memory: internal RAM free was the same after the jobs as before them (449,883 bytes).
         The worker's 32 KB stack is PSRAM; 3 to 5 KB of it were used.
     - **Size:** `bmds_panel` 48,890 bytes (+5,998), `bmds_gui` 66,672 (+1,370), `bmds_files`
       20,958 (+8,314) in flash. No new static RAM.
   - *Then, efficiency (2026-10-08, the user: "getting as much efficiency out of the system"):*
     - **Faster lookups on FAT and exFAT** (`storage.md` §3): the metadata cache reads ahead, holds
       256 sectors found by hash, and resists scans; FatFs's exFAT lookup checks a name's hash before
       loading its entries. The browser's copy of `share` takes 21 s instead of 119 s.
     - **The copy buffer** is taken per file while its bytes move, sized to the file (whole sectors,
       up to 32 KB): none while a question waits, and a small file takes a sector's worth. On the
       board, internal RAM during a copy of BusyBox (1 MB) was 32 KB lower, and all of it back after.
     - **The tree reads only the listing** to expand a folder: its folders (and links that lead to
       folders, looked up) sorted as the views sort names, instead of a `FolderModel` of every file
       with its type. It stays synchronous, since following the folder shown expands the path at
       once; a listing of 361 entries on the card takes about 12 ms.
     - **The rubber band scrolls** the view while the pointer is past its edge, faster the further
       past (a 50 ms timer only then, as a scroll bar's arrow held). On the board: a band from the
       end of 361 icons dragged above the view selected 80 as it scrolled; `win stats` 0 errors.
     - **The missed desktop double click** (step 4) didn't happen again: a window closed by its close
       box, then a double click on a desktop folder, opened it.
     - **Size:** `bmds_panel` 50,082 bytes (+1,192), `bmds_gui` 67,118 (+446), `bmds_files` 20,966.
7. **Drag and drop** (FB15): the window manager, the toolkit, the views, the terminal's drop;
   designed and built in `clipboard.md` §5 and §11 (2026-10-17).
8. **Properties and a Trash window** (the trash of every filesystem in one view, Restore,
   Empty), and a Trash icon on the desktop. Designed (§4k, 2026-10-17): `.bmds` on each
   filesystem, `/trash` in the VFS; 8a the VFS and `Trash`, 8b the Trash window, 8c Properties.
   - *As built (2026-10-17):*
     - **8a, the VFS and `Trash`:**
       - The trash index (`trashindex.cpp`) holds each trash's info names and their flat names,
         read when first needed. `tellEntryLocked()` and `writtenLocked()` mark it stale by the
         trash folders' ids, which are worked out at each mount. An unwatched change still marks
         it, since it is checked before the early return.
       - The VFS rewrites `/trash/NAME` with `/home`'s rewrite. A rename to the top of a trash,
         by `/trash` or by its real `files` path, is `Trash::moveAs()` taken outside the lock.
         `Trash` itself renames with `Fs::renameRaw()`, which skips that rule.
       - A labelled name only appears for a trash brought in from elsewhere: `move()` already
         picks names free in `/trash`, and keeps its ` (2)` after the extension, as before.
     - **8b, the Trash window:**
       - `FolderModel` reads each `/trash` entry's info as it fills (`Trash::find()`), in its
         batches, and stores the folder and date after the name. It marks `/trash`, and links to
         it, by the `id` `stat()` gives them.
       - `FolderItemModel` shows five columns in `/trash` (its sorts became members). The Details
         view takes the change on its next `relayout()`.
       - Restore is a worker job (`FileWorker::Kind::Restore`), so a taken place asks in the copy
         dialog. Merge moves the trashed folder's contents into the folder there, then removes it.
       - The toolbar shows Restore and Empty Trash in `/trash`, Copy and Paste elsewhere. Rename,
         New and Paste do nothing there. The tree's `trash` row gets the trash icon
         (`TreeView::setChildIcon()`).
       - The panel watches `/trash` to show it full or empty (`FileIcons::setTrashFull()`). It
         makes `Desktop/Trash` only when it makes the Desktop folder. The desktop now shows links
         by what they lead to; it left them out before.
       - Found on the way: the Details view's drag showed no image. Its rows are wider than the
         128-pixel image, so none fit (`clipboard.md` §11). Now a drag shows the icon and the name
         (`ItemView::dragBox()`).
     - **8c, Properties:** `PropertiesDialog` (bmds_panel) counts on its own `FileWorker` with
       the new `Measure` kind. `FileOps::measure()` now tells each item counted and counts
       folders. The dialog's height is its lines at a label's height, with the column's spacing
       set to none.
     - **Host tests:** `/trash` through the VFS and the shell (flat names, a clash labelled
       `notes (disk1p0).txt`, `mv` in and out, refused creation, removal with the info, watches);
       `Trash` (relative `Path=`, `find`, restore Refuse, Replace and Keep Both, folders made
       again); `FolderModel` on `/trash`; the worker's Restore (Keep Both, Merge) and Measure.
     - **On the board** (the card's existing `.trash` moved to `.bmds/trash` by hand):
       - **Shell:** `ls /trash` listed the six old entries, whose absolute `Path=` was still
         read. `mv` into `/trash` from `/tmp` and from the card went to each one's own trash.
         `mv` out across filesystems dropped the info, and `rm` removed the entry and its info.
         Making a file or a folder in `/trash` was refused.
       - **Trash window:** opened in Details with the right locations and dates. The watch
         followed changes made elsewhere. Restore worked; a taken file asked and Keep Both made
         `two (copy).txt`; a taken folder asked Merge and merged. Empty Trash asked and emptied.
         Delete in the trash asked ("It can't be restored after.") and deleted. A drag out moved
         the file back to the folder it was dropped on.
       - **Desktop:** the Trash icon showed full or empty as the trash changed. A drop on it
         trashed the file, and a double click opened the Trash window.
       - **Properties:** shown for a file, a folder (2 files, counted), the card (2,776 files in
         about 3 s, exFAT, 30 GB), 16 items, the trash and a trashed file. A rename at OK worked.
       - **Checks:** `win check` 0 pixels wrong; `win stats` 0 errors. Internal RAM was the same
         with a Properties dialog counting and after it closed.
     - **Size:** 19,008 bytes of firmware (2,304,048 to 2,323,056), not the 6 KB estimated. The
       panel's part was larger than guessed: the dialog, the window's trash cases, and the worker's
       Restore and Measure.

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
