# Clipboard: Copy and Paste, Data Types, Drag and Drop

Status: **designed** (2026-10-06, the user's request: "full copy/paste support in the OS (along
with command line apps that allow us to copy/paste/inspect the clipboard). Ideally, this would
work with vi (and other applications that support copy/paste). We should also plan for copy/paste
of different data types (with application translation being possible). We should also add full
drag/drop engine to the devplan (although that can come later)"). Built: steps 1 to 5 and 7, and
drag and drop (§5, §11).

## 1. Goal

- **One clipboard for everything:** GUI applications, the terminal, shell commands, programs
  built with the SDK and full-screen programs in a terminal (vi, less) all copy to and paste from
  the same place. It doesn't need the window manager: a shell on the serial console can use it
  as well.
- **Typed data:** what is copied is offered as one or more types (MIME types:
  `text/plain;charset=utf-8`, `text/uri-list`, `image/png`, ...). The reader asks for the type it
  wants, and the data is translated to it on the way if needed. Translation can come from:
  - the copying application itself, which renders each type it offers, on request;
  - converters built into the OS;
  - converter programs that packages install.
- **Command-line tools:** `clip` copies, pastes, lists and clears from the shell. The clipboard's
  text is also the file `/dev/clip0`, so any program that reads or writes a file can use it. That
  includes BusyBox vi unchanged (`:r /dev/clip0`).
- **The terminal:** select with the mouse, copy, paste, middle-click paste, bracketed paste, and
  OSC 52 so that programs in it can set the clipboard, even over a serial line.
- **Drag and drop** (§5, designed 2026-10-17): the same offers and transfers carried by the
  pointer from one window to another, with copy, move and link actions.

## 2. Facts that shape the design (checked 2026-10-06)

- **Programs find services by number,** never through singletons
  (`WindowManager::manager(0)`). A clipboard is numbered the same way.
- **There are pipes** (`Pipe::create()`, bmds_kernel): two `Stream` ends that can live in two
  processes' descriptor tables. Wayland moves data the same way: the reader gets one end, the
  owner writes into the other.
- **`/dev` is a fixed table of nodes** (`vfs.cpp`: `DevNodes`, then the block devices), one level
  deep. `stat` of a stream node gives size 0.
- **BusyBox vi (1.37, our configuration):**
  - `:r FILE` sizes its read from `stat`'s `st_size` and reads that many bytes.
  - `:[range]w! FILE` opens without `O_TRUNC`, writes, then calls `ftruncate` to the written length
    (`file_write`). Without `!`, it refuses a file that exists.
  - `:!cmd` runs a shell (`FEATURE_ALLOW_EXEC`), but there are no `:r !cmd` or `:w !cmd` filters.
  - It has named registers (`"a` ... `"z`) but no bracketed paste and no visual mode.
  - Autoindent is off by default, so pasted text arrives unchanged in insert mode.
- **The terminal's screen model:**
  - It knows mode 2004 (`isBracketedPaste()`).
  - It caps OSC strings at 64 bytes (`VtParser::MaxOsc`), far too few for OSC 52.
  - It doesn't record which rows were soft-wrapped, so a selection over a wrapped line would gain
    newlines that the program never wrote.
  - It has no mouse reporting (modes 1000, 1002, 1006), so for now no program in a terminal can
    take the mouse away from a selection.
- **The terminal is a raw window-manager client** (not the toolkit). It gets keys and the wheel
  on the window manager's task, and writes to the pty from there. A large paste would block that
  task when the pty's buffer fills.
- **Windows can have mouse callbacks** (`WindowConfig::mouse`, `gui.md` G3) with an implicit
  grab, and the cursor's save-under (`windows.md`) covers 19 × 19 pixels plus 4 on each side.
- **Memory:** PSRAM is plentiful; internal RAM and `.bss` are not.

## 3. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| C1 | Where the clipboard lives | **A kernel service, `Clipboard`, in a new component `bmds_clipboard`, numbered (`Clipboard::clipboard(0)`: the seat's; one now).** It knows no window manager and no application. Its pure parts (types, the store's accounting, the converter table) are in `bmds_core` and host-tested | in the window manager, as X's selections (then a shell without the window system has none) |
| C2 | Selections | **Numbered, append only: `Clipboard` (0: Ctrl+C, Ctrl+V), `Primary` (1: the last selection, the middle click pastes it), `Drag` (2: drag and drop's, §5).** Each holds one offer at a time | `Clipboard` only (then there's no select-and-middle-click, which terminal users expect) |
| C3 | What an offer is | **An owner (a process, and its window if any), a list of up to 8 types, and per type either the bytes, stored now, or "on request".** `clip copy` and programs about to exit store the bytes. A running application can offer many types and render only the one asked for. A newer offer replaces the older one, and the old owner is told it lost the selection | stored only (then every type has to be rendered at every copy); on request only (then the data is gone when the copier exits) |
| C4 | How data moves | **A reader asks for a type and gets a handle to read.** Stored bytes are read straight from their block, which is counted so that it outlives a newer copy while being read: no copy, no pipe, no worker. For a type on request, the handle is a pipe's read end. An owner that offered on request is sent a request event with the write end, writes, and closes it (Wayland's `receive`/`send`). The reader reads at its own pace, so a large transfer needs no copy of the whole. A reader that gets nothing in 5 s gets `Error::Timeout` | a shared buffer (has a size limit; needs a copy) |
| C5 | Types | **MIME strings, 63 bytes at most, compared case-insensitively, with parameters normalized** (`text/plain; charset=UTF-8` is `text/plain;charset=utf-8`). Text is always `text/plain;charset=utf-8`; the shorthand `text` names it in `clip`. Lines end with LF | the platform's own atoms (X), numbers |
| C6 | Translation | **When the type asked for isn't offered, the reader's side converts.** It walks a converter table for the shortest path, one or two steps, from an offered type. Converters are of two kinds. Built-in ones are functions: `text/uri-list` → text (paths, one per line), `text/plain` (ASCII and Latin-1) to text, and text to `text/plain` (lossy). Programs are filters from stdin to stdout, configured in `/boot/config/clipboard.conf`'s `[Convert]` and in packages' `share/clipboard/*.conf`, the first match winning as for applications (`gui.md` G11). A filter runs as the reader's child (its launcher, its environment), not as the kernel's. The copying application translating its own data is C3's "on request" | the kernel running filters (whose environment? who waits?) |
| C7 | Size | **Stored bytes are in PSRAM, 1 MiB for all of a clipboard's selections by default** (sysconf's `clipboard/limit`; first `[Limits] Bytes` in `clipboard.conf`). A copy over the limit fails with `Error::NoSpace`. Offers on request have no limit; nothing is stored | no limit (PSRAM is shared with surfaces) |
| C8 | The command | **`clip` (a shell command and `/bin` applet, frozen once shipped):**<br>`clip [paste] [-p] [-t TYPE]` writes the clipboard to stdout, converted to TYPE (default text).<br>`clip copy [-p] [-t TYPE]... [FILE]` stores stdin or FILE, as TYPE (default text, or by FILE's extension: `.png` is `image/png`). Several `-t` offer the same bytes as several types.<br>`clip list [-p]` shows the owner, the types and their sizes (`-` when on request), and the conversions it could do.<br>`clip clear [-p]` empties it.<br>`-p` means `Primary` | xclip/xsel's interface; pbcopy/pbpaste (two commands, no types) |
| C9 | `/dev/clip0` | **The clipboard's text, as a file:**<br>Reading it gives text (converted if needed), and `stat` reports that size, so vi's `:r` reads it all. The text is fixed when opened.<br>Writing it collects what is written, and closing it copies that as text. `ftruncate` and `O_TRUNC` are accepted.<br>`/dev/clip0p` is `Primary`'s.<br>Programs built with the SDK get text copy and paste with no new system calls | a directory of types (more than vi needs; `/dev` is one level) |
| C10 | The terminal | **xterm's conventions** (the selection goes only to `Primary` unless the copy key is pressed: the user, 2026-10-06):<br>A press and drag selects characters, a double click words, a triple click lines. With Ctrl held, the selection is a rectangular block (Alt+drag moves windows: `gui.md` §4g); with Shift, a press extends it. The selection is drawn in reverse and covers the scrollback.<br>A selection becomes `Primary` when the button comes up.<br>Ctrl+Shift+C copies it to `Clipboard`. Ctrl+Shift+V and Shift+Insert paste `Clipboard`; the middle button pastes `Primary`.<br>Pasting sends LF as CR, as typing Enter does. It is wrapped in `ESC [200~` ... `ESC [201~` when the program asked for mode 2004, with any `ESC [201~` inside removed.<br>The selection is cleared when the text under it changes.<br>Once there is mouse reporting, Shift+drag selects anyway | Windows' console (copy on Enter, right click pastes) |
| C11 | OSC 52 | **Programs set the clipboard with `OSC 52 ; c ; BASE64 ST`** (`p` is `Primary`; an empty payload clears it). The parser streams an OSC 52 payload through a base64 decoder into a `clip copy`-like store rather than buffering it, up to C7's limit. **Reading (`?`) is off by default:** with it, anything printed to the terminal, such as a `cat` of a downloaded file, could ask for the clipboard. `[Clipboard] AllowRead=true` in `terminal.conf` turns it on. Writing can be turned off with `AllowWrite=false` | no OSC 52 (then programs over the serial line or in tmux have no way in) |
| C12 | vi | **Works three ways, the first two with BusyBox unchanged:**<br>(a) In the terminal: mouse selection and Ctrl+Shift+C copy, and Ctrl+Shift+V pastes into insert mode.<br>(b) Files: `:r /dev/clip0` pastes below the line; `:.,+4w! /dev/clip0` copies five lines.<br>(c) A BusyBox patch (`dist/busybox/patches/0003-vi-clipboard-register.patch`, small): the registers `"+` and `"*` are `/dev/clip0` and `/dev/clip0p`, as in Vim, so `"+yy` and `"+p` work. Where there's no such file (Linux), they are ordinary registers. Vim and Neovim, if ever built, use OSC 52 or the files | (b) only (vi's users expect registers) |
| C13 | Applications' API | **The toolkit wraps it:** `GuiApplication::clipboard()` gives `copy(selection, types, Provider)` or `copyBytes(selection, type, bytes)`, `paste(selection, type, Received fn)` (the data arrives on the application's thread as events, read from the pipe without blocking the loop), `types(selection)`, and a `selectionLost` callback. At exit, an application that owns an offer on request renders its first type into storage (Windows' `WM_RENDERALLFORMATS`), so a copy outlives its window. Ctrl+C, Ctrl+X and Ctrl+V are the toolkit's standard keys | each application on the kernel API directly |
| C14 | The ABI | **None at first:** the C++ API is internal, as the window manager's is, until GUI programs get an ABI (`windows.md` §6). Then requests become events on the program's handle with the pipe's end as a descriptor. **Frozen at once:** `clip`'s interface, `/dev/clip0` and `/dev/clip0p`, OSC 52's behavior, `clipboard.conf`'s keys, and the selection numbers | — |

## 4. Design

### 4a. The service (`bmds_clipboard`)

The sketch it was designed from (as built: `clipboard.h` and §11):

```cpp
class Clipboard {
public:
        enum class Selection : uint8_t { Clipboard = 0, Primary = 1, Drag = 2 };   // append only
        static constexpr size_t MaxTypes = 8;
        static constexpr size_t MaxType = 63;

        static Result<Clipboard *> clipboard(size_t number);                         // 0: the seat's

        // An owner: who to tell. Calls come on the requesting task; they only queue (as the toolkit's).
        struct Owner {
                using Requested = void (*)(void *arg, StringView type, Stream *write);   // write, close
                using Lost = void (*)(void *arg);                                         // replaced
                Requested requested = nullptr;
                Lost lost = nullptr;
                void *arg = nullptr;
                int32_t process = 0;      // clip list's owner; 0: the kernel
                uint8_t window = 0;       // the window manager's, when copied from a window
        };

        Result<uint32_t> offer(Selection s, Span<const StringView> types, const Owner &owner);
        Error            store(Selection s, uint32_t serial, StringView type, ByteSpan bytes);
        Error            clear(Selection s, uint32_t serial = 0);                   // 0: whoever owns it
        Error            withdraw(void *ownerArg);                                   // an owner ending
        Result<Stream *> request(Selection s, StringView type);                      // the read end
        Error            describe(Selection s, Description &out);                    // owner, types, sizes
        Error            watch(Selection s, Changed fn, void *arg);                  // a new offer
};
```

- **An offer** is a serial number, which `store()` and `clear()` must match so that a late writer
  can't overwrite a newer copy. It also holds the types and, for each type, a PSRAM block or
  "on request". Copying stored bytes is `offer()` followed by one `store()` per type. One PSRAM
  allocation per stored type; the offer's header is in `.bss`, about 64 bytes per selection.
- **`request()`** for a stored type gives a stream over its block (it seeks, so `fstat` gives
  its size). For a type on request, it creates a pipe (4 KB, PSRAM), and the owner's
  `requested` is called with the write end. An owner that has gone (`withdraw()`) makes
  `request()` fail with `Error::NotExist`.
- **Ownership:** an owner withdraws its offers itself, as a window-manager client disconnects
  (the toolkit does it for its applications). Nothing is withdrawn when a process ends, so
  stored data stays. An owner's `lost` is called when a newer offer replaces its offer.
- **Locking:** one `Mutex` per clipboard, never held across a call to an owner or a pipe write.

### 4b. Conversion (`bmds_core` table, readers' side)

- `ClipboardTypes` (pure, host-tested) normalizes and compares types, maps file extensions to
  types, and finds a conversion path (breadth-first, at most two steps, over built-in and
  configured converters).
- `ClipboardReader` (bmds_clipboard) is what `clip paste`, `/dev/clip0` and the toolkit use.
  It asks for the type itself if offered, or the first type on the shortest path. It pipes the
  data through built-in converters (in the reader's thread) or filter programs (started with the
  reader's `ProgramLauncher`, stdin and stdout on pipes).
- **`/boot/config/clipboard.conf`** (a new default; the limit, once `[Limits] Bytes` here, is
  sysconf's `clipboard/limit` since `sysconf.md` step 3, and `[Limits]` lines are reported):

      [Convert]
      # FROM>TO=COMMAND [ARG...] (a filter from stdin to stdout; Exec's quoting)
      # text/html>text/plain=html2text

### 4c. `/dev/clip0`

- The VFS gains **registered nodes**: a fixed-size table where another component adds
  `/dev/NAME` with its own open, read, write, stat and close calls. `clip0` and `clip0p` are the
  first. The block devices and the fixed nodes stay as they are.
- Opening for reading runs a `ClipboardReader` for text into a PSRAM buffer, so `stat` (and
  `fstat` on the descriptor) can report its size. A transfer stops at C7's limit.
- Opening for writing collects into a PSRAM buffer, and the last close of that descriptor offers
  and stores it as text. An empty write leaves the clipboard empty.

### 4d. The terminal

- **TerminalScreen:** a soft-wrap flag per row, set when a line wraps at autowrap and kept in the
  history's lines. `TerminalView` gets a selection to draw in reverse, as a range of anchored
  rows and columns that scroll with the history (`historyAdded()`).
- **Selection:** the window asks for the mouse (`WindowConfig::mouse`). Coordinates become cells.
  Words follow xterm's character classes. A drag past the top or bottom edge scrolls the view
  (a timer while held). Wide characters count as one unit. The text taken trims trailing blanks
  per row and joins soft-wrapped rows. It is taken with the screen's lock held, then copied with
  `Clipboard::offer` and `store`.
- **Paste:** the terminal requests text and reads the pipe on its own thread, not the window
  manager's. It converts LF to CR, wraps the text for mode 2004, and writes to the pty a buffer
  at a time, so a large paste blocks only that thread. Ctrl+C typed during a paste (an
  interrupt key) abandons the rest.
- **OSC 52:** `VtParser` gains a streaming path for one OSC number. Once `52;` is seen, bytes go
  to `oscData()` in pieces instead of the 64-byte buffer, then `oscEnd()`. `TerminalScreen`
  decodes base64 into a growing PSRAM block, and the terminal copies it at the end. A query
  writes the reply onto the pty, when allowed.
- **Keys:** Ctrl+Shift+C and Ctrl+Shift+V are taken by the terminal (they send nothing today).
  Ctrl+C is still the interrupt.

### 4e. `clip`

A shell command in `bmds_clipboard` (`BMDS_SHELL_COMMAND`), so also a `/bin` applet, over
`Clipboard` and `ClipboardReader`. `clip list` example:

    Clipboard: from terminal (pid 7, window 3)
      text/plain;charset=utf-8     214 bytes
      can convert to: text/plain
    Primary: empty

### 4f. The toolkit

As C13. `GuiApplication` keeps the clipboard's pointer and turns owner calls into events in its
ring. A paste's pipe is read by a small reader thread per paste (one at a time per application).
That thread posts the data to the loop as it arrives, so the application never blocks on another
process. Text widgets don't exist yet; the panel's first use is copying a desktop icon as
`text/uri-list` (and `text/plain`: its path).

## 5. Drag and drop (designed and built 2026-10-17)

The first sketch (2026-10-06) is replaced by this design, which follows the file browser's
FB15 (`filebrowser.md`). Built as designed, with the changes in §11 (Drag and drop).

### 5a. Facts that shape it (checked 2026-10-17)

- **Every GUI application is built in** and talks to the window manager through its C++ API
  (`WindowManager`), not the SDK. The window manager's callbacks run on its task, locked, and
  the toolkit only queues them (`GuiWindow::mouseEntry`). Calls back into the window manager
  come later, from the application's loop. Programs built with the SDK have surfaces
  (`games.md`), not toolkit windows.
- **The mouse:** a press in a window that asks for the mouse grabs it until every button is up
  (`_mouseId`). Motion with no button held goes to the window under the pointer (`hoverAt()`).
  A full-screen window or a pointer lock captures every event (`capturedPointer()`).
- **The pointer** is drawn over each round with its save-under (`UnderBytes`: 2.6 KB, PSRAM).
  `restoreUnder()` puts the pixels back before anything is drawn, so whatever moves or redraws
  under it (a view scrolling at its edge) needs nothing special. Its pictures are text rows of
  edge and fill.
- **The item views** already tell a drag apart from a click (`ItemView::DragDistance`, 4 pixels;
  `dragged()`). The desktop's grid draws its dragged icons itself (`IconView`, redrawing only the
  rows that move).
- **The panel** holds the desktop and every folder window, so most drags start and end in one
  process. `GuiClipboard` renders a paste of its own copy on the loop's thread, with no pipe.
- **Files:** `FileWorker` copies and moves (across filesystems too), with its questions and
  progress. The VFS has symbolic links (`Vfs::symlink`, emulated on FAT) and `realPath()`.
  `Trash::mountOf()` finds a path's mount.
- **Desktop Entries:** `DesktopEntry::splitExec()` fills in `%f`, `%F`, `%u` and `%U` with one
  file (`filetypes.md` T7); several files have no way in yet.
- **The terminal** is a raw window-manager client with a paste path on its own worker thread
  (`termclip`): LF sent as CR, bracketed paste, paced by the program.

### 5b. Decisions

| # | Question | Choice | Alternatives |
|---|---|---|---|
| D1 | Who carries a drag | **The window manager.** It owns the pointer, knows the window under it and draws the drag over everything. The data goes through the clipboard's `Drag` selection (C2), so a drop reads as a paste does, across processes or within one | each application alone (then no drag leaves its window) |
| D2 | Starting | **The source offers its types on `Drag`, then calls `startDrag()`** with the offer's serial, the actions it allows (a mask of Copy, Move and Link), the one it prefers, an image and a callback for the end. The window manager refuses (`InvalidState`) unless a button is still down and the press was in that window: the loop may see the motion after the release. From then on the pointer is the drag's: the source gets no more motion, nor the release | the window manager detecting drags itself (it doesn't know what is draggable) |
| D3 | What is drawn | **An image the source renders, up to 128 × 128 ARGB, with a hot spot** (where the press was in it), drawn blended under the pointer. The views render the selected items' icons ghosted (alpha halved) at their places around the pressed one, with a count when not all fit. The window manager copies the image (PSRAM) and grows the save-under to cover image and pointer, both allocated at the start and freed at the end, so a drag redraws nothing under it | a fixed 32-pixel icon beside the pointer (the first sketch; the desktop's icons would no longer seem to move with it) |
| D4 | The action shown | **New pointer shapes, appended:** `DragMove` (the arrow), `DragCopy` (a plus), `DragLink` (a bent arrow) and `DragNone` (a slashed circle). The shape is the target's latest answer | text beside the pointer |
| D5 | Targets | **A window that takes drops sets `WindowConfig::drop`** (appended, as `mouse` was). It is told Enter, Motion and Leave with the position, the actions offered, the modifiers and the button. It answers when it likes with `answerDrop(drag, window, action)` (`None`: not here). The window manager keeps the latest answer, so a busy target slows only its own feedback, never the pointer. A window without `drop` (a game's, a frame, the Alt+Tab list) shows `DragNone` | synchronous answers (a stuck target would freeze the pointer) |
| D6 | Choosing the action | **The target chooses, from the offered actions:** Ctrl copies, Shift moves, Ctrl+Shift links (Windows' keys, FB15). With none held, the target's default, then the source's preferred, then the first offered. Files: the same filesystem moves, another copies (FB15). A modifier pressed or let go during a drag re-sends Motion. A pure function (`DropAction::choose()`) does it for everyone | the source choosing (it doesn't know where the files land) |
| D7 | The drop | **A release over a target always sends it Drop,** whatever its last answer, and the target decides again at that position: an answer still on its way can't lose a drop. The pointer is normal at once. The target reads with `request(Drag, type)` (the toolkit's `paste()`), then calls `finishDrop(drag, done)`, where done is `None`, `Copy`, `Move` (the source removes its copy), `Link` or `Moved` (the target did the move itself: files). The source is told done and clears its offer. A release over no target, Esc, the source's window closing, or no finish 10 s after the release ends it with `None` | Drop only after an accepting answer (loses fast drops) |
| D8 | The right button | **A right drag asks at the drop** (Windows): the target gets Drop with `Ask` and opens a menu at the pointer with what is offered (Move Here, Copy Here, Create Link Here, then Cancel), its default first | none |
| D9 | Feedback in the target | **The toolkit's:** a view frames the folder item or tree folder under the pointer, a view near its edge scrolls (the rubber band's timer), a tree folder held over for 0.8 s expands, and a window-list button held over for 0.6 s raises its window (restoring it if minimized), so a drop can reach a window that was behind | none (then a drop can't reach a covered window) |
| D10 | Within one application | **The same path, short-circuited by what exists:** the toolkit's paste of its own offer renders on the loop's thread, and the target can ask whether a drag is its own (`DropEvent::source()`: the widget, else null). The desktop's grid becomes an ordinary target: a drop of its own icons on empty cells moves them there (the drag's offset from the press, as now), onto a folder or an application moves or opens them into it. `IconView`'s own drawing of dragged rows goes | the desktop keeping its own drag (two ways that look different) |
| D11 | Multiple files to an application | **`splitExec()` takes a list:** `%F` and `%U` get every file, `%f` and `%u` one, and an application with only those is started once per file (the specification's rule), at most 16. An application dropped on gets the files, and so does a `.desktop` file shown in a folder or on the desktop | the first file only |
| D12 | The terminal | **A drop is typed in as a paste:** `text/uri-list` as the paths, each shell-quoted (`'...'`, a `'` as `'\''`), separated by spaces with one after the last; plain text as it is. Copy only. It goes through the paste path (`termclip`), so bracketed paste and pacing apply | the URIs as written |
| D13 | Not now | **Dragging text out of a field** (fields take drops: text, and a path for a file), **drops on SDK programs' surfaces** (needs the GUI ABI: `BMDS_EVENT_DROP`, brought to the user then), **spring-loaded folders** (held over, a folder opens in its window) | — |
| D14 | The ABI | **None:** the window manager's and toolkit's APIs are internal (C14); `WindowConfig`, `Pointer` and the clipboard's selections only grow | — |

### 5c. The window manager

```cpp
enum class DropAction : uint8_t { None, Copy, Move, Link, Moved, Ask };   // append only
enum : uint8_t { CanCopy = 1, CanMove = 2, CanLink = 4 };

struct DragConfig {
        uint32_t offer = 0;               // the Drag selection's serial: passed to targets
        uint8_t actions = CanCopy;        // what the source allows
        DropAction preferred = DropAction::Copy;
        const uint32_t *image = nullptr;  // ARGB, width × height; copied (null: the arrow alone)
        uint8_t width = 0, height = 0;    // 128 at most
        int8_t hotX = 0, hotY = 0;        // the pointer's place in the image (may be outside it)
        Ended ended = nullptr;            // void (*)(void *arg, uint32_t drag, DropAction done)
        void *arg = nullptr;
};

struct DropEvent {
        enum class Type : uint8_t { Enter, Motion, Leave, Drop };
        Type type;
        uint32_t drag, offer;
        int16_t x, y;                     // the client area's
        uint8_t actions, modifiers, button;
        bool ask;                         // Drop after a right drag
};

Result<uint32_t> startDrag(uint8_t window, const DragConfig &config);
Error answerDrop(uint32_t drag, uint8_t window, DropAction action);   // the latest wins
Error finishDrop(uint32_t drag, DropAction done);                     // InvalidState after the end
Error cancelDrag(uint32_t drag);                                      // the source gives up
```

- **State** (`.bss`, about 40 bytes): the drag's number, the source window and its `Ended`, the
  window under the pointer and its latest answer, the release time. One drag at a time: there is
  one pointer.
- **The pointer's round:** with a drag, `cursorArea()` is the union of the image's and the
  picture's rectangles. `drawCursorPixels()` blends the image first (its alpha), then draws the
  picture. The save-under is a drag-sized block taken at `startDrag()`; `UnderBytes` still
  serves the plain pointer.
- **Motion:** to the window under the pointer when it has `drop` (Enter when it changes, Leave to
  the old one), with no motion to the source. Popups were closed by the press. The wheel goes on
  scrolling the window under the pointer.
- **Keys:** Esc cancels; Ctrl and Shift re-send Motion; everything else is dropped until the
  drag ends, so Alt+Tab can't change the windows under it.
- **The end:** a window closing tells the drag (the source's: cancelled; the target's: no
  target). Each `Ended` call is made once, with the lock let go, like `MenuChosen`.
- **Test aid:** `win dnd` prints the drag: source, offer, the window under it, its answer.
  `win pointer` already drags with a button held.

### 5d. The toolkit

- **Source:** `GuiWindow::startDrag(const GuiDrag &)` offers the types on `Drag` (a `Provider`
  as for copies), then calls the window manager. `GuiDrag` adds the provider and an `Ended` told
  on the loop's thread. The application's offer is cleared when the drag ends, and `keep()`
  never stores a `Drag` offer.
- **Targets:** `Widget` gains `dragEntered(GuiDropEvent &)`, `dragMoved()`, `dragLeft()` and
  `dropped()`. Every `GuiWindow` sets `drop`; the deepest widget under the pointer that takes
  drags (`setAcceptsDrops()`) is told Enter and Motion, and Leave when another one is under it,
  and its answer is the window's (one widget, not its parents in turn: a window-list button
  that never accepts still needs to know it is held over). The event offers
  `hasType(type)` (offered or converted: `canPaste(Drag, ...)`), `suggested()` (D6 with the
  widget's default), `accept(action)`, `ignore()`, `read(type, Received)` and `finish(done)`.
- **`ItemView`:** a drag past `DragDistance` asks its owner for the drag (`DragAsked`: the
  types, the provider, the actions). The view renders the image (D3). As a target, it frames
  the row under the pointer if its owner says that row takes drops (`DropAsked`), and scrolls
  at its edges. `TreeView` expands a folder held over.
- **`LineEdit`** takes text, and a file's path from `text/uri-list` (one path; several joined
  by spaces), replacing its text, unless masked. That covers the address bar (Enter goes there)
  and Settings' Folder and File rows.
- **The right button:** an item view opens its menu at the release, not the press, so a right
  press that moves is a right drag.

### 5e. The panel and the first targets

- **Folder windows, the tree and the desktop** (FB15): the selected items as `text/uri-list` and
  text (their paths), Copy, Move and Link offered. A drop on a folder (a view's background, a
  folder item, a tree folder) runs `FileWorker` (Move, or Copy) or makes links (`Vfs::symlink`,
  a taken name made unique as Paste does) and finishes with `Moved`, `Copy` or `Link` once the
  job is queued. A folder onto itself, or into its own descendant, is refused while hovering
  (`DragNone`). Dropping into the folder it came from does nothing, except on the desktop's grid
  (D10).
- **Applications** (`.desktop` files on the desktop and in folders): a drop starts them with the
  files (D11).
- **The window list:** its buttons raise (D9) and never accept.
- **The terminal** (D12). **The image viewer** opens the first image dropped on it.

### 5f. Pure parts (`bmds_core`, host-tested)

- `DropAction::choose(offered, modifiers, targetDefault, preferred)` (D6).
- `ShellQuote::append()` (D12): quoting only when needed (`[A-Za-z0-9._/+-]` passes as is).
- `DragImage`: where the selected items' icons go in the 128 × 128 image around the pressed one,
  and the count.
- `DesktopEntry::splitExec()` with several files (D11).

### 5g. Budget

- **Firmware:** the window manager 3 to 4 KB; the toolkit 4 to 5 KB; the views and the panel 3 to
  4 KB; the terminal 1 KB; the pure parts 1 KB.
- **`.bss`:** about 40 bytes (the window manager's drag).
- **PSRAM, only during a drag:** the image (64 KB at most) and its save-under (about 57 KB at
  most).
- **CPU:** blending at most 16,384 pixels a round while the pointer moves; nothing while still.
  To measure (`win stats`); PIE if it shows.

### 5h. Steps

1. **Pure parts** (§5f), host-tested.
2. **The window manager** (§5c): `startDrag()` and the others, `drop`, the image, the shapes,
   Esc, modifiers, the 10 s end; `win dnd`.
3. **The toolkit** (§5d): sources, targets, the views' frames and scrolling, the tree's
   expanding, `LineEdit`'s drops.
4. **The panel** (§5e): folders, tree and desktop on it (the grid's own drag gone), the right
   drag's menu, applications with files, the window list.
5. **The terminal and the image viewer.**
6. **On the board:** drags by `win pointer` between two folder windows on one filesystem and
   across (`/sd0` and `/flash`), onto the desktop and back, rearranging the desktop, onto a
   terminal (quoted paths), onto an application, a right drag, Esc, a slow target, the window
   list raising a covered window; `win check` with a drag shown; `win stats` while dragging.

## 6. Budget (estimates)

- **Firmware:**
  - the service, reader and converters: 5 to 7 KB;
  - `clip`: 2 KB;
  - `/dev` registered nodes: 1 KB;
  - the terminal's selection and paste: 3 to 4 KB;
  - OSC 52 with base64: 1 KB;
  - the toolkit's wrapper: 2 KB;
  - drag and drop: §5g.
- **`.bss`:** about 200 bytes per clipboard (three selections' headers, the lock, watchers).
- **PSRAM:** only what is held (C7's limit), plus a pipe (4 KB) for each transfer in progress
  and the terminal's selection (a few KB while dragging).
- **CPU:** none while idle (no polling: watchers are told, transfers are paced by the pipe).

## 7. ABI impact

- **Frozen when shipped:**
  - `clip`'s interface (C8);
  - `/dev/clip0` and `/dev/clip0p` (C9);
  - OSC 52 (C11) and the terminal's keys;
  - `clipboard.conf`'s groups and keys;
  - `terminal.conf`'s `[Clipboard]`;
  - the selection numbers.

  The commit adding them carries an `ABI:` line.
- **No new system calls:** SDK programs use `/dev/clip0` with `open`, `read`, `write`,
  `ftruncate` and `fstat`. Typed access for SDK programs comes with the GUI ABI (C14).

## 8. Testing

- **Host:**
  - `ClipboardTypes`: normalizing, comparing, extensions, conversion paths, and the converter
    table from INI text;
  - base64 decoding in pieces;
  - `VtParser`'s OSC 52 stream (split anywhere, over the limit, cancelled by CAN);
  - `TerminalScreen`'s soft-wrap flags and the selected text (words, lines, blocks, wide
    characters, trailing blanks, wrapped rows, scrollback);
  - the store's serials and accounting.
- **Board:**
  - `clip copy` and `clip paste` round trips, including binary, a 900 KB file and the limit;
  - `clip list`;
  - `/dev/clip0` with `cat`, a redirect, and vi's `:r` and `:w!`;
  - an offer on request through the toolkit;
  - an owner killed in the middle of a transfer (the reader sees the end, not a hang);
  - the terminal: selections checked by `clip paste`, a scripted drag (`win pointer`), a paste
    into vi and into the shell, bracketed paste with `printf '\e[?2004h'`, and OSC 52 from
    `printf` over the serial console and in a window;
  - nothing to do (`idf.py size`, `.bss`) while idle.

## 9. Steps

1. **Pure parts** (host-tested; done, §11): `ClipboardType`, base64 in pieces, the converter table.
2. **The service, `clip`, `/dev/clip0`** (done, §11): `Clipboard`, `ClipboardReader` with the
   built-in converters, the VFS's registered nodes, `clipboard.conf`.
3. **The terminal** (done, §11): soft-wrap flags, selection and its drawing, `Primary`, the copy
   and paste keys, the middle click, paste on the terminal's thread, mode 2004.
4. **OSC 52** in `VtParser` and the terminal (done, §11).
5. **vi:** the register patch (C12 c) in `dist/busybox`, and the card rebuilt (done, §11).
6. **Converter programs** (`[Convert]`, packages' `share/clipboard`).
7. **The toolkit's API** and the panel's icon copy (done, §11).
8. **On the board** (§8), measured.
9. **Drag and drop:** designed (§5), with its own steps (§5h).

## 10. Open questions

- A clipboard history (a manager that watches and keeps the last few, as Windows' Win+V):
  `watch()` makes it an ordinary program later.
- Images: `image/png` copies work as soon as something offers them (`screenshot` could:
  `screenshot -c`). Pasting needs a program that takes images.
- Mouse reporting in the terminal (modes 1000, 1002, 1006), so vi with `:set mouse` or `less`
  can use the mouse; C10's Shift override is designed for it.

## 11. As built

- **Step 1** (2026-10-06, `bmds_core`, host-tested in `tests/host/clipboard.cpp`):
  - **`ClipboardType`:** `normalize()`, `isValid()`, `equal()`, `fromFileName()` (17
    extensions), `Text`. A normal form is at most 63 bytes. Parameters keep their order, so
    `a/b;x=1;y=2` and `a/b;y=2;x=1` are different types.
  - **`Base64Decoder` and `Base64Encoder`:** pieces of any size. The decoder skips white space,
    accepts missing padding, and fails on a bad character, on data after padding, or on an end
    in the middle of a byte. It writes what came before a failure.
  - **`ClipboardConversions`:** up to 32 converters, configured ones first; the first for a pair
    wins. Each type is kept with the hash of its normal form, so `find()` normalizes only on a
    hash match. It searches breadth-first, as is, then one step, then two, and at each depth
    the earliest offered type wins. The built-in ones:
    - `text/uri-list` to text;
    - `text/plain`, `text/plain;charset=us-ascii` and `text/plain;charset=iso-8859-1` to text;
    - text to `text/plain`.
  - **`[Convert]` lines are split by hand, not by `IniReader`'s key:** a type's parameters
    contain `=`. The pair is split at the first `>`. The separating `=` is the first one after
    TO's parameters: `text/html>text/plain;charset=utf-8=html2text` works. Lines that aren't
    converters are reported with their numbers.
  - **`ClipboardConverter`** (the built-in ones, in pieces of any size):
    - URI lists: `file://HOST/PATH` gives the path with `%XX` decoded. Other URIs are kept as
      written. Comments and blank lines are dropped, CR LF is accepted, and there's no LF after
      the last line.
    - Text to Latin-1: `?` for each character it has no byte for, and for each malformed
      sequence.
    - Every converter's output is checked to be the same at every piece size from 1 byte up.
- **Step 2** (2026-10-07; `bmds_clipboard`, host-tested and on the board):
  - **`Clipboard`:**
    - `offer`, `store`, `copy`, `clear`, `withdraw`, `request` and `describe`.
    - `create(n)` and `addFiles(n)`, so the host tests make clipboard 0 and its files as the
      `clipboard` boot stage does (after `/boot`: 0.66 ms).
    - An offer is one PSRAM block of about 600 bytes. Stored bytes are one block shared by the
      offer's types; for a reader it is counted, not copied.
    - Owner calls are made under a lock of their own. So an owner call must only queue: calling
      the clipboard back could deadlock. Once `withdraw()` returns, no call is still running.
  - **`ClipboardReader`:** up to two built-in converters on the way. A way through a converter
    program is `NotSupported` until step 6.
  - **`/dev/clip0` and `/dev/clip0p`:**
    - The VFS gained files that other components add (`Vfs::addDeviceFile`, 4 at most). They
      open without the VFS's lock, since a paste may wait for its owner.
    - Read, they are a regular file of the text when opened: converted if needed, and empty if
      there is none. A write is held in memory and copied when the last handle closes, if
      changed.
    - Opening without `O_TRUNC` loads the text first, so vi's write-then-`ftruncate` works.
      `O_TRUNC` with nothing written clears the clipboard.
    - `stat` by name gives the stored text's size (0 if it isn't stored as text); `fstat` on
      an open one gives the real size.
  - **`clip`**, as C8. `clip list` shows both selections (`-p`: the primary only) and the types
    each can also be read as.
  - **On the board:**
    - BusyBox vi, unchanged: `:$r /dev/clip0` pasted below the last line; `:1,2w! /dev/clip0`
      copied two lines (owner `/dev/clip0`, 18 bytes).
    - A 900,000-byte binary: copied in 0.18 s, pasted to the RAM disk in 0.12 s, MD5
      identical.
    - Holding it used 901,736 bytes of PSRAM; `clip clear` gave back every byte. Internal RAM
      didn't change.
    - 1,048,577 bytes is refused ("over the clipboard's 1048576 bytes", status 1) and the old
      contents stay.
  - **Size:** `bmds_clipboard` is 8,400 bytes of flash and 240 of `.bss`. The image grew 14,816
    bytes with step 1's parts, the VFS's added files and the default `clipboard.conf`.
- **Step 3** (2026-10-07; the terminal, host-tested and on the board):
  - **`TerminalCell::wrapped`** (the spare bit): set on a row's last cell when autowrap carries
    the text on, before the line feed that may scroll it into the history. Writing or erasing
    the cell clears it.
  - **`TerminalSelection`** (bmds_ui, pure):
    - Lines are numbered from the history's start (`TerminalView::line()`,
      `lineOfViewRow()`), so a selection stays on its text as it scrolls into the history.
    - Units: characters (boundaries between cells), words, and lines (with the rows the line
      wrapped across). A block takes the same columns of every line.
    - Its text trims each row's trailing blanks, joins wrapped rows, and takes a wide
      character once.
    - `TerminalView::cells()` draws the selected columns reversed, so a reversed cell that is
      selected shows plain.
  - **Mouse events carry the modifiers** (`MouseEvent::modifiers`). Ctrl, not Alt, makes a
    block, since Alt+drag moves windows.
  - **`Pty::input()`:** a paste waits for the program to read what doesn't fit in the 512-byte
    input buffer. A new input-space semaphore is released when the program reads and when
    either end finishes. `write()` still never waits: the window manager's key path uses it.
  - **The terminal:**
    - Copies and pastes run on a worker thread (`termclip`, 6 KB stack in PSRAM). It starts at
      the first copy or paste; the mouse and key callbacks only set a bit and wake it.
    - Output that dirties a selected row clears the selection, as in xterm. A whole-screen
      scroll dirties every row, so a selection on the screen goes when output scrolls it.
    - A paste sends LF and CR LF as CR. With mode 2004 it is wrapped in `ESC [200~` and
      `ESC [201~`, with any ESC inside dropped.
    - A typed Ctrl-C stops a paste in progress.
  - **On the board:**
    - A drag selected "selection" (`clip -p`: 9 bytes, from the terminal, its pid). A double
      click took a word, drawn reversed; a triple click took the line with its LF. `win check`
      found 0 wrong pixels with a selection shown.
    - Ctrl+Shift+V pasted a line, and a two-line script that ran. The middle button pasted
      the primary selection. Ctrl+Shift+C copied a selection; Shift+Insert pasted it.
    - 2,000 lines (8,893 bytes) pasted into `cat` all arrived: the paste waited for the
      program.
    - Pasted into BusyBox vi's insert mode, three lines with an indented one were saved
      exactly.
    - With mode 2004 left on, vi loses a paste's start: vi doesn't know bracketed paste, so
      `ESC [200~` ends insert mode. Mode 2004 is the program's to turn off; xterm does the
      same.
    - The console's line editing (cooked mode) drops the `ESC [200~` and `ESC [201~`
      sequences, so `cat -v` doesn't show them. Programs that read raw keys get them.
    - Once, `cat` in the terminal seemed to stop showing output until it was sent SIGINT,
      after which everything appeared. The same steps replayed didn't do it again. To watch.
  - **Testing aid:** `win hid` events have no character (they skip the keymap), so a raw
    Ctrl+C or Ctrl+D with `win hid` sends the program nothing; `win type 3 '\x03'` does.
    The terminal's own keys use the usage code, so `win hid` drives them.
  - **Size:** the image grew 4,976 bytes to 822,512.
- **Step 4** (2026-10-07; OSC 52, host-tested and on the board):
  - **`VtParser` streams long OSC strings:** at an OSC's first `;`, a handler whose
    `oscStream(number)` says so gets the rest through `oscData()` in pieces of up to 64 bytes,
    then `oscEnd(complete)`. BEL or ST ends it; CAN, SUB or a reset cut it off. The defaults
    keep every other handler (`ColorFilter`, which passes long sequences through anyway) as
    it was.
  - **`TerminalScreen`:**
    - Takes OSC 52 when its owner set a clipboard sink. Base64 is decoded as it arrives
      (`Base64Decoder`, white space skipped), and still nothing is allocated.
    - The sink is told Begin with the targets, Data, then End or Cancel; or Query (`?`).
    - `52;c;` empties the selection. A string cut off before its first 64-byte piece tells
      the sink nothing.
  - **The terminal:**
    - Gathers the bytes in PSRAM up to the clipboard's limit. Over it, the copy is dropped.
    - Copies them once the screen's lock is let go.
    - Targets: `c` and `s` are the clipboard, `p` the primary selection, and none means
      xterm's default (`s0`).
    - A query is answered by the worker as `ESC ] 52 ; c ; BASE64 BEL`, typed in as a paste
      is, only with `[Clipboard] AllowRead=true` in `terminal.conf`.
    - `AllowWrite=false` ignores sets. Both keys are in the default file (a board that has the
      file keeps its own: the defaults apply).
  - **On the board:**
    - A `printf` of OSC 52 in the terminal set the clipboard (16 bytes, from the terminal).
    - `seq 1 5000 | base64` inside OSC 52 (wrapped lines included) gave 23,893 bytes, MD5
      identical to `seq 1 5000`. Nothing was drawn while it passed.
    - With `AllowRead=true`, a query's reply carried `cXVlcnkgbWU=` ("query me"). The
      console's line editing (cooked mode) dropped its leading `ESC ]`; a program reading raw
      keys gets all of it.
  - **Size:** the image grew 2,864 bytes to 825,376.
- **Step 5** (2026-10-07; `dist/busybox/patches/0003-vi-clipboard-registers.patch`, on the board):
  - **Registers 28 and 29** (`reg[]` and `regtype[]` grew from 28 to 30) are `"+` and `"*`.
  - **A yank or delete into one** (`text_yank()`) also writes it to `/dev/clip0` or
    `/dev/clip0p`.
  - **A put from one** (`p`, `P`) first reads the file into it. Text ending in a newline is put
    as whole lines, other text into the line, as Vim does.
  - **Without the file** (on Linux) they are ordinary registers.
  - **Size:** BusyBox is 1,056,788 bytes, sent on its own with `bmdsboard.Board.send_files()`
    in 5.6 s. The patched `vi.c` builds with no warnings (upstream's other files have three).
  - **On the board:**
    - `"+p` with two lines on the clipboard put them below the cursor's line.
    - `"+yy` on the last line made the clipboard `second\n` (from `/dev/clip0`); `"*yy` made
      the primary selection `first\n`.
    - `"+p` of `XYZ` (no newline) put it after the cursor's character: `aXYZb`.
- **Step 7** (2026-10-07; `GuiClipboard` in bmds_gui, the panel; on the board):
  - **`GuiApplication::clipboard()`** makes a `GuiClipboard` the first time (PSRAM, about
    1.5 KB with its slots).
  - **Its calls:** `copyText()`; `copy()` of types with a `Provider` that renders a type on
    the loop's thread; `paste()`, whose bytes reach a `Received` on the loop's thread, whole;
    `canPaste()`, `owns()`, `setLost()`, `setName()` (`clip list`'s owner).
  - **Owner calls** (another program asking, or a newer copy) only fill fixed slots (4 requests,
    4 pastes) and `post()` one drain to the loop.
  - **A thread of its own** (`guiclip`, 6 KB stack in PSRAM, started at the first copy on
    request or paste) writes rendered bytes into the askers' pipes and reads pastes, so the
    loop never waits for another program.
  - **A paste from the application's own copy** is rendered and converted on the loop's
    thread: through a pipe, it would wait on its own thread.
  - **When `exec()` returns,** `keep()` renders each copy on request's first type, stores it
    and withdraws, while the application's objects still exist. The destructor withdraws
    whatever is left, so no owner call reaches a freed object.
  - **The panel:** a right click on a desktop icon selects it and opens Open and Copy. Copy
    offers the icon's `.desktop` file as `text/uri-list` (`file://`, `%XX` for what a URI
    can't hold) and as text (its path), rendered on request. `PanelEntry` keeps its file's
    path (128 bytes).
  - **On the board:**
    - Copy put `text/uri-list` and text "on request" on the clipboard, from the panel.
    - `clip` read the path, and `clip -t text/uri-list` read
      `file:///boot/desktop/terminal.desktop` followed by CR LF, both through pipes from the
      panel's thread. `cat /dev/clip0` gave the path.
    - After `kill -INT` of the panel, the clipboard held the URI list stored (39 bytes), and
      text converted from it.
    - Not yet exercised: `paste()` and `canPaste()` (no application pastes yet), and the
      paste of a copy of its own.
  - **Size:** the image grew 5,680 bytes to 831,056.
- **Drag and drop** (2026-10-17, §5; host-tested and on the board):
  - **Pure parts** (`bmds_core/dragdrop.h`, `tests/host/dragdrop.cpp`, 50 checks):
    `DropActions::choose()`, `ShellQuote` (plain words as they are, else single quotes),
    `DragImage::around()` and `holds()`; `DesktopEntry::splitExec()` with a list of files and
    `takesFileList()`.
  - **The window manager:** `startDrag()`, `answerDrop()`, `finishDrop()`, `cancelDrag()`,
    `dragInfo()`, `WindowConfig::drop`, and the four drag pointers. The image is blended under
    the pointer, the save-under a block of the drag's size. Error codes: `InvalidState` where the
    design said `Stale` (no new code). The test aid is `win dnd` (`win drag` already moved
    windows).
  - **A patch is a scanline at most:** `Surface::patch()` copies through the surface's line
    buffer, one row of the screen's bytes, so a 68 × 50 drag area was refused (`Invalid`) at
    every round. The pointer is now saved, drawn and put back in strips that fit
    (`cursorStripRows()`); the plain pointer is still one strip.
  - **The toolkit:** `GuiDrag`, `GuiDropEvent`, `DragPicture` (`guidrag.h`); one widget takes a
    window's drag (the deepest that accepts, without walking its parents: §5d). `ItemView`
    draws the selected rows ghosted (`drawDragRow()`, the icon view's with its 32-pixel icons)
    with a red count badge, frames the target row, scrolls 16 pixels at a time within 16 pixels
    of its edge, and tells `dropHeld()` after 0.8 s (the tree expands). Its menu now opens at the
    right button's release. `IconView`'s own drag drawing went; `moveSelected()` does the cell
    move for the desktop's own drops.
  - **The panel:** `FolderActions::dragFiles()`, `dropAction()`, `drop()` (copy and move by the
    worker or a rename, links with a copy's name if taken, the right drag's menu), and
    `launchFiles()`. Files dropped on the desktop's folder go where they were dropped
    (`FolderHost::dropping()`, placed when the desktop shows them). Window-list buttons raise
    their window after 0.6 s.
  - **On the board** (`win pointer` drags, `win dnd` mid-way):
    - A file from one card folder to another moved; onto the tree's `tmp` copied (another
      filesystem; the tree had scrolled a row in its edge zone, so it went into `tmp/dnd`, the row
      under the pointer at the drop).
    - The terminal typed `/sd0/p0/dndtest/b/one.txt '/sd0/p0/dndtest/a/two words.txt'`.
    - The desktop's icon moved to another cell (its place saved); its own drag over the desktop
      answered move. A drag left Ctrl "held" by an earlier `win type` of a control character
      copied instead: end such tests with `win hid release 0xe0`.
    - A right drag's menu made a link (`one.txt -> /sd0/p0/dndtest/a/one.txt`); Esc on the menu
      ended the waiting drop at once, nothing done; Esc during a drag cancelled it.
    - A drag held 0.6 s over a minimized window's button restored it, and the drop went in.
    - Two files dropped on a launcher with `%F` started it once with both paths; dropped on the
      Terminal launcher (no field code) they became its arguments.
    - Files dropped on the desktop landed at the drop point, and dragged back into a folder.
    - The address bar took a dropped file's path; the image viewer opened a dropped picture.
    - `win check` with a drag shown: 0 pixels wrong. A drag's round: 3.3 ms; 0 drawing errors.
  - **Size:** the image grew 21,216 bytes to 2,304,048.
  - **Fixed later (2026-10-17, `filebrowser.md` §4k):** a drag from the Details view showed no
    image. Its rows span every column, wider than the 128-pixel image, so `DragImage::holds()`
    took none. A drag now shows a row's icon and name (`ItemView::dragBox()`, the Details view's
    first column).
