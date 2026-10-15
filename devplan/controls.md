# Controls: actions and bindings for every application, gamepads

Status: **step 1 built** (2026-10-15; planned 2026-10-14 with the user). Builds on `hotkeys.md`
(actions, chords, the `Hotkeys` table), which it extends from the system's keys to every
application's; used by the games (`games.md`) and the GUI toolkit.

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

## 7a. Step 1's design (2026-10-15)

- **The user's first pad:** an NES-style pad, `0079:0011` (DragonRise's generic chip: report
  protocol, Joystick collection, no boot interface, no strings). Its five axes are four X and a
  Y; the D-pad is axes 3 and 4 (0, 127, 255), the rest sit at 127; B is `b2`, A `b1`, Select
  `b8`, Start `b9` (found with `pad test`, 2026-10-15). Its line is in the database, with the
  SNES-style buttons the same chip usually has (X `b0`, Y `b3`, L `b4`, R `b5`) to be checked
  on the user's SNES-style pad.
- **`HidDescriptor`** (`bmds_input`, pure) is the parser `hotkeys.md` calls `HidLayout`: it
  walks a report descriptor and hands each input element (its application collection, report
  ID, bit offset and size, logical range, usage or usage range for arrays, flags) to a visitor.
  Nothing is stored, so each user keeps only what it needs. `MouseLayout` is rebuilt on it (its
  tests unchanged); the consumer and system collections (`hotkeys.md` §3.2) will be too.
- **`PadLayout`** (pure): the first Joystick, Game Pad or Multi-axis collection's axes (Generic
  Desktop X to Wheel, in descriptor order: `a0`, `a1`...; at most 8), hats (`h0`, `h1`) and
  buttons (the Button page, by number: `b0` is button 1; at most 32). `decode()` gives the
  **raw pad**: axes scaled to −32768..32767, hats as SDL's bits (1 up, 2 right, 4 down, 8 left),
  buttons as bits. Pads repeat usages (DragonRise's has four X axes before its Y), which is
  why axes are numbered by position, not named by usage.
- **`PadMapping`** (pure): the standard pad (§4a) from the raw pad. Written in SDL's
  `gamecontrollerdb.txt` format (`a:b1,dpleft:-a3,leftx:a0,lefty:a1~,dpup:h0.1`; SDL's `a`, `b`,
  `x`, `y` are by position: South, East, West, North), parsed into a fixed table, formatted
  back for `pad map`. Without an entry, the usual order (§4b) by usage: X/Y the left stick, Z/Rz
  (or Rx/Ry when there is no Rz) the right, hat 0 the D-pad, buttons 1-4 South, East, West,
  North, then L1, R1, L2, R2, Select, Start, L3, R3, Guide. Sticks get a dead zone (1/8);
  an axis read as a button is down past half.
- **The database** (`/rom/share/gamepads.txt`): SDL's line format, our own entries (axis numbers
  are ours, by descriptor order, so SDL's Linux and Windows lines don't carry over unchanged),
  `platform:BMDS` or no platform. Looked up by vendor and product (the GUID's bytes 4-5 and
  8-9), read in place (`Vfs::map`), so the lookup on the USB task never blocks. The user's own
  file (`/cfg/config/gamepads.txt`) comes with step 2, read off the USB task.
- **`Pads`** (`bmds_input`, kernel): up to 8 pads, numbered in the order connected; a slot keeps
  the vendor, product and hub port of the pad it last had, and a pad coming back takes its old
  number. Each holds its layout, mapping, last raw pad and standard pad (PSRAM, ~500 bytes a
  pad). A report becomes events only on change: a button's edge, an axis moved by 1/16 of its
  range or back to centre. Step 1's only consumer is a **listener** (a ring of events and a
  semaphore) for `pad test`; step 3 delivers them to programs.
- **`pad`** lists them (`PAD VID:PID KIND MAPPING AXES HATS BUTTONS NAME`); `pad test N` prints
  each raw and standard change as it happens until Ctrl-C (`b2 down -> South down`); `pad map
  N` prints the mapping as a `gamepads.txt` line; `pad descriptor N` dumps the report
  descriptor in hex, for a pad that needs a new entry.

## 7b. Pads to programs before actions: `BMDS_EVENT_PAD` (2026-10-15; agreed with the user)

Step 3 (actions matched by the system) is large; the pad's own events can reach games first,
through the event type the surfaces reserved (`games.md` §11). Blocks and Doom read them now and
keep reading them for what actions don't cover (which pad is which player).

```c
/* BMDS_EVENT_PAD: a pad's standard control changed, or a pad came or went (the focused
 * surface's). data[0]: the pad's number (pad0 is 0). */
#define BMDS_PAD_BUTTON   1u /* flags: data[1] a BMDS_PAD_SOUTH..., data[2] 1 down, 0 up */
#define BMDS_PAD_AXIS     2u /* flags: data[1] a BMDS_PAD_LEFT_X..., data[2] int32: sticks
                                -32767..32767 (right, down positive), triggers 0..32767 */
#define BMDS_PAD_ATTACHED 3u /* flags: the pad is there; data[1] vendor << 16 | product */
#define BMDS_PAD_DETACHED 4u /* flags: the pad is gone (its buttons came up first) */

/* Buttons, by position (devplan/controls.md §4a). */
#define BMDS_PAD_SOUTH 0u   /* Xbox A, PlayStation cross, Nintendo B */
#define BMDS_PAD_EAST 1u
#define BMDS_PAD_WEST 2u
#define BMDS_PAD_NORTH 3u
#define BMDS_PAD_L1 4u
#define BMDS_PAD_R1 5u
#define BMDS_PAD_L2 6u      /* the triggers as buttons: down past a quarter */
#define BMDS_PAD_R2 7u
#define BMDS_PAD_L3 8u
#define BMDS_PAD_R3 9u
#define BMDS_PAD_UP 10u
#define BMDS_PAD_DOWN 11u
#define BMDS_PAD_LEFT 12u
#define BMDS_PAD_RIGHT 13u
#define BMDS_PAD_START 14u
#define BMDS_PAD_SELECT 15u /* Back, Share, Minus */
#define BMDS_PAD_GUIDE 16u  /* the logo button: the system may keep it (§5d) */

/* Axes. */
#define BMDS_PAD_LEFT_X 0u
#define BMDS_PAD_LEFT_Y 1u
#define BMDS_PAD_RIGHT_X 2u
#define BMDS_PAD_RIGHT_Y 3u
#define BMDS_PAD_LEFT_TRIGGER 4u
#define BMDS_PAD_RIGHT_TRIGGER 5u
```

- **Only to the focused surface,** as keys are. On gaining the focus it is told of each pad
  attached (`ATTACHED`) and what is down or off centre on it; on losing it, what was down comes up
  and sticks go to centre, so nothing sticks.
- **The standard pad only;** raw controls stay with `pad test` (a pad that maps wrongly gets a
  database line, not a game's workaround).
- **No new system call.** Names and labels (which pad, its kind for "press Ⓐ") come with step 3's
  calls.
- **Inside:** `Pads` hands standard events to `Input`'s sink (a `pad` function beside `key`); the
  window manager queues them for the focused window (`WindowConfig::pad`); `ProgramSurface`
  makes them records. `libbmdsgame` passes them to the game's `event` callback as it does keys
  (nothing new in `game.h`).
- **The games:** Blocks (the user's, 2026-10-15): the D-pad's left and right (and the left stick)
  move, down drops, up holds; South, West and L1 turn left, East, North and R1 right; Start
  pauses, starts and keeps a typed name; Select leaves a paused game. Doom: its platform layer's key events from the pad (D-pad and left stick
  move and turn, South fire, East use, West run, North and the shoulders cycle weapons, Start the
  menu, Select the map), as SDL ports do; in the menus South accepts and East goes back. Doom's
  weapon cycling keys, unbound upstream, default to `[` and `]`.
- **Later** (steps 3-5, the user's "standard input assignment"): the games' pad bindings become
  their actions' default pad bindings, changed in the standard editor.

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
