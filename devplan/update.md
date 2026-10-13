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
  no factory partition, IDF's bootloader falls through to `ota_1` when `ota_0` won't load.
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
2. **Check:** `update check FILE`: the checks of §2, the versions now and in the package, the
   sections and whether each differs from what is flashed (machine-parsable, as `rom info`).
3. **Apply:** `update apply FILE`: the checks again; if it carries a recovery firmware that
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
  recovery. A package can carry a new one (section 3).
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
- **The format as §2,** with these settled: the header's 128 bytes are `BMUP`, u16 format, u16
  header size, u32 flags, u32 section count, u32 ABI version, u16 least chip revision, u16 0,
  board[32], version[32], build id[32], i64 when it was made (by `mkupdate`, not the firmware's
  build); a section is u32 type, u32 flags, u32 offset, u32 size, BLAKE2s-256[32]. **Flags this
  code doesn't know are refused** (`NewerFormat`), not ignored: they may change what a package
  means. Unknown section types are skipped; any type listed twice, or a section inside the head
  or past the file's end, is refused.
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
