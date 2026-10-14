# Updates and the recovery firmware

Status: design approved by the user (2026-10-13), §7's choices as recommended.

An update reaches the board over the network or on a card, waits on the SD card, and is applied
by the recovery firmware. Everything can be done from a command line (headless, over SSH or the
serial console), and a user can do it graphically from start to finish: open the package, install,
watch it applied, see the result (the user's requirement, 2026-10-13).

## 1. What is decided already, and what is there

- **The layout** (`partitions.csv`, rom.md §3a): `ota_0` 4 MB the main firmware, `ota_1` 2 MB the
  recovery firmware (empty until now), `otadata`, `/boot` 512 KB (littlefs), `/rom` the rest
  (9.4 MB). No A/B slots: the user chose a recovery firmware and one main slot (rom.md §4).
- **Recovery applies updates** (rom.md §3b): "the main firmware checks and keeps the new firmware
  and ROM on the card, then reboots into recovery, which writes `ota_0` and `rom`, checks them,
  and sets `otadata`. The main firmware can't write itself or the ROM it has mounted." Recovery
  doesn't use `/rom` (it may be what is repaired); `reboot recovery` is appended to `reboot`. With
  no factory partition, IDF's bootloader falls through to `ota_1` when `ota_0` won't load (**not
  with this IDF: a bug, §11**).
- **Sizes now:** the main firmware is 2.01 MB of `ota_0`'s 4 MB (IDF checked it against
  `ota_1`'s 2 MB instead: fixed in step 1, §8). The bootloader has
  1.2 KB free: this design changes nothing in it (no IDF rollback, no secure boot).
- **There to reuse:** BLAKE2s (`bmds_core`, the ROM's own hash), Ed25519 in monocypher (9.7 ms to
  verify; in Dropbear only today), the hardware SHA engines, littlefs on `/boot`, the flash
  writer pattern of `/boot`'s `FlashDevice`, `scp`/`sftp` onto the card (works today), the ROM's
  `BMRO` header (size and hash, no version).
- **Not there:** any `esp_ota_*` writes, signing of anything on the board, an HTTP client.

## 2. The package (an on-disk format: frozen once shipped)

One file, `NAME.bmup`, made on the host by `tools/mkupdate` from a build:

- **Header** (fixed, little-endian, `uint32_t size` first as the ABI's structs): magic `BMUP`,
  format 1, flags; the firmware's version and build id (`PROJECT_VER`, the app's ELF SHA); the
  board's name and the least chip revision; `BMDS_ABI_VERSION`; the section count.
- **Sections:** a table of {type, offset, size, BLAKE2s-256}: `1` the main firmware (`ota_0`'s
  image), `2` the ROM (`rom`'s, its `BMRO` header first), `3` the recovery firmware (`ota_1`'s),
  `4` release notes (UTF-8 text, shown by the Updater). Any may be left out; unknown types are
  skipped (append only).
- **Signature:** Ed25519 over the header and the table (which hold every section's hash), 64
  bytes after the table. The public keys are built into both firmwares from `keys/`.
- **The acceptance mode** (the user's, 2026-10-13; independent of debug or release builds):
  `CONFIG_BMDS_UPDATE_KEYS_DEV` (the default) accepts the dev key, whose private half is in the
  repository so anyone can sign a dev package, and the release keys;
  `CONFIG_BMDS_UPDATE_KEYS_RELEASE` accepts only the release keys (`keys/release.pub`, one or
  more), and the build fails without one. A release key's private half never enters the
  repository (`keys/.gitignore` refuses any `*.key` but `dev.key`).
- **Checked before anything is written:** the signature, each section's hash (streamed from the
  card), the board and chip revision, each image's size against its partition, and the images'
  own headers (the app image's magic and its appended SHA-256, as the bootloader checks; the ROM's
  `BMRO` header and hash).

The parser and checks are in `bmds_core` (host-tested, fuzzed); reading from the card and writing
flash are in the kernel.

## 3. The flow

1. **Deliver.** Headless: `scp bmds-0.2.0.bmup root@bmds0:/sd0/p0/update/` (works today; a host
   wrapper `tools/bmds-update HOST FILE` does that and `update apply`). A user: copy it onto the
   card from a PC. Fetching from a URL waits for an HTTP client (§7).
2. **Check:** `update check FILE`: the checks of §2, the versions now and in the package (and
   whether it is newer, the same or older: §10), the sections and whether each differs from what
   is flashed (machine-parsable, as `rom info`).
3. **Apply:** `update apply FILE`: the checks again (a package not newer than what runs is
   refused without `--force`: §10); if it carries a recovery firmware that
   differs, the main firmware writes `ota_1` now (it doesn't run from it), reads it back and checks
   it, so recovery is never left broken by a cut-off write (until it checks, `otadata` stays
   `ota_0`); then it writes an intent to `/boot` (the package's path and header hash), sets
   `otadata` to `ota_1`, and reboots.
4. **Recovery** finds the intent, checks the package again (the card may have changed), writes
   `ota_0` and `rom` (each skipped if already identical), reads each back against its hash,
   writes the result to `/boot`, sets `otadata` to `ota_0`, removes the intent and reboots.
5. **First boot after:** the main firmware reads the result, logs it, `update status` shows it,
   and the Updater (or a notification) tells the user.

- **Power lost or a reset at any point:** the intent stays, and recovery starts over (every write
  is of whole partitions, so it is idempotent). `otadata` points back at `ota_0` only after both
  images are written and checked; a half-written `ota_0` fails the bootloader's check and it boots
  recovery anyway.
- **The card pulled, or the package changed or bad:** recovery stops before writing if it can; if
  `ota_0` was already being written, it stays in recovery, shows why, and waits for a card with a
  good package (it watches for the card, as the main firmware does), or for `reboot` once `ota_0`
  checks good again.
- **No rollback slot:** an update that boots but is broken is fixed by applying another package
  (an older one included): from the main firmware if it runs, from recovery if not (`update apply
  FILE` on recovery's console). A crash loop counter (N resets before the main firmware calls
  itself healthy, then recovery) is a later step.

## 4. The recovery firmware

- **A second build of the same components** (`recovery/`, an IDF project beside the main one),
  with `CONFIG_BMDS_RECOVERY` and a smaller configuration: the kernel, the serial and USB serial
  console, the SD card and FAT, `/boot`, the flash writer, BLAKE2s and Ed25519, the display with a
  text console and the update screen, a small shell (files, `update`, `reboot`, `rom check`, `log
  show`). Not: the network, audio, the GUI, `/rom`. Budget: under 1 MB of its 2 MB (the libraries
  left out are 0.9 MB of the main firmware's 2).
- **Its icons and font are its own** (rom.md §3b), built into it.
- **`idf.py flash` from the main project writes both**, so a board flashed from source has a
  recovery (built first in `build-recovery/`: §11). A package can carry a new one (section 3).
- **Entered by:** an update, `reboot recovery`, and the bootloader when `ota_0` won't load. A key
  held at power-on comes with the USB keyboard in recovery (later).

## 5. Graphical, start to finish

- **The Updater** (a built-in app, public APIs only): opened from the file browser on a `.bmup`
  (a new file type), or from its own launcher entry, where it lists the packages in `update/dir`.
  It shows the version now and the package's, its release notes and the checks' result; Install
  asks once, then shows "Restarting to install".
- **Recovery's update screen** carries the look on: the same title, a progress bar and the stage
  (Checking, Writing the system, Writing the ROM, Checking what was written), then "Done,
  restarting". On a failure, why, and what to do (a card with a good package; `reboot`).
- **After:** the Updater opens itself on the first boot after an update and shows the result
  (or a notification does: §7).
- **The app's API:** the same calls the `update` command uses, as syscalls appended to the table
  (check a package into a `bmds_update_info_t`, apply, read the last result), so the app is a
  program like any other. The `update` command's output stays the headless contract.

## 6. Steps

1. **The size check and the package:** the main firmware checked against `ota_0`, the recovery
   against `ota_1` (IDF's check replaced by one per build); the package format, `tools/mkupdate`,
   the parser and checks in `bmds_core` with host tests and a fuzzer; Ed25519 verify in the
   firmware.
2. **Recovery, headless:** the recovery build, `reboot recovery`, `update check/apply/status`, the
   intent and result in `/boot`; resets at every stage of a write (power loss) on the board.
3. **Recovery's screen:** the display, the text console and the update screen.
4. **The Updater:** the syscalls, the app, the `.bmup` file type, the result after.
5. **Later:** fetching from a URL; the crash loop counter; a key at power-on.
6. **At the very end, the partition sizes** (the user's, 2026-10-13): `ota_1` cut to recovery's
   measured size plus about half (likely 1.25 MB, once its screen is in), the rest to `rom`;
   `otadata` and `/boot` move with it. `/boot` may simply be reformatted (nothing on it needs
   keeping, the user's word); every board is flashed once over USB (a package can't change the
   partition table).

## 7. Decisions (the user's, 2026-10-13: each as recommended; the ABI as written)

- **Signing:** packages signed with Ed25519 and only signed ones applied (recommended: an update
  can arrive over the network, and a bad image is a bricked board until recovery), or hashes
  only (integrity, no authenticity; simpler, any build applies).
- **Delivery over the network:** push now (`scp` to the card, then `update apply`, wrapped by
  `tools/bmds-update`; no new network code) and fetch from a URL later with HTTPS (recommended),
  or an HTTP fetch in the firmware now.
- **Recovery's reach:** the serial console and the screen (recommended), or also the network
  (SSH into recovery: the network stack and Dropbear make it roughly twice the size).
- **Acceptance mode** (added 2026-10-13): a dev key in the repository for dev builds; release
  builds accept only the official key (§2).
- **The ABI:** the `.bmup` format above; `update check FILE`, `update apply FILE`, `update status`;
  `reboot recovery`; the setting `update/dir` (`path`, default `/sd0/p0/update`); the syscalls of
  §5; the `.bmup` file type.

## 8. As built: step 1, the size check and the package (2026-10-13)

- **The size check:** IDF's `app_check_size` compares the app with the smallest app partition
  (`check_sizes.py` without `--subtype`), which is recovery's 2 MB: it warned "nearly full (4%
  free)" on every build, and would warn (not fail: the app still fits `ota_0`) past it.
  `components/bmds_storage/project_include.cmake` replaces `partition_table_add_check_size_target`
  (called only for that check) with one that checks against `BMDS_APP_PARTITION`: `ota_0`, or
  `ota_1` in the recovery build. It is included after IDF's (checked: the build now says "Smallest
  app (ota_0) partition is 0x400000 bytes ... 52% free").
- **The format as §2,** with these settled (the header grew to 136 bytes after: §10): the header's
  128 bytes were `BMUP`, u16 format, u16 header size, u32 flags, u32 section count, u32 ABI
  version, u16 least chip revision, u16 0, board[32], version[32], build id[32], i64 when it was
  made (by `mkupdate`, not the firmware's build); a section is u32 type, u32 flags, u32 offset,
  u32 size, BLAKE2s-256[32]. **Flags this code doesn't know are refused** (`NewerFormat`), not
  ignored: they may change what a package means. Unknown section types are skipped; any type
  listed twice, or a section inside the head or past the file's end, is refused.
- **`bmds_core`:** `bmds/ed25519.h` (Monocypher's RFC 8032 Ed25519, built through
  `src/monocypher.c` and `src/monocypher_ed25519.c`: the two can't share a unit) and
  `bmds/updatepackage.h` (`headBytes()`, `parse()`, `verify()` against a list of keys). Unused
  until step 2, so the firmware's size is unchanged.
- **`tools/mkupdate`** (`cryptography`'s Ed25519, `hashlib`'s BLAKE2s): `keygen KEY PUB` (PEM,
  mode 600, never overwrites; PUB is `ed25519 HEX COMMENT`), `make OUT --board [--key] [--firmware]
  [--rom] [--recovery] [--notes] [--partitions]` (the version, build id and chip revision from
  the app image's header and description; the app's own appended SHA-256 checked; the ROM's
  `BMRO`; sizes against `partitions.csv`; the ABI version from `sdk/include/bmds/sys.h`), `info
  FILE [--pub PUB]` (the hashes and the signature; exit 1 if anything fails). On this
  build: the firmware (2.0 MB) and the ROM (3.5 MB) made a 5.5 MB package in a second.
- **Tests:** RFC 8032's vectors 1 and 2 (sign and verify, a flipped bit refused); a package made
  by `mkupdate` with the dev key (`tests/host/update/test.bmup`; `keys/dev.pub`) parsed and
  verified by the C++, its notes' hash checked; packages built in the test: each field, two keys,
  an altered byte, a newer format or flag, a short or long header size, too many sections, bad
  text, sections cut off, inside the head or twice. **Fuzzed** (`updatepackage_fuzz`, seeded with
  `test.bmup`): 573,000 inputs in 5 minutes (each parsed one also verified), nothing found.
- **The keys** (§2's acceptance mode; `keys/README.md`): `keys/dev.key` and `dev.pub`, made by
  `mkupdate keygen`; `keys/release.pub` when the user makes a release key (outside the
  repository). `bmds_update_keys()` (`cmake/bmds.cmake`) turns the mode's `.pub` files into
  `updatekeys.inc` at configure time (reconfigured when `keys/` changes; a release mode with no
  key is a configure error, checked), and `bmds_kernel`'s `UpdateKeys::accepted()` and
  `isDevMode()` give them to the firmware: rodata, unused until step 2 (the image is the same
  size). `mkupdate make` signs with `keys/dev.key` unless given `--key`, and `info` checks
  against `keys/`'s keys and names the one that signed.

## 9. As built: the `update` target (2026-10-13)

`build update` (the top-level `CMakeLists.txt`) makes `build/bmds.bmup` from that build's
`bmds.bin` and `rom/rom.bin` for its board (`CONFIG_BMDS_BOARD_DIR`), after building both; it is
not part of the default build. It signs with `BMDS_UPDATE_KEY` (a cache variable; empty means
`keys/dev.key`) and carries `BMDS_UPDATE_NOTES` if set. `mkupdate make --accept PUB` refuses a
key that isn't in PUB, and the target passes the keys the acceptance mode builds in, so a release
build signed with the dev key fails instead of making a package its own firmware would refuse
(checked with a stray key).

## 10. The version (the user's, 2026-10-13)

The package carried only `PROJECT_VER`, which IDF made the commit ("72cba49a-dirty"): nothing
could tell newer from older. Now:

- **One source:** `bmds/version.h`'s `BMDS_VERSION_MAJOR/MINOR/PATCH`. `cmake/version.cmake`
  (included before `project()`) makes `PROJECT_VER` that and the commit: `0.1.0+72cba49a`, with
  `-dirty` for uncommitted changes (as of configure time, as IDF's was). At most 31 characters
  (the app description's field; configure fails past it): each part under 10000 keeps it so.
- **In the header** (136 bytes now; format still 1, nothing has shipped): u16 major, minor, patch
  and u16 0 after `madeAt`, at 128. `UpdatePackage::Version` orders them (`<=>`). The text stays
  as the label. `mkupdate` takes the number from `bmds/version.h` and refuses a firmware image
  whose label starts otherwise (a stale image); a package with no firmware gets the number as its
  label.
- **Downgrades** are allowed, since an older package is the only rollback, but confirmed: `update
  apply` refuses a package that isn't newer without `--force`, and the Updater warns and asks
  (step 2 and 4).
- **Checked:** the host tests (the order; the reserved u16 refused), `test.bmup` remade in the
  new format, the fuzzer for 2 minutes (54,576 inputs, nothing), the firmware says "App bmds
  version: 0.1.0+72cba49a-dirty", and `build update`'s package shows `0.1.0
  (0.1.0+72cba49a-dirty)`.

## 11. As built: step 2, recovery and the headless update (2026-10-13)

- **The recovery build** (`recovery/`: its own `main`, `sdkconfig.defaults` read after the main
  one's, `CONFIG_BMDS_RECOVERY`, `BMDS_APP_PARTITION` `ota_1`): the console, `/boot`, sysconf, the
  log, the card and the shell, **634 KB (70% of `ota_1` free)**. The shell runs from its `main`
  through `Shell::programMain()` with no launcher: the program loader brings in the whole system
  (audio, the network, the synth...), so card programs (`tail`) don't run in recovery; the 76
  built-in commands do. `bmds_storage` builds no ROM image for it. Its sysconf declares 13 keys and
  keeps the saved values of the rest ("3 saved values kept for keys not declared"). Built
  separately in `build-recovery/` (a build nested in the main one would run its own unpaced
  parallel build); the main `flash` writes it to `ota_1` and `build update` carries it when it
  exists.
- **`reboot recovery`** (`System::setNextFirmware()`: `esp_ota_set_boot_partition()`, which checks
  the image first, so a missing or damaged recovery is refused: checked). In recovery, a plain
  `reboot` goes back to the main firmware only if it checks; otherwise it says so and stays. Its
  shell's prompt starts with a red `recovery` (the default `PS1`, the user's request).
- **Flash work needs an internal stack:** `esp_ota_set_boot_partition()` from the shell (a PSRAM
  stack) asserted in `spi_flash_disable_interrupts_caches_and_other_cpu` (the first `reboot
  recovery`). `Thread::runOnInternalStack()` runs such work on a kernel thread with an internal
  stack and waits; the update's whole job (card reads, hashing, erase and write) runs on one
  (12 KB stack, a 16 KB internal buffer).
- **`bmds_update`** (`bmds/update.h`): `check()`, `apply()`, `status()`, the boot stage `init()`,
  and the `update` command: `check FILE` (board, version against `ota_0`'s, when made, which key,
  each section the same as or differing from its partition, `Check: good` or why not; exit 1 if
  not good), `apply [--force] FILE`, `status`. The intent (`/boot/update/intent`: the package's
  path and the BLAKE2s of its signed head) and the result (`/boot/update/result`: `status:
  applied|failed`, package, version, when, message) are text. The setting `update/dir`
  (`/sd0/p0/update`) is where `tools/bmds-update` copies packages.
- **The flow as §3**, with these settled: the main firmware writes `ota_1` only if the package's
  recovery differs (read back against its hash), and says "installed already" (no restart) when
  the firmware and ROM are flashed already. Recovery waits for the card's first mount, checks the
  package again and that its head hashes as the intent says ("the package changed since it was
  chosen"), writes `ota_0` and `rom` where they differ (each read back, and written again up to
  twice when it reads back wrong), then tries to make the main firmware next: if it checks, the
  intent goes and it restarts; if not, recovery stays with the intent kept and its shell open,
  where `update apply FILE` (which writes its own intent first) repairs it. The main firmware logs
  a new result once at boot (`/boot/update/unseen`). Recovery has no version check: it repairs.
- **Tested on the board** (the dev key; packages from `build update` or `mkupdate`): a firmware
  install (19.5 s in recovery: checking 5 s, erasing 2 MB about 10 s, writing and reading back
  4 s); a reset (the console's RTS) during the erase, during the firmware's write, during the
  ROM's write (the ROM damaged first: back with its content hash), and during the main firmware's
  write of `ota_1` (the main firmware boots, nothing pending; applying again writes it); an intent
  whose package changed, and one whose package is missing (failed, back to the main firmware,
  which logs why); a missing package with `ota_0` damaged (recovery stays; `update apply` on its
  console installs and restarts); a damaged package (`Check: bad: its contents don't match their
  hash`), one signed by another key, the same version without `--force`, and `tools/bmds-update`
  from copy to result (68 s). A bug found and fixed on the way: recovery restarted in a loop when
  it meant to stay (one code had meant both "failed, the main firmware good" and "stay").
- **The IDF bootloader's fallback to `ota_1` doesn't work on this IDF:** with `ota_0` zeroed it
  said "image at 0x410000 has invalid magic byte" though `ota_1` read back good (`e9`) with
  esptool. `bootloader_flash_read_allow_decrypt()` invalidates the cache at `FLASH_MMAP_VADDR`
  after moving its window, which is at `FLASH_READ_VADDR`, so `ota_1`'s header comes from the
  cache as `ota_0`'s zeros; upstream master invalidates `FLASH_READ_VADDR` (checked 2026-10-13).
  The update flow doesn't need the fallback (`otadata` names recovery all the while `ota_0` is
  written), but a broken `ota_0` with `otadata` naming it does: an IDF update (or that one line)
  fixes it, the user's call.
- **Open:** one check in about thirty found a section's hash wrong on a package that checked good
  before and after (the same bytes on the card; 18 checks since, 6 just after a boot): a bad read
  not yet explained, hence recovery's second write. The check reads 6.2 MB and hashes as much
  flash: 5 s. The main firmware grew 47 KB (Ed25519, IDF's OTA code, the engine).

## 12. Step 3: recovery's screen (plan, 2026-10-13)

What §5 asks of recovery, made of what the GUI is made of below its window manager:

- **The display** at a fixed 1280×720 at 60 Hz (CEA), single-buffered: `Display::start()` from a
  boot stage, not `display/mode` (recovery doesn't trust the user's settings; 720p is what every
  HDMI sink takes). No display: recovery runs on, headless.
- **The picture:** a band across the top, "Recovery Mode" and the version, in the recovery color
  (amber on dark red); under it the update panel (only while an update runs or has a result: the
  package's version, the stage in words, a progress bar and percentage; then "Installed,
  restarting" or what failed and what to do); the rest a terminal running a second recovery shell
  (the first stays on the serial console). Text is unscii 8×16, drawn by `GlyphRenderer` into a
  strip of internal DMA RAM (a row of cells: 1280 × 16 × 3 bytes) and copied with
  `Surface::blit()`; the bands and the bar are `fill()`s. Only what changed is drawn.
- **One task** (`screen`, internal stack: it waits for the DMA) waits on a semaphore that the
  terminal's changed hook and the update's progress give; no timer, no polling.
- **The keyboard:** USB host and the HID decoder (`UsbHost`, `Input`), the screen as `Input`'s
  sink, keys encoded by `TerminalKeys` into the terminal's input. The mouse is ignored.
- **The update's progress** reaches the screen through an observer on `Update` (`setObserver()`:
  progress, and the end with its result), called from the update's job.
- **Size:** `bmds_drivers`, `bmds_ui` (whole, for its font and renderer: 121 KB), `bmds_console`,
  `bmds_input` and TinyUSB come in: about 250 KB, recovery about 900 KB of `ota_1`'s 2 MB.
- **Also in this step:** recovery's shells read neither `/boot/profile` nor `autoexec.sh` (they ran
  the user's startup: what recovery repairs may be what breaks it).

## 13. As built: step 3, recovery's screen (2026-10-14)

- **As §12**, in `recovery/main/recoveryscreen.cpp`: the display at 1280×720 single-buffered
  (`display/mode` not read), the band ("Recovery Mode", the version and the board), the panel, a
  rule, and a terminal of 158 × 36 cells running recovery's second shell. One task (`screen`, 8 KB
  internal stack) draws on a semaphore from the terminal's changed hook and the update's
  observer (`Update::Observer`: `started`, `progress`, `finished`, from the job). Terminal rows are
  hashed and only rows that changed are drawn. Keys: `Input`'s sink, `TerminalKeys`, the console's
  input. **Recovery is 918 KB** (56% of `ota_1` free; it was 635 KB): `bmds_drivers`, `bmds_ui`,
  `bmds_input`, TinyUSB.
- **The panel's phases:** idle (how to install from the card, what `reboot` does); installing (the
  version, the stage, "Don't turn the power off", the bar and percent); installed ("Restarting...");
  failed and restarting ("The system is as it was"); failed and staying (put a card with an update
  in and `update apply FILE`, or restart to try again). Recovery holds the end on the screen 2 s
  before it restarts (1.5 s after `update apply` typed in it).
- **Flicker:** redrawing the whole panel on each step of progress showed it half drawn (seen in a
  screenshot); progress now redraws only the stage's line and the bar, each replacing its pixels
  in one copy. **The erase** (most of a section's time: 17 s for the ROM) counts as a third of
  each section's progress, erased 256 KB at a time: the bar no longer sits at 0% through it.
- **Recovery's shells read neither `/boot/profile` nor `autoexec.sh`** (`prepareInteractive()`
  says so): they had run the user's startup, the card's included.
- **Tested on the board** with `screenshot` (in recovery through `bmds_ui`) saved to the card and
  fetched from the main firmware: idle, installing (the ROM at 24%, the firmware at 12% during
  its erase), installed, failed and restarting (a missing package), failed and staying (`ota_0`
  damaged), and a real update of both firmwares (the main firmware wrote the new recovery, which
  installed the firmware). **Not tested: typing on the USB keyboard** (it enumerates; nothing here
  can press its keys); the mouse is ignored.
