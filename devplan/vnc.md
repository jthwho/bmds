# Remote Desktop: the Display Off, Screen Capture and Input for a VNC Server

Planned 2026-11-16 (the user's ask: a VNC server for BMDS, in bmds-apps, not a core app; the OS
gains what it needs, and the hardware JPEG encoder is used where it fits; then the user's
follow-up: the HDMI output turned off while everything runs as if it were on, to give the scan's
PSRAM bandwidth back; and the user's simpler way to do it, the scan kept running on black, §5a). The server itself is bmds-apps' `devplan/vnc.md`; this plan is what BMDS
gives it, in a general form any program allowed it can use (built-in applications aren't
special: a screen recorder or a remote-support tool uses the same calls).

Status: **approved** (2026-11-16, §10); **steps 1-4 built and tried on the board** (§11, §12).

## 1. Goal

- **The display's output off, the desktop running:** the scan keeps running but reads black from
  a few bytes of internal RAM instead of the picture, and the HDMI transmitter goes off (the
  monitor sleeps). Surface 0, the window manager and every program carry on as if it were shown,
  paced by the same frames. The scan's PSRAM reads (166 MB/s at 720p) go to everything else.
- **The screen's changes and pixels for a program:** which parts changed since it last looked,
  and those parts copied into its own buffer, whole frames only, without the CPU reading PSRAM.
- **Input from a program:** keys and an absolute pointer, entering where a USB keyboard's and
  mouse's do, so hotkeys, controls and the pointer's modes behave as for the local user.
- **Encoding into memory:** `image_encode` (PNG and the hardware JPEG) into a buffer, not only a
  file.
- **The user always knows:** while any program watches the screen, the system shows it in the
  tray, whoever the program is.

Not in it: the VNC protocol (bmds-apps), display modes the HDMI link can't carry while the output
is off (§9), audio.

## 2. Facts that shape it

- **The scan reads surface 0's shown buffer straight from PSRAM** (`display.md` §5c): RGB888,
  166 MB/s at 720p60. The CPU's PSRAM traffic can't be ranked below the DMAs'
  (`docs/platforms/esp32p4.md` §3), so every other PSRAM user shares what the scan leaves.
- **Without a scan, a present swaps at once and `waitFrame()` times out** (`Surface`,
  `display.md` §5c). Anything paced by frames (the window manager's rounds, `SwapChain`, program
  surfaces, games' frame loops) would run unpaced: as fast as it can draw, the CPU busy for
  nothing.
- **`display off` stops everything:** `Display::stop()` and the bridge's output off, refused
  while the window manager has the display (`displaycommand.cpp` `runOff`). There is no state
  where surface 0 lives and nothing scans it.
- **Everything above the scanout leans on it:** frame counts (`SwapChain`, the window manager's
  pacing), `frameTime()` (the sequencer), `surface()`, `show()` and `forget()` (program
  surfaces), swap holds (screenshots). Keeping the scan running keeps all of them as they are.
- **The scan already restarts cleanly with the link up** (`haltAtFrameEnd()`, `stopStream()`,
  `startStream()`, `display.md` §5f): 30 switches with 0 underruns, the monitor keeping its
  picture. Changing what the descriptors read is such a restart.
- **A DW-GDMA descriptor has its own source-increment bit** (`ctrl_lo.sinc`,
  `dw_gdma_ll_lli_set_src_burst_mode()`): fixed, a block reads the same 8 bytes again and again.
- **The LT8912B's HDMI transmitter turns off on its own** (`Lt8912b::setOutput()`, register
  0x33), the DSI receiver still taking the stream.
- **Surface 0 already tracks what changed:** a double-buffered surface keeps a bitmap of 32×16
  tiles changed since the last present, to copy forward after a swap (`display.md` §5c). 32×16
  is a whole number of the JPEG encoder's 4:2:0 MCUs (16×16).
- **The scan doesn't always show surface 0:** a program's full-screen surface is flipped to
  (`games.md`: the zero-copy flip), and the scan's line mode repeats rows. A capture must copy
  what is shown, not surface 0.
- **The JPEG codec** (`JpegCodec`, `images.md`): `Bgr24` in without a copy; 1280×720 in 45 ms,
  1920×1080 in 94 ms; one codec for the system (the image viewer and Preso share it); sides
  padded to the MCU; it stops at its output buffer's end and reports success
  (`esp32p4.md` §4).
- **`image_encode` (slot 95) writes to a descriptor only.** `bmds_image_encode_t` begins with
  `size`, so fields can be appended.
- **The GUI protocol's `SHELL` extension** (`guiprograms.md` §4i, P15) is the privileged part
  (`gui/shell-clients`, local connections only unless `gui/shell-remote`). It planned
  `Capture(rect)`, pixels sent in the stream, and left it for "when something needs it"
  (§12 4c). A VNC server needs it, and needs it cheaper than a copy through a byte stream.
- **Keys can be injected inside the system only** (`win hid`: `WindowManager::key()`), below the
  input layer's hotkeys and controls. **No pointer is absolute:** USB mice are relative
  (`input.md`); a VNC client sends absolute positions.
- **The network:** 100 Mbit/s Ethernet (12 MB/s); a whole 720p frame is 2.7 MB raw. The work is
  in choosing and encoding what changed, not in the wire.

## 3. The shape

```
 window manager, programs ──draw──▶ surface 0 (PSRAM)      ┌─ HDMI on:  scan (DW-GDMA), swaps at its frame start
                                     │ tiles changed ──┐    └─ HDMI off: scan reads 8 bytes of black, swaps the same
                                     │                 ▼
                                     │         watchers' tile bitmaps (one each)
                                     │                 │
 VNC server (bmds-apps) ─SHELL: watchScreen ───────────┘ event: screen changed (no pixels)
         │             ─gui_screen_capture(conn, …) ──▶ the changed tiles' rectangles copied by
         │                                              2D-DMA from the shown picture into its buffer
         │             ─image_encode(-1, {out=…}) ─────▶ JpegCodec (hardware) into its buffer
         └──────────────SHELL: injectKey / injectPointer ──▶ input layer (a virtual keyboard and tablet)
```

## 4. Decisions to make

| | Proposed | Instead |
|---|---|---|
| R1 | **The output off as a state of a running display:** `Display::setOutput(bool)`; surface 0 and its clients don't notice. `display output on\|off` and the setting `display/output` | `display off` (stops the desktop too); a monitor's own power saving (the scan keeps reading PSRAM) |
| R2 | **The scan kept running on black** (the user's, 2026-11-16): its descriptors read one 8-byte word of zeros in internal RAM, the source address fixed; the frames, swaps, holds and interrupts go on as now, and the HDMI transmitter is off. The change is a restart at a frame's top with the link up | the scan stopped and a timer standing in for its frames (a second source of frames for every caller to follow, and the link rebuilt to come back); a black line in internal RAM, a descriptor a line (the fallback if fixed bursts from memory don't work: 720 interrupts a frame) |
| R3 | **`display/output`: `on` (the default), `off`, `remote`** (off while a program watches the screen, R6; on when the last one stops). **Local input turns it back on** (a key, a button, the pointer moving, from a USB device, not an injected one), as DPMS wakes a monitor, so a board can't be left dark with no way back; `display/output-wake` (on by default) | no wake (locked out if the network fails: only the serial console left); off only by command |
| R4 | **The same mode with the output off** (v1): the size and rate are `display/mode`'s, so turning the output on needs no change of mode | modes beyond the link while off (1920×1080 at 60): wanted, but the desktop can't change size live yet (§9) |
| R5 | **What changed, as tiles:** every drawing call on surface 0 already marks 32×16 tiles; each watcher gets its own bitmap, OR'd in at each present (single-buffered: at each frame tick or scan frame), and a flip to another picture marks all. 225 bytes a watcher at 720p | comparing frames (2.7 MB of PSRAM read a frame); the window manager's damage regions (miss programs' surfaces and the flips) |
| R6 | **Capture by a system call, local only:** `gui_screen_capture(conn, request)` takes the watcher's changed tiles (cleared atomically), merges them into rectangles, and copies them by 2D-DMA from the **shown** picture into the program's buffer, packed, in `Bgr24` or `Rgb565`; it answers the rectangles. The swap waits while the copy runs (a whole frame, never half of two). The protocol's `Capture(rect)` stays for remote clients, pixels in the stream | pixels through the protocol locally (two more copies of each frame); surface 0 mapped into the program read-only (zero copy, but the CPU then reads PSRAM against the scan, and sees half-drawn frames) |
| R7 | **Input as devices:** `SHELL`'s `injectKey(usage, modifiers, action)` and `injectPointer(x, y, buttons, wheel)` reach the input layer as a virtual keyboard (`kbdN`) and a virtual tablet (`tabletN`, absolute: the input layer's first absolute pointer), one each per connection, so hotkeys, controls and pointer lock apply as for USB. Keys held and buttons down are released when the connection ends | `WindowManager::key()` (under the hotkeys and controls; no pointer); a key at a time through `win hid` |
| R8 | **`image_encode` into memory:** appended fields `out`, `out_capacity`, `out_bytes`; with `fd == -1` the stream goes to `out`. A stream that reaches within 1 KB of `out_capacity` is `-BMDS_EOVERFLOW` (the encoder's silent cut, `esp32p4.md` §4) | a pipe or a memory file as the descriptor (a copy per encode, and the cut still silent) |
| R9 | **Who knows (changed, §13-§14):** any local program may watch and inject (`ScreenShare`); a setting couldn't stop one (any program can change it). While a program watches, the system marks it in the tray, a red dot on its own icon or an icon of the system's, which no program can hide | `gui/shell-clients` (first built so: no protection, and a setup step a start over SSH can't do); the indicator left to each program |

## 5. Design

### 5a. The output off: the scan on black (R1-R4)

- **`Scanout::setDark(bool dark, unsigned int timeoutMs)`** (`bmds_drivers`): a request to the
  supervisor's task, as `show()`'s is. There: `haltAtFrameEnd()` and `stopStream()`, every
  descriptor's source made fixed (dark) or incrementing (lit), then `startStream()`, which points
  them at the black word (dark) or the shown buffer (lit). If the DMA won't stop, the link is
  rebuilt, as for a switch.
- **The interrupt while dark:** everything as now (validating descriptors, frames, `frameTime()`,
  the swap's two steps and `swappedFromIsr()`), but `pointAt()` writes the black word's address,
  not the buffer's. A swap is still made at a frame's start, so `shown()` is always the buffer a
  lit scan would show.
- **Switches while dark** (a program's full screen, `show()`): the new surface's plan and list as
  now, its descriptors made dark. A rebuild of the link (a mode change, a stuck stream) keeps the
  state.
- **The black word:** 8 bytes of zeros, `MemSpace::InternalDma`, 64-byte aligned, made once.
- **`Display::setOutput(bool on, unsigned int timeoutMs)`**: off, the scan dark and then the HDMI
  transmitter off (`Lt8912b::setOutput(false)`: the monitor sees no signal and sleeps); on, the
  transmitter on and then the scan lit. Our side is back at the next frame; the monitor takes its
  own second or two to show it.
- **To measure:** the scan's internal-RAM reads while dark (166 MB/s of the same 8 bytes) against
  the CPU's: `bench` with the output on, off and dark, and PSRAM throughput (`display.fillcost`,
  the 2D-DMA's copies) each way.
- **`display/output`** (`on`, `off`, `remote`): applied at start and live by a watcher.
  `display output on|off` sets the live state without saving; `display` (status) says which and
  why ("off: a viewer connected").
- **The wake** (`display/output-wake`): the input layer tells `Display` of local input from a
  physical device; output off, it is turned on. With `remote`, it then stays on until the viewers
  have all gone and come back (the local user is at the board).
- **Statistics:** `display stats` says whether the scan is dark, and counts the output's
  switches.

### 5b. What changed (R5)

- **`ChangeTiles`** (`bmds_core`, host-tested): a bitmap of 32×16 tiles over a size; mark a
  rectangle, OR another bitmap in, take and clear, and merge into rectangles: rows of set tiles
  joined into runs, runs of the same span on adjacent rows joined into rectangles, at most
  `maxRects` (past it, the bounding rectangles of bands). `Surface`'s copy-forward bitmap becomes
  one.
- **Watchers:** `Surface` keeps the changes of the picture being drawn; at each present (or each
  frame, single-buffered) it ORs them into each watcher's bitmap. A switch of the shown picture
  (a program's full-screen flip, a return to surface 0, line mode on or off) marks every tile.
  Up to 4 watchers a display (static, `MemSpace::Internal`); a fifth is `Busy`.
- **The event:** `SHELL`'s `watchScreen(on)` adds the connection as a watcher; the event
  `screenChanged` is sent once when its bitmap goes from empty to not empty, and not again until
  a capture empties it. No pixels and no rectangles on the wire: a slow viewer costs nothing
  until it asks.

### 5c. The capture (R6)

- **`gui_screen_capture(conn, bmds_screen_capture_t *req)`** (a slot appended; local
  connections only, `SHELL` held and `watchScreen` on, else `-BMDS_EACCES`):

  ```c
  typedef struct bmds_screen_capture_t {
          uint32_t          size;          /* sizeof */
          uint32_t          format;        /* BMDS_PIXEL_BGR24 or BMDS_PIXEL_RGB565 */
          uint32_t          flags;         /* BMDS_CAPTURE_ALL: every tile, not only the changed */
          void             *buffer;        /* cache-line aligned (BMDS_DMA_ALIGN) */
          uint32_t          capacity;      /* bytes */
          bmds_rect_t      *rects;         /* out: the rectangles copied, packed in this order */
          uint32_t          max_rects;
          uint32_t          rect_count;    /* out */
          uint32_t          bytes;         /* out: used in buffer */
          uint32_t          width, height; /* out: the shown picture's (a flip may change them) */
  } bmds_screen_capture_t;
  ```

  Rectangles that don't fit `capacity` stay marked for the next call (`rect_count` says what was
  done). The rectangles are packed one after the other, each at its own width (stride = width ×
  bytes a pixel), so each is one JPEG encode's input as it stands.
- **Whole frames:** the swap waits while the copy runs (`Surface` holds the next swap, at most
  the copy's time: a full 720p frame by 2D-DMA, PSRAM to PSRAM at about 113 MB/s, is 24 ms, so a
  full-screen change can hold one frame; typical changes are small). Single-buffered, a tile
  drawn while it is copied is marked again, and the next capture fixes it (as X11's VNC servers
  do).
- **The shown picture:** surface 0, or a program's flipped surface; in line mode, each row
  copied as many times as the scan repeats it (the 2D-DMA's descriptors repeat a row for free).
  An L8 program surface is converted through its palette (the PPA's CLUT, as the scan's own
  paths do) or refused in v1 (§9).
- **`Rgb565`**: the 2D-DMA's transmit channels convert colours (to verify that RGB888 to RGB565
  is among its conversions; else the CPU does it, in the program).
- **The buffer** may be in the program's PSRAM or internal RAM; the call writes the cache back
  and invalidates as `esp32p4.md` §3 requires, in 64 KB pieces.
- **The tray icon** (R9) appears with the first `watchScreen(on)` and goes with the last.

### 5d. Input (R7)

- **`SHELL`'s `injectKey(usage, modifiers, action)`**: a HID usage (press, release, repeat)
  from the connection's virtual keyboard. The input layer treats it as a keyboard's report:
  hotkeys, global grabs, controls, then the focused window. Keysyms are the program's to map
  (a VNC server speaks X keysyms; BMDS speaks HID usages, so layouts stay in one place).
- **`injectPointer(x, y, buttons, wheel)`**: an absolute position on the shown picture, the
  buttons' state, wheel steps. The input layer gains absolute pointers: the pointer goes there;
  under pointer lock (a game's relative mode) the change from the last position is the motion.
- **Devices:** `kbdN` and `tabletN` appear in `usb`/`input` listings as virtual, named by the
  connection's program; they go when it ends, releasing what is held (no stuck Ctrl).
- **Injected input never wakes the output** (R3): a viewer doesn't light the local monitor.

### 5e. `image_encode` into memory (R8)

- Appended to `bmds_image_encode_t`: `void *out; uint32_t out_capacity; uint32_t out_bytes;`.
  `fd == -1` with `out` set: the stream goes there; `out_bytes` says how much. A struct without
  them (an older program's `size`) still needs a descriptor.
- `-BMDS_EOVERFLOW` when the stream ends within 1 KB of `out_capacity` (a cut is
  indistinguishable from a full fit; the caller tries again larger or at a lower quality).
- The codec's lock already makes callers take turns; a VNC server and the image viewer at once
  wait for each other, a JPEG at a time.

## 6. ABI

- **Appended:** `SHELL`'s methods `watchScreen`, `injectKey`, `injectPointer` and the event
  `screenChanged` (`sdk/gui/classes.def`, `gui.lock`, angel's `api.lock`); the system call
  `gui_screen_capture` at the next free slot with `bmds_screen_capture_t` and
  `BMDS_CAPTURE_ALL` (`bmds/sys.h`, the syscall lock test); `bmds_image_encode_t`'s three fields;
  the settings `display/output` and `display/output-wake`; `display output` as a new verb.
- **Unchanged:** `display off` (stops the display as before), `Capture(rect)` in the protocol
  (still to build, for remote clients), every existing call's meaning.

## 7. Budget (estimates)

- **The output off:** 64 bytes of internal RAM (the black word, aligned); no task, no timer.
- **Watchers:** 4 × 225 bytes at 720p (510 at 1920×1080), `MemSpace::Internal`, static.
- **Capture:** no kernel buffer; the 2D-DMA writes the program's.
- **Flash:** about 8 KB (the dark scan, the watchers, the call, the virtual devices).
- **PSRAM bandwidth with the output off:** 166 MB/s of scan reads freed at 720p.

## 8. Testing and steps

1. **The output off** (R1-R4): on the board, `display output off` with the desktop and
   `surfdemo` running: frames still 60 a second, the demo's frame rate unchanged, core load no
   higher, PSRAM throughput up; a screenshot while dark matches the picture; 30 offs and ons with
   0 underruns, stalls or rebuilds; `remote` and the wake by a USB key.
2. **Capture** (R5, R6, R9): `sdk/examples/screencap` (watch, capture everything, write a PNG):
   identical to `screenshot`'s, with the output on and off, and with a game flipped full screen;
   then only changes for a minute of a terminal scrolling, every pixel compared with a full
   capture; the tray icon comes and goes.
3. **Input** (R7): `screencap -i` types a line and clicks a menu; Alt+Tab and a grabbed hotkey
   work; killed with a key held, nothing stuck.
4. **Encoding into memory** (R8): the example's JPEG equals the file path's byte for byte;
   `-BMDS_EOVERFLOW` on a buffer too small.
5. **Then the server** (bmds-apps' plan), after the BMDS release that carries steps 1-4.

## 9. Open questions

- **Modes beyond the link while the output is off** (1920×1080 at 60, for a viewer with a large
  screen): needs a live change of surface 0's size (the window manager's work area, maximized
  windows) and refusing or changing the mode when the output comes on. Later, with
  `display.md`'s mode work.
- **The pointer's shape** for a viewer to draw locally (RFB's Cursor pseudo-encoding): done
  differently, §20 (a ScreenShare method and event, and captures without the pointer).
- **L8 program surfaces** in a capture: converted through the palette by the PPA, or refused.
- **Window moves as copies** (RFB's CopyRect): the window manager knows a move's source and
  destination; a watcher could be told them instead of the tiles. Later, if bandwidth says so.

## 10. Decided (2026-11-16, with the user)

- **R3:** the wake by local input is optional: `display/output-wake` (a sysconf setting, on by
  default).
- **R6:** capture by a system call, not a mapping of the screen.
- **A5** (bmds-apps' plan): the server is started by the user, from `autoexec.sh` or over SSH,
  and shows itself in the tray and by notifications while it runs.
- **R2 changed by the user:** the scan kept running on black instead of stopped with a frame
  clock in its place (§5a).
- **The whole plan approved** ("go ahead"), 2026-11-16.

## 11. As built (2026-11-16): steps 1-4, built and host-tested; the board not yet

Written on a laptop with no board attached: everything below builds without warnings and the host
tests pass, but nothing has run on the hardware yet. §8's board tests are what is left, first of
all whether the DW-GDMA takes a fixed source address from memory (R2: the dark scan depends on it;
the fallback is a black line a descriptor).

- **Step 1, the output off:**
  - `Scanout::setDark()` / `requestDark()`: a request to the supervisor's task, made as a switch
    of surface is (`haltAtFrameEnd()`, `stopStream()`, then `startStream()`; the link rebuilt if
    the DMA won't stop). `setSourceMode()` sets every descriptor's `ctrl_lo.sinc` (fixed while
    dark), `pointAt()` writes the black word's address while dark, `buildList()` makes a new list
    dark (a switch or rebuild keeps the state). The black word: 64 bytes of `InternalDma`, made at
    the first `setDark(true)`, kept. `Config::onDark` tells the display after each change.
  - `Display::setOutput()` / `requestOutput()`: the scan dark or lit; the HDMI transmitter
    follows in `onDark` (off while dark), and is turned off again after a rebuild
    (`onLinkRestart`: `Lt8912b::configure()` turns it on). `start()` applies `display/output`.
  - `display/output` (`on`, `off`, `remote`), followed live by a `SysConf::Watcher` (its
    notification only requests the change: a watcher mustn't block); `display/output-wake`. Both
    in Settings ▸ Display ▸ Output.
  - `Display::setViewers()`: `remote` turns the output off with the first watcher and on with the
    last gone. `Display::wake()`: the window manager's input sink (USB keyboards and mice only)
    calls it; injected input goes past it.
  - `display output [on|off]`; `display stats` says whether the scan is dark and counts changes.
- **Step 2, what changed and the capture:**
  - `ChangeTiles` (`bmds_core`, host-tested: 6 cases, a random one checking that the rectangles
    cover exactly the marked tiles and never overlap).
  - `Surface::setPresentHook()`: one hook for every surface's presents, given the tiles drawn
    since the last (a double-buffered surface's own bitmap) or null for all (single-buffered).
  - `ScreenWatch` (`bmds_ui`): up to 4 watchers, each a `ChangeTiles` fed by the scanned surface's
    presents (a program's full screen or the desktop; a new one marks all); `Notify` once until a
    capture empties them. **The capture copies with the CPU, not the 2D-DMA** (R6 said 2D-DMA):
    under the window manager's `capture()` as screenshots do, each rectangle's rows synced from
    PSRAM and copied (or converted to RGB565) straight into the program's buffer. Simpler, and the
    buffer needs no DMA alignment; with the output off the scan isn't competing. The 2D-DMA is
    the next step if the copy shows in the measurements.
  - **The shown picture is copied at its own size**, not with the scan's repeated rows (§5c said
    repeated): a program's 640×400 full screen comes out 640×400, which a VNC viewer takes as a
    desktop size change, and is cheaper to send.
  - `Shell.watchScreen(Bool)` and the event `screenChanged` (`gui.lock`, angel's `api.lock`);
    `gui_screen_capture`, slot 136, through `GuiOpener::screenCapture` to the connection (local
    only, its watch id kept on the connection); at most 64 rectangles a call (`more` says to call
    again).
  - The panel's `ScreenSharing` tray item: the system's display icon while anyone watches, its
    tooltip and menu naming the programs; no setting removes it.
- **Step 3, input:** `Shell.injectKey(usage, modifiers, action)` (0 release, 1 press, 2 repeat)
  and `injectPointer(x, y, buttons, wheel)` go into the window manager's queues as `win hid`
  does (its hotkeys and grabs apply; the output's wake doesn't). `PointerEvent` gained `absolute`,
  `x`, `y`: the window manager works out the motion where the pointer's position is known (under
  capture, from the last injected position). Keys held and buttons down are released when the
  `Shell` goes. **Not done:** the virtual devices in the input listings (R7's `kbdN`/`tabletN`).
- **Step 4:** `bmds_image_encode_t` gained `out` and `out_capacity` (48 bytes now); `fd` -1 writes
  there, `-BMDS_EOVERFLOW` if it doesn't fit. No `out_bytes` (R8): the call already returns the
  bytes written. The codec's own silent cut (`esp32p4.md` §4) is inside `JpegCodec`, not this
  buffer, so no margin rule is needed here.
- **`sdk/examples/screencap`:** the board test for steps 2-4: a whole capture, then `-w SECONDS`
  following changes onto a copy and checking it against a whole capture byte for byte, `-o`
  (`.jpg` through memory), `-5` (RGB565), `-i` (types and clicks).

## 12. On the board (2026-11-16, bmds0, the user's go-ahead)

Updated over the network (`tools/bmds-update`); the picture watched through the HDMI capture card
the board's output goes to, on the user's other machine.

- **Step 1:** `display output off`: black on the capture card, frames still 60 a second, 0
  stalls; 30 offs and ons (62 changes): 0 underruns, the picture back aligned. The fixed-address
  DMA source works (R2's open question). PSRAM with the scan dark (`docs/platforms/esp32p4.md`
  §4a'): DMA copies 72 to 125 MB/s, CPU writes and reads 39/41 to 49/47 MB/s, a PSRAM fill 23.9 to
  14.8 ms. `remote`: the output went off with a viewer and back on as it left, 20 changes over 10
  sessions; 2 underruns among them, made while the server was busy, which `changeDark()` had been
  hiding (it cleared the underrun after every change): it no longer does, and they were resynced.
  Not tried: the wake by a local key (nobody at the board).
- **Step 2:** a whole 1280x720 capture in 107-124 ms (5 calls with a quarter-screen buffer),
  identical to `screenshot` but for the tray's CPU meter (live); following changes for 4-10 s
  matched a whole capture byte for byte (a difference once, from the cursor blinking between the
  check's two captures: the check now takes another round, and it matched); RGB565 too.
  - **Found:** a capture right after `watchScreen(true)` answered `-BMDS_EACCES`: the request is
    taken in the service's own time. The first `screenChanged` (sent as the watch begins) is the
    point to start from; `sys.h` says so, and `screencap` and the server wait for it.
  - **Found:** the panel's "screen shared" icon never showed: an application may have 4 tray
    icons (`GuiApplication::MaxTrayIcons`) and the panel has 4 of its own already (`show()` was
    `Full`). Now 8; the panel logs a failure. Shown while watched, gone after.
- **Step 3:** `screencap -i` and the VNC viewer typed `echo screencap` and `echo vnc ok` into a
  terminal, and the pointer went to the middle.
  - **Found:** the letters didn't arrive at first, only Enter: a `KeyEvent` carries its character
    (`codepoint`), which the keyboard's driver fills from the keymap. `injectKey` now does too
    (`Keymap::us`, as `win hid` does).
- **Step 4:** a 1280x720 JPEG into memory in 36 ms, 145 KB at quality 85, right.
- **The network, found on the way** (`network.md` §36): non-blocking TCP sends that made no
  progress, and the loopback's received data holding lwIP's heap: both fixed.

## 13. One tray icon for a watcher with its own (2026-11-16, the user's ask)

The user: one tray icon for VNC is enough, with a red dot (as the notifications' unread one) while
someone is connected. R9 stays (a program can't hide that it watches), drawn differently:

- **The dot is the system's:** `ScreenWatch` keeps each watcher's GUI client (`add()`'s `client`,
  the connection's `GuiApplication::client()`), and the panel's tray draws a white-ringed red dot
  on every icon whose reply service is that client's (`tray.N`) while it watches
  (`ScreenWatch::isWatching()`, `NotifyArea::paint()`). The program sends nothing for it and can't
  leave it off.
- **The system's own icon** (`ScreenSharing`) shows only while some watcher has no visible icon of
  its own (`screencap`); `TrayService::setChanged()` tells it when icons come and go, and
  `repaint()` draws the dots again when watchers change.
- **Tried on bmds0:** a viewer connected, one VNC icon with the dot and no other; `screencap`
  watching alone, the display icon shown and the VNC icon without a dot.

## 14. ScreenShare: no permission setting, the tray's mark the protection (2026-11-16, the user's)

The user: any program can change any setting, so requiring `gui/shell-clients` buys nothing, and a
first start over SSH couldn't even be asked. R9 as it stands now: **the protection is the tray's
mark**, drawn by the system (§13), which no program can leave off; no setting gates watching or
injecting.

- **`ScreenShare`**, a class of its own (`classes.def`), any local connection's (several since
  §19, four watching at most; a remote one over TCP is refused, `GuiConnection::isLocal()`): `watch(Bool)`,
  `injectKey`, `injectPointer`, the event `changed`. `gui_screen_capture` takes a connection that
  watches through it. (Named `ScreenShare`: angel has a `Screen` type already.)
- **Out of `Shell`:** `watchScreen`, `screenChanged`, `injectKey` and `injectPointer`, unreleased,
  taken out of `classes.def`, `gui.lock` and angel's `api.lock` rather than left as dead ABI.
  `Shell` and `gui/shell-clients` are as they were for what they still cover (other programs'
  windows, global keys, panel and desktop levels).
- **Tried on bmds0** with `gui/shell-clients` at its default (empty): `screencap` followed changes
  and matched, `vncserver` started, a viewer over the LAN typed into a terminal, the VNC icon had
  its dot.

## 15. The capture by 2D-DMA, and the picture as the monitor shows it (2026-11-16, the user's)

R6 had the capture by 2D-DMA; §11 built it with the CPU. The user also found a game full screen
(Doom at 4:3) squashed in a viewer: a program's surface shorter than the mode is scanned in line
mode, its rows repeated, and the capture took the surface at its own size.

- **Rows as the scan shows them:** `shownRows()` is the mode's height for a scanned surface in
  line mode (screen row k is the surface's row k * rows / lines, the scan's and `screenshot`'s
  rule). Watchers' tiles are in screen rows: a present's tiles are mapped onto them
  (`markMapped()`), and a capture repeats rows to match, by either path. `width` and `height` in
  `bmds_screen_capture_t` are the screen's.
- **By the 2D-DMA** (`copyByDma()`): a `BlitChain` of 64 pieces, started with the first watcher and
  stopped with the last, copies each rectangle from the shown buffer into the program's buffer,
  packed (in line mode a piece a run of rows on consecutive source rows). The buffer is written
  back and dropped from the cache before, dropped after. Only when `buffer` and `capacity` are on
  `BMDS_CAPTURE_ALIGN` (128, appended to `sys.h`) and the format is B, G, R; else the CPU copies,
  as before.
- **Measured on bmds0:** a megabyte captured in 7-10 ms (21 by the CPU), and a capture of the
  desktop identical to `screenshot` (the notifications' corner aside). **Not seen yet:** a game
  in line mode through a viewer: `surfdemo -F` and Doom wouldn't stay full screen when started
  over SSH (the scan said line mode once, briefly); the user's Alt+Enter at the board is the test.

## 16. The capture without the window manager's wait (2026-11-16, the user's go-ahead)

Under load a capture cost 21-33 ms on average and up to 97: it ran under the window manager's
`capture()`, so it waited for the lock (a whole drawing round), then for the last present to be
shown (up to a frame), and the window manager waited for it in turn.

- **The desktop's latest present, copied as it is** (`copyLatest()`): the present hook records the
  buffer holding each desktop present (`Surface::drawn()` then, appended). With the swaps held, that
  buffer stays as it is, shown or waiting for its swap (the drawer waits for a swap before drawing
  again; the CPU never writes the buffers, and a present's drawing is finished before its hook).
  Tiles taken under the watchers' lock are that present's and earlier ones'; a later present marks
  tiles for the next capture. No window manager's lock, no wait; it draws its next round meanwhile.
  Before a desktop present is seen with watchers (and for a program's full screen, whose surface
  may go away) the old way.
- **Holds honoured off the scan:** an unscanned surface (the desktop under a full screen) swapped
  at once, holds or not; `swapNow()` now leaves a held present pending (a spinlock makes one of a
  present and a release swap it) and the last hold's release swaps it.
- **Measured on bmds0** (a terminal scrolling flat out, both ways on one firmware, alternated
  twice): a capture 10.8 ms on average, 15 at most (21-33 and 76-97 before): the copy alone. A
  viewer served 4.5 Mpixel/s at compression 0 (3.2) and 2.5 at 6 (2.25); the desktop's swaps the
  same (15-18 a second). A still desktop through a viewer identical to `screenshot` but for the
  notifications. **Not tried:** a full-screen switch during captures (the held swap off the scan).
- **Found by the user: a game full screen refreshed only its top part.** Not the line mode: a
  capture takes at most its buffer's pixels (vncserver's 768 KB: 262K), and `takeRects()` always
  began at the top row. A picture changing all over every frame (921K pixels at 1280 x 720) had its
  top marked again before each capture, so the rows below were never reached. A take that stops
  early now leaves where it stopped and the next starts there, round to it (`ChangeTiles`, with a
  host test). On bmds0, `surfdemo -F` in line mode through a viewer: the whole screen, as the HDMI
  output (bands of different frames where one frame needs several updates).

## 17. Clipboard.changed (2026-11-17, for the VNC server's clipboard)

A program could copy and paste, but learn of another's copy only if it had owned the selection
(`lost`); a VNC server sends the desktop's copies to its viewers as they happen, so it needs every
change. Appended: `Clipboard::watch()`/`unwatch()` (8 watchers, called on the changing task with
no lock held, one at a time under the owner-call lock, so `unwatch()` returning means none is under
way) after a copy, an offer, a clear that emptied something and a withdraw; `GuiClipboard::
setChanged()` (the selections' bits gathered and told on the loop, as `lost`); the GUI event
`Clipboard.changed(Atom)` to each Clipboard object of a connection (4 kept a connection), its own
copies included (`owns` tells them apart). Host-tested (`Clipboard tells its watchers of each
change`); on bmds0 `clip copy` reached a viewer within a second.

## 18. Decided (2026-11-17, the user's)

- **TLS servers in the kernel** ("proper TLS server support"): network.md §37, built.
- **Several viewers:** the first in has the keys and the pointer, the rest watch; when it leaves,
  the next in the order they came has them; the tray's menu gives them to any viewer, or to none.
  Each viewer its own watch (below), and no copy of the screen kept by the server (the user: "a lot
  of RAM for the app"): its one capture buffer serves every viewer in turn.

## 19. Several ScreenShares a connection (2026-11-17)

- **Each `ScreenShare` its own watch** (its own changed tiles: a viewer each). A connection keeps
  four watching (`GuiConnection::MaxScreenShares`, ScreenWatch's own limit): `watch(true)` past that
  is `BadAccess`; any number may exist (a program injects through one that doesn't watch).
- **`bmds_screen_capture_t::share`** appended (52 bytes; 48 before, its lock noted): the object
  whose watch is captured; 0, or a program built before it, the connection's first watching one.
- **Tried on bmds0** with vncserver's viewers (bmds-apps' vnc.md §21): four at once, each its own
  updates; a viewer's keys typed only while it had the input.


## 20. The pointer for viewers (2026-11-17, §9's open question, for RFB's Cursor)

A viewer drawing the pointer itself moves it at once, without waiting for an update; the screen's
picture must then be without it, or the viewer shows two. The window manager draws the pointer into
the frame (`drawCursor()`), what it covers saved first (`_under`, put back by the next round's
`restoreUnder()`), so both are known to it.

- **The shape:** `ScreenShare.pointerShape()` (a method, its result `Bytes`): little-endian, the
  width, height and hot spot's x and y as four 16-bit numbers, then width × height 0xAARRGGBB
  pixels, row by row. The arrow (or the shape now shown: the double arrows, the I-beam, the starting
  hourglass's current frame, a drag's) in the palette's colours, a drag's image under it blended as
  drawn. 0 × 0 when no pointer is drawn (`None`, hidden while typing, `setPointerVisible(false)`).
  At most 160 × 160 (a drag's 128-pixel image and its pointer): 100 KB, under the protocol's 256 KB.
- **`ScreenShare.pointerChanged`** (an event, on): the shape differs from the last drawn: told to
  every watching ScreenShare from the round that draws it (a signature of what the picture is made
  of compared each round, only while something watches), posted to its loop once until sent.
- **`BMDS_CAPTURE_NO_POINTER`** (a capture flag, 0x2): the desktop's pixels under the pointer in
  place of it. The window manager hands ScreenWatch the area and what it covers before each
  present (`ScreenWatch::pointerDrawn()`, copied into one of two slots, each kept with the buffer it
  belongs to); a capture patches the rectangles it copied from the slot of the buffer it copied.
  The swaps held during a capture keep that buffer, and so its slot, unchanged (its drawer waits for
  the swap before drawing it again, §16). A program's full screen has no pointer drawn: nothing
  patched. A kernel without the flag answers `EINVAL`: a program tries again without it (and
  leaves the Cursor encoding off).
- **Moves still mark tiles:** a pointer moving changes the frame, so its tiles come in captures
  even when, patched, they are as before. (Since §22: no longer; the pointer's drawing is apart.)
- **Not here:** where the pointer is (RFB's PointerPos, for a viewer to follow the board's own
  mouse). (Since §21: in each capture.)
- **As built:** `WindowManager::pointerImage()` and `tellPointer()` (after the round's batch ends:
  the batch's patches fill the saved pixels), `ScreenWatch::pointerDrawn()`, `pointerChanged()`,
  `setPointerNotify()` and `Capture::noPointer` (the slots grown only for a drag's image, freed with
  the last watcher). ABI: the flag (its lock line), the method and event (gui.lock, angel's
  api.lock).
- **Tried on bmds0** with vncserver (bmds-apps' vnc.md §24): the arrow (12 × 19, its tip at 0, 0)
  in a viewer's first update; with the pointer moved to 640, 400, a capture without it showed the
  wallpaper under it, and one with it differed in the arrow's 118 pixels only; sweeping the pointer
  over guigallery's window brought the side and corner arrows as it crossed the edges. **Not
  tried:** a drag's image, the starting pointer, a hidden one.

## 21. Where the pointer is, in each capture (2026-11-17, for RFB's pointer position)

- **`bmds_screen_capture_t::pointer_x`, `pointer_y`** appended (60 bytes; 52 before, its lock
  noted): where the pointer's tip is in the copied picture, from the same slot as §20's pixels (so
  it is the captured frame's); -1 when none is drawn there. A kernel before them leaves what the
  caller set (vncserver sets -2 to tell). A pointer moved by the board's own mouse changes the
  frame, so a capture follows and says where it went: no event of its own needed.
- **Tried on bmds0:** a second viewer (vncserver, bmds-apps' vnc.md §25) followed the first one's
  sweep of the pointer, 95 positions in 4 s, in the order sent.

## 22. A pointer's drawing isn't a change without it (2026-11-17)

- **The waste:** a viewer drawing the pointer itself (RFB's Cursor, captures with
  `BMDS_CAPTURE_NO_POINTER`) was sent the pointer's tiles, the same pixels again (the pointer
  taken out): on every move, and on every round of the window manager's, since each puts back
  what the pointer covers and draws it again (on bmds0, the tray's CPU meter: 4 a second).
- **Now:** `Surface::setPointerDrawing(on)` sends a double-buffered surface's marks to a bitmap of
  their own (`_pointerDamage`, beside `_damage`; the copy forward takes both). The window
  manager sets it around `restoreUnder()` and `drawCursor()`. The present hook gets both
  bitmaps; each watcher keeps `pointerTiles` apart from its `tiles`. A capture without the
  pointer clears them and copies none (a tile drawn by both is in `tiles` too); one with it ORs
  them in. The watcher is still told (`screenChanged`): a capture gives the pointer's place
  (§21), maybe with no rectangles. No ABI change: a capture without the pointer copies what
  differs in the picture without it, as before; it now leaves out what doesn't.
- **Cost:** a fourth tile bitmap a double-buffered surface (internal RAM: 360 bytes at 1280 × 720,
  544 at 1920 × 1080); 550 bytes more a watcher (PSRAM).

## 23. A tray icon's change repaints its cell (2026-11-17)

- `NotifyArea::iconChanged()` invalidated the whole notify area: each tick of the CPU meter (4 a
  second on bmds0) repainted the tray, and a VNC viewer was sent 192 × 32 pixels each time. Now
  only the icon's cell (`TrayRow::CellWidth` × the tray's height): 32 × 32 on bmds0, the tray
  drawn as before.

## 24. Copies for viewers: window moves (2026-11-17, for RFB's CopyRect)

**Status: built (the user's go-ahead, C1 then the rest); as built at the end.** The proposal:

- **What there is:** nothing on the desktop copies pixels within the screen today but the `win`
  test command's scroll (`Painter::scroll()` → `Surface::copy()`). A window moved (at its drop: the
  move is an outline until then) is drawn again by its owner at the new place, and what it
  uncovered by the windows under it. So a viewer is sent the whole window again.
- **C1, the window manager moves by copying:** at a drop that only moves (same size), the
  window's pixels where it was and showed are copied (`Surface::copy()`, 2D-DMA) to where it goes;
  only what didn't show before (it was covered, or off screen) and what it uncovers are drawn
  again. A gain on the board itself (no redraw by its owner: a GUI program's whole window, a
  terminal's text), as well as for viewers. The title bar's colours change with focus: drawn
  again too if the drop focuses it.
- **C2, the surface tells of its copies:** a double-buffered `Surface` keeps a frame's copies in
  order (8 at most; past it, plain damage), each `{from, to}`. A copy whose source was drawn
  earlier in the same frame is plain damage (its pixels aren't the last present's). The copy's
  fully covered destination tiles aren't marked damaged; its edge tiles are. The present hook
  gets the list.
- **C3, a watcher's pending copies:** each watcher keeps up to 16 in order. At a present, a copy
  is kept for it only if its source's tiles are clean in the watcher's tiles (what it has there
  is what was copied) and, for a watcher capturing with the pointer, the pointer wasn't over
  the source. The copy's covered destination tiles are cleared from its tiles (the copy will
  overwrite them), and the frame's damage is ORed in after (drawing after the copy is sent after
  it). Otherwise, or with no room, the destination is marked as damage. Line mode, or a program's
  full screen: no copies.
- **C4, the ABI:** `bmds_screen_capture_t` gains `bmds_screen_copy_t *copies; uint32_t
  max_copies; uint32_t copy_count;` (out: the copies, in order, before the rectangles), and
  `bmds_screen_copy_t {uint16_t from_x, from_y, width, height, to_x, to_y}`. A caller without them
  (an older struct, or `max_copies` 0) gets their destinations as rectangles, as today. Lock
  lines: the struct's new size and offsets (rule 4), and the new struct's.
- **C5, vncserver:** a viewer listing CopyRect (encoding 1) is given `max_copies`, and its update
  starts with CopyRect rectangles in order (12 + 4 bytes each), then the rest. RFB applies an
  update's rectangles in order, so a copy reads what the viewer had.
- **Later, free with C2-C5:** the terminal and the GUI's scrolling views scrolling by copying
  (`Painter::scroll()`), faster on the board, and sent to viewers as copies.
- **Cost (estimate):** C2 about 100 bytes a surface (internal); C3 about 200 bytes a watcher
  (PSRAM); C1 saves a window's redraw on each move.
- **To decide:** C1 on its own first (the board's gain), then C2-C5; or all together.

**As built (2026-11-17):**

- **C1:** `dropLocked()` (a drop that only moves) and `move()` copy a window that showed whole
  (`showsWhole()`: nothing over it, no update of it waiting, no band, no copies pending):
  `planMoveCopy()` takes what it shows at its new place (less the frame if the focus changes) and
  takes it out of the damage, but where what wasn't drawn yet at the source lands (an outline's
  sides, a change of its own): copied whole, that is drawn again over the copy. Overlapping its
  old place, it is copied in bands no deeper (or wider) than the move, the side it moves towards
  first; more than 32 bands (a nudge), it is drawn as before. `runMoveCopies()` runs them first in
  the next round (the pointer already off). `win stats` counts them ("Moves:"). `Region` gained
  `subtract()`, `WindowStack` `clearDamage(rect)` (host-tested).
- **C2:** `Surface::copy()` records a double-buffered frame's copies (8; a band joined to the one
  before, the same move: one copy read at once). Their destinations go to a bitmap of their own
  (`_copyDamage`, copied forward like the rest); a copy whose source was drawn earlier in the
  frame, or past 8, is damage. The present hook gets the list.
- **C3:** each watcher keeps up to `ScreenWatch::MaxCopies` (16) copies until it captures. A copy
  is always kept (while there is room): its covered destination tiles are cleared, and the
  source's tiles the watcher doesn't have as they are (its changes waiting; the pointer's
  drawing, for a picture with the pointer) are marked where they land, so they come after it. A
  capture gives them all (`Capture::copies`) or, too little room, their destinations as tiles; a
  capture of everything drops them. Line mode: everything.
- **C4, ABI:** `bmds_screen_copy_t {bmds_rect_t from; int32_t to_x, to_y}` (24 bytes) and
  `bmds_screen_capture_t`'s `copies`, `max_copies`, `copy_count` (72 bytes; lock: the new size and
  offsets, 60 before, and the new struct's).
- **C5:** vncserver (bmds-apps' vnc.md §27).
- **Tried on bmds0:** a window moved 13 ways (`win move`, `win drag`: overlapping in each
  direction, under the panel, partly under another window, an unfocused one) matched, pixel for
  pixel, the same place drawn by redrawing. A viewer with CopyRect got every move of a run of 7
  (5 drags with their outline, 2 moves a second apart) as one copy each, its picture the board's
  exactly. Over six drags, 13 % fewer pixels (11 % fewer bytes) than a viewer without it: the
  place uncovered is drawn either way, and a drag's outline frames come before its drop.
- **Not done:** a window's scrolling by copying (terminal, GUI views), which would come to viewers
  through C2-C5 unchanged.

## 25. A secret for its program: `BMDS_SYSCONF_FIELD_SECRET` (2026-11-17)

- **ABI:** `sysconf_get`'s field 6, the current value with a secret's too (a secret's value field
  still reads "(set)"). For vncserver's `vnc/password` (bmds-apps' vnc.md §28): VNC's challenge
  needs the password itself, so it is stored obfuscated, not hashed, and read back. Every program
  is root and the store is readable (network.md §24a): this hides nothing more than before, it
  only gives a program its own setting without reading the store's file. Host-tested; lock line
  appended.

## 26. R7's virtual devices: no listing to put them in (2026-11-17)

- §5d had `kbdN` and `tabletN` "in `usb`/`input` listings"; there is no `input` listing (`usb`
  lists USB devices, `pad` gamepads), and an injector isn't a USB device. Left as a question for
  the user: an `input` command listing every source (USB keyboards and mice, gamepads, and the
  programs injecting: vncserver's viewers), or the tray's "screen shared" item (which names the
  watching programs) as enough.
