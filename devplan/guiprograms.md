# GUI Programs: the Toolkit for Every Program, and Angel's Bindings to the Whole System

Status: **step 1 built** (2026-10-22; §9): the protocol's spine, `gui_open`, the service, five
classes, `libbmdsgui` and `guidemo` on the board. Next: step 2, the classes. The user agreed with the shape (2026-10-22): the
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
- **Found while checking for leaks, not the GUI service's:** each shell on a pty that ends by
  hang-up leaves its loader `Running` record (about 850 bytes); in the gallery's case a 16 KB
  `Pipe` too. The built-in terminal shows it as well (`win term`, then closed: one
  `Running` a time, by `mem mark` / `mem new`). To be followed up in the loader.
- **Size:** `bmds_guiservice` 41.9 KB of flash (33.7 KB after 2d).
