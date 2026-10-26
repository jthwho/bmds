# The GUI Protocol

What a program and the GUI service say to each other: the reference for anyone writing a client
library, a server, or a program that speaks the bytes itself. Version **1.0**. Designed in
`devplan/guiprograms.md` (§4a, §8; the steps' notes say why things are as they are).

- **The model is X11's:** a program (the client) opens a connection to the service (the server)
  and writes requests; the server writes back replies, events and errors. Only requests that
  answer something make a client wait, so a window of 200 widgets is built in one `write()`.
- **Widgets, not pixels:** the server runs the system's toolkit for each connection. A client
  makes objects of named classes (windows, buttons, menus, a canvas), sets their properties,
  calls their methods and hears their events. Only a canvas carries pixels.
- **The same bytes locally and remotely:** a connection is a byte stream, from a program on the
  board or through SSH from another machine. A client can't tell which, and needn't.
- **Userspace ABI** (`docs/ABI.md`): everything here only grows. Requests, value tags, error
  codes, extensions, classes and names are appended; nothing is changed or removed.

The classes, each property, method and event with its type, are in
[gui-classes.md](gui-classes.md), generated from `sdk/gui/classes.def`. §10 says what they are for.

| Where | What |
|---|---|
| `components/bmds_core/include/bmds/guiprotocol.h` | The numbers, and the server's codec (C++, host-tested) |
| `sdk/include/bmds/gui.h`, `sdk/gui/gui.c` | `libbmdsgui`, the C client (C99; builds for BMDS and for other systems) |
| `sdk/gui/classes.def` | The classes: the one source of the class ABI |
| `sdk/include/bmds/guiclasses.h` | The C wrappers, generated from the classes |
| `sdk/include/bmds/tray.h` | The tray's messages on the service bus (§11.2) |
| `components/bmds_guiservice` | The server |
| `sdk/examples/guidemo.c`, `guigallery`, `guipanel`, `guisurface`; `sdk/tests/guitest.c` | Clients: the small one, every class, a panel, a canvas's surface, the board test |

## 1. Connections

A connection is a full-duplex byte stream. Three transports carry it; the bytes are the same on
each.

- **Local:** the system call `gui_open(display, flags)` (slot 124; `bmds_gui_open` in
  `bmds/sys.h`) gives a descriptor for a stream pair to the server: 16 KB each way, readable,
  writable and pollable as a pipe is. Display 0 is the board's screen.
- **TCP on the loopback:** while the setting `gui/listen` is `loopback`, the server listens on
  127.0.0.1, port `gui/port` (6400 by default), and nowhere else. Other machines reach it through
  SSH (`ssh -L 6400:localhost:6400 root@board`). A TCP client must present the cookie (§4.2).
- **A command's stdin and stdout:** the client library can run `/bin/sh -c COMMAND` with a socket
  pair as its stdin and stdout and speak through it. `ssh -W localhost:6400 root@board` is such a
  command: one SSH session, no port opened anywhere.

**Which display** (the client library's; `bmds_gui_connect(display)`, else the environment):

| `BMDS_GUI_DISPLAY` | Connects |
|---|---|
| unset, empty or `local` | `gui_open(0)` (on the board only) |
| `HOST:PORT` | TCP; `HOST` an IPv4 address or `localhost` |
| `\|COMMAND` | through `COMMAND`'s stdin and stdout |

A remote client's cookie is `BMDS_GUI_COOKIE` (its 64 hex digits, or a file holding them), else
`$HOME/.config/gui/cookie`.

**What doesn't cross:** a connection is the display's, not the machine's. A remote program's
files, sound, settings and processes are its own machine's; its windows, clipboard,
notifications and tray icons are the board's.

## 2. Encoding

- **Little-endian; every field 4-aligned.** Strings and byte blocks are padded with zeros to 4.
- **A request:**

  | Bytes | Field |
  |---|---|
  | 2 | `opcode` (§6) |
  | 2 | `flags` (only `Call`'s, §6.8) |
  | 4 | `length`: the whole message, header included; a multiple of 4 |
  | ... | the body |

- **From the server** (a reply, event or error):

  | Bytes | Field |
  |---|---|
  | 2 | `kind`: 1 reply, 2 event, 3 error |
  | 2 | `code`: a reply's request opcode; 0 for an event; an error's code (§8) |
  | 4 | `length`: the whole message; a multiple of 4 |
  | 4 | `sequence`: the request answered; for an event, the last request run before it |
  | ... | the body |

- **Sequence numbers:** the server numbers requests as they arrive, from 1 after `Hello` (which is
  0), in 32 bits. The client counts the same way, so it knows which reply or error is whose.
- **Lengths:** at most 256 KB (`MaxMessage`, also told at setup) either way. A length that isn't
  a multiple of 4, is shorter than its header or is past the largest ends the connection. Large
  pictures go in pieces (§6.10, §6.11).

## 3. Values

Properties, method arguments and results, and events' data are values: a `uint32` tag, then the
body.

| Tag | Name | Body |
|---|---|---|
| 0 | None | nothing |
| 1 | Int | `int32` |
| 2 | Long | `int64` (low word first) |
| 3 | Bool | `uint32`, 0 or 1 (anything else is a malformed value) |
| 4 | String | `uint32` length, UTF-8 bytes, padding |
| 5 | Atom | `uint32`: a name (§5.2) |
| 6 | Id | `uint32`: an object (§5.1); 0 is none |
| 7 | Rect | `int32` x, `int32` y, `uint32` width, `uint32` height |
| 8 | Color | `uint32` 0x00RRGGBB, `uint32` a palette role's atom (0: none; reserved, sent as 0) |
| 9 | List | `uint32` the elements' tag, `uint32` count, the elements without their tags |
| 10 | Bytes | `uint32` length, any bytes, padding (appended for the clipboard's data) |

- **Lists** hold one tag, never `List`. The classes' lists are all lists of Strings, and an empty
  list may give any element tag.
- **Where `Bytes` is wanted, a `String` is taken** (it is coded the same way).
- **A value of the wrong tag** for a property or an argument is `BadValue`; one that doesn't parse
  is `BadLength`.

## 4. Setup

### 4.1. `Hello`

The client's first message is the request `Hello` (opcode 0). Anything else ends the connection.

| Bytes | Field |
|---|---|
| 8 | `"BMDSGUI\0"` |
| 2, 2 | `major` (1), `minor` (0): the version the client speaks |
| 4, 4 | the authentication's name length, its data's length |
| ... | the name, padded; the data, padded |

The answer is a reply (code 0) to sequence 0:

| Bytes | Field |
|---|---|
| 4 | `status`: 0 accepted, 1 bad version, 2 refused, 3 busy |
| 2, 2 | the server's `major` and `minor` |
| 4 | `idBase` (accepted only, from here on) |
| 4 | `idMask` |
| 4 | the largest message (256 KB) |
| 4 | the extensions' count, then each: its name (String body), its value (String body), its first opcode (`uint32`), its first event code (`uint32`) |
| 4 | the display's count, then each: a name (String body) and a tagged value |

- **Refused and bad-version answers stop after the version,** and the server closes the
  connection. A client library reports refusal as `EACCES`.
- **The version:** the major never changes once shipped (a new major would be a new protocol); the
  minor grows as requests are appended. A client speaking a later minor works with an earlier
  server for what they share; it learns which by the server's minor and the extensions.
- **The display's values** today: `screen` and `workArea`, both Rect. More are appended by name.
- **The extensions** are told by name (§11). Today all of them are classes, so their first opcode
  and event code are 0.

### 4.2. Authentication

- **Local connections present nothing** (name and data empty): they are the board's own programs.
- **A TCP or command connection presents the cookie:** the name `BMDS-COOKIE-1` and 32 bytes of
  data. The cookie lives at the setting `gui/cookie` (by default `/home/.config/gui/cookie`), 64
  hex digits and a newline, made from the system's random numbers when the listener first starts
  and none is there. A remote machine takes a copy (`scp root@board:.config/gui/cookie
  ~/.config/gui/`).
- **The server reads the file at each `Hello`** (a replaced cookie applies at once) and compares
  in constant time. A wrong or missing cookie is answered `refused`.

## 5. Ids and names

### 5.1. Ids

- **The client chooses each object's id,** so `Create` needs no reply (X11's lesson). An id is
  `idBase | n`, with `n` within `idMask` and not 0. The server gives each connection its own
  base: the connection's slot plus one, shifted left 24 (`idMask` 0x00FFFFFF).
- **An id belongs to its connection:** another connection's id, one not made, or one already in
  use is `BadId`.
- **0 is none** (no parent, no image).
- **Freed ids can be used again** after `Destroy`. The client library hands ids out and reuses
  them.
- **The window manager's window numbers** (`Shell`'s events, §11.4) are another space: plain
  integers, not ids.

### 5.2. Names (atoms)

- **Classes, properties, methods, events and the values of enumerations** (`across`, `down`,
  `ok`, `clipboard`) are all names, sent as 32-bit atoms.
- **`InternNames`** (§6.1) gives a name its atom. Atoms are the server's, shared by every
  connection, and last as long as it runs: the same name is the same number for everyone.
- **A client interns what it uses once, at the start:** one round trip. A name is 1 to 255 bytes,
  matched exactly (case counts).

## 6. Requests

Replies are marked ◆. A request that fails is answered with an error (§8); the next ones still
run.

| Opcode | Request | Reply |
|---|---|---|
| 0 | `Hello` (§4.1) | ◆ |
| 1 | `InternNames` | ◆ |
| 2 | `NameOf` | ◆ |
| 3 | `Describe` | ◆ |
| 4 | `Create` | |
| 5 | `Destroy` | |
| 6 | `Set` | |
| 7 | `Get` | ◆ |
| 8 | `Call` | ◆ if `flags` asks |
| 9 | `Listen` | |
| 10 | `PutImage` | |
| 11 | `PutPixels` | |
| 12 | `Sync` | ◆ |
| 13 | `Bell` | reserved: `BadRequest` today |

Bodies below are in order; *N ×* repeats what follows.

### 6.1. `InternNames` (1) ◆

- **Request:** `uint32` count; *count ×* a String body (length, bytes, padding).
- **Reply:** `uint32` count; *count ×* `uint32` atom, in the same order.
- **Errors:** `BadLength` (more than 4096 names, or a body cut short: the bad value is the name's
  index), `BadValue` (an empty name or one over 255 bytes), `NoMem`.

### 6.2. `NameOf` (2) ◆

- **Request:** `uint32` count; *count ×* `uint32` atom.
- **Reply:** `uint32` count; *count ×* a String body.
- **Errors:** `BadLength`, `BadName` (an atom that isn't one).

### 6.3. `Describe` (3) ◆

A class's own table (a parent's entries are the parent's: describe it too). Programs and tools
use it to build bindings at run time, as `angel` does.

- **Request:** `uint32` the class's atom.
- **Reply:**
  - `uint32` the parent class's atom (0: none), `uint32` 1 if `Create` makes it (0: abstract);
  - `uint32` count; *count ×* a property: `uint32` atom, `uint32` tag, `uint32` flags (1
    readable, 2 writable);
  - `uint32` count; *count ×* a method: `uint32` atom, `uint32` the result's tag (0: none),
    `uint32` the arguments' count, then their tags;
  - `uint32` count; *count ×* an event: `uint32` atom, `uint32` 1 if sent by default, `uint32` the
    values' count, then their tags.
- **Errors:** `BadClass`.

### 6.4. `Create` (4)

- **Request:** `uint32` id, `uint32` the class's atom, `uint32` the parent's id (0: none),
  `uint32` count; *count ×* a property's atom and a tagged value.
- **Makes the object, then sets the properties in order,** as a `Set` would. Which parents a class
  takes is in §10.1.
- **Errors:** `BadId` (the id isn't the client's, is 0 within the mask, or is in use; or no such
  parent), `BadClass` (no such class, or abstract), `BadMatch` (that parent can't hold it),
  `BadName`, `BadAccess` (a property that can't be written), `BadValue` (its tag or value), the
  class's own (§10), `NoMem` (more than 4096 objects, or memory).
- **A property refused after the object was made** leaves the object made, with the properties
  before it set.

### 6.5. `Destroy` (5)

- **Request:** `uint32` id.
- **Destroys the object and everything under it,** its toolkit side first. Windows close; open
  dialogs close without an answer; offers on the clipboard are withdrawn; names on the service bus
  are released.
- **Errors:** `BadId`.

### 6.6. `Set` (6)

- **Request:** `uint32` id, `uint32` count; *count ×* a property's atom and a tagged value.
- **Every assignment is checked before any is made** (names, access, tags, enumeration values);
  then they are made in order, and one the class refuses stops the rest.
- **A change the client makes isn't told back to it** as an event (a view's current row, a
  slider's value), except where it moves something else (a row removed before the current one).
- **Errors:** `BadId`, `BadLength` (more than 256), `BadName`, `BadAccess`, `BadValue`, the
  class's own.

### 6.7. `Get` (7) ◆

- **Request:** `uint32` id, `uint32` count; *count ×* a property's atom.
- **Reply:** `uint32` count; *count ×* a tagged value.
- **Errors:** `BadId`, `BadLength`, `BadName`, `BadAccess` (a write-only property).

### 6.8. `Call` (8) ◆ if asked

- **Request:** `uint32` id, `uint32` the method's atom, `uint32` count; *count ×* a tagged
  argument. `flags` bit 0 (`WantResult`) asks for the result.
- **Reply** (only when asked): one tagged value (None for a method without one).
- **The arguments must match the class's declaration** in count and tags (up to 6).
- **Errors:** `BadId`, `BadName`, `BadValue` (the wrong arguments, or values the method refuses),
  `BadLength`, the class's own.

### 6.9. `Listen` (9)

- **Request:** `uint32` id, `uint32` on (1) or off (0), `uint32` count; *count ×* an event's atom.
- **Turns the object's events on or off.** Each event is on or off by default (the class says,
  §9.2); `Listen` overrides that for this object.
- **Errors:** `BadId`, `BadLength`, `BadName`, `NoMem`.

### 6.10. `PutImage` (10)

An `Image` object's pixels, in pieces.

- **Request:** `uint32` id, `uint32` width, `uint32` height, `uint32` format, `uint32` offset (the
  first pixel's index, row by row), `uint32` count; *count ×* a pixel.
- **Format 1 is ARGB8888:** a `uint32` 0xAARRGGBB a pixel, alpha not premultiplied.
- **A piece at offset 0 sets the size,** at most 512 × 512 (a new picture if it differs: users of
  the old one keep it). Later pieces must give the same size.
- **Errors:** `BadId`, `BadMatch` (not an `Image`), `BadValue` (another format, a size of 0 or
  over 512, pieces past the end or of another size), `BadLength`, `NoMem`.

### 6.11. `PutPixels` (11)

A `Canvas`'s pixels through the protocol (any connection; a local one can draw through a surface
instead, §11.3).

- **Request:** `uint32` id, `int32` x, `int32` y, `uint32` width, `uint32` height, `uint32`
  encoding, `uint32` the data's bytes; the data, padded.
- **Encoding 1, raw:** RGB565 as `uint16` a pixel, row by row.
- **Encoding 2, run-length:** pairs of `uint16` count (not 0) and `uint16` RGB565 pixel. Runs go
  on across rows.
- **The server keeps the picture,** so repainting the canvas costs the client nothing. A rectangle
  past the canvas's size (drawn before a resize the client hadn't heard of) is cut to the
  picture, not refused.
- **Errors:** `BadId`, `BadMatch` (not a `Canvas`), `BadValue` (another encoding, a size of 0 or
  over 4096, a run of 0), `BadLength` (too little data).

### 6.12. `Sync` (12) ◆

- **Request:** no body. **Reply:** no body.
- **Answered once every request before it has run,** so every error they caused has been sent
  before the reply. A client syncs to check that a batch worked, or before using an object that
  must exist (a canvas's surface).

## 7. Replies

A reply's `code` is the request's opcode and its `sequence` the request's. Replies come in the
order the requests were sent, with events and errors between them.

## 8. Errors

An error's `code` is one of these; its body is `uint32` the failed request's opcode and `uint32`
the bad value (the id, atom, count or index at fault).

| Code | Name | Means |
|---|---|---|
| 1 | `BadRequest` | no such opcode (or a reserved one) |
| 2 | `BadLength` | a body too short or too long for what it says; too many entries |
| 3 | `BadId` | not this connection's id, not there, or already in use |
| 4 | `BadName` | an atom that isn't one, or a name the class hasn't |
| 5 | `BadClass` | no such class, or one that can't be made |
| 6 | `BadValue` | a value of the wrong tag, out of range, or not among a property's names |
| 7 | `BadAccess` | not allowed: a read-only property written, an extension not given, a name taken |
| 8 | `BadMatch` | the objects don't fit together: a parent that can't hold that child, an object of another class |
| 9 | `NoMem` | the server is out of memory or of slots |
| 10 | `BadImplementation` | the server's own fault |

- **Requests after an error still run,** as in X11. A client that must stop at an error checks in
  its error handler; `libbmdsgui` calls one (`bmds_gui_on_error`), and `angel` reports
  the request with the script line that made it.

## 9. Events

### 9.1. The message

An event's `kind` is 2 and its `code` 0. Its `sequence` is the last request run before it, so a
client knows which of its requests an event follows. The body:

| Bytes | Field |
|---|---|
| 4 | the object's id |
| 4 | the event's atom |
| 4 | a timestamp: milliseconds since the board started (wraps) |
| 4 | the values' count, then the tagged values |

- **Events carry their data:** a field's text, a row, a key, a point. Nothing is left to fetch.
- **Events come in order** with the replies and errors.

### 9.2. Which are sent

- **Each event is on or off by default,** as the class declares ([gui-classes.md](gui-classes.md):
  *on* or *off*). Off events cost the link nothing until a client listens: pointer motion over a
  canvas, a field's every edit, a window's activation, the right press and the wheel on any
  widget.
- **`Listen`** (§6.9) turns an object's events on or off.
- **`Window.closeRequested` is off by default, and that matters:** a window whose client doesn't
  listen to it is closed when the user closes it, and told `closed`. One that listens is asked
  instead and stays open until the client closes it (X11's `WM_DELETE_WINDOW`).

## 10. The classes

[gui-classes.md](gui-classes.md) has each class's entries and types. This section says what they
are for, which parent each class takes, and the rules that aren't in the types.

### 10.1. Parents

| An object of | Is made under |
|---|---|
| `Window` | nothing (0); a window's `owner` property ties it to another |
| a widget | a container: a `Window` (its column), a `Box` or `TabPage`, a `FormRow` (its field), a `Splitter` (its sides), `Tabs` (pages only), a `Form` (a row of its own) |
| `TabPage` | `Tabs` |
| `FormSection`, `FormRow` | a `Form` |
| `ToolButton` | a `ToolBar` |
| `MenuBar` | a `Window` |
| `Menu` | a `Window` (a popup), a `MenuBar` (a title), a `Menu` (a submenu), or a `TrayIcon` (its menu) |
| `MenuItem` | a `Menu` |
| `Image` | nothing |
| the rest (`Timer`, dialogs, `Clipboard`, `Notification`, `TrayIcon`, `Services`, `Shell`) | nothing; a parent is ignored |

Children are placed in the order made. A `Splitter` takes two sides, and a third is `BadMatch`.

### 10.2. Windows

- **`Window`** is a top-level window with a column of children, flush with its frame (set
  `margin` and `spacing` for plain widgets). It shows at `show`, at its `width` and `height` or
  its children's size, centred in the work area or on its `owner`, or at its `placement` (`X Y W H
  [maximized]`, which it also reads back so a program can remember it).
- **What the user does to it is told:** `resized` (also once when first shown), `activated`,
  `closeRequested` (§9.2), `closed`, and `windowMenuChosen` for its own item in the window menu
  (`windowMenuItem`).
- **What a program does to it:** `minimize`, `maximize`, `restore`, `raise`; `above`, `shaded`;
  its decorations (`frame`, `titleBar`, `buttons`, `resizable`); `icon` (a theme name) or
  `iconImage`; the `pointer` shape over it (a busy window); `minWidth` and `minHeight`.
- **Owners and modality:** a window with an `owner` opens centred on it; a shown `modal` one
  disables its owner's widgets until it goes.
- **`level`** (`normal`, `desktop`, `panel`) and `reserve` (a panel's strip, in pixels) are set
  before `show`; the desktop and panel levels need the `SHELL` extension (§11.4).

### 10.3. Layouts and controls

- **`Box`** lays its children `across` or `down`; `Widget.stretch` shares the room left over, and
  a `Spacer` takes room of its own.
- **`Form`** makes rows of a label and a field (`FormRow`, its `label` and `help`), headed by
  `FormSection`s.
- **`Splitter`** has two sides and a handle (`position`, `minFirst`, `minSecond`); **`Tabs`**
  holds `TabPage`s (boxes with a `title`); `current` and `changed` say which shows.
- **Controls:** `Label` (`align`, `frame`), `Button` (`click`, `clicked`, `checked` drawn pressed
  in, an icon), `CheckBox` (`toggled`), `LineEdit` (`accepted` on Enter; `edited` on each change,
  off by default; `masked` for passwords), `NumberField` and `Slider` (`value` within `minimum`
  and `maximum`, by `step`), `ComboBox`, `ProgressBar` (Long values: a file's bytes fit).
- **Bars:** `ToolBar` with `ToolButton`s, `StatusBar` with fields (`addField(width)` answers the
  field's index), `MenuBar` (F10 opens it).
- **Every widget** has `enabled`, `visible`, `stretch`, `geometry` (where it is in its window's
  client area, as laid out), `setFocus`; `dropTypes` and `dropped` (§11.1); `contextMenu(x, y)`
  for a right press and `wheel(notches)`, both off by default.
- **Layout runs after each round of requests,** so a batch of changes lays out once.

### 10.4. Views

- **`ItemView`'s** subclasses (`ListView`, one column; `DetailsView`, columns under a header;
  `IconView`, 32-pixel icons flowing) hold rows of text: `items`, `addItem`, `removeItem`,
  `clear`, icons by theme name or `Image` (`setIcon`, `setIconImage`), `current` (-1: none),
  `multiSelect`; events `activated` (a double click or Enter) and `currentChanged`.
- **`DetailsView`** sets its `columns` (six at most) and adds rows of texts; a click on a header
  asks the client to sort (`sortAsked(column)`), which it does and shows with `setSortIndicator`.
- **`TreeView`** works by path: `addRoot`, then `expanding(path)` asks for a row's children, which
  the client gives with `setChildren` (none: a leaf) or promises with `setHasChildren`.
- **`TextEdit`** edits text (`text`, `modified`, the cursor, the selection, undo, the clipboard's
  keys); `changed` carries no text, so the client reads it when it wants it. **`CodeEdit`** adds
  a `syntax` by language id, `lineNumbers` and a colour `scheme`.
- **`Terminal`** shows what the client writes and tells what is typed (`typed`, as a
  pseudo-terminal would get it) and its size in cells (`sized`). The client runs its own program
  on its own pseudo-terminal: the server never starts a command for a client.

### 10.5. Menus, dialogs and timers

- **`Menu`s hold `MenuItem`s, separators and submenus,** in the order made. `popup(x, y)` opens a
  window's popup menu at a point in its client area. An item's `shortcut` ("Ctrl+Shift+S",
  "F5") chooses it from the keyboard when no widget used the key; `chosen` is its event.
- **`MessageBox`** (`kind`, `buttons`; `answered` with `ok`, `cancel`, `yes` or `no`),
  **`FileDialog`** (`mode`: `open`, `openMany`, `save`, `folder`; filters; `finished` with the
  paths) and **`ColorDialog`** (`finished` with the colour; `changed` while choosing, off by
  default) open with `open` and answer by event. A dialog's `owner` is the window it blocks while
  open. Destroyed open, a dialog closes without answering.
- **`Timer`**: `interval` (10 ms at least), `repeat`, `start`, `stop`; `timeout`.

### 10.6. Pictures

- **`Image`** holds ARGB pixels sent by `PutImage` (§6.10), for icons: a window's, a button's, a
  menu item's, a row's. Each user holds the pixels, so an `Image` destroyed while shown stays
  until the last user lets go, and its id is free at once.
- **`Canvas`** is a widget whose picture the client draws. Its size comes from the layout
  (`preferredWidth`, `preferredHeight` hint it); `resized` says when the picture is made again
  (white). Input comes as events: `pressed`, `released`, `moved` (off by default), `wheel` and
  `key` (a HID usage, the modifiers, down or up, and the text typed). Pixels arrive by `PutPixels`
  (§6.11), or through a local surface (§11.3).

### 10.7. The desktop's services

- **`Notification`**: one notification, shown with `show` and shown again in place by another
  `show`; `title`, `body`, `icon`, `urgency`, `timeout` (ms; 0 the system's, -1 until dismissed),
  up to three `actions`. Events: `activated(0)` for its body or 1 to 3 for a button, `dismissed`,
  `expired`. A notification outlives its object.
- **`TrayIcon`**: an icon in the panel's tray (`icon` or a 16 × 16 `iconImage`, `toolTip`,
  `category`, `visible`), up to four a connection. A `Menu` made under it opens at a right click.
  Events: `activated` (`trigger`, `doubleClick`, `middleClick`, `context`), `scrolled`,
  `availabilityChanged` (whether a tray is there).

## 11. Extensions

`Hello`'s answer names the extensions a connection has (§4.1). Each today is classes (and one
system call), with no opcodes or event codes of its own.

| Name | Given to | What |
|---|---|---|
| `CLIPBOARD` | every connection | the `Clipboard` class, drops (§11.1) |
| `SERVICES` | every connection | the `Services` class: the service bus (§11.2) |
| `CANVAS-SURFACE` | local connections | a canvas's surface by a system call (§11.3) |
| `SHELL` | programs `gui/shell-clients` names | the `Shell` class, the desktop and panel levels (§11.4) |
| `REMOTE-FILES` | reserved | a remote program's file dialog listing its own machine's files: not built |

A client checks for an extension before using it. A class whose extension the connection hasn't
is `BadAccess`.

### 11.1. `CLIPBOARD`

- **`Clipboard`** acts on the selections `clipboard` and `primary` (the drag's stays the window
  manager's). Types are MIME types (`clipboard.md`).
- **Copying:** `copyText(selection, text)` stores text now. `offer(selection, types)` offers 1 to
  8 types given on request: the event `requested(request, selection, type)` asks, and the client
  answers with `answer(request, data)` (in as many pieces as it likes) then `finish(request)`.
  An empty answer is none. A request not finished in 5 s is given up.
- **Pasting:** `paste(selection, type)` is answered by the event `pasted(selection, type, data)`
  or `pasteFailed(selection, type, why)`: `empty`, `notOffered` (nor convertible), `tooBig` (over
  a message), `own` (the client's own offer) or `failed`.
- **Also:** `clear` (only this object's copy), `types`, `owns`; the event `lost` when another
  program copies over this object's copy. The object destroyed, its offers are withdrawn; a text
  copy stays.
- **Drops onto any widget:** `Widget.dropTypes` lists the types a widget takes, in the client's
  order of preference. A drop is told as `dropped(type, data, x, y)` and is always a copy. Up to 4
  drops a connection are read at once.

### 11.2. `SERVICES`

The window manager's message bus between programs, by name.

- **`Services`:** `register(name)` (1 to 31 bytes; taken is `BadAccess`), `unregister`,
  `send(name, bytes)` (at most 4 KB; `BadName` if nobody has it, `BadAccess` if its queue is
  full), `has(name)`; the event `message(name, bytes)` for one of this object's names.
- **Watching:** `watch(name)` and `unwatch` (16 names an object), with the event
  `changed(name, present)` when one comes or goes.
- **Names** are reverse-domain or an application's id (`org.bmds.guigallery`); the system's are
  short words (`tray`, `files`, `open`). Names go with their object or connection.
- **The system's services:**
  - `tray`: the panel's tray. Its messages are `bmds/tray.h`'s structs; a tray icon's replies go
    to the service its message names.
  - `open`: the panel opens files as a double click on the desktop does. The message is
    absolute paths, a NUL after each.

### 11.3. `CANVAS-SURFACE`

- **`gui_canvas_surface(conn, canvas, config)`** (slot 125; `bmds_gui_canvas_surface`) gives a
  surface descriptor for a `Canvas`, for the same surface calls a game uses: buffers acquired and
  presented, and `surface_control`'s `IMAGE`, `BLIT`, `WAIT` and `INFO` (the 2D-DMA's copies).
- **`config`** is `bmds_surface_config_t`: `width` and `height` (both 0: the canvas's size now),
  `format` RGB565 or BGR24, `buffers` 2 or 3, `flags` `BMDS_SURFACE_FAST` alone. A title, a
  palette or a pointer mode is `EINVAL`; the canvas's events still come by the protocol.
- **Local only:** the descriptor must be the client's end of a `gui_open` connection; anything
  else is `ENOTTY`. Sync first, so the canvas exists and is laid out
  (`bmds_gui_canvas_surface_open` does).
- **The canvas paints the newest presented buffer;** a second surface replaces the first, and with
  the descriptor closed the canvas keeps its last picture.

### 11.4. `SHELL`

What a panel or a desktop does, for the programs the setting `gui/shell-clients` names (paths as
run, `;` between; empty by default). Remote connections never have it.

- **`Shell`** (one a connection):
  - **The window list:** `windowChanged(window, title, state)` for each application window, at
    once and whenever one opens or changes; `state` is a list of names (`focused`, `minimized`,
    `maximized`, `shaded`, `above`, `icon`). `windowClosed(window)` when one goes. `window` is the
    window manager's number.
  - **Acting:** `act(window, action)` (`raise`, `minimize`, `maximize`, `restore`, `close` (asked,
    as the user's close is), `shade`, `unshade`); `showWindowMenu(window, x, y)` at a screen point;
    `copyIcon(window, image)` puts a window's icon into an `Image`.
  - **The screen:** `workArea` and `workAreaChanged`.
  - **Keys:** `grabKey(usage, modifiers)` and `keyGrabbed` (a modifier alone is a tap, as the
    Windows key); a chord another holds is `BadAccess`.
  - **Launch feedback:** `programStarting(pid)` shows the starting pointer until the program opens
    a window.
- **`Window.level`** `desktop` and `panel`, and `reserve` (§10.2), need it too.

## 12. Flow and limits

- **A connection's requests run in order,** and its answers come in order.
- **Batching:** the client library buffers requests and writes them when its buffer is full,
  before waiting for a reply or an event, and when flushed.
- **Reading:** the server holds at most 256 KB of unread requests, then stops reading until it has
  run them.
- **Writing:** a client that reads nothing for 10 s while the server has answers for it is
  disconnected.
- **Ending:** a closed connection (the program's exit, a remote client gone) destroys all its
  objects.

| Limit | Value |
|---|---|
| Connections at once (local and remote) | 32 |
| Objects a connection | 4096 |
| Ids a connection | 16,777,215 (`idMask`) |
| Names in one `InternNames`, `NameOf`, `Get` or `Listen` | 4096 |
| Properties in one `Create` or `Set` | 256 |
| A method's arguments | 6 |
| A name | 255 bytes |
| An `Image` | 512 × 512 |
| A `PutPixels` rectangle | 4096 × 4096 (in pieces under the largest message) |
| A message | 256 KB |
| Drops read at once | 4 |
| Tray icons a connection | 4 |
| Service names an application | 8 |
| Names an object watches | 16 |
| A service message | 4 KB |

## 13. Settings

| Setting | Default | What |
|---|---|---|
| `gui/listen` | `off` | `loopback` takes TCP connections on 127.0.0.1 (followed live) |
| `gui/port` | 6400 | the port it listens on |
| `gui/cookie` | `/home/.config/gui/cookie` | the cookie remote clients present (§4.2) |
| `gui/shell-clients` | empty | the programs given `SHELL` (§11.4) |

## 14. The client library

`libbmdsgui` (`bmds/gui.h`), linked into SDK programs by `bmds-cc`, speaks all of this:
connecting by `BMDS_GUI_DISPLAY`, ids handed out, names interned once and cached, requests
batched, replies awaited with events queued meanwhile, errors to a handler, images and canvases
in pieces. `bmds/guiclasses.h` wraps every class in typed C functions (`bmds_button_new`,
`bmds_label_set_text`), over the generic `bmds_gui_new`, `bmds_gui_set_named`,
`bmds_gui_get_named` and `bmds_gui_call_named` for anything newer than the wrappers. It is
portable C99: only the transport differs by system, so it builds for Linux too.
