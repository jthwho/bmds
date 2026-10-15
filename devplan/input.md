# Input: USB Host, Keyboard and Mouse

Phase 5 (`roadmap.md`). Started 2026-10-05; steps 1–4 done (§9).

## 1. Goal

A USB keyboard and mouse on the board's USB-A ports drive the desktop:

- Typed keys go to the focused window. A terminal turns them into the bytes a serial terminal
  would send, so its shell and programs work as they do over serial.
- The mouse moves a pointer. A click raises and focuses a window, a title bar drags the window
  (the outline move the window manager already has), and the close box closes it.
- The `usb` command lists what is plugged in, so a device that does not work can be told apart
  from one that is not seen.

Basic first: the HID boot protocol (every keyboard and mouse speaks it), one US layout, no
mouse acceleration. Report-protocol parsing, layouts, an input ABI for programs and other
device classes (storage, serial) come later (§8).

## 2. Facts that shape the design

- **The ports are behind a hub.** The four USB-A ports hang off a Terminus FE1.1s (a
  high-speed hub, one transaction translator) on the P4's high-speed OTG controller. Its reset
  is GPIO21 (active low); VBUS is switched by the hub (`docs/HARDWARE.md`).
- **Keyboards and mice are low or full speed.** Behind a high-speed hub they are reached with
  split transactions: the host talks to the hub's transaction translator at high speed, the hub
  talks to the device at its own speed. The DWC2 controller does splits in hardware per
  channel (`HCSPLT`), but only in buffer or slave mode, not in descriptor (scatter-gather) DMA
  mode (Linux's dwc2 refuses them there too).
- **ESP-IDF's USB host library cannot reach them** (tried first, 2026-10-05). v6 moved it to the
  component registry (`espressif/usb`); it runs the DWC2 in descriptor DMA mode and has no
  split transactions ("FS/LS devices behind a HS hub" is a documented limitation). Its 1.6.0
  `fsls_only` mode keeps the controller at full speed so the hub acts as a full-speed hub; on
  this board that enumerated the hub at full speed, but the Logitech keyboard and mouse (low
  speed) failed their first transfer (`ENUM: Bad transfer status 1: CHECK_SHORT_DEV_DESC`).
  Low speed behind a full-speed hub needs a preamble the P4's UTMI PHY does not seem to send;
  someone with the same board reported the same failure upstream (esp-usb PR #554). 1.6.0 also
  needs a newer USB HAL than our ESP-IDF snapshot has.
- **TinyUSB's DWC2 host driver does split transactions** (slave and buffer DMA modes), knows the
  ESP32-P4's two controllers (its root port 1 is the high-speed one), and has hub and HID host
  classes. With it, on this board, the keyboard and mouse enumerate at low speed through the
  hub at high speed (measured below).
- **The ROM has no USB host code** (it has miniz, used for screenshots: `display.md` §5d).

## 3. Decisions

- **I1. TinyUSB's host stack**, vendored in `thirdparty/tinyusb` (MIT, 0.21.0; only the host
  core, hub, HID host and DWC2 host files), built as `components/bmds_tinyusb` with our
  `tusb_config.h`. High speed, through the hub's transaction translator. ESP-IDF still sets up
  the UTMI PHY (`usb_new_phy`); TinyUSB drives the controller. Its callbacks are plain C
  functions on one task, which suits a small, efficient host better than the ESP library's
  client model (two tasks, queues). One root port: `MaxHosts` 1.
- **I2. Slave mode** (the CPU moves the controller's FIFOs), not DMA: keyboards and mice move
  a few bytes a millisecond, and slave mode needs no cache maintenance. Revisit for storage.
- **I3. TinyUSB's HID host in boot protocol** (its default: `SET_IDLE`, `SET_PROTOCOL(boot)` on
  boot interfaces). Reports go to our `Keyboard` and `Mouse`, which turn them into events.
- **I4. Keys are events, turned into text where they land.** A keyboard produces key events
  (the HID usage, modifiers, press/release/repeat, and the character the layout gives).
  The focused window decides what they mean: a terminal encodes them as VT sequences.
- **I5. Key repeat is made by the host** (boot keyboards report state, never repeats): 500 ms,
  then 30 a second, on the USB task: `tuh_task_ext()` waits at most until the next repeat is due,
  so repeat needs no timer of its own.
- **I6. The window manager owns the pointer.** Mouse motion accumulates in atomics the input
  side adds to; the window manager's task applies it once a round, so a 1 kHz mouse costs one
  round a frame, not a thousand. Clicks become raise, drag, drop and close on the same task,
  with the stack's own operations.
- **I7. A software cursor drawn last in each round.** Nothing in the scanout path can overlay
  a cursor (the GDMA reads the surface as it is). The cursor is a 2-colour mask the CPU writes
  into surface 0 after everything else in a round (a few hundred pixels, through the cache,
  then written back). When it moves, its old place is invalidated like the move outline's, so
  whatever is under it redraws itself; drawing it again is idempotent, so every drawing round
  just draws it.

## 4. Interface

A new component, `bmds_input` (target only, except the pure parts), between `bmds_drivers`
and `bmds_ui`.

- `UsbHost` (`MaxHosts` 1): `host(n)`, `start()` (the PHY, TinyUSB, the hub released),
  `devices(Span<DeviceInfo>)` (hubs included), `stats()`. One task, `usbN`, runs TinyUSB.
- `Keyboard` (`MaxKeyboards` 4, numbered as they come): boot reports diffed into
  `KeyEvent`s, repeat, the lock keys and their LEDs (`SET_REPORT`).
- `Mouse` (`MaxMice` 4): boot reports into `PointerEvent`s (dx, dy, buttons, wheel when the
  report has a fourth byte).
- `Keymap` (pure, host-tested): HID usage + modifiers + locks → a code point (US first;
  a table per layout).
- `Input` (pure types): `KeyEvent{usage, modifiers, action, codepoint}`,
  `PointerEvent{dx, dy, wheel, buttons}`, and one sink each, set by whoever consumes input
  (the window manager): `Input::setKeySink(fn, arg)`, `setPointerSink(fn, arg)`.
- `TerminalKeys::encode(KeyEvent, modes, out)` (pure, host-tested, in `bmds_input`): a key
  as a terminal's bytes. Text as UTF-8; Ctrl+letter → 0x01–0x1a; Alt+key → ESC key; Enter
  `\r`; Backspace 0x7f; Tab; Esc; arrows `ESC [ A`… (or `ESC O A` in application cursor
  mode); Home/End `ESC [ H`/`ESC [ F`; Insert/Delete/PgUp/PgDn `ESC [ 2~`/`3~`/`5~`/`6~`;
  F1–F4 `ESC O P`…, F5–F12 `ESC [ 15~`….
- `WindowManager`: `key(const KeyEvent &)` (queued, a ring of 32; drained by the task),
  `pointer(const PointerEvent &)` (added into atomics), `WindowConfig::key` (a window's key
  callback, on the window manager's task). `Terminal` gives its windows one that pushes the
  encoded bytes into its console.
- Shell: `usb` (the device tree: address, port, speed, VID:PID, class, strings), `usb stats`.

## 5. Design

```
USB ISR ─► usb0 task: tuh_task (enumeration, hubs, HID; repeat when due)
                       Keyboard ─► Input key sink ──► WindowManager::key ─► ring ─┐
                       Mouse    ─► Input pointer sink ► WindowManager::pointer ─► atomics
                                                                                   │
wm task, each round: drain keys ─► focused window's key callback (Terminal ─► pushInput)
                     apply motion and buttons ─► raise / drag / drop / close
                     draw ─► cursor last
```
- **Hot-plug** is TinyUSB's: a device that goes away unmounts its interfaces and the keyboard
  or mouse slot is freed. A keyboard unplugged with a key down releases it.
- **The pointer** starts in the screen's middle, is clamped to the screen, and is hidden until
  a mouse first moves. Press on a title bar: raise and start a drag (the grab offset kept);
  motion while held moves the outline (`drag()`); release drops it. Press on the close box,
  released on it: close. Press in a client area: raise. Later, client areas get pointer
  events too.
- **The cursor** is 12×19 (the classic arrow: black edge, white fill), on 4-pixel columns
  only in what it invalidates (the surface's alignment), drawn by `Surface::sprite()`: wait for
  the 2D-DMA, invalidate the cache lines under it, write, write back, mark the tiles drawn
  (double buffering copies them forward).

## 5a. Report-protocol mice (2026-10-05, the wheel didn't work; built)

- **The finding:** the user's mouse sends no wheel in boot protocol. A boot mouse report is only
  required to be 3 bytes (buttons, x, y); a wheel is a fourth byte on some mice and absent on
  others. TinyUSB sets every boot interface to boot protocol while enumerating, so a wheel was
  only ever seen on mice that add it there. The wheel was tested only with injected events.
- **The fix:** mice are switched to report protocol, and their reports decoded by the layout
  their report descriptor gives (TinyUSB hands the descriptor to the mount callback; its 256-byte
  enumeration buffer bounds it).
- **`MouseLayout` (pure, host-tested, `bmds_input`):** a parser of the descriptor's short items
  (the global state with push and pop, the local usages, usage ranges and extended usages, the
  main items) that finds the first application collection of a mouse (Generic Desktop, Mouse)
  and in it, for input reports, the buttons (the Button page, up to 8), X, Y and the wheel
  (Generic Desktop 0x38): each field's report ID, bit offset, size and sign. `decode()` turns a
  report with that ID into a `PointerEvent` (values clamped to the event's ranges); a report with
  another ID (a keyboard's or media keys' in the same interface) is not the mouse's.
- **On mount:** a boot mouse whose descriptor parses (X and Y found) is set to report protocol,
  and decoded by its layout once the switch is confirmed; until then, or when the descriptor
  doesn't parse, it is decoded as before. A HID interface that isn't a boot device but whose
  descriptor has a mouse collection (some gaming mice) is used too (it is in report protocol
  already). Keyboards stay in boot protocol.
- **Cost:** a layout is about 40 bytes, one per HID slot (8); parsing runs once per mount.

## 5b. A keyboard gone silent (2026-10-14, `games.md` §20a)

- The user's keyboard stopped mid-game (Doom in full screen), the Num Lock light with it: no
  reports reached `usb0`. The class driver asks again only when a report (or a failure) comes
  back, and TinyUSB's buffer-DMA channel handlers leave a channel halted when its cause is none
  they handle: the endpoint stays busy for good. The first local change to TinyUSB
  (`thirdparty/tinyusb/README.md`) retries such a halt as a transaction error (then fails the
  transfer, which the driver asks again) and counts it.
- `usb stats` shows the count (the last one's `hcint`), a `usb0.unhandled_halts` counter warns,
  and each HID interface is listed: waiting for a report or not, its reports, and how long since
  the last. If it happens again, `usb stats` from another machine says whether the transfer was
  lost below the driver.

## 5c. Silent again: a race in the host driver (2026-10-15, Solitaire)

- **Seen:** the keyboard stopped again while the user played Solitaire. `usb stats`: the
  keyboard (low speed, behind the board's high-speed hub, so every poll is a split transaction)
  had sent 8 reports, the last 21 minutes before, and was "waiting"; the mouse on the same hub
  was current; 0 refused requests and 0 unhandled halts. So the poll was asked for and never
  came back, with no error anywhere.
- **The cause found by reading `hcd_dwc2.c`:** an interrupt poll that gets NAKs is put off to a
  later frame, and the SOF interrupt starts it again (`edpt_xfer_kickoff()` from the handler),
  while the USB task starts transfers too (each mouse report asks for the next). Both take
  channels with an unguarded test and set (`channel_alloc()`) and change `haintmsk` by read,
  modify and write, and nothing serialised them (`hcd_int_disable()` frees the interrupt on this
  port, so TinyUSB's own guard was left commented out). An interrupt between a read and its
  write left a poll with its channel's interrupt off (or two polls on one channel): the
  controller finished it and nobody heard. The mouse, restarted every report, recovers; the
  idle keyboard waits forever.
- **Fixed:** one spinlock (`portENTER_CRITICAL_SAFE`) around the whole interrupt handler and the
  task's entries that touch channels or endpoints (open, close, transfer, abort, clear stall,
  device close). `usb stats` adds a line: the channels taken, those interrupting, and any taken
  but not heard (this bug's mark, if anything like it comes back). Not reproduced on demand: the
  window is a few instructions, hit after many thousands of polls.

## 5d. Keys held in Doom; a pad polled flat out (2026-10-15, the user's reports)

- **Doom kept walking forward after Up was let go** (windowed, maximized). A program surface's
  queue (64 events) dropped its oldest event when full, and mouse motion over a windowed game, a
  pointer event per report, could fill it while the game stalled (a level loading), pushing a key's
  release out. Now a move with the buttons unchanged merges into the move queued last (when it is
  the newest event), and a full queue drops its oldest move or wheel; a key, action or pad event
  only when it holds nothing else, logged (`ProgramSurface: events lost`).
- **The pad was polled ~270 times a second.** TinyUSB's DWC2 host keeps an interrupt endpoint's
  `bInterval` only to retry after a NAK; a report asked for again at once after data starts at
  once. Keyboards and mice NAK until something changes; DragonRise's pad always has a report, so it
  was read back to back, low speed through the hub's transaction translator beside the keyboard.
  `UsbHost` now asks for a pad's next report 8 ms after the last (`service()`'s timer).
- **Then Doom walked backwards for good**, with no event lost: `input.keys` counted 30 a second,
  the keyboard's repeat, and its last report was 66 s old. The release report itself was lost
  below the driver. A low-speed keyboard's reports cross the hub's transaction translator, which
  answers the keyboard on its own; a split that then fails loses the report, and the keyboard,
  answered, never sends it again. TinyUSB sets every HID interface's idle rate to 0 (reports on
  change only), so nothing corrected it. `UsbHost` now sends keyboards SET_IDLE 500 ms (the HID
  specification's default for keyboards) when they attach: the keys come again twice a second, a
  lost release is put right within half a second, and the same report again makes no events
  (checked: 2 reports a second idle, `input.keys` still 0). Mice keep 0: a repeated report would
  repeat its motion.

## 6. Resource budget (estimates; measured as the steps land)

| What | Where | Estimate / measured |
|---|---|---|
| TinyUSB state, buffers | Internal (.bss) | 3,326 bytes (measured) |
| `usb0` task stack | Internal | 4 KB (2,180 bytes left after enumerating a hub, keyboard, mouse) |
| `UsbHost`: devices, keyboards and mice | Internal (.bss) | 2,932 bytes (`bmds_input`, measured) |
| Key and pointer queues in the window manager | Internal | 32 × 12 + 16 × 6 bytes |
| Flash | | TinyUSB 17,462 bytes, `bmds_input` 6,767 (measured) |
| CPU | | an interrupt per report (125–1000 Hz), one wm round a frame while the mouse moves |

## 7. Steps

1. **USB host:** TinyUSB vendored, `bmds_input` with `UsbHost`, the hub released, the `usb`
   command. Done when a keyboard and a mouse show up in `usb` through the hub, at their
   speeds, plugged and unplugged.
2. **Keyboard:** `Keyboard`, `Keymap` (US), repeat and LEDs, `TerminalKeys`,
   key events into the window manager and the focused terminal. Done when a shell in a
   terminal window is used from the keyboard (BusyBox `vi` included).
3. **Mouse:** `Mouse`, the pointer and cursor, click to raise, title-bar drags, the close box.
4. **Screenshots** (`display.md` §5d), independent of the rest.

## 8. Later

- DMA mode for bulk devices (storage), with the cache maintenance TinyUSB's ESP32 port has.
- Report-protocol HID beyond mice (§5a did mice): media keys (planned in `hotkeys.md`), game
  controllers.
- Layouts beyond US; a `keymap` command and a setting.
- An input ABI for programs (raw key and pointer events, e.g. for a game or a paint program),
  and pointer events for windows' client areas.
- Keyboard and HDMI as a local console without the window manager (roadmap Phase 5).
- Mass storage and CDC serial devices.
- The window manager's affordances that build on input (resize handles, pointer shapes and
  hiding, the wheel, context menus, a panel): `windows.md` §8, step 8.

## 9. As built (2026-10-05)

- **Step 1, the host:** `components/bmds_tinyusb` builds the vendored TinyUSB 0.21.0 subset
  with `tusb_config.h` (root port 1, high speed, slave mode, 3 hubs, 8 devices, 8 HID
  interfaces). `UsbHost` (in `bmds_input`) holds the hub in reset, makes the UTMI PHY with
  `usb_new_phy()`, starts TinyUSB and its task (`usb0`, priority 4), and lets the hub go; the
  boot stage `usb` takes 40 ms or less. Devices come from TinyUSB's callbacks (a hub is never
  mounted for its user: it is recorded from its configuration descriptor and listed while
  `tuh_connected()`); strings are fetched after mounting. `usb` lists them:
  ```
  ADDR PORT  SPEED VID:PID   CLASS             NAME
  1    9.1   low   046d:c31c keyboard,hid      Logitech USB Keyboard
  2    9.2   low   046d:c077 mouse             Logitech USB Optical Mouse
  9    root  high  1a40:0101 hub
  ```
  The ESP-IDF library route (`espressif/usb` 1.6.0 vendored with a HAL shim and the
  `fsls_only` bit set before each port reset) enumerated the hub at full speed but not the
  low-speed devices behind it, and was removed (§2).
- **Step 2, the keyboard:** `KeyboardDecoder` and `Keymap::us()` (pure, host-tested): reports
  diffed into presses and releases (releases first, a rollover report ignored), the lock keys
  shared by all keyboards and sent to their LEDs, repeat on the USB task (`tuh_task_ext()`
  waits until the next is due). `Input` routes events to one sink, the window manager while it
  runs: keys are queued (32) and given to the focused window's `key` callback on its task.
  `TerminalKeys::encode()` (pure, host-tested) turns a key into xterm's bytes; `Terminal`
  pushes them into its console, application cursor keys from its screen's DECCKM. Typing in a
  terminal window verified by the user.
- **Step 3, the mouse:** boot reports into `PointerEvent`s; the window manager queues them
  (16, motion with unchanged buttons merged) and applies them each round: a press raises the
  window under the pointer, on the title bar grabs it (`drag()`'s outline follows, release
  drops), on the close box closes it when released there. The pointer (12×19, palette roles
  `cursor-edge` and `cursor-fill`) is drawn last in every drawing round with
  `Surface::patch()`: its 4-pixel columns read into internal RAM by the 2D-DMA, the arrow
  drawn there, written back (two jobs; the surface's rule that the CPU never writes it kept).
  Moving it invalidates where it was. `win pointer DX DY [BUTTONS]` feeds the same path for
  tests; `win check` skips the pointer's area and stays at 0 wrong pixels with it shown.
  Verified with the user's mouse: drags and raises through 1,503 reports.
- **Step 4, screenshots:** `display.md` §5d.
- **Report-protocol mice** (§5a, 2026-10-05): `MouseLayout` (`bmds_input`, pure). Host-tested
  with a classic wheel mouse's descriptor, a receiver's (a keyboard with report ID 1 before a
  mouse with ID 2: 16 buttons, 12-bit X and Y, a wheel, AC Pan; the keyboard's reports refused),
  the same mouse with 4-byte usages, every truncation of a descriptor, and 20,000 random ones.
  On the board the user's Logitech mouse (046d:c077, low speed) mounts as "3 buttons, a wheel"
  and is switched to report protocol; the wheel needs the user's hand to confirm.
