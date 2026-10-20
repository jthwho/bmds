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
5. **Later:** the crash loop counter; a key at power-on (§17); fetching from a URL (postponed).
5a. **After step 4 (the user's, 2026-10-14):** IDF's app rollback for the switch to a new recovery
   (on probation until its update stage runs; a crash before falls back to `ota_0`), if the
   bootloader has room (1.2 KB free: measure first); and `update status` in the main firmware
   saying when `ota_1` isn't the recovery of the last package applied (one applied from recovery
   can't replace recovery). Recovery already goes first in every update that carries a different
   one (§11), so the installer is always the newest.
6. **At the very end, the partition sizes** (done: §19) (the user's, 2026-10-13): `ota_1` cut to recovery's
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
  `-dirty` for uncommitted changes. At most 31 characters (the app description's field; the
  build fails past it): each part under 10000 keeps it so.
- **The result's time** (2026-10-19): recovery wrote "when: 1955-11-05": its clock is never set,
  since the main firmware's clock record is in RTC memory where each image's linker put it, and
  an unset clock counts from 1955 (`ClockModel::UnsetEpoch`). Recovery now leaves the line out
  while its clock is unset, and the main firmware adds it from its own clock when it takes the
  result, seconds later (checked: an update through recovery reads `when: 2026-10-20 01:02:16 UTC`).
- **Current at every build** (2026-10-19): IDF compiles `PROJECT_VER` into the app description as
  it was at configure time, so a commit since then left the old one (a package of new code said
  `a1ad6d82` two commits later, and `update` called it "the same"). After `project()`,
  `bmds_version_every_build()` (`cmake/version.cmake`) adds a target that runs before every build
  and writes `bmds_version_text.h` in the build directory, only when the text changed; it is
  force-included into `esp_app_desc.c` and redefines `PROJECT_VER`. An unchanged build compiles
  nothing; a changed version recompiles that file and relinks. Both firmwares do it.
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

## 14. Step 4: the Updater (plan, 2026-10-14)

The user's choices (2026-10-14): **no syscalls yet** (the Updater calls `bmds/update.h` as the
other built-in applications call the kernel's C++; the SDK has no GUI for a separate Updater to
be built with, and the `update` command stays the scripts' contract; syscalls come when a
separately built program needs them), and **a notification after an update** (posted at boot,
opening the Updater) rather than the Updater opening itself.

- **`/bin/updater [FILE]`**, a built-in GUI application (`bmds_updater`, the toolkit only, as the
  image viewer). With FILE: that package. Without: the newest package in `update/dir`, or "No
  update packages in DIR" with the last update's result.
- **Its window:** the package's version against the installed one ("newer", "the same", "older"),
  when it was made and which key signed it, the check's result in words, the release notes (the
  `Notes` section, scrolled), and **Install** (and Close). A package that isn't newer: Install asks
  "Install an older version?" first (the command's `--force`). A bad package: Install is off and
  the reason is shown.
- **Install:** the checks run again; the recovery firmware is written if it differs, with a
  progress bar ("Writing the recovery system"); then "Restarting to install..." and the restart
  (`Update::apply()`, then `Vfs::sync()` and `System::restart()`, as the command does).
- **The file type:** `.bmup` and the magic `BMUP` are `application/x-bmds-update` in `FileFormat`
  (append only); `mimeapps.list` opens it with `updater.desktop`; a launcher entry "Updater".
- **After:** the main firmware's `Update::init()` posts a notification once (on the
  `/boot/update/unseen` marker it logs on now): "BMDS VERSION installed" or "The update wasn't
  installed" with the reason, its target `updater.desktop`, which shows the last result when no
  package is newer.

## 15. As built: step 4, the Updater (2026-10-14)

- **`/bin/updater [FILE]`** (`components/bmds_updater`, 7.5 KB): as §14. The window shows the
  version, the installed one and which is newer, when it was made and the key, the check's
  verdict, the release notes (a `ScrollArea` that wraps them at word ends, in a field's frame),
  then a progress bar and its stage while installing. Checking and installing run on a thread of
  the program's own and are posted to the loop; progress is posted at most once until it is drawn.
  A package not newer asks "It isn't newer than what is installed. Install it?" first. Close is off
  while it installs. After `Update::apply()`: "Restarting to install...", a second, `Vfs::sync()`
  and the restart. With no FILE: the newest `.bmup` in `update/dir` by its time, or "No update
  packages in DIR" with the last result (and what is pending).
- **The file type:** `.bmup` and `BMUP` at 0 are `application/x-bmds-update` (`FileFormat`, host
  tested); `mimeapps.list` opens it with `updater.desktop` (new boards; the user's board's
  `/boot/config/mimeapps.list` got the line by hand: existing `/boot` files aren't replaced).
  Icons appended to `rom/icons.list`: `system-software-update` (the application's, flat-color's
  synchronize) and `application-x-bmds-update` (the file's, its package).
- **After an update:** the main firmware's boot stage posts "Update installed" ("BMDS VERSION is
  installed.") or a critical "The update wasn't installed" with the reason, from the Updater, its
  icon, opening `updater.desktop` when clicked; the balloon shows 15 s (a critical one stays):
  with the default 6 s it was gone before the desktop was looked at, though the panel shows those
  posted before it started (checked with a 30 s timeout at boot).
- **Long notes** scroll: the wheel, the bar, and the keys (Up, Down, Page Up and Down, Home, End)
  once the pane has the focus (a click or Tab). They are wrapped again when the vertical bar
  appears, which narrows them (the first version cut the lines' ends and showed a horizontal bar:
  seen with 60 entries). Notes over 64 KB show their first 64 KB, cut at a whole line, and say
  the rest isn't shown (checked with 71 KB).
- **Tested on the board** (screenshots, keys by `win hid`, clicks by `win pointer`): the window on
  a package with notes; Install on the same version (the question, then Alt+I): progress, the
  restart, recovery installed it (the firmware differed), back with the result; the notification
  at boot; the window with no packages; a `.bmup` opened by its type (a notification targeting
  the file, clicked).

## 16. App rollback, and flash auto-suspend under load (2026-10-14)

- **Rollback** (§6 5a): `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` costs the bootloader 128 bytes
  (0x5b30 → 0x5bb0, 0x450 free: measured in a scratch build first). A firmware switched to
  (`esp_ota_set_boot_partition()`) boots on probation and confirms itself (`Update::confirm()`,
  `esp_ota_mark_app_valid_cancel_rollback()`): recovery in its update stage, the main firmware in
  a last boot stage (`healthy`). One that resets first is abandoned by the bootloader for the
  other. **A new recovery that doesn't start:** the main firmware boots again with the intent
  still on /boot, reports "the recovery firmware it brought didn't start" (the result, the
  notification) and drops the intent. **A main firmware that doesn't start:** recovery runs
  without being chosen (its own entry isn't on probation) and says "The system didn't start" on
  its screen, with what to do. Existing boards need the bootloader flashed once over USB (a
  package doesn't carry it). `update status` gains a last line, `Recovery: VERSION`, saying when
  it isn't the system's (an update applied from recovery can't replace recovery).
- **Found while testing it: flash writes fault reads under load.** Writing recovery to `ota_1`
  from the main firmware panicked on core 1 (an instruction access fault in `snprintf`, the stats
  task). Reproduced: `ota_1` rewritten 20 times on an idle system, no fault; with a second session
  reading `/rom` (mapped flash) in a loop, a fault on the first write, twice (a load access fault;
  the address in the firmware's rodata, then in `/rom`); erasing 4 KB sectors instead of 64 KB
  blocks, the same. With `CONFIG_SPI_FLASH_AUTO_SUSPEND` (devplan/audio.md §17: the caches stay
  on during an erase or a write, for the audio engine), a cache read that suspends the operation
  sometimes reads wrong: every fault was a flash address, on core 1. The W25Q128JV's tSUS is 20 µs
  (its datasheet), within IDF's 50; IDF documents neither the P4 nor this chip for the feature,
  and this P4 is revision 1.3 (`ESP32P4_SELECTS_REV_LESS_V3`, no separate resume timing). It
  affects every flash write, `/boot`'s too, when something reads flash meanwhile.
- **Tried, to keep it:** IDF's own tuning (`SPI_FLASH_AUTO_CHECK_SUSPEND_STATUS`, tSUS 100 µs):
  a fault on the first write under load again. **With auto-suspend off:** 12 rewrites under the
  same load, none (slower: the read loop and the writes take turns at the flash).
- **The user's choice:** auto-suspend off everywhere (IDF's default), the audio glitch during a
  flash write accepted ("the alternatives are lipstick on a pig": an exclusive path for updates
  alone, or a playout reserve in internal RAM). `flashchips.c` (the board's chip list, there only
  to allow suspend) is gone; `sdkconfig` regenerated from the defaults.
- **Rollback, tested on the board** (packages with a firmware or a recovery that `abort()`s at
  once, built for the test and not kept): a recovery that doesn't start: aborted, the bootloader
  back to the main firmware, which recorded "the recovery firmware it brought didn't start" and
  posted a critical notification; a main firmware that doesn't start: installed by recovery,
  aborted, the bootloader back to recovery, whose screen says "The system didn't start" with what
  to do; `update apply` there installed a good one, which confirmed itself and stayed through a
  reset. The normal path (`reboot recovery` and back) confirms each side quietly. With
  auto-suspend off, writing `ota_1` from the main firmware didn't fault (12 under load, and these).

## 17. Step 5: fetching, the restart counter, the key at power-on (plan, 2026-10-14)

**What is there to build on.** BMDS runs its own lwIP (vendored, `bmds_net`) behind `Socket` and
`NetStack::resolve()`, not IDF's: IDF's `esp_http_client` and `esp-tls` need IDF's lwIP and its
sockets, and can't be used. mbedTLS is in the build (TLS 1.2 client, IDF's full certificate
bundle), its allocations internal RAM today. Boot takes 0.64 s from reset to the shell; Ethernet
starts at about 0.61 s; a USB keyboard is there at about 1.3 s.

- **Fetching (the main firmware only).** A small HTTP/1.1 client: `http://` and `https://`
  (mbedTLS over `Socket`, its send and receive as mbedTLS's BIO; the server checked against IDF's
  certificate bundle; the clock is NTP's), redirects (up to 5), `Content-Length` or chunked,
  `Range`. The parsing (the URL, the status line and headers, chunks) is portable, in `bmds_core`,
  host-tested and fuzzed; the connection in `bmds_net`. mbedTLS's allocations go to PSRAM
  (`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`, through `Memory` with a named space). A package is signed,
  so plain HTTP can't change what is installed; HTTPS is for the servers that require it (most
  release hosts redirect to it). Measured on the build before it lands: flash (mbedTLS's TLS and
  X.509, the bundle: about 200 KB expected, of 2 MB free) and the RAM of one connection.
  - **`update fetch [--check] [URL]`** (append-only, a new subcommand): downloads the package at
    URL (default: the setting `update/url`) into `update/dir` under its name (written as
    `NAME.part`, renamed when whole), then checks it and prints what `update check` prints. With
    `--check`, only the head is read (a `Range` request for the first 4 KB: the header, the
    sections and the signature), checked, and its version said against the installed one; nothing
    is written. Then `update apply FILE` as now.
  - **The setting `update/url`** (new, default empty): where this board's packages are published,
    one URL that always names the newest (a release host's "latest" link). No feed or manifest
    format: the package's own signed head is what is compared.
  - **The Updater:** with `update/url` set, **Check for updates** (the head only, as `--check`),
    then **Download** with a progress bar, then the existing Install. Recovery doesn't fetch (no
    network in it, and it has no room for TLS once `ota_1` shrinks): a package for recovery is
    put on the card.
- **The restart counter (the main firmware).** A crash or a watchdog reset (`panic`, the
  watchdogs, a CPU lockup: `esp_reset_reason()`) adds one to a count kept in RTC memory (kept over
  resets, lost at power off); any other reset (power on, `reboot`, the reset button) clears it;
  so does running 60 s after the `healthy` stage (a one-shot timer). At **3**, the main firmware
  leaves `/boot/update/restarts` and starts recovery (`setNextFirmware(Recovery)`), whose screen
  and log say "The system kept restarting", with what to do (`reboot` tries again; or install an
  update). Counted in the `update` stage, moved up to just after `boot` (the first that can write
  /boot), so `sysconf` and later stages' crashes count; a crash before it is a firmware that never
  starts, which rollback catches when it is new. Not a setting: the settings are among what may
  be broken.
- **The key at power-on.** The BOOT button can't be it: it is a strapping pin (held at reset, the
  chip starts its ROM's download mode) and Ethernet's TXD1 once Ethernet starts, 0.61 s after
  reset. A **USB keyboard key held** can: a key down in a keyboard's first report (held as it
  was attached), in the first 10 s after reset, starts recovery (`setNextFirmware(Recovery)`)
  before the desktop starts. Recovery's screen says "Recovery Mode" as for `reboot recovery`.

**The user's choices (2026-10-14):** fetching **postponed** (the plan above stands for when it
comes back); the key is **Esc**; the restart limit **3**.

## 18. As built: step 5, the crash counter and Esc (2026-10-14)

- **`Update::watch()`**, a boot stage (`recovery`) just after `boot`, main firmware only. The count
  is in RTC memory (magic, count, its complement: a count that doesn't check, as after power on or
  under recovery's own RTC data, is 0). A reset by a panic, a watchdog or a CPU lockup adds one;
  any other clears it; so does a one-shot FreeRTOS timer (static, 60 s) started by `confirm()` in
  the `healthy` stage. At `Update::RestartLimit` (3) it writes `/boot/update/restarts`, sets
  recovery next and restarts. Recovery takes the marker and reports
  `Update::SystemKeptRestarting` as it does `SystemDidntStart`: "The system kept restarting." on
  its screen, with the same advice.
- **Esc:** `Input::setAttachKeyHandler()` (new): the USB host passes the keys in a keyboard's
  boot reports during its first second attached (`Input::AttachKeyMs`). `watch()` sets a handler
  that starts recovery on Esc within 10 s of reset.
- **Rollback comes first:** a firmware still on probation (just installed, or just returned to
  from recovery) that crashes before `healthy` is abandoned by the bootloader on the first crash
  ("The system didn't start"), before the count matters. The count catches a confirmed firmware
  that crashes after `healthy` (a broken setting, a program the profile starts).
- **Tested on the board** with a test build (a stage after `healthy` that aborted while a count
  in `/boot/crashtest` was above 0, and a `crashtest` command; not kept):
  - three crashes in a row: "crash 1/2/3 of 3", recovery, "recovery runs because the system kept
    restarting";
  - two crashes, then a good boot: stays in main;
  - a crash after 62 s up: counted as 1; another at once: 2.
  - Esc held through a reset: recovery (the user, by hand, 2026-10-14).
- **Found on the way: `/boot` wasn't durable on `sync`.** `FlashPartition::sync()` returned `Ok`
  without waiting for the write queue, so `Vfs::sync()` (the `sync` command and system call too)
  didn't wait for `/boot`. The test's countdown, written and synced before `abort()`, was lost
  every time. `sync()` now drains the queue (`flush(WaitForever)`), and the countdown kept. The
  intent, the result and the marker are all synced before a restart, but a restart flushes anyway
  (the shutdown handler); a panic doesn't.
- **Sizes:** main 51% free, recovery 56% (unchanged to the KB), bootloader unchanged.

## 19. As built: the partition sizes (2026-10-14)

- **The table** (`partitions.csv`, `rom.md` §3a): `ota_1` 2 MB → **1.375 MB** (0x160000; recovery
  is 0.90 MB, 36% free: "plus about half" rounded to the 64 KB page every partition starts on),
  `otadata` 0x610000 → **0x570000** (and its 56 KB spare after), `/boot` 0x620000 → **0x580000**
  (512 KB, as was), `rom` 0x6A0000 → **0x600000**, 9.375 MB → **10 MB** (the ROM is 3.4 MB).
  `ota_0` and `nvs` stay. Nothing in the code named an offset; the size check reads the table.
- **Moving a board** (a package can't change the table, so one USB flash each): `/boot` is
  copied rather than reformatted, so its settings and keys stay (littlefs doesn't depend on where
  it is):
  ```
  esptool.py -p PORT --after no_reset read_flash 0x620000 0x80000 boot.img   # the old /boot
  esptool.py -p PORT --after no_reset write_flash 0x580000 boot.img           # at its new place
  idf.py -B build -p PORT flash                                               # the new table, all
  ```
  (`sync` on the board first. `flash` doesn't write `boot`; the image lands where the new
  `ota_1` no longer reaches.) Without the copy, the first boot finds `/boot` blank and makes it
  again with the defaults.
- **Tested on the board:** moved as above; `/boot` came back whole (settings, the SSH host key,
  `home`); `rom` says 3440 KB of 10240 KB, matching the firmware; then two updates through
  `tools/bmds-update` (a build with a changed log line, then the real one): recovery written to
  the new `ota_1`, the firmware by recovery, back to main; `update check` after: every section
  the same as flashed.
