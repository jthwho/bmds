# Notifications, screenshots and the system's name

Planned 2026-10-10 (the user's request): notifications as an OS-level API that any program can
use, as the tray is; their first users the media player and Print Screen, which saves a
screenshot under the home's `Images/Screenshots` named by the system's host name, which the
system doesn't have yet. So three parts, built in that order of need: the system's name and
serial number, notifications, screenshots.

## 1. Goals

- **A name for the system:** a host name in the system configuration, by default `bmds-` and
  the serial number's last six characters; the shell's prompt shows it where it said `bmds`.
- **A serial number** the system and programs can read: the board's, read as its definition says
  (this board's is its chip's, from the eFuses).
- **Notifications** any program can post, through the SDK as well as the toolkit:
  - shown as balloons by the tray, stacked, for a few seconds;
  - kept: the last 64 in a notification centre in the panel, the unread counted;
  - a click runs the notification's target (a file or folder opened, an application started, a
    message to a service), and tells the program if it asked (an fd of events), with up to three
    action buttons;
  - a sound by urgency (the system's `Notify` sound), a setting turning sounds off.
- **Print Screen** saves the screen as a PNG under `~/Images/Screenshots/` and says so with a
  notification.

Non-goals for now: notifications over the network, images in notifications (a thumbnail of the
screenshot), Do Not Disturb schedules, per-application rules.

## 2. The system's name and serial number

- **The serial number is the board's** (the user, 2026-10-10: other hardware will have its own),
  so the board definition names its reader (`Board::serial`, a `BoardSerialReader`; `noSerial`
  by default). Readers boards share live in `bmds_board/src`: `chipMacSerial()`, the chip's
  factory MAC address (eFuse block 1, `MAC`, burned by Espressif for every chip;
  `esp_efuse_mac_get_default()`) as 12 lower-case hex digits, this board's (it has no serial of
  its own; the ESP32-P4 has no radio, but it has the field, for its Ethernet). The optional
  128-bit unique id (`OPTIONAL_UNIQUE_ID`) is left: it may be unburned. `System::serial()` asks
  the board once; up to 31 bytes.
- **`system/hostname`** (string, default empty, up to 63 bytes, RFC 1123: letters, digits and
  `-`, not starting or ending with `-`): empty means `bmds-` and the serial's last six letters
  and digits, in lower case (the user: six, not all twelve), so `bmds-e2e222` here; just `bmds`
  without a serial. `System::hostname()`
  answers with the default filled in; everything reads it there.
- **The shell:**
  - **`\h` and `\H`** in a prompt read the system's host name each time it is drawn (bash's
    `gethostname()`), so a change shows at the next prompt; the default prompt
    `\[\e[1;32m\]\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]> ` (it had `bmds` written in).
  - **`HOSTNAME`** (a shell variable, as bash's) and **`BMDS_SERIAL`** (exported, beside
    `BMDS_VERSION`) set as a shell starts.
  - **`hostname [NAME]`:** prints the host name; with `NAME` sets `system/hostname` and saves it
    (as `hostnamectl set-hostname`: a device's name is meant to stay). `hostname ''` goes back to
    the default.
  - **`version`** gains a last line, `Serial: a0b1c2d3e4f5`.
- **Programs (the SDK):** a new call, `system_info(bmds_system_info_t *)`: the serial, the host
  name, the board, BMDS's version, the cores and their clock (160 bytes; the serial 32, for other
  boards' longer ones). `linux.c`'s `uname()` answers
  `nodename` with the host name (so `gethostname()`, BusyBox's `hostname` and `uname -n` agree);
  `sethostname()` stays refused.

## 3. Notifications

### 3.1 What a notification is

| Field | |
|---|---|
| id | 32 bits, never 0; posting with `replaces` set updates that one in place (a player's "now playing") |
| application | its name (32 bytes), as the centre groups by it; the program's name if empty |
| icon | a theme icon's name (64 bytes); the program's own icon if empty |
| title, body | 64 and 256 bytes of UTF-8; the body may have `\n` |
| urgency | low (a balloon, no sound), normal (a sound), critical (a sound, a red edge, stays until dismissed) |
| timeout | ms; 0 the setting's default (`notify/timeout`, 6 s), `NoTimeout` until dismissed |
| target | none; open a path (by its type's application, a folder in a window); show a path (its folder opened with it selected); start a Desktop Entry by id; send a message to a service |
| actions | up to three buttons (an id each, a 24-byte label), told to the program's events |
| flags | silent (no sound), transient (not kept in the centre) |
| kept by the system | the sender's pid, when it was posted (real time), its state (shown, in the centre, closed), read |

### 3.2 The kernel's registry (`Notifications`, bmds_kernel)

- **The pure part, `NotificationLog`** (bmds_core, host-tested): the last 64 notifications in a
  ring (PSRAM, allocated with the first), ids, replacing, closing, read and unread, each change
  numbered (a viewer asks for the changes after the last it saw, as the problem log is read).
- **The kernel's** (one, a lock around the log): `post()`, `close(id, reason)`, `activate(id,
  action)`, `markRead()`, `list(after)`, and one **server** at a time (`setServer()`): the panel,
  told of each change through a function that posts to its loop (as a settings watch is). With no
  server the notifications are kept all the same, and shown when one comes.
- **Events to the sender:** a sender may attach a listener (a C++ `Notifications::Listener` with a
  function, or an fd from the SDK): activated (with the action, 0 the body), dismissed (by the
  user), expired (its balloon timed out; it stays in the centre), closed (by the sender, or
  replaced).
- **Who may close or replace:** the process that posted it (or anyone with its listener).

### 3.3 Programs: system calls (appended, slots 88-91)

- **`system_info(bmds_system_info_t *)`** (§2).
- **`notify_open(uint32_t flags)`:** an fd of events (`bmds_notify_event_t`: the id, what
  happened, the action), readable and pollable; closing it stops the events, the notifications
  stay.
- **`notify_send(int32_t fd, const bmds_notification_t *n)`:** posts (or with `replaces`,
  updates) one; `fd` its events' (-1: none, fire and forget). Returns the id.
- **`notify_close(uint32_t id)`:** closes one of the caller's.
- **`linux.c`:** nothing to emulate (Linux's are D-Bus's); a program uses the calls.
- **Shell:** `notify [-u low|normal|critical] [-t MS] [-i ICON] [-a APP] [-o PATH | -s PATH] TITLE
  [BODY]` posts one (as `notify-send`), printing its id; `notify list` the centre's, `notify close
  ID`, `notify clear`.

### 3.4 The panel: balloons and the centre

- **The server** is the panel's (`NotificationServer`), while it runs.
- **Balloons:** a window each (a new window manager level, `Notification`: above applications
  and the panel, below menus; not focusable, a press doesn't take the focus), 320 pixels wide, in
  the corner nearest the tray (top right with the panel at the top), the newest nearest the
  panel; at most three at once, the rest queued. Each: the icon, the application's name, the
  title, the body (wrapped, four lines at most), a close box, the action buttons. A click on the
  body runs the target and tells the sender; the close box dismisses it. Its timeout waits while
  the pointer is over it. A critical one stays until dismissed and has a red edge.
- **The centre:** a tray icon (category System, a bell; a dot while some are unread). A click
  opens the list, newest first (icon, title, the body's first line, how long ago), each clickable
  as its balloon was; Clear All. Shown, they are read.
- **Sound:** normal and critical play `SystemSound::Notify` unless silent or `notify/sound` is
  off.
- **Settings:** `notify/sound` (bool, true), `notify/timeout` (ms, 1,000 to 60,000, 6,000),
  `notify/quiet` (bool, false: Do Not Disturb until turned off),
  `notify/balloons` (bool, true: false keeps them in the centre only), `panel/tray/notifications`
  (bool, true: the centre's icon, as the meter's).

- **The centre's menu** (a right click on its icon; the user's ask after trying it, 2026-10-10):
  - **Show Balloons**, **Play Sounds** (checked as set): `notify/balloons`, `notify/sound`;
  - **Balloon Time** ▸ 3, 6, 10 or 20 seconds (`notify/timeout`; the one in force checked);
  - **Do Not Disturb** ▸ For 10 Minutes, For 1 Hour, For 8 Hours, For 24 Hours, Until Turned Off;
    Turn Off (while on);
  - **Clear All**.
  - Each setting saved at once, as the panel's menu's.
- **Do Not Disturb:** while on, notifications go straight to the centre, unread, with no balloon
  and no sound; a critical one still shows (and sounds): a full disk shouldn't wait for 8 hours.
  The centre's icon is drawn at half alpha, its tooltip saying until when ("Do Not Disturb, 52
  more minutes"; "Do Not Disturb").
  - **Until Turned Off** is a setting (`notify/quiet`, bool, false), kept across restarts.
  - **The timed ones** count on the monotonic clock in the panel, and end with it (a restart of
    the panel or the board): the real-time clock may not be set at boot, so a deadline in real
    time could end at once or never.
  - **As built (2026-10-10):** seen on the board: the menu and its submenus (opening leftward by
    the screen's edge), For 10 Minutes holding a normal notification back to the centre while a
    critical one showed, the icon faded and its tooltip "Do Not Disturb until 6:26 AM", the menu
    saying "Do Not Disturb (on)" with Turn Off enabled, Until Turned Off saved, Balloon Time's 10
    seconds saved; each put back to its default after.

### 3.5 The toolkit

- **`Notifier`** (bmds_gui; not `Notification`, which is the kernel's struct it posts): `show(n)`
  (`replaces`: one of its own), `close(id)`, `setTold(fn, arg)`, told on the application's loop
  (its listener queues the events and posts). A notification without an application's name gets
  `GuiApplication::applicationName()` (new, `setApplicationName()`, Qt's display name). An
  in-process program doesn't know its pid, so its notifications are owned by its listener
  (`Notifications::listenerOwner()`, a negative number below -1, never a pid).
- **`TrayIcon::showMessage(title, text, icon, timeoutMs)`** (the media player's plan, §6e step 10):
  a notification through the icon's own `Notifier`, the last message replaced (Qt's), and
  `setMessageClicked()` told of a click on it.
- **The media player:** "Now playing" when a track starts while its window is hidden: the title,
  the artist and album, its icon, low urgency, silent and transient (a track list in the centre
  would be clutter), replacing the last; a click shows the player (which closes it), the buttons
  are Previous, Pause (Play while paused) and Next. `mplayer` names itself "Media Player".

## 4. Screenshots

- **`Screenshot::save(path, level, stats)`** (bmds_ui): the `screenshot` command's capture (the
  window manager's last whole round) and PNG writing, shared; the command keeps its output.
- **Print Screen** (HID 0x46) is the panel's (`grabKey`, as the Windows keys): the screen is
  captured at once (the copy, about 10 ms) and written by a thread of the panel's (the PNG takes
  longer), to `$HOME/Images/Screenshots/<host>_<YYYY-MM-DD>_<HH-MM-SS>.png` in local time (no `:`:
  FAT), the directories made, `_2` and on for a second in the same second.
- **Then a notification:** "Screenshot saved", the file's name, its target to show the file in
  its folder; a failure says why (critical).

## 5. Phasing

1. **The name and serial** (done 2026-10-10): `System::serial()`, `system/hostname` and
   `SysConf::hostname()` (the pure `HostName`, host-tested), `hostname`, `\h` read live, the
   default prompt, `HOSTNAME` and `BMDS_SERIAL`, `version`'s `Serial:` line, `system_info` (slot
   88), `uname`'s nodename; the board's serial reader. Seen on the board: serial `e8f60ae2e222`
   (Espressif's `e8:f6:0a`), the prompt `bmds-e2e222:/>`, `hostname studio` changing the next
   prompt, a name with `_` refused, `hostname ''` back to the default with nothing stored;
   `progtest system` passes (`uname -n` and `gethostname()` agree, an older program's smaller
   struct gets what fits).
2. **The registry** (done 2026-10-10): `NotificationLog` (bmds_core, host-tested: posting, order,
   replacing, a balloon's end, closing, transient, Clear All, a full log, texts cut at a
   character), `Notifications` (bmds_kernel, a boot stage; host-tested with a server, a listener
   and an events fd), `notify`. On the board: `notify`, `notify list`, `notify close`.
3. **The calls** (done 2026-10-10): `notify_open` (an events fd, `Stream::notificationListener()`),
   `notify_send`, `notify_close` (slots 89-91), `bmds_notification_t` (772 bytes),
   `bmds_notify_event_t` (16, the kernel's `EventRecord`). `progtest notify` passes; with
   `served` (the panel running) its balloon's expiry is read from the fd.
4. **Balloons** (done 2026-10-10): the window manager's `Notification` level (between `Panel` and
   `Popup`; the stack orders by level), `NotificationServer`, `NotificationBalloon`, targets
   through the panel (`FolderActions::open()`, `openFolder()` with the file selected,
   `openWith()`, a service's message), actions, the `Notify` sound. The toolkit's windows gain
   `pointerEntered()` and `pointerLeft()` (anywhere in the window, its widgets too): a balloon's
   time waited only over its own background before, not over its buttons.
5. **The centre** (done 2026-10-10): its tray icon (the theme's information sign, a red dot drawn
   on it while some are unread; its tooltip the count), the list (newest first, a red mark on the
   unread, scrolled by the wheel, a close box each, Clear All), read once it closes (so the marks
   show while it is open), the settings, the panel's menu's Show Notifications.
6. **The toolkit and the player** (done 2026-10-10): `Notifier`, `TrayIcon::showMessage()`,
   `GuiApplication::setApplicationName()`, the player's "Now playing". Seen on the board: two
   balloons stacked (the newest by the panel, the critical one red-edged, the other's body
   wrapped), the normal one expiring after 6 s, the critical one staying until its close box, the
   centre listing both; the player hidden, Next from its tray menu: "Media Player · t.ogg" with
   its three buttons; Pause paused it (the time held at 0:12), staying 12 s under the pointer; a
   click on the body showed the player.
7. **Screenshots:** `Screenshot::save()`, Print Screen, its notification.

## 6. ABI impact

- **Calls** (appended, slots 88-91): `system_info`, `notify_open`, `notify_send`, `notify_close`,
  with `bmds_system_info_t`, `bmds_notification_t`, `bmds_notify_event_t` and their constants.
- **Settings:** `system/hostname`, `notify/sound`, `notify/timeout`, `notify/balloons`, `notify/quiet`,
  `panel/tray/notifications`.
- **Shell:** `hostname`, `notify` (their output frozen); `version` gains a line; `HOSTNAME` and
  `BMDS_SERIAL`; the default prompt shows the host name.
- **Files:** `~/Images/Screenshots/` and its names.
- **`uname()`'s `nodename`** is the host name (it was `bmds`).

## 7. Budget

- The log: 64 × about 800 bytes (a notification's texts are most of it), 52 KB of PSRAM,
  allocated with the first notification.
- A balloon: a window of 320 × about 100 pixels, while shown; the centre's list while open.
- Print Screen: the screen's copy (3.5 MB of PSRAM at 1280 × 720) while the PNG is written.

## 8. Revisions

- **2026-10-10, steps 2-6 as built:** the toolkit's class is `Notifier` (the kernel's struct is
  `Notification`); toolkit programs' notifications are owned by their listener (they don't know
  their pid); the centre marks what it showed read when it closes, not when it opens; a balloon
  waits under the pointer anywhere in it (`GuiWindow::pointerEntered()`); text and icons in the
  centre's rows start on 4-pixel columns (the painter draws nothing elsewhere: the first rows
  came out empty).

## 9. Decisions (the user, 2026-10-10)

- **An OS-level API**, as the tray, the media player its first user and Print Screen its second.
- **Clicks:** targets the system runs, and events for programs that want them (actions).
- **History:** the last 64, in a centre.
- **Sound:** by urgency, a setting.
- **Placement:** by the tray, stacked.
- **Screenshots** in `Images/Screenshots/${HOSTNAME}_${DATE}_${TIME}.png` under the home.
- **The host name** in the system configuration, by default `bmds-<serial>` (its last six, the
  user decided after step 1), in the prompt; the serial read as each board says.
