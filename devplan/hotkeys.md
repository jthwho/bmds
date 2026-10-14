# Hotkeys: Special Keys and Global Bindings

Planned 2026-10-08, not started. The user's request: support the keyboard's special keys (volume
up and down, mute, and the rest) through a general way of declaring the system's global hotkeys,
which the user can later set to do whatever they want. This document captures the design; the
user-editable part comes later.

`controls.md` (2026-10-14) extends this to every application's actions, with gamepads, and one
standard editor for all bindings; the chords, actions and table here are its base.

## 1. Goal

- **Special keys work:**
  - volume up, volume down and mute drive the default audio device;
  - play/pause, next, previous and stop go to whatever plays media;
  - later, the rest a keyboard may have (calculator, home, mail, sleep, power).
- **One mechanism for every global key.** A **binding** maps a **chord** (a key and its
  modifiers) to a named **action**. The built-in keys move onto it:
  - the window manager's keys (Alt+Tab, Alt+F4, Win+arrows, Ctrl+Shift+Win for a terminal);
  - the panel's Windows-key tap.
- **Configurable later:** bindings are data with defaults, so the user can rebind any action, bind
  a chord to run a command, or unbind a key, first through settings and later from a GUI. None
  of that is needed now.
- **Unbound keys go on to the focused window as today**, so a program may still use a special key
  itself.

## 2. Facts that shape the design (checked 2026-10-08)

- **Special keys aren't on the keyboard page.** Boot-protocol keyboard reports carry HID usage page
  0x07 only (`input.md` I3), and keyboards are kept in boot protocol. Media and system keys are
  sent elsewhere:
  - **Consumer page (0x0C):**
    - Volume Increment 0xE9, Volume Decrement 0xEA, Mute 0xE2;
    - Play/Pause 0xCD, Scan Next 0xB5, Scan Previous 0xB6, Stop 0xB7;
    - AL Calculator 0x192, AC Home 0x223 and many more. Usages run past 8 bits.
  - **Generic Desktop's System Control collection (0x01/0x80):** System Power Down 0x81, Sleep
    0x82, Wake Up 0x83.
  - **Where they arrive:** usually on a second HID interface that isn't a boot device (the
    `tusb_config.h` note: "a keyboard is often two"), already in report protocol. Some keyboards
    put them in the keyboard's own interface under another report ID, which boot protocol drops.
  - **Two report shapes:** consumer reports come as bit fields (one bit a usage) or as arrays (a
    field holds the usage pressed), so the report descriptor must be parsed.
- **`MouseLayout`** (`input.md` §5a) already parses report descriptors (globals with push and pop,
  usage ranges, extended usages, report IDs) for mice, and ignores reports with another ID.
- **`KeyEvent::usage` is 8 bits of the keyboard page.** Key events aren't in the SDK, so the event
  may change.
- **Global keys today:**
  - `WindowManager::grabKey()`: 8 claims, a key and exact modifiers, or a modifier alone as a
    tap. The panel claims the Windows keys with it.
  - The window manager's own keys in `shortcut()`, ahead of claims (`gui.md` §4g, §4k, §4p).
  - Both work only with the window manager running. The keyboard isn't a console without it
    yet (roadmap Phase 5).
- **The volume** is the `audio/dsp0/volume` and `audio/dsp0/mute` settings (`audio.md` §17), so
  an action only has to set them.

## 3. Design

### 3.1 One name for every key: the HID extended usage

- A key is identified by its **extended usage**: (page << 16) | usage, HID's own 32-bit form.
  - The keyboard's A is 0x00070004;
  - Volume Increment is 0x000C00E9;
  - System Sleep is 0x00010082.
- `KeyEvent` gains it alongside `usage`, which stays for the keyboard page so nothing that reads
  it changes. Keys of other pages have `usage` 0 (none) and no character, so windows that don't
  know them ignore them.
- **Chords are written as text names** (decided by the user, 2026-10-08): `Alt+F4`, `Win+F`,
  `Ctrl+Shift+T`, `VolUp`.
  - **Modifiers:** `Ctrl`, `Shift`, `Alt` and `Win`, joined to the key by `+`. Left and right
    count alike, as `grabKey()` treats them now.
  - **A tap:** a modifier alone (`Win`).
  - **Keys:** the keyboard's names (`A`, `F5`, `Tab`, `Space`, `Left`, `PageUp`, ...), and for
    special keys `VolUp`, `VolDown`, `Mute`, `PlayPause`, `Next`, `Prev`, `Stop`, `Calculator`,
    `Home`, `Power` and `Sleep`. A key without a name is written as its extended usage,
    `0x000C00E9`.
  - **Reading and writing:** read in any case, written in this canonical form.

### 3.2 Decoding special keys (`bmds_input`)

- **`HidLayout`:** `MouseLayout`'s parser generalized. It also finds the Consumer Control (0x0C/
  0x01) and System Control (0x01/0x80) application collections, with their fields as bit fields
  (Variable) or arrays (Array). Pure and host-tested on real keyboards' descriptors.
- **Events:** each report is compared with the last for that collection. Usages newly down become
  press events, those gone up release events; the keyboard's modifiers are the ones in force.
- **Repeat:** host-made like other keys (`input.md` I5), for the keys where it means something:
  volume up and down repeat; mute and play/pause don't.
- **Interfaces:**
  - A non-boot interface with these collections is used as it is (report protocol).
  - A keyboard interface whose descriptor also carries them under another report ID would have
    to leave boot protocol, with its keyboard reports then decoded through `HidLayout` too. That
    waits until a keyboard that needs it turns up.

### 3.3 Actions

- **An action:**
  - a dotted name, such as `audio.volume-up`, `audio.mute`, `wm.switch-next`, `wm.close`,
    `wm.snap-left`, `wm.terminal`, `panel.launcher` or `media.play-pause`;
  - an optional argument;
  - a function and the context it runs in.
- **Registration:** the component that can do something registers the action when it starts.
  - **Kernel actions** are declared with `BMDS_HOTKEY_ACTION` (rodata, a linker section, like
    `BMDS_SYSCONF`).
  - **Components and programs that come and go** (the window manager, the panel, a media player)
    register at run time and drop the action when they end.
- **Where an action runs:** never on the input task, which must not block.
  - Kernel actions run on a small kernel worker: setting the volume takes the settings' lock.
  - Window manager actions run on its task, as its keys do now.
  - A GUI program's actions are posted to its loop, as `GuiApplication::grabKey()` does now.
- **Built-in actions** (first set):
  - **`audio.volume-up` / `audio.volume-down`:** move `audio/dsp0/volume` (the default
    device's) within −60 to 0 by the step. Turning it up unmutes.
    - **The step:** a setting, `audio/volume-step` (int dB, 1 to 12, default **2**: the user's,
      2026-10-08). A binding's argument may override it.
  - **`audio.mute`:** toggle `audio/dsp0/mute`. The level is kept, as the tray's Mute button does.
  - **`run` (argument: a command line):** run it, as the launcher does (a terminal, a program, a
    script).
  - **`media.*`:** play-pause, next, previous, stop. Registered by whatever plays (later `play`
    and the synth). With none registered, the key goes to the focused window.
  - **The window manager's keys** become actions with today's chords as their defaults.
  - **`panel.launcher`:** a tap of either Windows key.
- **Saving:** a volume changed by a key is saved as the tray saves it: 2 s after the last change.

### 3.4 Bindings and matching

- **A binding:** a chord (an extended usage, the modifiers that must be down with left and right
  alike, and press or tap) and an action with its argument.
- **The defaults** are in rodata, next to the action they bind:

| Chord | Action |
|---|---|
| VolumeUp | `audio.volume-up` |
| VolumeDown | `audio.volume-down` |
| Mute | `audio.mute` |
| PlayPause, ScanNext, ScanPrevious, Stop | `media.*` |
| Alt+Tab, Alt+F4, Alt+Space, Alt+F10, Win+arrows | the window manager's |
| Ctrl+Shift+Win | `wm.terminal` (today `[Keys] Terminal`'s command) |
| Win (tap) | `panel.launcher` |

- **Matching:** a single table, consulted for each key event before it is delivered.
  - A chord whose action is registered runs it, and the key isn't delivered (its release
    neither).
  - A chord bound to an action nobody provides now (no media player) passes on.
  - Taps keep today's rule: a modifier released with no other key between.
  - The Alt+Tab switcher, once open, still takes every key until Alt is released, as now.
- **Where the table lives:** in the kernel (`Hotkeys`, a service of `bmds_input`), not the window
  manager. Volume and mute then work with the window manager stopped, and on a keyboard console
  when there is one.
  - The window manager's actions simply aren't registered while it isn't running.
  - `WindowManager::grabKey()` stays for programs, as a run-time binding of that program's
    action.

### 3.5 Making it the user's (later; designed so it needs no new mechanism)

- **Settings:**
  - one key per action, `hotkeys/<action>`, holding its chords (`Ctrl+Alt+T`, `VolumeUp`;
    several separated by `;`; empty to unbind);
  - user bindings under `hotkeys/user/<n>` (a chord and `run:COMMAND`);
  - the table follows them live (`SysConf::Watcher`), and the defaults are the declarations';
  - the chord syntax and the action names are frozen once shipped.
- **The `hotkeys` command:**
  - lists bindings, with the actions registered or not;
  - `hotkeys keys` shows the next key's extended usage and name, to find what a key sends;
  - `hotkeys bind CHORD ACTION [ARG]` and `hotkeys unbind CHORD` set the settings.
- **A GUI** (a settings program) on the same settings.
- **Programs** register actions through a system call once GUI programs have an ABI. Until then
  the built-in programs use the C++ API.

### 3.6 Feedback: an on-screen display like Alt+Tab's

Decided by the user (2026-10-08): on screen, as the Alt+Tab switcher is.
- **The look** (the switcher's, `gui.md` §4n): a raised box, centred low on the screen.
  - A 32-pixel speaker, with the tray's red circle and slash when muted.
  - The level as a row of blocks, as the progress furniture draws them.
  - Under it, in a sunken field, `-9 dBFS` or `Mute`.
- **Behaviour:**
  - Each press and repeat updates it, and it goes 1.5 s after the last.
  - It is on the `Tooltip` level (`gui.md` §4s), so it takes no keys and no clicks, and closes
    no menu.
- **Who draws it:** the window manager, as it draws the switcher, through a general call:
  - `showOsd(icon, text, fraction)`, for brightness and the like later;
  - the kernel's actions reach it through a sink the window manager registers, as input's key
    sink works, so `bmds_input` doesn't depend on the window system;
  - without the window manager there is no display, and so none.
- **When it shows:** for changes by keys. The tray's slider shows its own level.
- **Sound:** with `audio/feedback` on, a click at the new level, as desktops do.

## 4. Phasing

1. **Special keys:**
   - `HidLayout` (consumer and system collections; bit fields and arrays), host tests on real
     descriptors;
   - extended usages in `KeyEvent`;
   - `hotkeys keys` to see what each key sends.
   - **Check:** the user's keyboard's media keys show up.
2. **Hotkeys with built-in actions:**
   - the `Hotkeys` table in the kernel;
   - the audio actions;
   - default bindings in rodata;
   - the `hotkeys` listing.
   - **Check:** volume up, down and mute work, with the tray following.
3. **The window manager's keys and the panel's tap become actions,** and `grabKey()` a run-time
   binding. Behaviour unchanged; `win` tests still pass.
4. **The user's bindings:**
   - `hotkeys/*` settings, followed live;
   - `hotkeys bind`, `unbind`;
   - the `run` action.
5. **Later:**
   - media actions with a player;
   - system power and sleep (with a confirmation);
   - a GUI editor;
   - program actions through the ABI.

## 5. ABI impact

- **Frozen once shipped:**
  - the chord syntax and key names;
  - the action names and their arguments;
  - the `hotkeys/*` setting keys and `audio/volume-step`;
  - the `hotkeys` command's formats.
- **No system call before phase 5.** Key events aren't in the SDK, so widening `KeyEvent` breaks
  nothing.

## 6. Decisions (the user's, 2026-10-08)

- **Chords:** text names, such as `Alt+F4`, `Win+F` or `VolUp` (§3.1).
- **The volume step:** 2 dB, as the setting `audio/volume-step` (§3.3).
- **Feedback:** an on-screen display like Alt+Tab's (§3.6).

## 7. Open questions

- **Keyboards that send media keys only on the keyboard's own interface** (report protocol, under
  another report ID): whether to switch those interfaces out of boot protocol (§3.2). To discuss
  with the user when such a keyboard turns up; the trade-off is decoding the keyboard's reports
  ourselves (through `HidLayout`) instead of the fixed boot layout.
