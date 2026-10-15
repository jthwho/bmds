# Roadmap

Phases are ordered so that each one makes the next faster to build. Each phase gets its own devplan document(s) before implementation starts. Check items off as they land.

## Phase 0 — Foundation

Plan: [`foundation.md`](foundation.md). Complete; verified on hardware.

- [x] ESP-IDF project skeleton (`CMakeLists.txt`, `main/`, `sdkconfig.defaults`, `partitions.csv`), builds for `esp32p4`.
- [x] Compiler flags: C++20, `-fno-exceptions`, `-fno-rtti`, warnings-as-errors for BMDS components.
- [x] `bmds_core` skeleton: `config.h`, namespace macros, `Error`, `Result<T>`, `StringView`, `Span`.
- [x] Host test harness (doctest, plain CMake) running `bmds_core` tests.
- [x] `bmds_board`: Olimex ESP32-P4-PC board definition from the schematic; `MemSpace` mapping. Fill in `docs/HARDWARE.md`.
- [x] Logging macros (`bmdsDebug/Info/Warn/Err`, per-module debug registration).
- [x] Boot sequence: `app_main()` → BMDS init stages, with boot timing and memory usage report.
- [x] Baseline size report recorded (RAM/IRAM/flash) for future comparison.

## Phase 1 — Serial Shell

Plan: [`shell.md`](shell.md). Complete; verified on hardware.

- [x] Console abstraction (a terminal endpoint: read/write bytes, size, capabilities).
- [x] Serial console on USB-Serial/JTAG.
- [x] Line editor: editing keys, history. (Completion is deferred to Phase 2, when there are paths to complete.)
- [x] Shell core: command registration (rodata table), argument parsing, exit codes.
- [x] Built-ins: `help`, `version`, `uptime`, `mem` (per-MemSpace usage; `mem mark` and `mem new`: the heap blocks in use now that weren't at the mark, with their sizes and first bytes, for finding leaks), `tasks`, `log` (levels/modules), `reboot`.
- [x] Log history (`log show`), since a USB serial port misses the boot messages.

## Phase 1b — Shell Language, Standard I/O, Terminal Model

Plans: [`shell-language.md`](shell-language.md), [`terminal.md`](terminal.md). Done.

- [x] `String`, `List<T>` and an arena allocator in `bmds_core`.
- [x] Standard I/O: `Stream`, refcounted fd tables, pipes, memory/null streams; Ctrl-C interrupts.
- [x] Commands use stdin/stdout/stderr; pipelines (concurrent stages, workers with PSRAM stacks); fd redirection.
- [x] Shell language (bash subset): lexer, parser, executor; variables, expansion, `$(…)`, `$((…))`, `if`/`while`/`until`/`for`/`case`, functions, language built-ins, `printf`/`read`/`type`/`command`, `set -x`, `PS1`/`PS2`, completion. (`source` and path completion need storage: Phase 2.)
- [x] Terminal model: 24-bit color, SGR with depth downgrade, VT parser, UTF-8, `term` built-in, colored prompt and log levels.

## Phase 2 — Storage

Plans: [`storage.md`](storage.md), [`time.md`](time.md). Complete; verified on hardware.

- [x] SD card driver (SDMMC host), card detect, hot-plug handling; `BlockDevice`; `sd` command.
- [x] System clock (Linux-style: software clock, set/slewed/disciplined by sources, survives resets), time zones, `date`, `clock`.
- [x] Filesystem (FatFs with exFAT, vendored) and VFS: numbered block devices and partitions (`/dev/sd0p0`), run-time mount table with automatic mounting (`/sd0/p0`), `mount`/`umount`, `/dev` nodes, files as streams; host tests on a RAM disk.
- [x] Shell: `ls`, `cd`, `pwd`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `rmdir`, `touch`, `df`, `hexdump`, `mkfs`.
- [x] Shell: file redirection, globbing, tilde, file tests, `source`/`sh`, `/sd0/p0/autoexec.sh`, path completion.
- [x] File transfer over serial: YMODEM `rz`/`sz` (works with picocom/minicom/tio through `lrzsz`).

## Phase 3 — Programs

- [x] Design: syscall mechanism, executable format, memory layout (`docs/ABI.md` open questions resolved).
- [x] Userspace SDK: C headers, startup code, linker script, host build (CMake toolchain file; `bmds-cc` for other build systems).
- [x] Loader: load from SD, relocate, run, return exit code; clean up on exit.
- [x] Syscall table v1: fd-based I/O (stdin/stdout/stderr, `read/write/close/dup2/isatty`), terminal info/modes, file I/O, memory, time, exit, args, environment.
- [x] ABI lock tests.
- [x] Shell: run programs by name, `PATH`-like search; `#!` and `.sh` scripts launched by content; kill on a second Ctrl-C.
- [x] Push-and-run over serial (host tool sends a program, device runs it — no reflash): `tools/bmds-run`, into `/tmp` (a tmpfs since 2026-10-12, `storage.md` §15; the RAM disk `/ram0/p0` before); compatibility corpus `tests/compat/v1` (`tools/bmds-compat`).
- [x] The SDK on static musl (`devplan/programs.md` §12, step 7): vendored musl, the Linux system call layer, file identity, `ftruncate`; BusyBox builds and runs.
- [x] Emulated symbolic links on FAT/exFAT, Cygwin's format (`storage.md` §4: `symlink`, `readlink`, `lstat`); `ln -s`.
- [x] Terminal size learned from a serial terminal (`ESC [ 18 t`, the answer taken out of the input; `terminal.md`), `COLUMNS`/`LINES`, `TIOCGWINSZ`; `clear`, `reset`.
- [x] Distribution structure (`devplan/distribution.md`): `dist/` recipes, `tools/bmds-dist` build and install; BusyBox 1.37 (115 applets as links).
- [x] USB serial console: input bytes lost under two-way traffic. ESP-IDF's ESP32-P4 TX FIFO write is a read-modify-write that pops an RX byte (`shell.md`).
- [x] Storage: a sector cache for directories and the FAT under FatFs (`storage.md` §3): `ls -l` of BusyBox's `bin` 2.36 → 0.30 s.
- [x] POSIX completion (§12, step 8+): process creation (`vfork`/`posix_spawn`, `waitpid`, `pipe`), signals, `termios`, `poll`. Yardstick: BusyBox no-MMU (shells included) runs. A shared runtime only if measurements ask for it.
  - [x] 8a: processes, `vfork`/`exec`/`posix_spawn`, `waitpid`, `pipe`, close-on-exec (`programs.md` §13); `hush` runs programs and pipelines.
  - [x] 8b: `/bin/sh` (the shell as a process) and `#!` scripts in `exec`; `system`, `popen`.
  - [x] 8c: BusyBox with `xargs`, `find -exec`, `time`, `nohup` (116 applets as links).
  - [x] 9a: signals in the kernel (`programs.md` §15): `kill`, handlers and masks, Ctrl-C as `SIGINT`, `SIGPIPE`, `SIGCHLD`.
  - [x] 9b: `SIGWINCH`; BusyBox with `kill`, `timeout`, `watch` (119 applets as links).
  - [x] 10a: `poll` in the kernel (`programs.md` §16): stream readiness, `io_poll`; `poll`, `ppoll`, `select`, `pselect`.
  - [x] 10b: `termios`: `ECHO`, `ICRNL`, `VMIN`/`VTIME`, `O_NONBLOCK`; BusyBox `vi`, `less`, `hush` `read -t`/`-n`/`-s`.
- [x] Processes, threads and jobs (`processes.md`): a process is one or more threads; every shell and program is one; process groups, the console's foreground group; `&`, `jobs`, `fg`, `bg`, `wait`, Ctrl-Z; `ps`.
  - [x] 1: processes own threads; `ps`, `ps -T`, `kill` any pid.
  - [x] 2: process groups, sessions, the foreground group and the console's signals (Ctrl-C, Ctrl-\, `SIGWINCH`, hangup); shells are processes: pid 1 is the serial shell (`/bin/sh -l`), the terminal's shell is its child (`/bin/sh -i`).
  - [x] 3: `&` and the job table: `jobs`, `wait`, `kill %N`, `disown`, `$!`; background subshells are processes.
  - [x] 4: stopping and continuing: Ctrl-Z, `fg`, `bg`, `SIGTTIN`, `WUNTRACED`.
  - [x] 5: program threads: pthreads through musl (slots 53-57: thread start/exit/kill/clear, futex), signals per thread.
  - [x] 6: real-time threads (§3.12): `SCHED_FIFO`/`SCHED_RR` per thread (slot 113, `sched`), a band of kernel priorities under the drivers, Linux's 95 % budget, precise sleeps for them on a GPTimer; `sched_yield`, `sched_getaffinity`, `sched_getcpu`.
- [ ] Shared program images and an image cache (`programs.md` §14): `exec` from memory, code pages shared between instances through the MMU.
  - [x] MMU spike (aliases, caches, DMA) and design.
  - [x] (a) The image cache with plain copies (`programs.md` §14.1): `exec` from memory, without the card.
  - [ ] (b) Shared pages through the MMU window.
  - [ ] (c) SDK layout changes, each measured.
- [ ] Example programs: `hello`, `cat`, a benchmark.
- [x] Application folders (`apps.md`, planned 2026-10-14): one folder per application (`/sd0/p0/apps/NAME.app`: the program, its data, `app.desktop`), found by looking (no registry), each one a command in a virtual `/apps` on the `PATH`, `BMDS_APP_DIR`, its data in `$HOME/.local/share/ID`; the launcher, the file browser and Open With from them. Done (`apps.md` §11): `Apps` (the scan, the watches, ids and commands, `apps/path`, the `apps` command), `/apps` in the VFS, the default `PATH`, `BMDS_APP_DIR` and `BMDS_APP_ID`, the SDK's `bmds/app.h` (`appinfo`); the launcher reads `/apps` and watches its directories (§12); the file browser and desktop run `NAME.app` folders (Show Contents), `MimeType=` opens files when `mimeapps.list` names none (§13). building folders: `bmds_add_app()`, `bmds-dist` recipes into `apps/` (§14). Later: an application's own icon in the file browser, an Open With submenu.
- [ ] Partition editing (`storage.md` §14): BusyBox `fdisk` (MBR) on the card, not built in; raw block device nodes as streams, writes kept out of mounted partitions, the table re-read on close; extended partitions read.
- [x] A time zone database (`time.md` §9): zone names (`TZ=Europe/Berlin`, `clock zone Europe/Berlin`) from one bundle on the card, shown to programs as `/share/zoneinfo` (musl's own path: history included); looked up at boot, when a filesystem mounts and when `TZ` changes. The IANA sources in `thirdparty/tzdata` (2026e), `tools/update-tzdata`, the `tzdata` package; `clock tzdata`.
- [x] `/dev/zero`, `/dev/urandom` and `/dev/random` (`storage.md` §4).
- [x] The kernel's random pool (`random.md`): seeded at boot from the SAR ADC noise source (which the bootloader turns off), `/boot/random-seed` and the hardware generator per read; BLAKE2s and ChaCha20; `/dev/urandom` and `getrandom` read it.

## Phase 4 — Display

Plans: [`display.md`](display.md), [`windows.md`](windows.md), [`gui.md`](gui.md).

- [x] Bring-up (`display.md` §4): the LT8912B and EDID on I²C, a test picture at 1280×720.
- [x] Line-rendered scanout (`display.md` §5): a ring of line buffers in internal RAM streamed by the DSI DMA, refilled per block; measured; free-running.
- [x] Framebuffer (`display.md` §5b, §5c): first an RGB565 double buffer widened into a ring; now surface 0, an RGB888 surface in PSRAM the DW-GDMA scans directly, single or double buffered, swapped at a frame's start; the ring paths removed.
- [x] PSRAM at 200 MHz (`psram.md`): `memtest`, verified at 80 and 200 MHz in a separate build; the default since 2026-10-05 (surface 0 needs it).
- [ ] Desktop and windows (`windows.md`): a flat desktop, a bare-bones window manager (windows drawn directly on surface 0, outline moves), unscii-16; the terminal as the first program. Done: `TerminalScreen`; step 1 (regions, the compose plan, the font); step 2 (surface 0); step 3 (the window manager on surface 0, the `win` command); step 4 (the Painter: a PIE glyph kernel, text overlapped with the 2D-DMA, scrolling, unaligned fills); step 5 (the terminal program: a shell in a window, `win term`/`win type`).
  - Decided (2026-10-05): surface 0, an RGB888 PSRAM framebuffer scanned directly by the DW-GDMA; every surface single-buffered unless it asks for two; windows drawn directly on surface 0 by default, backing surfaces and DMA-read secondary surfaces later as per-window options; outline moves (`windows.md` §0).
- [ ] Window manager affordances (`windows.md` §8, step 8): decorations under the application's control, minimize/maximize/close buttons, resize handles and pointer shapes, pointer hiding, the wheel scrolling a terminal, context menus (the desktop's starts programs), a panel with a launcher and a window list. Done: decorations the window chooses, title-bar buttons, minimize and maximize (8a, §4f); resize handles and pointer shapes (8b, §4g); pointer control (8c, §4h); the wheel and terminal scrollback (8d, §4i). Done: `/boot`, a persistent FAT partition on the flash always mounted, with `/boot/profile` and `/boot/autoexec.sh` (`storage.md` §13). Done: `/bin` of built-in programs, with every shell command but the shell-only ones as an applet, and `env` (`programs.md` §17). Done: built-in applications as ordinary applications (step 9): pseudo-terminals (`pty.md`), GUI applications connect first, the terminal is `/bin/terminal`. Done: context menus and the panel on a GUI toolkit (`gui.md`): `bmds_gui` (an event loop, widgets, menus), window levels, mouse events, popups and a work area in the window manager, the window menu, `/bin/panel` (a launcher from Desktop Entry files in `/boot/applications` and packages' `share/applications`, a window list, a clock; its layout from `/boot/config/panel.conf`), `terminal -e`, and `/boot`'s defaults installed from the repository's `boot/`. Done: new windows cascade; the toolkit, menus and title-bar buttons in the classic 3D look; Alt+Tab with a list in the screen's middle, Alt+F4, Alt+Space, Alt+drag, Always on Top (`gui.md` §4e-§4g); icons: the Flat Color set at 16 and 32 pixels as a theme on the card, a PNG decoder, icons built into programs (`BMDS_NOTE_ICON`), a second theme (Glyphs, `IconTheme=glyphs`) (§4h, §4j); the Windows key opens the launcher, snapping to the top and sides, rolling up (§4k). Done: the terminal's own colors, from `/boot/config/terminal.conf` (the 16 ANSI colors too), and resources in the window manager for `win color terminal-*` (§4l). Done: desktop icons, 32 pixels, from `/boot/desktop`'s Desktop Entry files (`[Desktop] Icons`), a double click to start one (§4m). Done: desktop icons dragged to other cells of their grid, their places kept in `/boot/config/desktop.conf` (§4m). Done: Alt+Tab as a grid of 32-pixel icons (§4n); the window manager's own desktop menu from `/boot/config/wm.conf`, a terminal without the panel (§4o); busy programs no longer set off the task watchdog (`processes.md` §3.11). Done: Win+arrows and Alt+F10, Ctrl+Shift+Windows for a terminal, minimizing only with a window list to come back from (§4p). Done: the panel ends when its bar or desktop window is closed. Next: copy and paste (`clipboard.md`), then keys for the desktop's icons.
- [ ] Copy and paste (`clipboard.md`): a typed clipboard service with conversion, `clip`, `/dev/clip0`, the terminal's selection and paste, OSC 52, vi's `"+` register; the toolkit's API. Done: the types, base64 and the converter table (step 1); the clipboard service, `clip`, `/dev/clip0` and `/dev/clip0p` (BusyBox vi's `:r` and `:w!` work with them unchanged), `/boot/config/clipboard.conf` (step 2); the terminal's selection (characters, words, lines, Ctrl for a block) into the primary selection, Ctrl+Shift+C and V, Shift+Insert, the middle button, bracketed paste, and pastes paced by the program (step 3); OSC 52 sets the clipboard from programs (reading it only with `[Clipboard] AllowRead=true`) (step 4); BusyBox vi's `"+` and `"*` registers are the clipboard and the primary selection (step 5); the toolkit's clipboard (`GuiClipboard`: copies stored or rendered on request, pastes read on its own thread, copies kept when an application ends), and the panel's desktop icons copied as `text/uri-list` and their path (step 7). Done: the terminal's right-click menu (Copy, Paste, Select All, by the window manager's `showMenu()`); the desktop's Paste (a copy's name if taken: `NAME (copy).EXT`) and the icons' Delete into a trash on each filesystem (`MOUNT/.trash`, freedesktop.org's layout), and `trash` to list, restore and empty it (`files.md`). Done: desktop icons named by their files and renamed in place (Rename, a slow second click, a long press), the toolkit's `LineEdit`, `PopupEdit` and `MessageBox` (`gui.md` §4q). Done: the in-place editor drawn as the label, mnemonics (`&OK`, Alt and the key, underlined; in buttons, labels and every menu), focus and Tab, a default button, keys for the desktop's icons (arrows, Enter, F2, Delete, Ctrl+C and V, the Menu key, a letter) and the panel's buttons, the desktop and panel taking the keys (`gui.md` §4r); what a file is (`file_type()`, slot 58: by name, then by its first 512 bytes), opening files by `/boot/config/mimeapps.list` (vi for text, a terminal for programs), every file on the desktop with its type's icon (`filetypes.md`); New ▸ Folder, Text Document, Empty File on the desktop (`files.md` F10). Done: a power-safe `/boot`: the erased directories were block writes cut off by resets (`storage.md` §13a); `/boot` is now littlefs through an ordered write cache, behind a filesystem interface in the VFS; about 40 times less pause per small file than FAT, and whole after 32 hardware resets during writes (§13c, §13d). Done: system configuration (`sysconf.md` steps 1-3): a typed registry of 60 settings with defaults, saved to `/boot/sysconf` only by `sysconf save`, watchers (the window manager's palette, the terminal, the clipboard and the clock follow changes live), `/home` as a link the VFS provides to the registry's `home` (falling back to `/boot/home` while it isn't mounted), and `terminal.conf` and `panel.conf` replaced by parameters. Done: one way to set a thing (`sysconf.md` §13): the commands that duplicated parameters lost those forms (`win color`, `clock zone`, `panel -c`, `log level`, `display start`'s mode), `display/mode`, `display/buffering`, `log/level` and `log/debug` (65 parameters), the window manager's resources removed, and `waitmount` no longer waits for an empty card slot. Designed: ids and the SDK's calls (`sysconf.md` §12). Done: sysconf step 4a-4b (`sysconf.md` §12.4a, §12.4b): parameter ids, and the SDK's `bmds_sysconf_*` calls (slots 59-65) with watch descriptors, and the `conf` example. Done: `GuiApplication::watchConfig()` and the panel following its settings live (step 4c, §12.4c). Done: 255 windows and connections in storage that grows (`filebrowser.md` FB17); the toolkit's furniture: scroll bars and scroll areas, a menu bar, tool and status bars, a splitter, a progress bar, dialogs, a clipped painter, the wheel and the pointer's shape per widget, and `/bin/widgets` to show them (§4e). Done: `bmds_files` (a folder model filled in batches and kept sorted; copies, moves, removals, the trash, renames and new items with progress and questions), the desktop's operations on it, and the trash following links (`/home`) (§4f). Done: the item views (icons flowing or on a grid, details under a sortable header, a folder tree) on a selection with Ctrl, Shift and a rubber band, and the desktop moved onto them with no limit on its icons (§4g). Done: folder windows in the panel (a menu bar, a toolbar, an address bar, the folder tree, Large Icons or Details, a status bar; Back, Forward and Up; renaming in place, New, Copy, Paste, Delete to the trash), the window manager's services, `files [PATH...]` and the launcher's Files, `folders.conf` and `files/show-hidden`, `files/default-view` (§4h). Done: directory watches in the VFS (`DirectoryWatch`: entries made, removed and renamed, files written, mounts), shared per program (`FolderWatches`); folder windows, their trees and the desktop follow changes from elsewhere, and the desktop's retry timer for a card mounted later is gone (§4i). Done: the panel's file worker (`FileWorker`: copies, moves across filesystems and deletes on a thread of their own, queued, counted first, cancelled, a taken name asked Replace or Merge, Keep Both, Skip or Cancel, for all), its progress window after a second, Cut (GNOME's `x-special/gnome-copied-files`, cut items dimmed until pasted or the clipboard changes), Shift+Delete asking first, and a check box in the toolkit (§4j). Done: faster lookups on FAT and exFAT (`storage.md` §3: a metadata cache of 256 sectors found by hash, read ahead in runs, scan-resistant; two FatFs patches), `du` of a 361-entry folder 3.7 to 0.23 s and the browser's copy of `share` 119 to 21 s; copy buffers per file, the tree reading only listings, the rubber band scrolling at a view's edge. Done: the card's metadata written back within a second, in an order a cut can't turn into a cross-link (`storage.md` §3), `sync` (a command, and slot 66 for programs), `reboot` syncing first; a cut test at every write on FAT32 and exFAT; FatFs passing over broken exFAT entry blocks. Next: drag and drop, Properties and the Trash window (`filebrowser.md` steps 7-8); converter programs (clipboard step 6).
- [ ] Drag and drop (`clipboard.md` §5): carried by the window manager over the clipboard's `Drag` selection.
- [x] The desktop and a terminal at boot: from `/sd0/p0/autoexec.sh` (`display start`, `win start`, `win term`); BMDS itself boots with the display off.
- [ ] `Display` as a service (hot-plug).
- [ ] Display modes (720p and up, the EDID's preferred where the DSI link allows) and depths for programs (8-bit palette, 16, 24); line-based graphics.

## Phase 5 — Input

Plan: [`input.md`](input.md).

- [x] USB host bring-up, hub support: TinyUSB's host stack (ESP-IDF's library has no split transactions, which low-speed devices behind the board's high-speed hub need).
- [x] Screenshots: `screenshot PATH` saves the screen as a PNG (`display.md` §5d).
- [x] HID keyboard: boot protocol, the US layout, repeat, lock LEDs; keys into the focused window, a terminal's as xterm's bytes.
- [ ] Keymaps/layouts beyond US; report-protocol HID.
- [x] HID mouse: the pointer (drawn last each round), click to raise, title-bar drags, the close box.
- [ ] Input event model for the kernel and the SDK.
- [ ] Keyboard + HDMI console = a full local shell.
- [x] Notifications, screenshots and the system's name (`notifications.md`, planned 2026-10-10):
  a host name (`system/hostname`, by default `bmds-` and the serial's last six, in the prompt) and
  the board's serial;
  notifications for every program (balloons by the tray, a centre of the last 64, targets and
  events, a sound by urgency); Print Screen saving where `screenshot/path` says (a text template,
  `templates.md`).
  Step 1 done 2026-10-10: the serial (the board's reader), `system/hostname` (`bmds-e2e222`),
  `hostname`, the prompt, `system_info`. Steps 2-6 done 2026-10-10: the kernel's notifications,
  `notify`, the calls (slots 89-91), balloons by the tray and the centre in the panel, the
  toolkit's `Notifier` and `TrayIcon::showMessage()`, the media player's "Now playing".
  Step 7 done 2026-10-10: Print Screen (the file where `screenshot/path` says, then a
  notification showing it in its folder).
- [ ] Text templates (`templates.md`, planned 2026-10-10): `${NAME}` and `${NAME:ARG}` (`HOME`,
  `HOSTNAME`, `SERIAL`, `DATE`, `TIME`) expanded alike by the OS and programs (slot 92); paths
  become settings with defaults, templates where they name by the date or host. The scheme,
  `bmds_expand` and `screenshot/path` done 2026-10-10; the paths left in the code to go (§6).
- [x] Images (`images.md`, planned and done 2026-10-10): PNG (every kind), BMP and JPEG for the OS
  and programs (`image_info`, `image_decode`, `image_encode`, slots 93-95), JPEG decoded and
  encoded by the P4's codec (`jpeg0`, zero-copy when the buffer allows, full-range colour loaded
  into the 2D-DMA) and TJpgDec for photos too big for memory; `image`; screenshots as JPEG;
  `/bin/imageview` (fit, zoom, pan, turn, the folder a key away, opened from the file browser).
  To do: SIMD for the scaler and TJpgDec, progressive JPEG.
- [ ] SIMD (`simd.md`, planned 2026-10-10): the P4's PIE unit on every integer hot path it fits,
  each with a scalar twin it matches byte for byte; `simd bench` and `simd test`. Done 2026-10-10:
  the scaler (730 to 291 ms for 1080p to 720p), PNG's write filter (8.8×), the RGB swap, TJpgDec's
  output (YCbCr averaged before converting; PIE conversion), plus non-SIMD: orient (5.5×), PNG
  decode (2×); the glyph renderer off the hardware loop (a silicon bug); the JPEG encoder's cache
  bug. Then the GUI and audio: the alpha blend (icons, RGBA pictures: 6.7× in internal RAM, 2.4×
  from PSRAM) and the synth's voice loop (not SIMD: 80 to 53 cycles a voice a sample). Next:
  TJpgDec's IDCT (or a reduced one), streaming PNG inflate.
- [ ] Special keys and global hotkeys (`hotkeys.md`, planned 2026-10-08): media keys decoded,
  named actions and bindings in the kernel, the window manager's keys moved onto them; user
  bindings later.
- [x] System monitor (`sysmon.md`, planned 2026-10-08, revised 2026-10-09): CPU per core, task
  and process from run-time stats; every subsystem's counters in one named registry with rules
  for what is abnormal and a problem log; system calls; a System Monitor window, a tray meter that
  turns amber or red, `top` and `stats`. Step 1 done 2026-10-09: each task's CPU time is counted
  (about 0.9 µs a context switch, no audio cost), `sysmon/enable` on by default. Step 2 done
  2026-10-09: the registry (`Stats`), rules, overrides and problem log; every subsystem's counters
  (audio, synths, MIDI, display, compositor, storage, caches, USB, input, console); `stats`.
  Step 3 done 2026-10-10: the calls (slots 80-87: counters, history, problems, watching, tasks,
  processes' CPU time), with `progtest stats|tasks|procs`.
  Step 4 done 2026-10-10: the toolkit's `Graph`, and `/bin/sysmon` (Overview, CPU, Processes,
  Memory, Audio, Display, I/O, Counters; problems in amber and red, tabs marked, End Process).
  Step 5 done 2026-10-10: the panel's tray meter (`panel/items`' `cpu`, on by default): each core's
  load, amber or red for problems not yet seen, a click opens the monitor at Overview.
  Step 6 done 2026-10-10: `top` (`-d`, `-n`, `-T`), `tasks` from the kernel's task list with a
  `cpu_ms` column, and `getrusage()`, `times()`, the CPU clocks and `sysinfo()` from the process's
  own time.
- [ ] System logging (`logging.md`, planned 2026-10-11, the user's request): one log for the
  kernel, built-in programs, programs (`syslog()` through `/dev/log`), scripts (`logger`) and
  services; kept in the history, on the console, and in rotated files in `log/dir`
  (`/sd0/p0/logs`), the boot's lines from before the card included. Steps 1-4 done 2026-10-11:
  `LogText` (host-tested, fuzzed), `Log::writeText`/`read`/`waiters`, `/dev/log`, musl's
  `syslog()` through `linux.c`, Dropbear with syslog, `LogFiles` with rotation, `logger`. Next: a
  log tab in the System Monitor.

## Phase 6 — Graphics

- [ ] Surfaces for programs (`games.md` §3, planned 2026-10-14): a swap chain of the program's own buffers (L8 with a palette, RGB565, RGB888) presented in a window or full screen through one API (a per-application setting the application suggests, Alt+Enter toggles), scaled by the PPA, a zero-copy flip at the display's size, raw key and pointer events, the escape keys. Done: the PPA measured (its scaler interpolates and is slow: not the games'), the scan's line mode (rows repeated), the calls (slots 117-121) and `ProgramSurface`: windows at whole multiples and full screen with Alt+Enter, lazy copy-forward, `surfdemo` (`games.md` §12); damage (only what a present changed is converted or redrawn: full screen at 60); pointer modes (hidden, and relative: the window manager's pointer lock). Next: the per-application setting (with `apps.md`).
- [ ] The SDK's game library (`games.md` §4): drawing in PIE (fills, blits, sprites, tiles, text), PNG sheets, a sound-effect mixer, music through the synth, a frame loop. Done (§13-§15): `libbmdsgame` part 1, the canvas (clipped drawing, keyed blits, unscii text, damage kept), the frame loop (frames start as the last shown, only changes presented), `bounce`; the scan's switch race fixed. Part 2, images (PNG, JPEG, BMP into the canvas's format, alpha or a colour as the key, L8 by nearest colour); slow L8 and scaled windows fixed. Next: sound.
- [ ] Controls (`controls.md`, planned 2026-10-14): gamepads (HID through `HidLayout`, XInput, a standard pad by position, numbered, `pad`), actions declared by every application with a keyboard binding required and pad bindings where they apply, matched by the system, changed in one standard editor (the Controls window, `controls`, and the game library's in-game one).
- [ ] Games (`games.md` §6): Blocks (Tetris's rules), Doom (doomgeneric as a `dist/` recipe, Freedoom, music through the synth), Solitaire (Klondike, in a window).
- [x] Mouse cursor (`input.md` §9, step 3).

## Phase 7 — Sound and Network

- [ ] Audio (`audio.md`, designed 2026-10-08):
  - [x] 1. ES8311 and I²S bring-up; `audio` status, registers, test tone.
  - [x] 2. Engine: mixer, conversion, resampling, volume, system sounds and bell, tray control.
  - [x] 3. ABI: native audio fds, `/dev/dsp*`, OSS ioctls in `linux.c`, `play` (WAV).
  - [x] 4. Decoders (MP3, Ogg Vorbis, FLAC) in the firmware via the SDK; Open from the browser
    (§19: minimp3, stb_vorbis, dr_flac; a decoder is an fd, slots 71–73; fuzzed).
  - [x] 5. Real-time clients and exclusive mode (§20: slot 74 `audio_cycle`, JACK's model, down to
    16-frame periods; the `keytone` example).
  - [x] 6. MIDI: ports, routing, virtual ports, `/dev/midiN`, slots 75-77, `midi`, `midimon`
    (§21). USB MIDI built and waiting for a device to try; serial MIDI waits for its circuit.
  - [x] 7. Synth: General MIDI player and synth library. Done (§22): the GM engine on a patched,
    fuzzed TinySoundFont, the synth fd (slots 78-79), `synth`, MIDI files through the decoders
    (`play`, Open, `audio_decode`), the TimGM6mb package. Deferred (the user): the SDK's building
    blocks for instruments (oscillators, envelopes, filters), until an instrument needs them.
  - [ ] 8. Media player (`mediaplayer.md`, planned; the user's choices 2026-10-09): `mplayer`, a
    GUI player for audio and MIDI files (video later), with saved playlists, a library by tags, a
    visualizer, a MIDI view, a toolkit Open dialog, and a tray controller while it plays hidden;
    designed in detail (§6), with a Qt-style system tray API that the volume control moves onto.
    Steps 1 (the pure parts) and 2 (the toolkit: `FileDialog`, `ComboBox`, `TabWidget`, the
    slider's release) done. Step 3 (the player: the playback thread, the window, Open and Add,
    keys, errors, one player, `mplayer.desktop`) built and tried on the board; the user's ear next.
    Step 4 (kept between runs: `mplayer.conf`, `playlist.m3u8`, Open and Save Playlist) built and
    tried on the board.
    Step 5 (the system tray: `TrayIcon` and the panel's `tray` item, the volume control moved onto
    it, the player's icon and hiding on close) built and tried on the board.
  - [x] The ROM (`rom.md`, planned 2026-10-09, the user's idea and layout): a read-only `/rom` in
    its own flash partition, mapped in place: standard icons (pre-decoded, drawn from flash) and
    the time zone database without a card; `Vfs::map()`, later programs' read-only `mmap()`
    without a copy. A new flash layout with it: a 2 MB recovery firmware, the 4 MB main one, a
    512 KB `/boot`, the rest the ROM. Before the player's step 3.
    Steps 1 (the format, `RomVolume`, `Vfs::map()`, the `bmds` icon theme, host-tested), 2 (the
    new flash layout on the board, `/rom` mounted, `rom`), 3 (icons drawn from the ROM by id,
    `bmds` the default theme) and 4 (time zones without a card) done.
  - [ ] Mixer window (`audio.md` §17, planned 2026-10-09): `mixer`, per-application levels (the
    device, each stream, system sounds, the MIDI synth's `synth/gain`); the volume tray icon's
    context menu (Mute, Mixer…). After the media player.
  - [ ] Speech (`speech.md`, planned 2026-10-12, the user's request): text to speech on SVOX
    Pico, the en-US voice read in place from `/rom`, the others a package; `say` (the speakers or
    a WAV file), a speech fd for programs (slots 114-115) and `bmds_say`. Steps 1 (Pico vendored,
    patched and fuzzed; `Speaker`) and 2 (`Voices`, `say`, the settings, the ROM; measured: 6×
    real time, a sixth of a core speaking, the voice kept in place) and 3 (slots 114-115, `bmds_say`,
    `progtest speech`, the `speak` example) done. Next: the voices package (step 4).
  - [ ] 9. Capture and `record` (after the loopback cable), then the pedal work.
  - [ ] 10. AES67 and USB audio (after the network stack).
- [ ] Network (`network.md`, designed 2026-10-10; the user's requirements and decisions):
  Ethernet on `eth0`, vendored lwIP under the kernel's own BSD sockets (musl's Linux calls),
  sysconf for every parameter (`/etc` a pseudo filesystem over it), multicast first class, a
  real-time fast path (classifier, priority transmit queue, 1588 timestamps) for PTP and AES67,
  NTP and PTP in the kernel, SSH (keys and passwords, off by default) and SFTP for `sshfs`.
  - [x] 1. Driver (EMAC, IP101, rings, 1588 clock; no AV DMA channels on this chip): `net`, §14.
  - [x] 2. lwIP 2.2.1 (`NO_SYS`, our task and core lock, heap in PSRAM), DHCP/static live from
    sysconf, ARP, ICMP, IGMP over the MAC's 8 filters, `lo`, `ping`; flood-tested, §16.
  - [ ] 3. Sockets ABI, `linux.c`, DNS; BusyBox's network applets. Done (§19): the kernel's
    `Socket` (TCP, UDP, raw ICMP) with host tests, slots 96-107, `linux.c`, `NetStatus`,
    `/etc`, DNS (DHCP's servers or `net/dns/servers`, `net/hosts`), `ping NAME`; BusyBox built
    with `nc`, `wget`, `telnet`, `ping`, `nslookup`; on the board `progtest net`, the applets
    and names against the LAN, TCP about 20 Mbit/s each way through this host's Wi-Fi and 47
    over loopback; `alarm`/`setitimer` (slot 108, `programs.md` §18) for BusyBox `ping`,
    `nc -w`, `wget -T`. Left: checksum offload, a wired peer.
  - [x] 3a. The tray's network icon: `NetStatus` (public, change-driven: compared after each
    marked event, no timer), `net_watch` (slot 109), the panel's `NetworkStatus` (a drawn
    socket, the tooltip per interface, a menu with the lease's time left, Copy Address, Renew
    Lease), `panel/tray/network`; tray tooltips to 383 bytes and 12 lines, §20.
  - [x] 3b. The network in sysmon: `net.ethN` and `net.ip` (lwIP's heap, pools and MIB-II
    counts) in the registry, the System Monitor's Network tab and Overview line, `top`'s
    headline, §21.
  - [x] 4. NTP and mDNS: the kernel's NTP client (filter, selection, step/slew/frequency
    discipline, a poll from 64 s to 1024 s), `ntp`, `net.ntp` counters; lwIP's mDNS responder
    for `${HOSTNAME}.local` with conflicts and live renames; a hostname change renews DHCP;
    the parser fuzzed, §22-23.
  - [x] 5. SSH: Dropbear, passwords in sysconf, our own `sftp-server` (`network.md` §24-25): in `/rom`, the
    server a kernel service (`net/ssh/server`); a file manager's `sftp://` still to try.
  - [x] 5a. Crypto, measured (`network.md` §26-27): delayed ACKs within 40 ms (a 0.2 s stall
    per login); Dropbear's X25519 and Ed25519 on Monocypher (11-37 times TweetNaCl's); the AES
    and SHA engines in register mode (`bmds_crypto`, slot 112 `crypto`) under libtomcrypt, the
    board's client preferring AES-CTR. A login 1.03 s to 0.2 s; MPI and ECC not worth it.
    Then the TCP path (§28): 16-segment buffers, writers woken at half room, the EMAC's sent
    interrupt only now and then: SSH downloads 2.2 to 3.2 MB/s, uploads 1.7 to 2.3, raw TCP up
    2.9 to 4.6. The sampling profiler (`prof`, `tools/bmds-prof`, `sysmon.md` §9) found it.
    Then (§29): lwIP's heap in internal RAM and frames sent from it in place (the EMAC can't send
    from PSRAM), received checksums checked by the MAC (it can't insert them past 256 bytes), AES
    by DMA from 1 KB: SSH AES-CTR's CPU per MB down 334 to 297, up 308 to 270.
  - [x] 6. Real time: fast path, priority queue, timestamps, `netlat`. Design `network.md` §30: a
    real-time flow always sends and receives, however much TCP takes. Piece 1 built (§30a): the
    driver's bulk limits and receive reserve, a spinlock-only transmit path on uncached
    descriptors. Then (§30b) no frame lost under line-rate floods: receive interrupts masked while
    draining, and no flushing on an empty ring (it wedged the MAC's receive path for good).
    Hardening (§30c): a receive watchdog that resets the MAC, `tools/bmds-netstress`, the loss
    counters in `stats`, `eth0` on core 0 and real-time senders on core 1. Piece 2 (§30d-e): the
    classifier (host-tested, fuzzed) and `NetFlow` (UDP or EtherType; its own ring, depth and wake;
    headers built without lwIP, ARP snooped; departure stamps handed to the flow; the bulk limits
    set as flows open and close): 0 of 60000 real-time packets lost under TCP both ways and a
    90 Mbit/s flood, departure at most 217 µs. ICMP errors limited as Linux does (§30g). Piece 3
    (§30f, §30h): `BMDS_SO_FASTPATH`, `SO_PRIORITY`, `SO_TIMESTAMPING` with the error queue, on
    the board (`progtest rtnet`). Piece 4 (§30i): `netlat` and `tools/netlat-reflector`: the
    kernel path loses nothing under load, but a program can neither pace finer than the 1 kHz
    tick nor raise its threads' priority, so its receiver falls behind. Then (§30j,
    `processes.md` §3.12) real-time program threads: `SCHED_FIFO`/`RR` (slot 113) in a band
    under the drivers with Linux's 95 % budget, sleeps on a hardware timer (about 30 µs late):
    `netlat -r` lost 1 of 30000 under the full load (5953 before), its wake 37 µs at the median.
    The tails (§30k): datagram sockets no longer take lwIP's core lock in `poll()` or an empty
    receive, and the wake timer's interrupt is on core 1. Under the full load: 0 lost in 26 runs,
    driver to program 116 µs at the 99th percentile (1538 before), wake 56 µs (124).
  - [ ] 7. PTP, master and slave (BMCA): two boards alone elect a grandmaster. Design §31 (the
    grandmaster down: the protocol proven on a simulated network and against linuxptp; the
    leap-second file in `/rom`, `clock/leapseconds`). Step 1 done (§31a): the portable core
    (messages, BMCA, linuxptp's PI servo, the port with holdover), host-tested on a simulated
    network (lock within 251 ns at 1 µs jitter, election, failover without a step), fuzzed.
    Step 2 done (§31b): the 1588 clock stepped in one update; PPS out on GPIO26, the MAC's own
    from chip revision 3, before it (this v1.3 board) a GPTimer through the ETM, within 120 ns.
    Step 3 done (§31c-d): the service on core 1, `net/ptp/*`, `ptp`, `net.ptp.*`; the system
    clock follows a slave's ptpclock0, a master's follows NTP. Against `ptp4l` (software stamps):
    both roles and the takeover work.
    Step 4 done (§31e-f): `/dev/ptp0` (slot 116, Linux's PHC calls and ioctls in `linux.c`),
    `CLOCK_TAI`, IANA's leap-second list in the ROM and the `tzdata` package (`clock/leapseconds`,
    hash checked), `clock`'s `TAI:` line and `clock leapseconds`.
    Step 5 done (§31g): against `ptp4l`, both profiles, both roles, standing down and taking over;
    `ptp4l` sees the board's UTC offset valid on PTP's timescale. Fixed on the way: the servo
    steps a new master's first offset, a new parent's timescale is taken at once, NTP gets its
    rate back from PTP and takes a large rate error whole. Left: the grandmaster's tests;
    where to pick up, and the hardware wanted: §31h.
  - [x] Updates (`update.md`, the user's ask, 2026-10-13; fetching from a URL postponed): a signed `.bmup` package (firmware, ROM,
    recovery) pushed onto the card over SSH or copied there, checked, and applied by a recovery
    firmware in `ota_1`; `update check|apply|status`, `reboot recovery`; headless or graphical
    start to finish (the Updater, recovery's progress screen).
    - [x] Step 1: the package, `tools/mkupdate`, dev and release keys, `build update`, the version.
    - [x] Step 2: the recovery firmware (headless), `update check|apply|status`, `reboot recovery`,
      `tools/bmds-update`; resets at each stage tested (§11). Open: the IDF bootloader's fallback
      bug, one unexplained bad read.
    - [x] Step 3: recovery's screen ("Recovery Mode", the update's progress, a terminal; §13).
      Open: the keyboard tested by hand.
    - [x] Step 4: the Updater (`/bin/updater`, the `.bmup` type, the notification after; §15).
    - [x] App rollback (both firmwares on probation until they confirm), `update status`'s
      Recovery line; flash auto-suspend off (it faulted reads under load; §16).
    - [x] Step 5: the crash counter (3 in a row: recovery), Esc held at power on (§18), both
      tested on the board. Fetching from a URL postponed (the user's, 2026-10-14; planned in §17).
    - [x] Last: `ota_1` cut to 1.375 MB (recovery is 0.9 MB), `rom` 10 MB; `/boot` moved with its
      contents (§19).
  - [x] `/cfg` on the card, `/boot` renamed `/flash` (`cfg.md`, the user's, 2026-10-14): the
    configuration on the card when there is one (no flash writes, so no audio stalls), else on
    `/flash`; no syncing; `/flash`'s writes held while audio plays. All three steps built and
    tested on the board (§5). Open: a boot without a card, tried by hand.

## Phase 8 — Self-Hosting

- [ ] On-device text editor.
- [ ] Compiler evaluation (TinyCC RV32 port vs. alternatives) and port.
- [ ] Assembler/linker producing BMDS executables on the device.
- [ ] Build the SDK examples on the device.
