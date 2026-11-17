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
| R9 | **Who may, and who knows:** watching, capturing and injecting are `SHELL`'s, so only programs in `gui/shell-clients`, local unless `gui/shell-remote`. The user adds a VNC server there (Settings ▸ Desktop shows who holds it). While any program watches the screen, the panel's tray shows a "screen shared" icon naming it, drawn by the system: a program can't hide it | a setting of its own for capture (one more list to keep); the indicator left to each program |

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
- **The pointer's shape** for a viewer to draw locally (RFB's Cursor pseudo-encoding): a
  `SHELL` event with the shape's image id when it changes. Appended when the server wants it.
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
