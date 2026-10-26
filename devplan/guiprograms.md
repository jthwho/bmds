# GUI Programs: the Toolkit for Every Program, and Angel's Bindings to the Whole System

Status: **steps 1-3 built** (2026-10-22; §9-§11): the protocol, the classes, pictures and the
clipboard. **Step 4 built** (§12). **Step 5 built** (§13): 5a (angel's GUI classes, `app`,
callbacks; `Screen` for games), 5b (the one-line dialogs, icons from angel's `Image`, the notes
example), 5c (a `Canvas` drawn as a `Screen` is, screens' events in `app.run()`, the paint
example). **Step 6** designed (§14), in three parts: 6a built (settings, processes, the system,
dates, the folders); 6b built (folders watched, file types, opening files, pads and actions); 6c
built (players, the mixer, voices, MIDI ports). Next: step 7 (the guide). The user agreed with the shape (2026-10-22): the
toolkit as a service (P1), names (P2), and **a wire protocol in the spirit of X11's** between
programs and the service (P8), so that applications can also run on another machine. This plan
revises `gui.md` G1 and fills in `windows.md` §6 ("ABI (later)").

## 1. Goal

The user's direction (2026-10-22):
- **Full GUI applications in angel:** windows with buttons, menus, fields, lists and dialogs,
  not only a drawing surface.
- **Multiple windows,** used together.
- **Ideally, angel can use anything the OS provides.**

What the plan gives:
- **Every program the toolkit:** a GUI interface for programs that any program can use, C ones
  from a card as much as `angel`.
- **Remote applications** (the user's follow-up, 2026-10-22): programs speak to the service in a
  protocol on a byte stream, as X11 clients do. A program on another BMDS board or on a PC (C
  through the client library, or `angel` built for Linux) can open windows on this board's
  screen.
- **Angel's bindings to it:** widgets, menus, dialogs, many windows, one event loop for all of
  them.
- **Angel's bindings to the rest:** the services that already have system calls but no script
  API.

Not goals: a different look from the built-in applications (programs look exactly like them);
running program code inside the system (a program's code never runs on the window manager's
task); a web-style layout language.

## 2. Where things stand (checked 2026-10-22)

**Angel's API** (`tests/scripts/api.lock`, 373 lines):
- **Windows:** up to 8 per script, each a `Window`. A `Window` is a picture to draw on, with its
  own events (`nextEvent`, `waitEvent`, `keyDown`, the mouse). Games run in one with
  `run(Game)`.
- **The rest:** files, other programs (`run`, `output`), reading a setting, simple
  notifications, sound and MIDI notes, speech, sockets and HTTP.
- **Not there:**
  - controls: no buttons, menus, fields, lists, dialogs or message boxes;
  - waiting on many windows at once (no "the next event of any of my windows");
  - windows the user resizes (no resize event), dialogs and popups above an owner;
  - gamepads and actions, though surfaces get them (`BMDS_EVENT_PAD`, `BMDS_EVENT_ACTION`);
  - the clipboard, beyond reading and writing `/dev/clip0` as a file;
  - settings written or watched, notifications with actions, statistics, the network's state,
    audio streams and decoders, MIDI ports, processes with pipes.

**What programs have** (`sdk/include/bmds/sys.h`):
- **Surfaces** (`games.md` §3, §11): a swap chain of the program's own buffers, at a fixed size
  that the window manager scales into a window or the full screen. Events come on the surface's
  descriptor: keys, the pointer, the wheel, active, presented, close, actions, pads. There are
  controls for the title, the pointer, a palette, waiting for a frame, information, and 2D-DMA
  images and blits.
  - This suits games. It doesn't suit a GUI application, whose content should follow the
    window's size, not be scaled to it.
- **System calls for most services:** settings (find, get, set, declare, watch), notifications
  (open, send, close, an event descriptor), audio, MIDI and the synth, speech, statistics, the
  network's information and watching, processes and pipes, `poll`.
- **None for the toolkit:** `gui.md` G1 and `clipboard.md` C14 and D14 say the toolkit and the
  typed clipboard stay internal until GUI programs get an ABI; drops on program surfaces wait for
  it too (`clipboard.md` D13).

**The toolkit** (`bmds_gui`, about 52 files and 14,800 lines of C++):
- **The widgets:** windows, layouts, labels, buttons, check boxes, fields, number fields, sliders,
  combo boxes, list, icon and details views, trees, tabs, splitters, scroll areas, `TextEdit` and
  `CodeEdit`, the terminal widget, menus and the menu bar, the status bar, tooltips, tray icons,
  message boxes, file and colour dialogs, the typed clipboard, drag and drop.
- **Drawing is the window manager's:**
  - A window has no buffer of its own. The window manager calls the application back to paint
    the window's visible pieces straight onto the screen's surface, through a `Painter` (fills
    by the 2D-DMA, text through a strip in internal RAM).
  - The paint runs on the window manager's task, holding the application's lock.
  - An application's own loop (`GuiApplication::exec()`) runs on its own thread and dispatches
    the events the window manager queued.
- **Who uses it:** built-in applications (Edit, the panel, the file browser, the settings, the
  media player and others), which are linked into the firmware. They use only its public calls
  (the user's rule: built-in applications aren't special).
- **Why programs can't simply be given it:** a program can't be called back on the window
  manager's task. Its code isn't the system's to run there, and a slow or broken program would
  stall every window.

**Memory** (`top` on the board, the desktop and a game running):
- PSRAM: 19.4 MB of 31.9 MB in use.
- Internal RAM: 406 KB of 534 KB.

## 3. Decisions

| # | Decision | Recommended | Alternative |
|---|---|---|---|
| P1 (agreed) | How programs get the toolkit | **The toolkit as a service:** the widgets live in the system, which runs them for the program exactly as for a built-in application; the program holds handles, sets properties, calls methods and reads events, all in messages on one connection (§4a). Revises G1 | **The toolkit as an SDK library** (G1 as written): the same sources compiled into each program, drawing into a window-sized buffer of the program's own |
| P2 (agreed) | How the ABI names classes, properties, methods and events | **By name**, checked against each class's table (`"text"`, `"clicked"`): append-only by adding names, no new message per widget, and the program can ask the system for the table (§4b). On the wire a name is sent once and then used by its number (an atom, §4a) | numbers in a header (smaller, but every addition is a header change and an SDK release) |
| P3 | Custom drawing | **A `Canvas` widget** at the widget's size (resized with it): locally backed by a surface's buffers, drawn as a game's surface is today; remotely sent as damaged pixels (P11). `angel`'s `Window` drawing methods work on it | drawing commands sent to the system (a display list: complex, and the system keeps them for repaints) |
| P4 | Where a program's toolkit runs | **One system task per connection** (its `GuiApplication` loop, reading the connection's requests), started when it connects and ended when it closes; its stack in PSRAM if the measurements allow (§5) | one task for all programs (`GuiApplication::exec()` is one loop per application; a big list in one program would stall the others) |
| P5 | A window's close | **Asked, not done:** a program's window gets a close request and closes itself (or doesn't: unsaved changes). Also for built-in applications, which fixes Edit's "reopen to ask" (`editor.md` §16) | closed at once and told after (today's) |
| P6 | Angel's event model | **Callbacks:** `button.onClick(@handler)`, a delegate or a function; `App.run()` serves every window and timer until the last window closes or `App.quit()`. Events as objects stay for those who poll | events polled from each window (today's model; awkward with many) |
| P7 | Games' windows | **Unchanged:** surfaces stay as they are for games (`Window` in angel, `libbmdsgame`), and a GUI window may hold a `Canvas` | games moved onto the service (no gain: they draw everything themselves) |
| P8 (agreed) | How programs talk to the service | **A protocol on a byte stream** (§4a), X11's model with its lessons: client-chosen ids, requests asynchronous unless they answer, names interned once, versions and extensions agreed at connection, no pointers on the wire. Locally the stream is a connection descriptor from `gui_open()`; remotely, TCP | system calls per operation (§4a's first draft: no remote programs, a call per property) |
| P9 | Remote access and its security | **The service listens only on the board's loopback, and only when a setting says so; a remote program reaches it through SSH forwarding** (Dropbear in the ROM forwards TCP both ways), and a connection by TCP also presents the user's cookie (§4g). Never a listener on the network's interfaces in this plan | an open TCP port (X11's `xhost +`, a known liability) |
| P10 | Byte order and layout | **Little-endian, every field 4-aligned, sizes in bytes**: both the P4 and every PC this is likely to meet are little-endian, so neither side swaps; a big-endian client converts | X11's either order, chosen at connection (both sides need both paths) |
| P11 | Pictures | **Images are objects:** sent once (`PutImage`) and then named by id wherever an image is wanted (an icon, a button's picture, a canvas copy). A `Canvas` is drawn locally through its own surface (a system call gives the descriptor: the protocol's only local-only part), remotely by `PutPixels` of damaged rectangles, compressed | pixels in every message that shows one |
| P12 | Reading a remote program's files | **Later (§4g):** a file dialog opened by a remote program shows the board's files at first; the protocol reserves a way for the dialog to list the program's own folders through it | the board's files only, forever |
| P13 | Notifications for GUI programs | **A `Notification` class in the protocol** (§4b), shown by the display's notification service, so a remote program's notifications appear on the board; the fields and events are `bmds_notification_t`'s (`notifications.md`). Programs without a GUI keep the system calls | the system calls only (a remote program's notifications would appear on its own machine) |
| P14 | Programs talking to each other | **The window manager's service bus for every client** (the `SERVICES` extension, §4h): register a name, send to one, receive as events, watch them come and go. The tray protocol already runs on it, so any program can host a tray | none (each pair of programs invents its own pipe or file) |
| P15 | Panels, taskbars, docks and desktops | **The `SHELL` extension (§4i), privileged:** the window list and its changes, other programs' windows acted on, screen edges reserved, the desktop's and panel's levels, the desktop menu, global hotkeys, launch feedback, the palette, screen capture. Granted by a setting, to local connections only unless the user allows a remote one. The user's rule (built-in applications aren't special) asks that a panel from a card can do what `/bin/panel` does | the shell's calls internal (only the built-in panel could ever be one) |

**Why P1's service is recommended over the library:**
- **Memory:** the library needs a buffer per window, about 1.6 MB for a 720 × 576 window in
  24-bit with two buffers, against today's 31.9 MB of PSRAM. The service draws straight to the
  screen, as built-in applications do, and needs none.
- **One copy of the code:** the toolkit (hundreds of KB) is in the firmware once, not linked into
  every program.
- **One look:** fixes to it reach every program at once.
- **Usable from C and from elsewhere:** the interface is messages of handles and names, which any
  language with a byte stream can speak, including C, the coming compiler (Phase 8), and
  programs on other machines (P8). A C++ library would need a C interface written anyway.
- **Isolation:**
  - The program's code never runs on the system's tasks.
  - A program's widgets are its own: checked on every call, freed when it ends, limited in number.
  - A slow program only delays its own windows.
- **Thin bindings:** angel's become mostly a table of names.

**What the service costs:**
- **A wider ABI:** every widget class, property, method and event is frozen once shipped, and so
  is the protocol. Names (P2) keep that append-only and the protocol small, but the toolkit's
  behaviour becomes userspace ABI: a property's meaning can't change.
- **A system task and some memory per GUI program** (§5).
- **Widgets the toolkit lacks** can't be made by a program, only drawn on a `Canvas`. They get
  added to the toolkit, which is how every program then gets them.

## 4. Design

### 4a. The protocol (P8)

**The model**, as X11's: a program (the client) opens a connection to the service (the server)
and writes requests on it; the service writes back replies, events and errors. Requests are
asynchronous: only those that answer something make the client wait, and the client library
writes requests in batches. Building a window of 200 widgets is one `write()`.

**Connections:**
- **Locally:** `gui_open(display, flags)` returns a connection descriptor. It reads, writes and
  works with `poll()`, a full-duplex stream between the program and the service in the kernel,
  with no network stack. The kernel has no `socketpair` or Unix sockets, so this is a new kind of
  stream: two queues, one each way, of 16 KB each.
- **Remotely:** TCP to the service's port on the board's loopback (P9, §4g).
- **Which display:** the client library reads `BMDS_GUI_DISPLAY`, as X11's `DISPLAY`:
  - unset, or `local`: `gui_open(0)`;
  - `HOST:PORT`: TCP, normally `localhost:PORT` through an SSH tunnel.
- **The same bytes both ways:** a program can't tell, and needn't, whether it is local.

**Framing** (P10: little-endian, 4-aligned):
- **A request:** `uint16 opcode`, `uint16 flags`, `uint32 length` (the whole message in bytes, a
  multiple of 4), then its body. Requests are numbered by the server as they arrive, from 1 (the
  sequence; 32 bits).
- **From the server:** `uint16 kind` (reply, event or error), `uint16 code`, `uint32 length`,
  `uint32 sequence` (the request a reply or error answers; for an event, the last request
  processed before it), then the body.
- **Lengths:** at most 256 KB a message (announced at setup). An image sends larger ones in
  pieces.

**Setup:**
- **The client sends:** the magic `BMDSGUI\0`, the protocol version it speaks (major, minor), and
  authentication (a name and data: none locally, `cookie` and 32 bytes by TCP).
- **The server answers:**
  - accepted or refused (and why);
  - the version it will speak;
  - the client's id base and mask;
  - the largest message;
  - the extensions it has, by name, each with its first opcode and event code;
  - the display: the screen's size, the work area, the palette's roles and colours, the theme's
    name, and the font's cell size.
- **Versions:** the minor version grows as requests are appended; the major never changes once
  shipped (a new major would be a new protocol). An extension is optional and named
  (`CANVAS-SURFACE`, `CLIPBOARD`, `SERVICES`, `SHELL`, `REMOTE-FILES`), so a client asks before
  it uses one.

**Ids** (X11's lesson):
- **The client chooses** every object's id from its range (base | n, n within the mask), so
  `Create` needs no reply. The client library hands them out and reuses freed ones.
- **What has an id:** windows, widgets, menus and items, timers, images, dialogs, the clipboard's
  offers.
- **Id 0 is none.** An id is the client's only: another client's ids are refused (`BadId`).

**Names** (P2, as X11's atoms):
- **`InternNames(strings)`** answers with a 32-bit number for each name (an atom). One request at
  startup interns every name a client uses: one round trip.
- **An atom stays for the server's life.** All clients share atoms: the same name, the same
  number.
- **Classes, properties, methods, events and the values of enumerations** (`Down`, `Across`) are
  all names.

**Values** (tagged; each 4-aligned):

| Tag | Value | Body |
|---|---|---|
| int | 32-bit signed | 4 bytes |
| long | 64-bit signed | 8 |
| bool | true or false | 4 |
| string | UTF-8 | `uint32` length, the bytes, padding |
| atom | a name's number | 4 |
| id | an object | 4 |
| rect | x, y, width, height | 16 |
| color | 0xRRGGBB, or a palette role's atom | 4 + 4 |
| list | values of one tag | tag, count, the values |
| none | no value | 0 |

**The core requests** (opcodes appended in this order; replies marked ◆):

| Opcode | Request | Does |
|---|---|---|
| 1 | `InternNames(strings)` ◆ | atoms for names |
| 2 | `NameOf(atoms)` ◆ | names for atoms (for messages, debugging) |
| 3 | `Describe(class)` ◆ | a class's table: parent, properties (type, writable), methods (argument tags, result), events (value tags) |
| 4 | `Create(id, class, parent, properties)` | an object, with properties set at once |
| 5 | `Destroy(id)` | an object and its children |
| 6 | `Set(id, property-value pairs)` | properties |
| 7 | `Get(id, properties)` ◆ | their values |
| 8 | `Call(id, method, arguments)` (◆ if the flag asks) | a method; its result only when asked |
| 9 | `Listen(id, events, on)` | which of an object's events are sent (motion, changes as typed: only when wanted, saving the link) |
| 10 | `PutImage(id, width, height, format, offset, pixels)` | an image object, in pieces if large (P11) |
| 11 | `PutPixels(canvas, rect, encoding, data)` | a canvas's pixels sent through the protocol (remote canvases; RLE or raw) |
| 12 | `Sync()` ◆ | answers once every request before it is done: errors before it have been sent |
| 13 | `Bell()` | the system's beep |

**Events:** `code` is the event's atom; the body is the object's id, a timestamp in
milliseconds, and the event's values (a field's text, a row, a key's name and modifiers, a
point). Events carry their data: nothing is left to fetch.

**Errors:** an error message names the failed request's sequence and opcode, an error code
(`BadId`, `BadName`, `BadClass`, `BadValue`, `BadLength`, `BadAccess`, `BadMatch`, `NoMem`,
`BadImplementation`) and the id or atom at fault. The next requests still run, as in X11. The
client library hands errors to a handler: a C program's own; `angel`'s reports the request and
its name with the script line that made it.

**Flow:**
- **Requests:** the client library buffers them and writes when the buffer is full, before
  waiting for a reply or an event, and on `flush()`.
- **Events:** the server merges pointer motion (only the latest waits) and never drops other
  events. A client that leaves more than 1 MB of events unread is disconnected, with the reason
  in the system's log. A remote link can't make the system run out of memory.
- **Ordering:** a connection's requests are done in order, and its events come in order.

**In the system:**
- **Each connection's task** (P4) reads requests, checks them (lengths, ids, names against the
  class tables), runs them on the toolkit as a built-in application's code would, and writes
  answers.
- **Ending:** a closed connection, by its program's exit or a remote client gone, destroys its
  objects.
- **Limits:** ids per connection (65536 by the mask), windows per connection (64), images'
  bytes per connection (16 MB, PSRAM).

**The client library** (`libbmdsgui`, the SDK):
- **Portable C99:** the transport is the only part that differs by platform (`gui_open` on BMDS,
  a TCP socket elsewhere), so it builds for Linux, macOS and Windows as well.
- **Generated wrappers:** typed C functions per class are generated from the class tables
  (`bmds_button_new(conn, parent, "OK")`, `bmds_label_set_text(...)`), over generic `set`,
  `get` and `call` for anything newer than the wrappers.

### 4b. Classes, properties, methods and events by name (P2)

- **A table per class** in the system: its parent class, its properties (each with a type and
  whether it can be written), its methods (their argument types), and its events.
- **`Describe(class)`** (§4a) returns the table. Angel builds its bindings from it at start, so a
  widget added to the toolkit is in angel too with no angel change. The API lock still lists
  them, so additions are deliberate.
- **The first classes** (the toolkit has them all already):
  - **Windows:** `Window` and `Dialog`.
  - **Layouts:** `Box` (across or down), `Form` (labels and fields), `Grid`, `Splitter`, `Tabs`,
    `ScrollArea`.
  - **Controls:** `Label`, `Button`, `CheckBox`, `RadioButton`, `LineEdit`, `NumberField`,
    `Slider`, `ComboBox`, `ProgressBar`.
  - **Views:** `ListView`, `TreeView`, `DetailsView`, `IconView`, `TextEdit` and `CodeEdit`
    (with a language by name: the syntaxes), `TerminalWidget` (a program in a pty).
  - **Menus:** `MenuBar`, `Menu`, `MenuItem`.
  - **Bars:** `StatusBar` and `ToolBar` (new: buttons with icons).
  - **Drawing:** `Canvas` (P3).
  - **Dialogs:** `MessageBox`, `FileDialog`, `ColorDialog`.
  - **Others:** `Timer`, `TrayIcon` (the tray protocol, `mediaplayer.md` §6e: icon, tooltip,
    menu, clicks), `Notification` (P13: title, body, icon, urgency, timeout, up to three
    actions, replacing an earlier one; events activated, an action, dismissed, expired),
    `Clipboard` (typed: `clipboard.md`'s offers and requests, on request too).
  - **Menus anywhere:** `Menu.popup(x, y)` opens a menu at a point (a right click's), as the
    window manager's `showMenu` does for built-in applications.
- **Names** are lower camel case, as the toolkit's methods are (`text`, `checked`, `currentRow`,
  `clicked`, `changed`), so the C++ and the ABI read alike.

### 4c. Windows, for programs and built-in applications alike

- **What a window gains:**
  - resized by the user, its layout following, within its minimum and maximum sizes;
  - its level: normal, dialog (above its owner, optionally modal), popup or tool;
  - its icon, from the theme or the program's own icon notes;
  - its placement read and set (`WindowPlacement`, `editor.md` §18), so any program remembers
    where it was;
  - a close that is asked (P5);
  - drops of the clipboard's types (`clipboard.md` §5).
- **What a program may do to its own windows** (properties and methods of `Window`, every
  client), the window manager's calls that built-in applications use today:
  - `minimize()`, `maximize()`, `restore()`, `raise()` (and take the keys), and their state as
    properties and events (`minimized`, `maximized`, `active`);
  - `fullScreen` (Alt+Enter's), `above` (always on top), `shaded` (rolled up to its title bar);
  - `title`, `icon` and `largeIcon` (an image id or a theme icon's name), `decorations` (which of
    the frame, the title bar and its buttons it has);
  - the pointer: its shape over the window or a widget, hidden while typing, locked (relative
    motion for games and 3D views), and where it is;
  - `windowMenuItem`: one item of its own in the window's menu (`setWindowItem`), its choice an
    event;
  - starting a drag of the clipboard's types from a widget (`startDrag`), and the drop's outcome
    as an event.
- **A `Canvas`** (P11): a picture the program draws, at the widget's size (made again when it
  resizes, with an event saying so).
  - **Locally** (the `CANVAS-SURFACE` extension): the system call `gui_canvas_surface(conn,
    id)` gives a surface descriptor for the canvas's buffers. The program draws into them with
    the CPU or the 2D-DMA (`BMDS_SURFACE_BLIT`) and presents them, as a game does today. The
    window manager copies the presented picture where the widget is when it paints.
  - **Remotely:** `PutPixels` of the damaged rectangles, run-length coded. The service keeps the
    canvas's picture, so a repaint costs the link nothing.
  - Its pointer, key and wheel events come as protocol events.
  - A `Canvas` costs a buffer, as any program-drawn picture does; only it does.

### 4d. Angel's bindings

```angelscript
App app;                                   // the program's connection; one per script
Window w("Notes", 480, 360);               // a GUI window (its name settled at step 4: §4d)
Box column(w, Down);
TextEdit text(column);
Box row(column, Across);
Button save(row, "&Save");
Button quit(row, "&Quit");
save.onClick(function() { writeFile(path, text.text); });
quit.onClick(function() { app.quit(); });
w.onClose(function(Window@ w) { return !text.modified || ask("Save first?"); });
app.every(1000, function() { status.text = "" + text.lineCount + " lines"; });
app.run();                                  // every window and timer until the last closes
```

- **Classes** come from the system's tables (§4b): each a script type with its properties as
  properties (`text.text = "..."`, the accessor mode angel already uses), its methods as methods,
  and its events as `onName(callback)`.
  - A callback is a funcdef taking the event's arguments: a global function, a delegate to an
    object's method, or an anonymous function.
- **One loop:** `App.run()` waits on the GUI connection, every surface `Window`, the timers and
  the script's sockets together.
- **Remote scripts:** `angel` built for Linux has the same bindings over TCP. With
  `BMDS_GUI_DISPLAY=localhost:6400` and an SSH tunnel, a script edited on a PC runs there and shows
  its windows on the board.
- **Keeping it gentle for beginners:** `alert(text)`, `ask(question)` (yes or no),
  `askText(prompt)` and `chooseFile()` are one-line dialogs that wait for the answer.
- **Names:** today's `Window` is a game's surface. For the GUI window's type, either:
  - `Window` becomes the GUI one, and the game surface is renamed (`Screen` or `GameWindow`).
    Not released yet, so no compatibility is owed (the user's rule).
  - Or the GUI one is `Form`.

  To settle with the user at step 4 (§7).
- **The debugger** works as it does: callbacks run on the script's context, nested, as `Game`'s
  methods do.
- **The guide** grows:
  - lesson 15, a window with buttons;
  - lesson 16, a small application (a notes window that saves);
  - lesson 17, two windows that talk to each other.
- **The examples** grow: a calculator, a notes application, a paint program with a tool palette
  window.

### 4e. The rest of the system for angel

Each binding is an addition to the API lock, on system calls that exist today unless marked
**new**.

| Service | Today in angel | Bindings to add |
|---|---|---|
| Settings | `setting(key)` | `Setting` read, write, save, declare an application's own, and watch (a callback) |
| Notifications | `notify(title, text)` | `Notification` (the protocol's, P13): actions, an icon, replaced or closed, its clicks as callbacks |
| Other programs | none | `Service` (§4h): register a name, send, receive as a callback, watch names |
| The shell | none | `Shell` (§4i), for scripts allowed it: the window list, other windows acted on, a panel's strip, global hotkeys |
| Clipboard | (`/dev/clip0` as a file) | `Clipboard.text` now, through the file; typed offers and requests with the service (§4b) |
| Gamepads and actions | none | `Event` gains pads and actions on a game's `Window`; `pad(n)` state |
| Processes | `run`, `output` | `Process`: started with pipes, read and written, waited for, signalled, the exit status |
| Timers | `sleep`, `millis` | `App.after`, `App.every` (the loop's) |
| Files | read, write, list | watching a folder for changes (**new**, if the kernel has no watch for programs yet: `files.md`), file types and their applications (`filetypes.md`) |
| Audio | `Sound`, `playMusic`, `tone`, `noteOn` | `AudioPlayer` (decoders, streams, position, tags), the mixer's volume, MIDI ports in and out |
| Speech | `say` | voices, rate and pitch, done-speaking callbacks |
| The system | none | statistics (CPU, memory), the network's state and changes, the date in a time zone, the user and the host's name |
| Application folders | none | `appDir()`, `dataDir()` (`apps.md`) |

**Not planned:** threads in scripts. AngelScript contexts on many threads need care everywhere
(the engine, the garbage collector, the bindings). Timers, callbacks and non-blocking sockets
cover what a script needs without them.

### 4f. ABI

- **New system calls (appended):** `gui_open(display, flags)` (a connection descriptor) and
  `gui_canvas_surface(conn, id)` (a canvas's surface, the `CANVAS-SURFACE` extension). Everything
  else is the protocol.
- **Frozen once shipped:**
  - the protocol: framing, setup, the core requests and their bodies, the value tags, the error
    codes, the event message, the extensions' names and contents (`CANVAS-SURFACE`,
    `CLIPBOARD`, `SERVICES`, `SHELL`, `REMOTE-FILES`);
  - the settings `gui/shell-clients` and `gui/shell-remote`;
  - each class's name, parent, properties, methods and events, and what each means;
  - the two system calls, `BMDS_GUI_DISPLAY`'s forms, and the TCP port's setting.

  All only grow: requests, tags, error codes, extensions, classes and names are appended.
- **The SDK:** `bmds/gui.h` and `libbmdsgui` (§4a), portable.
- **Tests:**
  - **The protocol, on the host:** the codec both ways (every tag, framing, lengths, padding),
    and recorded sessions replayed byte for byte against expected answers.
  - **The C library:** `guidemo` on the board, locally and through a TCP connection from the PC.
  - **Angel:** the API lock and corpus scripts that build windows on the host (the host's
    toolkit draws into a picture, as today's `Window` does).

### 4g. Remote programs (P9, P12)

- **Listening:**
  - The setting `gui/listen` (`off` by default, or `loopback`) and the setting `gui/port`
    (6400) open a TCP listener on 127.0.0.1 only.
  - A program on a PC reaches it through Dropbear: `ssh -L 6400:localhost:6400 root@board`,
    then `BMDS_GUI_DISPLAY=localhost:6400`. A program on another board does the same with
    `dbclient`.
- **The cookie:**
  - A connection by TCP presents 32 random bytes kept in `~/.config/gui/cookie` on the board,
    made at the first `loopback` listen and readable only by the user. The client library reads
    it from `BMDS_GUI_COOKIE` or a file named there.
  - Another local user, if there ever is one, or anything that reaches the loopback without
    the file, is refused.
  - Local connections (`gui_open`) need none: they are the board's own programs.
- **What works the same remotely:**
  - windows and their operations, widgets, menus, dialogs, images, canvases (sent as pixels);
  - the clipboard and drops (they are the display's, as in X11);
  - notifications (`Notification`, P13) and tray icons, shown on the board;
  - the service bus (§4h): a remote program can talk to the board's programs by service name.
- **What a remote program can't, unless the user allows it:** the `SHELL` extension (§4i).
- **What doesn't, at first:**
  - **A `FileDialog` lists the board's files.** The `REMOTE-FILES` extension is reserved for the
    dialog to ask the client to list its folders (events to the client, answers in requests);
    built when remote programs are real (P12).
  - **System services other than the GUI** (settings, sound, processes) are the machine's the
    program runs on, not the display's. A remote `angel` plays sound on the PC. That is the
    model: the protocol is the display's.
- **Speed:** a widget protocol is light. A window built is one batch, and a property change is
  tens of bytes, so an SSH link is enough. Only canvases send pixels, and only their damage.

### 4h. The service bus between programs (`SERVICES`, P14)

The window manager already passes messages between applications by service name
(`registerService`, `sendToService`, `takeServiceMessage`, `watchServices`): the tray is a service
named `tray`, and each `TrayIcon` replies on `tray.<connection>`. The extension gives the same to
every client:

| Request or event | Does |
|---|---|
| `RegisterService(name)` | the client answers to `name` (refused if another has it: `BadAccess`) |
| `UnregisterService(name)` | lets it go (done anyway when the connection closes) |
| `SendToService(name, bytes)` | a message to whoever has `name` (`BadName` if no one) |
| event `ServiceMessage(name, bytes)` | a message to one of the client's names |
| `WatchServices(on)` and event `ServicesChanged(name, present)` | names coming and going |

- **Messages** are bytes of the services' own making, at most 4 KB each. Their format is the
  services' business; the system's own (`tray`) are documented where they are defined.
- **Uses:** a program that runs once (a second start sends its arguments to the first and
  exits), an application's helper processes, a tray host, a media player's remote control.
- **Names:** reverse-domain or the application's id (`apps.md`), so programs don't collide; the
  system's are short words (`tray`, `notifications`).

### 4i. The shell extension (`SHELL`, P15)

What `/bin/panel` and the desktop do through the window manager's own calls today, for any
program allowed it:

- **Other programs' windows:**
  - `ListWindows` ◆: every window's id, title, level, state, icon and the client it belongs to.
  - `WatchWindows(on)` and the event `WindowsChanged` (opened, closed, titled, focused,
    minimized, restored, maximized, its icon changed), as the panel's window list follows them.
  - `ActOnWindow(id, action)`: raise, minimize, maximize, restore, close (asked, as the user's
    close is), shade, full screen.
  - `ShowWindowMenu(id, x, y)`: the window's menu, at a point (a taskbar button's right click).
  - The window ids are the window manager's, a separate space from the clients' own ids.
- **The screen:**
  - a window at the panel's level reserving a strip at an edge (`reserve`: the work area
    shrinks, maximized windows follow);
  - a window at the desktop's level (under all others, never in Alt+Tab);
  - the work area, read and watched.
- **The desktop menu:** `SetDesktopMenu(labels)` and the event of a choice (the right click on
  the desktop).
- **Global hotkeys:** `GrabKey(usage, modifiers)` and the event `KeyGrabbed`, through
  `controls.md`'s bindings, so the user can rebind them; refused if another has the chord.
- **Launch feedback:** `ProgramStarting(pid)` shows the starting pointer until the program's
  first window or the timeout, as the panel does when it starts a program.
- **The look:** `SetPalette(roles)` restyles every window (the settings' themes).
- **Screen capture:** `Capture(rect)` ◆ answers with the screen's pixels (`screenshot`'s), sent
  as an image's are.
- **Who may:**
  - the setting `gui/shell-clients`: the programs' paths allowed (by default the system's own:
    `/bin/panel`, `/rom/bin/...`'s shells, none from a card);
  - a remote connection never, unless `gui/shell-remote` is set to `on`;
  - refused requests answer `BadAccess`, and the user can see who holds the extension in the
    settings.
- **Proving it:** the built-in panel could move onto `SHELL` (as Edit could onto the core,
  §7), so that the interface is complete by construction; a panel written in angel would be the
  guide's last example.

## 5. Budget (estimates; measured as steps land)

- **Firmware:** the service is the toolkit plus its interface: the class tables, the id store,
  the codec, the connection stream, the TCP listener. Estimate 50-80 KB.
- **Per connection:**
  - its toolkit task's stack: 8-16 KB, in PSRAM if the measurements allow, as the window
    manager's callbacks never run on it;
  - its connection's queues: 32 KB (16 KB each way, PSRAM), and its images and canvases;
  - its widgets, as a built-in application's (KB, not MB);
  - nothing per window but a `Canvas`'s buffers.
- **Angel:** the bindings built from the tables, a few KB of code. The ROM's `angel` grows by
  the GUI and service bindings, about 40-80 KB.
- **CPU:** a request is a few dozen bytes encoded, copied through the stream and decoded:
  microseconds. A window of 200 widgets is one write and one wake of the toolkit task. Only
  replies cost a round trip, and a client asks for few.
- **The client library:** about 20 KB in a program (codec, buffering, the generated wrappers).
- **The service bus:** messages are queued per receiver (at most 64 KB waiting, then the sender
  is told `NoMem`); nothing per name beyond the window manager's table.

## 6. Steps

1. **The protocol's spine:**
   - the connection stream and `gui_open`;
   - setup, ids, `InternNames`, `Create`, `Set`, `Get`, `Call`, `Destroy`, `Listen`, `Sync`,
     events and errors;
   - the connection's toolkit task, cleanup, limits;
   - `Window`, `Box`, `Label`, `Button`, `LineEdit`;
   - `libbmdsgui` with its codec;
   - host tests of the codec and of replayed sessions;
   - `guidemo` on the board.
2. **The classes:** the rest of §4b; menus and dialogs; `Describe`; the generated C wrappers.
   Windows resized by the user, levels and modality, placement, the window operations of §4c
   (minimize to the window-menu item, pointer and drags), the asked close (P5), also given
   to built-in applications (Edit's close then asks in place).
3. **Pictures and the clipboard:** `PutImage`; `Canvas` with `CANVAS-SURFACE` and `PutPixels`;
   the `CLIPBOARD` extension (offers, requests on demand, drops), meeting `clipboard.md` C14 and
   D13.
4. **Between programs and the shell:** `Notification` and `TrayIcon`; the `SERVICES` extension;
   the `SHELL` extension and its settings; a small panel in C from a card, doing what
   `/bin/panel` does, as the test that nothing is missing.
5. **Angel's GUI bindings:** `App`, the classes from the tables, callbacks, timers, the one loop,
   `alert`, `ask` and the rest; the window names settled (§4d); examples (calculator, notes,
   paint with a palette).
6. **Angel's service bindings:** §4e's table, in the order the guide wants them (settings,
   notifications, processes, timers first).
7. **The guide:** lessons 15-17; Help and completion pick the new names up from `--api`.
8. **Remote programs:**
   - the TCP listener (`gui/listen`, `gui/port`) and the cookie;
   - `libbmdsgui` and `angel` built for Linux;
   - a script on the PC shown on the board through an SSH tunnel, measured;
   - `REMOTE-FILES` decided then (P12).
9. **Documents:**
   - `docs/ABI.md`'s rows;
   - the protocol's reference (`docs/gui-protocol.md`: every message, as X11's protocol
     document);
   - `gui.md` G1 revised; `windows.md` §6 and `clipboard.md` C14 pointed here.

Each step ends tried on the board and with its measurements (§5's estimates replaced).

## 7. Open questions

- **The window types' names in angel** (§4d): `Window` for GUI and `Screen` for games, or
  `Form` for GUI.
- **The panel through `SHELL`:** whether `/bin/panel` moves onto the extension once it exists
  (§4i), so the system's own shell proves it.
- **Built-in applications through the ABI:** whether some move onto it, so the interface is
  proven by the system's own use. Edit is the natural first; it would cost it nothing in speed,
  since the same toolkit draws.
- **Styling:** whether programs may set colours and fonts per widget, or only the system's
  palette (today's built-in applications use only the palette).
- **A `Canvas` scaled** like a game's surface (pixel art in a GUI window), or always at scale 1.
- **The port's number:** 6400 is a placeholder (X11's 6000 + n is taken by X); any free port will
  do, and it is a setting.
- **Compression on remote links:** run-length for canvases at first; `zlib` (the system has
  `inflate` already, `deflate` for PNGs) if a measured canvas over SSH needs it.

## 8. Step 1 design: the protocol's spine (2026-10-22)

**Pieces and where they live:**

| Piece | Where | What |
|---|---|---|
| The protocol's constants and codec | `bmds_core`: `bmds/guiprotocol.h` (pure, host-tested) | opcodes, kinds, tags, error codes; a writer and a reader of messages and values over byte buffers |
| The connection stream | `bmds_kernel`: `bmds/streampair.h` (host-tested) | two ends, each reading one ring and writing the other; readiness and hang-up as a pipe's; 16 KB each way, PSRAM |
| `gui_open` | `bmds_loader`: `guicalls.cpp`, slot 124 | calls the opener the service registered at boot (`bmds/guiopener.h`: the toolkit depends on the loader, so the loader can't call it directly); the program gets its end |
| The service | new component `bmds_guiservice` (`GuiService`, `GuiConnection`, the class tables) | boot stage `gui`: registers the opener. A connection is two threads: a reader (frames requests, queues them, wakes the loop) and the loop (`GuiApplication::exec()`: runs requests, writes answers and events) |
| The client library | `sdk/gui/gui.c`, `bmds/gui.h` → `libbmdsgui.a`, linked by `bmds-cc` as `libbmdsgame` is | C99; the transport `gui_open()` on BMDS, any descriptor on a host (`bmds_gui_connect_fd`: the host tests' socketpair now, TCP at step 8) |
| `guidemo` | `sdk/examples/guidemo.c` | a window: a label, a field, two buttons; the field's text shown in the label on Enter or a click |

**The wire, as built at step 1** (§4a's, made exact; little-endian, 4-aligned):
- **Setup:** the client's first message is `Hello` (opcode 0): `"BMDSGUI\0"`, `uint16 major`
  (1), `uint16 minor` (0), `uint32` the authentication's name length, `uint32` its data's,
  then both, padded. The answer is a reply to sequence 0:
  - `uint32 status` (0 accepted; 1 bad version, 2 refused, 3 busy);
  - `uint16 major`, `uint16 minor`, `uint32 idBase`, `uint32 idMask`, `uint32 maxMessage`;
  - the extensions (a count, then each: its name, a string value, and its first opcode and
    first event code);
  - the display as a property list (`screen`, `workArea`: rect values), so more is appended
    without changing the layout.

  Requests after it are numbered from 1.
- **Ids:** `idBase` is the connection's slot + 1 shifted left by 24; `idMask` is 0x00FFFFFF.
- **Events listened to by default:** `clicked`, `accepted`, `closed`. `edited` (each change of
  a field) only after `Listen`.
- **Step 1's classes:**

  | Class | Properties | Methods | Events |
  |---|---|---|---|
  | (all widgets) | `enabled`, `visible` | `setFocus` | — |
  | `Window` | `title`, `width`, `height` (the client area at first) | `show`, `close` | `closed` (the window manager closed it; asked at step 2) |
  | `Box` | `direction` (`across`, `down`), `spacing`, `margin` | — | — |
  | `Label` | `text` | — | — |
  | `Button` | `text` | `click` | `clicked` |
  | `LineEdit` | `text`, `masked`, `maxLength` | `selectAll` | `accepted` (Enter, the text), `edited` (the text) |

  A widget's parent is a `Window` (its column of children) or a `Box`. Children are placed in
  the order made.
- **Output:** the loop writes answers to the stream, waiting at most 10 s for room. A client that
  reads nothing for that long is disconnected (step 1's form of §4a's 1 MB rule).
- **Input:** the reader holds at most 256 KB of unread requests, then waits for the loop.

## 9. As built: step 1 (2026-10-22)

**What exists:**
- **The protocol:** `bmds/guiprotocol.h` (the constants, a writer and a reader) and `sdk/gui/gui.c`
  (the client) speak §8's bytes.
  - The host tests check the codec's every tag both ways, and the lengths and bad values it
    refuses.
  - They also run the C client against a scripted service speaking through the C++ codec, on a
    socketpair: setup, interned names, a batch, an error that comes later, an event queued while a
    reply was awaited, a `Get` and a failed one, the service gone.
- **The connection stream:** `StreamPair` in `bmds_kernel` (host-tested: both ways, the ends'
  closes, more than a ring's worth).
- **`gui_open`:** slot 124 in the table and the ABI lock. It asks the service through
  `GuiOpener`, which the service sets at the boot stage `gui`.
- **The service:** `bmds_guiservice`.
  - **Atoms:** one table for every connection, made at boot (no global constructor).
  - **A connection:** a loop thread running its `GuiApplication` (16 KB stack, PSRAM) and a reader
    thread (4 KB), both kernel threads.
  - **Objects:** kept in a list sorted by id. Removing one removes its children, then the
    toolkit's side.
  - **Layout:** after each round of requests, the touched windows' boxes are laid out again.
- **The classes:** `Window` (a column of children, centred in the work area at its size or its
  children's), `Box`, `Label`, `Button`, `LineEdit`, as §8's table.
- **`libbmdsgui`:** linked into SDK programs by `bmds-cc` and `bmds_add_program`.
- **`guidemo`:** in `sdk/examples`.

**Measured:**
- **Firmware:** `bmds_guiservice` 11.8 KB of flash.
- **A program:** `libbmdsgui` adds 6.1 KB (text and data); `guidemo.elf` is 188 KB with musl, its
  relocations and symbols (`programs.md` §20).
- **Memory:** twelve connections opened and closed after the first six: internal RAM unchanged,
  PSRAM within 9 KB, no trend. A connection's own cost is its two stacks (20 KB, PSRAM), its
  stream (32 KB), its application and objects.
- **Speed:** the window, six objects and their properties go as one batch before the first event
  is waited for. It is on the screen as the program starts.

**Tried on the board:**
- `guidemo` from `/tmp`: its window drawn as a built-in application's.
- A name typed and Enter: `accepted`, then a `Get` of the field and a `Set` of the label
  ("Hello, Ada!" on the screen and on its stdout).
- Alt+Q (the Quit button's mnemonic): `clicked`, and the program ended, its window gone.
- The window's close button: `closed`, and the program ended.
- `kill -9` of the program: its window went with its connection.

**Found on the board:**
- **`Thread::detach()` must not be called at a thread's start:** the thread can run before
  `start()` has recorded its handle (an assertion, then a reset, on the first try). The loop now
  waits for `start()` to finish (a semaphore) and detaches as it ends.
- **The variable naming the display is `BMDS_GUI_DISPLAY`,** not `BMDS_DISPLAY` as §4a first said:
  `bmds-cc` already uses `BMDS_DISPLAY` at build time, for programs that draw continuously.

**Left for later steps:**
- `Describe`, the other classes and the generated C wrappers (step 2).
- The client's requests ignore `sync()`'s errors past their handler: a program that wants to
  stop at an error checks in its handler.
- Answers are written with a 10 s limit rather than §4a's 1 MB rule (§8).

## 10. Step 2 design: the classes (2026-10-22)

**Built in six parts, each tried on the board:**
- **2a, the schema:** every class, property, method and event in one declarative file,
  `sdk/gui/classes.def` (X-macros). It is the single source of the class ABI:
  - **The service** includes it for the names, the types, the access and the signatures. It binds
    each entry to a function named after it (`set_Label_text`, `call_Window_show`,
    `create_Button`), so a missing implementation is a compile error.
  - **`tools/guigen.py`** reads it to write the C wrappers, `sdk/include/bmds/guiclasses.h`, and
    the lock's lines.
  - **`tests/scripts/gui.lock`** is the class ABI's lock, as `api.lock` is the scripts'. Every line
    must stay; new ones are appended. A host test runs the generator and checks the lock and that
    `guiclasses.h` is current.
  - **`Describe`** answers from the same tables. The connection checks a method's arguments
    against the schema (count and tags) before the class's code runs.
- **2b, windows:**
  - the `resized` event, and `minWidth` and `minHeight`;
  - the operations of §4c: `minimize`, `maximize`, `restore`, `raise`, `fullScreen`, `above`,
    `shaded`, `decorations` (`frame`, `titleBar`, `buttons` booleans), `icon` (a theme icon's
    name), the pointer's shape, `windowMenuItem` and its event;
  - `placement` (`WindowPlacement`'s text) read and set;
  - `owner` and `modal`: a window with an owner is centred on it, and while a modal one is shown
    its owner's widgets are disabled (the window manager has no owner relation: §7).
- **2b, the asked close (P5):**
  - The window manager's three user closes (the close box, the window menu's Close, Alt+F4) call
    the window's `closeRequested` callback if it has one, and close it otherwise, as today.
  - `GuiWindow` passes it to a virtual `closeRequested()`, by default `close()` then
    `closedByManager()`, so built-in applications behave as before. Edit overrides it and asks
    without closing, which replaces its reopen-to-ask.
  - For programs, opt in (X11's `WM_DELETE_WINDOW`): a window whose client listens to
    `closeRequested` is asked; one that doesn't is closed and told `closed`.
- **2c, controls and layouts:** `CheckBox`, `NumberField`, `Slider`, `ComboBox`, `ProgressBar`,
  `Form` (rows of a label and a widget), `Splitter`, `Tabs`, `StatusBar`, `ToolBar`, `Spacer`.
  `RadioButton` waits for the toolkit's check box to draw one.
- **2d, views:**
  - `ListView` (rows of text, icons by theme name), `DetailsView` (columns), `TreeView` (paths,
    children given when expanded), `IconView`;
  - `TextEdit` and `CodeEdit` (`text`, `syntax` by id, `readOnly`, `modified`, `cursor`, events
    `changed` and `cursorMoved`);
  - `TerminalWidget` (`start(path, args)`, the `ended` event with the status).
  - Items are set by methods (`setItems`, `addItem`, `removeItem`, `clear`) with a `current`
    property, and the events `activated` and `currentChanged`.
- **2e, menus, dialogs and timers:**
  - `MenuBar`, `Menu` and `MenuItem` (`text`, `shortcut`, `checked`, `enabled`, `chosen`);
    `Menu.popup(x, y)`;
  - `MessageBox` (`kind`, `title`, `text`, `buttons`; event `answered`);
  - `FileDialog` (`mode`, `title`, `folder`, `fileName`, `addFilter`, `open()`; event `finished`
    with the paths);
  - `ColorDialog` (`color`; `finished`);
  - `Timer` (`interval`, `repeat`, `start`, `stop`; `timeout`).
- **2f, the gallery:** `sdk/examples/guigallery`, every class in one window with menus and
  dialogs, tried on the board; `guidemo` stays the small one.

**2a, as built (2026-10-22):**
- **The schema's lines:** `GUI_CLASS(C, Parent, Concrete|Abstract)`,
  `GUI_PROP(C, name, Tag, RW|RO|WO)`, `GUI_VALUES(C, prop, names...)`,
  `GUI_METHOD(C, name, Result, ArgTags...)` and `GUI_EVENT(C, name, On|Off, ValueTags...)`. On
  means sent unless the client turns the event off.
- **The service** builds flat `constexpr` tables from them, one `#include` each with its own
  macros. A parent is kept as its name ("None": none).
- **`guigen.py`:**
  - writes `bmds_<class>_new`, `_set_<prop>`, `_get_<prop>`, `bmds_<class>_<method>` and
    `BMDS_GUI_<CLASS>_<EVENT>` names, over the new `bmds_gui_new`, `bmds_gui_set_named`,
    `bmds_gui_get_named` and `bmds_gui_call_named`, which intern each name once;
  - checks that parents come first, that tags are known and that no name is made twice;
  - marks the header `clang-format off`, so formatting the tree can't make it stale.
- **`Describe`'s reply:**
  - the parent's atom (0: none) and a creatable flag;
  - the properties: atom, tag, and flags (readable 1, writable 2);
  - the methods: atom, result tag, then the argument count and tags;
  - the events: atom, default, then the value count and tags.
- **A box's direction** can be changed at any time: `BoxLayout::setDirection()` is new. Step 1's
  read-before-create special case is gone, since the wrappers create first and set after.

**2b, as built (2026-10-22):**
- **The window manager:**
  - `WindowConfig::closeRequested`: the close box, the window menu's Close and Alt+F4 call it
    when it is set (`requestCloseLocked`), and close the window otherwise.
  - `setMinimumSize()`: an interactive resize stops at the client area's floor (its width
    rounded up to whole 4-pixel columns).
  - `isShaded()`.
- **`GuiWindow`:**
  - always passes the asked close on as the event `CloseRequested`, to the virtual
    `closeRequested()`. Its default is `close()` then `closedByManager()`, so built-in
    applications behave as before;
  - Edit overrides it: with unsaved work it asks in place (`quit()`), and the window stays
    where it was, maximized or not;
  - new: `setDecorations()`, `setPointer()` (a shape over the whole client area that sets the
    widgets' own aside: a busy window), `setMinimumSize()`.
- **`Window`'s properties:**
  - `minWidth` and `minHeight`, which also floor the first size;
  - `placement` (`X Y W H [maximized]`: where it opens, or moves to while shown);
  - `icon` (a theme name; the 16 × 16 icon is copied, since the theme's cache may drop its own);
  - `pointer` (`arrow` … `starting`);
  - `frame`, `titleBar`, `buttons` and `resizable`, kept as set. Shown, the buttons need the
    title bar and resizing needs the border;
  - `above` and `shaded`;
  - read-only `minimized`, `maximized` and `active`;
  - `owner`, `modal` and `windowMenuItem`.
- **`Window`'s methods:** `minimize`, `maximize`, `restore`, `raise`.
- **`Window`'s events:**
  - `resized(width, height)` (On), also sent once when the window is first shown;
  - `activated(Bool)` (Off);
  - `closeRequested` (Off: the client opts in, as X11's `WM_DELETE_WINDOW`; without it the
    window closes and `closed` follows);
  - `windowMenuChosen` (On).
- **Owners and modality:**
  - A window with an owner opens centred on it.
  - A shown modal window disables its owner's column (a count, so several can stack).
  - An owner activated while blocked passes activation on to its modal window. The window
    manager has no owner relation (§7), so this keeps the box above its owner.
- **`fullScreen` is left out.** The window manager's full screen means a program owning the
  display (games, `setFullScreen`). For a toolkit window it would only cover the work area, so
  it waits until the window manager can cover the panel too.
- **Tried on the board with `sdk/examples/guigallery`, 2b's part:**
  - maximize and restore, with `resized` updating a label;
  - a resize by hand stopped at the minimum;
  - the busy pointer;
  - About from a button and from the window menu's own item;
  - Alt+F4 asking through the program's own modal "Quit?" box: the owner was blocked, and
    clicking it brought the box back up;
  - quitting, which printed the placement;
  - Edit's close: asked in place, Cancel kept it, No closed it, and an unmodified file closed
    at once.
- **Size:** `bmds_guiservice` 17.0 KB of flash after 2a and 2b (11.8 KB at step 1).

**2c, as built (2026-10-22):**
- **Classes:**
  - `CheckBox`: `text`, `checked`; event `toggled(Bool)`.
  - `NumberField` and `Slider`: `value`, `minimum`, `maximum`, `step` (a page is ten steps);
    events `changed(Int)` and, for the slider, `released(Int)` (Off). A slider has a
    `direction`, `across` or `down`.
  - `ComboBox`: `items` (a list, read and set), `current`, `addItem`, `clear`; event
    `changed(Int)`.
  - `ProgressBar`: `value` and `maximum`, both Long (a file's bytes fit).
  - `Spacer`: `size` (its least length), stretching by default.
  - `Form` with `FormSection` (`title`) and `FormRow` (`label`, `help`); a row's widgets go in
    its field, and any other widget made in a form is a row of its own.
  - `Splitter`: `direction`, `position`, `minFirst`, `minSecond`. Its first two children are
    its sides; a third is refused (`BadMatch`).
  - `Tabs` (`current`; event `changed(Int)`) with `TabPage`, a `Box` with a `title`. Only pages
    go in tabs.
  - `ToolBar` (`addSeparator`) with `ToolButton` (`text`, `icon` by theme name, `click`; event
    `clicked`).
  - `StatusBar`: `addField(width) -> Int` and `setField(index, text)`.
  - `Widget.stretch` for every widget.
- **Containers that place their children themselves** (a splitter's sides, tabs' pages) have
  hooks on their object: `adopt` when a child is made in them, `release` before one goes (a
  page's tab removed, a side let go), and `relayout` after a round of requests. The layout pass
  calls the hook instead of assuming every container is a `BoxLayout`.
- **A splitter's sides** are recorded by id when adopted. They are placed at the next layout,
  once they are in the connection's list.
- **New in the toolkit:** `Slider::setOrientation()` and `Splitter::setOrientation()`, for the
  same reason as the box's direction.
- **Icons:** tool buttons' icons and windows' share `GuiIcon`, a copy in two slots. A new icon
  goes in the other slot, and the old one is freed only after the toolkit has the new one.
- **Client library:** `bmds_gui_atom_name()`, an atom's name, from the cache or by one `NameOf`.
  The gallery uses it to show each event in its status bar.
- **Tried on the board with the gallery** (tool bar, tabs, status bar):
  - the Controls page's form: the check box, the number field stepping in twos, the slider
    with the progress bar following it, and the combo box's items and choice;
  - the splitter's handle dragged;
  - the tool buttons' About and Quit.
- **Release hooks, tried with a throwaway program:** it removed a tab page, a splitter side, a
  form row and a tool button, then added a side again, and was refused a third side and a
  non-page in tabs. Destroying the tabs took their pages, and the board stayed up.
- **Size:** `bmds_guiservice` 25.1 KB of flash (17.0 KB after 2b).

**2d, as built (2026-10-22):**
- **`ItemView`** (abstract) is the list views' common part, each with a model of its own:
  - `items` (column 0, a list read and set), `current` (-1: none), `multiSelect` (a list
    view: off; the others: on, as the toolkit's views);
  - `addItem`, `removeItem`, `clear`, and `setIcon(row, name)`, a theme icon by name, small and
    large;
  - events `activated(row)` and `currentChanged(row)`. A change the client made isn't told
    back to it (the object's `quiet`); a row added or removed that moves the current one is.
  - **`ListView`:** a details view of one column, as wide as the view, without a header.
  - **`DetailsView`:** `columns` (their titles, six at most), `addRow(texts)`,
    `setColumnWidth`, `setSortIndicator`; event `sortAsked(column)`. The client sorts and sets
    the rows again.
  - **`IconView`:** the flowing arrangement, 32-pixel icons.
- **Rows' icons:** the connection keeps copies (`GuiConnection::icon()`, 64 at most) for its
  life, since a model holds pointers and the theme's cache may drop its own. The toolkit's
  `ListItemModel` gains `removeAt()`.
- **`TreeView`** works by path, as the toolkit's does:
  - `addRoot`, then `expanding(path)` asks for a row's children, which the client gives with
    `setChildren(path, names)` (none: a leaf);
  - also `setHasChildren`, `expand`, `collapse`, `clear`, `current` (a path) and the events
    `activated(path)` and `currentChanged(path)`;
  - its icons are the theme's drive and folders.
- **`TextEdit`:**
  - `text` (set unmodified; read as saved), `readOnly`, read-only `modified`, `cursorLine`,
    `cursorColumn` (bytes) and `selectedText`;
  - `markSaved`, `insert`, `goToLine`, `selectAll`, `undo`, `redo`, `cut`, `copy`, `paste`;
  - events `changed` (On, no values: the client reads the text when it wants it) and
    `cursorMoved(line, byte)` (Off).
- **`CodeEdit`** adds `syntax` (a language's id: `Syntax::find`), `lineNumbers` and `scheme`.
- **`Terminal` replaces the plan's `start(path, args)`/`ended`:**
  - The client feeds it: `write(bytes)` shows a program's output, and the events
    `typed(bytes)` (the keys and pastes, as a pty would get them) and `sized(columns, rows)`
    go back.
  - The program runs its own child on its own pseudo-terminal.
  - Why:
    - The toolkit's `TerminalWidget` must be owned by a process that waits for its command,
      and the service's threads are not processes.
    - Started by the service, a command would run as the service for any client, a remote
      one too.
    - X11's terminals are the same: xterm runs the shell on its side.
  - **The toolkit's `TerminalWidget` gains a fed mode:** with no command of its own,
    `setTyped()` gets the typing and `setSized()` its size in cells.
- **Tried on the board with the gallery:**
  - **Views:** `/rom` as a tree, its children read by the program as it opened, and the chosen
    folder's entries in a details view with sizes and icons.
  - **Text:** C coloured with line numbers; typing updated the cursor and modified label.
  - **Terminal:** `/bin/sh` on the gallery's own pty (`openpty`, `vfork`, `setsid`,
    `TIOCSCTTY`) ran commands with colours, and `stty size` followed the window (34 × 153
    maximized, 11 × 49 restored).
  - **Layouts:** a list view and an icon view; clicking and double-clicking showed
    `currentChanged` and `activated`.
  - Quitting hung up the shell, and both processes were gone.
- **Size:** `bmds_guiservice` 33.7 KB of flash (25.1 KB after 2c).

**A window's padding (2026-10-22, the user's report):** a program's window had an 8-pixel
margin and 4-pixel spacing around its column, unlike the built-in applications. The file browser,
Settings and Edit keep their column flush with the frame, so their menu, tool and status bars
touch its edges and only their content areas pad themselves.
- A client window's column is now flush by default.
- New write-only `Window.margin` and `Window.spacing` pad plain widgets (guidemo's: 8 and 4).
- A `TabPage` is a plain box (no margin of its own), and a page of plain widgets sets its
  `margin` (the gallery's Windows page).
- Checked on the board beside a folder window: the gallery's status bar and tool bar now sit as
  the file browser's.

**2e, as built (2026-10-22):**
- **Menus:**
  - `MenuBar` (a window's own: it is made in a window, and F10 opens it), `Menu` (`title`,
    `addSeparator`, `popup(x, y)` in its window's client area) and `MenuItem` (`text`,
    `shortcut`, `checked`, `enabled`, `icon` by theme name; event `chosen`).
  - A menu is made in a window (a popup), a menu bar, or a menu (a submenu).
  - **A menu object keeps its entries in order** (items, separators as 0, submenus). It builds
    the toolkit's menu again only when something changed, just before it opens (a bar's
    `AboutToShow`, `popup`), its submenus first. A bar builds its titles again in the layout
    pass.
  - **A child going** clears the toolkit's menu or bar at once, before it is freed (the
    `release` hook, now called for every object with a parent, not only widgets).
  - **Shortcuts are keys:** the text ("Ctrl+Shift+S", "F5", "Del", "PgUp"...) is parsed into a
    HID usage and modifiers. A key no widget used (`ClientWindow::keyUnused`) chooses the
    window's enabled item with that shortcut.
  - **New in the toolkit:** `MenuBar::setTitle()` and `MenuBar::clear()`.
- **`MessageBox`:** `kind`, `title`, `text`, `buttons` (atoms), `open`; event
  `answered(Atom: ok, cancel, yes, no)`.
  - The toolkit's message box freed itself, unreachable once shown. It gains `open()`, which
    returns the box, and `abandon()`, which closes and frees it without telling an answer.
    `show()` wraps `open()`.
  - An unanswered box goes with its object, or with the connection.
- **`FileDialog`** (`mode`, `title`, `folder`, `fileName`, `addFilter`, `open`; event
  `finished(Bool, List paths)`) and **`ColorDialog`** (`title`, `color`, `open`; events
  `finished(Bool, Color)` and `changed(Color)`, Off). Their toolkit dialogs take their mode
  and title in the constructor, so the object keeps what was set and makes the dialog at
  `open`; it is freed with the object.
- **`Timer`:** `interval` (10 ms at least), `repeat`, read-only `active`, `start`, `stop`;
  event `timeout`. It uses the connection's application's timers, which its widgets share (16).
- **Dialogs aren't owned** by a program window yet: they are above-level windows centred in the
  work area, as the toolkit's are for built-in applications. Ownership of the toolkit's dialogs
  waits for the window manager's owner relation (§7).
- **Tried on the board with the gallery** (a menu bar, and a Dialogs tab):
  - **File ▸ Open:** the file dialog with the program's filters returned the path typed.
  - **Shortcuts:** Ctrl+O opened it and Escape cancelled; F1 opened About.
  - **View ▸ Status bar:** toggled its check and the bar.
  - **The message box** answered `yes`.
  - **The colour dialog** opened with the program's colour and returned the swatch chosen.
  - **The popup menu** opened at its place, and its items were chosen.
  - **The timer** counted seconds and stopped.
  - **Killed with a box open and the timer running:** the box went with the connection.
    Internal RAM came back exactly over six such runs.
- **Leak checks, corrected afterwards (the user asked to chase the leak):** the `Running` and
  `Pipe` blocks `mem new` listed were the measuring ssh session's own objects, not leaks. Its
  shell is a new process since the mark, and dropbear gives it two pipes. Measured in one
  session, nothing in the GUI service or the loader leaks: five gallery kills with a box open
  and the timer running cost 280 bytes, which was the measuring shell's job list. That job list
  was the real leak: a script's shell kept every ended background job (`devplan/processes.md`
  §3.6.2).
- **Size:** `bmds_guiservice` 41.9 KB of flash (33.7 KB after 2d).

**2f, as built (2026-10-22): step 2 closed.**
- **The gallery uses every class:**
  - a line edit (its `accepted` text in the status bar);
  - a spacer (Quit at the right of its row);
  - a plain `TextEdit` under the `CodeEdit` (a splitter, down);
  - a submenu (View ▸ Pointer, its items checked in turn);
  - the details view sorted by the column whose header is clicked (again: the other way).
- **`sdk/tests/guitest.c`**, the service's board test (run it on the board; it prints
  `guitest: ok`). It checks:
  - **Describe** of all 36 classes (which can be made), entries' tags, and the parents;
  - **every error a bad request gets:** `BadClass`, `BadMatch` (a widget with no window, a
    window in a window, a bar in a box, an item in a window, a third splitter side, a non-page
    in tabs), `BadValue` (a value not among a property's names, wrong arguments, a shortcut
    that isn't one, a 1 ms timer), `BadAccess` (a write-only property read, a popup before its
    window shows), `BadName`, `BadId`;
  - **events from methods** (a button's click), an event turned off, the client's own changes
    not told back, a removed row moving the current one;
  - **a one-shot timer** firing once;
  - **containers letting go** of their children;
  - **100 windows** with children made and destroyed.
  - Run five times in one session: 36 bytes of PSRAM, none internal.
- **`bmds_gui_describe()`** in libbmdsgui: a class's raw `Describe` body, its layout documented
  in `gui.h` (for tools and angel's bindings, step 5).
- **Fixed while testing:**
  - **A submenu's items did nothing.** The toolkit tells the *top* menu of a tree the number of
    the item chosen, so numbers must be unique across the tree. Items were numbered by their
    place in their own menu; they are numbered by their object id now (its 24 bits within the
    connection).
  - **`removeItem` before the current row** kept the current index, so the current item became
    the next one. The current row now follows its item.
- **Size:** `bmds_guiservice` 42.0 KB of flash.
- **Step 2 as a whole:** 36 classes in `sdk/gui/classes.def`, locked in `tests/scripts/gui.lock`;
  C wrappers generated; the service 11.8 KB → 42.0 KB of flash. Next: step 3 (§6), pictures
  and the clipboard (`PutImage`, `Canvas`, the `CLIPBOARD` extension).

## 11. Step 3 design: pictures and the clipboard (2026-10-22)

**Classes, as step 2's were.** Images, canvases and the clipboard are classes in
`sdk/gui/classes.def`, so they get the generated wrappers, the lock and `Describe` for free.
- **Bulk pixels** travel by the two reserved requests, `PutImage` (10) and `PutPixels` (11), since
  a tagged value has no binary type.
- **The extensions** named at `Hello` (§4a) say what a client may use: `CLIPBOARD` (value "1"),
  and `CANVAS-SURFACE` on local connections only. Their first opcode and event code are 0: they
  are classes, not opcodes of their own.

**Built in five parts, each tried on the board:**
- **3a, images:**
  - The `Image` class (no parent; read-only `width`, `height`).
  - `PutImage(id, width, height, format, offset, count, pixels)`: format 1 is ARGB8888 (a
    `uint32` 0xAARRGGBB a pixel, as `Icon`). The pieces of a large image are sent in turn
    (`offset` its first pixel); the first sets the size, at most 512 × 512.
  - **Where an image goes:** `Window.iconImage`, `Button.iconImage`, `ToolButton.iconImage`,
    `MenuItem.iconImage` and `ItemView.setIconImage(row, image)`, beside the theme names of
    step 2.
  - **Its pixels are a counted block.** Every user holds a reference, so an `Image` destroyed
    while shown stays until the last user lets go, and the client's id is free at once.
- **3b, `Canvas` through the protocol (`PutPixels`):**
  - A widget whose picture the client draws: read-only `width` and `height` (its size from the
    layout); `preferredWidth` and `preferredHeight` (its hint); event `resized` (On).
  - Input as events: `pressed`, `released` and `moved` (x, y, buttons), `wheel` (notches), and
    `key` (usage, modifiers, down, text). It takes the focus, so keys come to it.
  - `PutPixels(canvas, x, y, width, height, encoding, data)`: the service keeps the picture in
    the display's format, so a repaint costs the client nothing. Encoding 1 is raw RGB565; 2 is
    run-length (`uint16` count, `uint16` pixel).
  - This works for any client, local or remote.
- **3c, `CANVAS-SURFACE`** (local only):
  - The system call `gui_canvas_surface(conn, canvas)` gives a surface descriptor for a canvas,
    used with the surface calls a game uses (buffer, acquire, present, the 2D-DMA blits).
  - The canvas paints the last presented buffer. The loader's surface calls then take a surface
    interface, which `ProgramSurface` and the canvas both implement, instead of
    `ProgramSurface` itself.
- **3d, `CLIPBOARD`:** the `Clipboard` class (no parent).
  - **Selection values:** `clipboard`, `primary`.
  - **Copying:** `copyText(selection, text)`; `offer(selection, types)` offers types the client
    gives on request. The event `requested(request, selection, type)` asks for one; the client
    answers with `answer(request, data)`, then `finish(request)` (an empty answer: none).
  - **Also:** `clear(selection)`, the event `lost(selection)`, and `paste(selection, type)`,
    answered by the event `pasted(selection, type, data)` or `pasteFailed(selection, type)`.
  - **How the service does it:** it is the owner to the system's clipboard. A request's pipe end
    is kept until the client finishes, written as the answers come. That is `clipboard.md` C14's
    "requests become events".
- **3e, drops** (`clipboard.md` D13, for programs):
  - `Widget.dropTypes` (a list): a widget takes drops of those types.
  - The event `dropped(type, data, x, y)`, its data read by the service.
  - Programs starting drags of their own wait for a later step.
- **Budget:** an image's pixels and a canvas's picture are the only new memory. They come from
  PSRAM, freed with their object, a canvas's made again at its size.

**3a, as built (2026-10-22):**
- **`Image`** objects, their pixels sent by `PutImage`:
  - The body is id, width, height, format (1: ARGB8888), offset, count, then the pixels.
  - A piece at offset 0 sets the size, at most 512 × 512.
  - `bmds_gui_put_image()` sends 32768 pixels (128 KB) a piece, flushing each.
- **The pixels are a counted block (`GuiImage`).** The object holds one reference; a window, a
  button, a tool button and a menu item hold one each (`GuiObject::image`), and a view one for
  each image its rows show (`GuiObject::images`).
  - A view lets go of images no row shows any more (on every change of its rows and
    `setIconImage`).
  - So an image destroyed while shown stays until the last user lets go, and its id is free at
    once.
- **Users:** `Window.iconImage`, `Button.iconImage`, `ToolButton.iconImage`, `MenuItem.iconImage`
  and `ItemView.setIconImage(row, image)`. Rows show it at its own size (16 × 16 for lists,
  32 × 32 for icons). A theme name and an image replace each other.
- **Tried:** guitest checks sizes read back, the errors (too big, not an image, no pixels yet,
  under a window), destroying while shown by a button, the window, a menu item and rows, and a
  512 × 512 image in 8 pieces; five runs cost 1.2 KB of PSRAM. The gallery draws a diamond
  (16 and 32 pixels, transparent corners) shown on a tool button, a list row and the icon view.

**3b, as built (2026-10-22):**
- **`Canvas`:** a widget holding an RGB565 picture at its size, made again (white) when the
  layout resizes it, with the event `resized(width, height)`.
  - It paints with `Painter::pixels565`, so a repaint costs the client nothing.
  - Read-only `width` and `height`; `preferredWidth` and `preferredHeight` as its hint.
  - Input: `pressed`, `released` (x, y, button), `moved` (x, y, buttons; Off), `wheel`, and
    `key(usage, modifiers, down, text)`. Tab and Alt still go to the window. It takes the focus
    on a press.
- **`PutPixels`:** canvas id, x, y, width, height, encoding (1: raw RGB565; 2: run-length,
  `uint16` count and pixel), the data's bytes, the data.
  - A rectangle drawn for a size the canvas no longer has is cut to its picture, not refused:
    the client can't know of a resize before its event.
  - `bmds_gui_put_pixels()` codes each band run-length when that is smaller, in bands under the
    largest message.
- **Measured on the board** (a 640 × 480 canvas, a local connection):
  - At first a raw frame took 270 ms (2.3 MB/s). The connection's reader copied the stream a
    byte at a time, twice. Now it copies whole reads (`List::append(Span)`, new, with
    `truncate()` dropping trivially destructible elements by the size alone): 89 ms (6.9 MB/s)
    raw, 51 ms run-length.
  - The rest is the stream's copies (a 16 KB ring), which 3c's local surfaces avoid.
- **Tried:**
  - guitest checks the size told on layout, raw and run-length pixels, a rectangle outside the
    picture (dropped) and partly outside (cut), and `BadMatch` for a non-canvas.
  - The gallery's Paint tab draws with the mouse (each motion's dots, only the changed
    rectangle sent), its brush from the colour dialog. The picture stayed across tab switches.
- **Size:** `bmds_guiservice` 46.6 KB of flash after 3a and 3b (42.0 KB after step 2).

**3c, as built (2026-10-22):**
- **The system call:** `gui_canvas_surface(conn, canvas, config)`, slot 125
  (`bmds_gui_canvas_surface` in `bmds/sys.h`). It takes `bmds_surface_config_t`, as
  `surface_open` does:
  - `width` and `height` (both 0: the canvas's size now);
  - `format`: RGB565 or BGR24 (the painter draws both at 1:1; L8 has no palette here);
  - `buffers`: 2 or 3; `flags`: `BMDS_SURFACE_FAST` alone. The title and aspect are ignored.
  - It answers an fd, close-on-exec, for the surface calls a game uses: buffer, acquire, present,
    and `surface_control`'s `IMAGE`, `BLIT`, `WAIT` and `INFO`. A title, a palette or a pointer
    mode is `EINVAL`, and the fd has no events: the canvas's come by the protocol.
  - `libbmdsgui`'s `bmds_gui_canvas_surface_open(gui, canvas, config)` checks the extension, syncs
    (so a canvas just made exists and is laid out), then calls it. `bmds_gui_has_extension()` is
    new too.
- **The surface interface is `SwapChain`** (`bmds_ui`, `bmds/swapchain.h`), a `Stream` the loader's
  surface calls take through `Stream::swapChain()`:
  - **Shared:** the buffers and their states (acquire, the newest presented, the one being read),
    the image slots and the 2D-DMA's blits, frame waits. These moved out of `ProgramSurface`
    unchanged.
  - **The subclass's:** `present()` and `info()`, and optionally a palette, a title and a pointer
    mode (`NotSupported` otherwise).
  - `ProgramSurface` is one (its window, full screen and events stay its own). The service's
    `CanvasSurface` is the other.
- **How the loader reaches the service:** through `GuiOpener`, which now also takes a
  `CanvasSurface` maker, so the loader still doesn't depend on the service.
  - **Finding the connection:** the service keeps a registry of its connections by slot. Each
    connection records its client's end of the stream pair, compared only, never used: the pair's
    block lives as long as the service holds its own end.
  - **The fd decides locality:** a stream that isn't a local client's end answers `ENOTTY`, so
    remote connections can never have one.
- **Running it on the connection's loop:** the call queues a job on the connection under the
  registry's lock, posts to its loop, and waits.
  - **Ordering:** the loop runs it after the requests it holds.
  - **Ending connections:** a connection that ends refuses the jobs left, so the caller never
    waits forever.
- **`CanvasSurface`:** a counted `SwapChain`. The program's descriptor holds one reference and the
  canvas another, so either may go first.
  - **Presenting:** a present writes the buffer back from the cache, unites the damage, and posts
    (once until it runs) to the loop. The loop invalidates that part of the canvas.
  - **Painting:** the canvas paints the newest presented buffer at its top left with
    `Painter::pixels565` (or `pixels`), white around it. The swap chain's reading state keeps
    that buffer from being freed while it paints.
  - **Replacing and closing:** a canvas given another surface detaches the old one, whose presents
    then show nowhere (`INFO`'s `shown` is 0). With the fd closed, the canvas keeps the last
    picture.
- **The extension:** `Hello` names `CANVAS-SURFACE` (value "1", first opcode and event 0) on local
  connections.
  - The extensions' layout is name, value, opcode, event, as the wire of §8 says. The client
    read no value before; nothing had sent an extension yet.
  - The host test now sends one, checked by `bmds_gui_has_extension`.
- **Measured on the board** (`guisurface`, a new example: a plasma drawn by the CPU into a canvas in
  an ordinary window, a label beside it):
  - **640 × 480, drawn and presented:** 26 ms a frame with every frame shown (none dropped).
    Through PutPixels just sending a frame took 89 ms.
  - **Presents alone:** 0.2 ms each.
  - **Paced by the display** (`BMDS_SURFACE_WAIT`): 29 fps, of which drawing takes 19 ms and
    presenting (the cache's write-back of 600 KB) 2.1 ms.
  - **Maximized** (1272 × 646): it follows with a new surface on `resized`, at 14 fps; the CPU's
    drawing is the limit.
- **Tried:**
  - guitest checks:
    - the extension;
    - a surface at the canvas's size: its info, its buffer's stride, ten frames of CPU and 2D-DMA
      fills, no title or pointer mode;
    - a second surface (32 × 16, BGR24, 3 buffers) replacing it, and presenting a buffer twice;
    - the refusals: L8, half a size, too big, a label, no such object, a bad fd (`EBADF`), stdout
      (`ENOTTY`);
    - the canvas destroyed before the fd, and the fd closed before the canvas.
  - Ten rounds of guitest and guisurface left internal RAM unchanged and PSRAM within 40 bytes,
    once the connections had ended (each frees its surface a moment after its program exits).
  - blittest and surfdemo still pass on `ProgramSurface`.
- **Size:** `bmds_guiservice` 49.6 KB of flash (46.6 KB after 3b).

**3d, as built (2026-10-22):**
- **`Clipboard`** (no parent), on the connection's application's clipboard (`GuiClipboard`), so the
  service is the owner to the system's. Selections are the atoms `clipboard` and `primary`
  (drag's stays the window manager's); anything else is `BadValue`.
  - **Copying:** `copyText(selection, text)` stores it now. `offer(selection, types)` (1 to 8)
    is answered on request: the event `requested(request, selection, type)` asks, and the client
    answers with `answer(request, data)` (as many pieces as it likes, up to the clipboard's limit)
    and then `finish(request)`.
  - A request not finished in 5 s is given up (the asker's read has timed out by then). Answering
    one finished or given up is `BadValue`.
  - **Pasting:** `paste(selection, type)` is read on the clipboard's thread. The answer is the
    event `pasted(selection, type, data)`, or `pasteFailed(selection, type, why)`. `why` is one of
    `empty`, `notOffered` (nor convertible), `tooBig` (over a message: about 255 KB), `own` (its
    own offer: the client has it) or `failed`. The design had no reason; it costs one value.
  - **Also:** `clear(selection)` (only this object's copy), `types(selection)` (what is offered
    there, as a list) and `owns(selection)`, and the event `lost(selection)` when another program
    copies over this object's copy.
  - **Its object going:** its offers are withdrawn, and its requests end unanswered (the asker
    gets nothing). A text copy stays.
- **The `Bytes` tag** (10, appended to the wire): a length and any bytes, coded as a String is.
  `answer`'s and `pasted`'s data use it, so binary types travel without breaking the String
  tag's UTF-8.
  - Where `Bytes` is wanted, a String is taken.
  - `bmds_gui_bytes()` makes one; the generator gives `Bytes` arguments a pointer and a size.
- **The toolkit's side:** `GuiClipboard::offer(selection, types, Asked, arg)`, `answer()`,
  `finish()` and `forget(arg)`.
  - An asked copy's requests are told on the loop's thread and wait in their slot (state
    `Asked`, a deadline) until finished. The clipboard's thread then writes them, as rendered
    ones are.
  - At the application's end an asked copy is withdrawn: `keep()` can't render it.
  - A toolkit application's clipboard is now named after it for `clip list` (was
    "application").
- **`Hello`** names `CLIPBOARD` (value "1") on every connection, beside `CANVAS-SURFACE` on local
  ones.
- **Tried:**
  - guitest copies and pastes between two connections of its own:
    - text copied by one and pasted by the other (`text/plain` from the stored
      `text/plain;charset=utf-8`), and `notOffered` for a PNG;
    - an offer answered in two pieces of bytes with a NUL among them, then finished twice
      (`BadValue`), and its own offer pasted (`own`);
    - the loss when the other copies over it, and a clear by the non-owner (left) and the owner
      (`empty` after);
    - an offer gone with its object, and the refusals.
  - It saves the user's clipboard and primary text first and puts them back.
  - The gallery's new Clipboard tab: Offer, then `clip paste` gave the field's text and
    `clip paste -t text/html` the same in bold, each asked for and answered. `clip copy` from the
    shell told it `lost`, and Paste brought that text in. Killed with an offer out, the gallery
    left the clipboard empty.
  - Eleven guitest runs: memory unchanged to 48 bytes of PSRAM.
- **Size:** `bmds_guiservice` 52.7 KB of flash (49.6 KB after 3c).

**3e, as built (2026-10-22):**
- **`Widget.dropTypes`** (a list of clipboard types, read and written; invalid ones are
  `BadValue`; an empty list takes none) and the event **`dropped(type, data, x, y)`**: the type
  taken, its bytes (`Bytes`), and where in the widget.
  - The first of the widget's types the drag has (the client's order is its preference) is
    taken, conversions included (`GuiDropEvent::hasType`).
  - A drop is always a copy: the client gets the data and the source keeps its own. A drag that
    can't be copied is refused.
  - Up to 4 drops a connection are read at once; more are refused. A widget gone before its data
    arrives refuses the drop, and data over a message (about 255 KB) is refused too.
- **The toolkit's side:** `Widget::setDropHook(hook, arg)`, asked before the widget's own
  `dragMoved()` and `dropped()`, whatever its class. The service's widgets are the toolkit's own
  classes and can't override those.
  - A hook that declines (none of its types) leaves the drag to the widget, so a `LineEdit` still
    takes text on its own.
  - `GuiWindow` finds targets by `takesDrags()`: its class's `acceptsDrops()`, or a hook.
  - The data is read on the drag selection by the clipboard's thread. The loop tells `dropped`
    and finishes the drop with Copy (None when it failed).
- **Tried:**
  - guitest sets and reads back `dropTypes`, and checks an invalid type and an empty list. A drop
    needs a drag, so it was tried by hand.
  - The gallery's Clipboard tab has a label taking `text/uri-list` and text. The desktop's
    terminal icon dragged onto it with `win pointer` showed the copy badge. Let go, it told
    `dropped` as `text/uri-list` (`file:///home/Desktop/...`) at (120, 8) in the label; the icon
    stayed.
- **Size:** `bmds_guiservice` 53.9 KB of flash (52.7 KB after 3d).
- **Step 3 as a whole:**
  - `Image` (PutImage) and a `Canvas` drawn by PutPixels or, locally, through a surface
    (`gui_canvas_surface`, slot 125, on the new `SwapChain` under the surface calls).
  - The `Clipboard` with offers answered on request, the `Bytes` tag, and drops onto any widget.
  - The service 42.0 KB → 53.9 KB of flash.
  - Next: step 4 (§6), between programs and the shell (`Notification`, `TrayIcon`, `SERVICES`,
    `SHELL`, a panel in C).

## 12. Step 4 design: between programs and the shell (2026-10-23)

**Classes again, as step 3's were.** `Notification`, `TrayIcon`, `Services` and `Shell` are
classes in `classes.def`, with no opcodes of their own. `Hello` names `SERVICES` (value "1") on
every connection, and `SHELL` (value "1") on connections the setting allows.

**Built in four parts, each tried on the board:**
- **4a, `Notification` and `TrayIcon`** (P13): the toolkit's `Notifier` and `TrayIcon`, run for
  the client by its connection's application, as a built-in application's.
  - **`Notification`** (no parent): one notification, shown again in place.
    - Properties (RW): `title`, `body`, `icon` (a theme name), `app` (empty: the program's
      name), `urgency` (`low`, `normal`, `critical`), `timeout` (ms; 0 the setting's, -1 until
      dismissed), `actions` (a list of up to 3 labels), `silent`, `transient`.
    - `show()` posts it; showing it again replaces what it showed, in place. `close()` takes it
      away.
    - Events: `activated(action)` (0 its body, 1 to 3 a button), `dismissed`, `expired`.
    - Its object destroyed: the notification stays (as `Notifier`'s), its events go nowhere.
    - One `Notifier` a connection, made at the first `show()` (one listener of the system's 32),
      its events routed to the object by the notification's id.
    - Not taken from `bmds_notification_t`: the click targets (open a path, launch an entry).
      The client gets `activated` and does it itself.
  - **`TrayIcon`** (no parent):
    - Properties: `icon` (a theme name), `iconImage` (an `Image`, 16 × 16), `toolTip`,
      `category` (`application`, `communications`, `system`, `hardware`), `visible` (RW: shown
      in the tray, or kept hidden), `menu` (a `Menu` object, opened at the icon on a right
      click), `available` (RO: a tray is there).
    - Events: `activated(reason)` (`trigger`, `doubleClick`, `middleClick`, `context`),
      `scrolled(notches)`, `availabilityChanged(available)`.
    - Up to 4 a connection (`GuiApplication::MaxTrayIcons`).
- **4b, `SERVICES`** (P14, §4h): the window manager's service bus for clients, as the
  `Services` class (no parent; one is enough, several are allowed).
  - `register(name)` (`BadAccess` if another has it), `unregister(name)`, `send(name, bytes)`
    (`BadName` if nobody has it, `BadAccess` if its queue is full), `has(name)` (Bool).
  - Event `message(name, bytes)`: one to a name this object registered.
  - `watch(name)` and `unwatch(name)`, with the event `changed(name, present)`. The window manager
    tells only that something changed, so the service keeps each watched name's last state and
    tells only real changes.
  - Messages are the window manager's (4 KB at most, the queue's limit). A name registered by
    one connection is the connection's, released when its object or the connection goes.
  - Names: reverse-domain or the application's id; the system's are short words (`tray`,
    `files`).
- **4c, `SHELL`** (P15, §4i), privileged: the `Shell` class (no parent) can be made only by a
  connection allowed it, others get `BadAccess`.
  - **Who may:** the setting `gui/shell-clients` (Paths, empty by default), the programs' paths
    as they were run. No program in the system uses it yet: the built-in panel is linked in.
    Remote connections arrive at step 8, with `gui/shell-remote`.
  - **The window list, by events:** while the client listens, `windowChanged(window, title,
    state)` is told for each application window (normal and above levels), at once and whenever
    one opens or changes. `state` is a list of names (`focused`, `minimized`, `maximized`,
    `shaded`, `above`, `icon`), so later states are new names, not new tags. `windowClosed(window)`
    is told when one goes.
    - The service compares a snapshot taken at each change of the window manager's list, its
      icons included (a checksum of the 16 × 16 pixels).
    - `window` is the window manager's number, a separate space from the client's ids.
  - **Acting on windows:** `act(window, action)`: `raise`, `minimize`, `maximize`, `restore`,
    `close` (asked, as the user's close is: a new `WindowManager::requestClose()`), `shade`,
    `unshade`. `showWindowMenu(window, x, y)` opens its menu at a screen point.
  - **Icons:** `copyIcon(window, image)` puts a window's icon into one of the client's `Image`s
    (false: it has none), to show on a button with `iconImage`.
  - **Its own windows:** `Window.level` (`normal`, `desktop`, `panel`; set before `show()`) and
    `Window.reserve` (a panel's strip, in pixels). The desktop and panel levels need a `Shell`
    object, else `BadAccess`. Panel and desktop windows never take the focus from a press.
  - **The screen:** `workArea` (RO) and the event `workAreaChanged(rect)`.
  - **Keys:** `grabKey(usage, modifiers)` and the event `keyGrabbed(usage, modifiers)` (a
    modifier alone is a tap, as the Windows key). `Exists` from the window manager is
    `BadAccess`. They are released with the connection.
  - **Launch feedback:** `programStarting(pid)` shows the starting pointer until a window of the
    program's session opens, as the panel does.
  - **Not at this step:** the desktop menu (the system's own, made from settings by
    `desktopmenu.cpp`; a client's would need the system's put back when it goes), the palette
    (a setting: any program allowed to set it can), and screen capture (`screenshot` does it).
    Each is appended when something needs it.
- **4d, a panel in C** (`sdk/examples/guipanel`): the test that nothing is missing.
  - A window at the panel level reserving a strip at the edge the built-in panel doesn't use
    (`panel/edge`).
  - A start menu of the programs in a folder, a button for each application window (its icon
    and title: a click raises or minimizes it, a right click opens its window menu), a clock.
  - The Windows key on a chord the built-in panel doesn't hold.
  - A tray when the `tray` service is free (`-t`, with the built-in panel's tray off). That needs
    the tray's messages in C: the SDK gets `bmds/tray.h` beside `TrayProtocol`, the same structs.
  - Whatever it finds missing is added to the classes, and recorded here.
- **Budget:** each part some KB of the service's flash. Per connection: a `Notifier` (a few
  hundred bytes) once used, tray icons as the toolkit's, a watched name a few bytes, the shell's
  snapshot about 80 bytes a window.

**4a, as built (2026-10-23):**
- **`Notification`** as designed: one notification a object, shown again in place (`replaces`),
  through one `Notifier` a connection made at the first `show()`. Its events find the object by
  the notification's id. A `show()` whose notification went meanwhile (dismissed, cleared) posts a
  new one, as `TrayIcon::showMessage` does.
  - Texts are cut on a character's boundary at `Notification`'s sizes (title 63 bytes, body 255,
    a button's label 23). `timeout` is -1 to an hour; more than 3 actions is `BadValue`.
  - `NoMem` without the memory; the system's 32 listeners all in use is `BadAccess`.
- **`TrayIcon`** as designed, but for its menu: there is no `menu` property. **A `Menu` made
  under a `TrayIcon` is its menu** (the newest, if several), opened at the icon on a right click
  before `activated(context)` is told. So a program with only a tray icon needs no window to hold
  one.
  - `iconImage` copies the `Image`'s pixels (16 × 16, else `BadValue`); 0 goes back to `icon`'s
    name. A fifth icon shown is `BadAccess`.
  - The toolkit's `TrayIcon` gained `category()`.
- **Programs' names:** the connection's application is named after the program's file without
  `.elf` (`guigallery`, was `guigallery.elf`), as the notification centre and `clip list` show.
- **Tried:**
  - guitest checks:
    - a notification's fields read back, and its refusals (4 actions, a timeout under -1, an
      urgency that isn't one);
    - shown silent and transient for 300 ms until `expired` is told, then shown again in place
      and closed (twice: fine);
    - a tray icon's fields, shown and hidden, a menu under it, a 16 × 16 image and a 32 × 16 one
      (`BadValue`), the application's four icons (a fifth is `BadAccess`), and the menu destroyed
      with its icon.
  - The gallery's Desktop tab, by hand:
    - Notify showed the balloon with Raise and Dismiss. Raise told `activated(1)`, and the
      notification was shown again in place (the same id). The close box told `dismissed`, and one
      left alone `expired`.
    - The tray icon came and went with its check box. A left click told `trigger`, the wheel
      `scrolled(1)`, a right click opened the program's menu at the icon and told `context`, and
      the menu's Notify item worked. The gallery killed, its icon left the tray.
  - Once, a balloon that had already expired stayed in two screenshots until the pointer moved
    away. That didn't happen again in later tries (with and without buttons), so it is noted, not
    fixed.
  - Twenty guitest runs: memory unchanged over the last ten (internal RAM and PSRAM to the byte);
    the swings before were caches.
- **Size:** `bmds_guiservice` 56.4 KB of flash (53.9 KB after 3e). The toolkit's `Notifier` and
  `TrayIcon` were linked already (the panel and the media player use them).

**4b, as built (2026-10-23):**
- **`Services`** as designed, on the connection's application's services (the window manager's
  bus). `Hello` names `SERVICES` (value "1") on every connection.
  - `register(name)`: a name of 1 to 31 bytes (else `BadValue`). Taken by anyone, this object
    included, or all in use: `BadAccess`. The application's names grew from 4 to 8
    (`GuiApplication::MaxServices`, 32 bytes more each); its tray icons take one while shown.
  - `unregister(name)`: `BadValue` unless this object has it.
  - `send(name, bytes)`: `BadName` if nobody has the name, `BadValue` for a bad name or more than
    4 KB, `BadAccess` when the receiver's queue is full.
  - `watch(name)`: up to 16 names an object, its present state taken then; `changed(name,
    present)` is told only when that changes. Watching a name twice is fine; `unwatch` forgets it.
  - The object destroyed, or the connection ended: its names are released and its watch stopped.
- **Tried:**
  - guitest, between two connections of its own:
    - a name watched by one and registered by the other (`changed` true), taken twice
      (`BadAccess` from both), a message with a NUL among its bytes, the refusals (nobody,
      a name too long, 5000 bytes);
    - given up (`changed` false) and again (`BadValue`), released with its object, and the other
      connection's name gone when that connection closed.
  - The gallery runs once: a second start finds `org.bmds.guigallery` taken, sends it `raise` and
    ends. Tried with the first minimized: it came back, focused.
  - Fifteen guitest runs: memory unchanged to the byte.
- **Size:** `bmds_guiservice` 58.0 KB of flash (56.4 KB after 4a).

**4c, as built (2026-10-23):**
- **Who may:** the setting `gui/shell-clients` (Paths, `;` between, empty by default), compared
  with the program's path as run, once when its connection starts. `Hello` names `SHELL` (value
  "1") to those programs only. `Shell` objects and the desktop and panel levels are `BadAccess`
  to others.
- **`Window.level`** (`normal`, `desktop`, `panel`) and **`Window.reserve`** (0 to 360), set
  before `show()` (after: `BadAccess`), through the toolkit's new `GuiWindow::setLevel()`. A
  panel's window isn't focused by a press, as the system's panel. Its place is `placement`'s, and
  the client turns its frame and title bar off.
- **`Shell`**, one a connection:
  - **Following windows:** at once (posted after its `Create`) and at every change of the window
    manager's list, `windowChanged(window, title, state)` for each application window new or
    changed, and `windowClosed(window)` for each gone. `state` names `focused`, `minimized`,
    `maximized`, `shaded`, `above` and `icon`. An icon's change is found by a checksum of its
    pixels, so a new icon is told too.
  - **`act(window, action)`:** `raise`, `minimize`, `maximize`, `restore`, `close`, `shade`,
    `unshade`. `close` asks, as the close box does: the new `WindowManager::requestClose()`.
    A window not told (not an application's, or gone) is `BadValue`.
  - **`showWindowMenu(window, x, y)`**, at a screen point.
  - **`copyIcon(window, image)`:** the window's icon becomes the client's `Image`'s pixels (a new
    picture: users of the old one keep it until the image is set again). False: no icon.
  - **`workAreaChanged(rect)`** and `workArea`. The window manager now tells its watchers when
    the work area changes too (before, only application windows' changes), so a panel's strip
    coming or going is told.
  - **`grabKey(usage, modifiers)`** (HID usage and modifier bits, as `Canvas.key`'s) and the event
    `keyGrabbed`. The claims belong to the connection, as the window manager's do (it has no
    release): a `Shell` destroyed leaves its keys going nowhere, and a later `Shell` of the same
    connection claiming the chord takes them.
  - **`programStarting(pid)`:** the window manager's launch feedback, with the loader as the
    launcher (`GuiService::init()` now takes it).
- **Also fixed:** a window that can't be shown at its size (smaller than its frame) was `NoMem`;
  it is `BadValue`.
- **Tried:**
  - guitest without the setting: the refusals (`Shell`, the desktop level, a reserve), the
    normal level allowed.
  - guitest with itself named (set for the run, then reverted; never saved):
    - another window of its own followed: opened (with `icon`), minimized and restored by `act`,
      its icon copied into an `Image` (16 wide), and closed by asking (`closeRequested` came,
      then `windowClosed` once destroyed);
    - a panel of its own at the bottom: the work area 24 pixels shorter, and back when it went;
    - the refusals: a second `Shell`, an action that isn't one, window 0, an icon into a window,
      a level changed once shown, a bad key.
  - `keyGrabbed` and `programStarting` need a key typed and a program started: tried with 4d's
    panel.
  - Fifteen runs: memory unchanged after the first five (24 bytes of PSRAM).
- **Size:** `bmds_guiservice` 62.0 KB of flash (58.0 KB after 4b).

**4d, as built (2026-10-23):**
- **`guipanel`** (`sdk/examples/guipanel`, about 460 lines of C), run from the PC's files on the
  board beside the system's panel, named in `gui/shell-clients` for the runs (never saved):
  - A strip at the edge `panel/edge` leaves free (the bottom here; `-e` to choose), its work area
    reserved: maximized windows stopped above it.
  - **Start:** a menu of the `.elf` programs in a folder (`-d`; the panel's own by default), each
    started with `vfork`, `setsid` and `execv`, then `programStarting(pid)`; Ctrl+Escape
    (`grabKey`) opens it too.
  - **Window buttons:** one for each application window, from `windowChanged` and
    `windowClosed`: its icon (`copyIcon` into an `Image`) and title. A click raises the window, or
    minimizes it if it has the focus; a right click opens its window menu at the pointer.
  - **The tray** (`-t`), when the `tray` service is free: it registers `tray`, reads the programs'
    messages (`bmds/tray.h`) into tool buttons (a theme icon, or the pixels as an `Image`), answers
    clicks, right clicks and the wheel to each icon's reply service with the icon's place on the
    screen, and watches each reply service so an icon goes with its program.
  - A clock, from a `Timer`; the programs it started are reaped each second.
  - `-v` prints each event and tray message as it comes.
- **What it found missing, now added:**
  - **A right press and the wheel on any widget:** `Widget.contextMenu(x, y)` and
    `Widget.wheel(notches)`, both Off until the client listens. The toolkit's widgets gained an
    input hook (`Widget::setInputHook()`), asked before a widget's own right press or wheel,
    whatever its class, as 3e's drop hook is for drops. The service sets it on every widget object
    when it is made (`guiWidgetMade`), however it was made: a tool button is made by its tool bar,
    not `makeWidget`, and first went without one. A canvas's own `wheel` (On) is told the same way.
  - **`Widget.geometry`** (RO): where a widget is in its window's client area, for menus and the
    tray's icon rectangles.
  - **Launch feedback for GUI programs:** the window manager ends a program's starting pointer when
    a task of its session opens a window, but a GUI program's windows are opened by its
    connection's task, in no session. `gui_open` (on the program's own thread) now records the
    caller's session, the connection's application carries it (`GuiApplication::setSession()`),
    and `WindowConfig::session` tells the window manager. This helps programs started by the
    system's panel too.
  - **A tool button without its icon:** `Button::paint` checked the room for the icon at the
    text's column, not the icon's own, so a 24-pixel tool button on a column that isn't a multiple
    of 4 (the tray's, after the clock and spacing) drew no icon. It checks the icon's column now.
  - **The tray's messages in C:** `sdk/include/bmds/tray.h`, the same structs as `TrayProtocol`.
    The service checks sizes, offsets and constants against `TrayProtocol` with `static_assert`s,
    so the two can't drift.
- **Tried on the board** (the system's panel at the top, `guipanel` at the bottom):
  - The start menu opened over the Start button with the folder's 19 programs. The gallery chosen
    from it started with the starting pointer, which ended when its window opened.
  - Its button showed its icon and title at once. A click minimized it (it had the focus), another
    raised it. A right click opened its window menu at the button. Ctrl+Escape opened Start.
  - The gallery maximized stopped above the strip.
  - With the system's panel's tray item off (`panel/items`, live, put back after), `guipanel -t`
    showed the system's own tray icons (the CPU meter, live; volume; network; notifications).
    Clicking the volume icon opened the system's volume control above it, placed by the rectangle
    `guipanel` sent.
  - The gallery's own tray icon came to `guipanel`'s tray. Its right click opened the gallery's
    menu at the icon, and the wheel told it +1 and -1. The gallery killed, its button and its tray
    icon went. With `guipanel` ended and the tray item back, the system's tray came back.
  - Four starts and kills of `guipanel`: memory within 40 bytes; fifteen guitest runs after all
    this: unchanged after the first five.
  - Two `guipanel`s at once (by mistake: a kill that missed) stacked, the second above the first,
    as each is placed within the work area the other leaves. A second one's Ctrl+Escape was
    `BadAccess`: the first holds the chord.
- **Not done:** a panel's tooltips (no `toolTip` property on widgets yet), and §12 4c's deferred
  desktop menu, palette and screen capture. Each is appended when something needs it.
- **Size:** `bmds_guiservice` 62.4 KB of flash (62.0 KB after 4c).
- **Step 4 as a whole:**
  - Notifications and tray icons as classes; the service bus (`SERVICES`); the shell (`SHELL`,
    `gui/shell-clients`); a panel in C that does what `/bin/panel` does, with the system's own
    tray icons in it.
  - Fixed on the way: launch feedback for GUI programs, a tool button's icon, the work area told
    to window watchers, a window too small for its frame (`BadValue`).
  - The service 53.9 KB → 62.4 KB of flash.
  - Next: step 5 (§6), angel's GUI bindings, with the window types' names to settle (§4d).

## 13. Step 5 design: angel's GUI bindings (2026-10-23)

**The names (§4d, settled by the user 2026-10-23):** `Window` becomes the GUI window, and today's
game surface is renamed **`Screen`** (its methods, `run(Game@)`, `Event` and `EventType`
unchanged). Not released yet, so no alias is kept: the API lock's `Window` lines become `Screen`
lines (the user's explicit go-ahead for that edit), and the guide's lessons 10-12, the examples
and the corpus say `Screen`.

**Bound from `classes.def` at build time, not from `Describe` at start:** angel includes the
schema (the same X-macros the service uses), so `angel --api` is the same with or without a
display (the host has none), and the lock lists every class. A class added to the schema is in
angel at its next build.

**How a class looks in a script:**
- **Types:** each class a reference type of its own name (all but `Image`: angel's own `Image` is
  the picture a GUI image is made from, sent when it is used). Parents' members are on each class
  (AngelScript types don't inherit), and each class converts implicitly to its parents and to
  `GuiObject`, the base every GUI object has: `destroy()`.
- **Making one:** `Button b(row, "&Save");`. Every class takes its parent (`GuiObject@`, null
  for none); a class with a `text` (or `title`) property also takes it second. `Window w("Notes",
  480, 360)` (title, width, height), `Box b(parent, "down")` (direction).
- **Properties** as script properties (`label.text = "Hi"`), through accessors. Types: `Int`
  `int`, `Long` `int64`, `Bool` `bool`, `String` and `Atom` `string` (an enumeration's value by
  name: `box.direction = "across"`), `Color` `uint` (angel's 0xRRGGBB), `List` `array<string>@`,
  `Rect` the new value type `Rect` (x, y, width, height), `Id` the class it names (`owner`:
  `Window@`; an image: `Image@`, angel's).
- **Methods** as methods; one with a result waits for the answer, others are sent in batches.
- **Events** as `onName(callback)`: `save.onClicked(function() { ... })`. A callback is a funcdef
  for the event's values, named after their types: `Callback` (none), `IntCallback`,
  `StringCallback`, `IntIntCallback`... A delegate (`MyClass::method`), a global function or an
  anonymous one. An event the service sends only when asked (`Off`) is asked for when its
  callback is set, and no more when it is cleared (null).
- **Lifetime:** an object lives until `destroy()`, its parent's destruction or the script's end,
  not until the script lets go of its handle (a button made in a function stays in its window).
  The runtime keeps every object it made, to find it by id when an event comes.

**`app`** (a global, type `App`): the program's connection, opened at the first object.
- `app.run()` serves the GUI's events and the timers, calling the callbacks, until `app.quit()`,
  or until no window is shown once one was. A script that ends with windows shown runs the loop
  then, as one ending with a `Screen` waits for it to close.
- `app.after(ms, callback)` and `app.every(ms, callback)`: timers of the loop's own (an `int`
  each, `app.stop(timer)`), not round trips.
- Callbacks run nested in the script's context, as `Game`'s methods do (the debugger stops in
  them); after the script's end, in a context of their own.

**Errors** (the service's, which come after the call): reported on stderr with the script line
that made the request and the request's name (`line 12: Label.text: BadValue`); the script goes
on, as X11's clients do. libbmdsgui gains `bmds_gui_sequence()` (the last request's number) for
that. Making an object without a display raises a script exception ("no GUI here").

**Built in three parts, each tried on the board:**
- **5a:** the rename, the classes, `app` and its loop and timers, callbacks, errors; the
  calculator example.
- **5b:** the one-line dialogs (`alert(text)`, `ask(question)`, `askText(prompt)`,
  `chooseFile()`), images (angel's `Image` as an icon), the notes example.
- **5c:** `Canvas` drawn with `Screen`'s drawing methods (locally through its surface), events of
  `Screen`s in the same loop, the paint example with a palette window.

**Budget:** the bindings are tables walked at start: some KB of code in angel, a few KB of
registrations in its heap, one object per GUI object made.

**5a, as built (2026-10-23):**
- **The rename:** angel's game surface is `Screen` (its registration's names, the lock's lines
  edited in place with a comment saying why, lessons 10-12, six examples, three corpus scripts,
  `serve.py`). The C++ class in `graphics.cpp` keeps its name.
- **`programs/angel/gui.cpp`:** the schema included as the service includes it, into tables of
  classes, properties, methods and events. Generic functions read which member they serve from
  each registration's auxiliary pointer (`methodAux()` and `behaviourAux()`, new in `api.cpp`),
  one `Binding` for each member of the schema, shared by the classes that inherit it.
  - Types as designed: properties through `get_`/`set_` accessors, methods (one with a result
    waits), events as `onName(XCallback@)`. There are 19 funcdefs, named after their values'
    types (`Callback`, `IntCallback`, `StringStringIntIntCallback`...). A child's member hides
    its parent's of the same name (`Canvas.wheel`, `Widget.wheel`). Image Ids aren't bound yet
    (5b).
  - **Handles:** angel's generic functions release the handle arguments they don't keep (as
    `Screen::run`'s `Game@`): a parent, an `Id`, a callback (kept as the callback's reference).
  - **Objects** are kept in a registry, which holds a reference to each, until `destroy()` (with
    their children), or the runtime's end (`releaseGui()`, before the engine is shut down). A
    destroyed object's use raises "This Button was destroyed".
  - **The loop:** `app.run()` waits on the connection (`bmds_gui_next_event`, flushing first) or
    the next timer, runs the timers due, and ends on `quit()` or when no window is shown once one
    was. A window counts as shown from `show()` until `close()` or its `closed` event. With
    windows shown at the script's end, `finishGui()` runs it then.
  - **Callbacks** run nested in the active context (PushState), else in a context of the
    runtime's own. An exception in one is reported with its line, and the script ends, as in a
    `Game`'s methods.
  - **Errors:** a ring of the last 128 requests (number, script line, class and member). A
    request that doesn't wait is recorded after it is made; one that waits, before (as the next
    number), since its error can come while it waits.
- **The cost, and what was done about it:** registering every class's members (about 1,000
  methods) took **1.07 s** at every start on the board. AngelScript parses each declaration and
  checks each property accessor against every method of its type
  (`asCBuilder::ValidateVirtualProperty`, half the time on the host's profile). So a script gets
  only the members of the classes it names (whole words in its sources, with their parents and
  `Window`), registered before it is built (`registerGuiClasses()`). The types, their reference
  counts, `Rect`, `GuiObject`, the funcdefs and `app` are always registered; `--api` and
  `--serve` get every class, so the lock and an editor's completion are whole.
  - A script without GUI starts in 0.13 s on the board (1.29 s with every class). The
    calculator's build takes 0.30 s.
- **The SDK:** `bmds_gui_sequence()`. The host's angel links `gui.c` (no display there: making
  an object raises "No GUI here: the window system isn't running").
- **Tried:**
  - `tests/board/gui.as`, run on the board with angel (new: `tests/board` holds scripts that need
    the board): properties of every kind read back (a list, a 64-bit number, an atom, a bool),
    a button as a `Widget`, `click()` coming back to its callback, a button made in a function
    and let go of still calling back, `every()` three times and the loop ending when the window
    closes, a widget's geometry, `after()` stopped, `destroy()` and a destroyed object's use
    raising. It prints `gui.as: ok`. Ten runs left memory unchanged to the byte.
  - A window's width set out of range was reported as `errors.as:3: GUI error: Window.width:
    BadValue`, and the script went on.
  - A script that shows a window and ends ran its loop: its timer closed the window.
  - The calculator (`programs/angel/examples/calculator.as`, in the ROM): 7 × 6 = showed 42, and
    its close box ended the script (`onClosed` calling `app.quit()`).
  - The host corpus has `guinone.as`: a GUI script builds, and making an object raises.
- **What the calculator wanted** (the user asked for centred keys that follow the window's size):
  rows and keys already stretch (`Widget.stretch`; a box's children fill its other side), but
  three things the toolkit has weren't classes' properties yet. Appended: `Button.centered` (its
  text in the middle), `Label.align` (`left`, `center`, `right`) and `Label.frame` (`none`,
  `field`, `status`, `sunken`, `raised`). The toolkit gained the getters, and `Label::clearFrame()`.
  The calculator has a sunken display, right aligned, and keys that share the window, at 240 × 280
  or maximized.
- **Size:** angel's `gui.cpp` is 22.5 KB of code and tables (and libbmdsgui, about 6 KB, now
  linked); angel's text is 1.05 MB.
- **Not yet:** the debugger stopping in a callback after the script's end (its own context isn't
  debugged); a `Screen`'s events in `app.run()` (5c).

**5b, as built (2026-10-24):**
- **The one-line dialogs** (globals, registered always: they name no class): `alert(text, title
  = "")`, `ask(question, title = "")` (true for Yes), `askText(prompt, text = "", title = "")`
  (what was typed, "" if cancelled), `chooseFile(title = "")` and `chooseSaveFile(name = "",
  title = "")` (a path, "" if cancelled; a fifth, since saving wants its own). Each makes its
  objects itself (not kept in the registry), opens them, and waits: the loop's work (events to
  the script's other objects, the timers) goes on meanwhile, so `app.quit()` in a timer ends the
  wait. A dialog belongs to the script's active window, else the one shown last: its title is
  that window's unless given, and that window is blocked until the answer.
  - `alert` and `ask` are the service's `MessageBox`; the file dialogs its `FileDialog`, opening
    where the last one was. `askText` is a window angel builds (a label, a line, OK and Cancel;
    modal over its owner, no minimize or maximize box): the toolkit has no such dialog, and
    making one from the classes costs no ABI.
  - A wait matches its answer by the event's name: a window says `resized` as it is shown, and
    the first try took that for the close box (askText returned "" at once).
- **A dialog's owner** (the service, appended: `MessageBox.owner`, `FileDialog.owner`,
  `ColorDialog.owner`): the toolkit's message box and dialogs don't block other windows, so a
  modal block had to be the service's. While one is open its owner's widgets are disabled, as a
  modal window's owner's are (one `blockOwner()` now serves both), and activating the owner
  brings the dialog up. Clicking into the blocked notes window left the focus on the question,
  and typing went nowhere.
- **Images:** an `Id` member that names an image (`iconImage`, `ItemView.setIconImage`) takes
  angel's `Image@`, assigned as handles are (`@button.iconImage = picture;`). The first use
  makes a GUI `Image` and sends its pixels (ARGB, transparent where the image is keyed); drawn on
  since (its generation counted), it is sent again at the next use. The runtime keeps each GUI
  image, and a reference to its `Image`, until the end; the getter gives the same `Image` back.
  More than 512 pixels either way raises. `graphics.cpp` lends `gui.cpp` an opaque view of an
  `Image` (`imageInfo()`, `imageArgb()`).
- **The notes example** (`programs/angel/examples/notes.as`, in the ROM): a menu bar with
  shortcuts, a toolbar and a window icon drawn by the script, a `TextEdit`, a status bar with the
  file and the cursor's place; New, Open, Save, Save As, Go to Line (askText), About (alert),
  and the close box asking first (`onCloseRequested`).
- **Found on the way:**
  - The service's `Spacer` painted nothing. A box paints only the gaps between its children, so a
    spacer showed whatever was there before (the desktop, a caret's stale pixels). It paints the
    face now.
  - A script that called `app.quit()` and then ended with a window still shown went back into the
    loop at its end (`finishGui()`); `app.quit()` now means the script is done with it.
  - A status field 140 pixels wide clipped "Line 2, column 12" to "...column 1". It looked stale.
- **Tried on the board:**
  - Notes: two lines typed, the title `*` and the cursor's field following. Ctrl+G asked, and 1
    went to line 1. Ctrl+S opened the save dialog; saved to `/tmp/notes.txt`, both lines in the
    file. Ctrl+N cleared. Ctrl+O opened in `/tmp`, and the file came back. An edit, then the close
    box asked: No kept the window, Yes ended the script.
  - `alert` showed the window's title and blocked the window.
  - `tests/board/gui.as` gained an icon read back as its `Image`, set again after drawing, none,
    one over 512 pixels raising, and a dialog's owner. It passes. Free memory over 25 runs moved
    both ways with the board's other tasks, with no steady growth.
  - The host corpus: `guinone.as` asks with a dialog and raises "No GUI here".
- **Size:** angel's text is 1,059,044 bytes (was about 1.05 MB); the service's part is a few
  hundred bytes.

**5c, as built (2026-10-25):**
- **A `Canvas` drawn as a `Screen` is:** the canvas class has every drawing method a `Screen`
  and an `Image` have (`clear`, `plot`, `pixel`, lines, shapes, `floodFill`, `text`, `image`,
  `clip`, `save`), and `part()` (a new `Image` of what is drawn; `Screen` has it too). Its size
  is its `width` and `height` properties, so it has no `width()` methods. graphics.cpp's drawing
  functions find their picture through a hook given as the registration's auxiliary pointer
  (`CanvasHooks`, gui.cpp's `canvasOf()`); a destroyed canvas raises, its drawing going to a
  scratch picture.
  - **Its picture:** made at the first drawing, BGR24 as a screen's (a colour reads back as it was
    drawn). Laid out (its window shown), it is a surface of the service's (`CANVAS-SURFACE`, two
    buffers, its width rounded up to 4); before that, a picture in memory at its
    `preferredWidth` x `preferredHeight` (none: "no size yet" raised), white as the service's
    canvas is. At each `resized` the runtime opens a surface of the new size before the script
    hears of it, the picture so far copied over and the rest white.
  - **Shown by the loop:** drawing marks the canvases; before the loop waits (`app.run()`, a
    dialog's wait) each canvas drawn on presents its held buffer and takes the other. Drawing in a
    loop that never returns to `app.run()` isn't shown, as in any toolkit.
  - **The shared drawing:** `Window` (a screen) became `Surface`, the buffers, their damage, the
    2D-DMA's fills, copies and registered images, and presenting; `Window` keeps its thread,
    events and pacing on it, and `CanvasPicture` is the other `Surface`.
  - **The service** lets go of a canvas's own RGB565 picture while a surface shows it (a 640 x 436
    canvas's is 558 KB of PSRAM); `PutPixels` to it draws nowhere then.
- **A screen's events in `app.run()`:** `Screen.onEvent(EventCallback@)` hands each event (Close
  too) to a callback while the loop runs, beside the GUI's. The screen's thread writes to a pipe
  the loop polls with the connection (which works with no GUI connection at all), and a screen
  served so counts as a window shown: the loop ends once neither is open. With one at the
  script's end, the loop runs then (`finishGui()` now comes before `waitForWindows()`, which skips
  served screens).
- **`chooseColor(color = 0, title = "")`:** the service's `ColorDialog`, owned and waited for as
  the other one-line dialogs; `color` back if cancelled.
- **The schema, appended:** `Button.checked` and `ToolButton.checked` (drawn pressed in: a
  palette's chosen tool). The toolkit had `Button::setChecked()` already.
- **Found on the way: canvases off the 4-pixel columns showed nothing.** `Painter::pixels()` and
  `pixels565()` refused an x or a width off the display's 4-pixel columns (`Surface::Align`, the
  2D-DMA's rows in PSRAM) with `Invalid`, and the canvas widget ignored it. A canvas in a window's
  6-pixel margin (the paint example's palette) stayed white; the main canvas, at an aligned x,
  worked. The painter now copies the whole columns by one job and patches the edges' few
  columns (`Surface::patch()`, a surface row's bytes at a time). Noted in
  `docs/platforms/esp32p4.md`.
- **The paint example** (`programs/angel/examples/paint.as`, replacing the single-screen one): the
  picture on a canvas in a window with a menu bar (New, Open, Save, Save As, Quit; Undo, which
  swaps with the kept picture, and Clear; Window ▸ Tools; About) and a status bar (the file, the
  pointer's place, the size). A Tools window beside it (placed from both windows' `placement`):
  seven tools as tool buttons with drawn icons, the chosen one checked (pencil, brush, line,
  rectangle, ellipse, fill, eraser); the colours as cells drawn on a small canvas (left button
  the first colour, right the second); both shown on another; More... (`chooseColor()`); a size
  slider. A shape follows the pointer by putting back its last place from the kept picture,
  clipped, and drawing again.
- **Tried on the board:**
  - Paint: a pencil stroke, a red brush stroke, a blue rectangle, a lime ellipse and a yellow fill
    all drew; Undo took the fill back; the eraser with the right button cut the rectangle; More...
    opened at the first colour and teal came back; maximized to 1272 x 624 the picture stayed;
    Window ▸ Tools brought the palette back over it; size 19 drew a thick brush and line; the
    close box asked, and Yes ended it.
  - A canvas saved to PNG and loaded back as an `Image`: 200 x 120, its pixels as drawn.
  - A screen served in `app.run()`: clicks drew where they were, its Close called back and
    `app.quit()` ended the loop. A script with only a screen and `onEvent` ran its loop at its end
    and ended when the screen closed.
  - `tests/board/gui.as` gained a canvas drawn before it is shown and through its surface, its
    picture kept when it is laid out, `part()`, `resized`, one with no size raising, a destroyed
    one raising, and `checked`. It passes; 20 runs left internal RAM unchanged, PSRAM 380 bytes
    down after the first ten and unchanged after the next ten.
  - The host corpus: `guinone.as` builds a canvas's drawing and a screen's `onEvent`, and takes
    a screen's `part()`.
- **Size:** angel's text is 1,066,414 bytes (+7.4 KB).
- **Not yet:** a canvas over a remote connection (no surface there): step 8 sends its damage by
  `PutPixels`. The palette isn't kept above its picture's window (the window manager has no
  owned-window stacking), so a maximized picture covers it until Window ▸ Tools.

## 14. Step 6 design: angel's service bindings (2026-10-25)

§4e's table in three commits, as step 5 was. Four of its rows are done already: 5a bound every
class of the schema, so `Notification`, `TrayIcon`, `Services`, `Shell` and `Clipboard` are
there, and `app.after()`/`app.every()` are the timers.
- **6a:** settings, processes, the system, the application folders; and the loop's other sources
  under all of them.
- **6b:** files (a folder watched, file types and their applications), gamepads and actions on a
  `Screen`.
- **6c:** audio (`AudioPlayer`, the mixer's volume, MIDI ports) and speech (voices, rate, pitch,
  done).

**6a's design:**
- **The loop's other sources** (gui.cpp): descriptors `app.run()` and a dialog's wait poll beside
  the GUI's connection and the screens' pipe, each with a function the loop calls when it is
  readable, and whether it keeps the loop running (a process with callbacks, a setting or the
  network watched). With no GUI connection the loop serves them too, so the host runs processes'
  callbacks. Callbacks with values go through gui.cpp's one way of calling the script (nested in
  the script's context, or its own context after the script's end), shared with the events.
- **`Setting`** (on `sysconf_find`, `get`, `info`, `set`, `next`, `declare`, `watch`):
  - `Setting(key)` names one; `exists` says whether it is there. `value` reads it (`""` if
    not) and is set (checked by the system: "Can't set ...: not a number" raised; not saved).
    `defaultValue`, `help`, `type` (`bool`, `int`, `string`, `color`, `enum`, `path`, `paths`,
    `secret`), `choices` (an enum's words, `|` between), `isDefault`, `isSaved`, `changed` (not
    what a reboot gives).
  - `save()`, `revert()` (to the saved value, else the default), `reset()` (to the default,
    not saved).
  - **An application's own:** `Setting(key, type, defaultValue, help = "")` declares it
    (`sysconf_declare`: `owner/name` keys, kept until reboot; a saved value applies at once).
    The type is a word as above, `int MIN MAX`, or an enum's words (`small|medium|large`).
  - `onChanged(StringCallback@)`: the new value, in the loop (a watch descriptor per setting).
  - `settings(prefix = "")`: the keys under a prefix. `setting(key)` stays.
- **`Process`** (musl's `posix_spawn` with file actions, `waitpid`, `kill`, `pipe2`):
  - `Process(command)` runs a shell command line (`/bin/sh -c`, as `run()` does);
    `Process(program, arguments)` runs a program by name on the `PATH` with no shell.
  - Its input, output and errors are pipes. `write(text)`, `closeInput()`. `read()` gives what
    has come so far (no wait), `readLine()` waits for a line, `readAll()` until the output ends;
    `readErrors()` likewise for the errors. `wait()` waits for it to end, keeping what it wrote
    meanwhile (so a full pipe doesn't hang it), and gives the exit status.
  - `pid`, `running`, `exitStatus` (-1 while running; 128 + N if killed by signal N, as `run()`
    gives), `signal(sig = 15)`.
  - `onOutput(StringCallback@)`, `onErrors(StringCallback@)`, `onExit(IntCallback@)`: in the
    loop, the output before the exit. The callbacks are let go at the exit (no cycles kept).
  - **Ends learnt without polling:** a `SIGCHLD` handler (`SA_RESTART`) writes to a pipe the loop
    polls, while any process is started; each tracked process is asked with `WNOHANG`. A process
    let go while running is still reaped. Writing to one that ended raises (`SIGPIPE` ignored
    around the write only, so children don't inherit it ignored).
- **`system`** (an object, as `app` is): `hostName`, `userName`, `board`, `version`, `cores`,
  `cpuMhz` (`system_info`); `memoryTotal`, `memoryFree`, `programMemory` (`mem_info`);
  `cpuLoad` (0 to 1, the cores' busy time since the last time it was asked, the first time
  since boot; -1 when CPU time isn't counted: `stats_read`); `online`, `address` (`net_info`),
  `onNetworkChanged(Callback@)` (`net_watch`).
- **`DateTime`** (a value type, musl's `localtime_r` with `TZ` switched for a zone, so names use
  the zone database with its history): `DateTime()` now, `DateTime(seconds, zone = "")`,
  `DateTime(year, month, day, hour = 0, minute = 0, second = 0, zone = "")`; `year`, `month`,
  `day`, `hour`, `minute`, `second`, `weekday` (0 Sunday), `yearDay`, `seconds` (Unix time),
  `offset` (seconds east of UTC), `zone` (its abbreviation); `inZone(zone)`, `format(pattern)`
  (`strftime`'s). `""` is the program's zone (`TZ`, the kernel's).
- **Application folders:** `appDir()` is `BMDS_APP_DIR`, or the script's own folder when it isn't
  in an application; `dataDir()` is `bmds_app_data_dir()`, or `$HOME/.local/share/NAME` (the
  script's name without `.as`) for a script outside one, made if it isn't there.
- **Tests:** the host corpus runs a process with callbacks in `app.run()`, reads and writes one,
  `wait()`s, and takes dates in named and POSIX zones (the host's zone database); settings and
  `system` build there and give empty answers. `tests/board/services.as` declares, sets, watches,
  saves nothing and reverts a setting of its own, runs processes, reads `system` and dates.

**6a, as built (2026-10-25):**
- **The loop's other sources** (gui.cpp, angel.h's `LoopSource`): up to 64 descriptors polled with
  the connection and the screens' pipe, each with a function called when it is readable and a
  `LoopHold`: `Nothing`, `Run` (`app.run()` waits for it: a watched setting, the network) or
  `RunAndEnd` (also runs the loop at the script's end: a process with callbacks). With no GUI
  connection the loop polls them alone, so the host runs processes' callbacks. `callScript()`
  calls the script with ints and strings through the events' own path (`beginCall`/`endCall`,
  split out of `call()`).
- **`Setting`** (services.cpp) as designed. A watched setting is kept (a reference) until
  `onChanged(null)`, so one made in a function is still heard. The declared type's words:
  `bool`, `int`, `int MIN MAX`, `string`, `color`, `path`, `paths`, `secret`, or an enum's words.
  On the host there are no settings: `exists` is false, setting one raises "No setting".
- **`Process`** (process.cpp): `read(most)`, `readLine()`, `readAll()`, `eof()`, `ready(ms)`,
  `write()`, `writeLine()` as `Socket` has them (so `read()` waits for some, unlike the design's
  word); `readErrors()` doesn't wait. Callbacks get whole
  lines. Every process started is kept until it has ended, its pipes have ended and its
  callbacks have run (a list of 32); one let go of that has ended, or only the list holds, is
  dropped at the next start, so a script starting processes in its own loop doesn't fill it. The
  screens' presenter threads block `SIGCHLD`, so only the script's thread is interrupted.
- **Found on the board: a program has 16 descriptors.** A process holds three until it ends;
  starting one needs six for a moment. The fourth process in one test failed with `EMFILE` while
  an ended one still held its input pipe: a process's input is now closed as soon as it is known
  to have ended.
- **`system`, `DateTime`, the folders** as designed, but for `system.address`, left out:
  `localAddress()` (the network step's) gives it. `system.userName` is `USER` or `LOGNAME`
  (on the host also `getpwuid()`, kept off the board: 2 KB of passwd and nscd code). `cpuLoad`
  compares the `cpu.coreN.busy_us` totals (`stats_read`) with the last call's. `DateTime`'s `%Z`
  is put in before `strftime()`: musl's names only a zone pointer into its own tables, so a copy
  printed nothing. `toString()` makes `println(date)` work.
- **Found: the SDK leaked a page per zone switch.** musl unmaps a zone file by its size, and the
  SDK's `munmap` (`sdk/crt/linux.c`) freed a mapping only when the length covered its whole
  last page, so each switch kept 4 KB until the program ended (200 dates in Tokyo: 1.6 MB). The
  length is now rounded up to pages, as Linux does; `progtest mem` checks it.
- **The System example** (`programs/angel/examples/system.as`): the host, CPU, load, memory and
  network (refreshed each second; `onNetworkChanged`), a clock in a zone kept as the example's
  own setting (declared, saved when changed), and a command line whose output and errors come in
  line by line, with its exit status in the status bar.
- **Tried on the board:**
  - `tests/board/services.as` passes: a system setting read; one declared, set, refused outside
    its limits and outside its enum, declared again, watched (a change from a timer called back
    in the loop), reverted and reset, nothing saved; processes read, written, given arguments,
    missing, signalled, written to after their end, 40 in turn, and called back in the loop; the
    system's names, memory, load and network; the epoch, Moscow's 1990 summer time from the
    database's history, a date made in Berlin, POSIX rules; the script's folders. PSRAM over 15
    further runs moved by 36 bytes.
  - The System example: `ls /` and a failing command's output and errors in the window, "Exit
    status 0"; the zone set to Asia/Tokyo showed JST and was saved, came back at the next
    start, and emptied again (taken out of the store).
  - The host corpus: `services.as` (processes with callbacks in `app.run()` with no GUI, dates in
    named zones and POSIX rules, normalised dates, no settings).
- **Size:** angel's text is 1,102,322 bytes (+35.9 KB: services.cpp 16 KB, process.cpp 10 KB,
  musl's `posix_spawn`, `strftime`, `mktime` and the rest).

**6b's design (2026-10-25):**
- **A folder watched: `fs_watch`, slot 126 (new).** The VFS has watched folders for the panel
  since `filebrowser.md` §4i (`DirectoryWatch`), but no program could ask. The call gives one as a
  descriptor, as `net_watch` and `sysconf_watch` do:
  - `int32_t fs_watch(const char *path, uint32_t flags)`: a new descriptor watching folder
    @p path (links and `/home` followed). `flags`: 0 or `BMDS_O_CLOEXEC`. `-BMDS_ENOENT`,
    `-BMDS_ENOTDIR`, `-BMDS_EINVAL`, `-BMDS_ENOMEM`, `-BMDS_EMFILE`.
  - A read (4 bytes or more, a multiple of 4) gives one `uint32_t`: what changed since the last
    read, as bits: `BMDS_FS_WATCH_ENTRIES` (1: an entry made, removed or renamed; the folder
    itself renamed or removed), `BMDS_FS_WATCH_CONTENTS` (2: a file in it written and closed, or
    touched), `BMDS_FS_WATCH_MOUNTS` (4: a mount or an unmount: anything may have changed). It
    waits while nothing changed; `io_poll` says `BMDS_POLL_IN` when something did. However many
    changes come, they are one record until read, so a folder filled by a copy is one read again.
  - Not told, as for the panel: the folders below one renamed or removed, `/dev` gaining a
    device. Closing it ends the watch. In the kernel: a `Stream` around a `DirectoryWatch`
    (bmds_fs), its notify releasing a semaphore and waking the pollers.
- **`FolderWatch`** (angel, files.cpp): `FolderWatch(path)` (raises if it isn't a folder), `path`,
  `onChanged(IntCallback@)` with the bits as `FolderChange::Entries`, `Contents`, `Mounts`, in the
  loop (a `LoopSource` holding `app.run()`, as a watched setting does; kept until
  `onChanged(null)` or `stop()`). On the host, Linux's inotify gives the same bits, so the corpus
  tests it.
- **File types:** `fileType(path)` (`file_type`: by the name, else by the first bytes; `""` when it
  can't be told, and on the host). **Opened with their applications:** `openFile(path)` opens a
  file as a double click on the desktop does: a folder in a window, a program run, a launcher's
  command, another file by its type's application (`mimeapps.list`, `filetypes.md`). Only the
  panel opens files (`filetypes.md` T11), so it answers a new service, **`open`** (the paths,
  absolute, a NUL after each, as `files` has them; `files` itself shows a file selected in its
  folder, so it can't be the one). angel sends through the GUI connection's `Services`
  (`has("open")` first: "Nothing opens files here (start the panel)" raised if no one has it).
- **Gamepads on a `Screen`:** the surface's `BMDS_EVENT_PAD` events, until now dropped, become
  `Event`s, and their state is kept for asking:
  - `EventType` gains `PadDown`, `PadUp`, `PadMove` (an axis), `PadAttached`, `PadDetached`;
    `Event` gains `pad` (its number; -1 for the keyboard), `control` (a button's or an axis's name,
    `controls.conf`'s: `South`, `East`, `West`, `North`, `L1`...`R3`, `Up`, `Down`, `Left`, `Right`,
    `Start`, `Select`, `Guide`; axes `LeftX`, `LeftY`, `RightX`, `RightY`, `L2`, `R2`) and
    `value` (a float: -1 to 1 for a stick, right and down positive; 0 to 1 for a trigger; 1 or 0
    for a button).
  - An axis's moves are coalesced in the queue as the mouse's are (the latest of one axis
    replaces the one waiting), so a stick can't push the keys out of it.
  - `padDown(control, pad = 0)`, `padAxis(control, pad = 0)`, `padAttached(pad = 0)`: pads 0-3.
- **Actions** (`bmds_controls`, `controls.md` §7c): a game declares what it can do, each with keys
  and pad controls the user may rebind; the system matches them and the game gets the action.
  - `controls.declare(text = "")`: the declaration (`controls.conf`'s INI), or `""` for
    `controls.conf` in `appDir()`. An action without `Id=` gets one (angel numbers them), since a
    script names its actions. Refused: "The controls weren't accepted: ..." with the system's
    reason where it gives one (the line is in the system log).
  - `controls.activeSet` (`""`: none, every event raw, for typing a name), `controls.keys(action)`
    and `controls.pad(action)`: the bindings as text, for hints ("Space to play again").
  - `EventType` gains `ActionDown` (`repeat` for a held key's repeats), `ActionUp` and
    `ActionMove` (an axis action); `Event.action` is its name, `value` and `pad` as above.
    `actionDown(name)` and `actionValue(name)` on the `Screen`.
  - The host keeps the declaration (its names checked) and binds nothing.
- **The example:** Snake steers by actions (arrows and WASD, the D-pad and the left stick), its
  "play again" hint made from the binding.
- **Tests:** progtest `files` watches a folder (made, written, renamed, removed: each bit; one
  record for many changes; `ENOTDIR`, `ENOENT`). The corpus's `files` test gains a `FolderWatch`
  (inotify) and `fileType()`; a `controls` test of declarations. `tests/board/files.as`: a watched
  folder's changes in the loop, `fileType()` of a text, a folder and a program, a declaration
  the system accepts. `openFile()`, pads and actions tried by hand.

**6b, as built (2026-10-25):**
- **`fs_watch`** (slot 126) as designed: `DirectoryWatch::openFile()` (bmds_fs,
  `directorywatchfile.cpp`, 900 bytes) is the stream; its handle closed, the watch leaves the
  VFS's list before the rest of the stream goes, since a change in another task may be telling it
  meanwhile. `Process::fsWatch()` resolves the path with the process's directory, as
  `file_type` does. The bits are `DirectoryWatch::Change`'s (checked equal at build).
- **`FolderWatch`, `fileType()`** (angel, `folders.cpp`) as designed; `watching` says whether it
  was stopped. The host's inotify is mapped to the system's bits: a file made counts when it is
  closed after writing (`IN_CLOSE_WRITE`), as the system tells it, and a folder made, an entry
  removed or moved are `Entries`. At most 16 folders watched with callbacks.
- **`openFile()`** (gui.cpp): the path made absolute and checked, then sent to the panel's `open`
  service (`PanelApp::openMessage()` calls `FolderActions::open()`, the double click's own).
- **Pads and actions on a `Screen`** as designed. The surface's events are kept as they were
  (`RawEvent` gains the pad, the control or the action's Id, the value); an action's name is
  found as the script takes the event. A stick's or an axis action's waiting event is updated in
  place, anywhere in the queue. Pad and action state live with the window, under its lock (the
  presenter thread writes them). `controls.declare()` checks the names (`[a-z0-9-]`, at most 32,
  each once), the Ids (1 to 65535, each once), and numbers the actions without one past those
  given; the system checks the rest (an action without `Keys=` is refused).
- **Snake** declares five actions (four turns: arrows and WASD, the D-pad and the left stick's
  halves; `again`: Space or Enter, the pad's South or Start); "Game over! Space to play again"
  takes its key from `controls.keys("again")`.
- **Tried on the board** (flashed):
  - `tests/board/files.as` passes: a folder's changes called back (written: `Contents`; a folder
    made: `Entries`; 20 files written and a folder removed: one call with both), a missing folder
    and a file refused; the types of a text, a folder, a program, and of nothing; a declaration
    accepted, its bindings as text (`Left; A`, `Pad.South`), its sets chosen, a set not declared
    and an action without keys refused. `services.as` still passes.
  - progtest `files` (built fresh) passes with the watch's checks.
  - `openFile()` of a text file opened vi on it in a terminal; of `/tmp`, a folder window.
  - A script printing a `Screen`'s events: the pad attached told as the screen took the focus
    (`PadAttached`, pad 0); Left and A each pressing `left` once, both held together one press
    and one release; a held key's repeats not given (no `Repeat=true`); a key axis (`Keys=Down
    Up`) at -1, 1 and 0; the set emptied releasing what was down; `actionDown()`,
    `actionValue()`, `padAttached()` following. Pad buttons and sticks were not pressed (no one
    at the pad): their events take the same path as the attached one.
  - Snake: W turned the snake up, the hint read "Space to play again", Space started again.
- **Size:** angel's text is 1,114,488 bytes (+12.2 KB: `folders.cpp` 2.8 KB, the pads, actions
  and `controls` in graphics.cpp, `openFile()`).

**6c's design (2026-10-25):** on calls that exist (`audio_decode`, `/dev/dsp`, `audio_speak`,
`/dev/midiN`, the `audio/` settings); nothing new in the kernel.
- **Playing in the background:** a player and a speaking voice each pump on a thread of their own
  (the decoder's or the speaker's reads into `/dev/dsp`, as libbmdsgame's music does), started
  when they begin and joined when they end. The threads touch no script object: they tell the
  loop through one pipe all of them share (a `LoopSource`), and the callbacks run there. Each
  holds two descriptors while it sounds (the source and the stream).
- **`AudioPlayer`** (`audio_decode`: WAV, MP3, Vorbis, FLAC, MIDI through the synth):
  `AudioPlayer(path)` opens and reads the header (raises: "Can't play 'x': ..."); `play()`
  (again after `pause()` resumes; after the end starts over), `pause()`, `stop()` (back to the
  start), `wait()` (until it ends; raises if it loops); `playing`, `position` (seconds; set: a
  seek, exact as the decoder's `lseek` is), `duration` (-1 if unknown), `volume` (0 to 1, the
  stream's gain in dB), `loop`; `codec`, `rate`, `channels`, `bitrate`; `tag(name)` (`TITLE`,
  `ARTIST`...; `""` if none) and `tagNames()`; `onFinished(Callback@)` at its end (not at a
  `stop()`). A playing player is kept until it ends, so `AudioPlayer("a.mp3").play()` is heard;
  at the script's end every player stops (`wait()` hears one out). The position is the device's:
  the frames it has played since the last seek (`audio_status`), not what was decoded ahead.
- **The mixer:** `mixer.volume` (dB, -60 to 0) and `mixer.muted` of the default device
  (`audio/default`'s `audio/DEV/volume`, `/mute`), changed as the panel's control changes them
  but not saved: the saved level is the user's.
- **Speech:** `Voice(name = "")` (the default voice, `speech/voice`; raises if there is no such
  voice); `name`, `speed` (percent, 20 to 500), `pitch` (50 to 200), `volume` (1 to 500);
  `speak(text)` queues the text and returns, `say(text)` waits until it has been heard,
  `stop()`, `wait()`, `speaking`; `onSaid(StringCallback@)` with each text once heard (each one
  ended with a NUL and the stream drained before the next). `voices()`: the voices in
  `speech/path`'s directories (`NAME_ta.bin`). `say()` stays. On the host nothing is heard, but
  `speak()`'s texts are told said in the loop, so the corpus tests the callbacks.
- **MIDI ports:** `midiPorts()` (their names, `midiN`'s order); `MidiPort(name)` (by name or
  `midiN`) reads the port's source and writes to its destination as events
  (`BMDS_MIDI_EVENTS`); `name`, `number`; `send(status, data1 = 0, data2 = 0)`,
  `noteOn(channel, note, velocity = 100)`, `noteOff(channel, note)`, `control(channel,
  controller, value)`, `program(channel, program)` (channels 1 to 16, as players count them);
  `onMessage(IntIntIntCallback@)` (status, data1, data2; real-time and SysEx bytes skipped) in
  the loop; `close()`.
- **Not now:** a script's own virtual port (`/dev/midinew`), recording, real-time audio from a
  script (a script's callbacks can't keep a period's deadline).
- **The example:** `player.as`: a window with a file chosen (`chooseFile`), play, pause, stop, a
  position slider, the volume, the tags; a voice reading the title.
- **Tests:** the corpus: a voice's `speak()` texts told said in order (host), `voices()` and
  `midiPorts()` empty, the errors. `tests/board/audio.as`: a WAV made by the test (a tone),
  played, paused, sought, waited for, its finish called back; its tags; a voice speaking two
  texts in the loop; the wavetable port sent a note; the mixer read (and set and put back).

**6c, as built (2026-10-25):**
- **`audio.cpp`** as designed. A player's thread reads the decoder in 1024-frame pieces into a
  half-second stream; a voice's writes each text and its NUL to the speaker, writes the samples
  as they come, and drains the stream before telling the text heard, so each `onSaid` comes when
  it has been heard, not made. The threads share the object's lock with the script and one pipe
  to the loop; `settle()` joins a thread that ended. A stop or a seek drops what is queued, joins
  the thread, and drops once more: the thread may have written a piece after the first drop,
  before it saw the stop, and a seek's new position would otherwise play after it.
- **Kept while sounding:** at most 16 players and 16 voices. A script that never runs the loop
  still lets go of those that ended (swept when the list is full), unless a callback is still to
  come. `wait()` hears one out and tells nothing.
- **MIDI ports** are opened for the sides they have: a synth's port has only a destination, and
  opening it to read is refused (`ENOTSUP`), so `RDWR`, then `WRONLY`, then `RDONLY`. Programs'
  notes (`/dev/midi`) come out of `system`'s source, so a script hears angel's own `noteOn()`
  there (on channel 2, angel's).
- **`player.as`**: open, play and pause, stop, a position slider (sought when let go of), the
  volume, the tags (`TITLE`, `ARTIST`) and the codec in the status bar, "Read the title" by a
  `Voice`; a file can be given on the command line.
- **Tried on the board** (the staged angel; nothing in the kernel changed, nothing flashed):
  - `tests/board/audio.as` passes: a WAV it makes, played (the position between 0.1 and 0.6 s
    after 0.4 s), paused (still), sought to 0.5 s, waited for (at 1.0 s), played again with its
    end called back, looped past its length, `wait()` on a loop refused, stopped back at 0, let go
    of while playing; a text refused; `t.ogg`'s format, length and tags (Vorbis, 44.1 kHz,
    stereo, 139 kb/s, 30 s, `ENCODER`); the voices (`en-US`), one not there refused, two texts
    told in order in the loop, `say()`, `stop()`; the ports (`system`, `wavetable`), a note to the
    synth, angel's notes heard on `system`, a port and a channel refused; the mixer's volume
    changed and put back (the user's saved 0 dB still the state).
  - A first run of each part made the kernel's lasting state (speech and audio 14.5 KB of
    internal RAM, the MIDI ports 8.5 KB); three runs after that left internal RAM where it was.
  - `player.as` on `t.ogg`: playing, the position moving, paused with Alt+P, the title read.
  - The corpus: `audio.as` (a voice's texts told in order with no audio, the limits, no voices,
    ports, players or mixer on the host).
- **Size:** angel's text is 1,134,720 bytes (+20.2 KB: `audio.cpp` 20 KB).
