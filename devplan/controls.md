# Controls: actions and bindings for every application, gamepads

Status: **planned** (2026-10-14, with the user). Builds on `hotkeys.md` (actions, chords, the
`Hotkeys` table), which it extends from the system's keys to every application's; used by the
games (`games.md`) and the GUI toolkit.

## 1. Goal

- **The keyboard is a first-class citizen** (the user's): everything can be done from the
  keyboard. Every action an application offers has a keyboard binding.
- **Gamepads, where they apply** (the user's): games and the desktop's few things that suit a pad
  get gamepad bindings, and gamepads work as well as the keyboard does.
- **One standard way to change bindings** (the user's), which every application uses: the same
  editor, the same files, the same rules, for the system's keys and for any application's.

## 2. Decisions (the user's, 2026-10-14)

- Gamepad support, absolutely. The user has an assortment of pads: when step 1 lands, all of
  them are tried (`pad test`), and their kinds decide the order of XInput and GIP in step 2.
- Keyboard bindings for everything; gamepad bindings where applicable.
- A standard way to change bindings that applications use.

## 3. Facts that shape the design (checked 2026-10-14)

- **`hotkeys.md`** (planned, not built) already has: keys named by their HID extended usage
  ((page << 16) | usage), chords as text (`Ctrl+Shift+T`, `VolUp`), dotted action names,
  `HidLayout` (the report-descriptor parser `MouseLayout` generalized), a `Hotkeys` table in the
  kernel matched before a key is delivered, bindings as data with defaults in rodata.
- **TinyUSB's host** here has 8 HID interfaces and 8 devices (`tusb_config.h`), with the mouse
  and keyboard in boot protocol; USB MIDI is a class driver of its own (`usbmidi.cpp`), so a
  vendor-class driver (XInput) can be added the same way.
- **Gamepads speak three ways:**
  - **HID** (Generic Desktop Game Pad 0x05 or Joystick 0x04): buttons (Button page 0x09), axes
    (X, Y, Z, Rx, Ry, Rz), a hat switch; the layout is in the report descriptor. Most third-party
    pads, PlayStation pads over USB, 8BitDo pads in their D-input and Switch modes.
  - **XInput** (Xbox 360 and many PC pads; class 0xFF): a fixed 20-byte report, no descriptor;
    LEDs and rumble by output reports.
  - **GIP** (Xbox One and Series pads): a vendor protocol that needs a start packet before it
    reports anything.
- **The button names differ by maker** (Xbox's A at the bottom, Nintendo's A at the right), so
  games bind by **position**, as SDL's game controller API and the W3C Gamepad standard layout
  do: South, East, West, North.
- **Which buttons a HID pad's numbers are** isn't in its descriptor: SDL keeps a database by
  vendor and product (`gamecontrollerdb.txt`, community-made, zlib licence) for that.
- **No Bluetooth on this board:** USB pads only (wired, or a USB dongle that presents one).

## 4. Gamepads (`bmds_input`)

### 4a. The standard pad

Every pad is presented as one layout, whatever it speaks:

| Group | Controls (names used in bindings) |
|---|---|
| Face buttons, by position | `South`, `East`, `West`, `North` |
| Shoulders, triggers | `L1`, `R1` (buttons); `L2`, `R2` (analog 0-1, and as buttons past a threshold) |
| Sticks | `LeftX`, `LeftY`, `RightX`, `RightY` (−1 to 1, a dead zone applied); `L3`, `R3` (clicks) |
| D-pad | `Up`, `Down`, `Left`, `Right` (a hat becomes four buttons) |
| The rest | `Start`, `Select` (Back, Share, Minus), `Guide` (Home, the logo button) |

- Bindings write a pad's control as `Pad.South`, `Pad.LeftX+` (a stick's half, as a button),
  `Pad.L2` and so on.
- Labels for what the user sees come from the pad's kind (Xbox: A B X Y; PlayStation: × ○ □ △;
  Nintendo: B A Y X by position), so an editor and a game's hints show the right ones.

### 4b. Decoding

- **HID pads:** `HidLayout` finds the Game Pad and Joystick collections and their fields; a
  mapping from the device's buttons and axes to the standard pad comes from (1) the mappings
  database by vendor and product, (2) else the usual order (buttons 1-4 as South, East, West,
  North; X/Y the left stick; Z/Rz the right; the hat the D-pad). The database: SDL's format, its
  entries for the pads that work, in `/rom` (`share/gamepads.txt`); the user's own entries in
  `/cfg/config/gamepads.txt` first. Pure and host-tested on real descriptors.
- **XInput:** a class driver of our own on TinyUSB's host (as `usbmidi.cpp` is); a fixed report,
  so no mapping. **GIP** after it (the start packet; then a report close to XInput's).
- **Reports to events:** each report against the last: buttons pressed and released, axes
  changed beyond a small step (no event per unchanged report: pads report every 4-8 ms whether
  anything moved).
- **Several pads, numbered** (no singletons): `pad0`, `pad1`... in the order connected, a number
  kept for a pad that comes back. A pad's events carry its number; a game assigns players.
- **Later:** rumble and LEDs (output reports; the player number on an Xbox ring).

### 4c. The `pad` command

`pad` lists pads (number, name, kind, vendor and product, the mapping used); `pad test N` shows
the controls live, as `hotkeys keys` does for keys, and the line for `gamepads.txt` when the
mapping is wrong. The first check for the user's pad.

## 5. Actions and bindings for applications

`hotkeys.md` binds chords to the system's actions. The same mechanism, opened to applications:

### 5a. Declaring actions

- **An application declares its actions,** each with:
  - a name (`game.rotate-left`, `edit.undo`), a label (`Rotate left`), a group for the editor
    (`Movement`, `Game`, `Edit`);
  - its kind: a **button** (pressed, released) or an **axis** (−1 to 1: a stick, or two keys or
    buttons as its halves);
  - **its default keyboard binding: required.** A declaration without one is refused. That is
    the rule that makes the keyboard first-class: nothing exists that only a pad or the mouse
    can do. An axis's keyboard default is a pair of keys.
  - its default gamepad binding: optional, where it applies.
- **Action sets** (Steam Input's idea, small): a game's `menu` and `play` sets may bind the same
  key to different things; the application says which set is active. Conflicts are checked
  within a set.
- **Where the declaration lives:** a file in the application's folder, `controls.conf`
  (`apps.md`), so the editor can show and change an application's bindings while it isn't
  running; programs not in a folder register the same table at run time.

### 5b. Bindings and matching

- **The defaults are the declaration's;** the user's changes are saved in the user's home,
  `$HOME/.config/bmds/controls/ID.conf` (the application's id, `apps.md` §6): only what differs.
  The system's own (`hotkeys/*`) stay system settings (`hotkeys.md` §3.5).
- **Matching is the system's,** in the `Hotkeys` table's place in the kernel: a key or pad event
  is matched against the system's bindings first, then the focused application's active set.
  A match is delivered to the application as an **action event** (the action, pressed or
  released or the axis's value, the device and pad number); a non-match goes on as the raw
  event it was. Applications that type text (a name for the high-score table) still read keys.
- **Why the system matches** and not each program: one editor works for every application, a
  change applies at once to a running one, and capturing a new binding ("press a key") works
  whatever the application is doing.
- **Text for menus and hints:** an application asks for an action's current binding as text
  (`Ctrl+Z`, `Pad.South` with the pad's label), so menus show the real shortcut and a game's
  "press Ⓐ" hint follows a rebinding. The toolkit's menus take their shortcut text from it.

### 5c. Changing bindings: one standard editor

- **The Controls window** (a built-in application, `/bin/controls`, through the public APIs
  only): a list of an application's actions by group, each with its keyboard and pad bindings;
  select one, press the new key or button (captured by the system), conflicts in the same set
  shown and offered to swap; Reset to defaults, per action and for all. The system's keys are
  one more "application" in it (`hotkeys.md` §3.5's GUI).
- **Reached from:** every application window's window menu (Alt+Space ▸ Controls...), when it
  has actions; `controls ID` from the shell; the application itself (a call that opens it).
- **For full-screen games** a window doesn't fit: the SDK's game library has the same editor
  drawn on the game's own surface, on the same calls (list, capture, set, reset), so it looks
  like the game and works with a pad alone.
- **A text form** too: `controls ID` lists, `controls ID bind ACTION KEY`, `unbind`, `reset`; the
  files are plain text for anyone who prefers an editor.

### 5d. The desktop and the pad

"Where applicable": a pad isn't a mouse, so the desktop gets only what suits it:
- **`Guide`** is the pad's escape hatch, as the keyboard's (`games.md` §3c): it leaves a
  full-screen game and opens the launcher. A pad alone can start and leave games.
- The launcher, menus and message boxes take `Up`/`Down`/`Left`/`Right`, `South` (accept) and
  `East` (back): the keys they already have, bound to the pad.
- The rest of the desktop (the file browser, the terminal) stays keyboard and mouse.

## 6. ABI impact

- **Frozen once shipped:** the standard pad's control names; the binding syntax (`hotkeys.md`'s
  chords plus `Pad.*`); `controls.conf`'s format; the per-application file's place and format;
  the action event; the `pad` and `controls` commands' formats.
- **System calls** (appended, designed with the surfaces of `games.md` §3e and brought to the
  user first): declare actions, choose the active set, read a binding as text, open the editor,
  and the editor's capture, set and reset for the in-game editor. Action and pad events come on
  the surface's descriptor (`games.md` §3d); a program without a surface gets them on a
  descriptor of their own.

## 7. Steps

1. **`HidLayout`** (with `hotkeys.md` phase 1): game pads and joysticks, host-tested on real
   descriptors; the standard pad, the default mapping, `pad`, `pad test`. **Check:** the user's
   pad.
2. **XInput** as a class driver (and GIP, if the user's pad needs it); the mappings database.
3. **Actions for applications:** declarations, sets, matching in the kernel, action events, the
   user's files; with `hotkeys.md`'s table (its phases 2-4).
4. **The Controls window,** the window menu's entry, `controls`; the toolkit's menus showing
   bindings.
5. **The game library's editor;** the games declare their actions (Blocks, Doom's keys mapped to
   actions in its platform layer, Solitaire).
6. **The desktop's pad navigation,** `Guide`.

## 8. Budgets (estimates)

- A pad: its layout (~200 bytes) and last report; events only on change.
- The bindings of the focused application: a small table (a few dozen actions), loaded when it
  declares them; matching is a lookup per event.
- The mappings database: in `/rom`, read when a pad without a known mapping connects; only the
  matched line is kept.

## 9. Open questions

- Mouse buttons as bindings (`Mouse.Left`, `Mouse.Wheel+`): useful for games (Doom's fire), cheap
  to add with the rest.
- Rumble's API (with the action events, or a separate call).
