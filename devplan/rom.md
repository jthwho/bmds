# The ROM (`/rom`) and the Flash Layout

Planned 2026-10-09. The user's idea (2026-10-09), made while the media player wanted icons the
default theme lacks: "provide a way for the image to add builtin files perhaps in a /system
folder ... bake in a read-only filesystem for things like standard icons", then "I suspect
that'll also allow us to mmap these files later?" The user's decisions the same day (§4): the
name `/rom`; a 2 MB recovery firmware, a 4 MB main firmware, a 512 KB `/boot`, the rest the
ROM; the device may be reformatted. It comes before `mediaplayer.md` step 3.

## 1. Goal

- **Files that come with the firmware,** readable by every program as ordinary files, read-only:
  standard icons first, then the time zone database and whatever else the system ships.
- **No card needed:** a board without a card still has icons, file-type icons and time zones.
- **Nothing copied into RAM to use them:** the partition is mapped through the flash cache, so a
  file's bytes are at an address. The kernel uses them in place (`Vfs::map()`), and later
  programs' read-only `mmap()` can too (§6).
- **Overridable:** the same layout as the card's `share/`, looked up after the user's and the
  card's, so installing a newer theme or database on the card wins.
- **A flash layout for the long run:** room for a recovery firmware and updates, and for `/boot`
  to give space to the ROM later.

## 2. What exists to build on (checked 2026-10-09)

- **Flash** (16 MB, `partitions.csv`): `nvs` at 0x9000, `factory` (app) 4 MB at 0x10000 (the
  firmware is 1.32 MB), `boot` (littlefs, `/boot`) 4 MB at 0x410000; 0x810000 to the end (8 MB)
  is not partitioned. `fsbench` uses a scratch region past the last partition (`storage.md` §13,
  4 MB), which the new table takes.
- **OTA** isn't built (`foundation.md`: "OTA slots and a coredump partition, once there is a
  reason").
- **Reading flash through the cache** is already done for `/boot` (`storage.md` §13: mapped reads
  cut littlefs's pauses from 783 to 238 ms); the flash partition device keeps a mapping.
- **Files in the firmware today:** `boot/` is embedded (`tools/embedfiles.py`) and copied onto
  `/boot` when missing (`gui.md` G14). This suits files meant to be edited; each costs flash twice
  (in the app and on `/boot`). Programs' own icons are embedded per program
  (`BMDS_PROGRAM_ICONS`).
- **The VFS:** filesystems are `Volume`s (`bmds_fs/src/fslayer.h`) mounted by
  `Vfs::mount(path, BlockDevice&)` after `identify` recognizes them; at most `MaxMounts` = 4,
  and `/boot`, `/sd0/p0` and `/ram0/p0` use three. `/bin` and `/share/zoneinfo` are virtual.
- **Icons:** `IconTheme` reads PNGs (16 and 32 pixels) from `/boot/icons`, then the card's
  theme (`/sd0/p0/share/icons/THEME`), its parent, `flat-color`, `hicolor`; it decodes each into
  PSRAM and keeps up to 64 KB of them. The themes in the repository: flat-color (2.6 MB of PNGs,
  329 names; no play, pause or stop) and glyphs (6.8 MB; has them).
- **The time zone database:** one bundle (`BMTZ`, about 234 KB) on the card, shown as
  `/share/zoneinfo` (`time.md` §9).
- **The MMU** (`programs.md` §11): flash at 0x40000000 and PSRAM at 0x48000000, 64 KB pages.
- **IDF's bootloader:** with `otadata` choosing an OTA slot, it boots that slot; with `otadata`
  blank, the `factory` partition. With rollback on
  (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`), a new image that never confirms itself is abandoned
  at the next reset. `idf.py flash` writes the app to `factory` when there is one.

## 3. Design

### 3a. The flash layout (the user's, 2026-10-09)

| Partition | Type | Offset | Size | |
|---|---|---|---|---|
| `nvs` | data, nvs | 0x9000 | 24 KB | as now (IDF's own use) |
| `ota_0` | app, ota_0 | 0x10000 | 4 MB | the main firmware (where the app always was) |
| `ota_1` | app, ota_1 | 0x410000 | 2 MB | the recovery firmware (§3b) |
| `otadata` | data, ota | 0x610000 | 8 KB | which firmware boots |
| (spare) | | 0x612000 | 56 KB | so `boot` starts on a 64 KB page |
| `boot` | data, littlefs | 0x620000 | 512 KB | `/boot`, read and write (§3h) |
| `rom` | data, 0x40 | 0x6A0000 | 9.375 MB | `/rom`, read-only (§3c) |

- **Recovery is `ota_1`, not `factory`** (changed in step 2): IDF's `idf.py flash` writes the
  app to `factory` when there is one, and its bootloader, with no `factory` and a blank
  `otadata`, boots `ota_0`, and if `ota_0` won't load tries the other slots
  (`bootloader_utility.c`, read 2026-10-09). So the main firmware flashes where it should with
  no change to IDF's flashing, a broken main firmware falls through to recovery by itself, and
  recovery is chosen by setting `otadata` to `ota_1`. The main firmware stays at 0x10000.

- **`/rom` last, after `/boot`:** if `/boot` is resized later, its start stays put and the ROM's
  start moves. The ROM is written whole by every flash and update, so moving it costs nothing;
  `/boot` keeps the user's files where they are (growing it is littlefs's `lfs_fs_grow()`;
  shrinking it, a copy off and back).
- **Every partition starts on a 64 KB page,** so a mapping's address keeps the image's own
  alignment (§3c's 4 KB-aligned files stay 4 KB-aligned in memory).
- **The main firmware is 1.32 MB** in its 4 MB; the recovery's 2 MB is the user's estimate for
  HDMI, the keyboard and mouse, and the card.
- **Moving to it:** `/boot` moves, so the first flash of the new table finds it blank and makes
  it again with the defaults (G14); anything kept on the board's `/boot` is copied off first
  (`sz`) and back. A coredump partition, if wanted later, comes out of the ROM's end.

### 3b. Booting, recovery and updates (the layout's part now; their firmware is later work)

- **Normally** `otadata` chooses `ota_0`, and the main firmware boots.
- **Recovery** runs when the main firmware won't load or fails its rollback check, or when
  asked: `reboot recovery` from the main firmware (it sets `otadata` to `ota_1`), and a key held
  at power-on (later, with the recovery's own plan).
- **An update** (later, its own plan): the main firmware checks and keeps the new firmware and
  ROM on the card, then reboots into recovery, which writes `ota_0` and `rom`, checks them, and
  sets `otadata`. The main firmware can't write itself or the ROM it has mounted.
- **The recovery firmware doesn't use `/rom`** (it may be what is being repaired): its icons and
  fonts are its own. It is a second build of the same components with a smaller configuration;
  its plan comes later. Until it exists, `ota_1` is empty.
- **`idf.py flash`** writes the bootloader, the table, the main firmware to `ota_0` (IDF's
  choice with no `factory`), a blank `otadata` (`app_update`'s, which boots `ota_0`), and the
  ROM.

### 3c. The ROM image

- **Built by the build:** `tools/mkrom.py` writes `build/rom.bin` from the repository's `rom/`
  tree and generated parts (the icon set, the zone bundle); CMake's
  `esptool_py_flash_to_partition(flash rom ...)` makes `idf.py flash` write it.
- **The format, `BMRO`** (little-endian, our own: simple to map, nothing to unpack):
  - **Header** (80 bytes): magic `BMRO`, format version, header size, the image's size, entry
    count, the offsets of the entries and the names, the content's hash (BLAKE2s-256 of
    everything after the header, 32 bytes), the build time (every file's time), a CRC-32 of the
    header.
  - **Entries** (16 bytes): the name's offset and length, the kind (file or directory), flags
    (System, Mappable), a file's offset and size or a directory's first child and count. Entry 0
    is `/`; a directory's children are contiguous, sorted by name (bytes), after it (breadth
    first), so a path is found a component at a time by binary search.
  - **Data:** each file aligned to 64 bytes (a cache line); a file flagged mappable for programs
    (§6) to 4 KB.
  - **Links** are files with the System flag holding Cygwin's link, as the VFS writes them on
    FAT and littlefs (`storage.md` §4), so the VFS follows them with no code of the ROM's.
- **Checked at mount, all of it:** the magic, the header's CRC, every entry's name and bytes
  inside the image, the children in order, the directories a tree (each child range follows the
  last, each child after its parent). Nothing after `open()` can read outside the image. The
  content's hash isn't checked at boot; `rom check` does it on demand.
- **Matched to the firmware:** the main firmware embeds the content hash it was built with. A
  mismatch (the ROM flashed from another build) is logged at boot and by `rom`, and the ROM is
  mounted anyway. A blank or damaged partition: no `/rom`, a warning, and everything falls back
  as it does today.

### 3d. The volume

- **`RomVolume`** (`bmds_fs`), a read-only `Volume` over a mapped span: stat, open (a stream
  reading from the span), directories (the children's range), links; writes are `ReadOnly`.
  `identify` recognizes `BMRO`; the storage service mounts the `rom` partition at **`/rom`** at
  boot, before anything reads it.
- **`Vfs::map(path)`** (new): a file's bytes in place as a `ByteSpan`, valid while the volume is
  mounted (this one never unmounts), or `NotSupported` for a volume that can't. The reader is
  pure and host-tested over a span in RAM, as `ZoneBundle` is.
- **`MaxMounts`** goes to 8 (its cost measured), so `/rom` doesn't take the last slot.

### 3e. What goes in (first)

```
/rom/share/icons/bmds.icons                   the bmds icon theme, one file, decoded (§3f)
/rom/share/icons/bmds.LICENSES                the two upstreams' licenses and credits
/rom/share/zoneinfo.bundle                    the time zone database (§3g)
```

- **The `bmds` icon theme** (the user's, 2026-10-09): BMDS's own theme, made from flat-color and
  glyphs, the theme every application BMDS ships uses, so they always have the icons they ask
  for (§3i).
- **Not in it:** a SoundFont (TimGM6mb is about 6 MB: it stays on the card), and the editable
  defaults (they stay on `/boot`, G14).
- **Later candidates:** stock `.desktop` entries and the MIME table read in place rather than
  copied (needs care: `/boot` has copies), sampled system sounds, fonts.

### 3f. Icons in place: one theme file, and icon ids (the user's, 2026-10-09)

- **The whole theme is one file, `bmds.icons`,** its pixels already decoded, so no icon is ever
  opened or inflated, and the ROM holds one entry for it rather than hundreds of files (and no
  PNGs: nothing reads the theme but `IconTheme`).
- **Icon ids:** the build generates `bmds/iconid.h` from `rom/icons.list`: `enum class IconId :
  uint16_t`, an enumerator a line (`IconId::Folder`, `IconId::MediaPlaybackStart`), numbered in
  the list's order. **The list is append only,** so an id never changes once shipped (the
  file's own version lets new ids be added; an id the file lacks is no icon, not a wrong one).
  - `IconTheme::find(IconId, side)`: an index into the file's table, no strings, no search.
  - **The size isn't part of the id** (asked 2026-10-09): an id names the icon, the size is
    `find()`'s argument, as in freedesktop.org's themes and Qt's `QIcon`. One name is one id
    (`idOf()` stays a plain search), a size added later (24, 48) changes no id, and the lookup
    is still two indexes (the id's row, the size's column).
  - `IconTheme::find(name, side)` stays for names from files (`Icon=` in a `.desktop` entry, a
    file type's name, a path) and other themes; `IconTheme::idOf(name)` is a binary search of
    the file's sorted names.
  - **Code that names an icon by id can't name a missing one:** it doesn't compile. The build's
    check (§3i) is left with the names that come from files and strings.
- **The format, `BMIC`** (little-endian):
  - **Header:** magic `BMIC`, format version, header size, the icon count, the sizes it holds
    (16 and 32 now; more can be added), the offsets of the table, the sorted index and the
    names.
  - **Table,** by id: the name's offset and length, and for each size the pixels' offset (0:
    none at that size).
  - **Sorted index:** the ids in the order of their names (bytes, as `strcmp`), for `idOf()`.
  - **Pixels:** 0xAARRGGBB, rows top first, as `Icon` holds them, each icon aligned to 64 bytes
    (a cache line). Mapped (`Vfs::map()`), `IconTheme` hands out an `Icon` pointing into the
    file: no PSRAM, nothing to evict.
- **With another theme chosen,** an id is looked up by its name in that theme first (the card's
  PNGs, decoded and kept as now), then here; with `bmds` itself chosen (the default), an id is
  only the index.
- **To measure first:** drawing an icon from flash through the cache against from PSRAM. If
  flash is much slower, `IconTheme` copies the pixels to PSRAM (still no decoding).
- **Budget:** about 200 icons × (1 KB + 4 KB) ≈ 1 MB, the tables a few KB (estimates, measured
  in step 2).

### 3g. The time zone database

- **The bundle in `/rom`** as well as on the card: `setZoneDatabase()` takes the card's if there
  is one, else the ROM's, read in place (`ZoneBundle` over the mapped span: no reads at all). So
  time zones work without a card.

### 3h. `/boot` at 512 KB (the user's, 2026-10-09)

- **What it holds:** the board's `/boot` used 48 KB of 4 MB (`df -h`, 2026-10-09): the
  defaults (`boot/` in the repository, 60 KB on the host, mostly files under 1 KB),
  `sysconf`, `random-seed`, `.defaults`. 512 KB is 128 blocks of 4 KB, ten times what it uses.
- **What could fill it:** `/boot/home` is the user's home when no card holds one
  (`sysconf.md` §7), with its `.trash` and programs' files in `.config` (the media player's
  playlist and library index). A board used without a card has only 512 KB for these; with a
  card (the normal case: `autoexec.sh` puts the home there), nothing changes. A full `/boot`
  is `NoSpace` to the program writing, as on any filesystem.
- **Wear:** littlefs spreads erases over the partition (`block_cycles` 500), so 128 blocks wear
  eight times faster than 1,024. Estimated: a few hundred block erases a day (sysconf saves, the
  volume's level, `random-seed`, the player's settings) is two or three per block a day; NOR
  flash is rated for about 100,000 erases, so decades. Looked at again if something starts
  writing often.

### 3i. The `bmds` icon theme (the user's, 2026-10-09)

- **Why:** neither upstream theme has every icon BMDS's applications need (flat-color has no
  play, pause or stop; glyphs is a different set again), and a theme on the card may be missing. One
  theme of BMDS's own, in the ROM, guarantees them.
- **Made from both upstreams by a list** in the repository, `rom/icons.list`, a line an icon:
  the freedesktop.org name (or BMDS's own, for what that specification doesn't name), the
  source theme (`flat-color` or `glyphs`) and the source icon:

  ```
  # NAME                  THEME       SOURCE
  folder                  flat-color  folder
  media-playback-start    glyphs      play
  media-skip-backward     glyphs      seek-skip-1  mirror
  ```

  An option after the source (`mirror`: flipped left to right) covers an icon an upstream draws
  only one way. `mkrom.py` takes each from the repository's rendered PNGs
  (`tools/render-icons`) at both sizes, decodes it and writes `bmds.icons` (§3f) and
  `iconid.h`. A line's place is its id, so lines are only added at the end.
- **Which upstream for which icon** is chosen icon by icon for a consistent look: flat-color
  where it has the icon (the look the desktop has today), glyphs for the rest. The user sees the
  set on the board (step 3) before it is settled.
- **What it holds:** every icon BMDS's own code and files ask for. The code asks by id (the
  compiler checks those); **a check in the build** (`tools/mkrom.py --check`) fails it on a name
  the list lacks among those that come as text: the file types' icon names in `FileFormat`, the
  `Icon=` of the `.desktop` files under `boot/` and `rom/`, and the panel's defaults. A name
  built at run time is listed in the check's own exceptions file with where it comes from.
- **The default theme:** `IconTheme::DefaultTheme` becomes `bmds`, and every lookup ends in
  it, whatever theme is chosen. The panel's `IconTheme` setting still chooses another theme
  (flat-color or glyphs from their packages on the card); what that theme lacks comes from
  `bmds`, so no application is ever without its icons.
- **Licenses:** both upstreams are MIT; `bmds.LICENSES` beside the theme carries both notices
  and where each icon came from (written by `mkrom.py` from the list).
- **The upstream themes stay** as they are: their packages (`icons`, `icons-glyphs`) install
  them on the card for anyone who wants a whole theme.

## 4. Decided (the user, 2026-10-09)

- **The name is `/rom`** ("clearer what it's doing"), not `/system`.
- **The layout:** a 2 MB recovery firmware, a 4 MB main firmware, a 512 KB `/boot` (first 4 MB,
  then "seems like that'd be enough"), the rest the ROM; the device may be reformatted (§3a,
  §3h).
- **A `bmds` icon theme** from flat-color and glyphs, the theme BMDS's applications use, so
  they always have what they need (§3i); **one file of decoded pixels, with icon ids** (§3f).
- **Recovery, not A/B slots:** considered first were two app slots and two ROM slots (updates in
  the background, rollback to the last version); the user chose a recovery firmware and one main
  slot, giving the main firmware and the ROM more room.

## 5. The `rom` command

- `rom` (or `rom info`): mounted or not, the format version, the build time, the content hash
  and whether it matches the firmware, the size used of the partition, the file count; also
  which firmware is running (main or recovery) and the partitions' places.
- `rom check`: the content's hash computed and compared (its time measured).

## 6. Mapping (the user's question, 2026-10-09)

- **In the kernel, from the start:** `Vfs::map()` (§3d) gives the icons (§3f) and the time zone
  database (§3g) their bytes in place.
- **For programs, later:** today a program's `mmap()` of a file copies it in (`programs.md`
  §12's table). A read-only mapping (`PROT_READ`, private or shared) of a `/rom` file can
  instead return the address in the partition's mapping: no copy, one copy shared by every
  program, nothing freed. A file flagged mappable is aligned to 4 KB so the address is page
  aligned as POSIX promises (the page size musl is told to be checked first).
  - **To check on the board first:** what a store to the flash window does (a fault the kernel
    turns into the program's crash, or silently nothing). With no memory protection between
    programs, a mapping can't be made unwritable otherwise.
- **Later still, ideas only:** a SoundFont's samples played from flash instead of loaded into
  PSRAM (if the synth's reads are fast enough from there); programs run in place from `/rom`.
  The second isn't simple: flash and PSRAM are separate windows of the MMU, so an image's shared
  pages in flash and its private pages in PSRAM can't sit side by side as `programs.md` §14 lays
  them out.

## 7. Budget (estimates)

- **Flash:** the ROM's 9.375 MB, used at first about 1.3 MB (icons about 1 MB, the bundle
  0.23 MB); the main firmware a few KB more (the volume, `Vfs::map()`, the command).
- **RAM:** none at rest beyond the mount's record; icons from `/rom` cost no PSRAM.
- **Boot:** the header read and checked (well under a millisecond).

## 8. ABI impact

- **New and kept once shipped:** the path `/rom` and its layout under `share/`, the `rom`
  command's interface, the `BMRO` format (its version field lets it grow), the `BMIC` format,
  and the `bmds` theme's names and ids (added at the end, never removed, renamed or renumbered).
- **The flash layout:** `/boot` moves (boards are reflashed once and `/boot` made again); not a
  userspace interface. `reboot recovery` is appended to `reboot`'s interface when recovery
  exists.
- **Not frozen yet:** `Vfs::map()` (a kernel class). No SDK change until programs' `mmap()` uses
  it (§6).

## 9. Steps

1. **The format and the volume, host-tested** (done, 2026-10-09): `mkrom.py`, `RomImage`,
   `IconPack`, `MemoryBlockDevice`, `RomVolume`, `Vfs::map()`, `MaxMounts`; the `bmds` theme's
   list, `bmds.icons` and `iconid.h`; the build's check of the names asked for (§11).
2. **The new layout on the board** (done, 2026-10-09): the table, `ota_0`, the ROM written by
   `idf.py flash` and mounted at `/rom`, the hash match, `rom` and `rom check` (§11).
3. **Icons in place:** `IconTheme::find(IconId)`, the new order, `bmds` the default theme, the
   OS's code moved to ids; drawing from flash measured; the user looks over the theme on the
   board.
4. **The time zone database** from `/rom` without a card.
5. **Then the media player** (`mediaplayer.md` step 3) uses the `media-*` names.
6. **Later, each with its own plan:** the recovery firmware and `reboot recovery`; updates;
   programs' read-only `mmap()` (§6), after the store-to-flash check.

## 11. As built

- **Step 1** (2026-10-09), host-tested, nothing on the board yet:
  - **`tools/mkrom.py`** (the standard library only; outputs rewritten only when they change):
    `image` (a tree from `--dir`, `--file`, `--link`; `--mappable` globs; `--hash-header` writes
    the content hash as a header for the firmware), `icons` (`bmds.icons` and `bmds.LICENSES`
    from `rom/icons.list`, the PNGs decoded by its own reader: 8-bit RGBA, every filter, as
    `tools/render-icons` writes them), `iconid` (`bmds/iconid.h`) and `check`.
  - **`bmds_core`:** `RomImage` (`BMRO`) and `IconPack` (`BMIC`), pure; `open()` checks
    everything once (linear, no allocation).
  - **`bmds_fs`:** `BlockDevice::mapped()` (empty by default); `MemoryBlockDevice` (read-only,
    over memory); `RomVolume` (a pool of 2; files are `RomFile` streams over the image, gone
    stale when it is unmounted); `Vfs::mount()` picks it for a device whose mapped bytes start
    with `BMRO`; `Volume::map()` and **`Vfs::map()`**. **`MaxMounts` is 8**; littlefs's and
    FAT's pools stay at 4 each (`LittleFsVolume::MaxVolumes`, `FF_VOLUMES`), so the internal RAM
    grows only by the mount table's four more entries.
  - **The `bmds` theme:** 154 icons at 16 and 32 pixels, 794,816 bytes (estimated 1 MB). Two first
    choices changed on sight: flat-color's `display` is a digital readout, so `computer`,
    `video-display` and `user-desktop` are glyphs' `monitor`; `dialog-question` is glyphs'
    `question-circle`.
  - **Host tests** (`tests/host/rom.cpp`, 6 cases; 455 in all pass): an image of
    `tests/host/rom` built by `mkrom.py` at the build (names in byte order, contents, the hash
    against the generated header, `noPath` cases, a mappable file on a page, the link); damage
    refused (the magic, the CRC, a later format, cut short, an entry past the end, children out
    of order, a count that orphans one, and 3,000 random damages to the tables read or refused
    without reading outside); mounted at `/rom` (reads, seeks, `stat`, the link followed and
    read, every change `ReadOnly`, a listing in order, `Vfs::map()` into the image itself and
    through the link, `statfs`, `listMounts`, a file gone stale on unmount, a damaged image not
    mounted); every icon of the real `bmds.icons` compared pixel for pixel with its PNG as
    `Png::decode()` reads it (mirrored where the list says), by id and name; every generic
    icon `FileFormat` names is in it; damaged packs refused. **`rom_icon_names`** (ctest) checks
    the `Icon=` of every `.desktop` file under `boot/`, `dist/` and `rom/`.
  - **The firmware:** builds warning-free, 2,912 bytes larger (the readers, the volume), with no
    ROM yet (step 2).
- **Step 2** (2026-10-09), on the board:
  - **The table** (`partitions.csv`, §3a with recovery in `ota_1`); `idf.py flash` writes
    `bootloader`, the table, `ota_data_initial.bin` (blank: `app_update`'s), `rom/rom.bin` at
    0x6A0000 and `bmds.bin` at 0x10000. The board booted `ota_0`; `/boot` was blank at its new
    place and was made again with the defaults (13.9 ms for the boot stage, formatting
    included). `fsbench` needed nothing: it takes a directory (the scratch region of
    `storage.md` §13 was the measurement's, not the command's).
  - **The build** (`bmds_storage`): `mkrom.py icons`, then `image` with `--max-bytes` the
    partition's size (the build fails if the ROM outgrows it), `rom/files/` added when there is
    such a tree; `romhash.h` (the content hash) compiled into `RomDisk`; the image is part of
    `all` and of `flash`.
  - **`RomDisk`** (a boot stage after `boot`, 0.9 ms): the partition mapped whole through the
    cache (`esp_partition_mmap`) as `RomPartition`, the block device `rom0`, mounted at `/rom` and
    pinned. Its log line: "/dev/rom0 mounted on /rom, 786 KB, 5 entries, content e0b571be...",
    or a warning for no partition, no image, or another build's content (mounted anyway).
  - **`rom`** (`rom info`): the path, format and entry count, the build time, the content hash,
    whether it matches the firmware, the size of the partition's, the firmware running
    (`ota_0 (main), at 0x10000`). **`rom check`**: 786 KB hashed in 243 ms, "content good".
  - **Tried on the board:** `ls -la`, `df -h` (`/dev/rom0 787K 787K 0 100% /rom`), `mount`
    (`type rom`); `touch`, `rm`, `mkdir` refused "Read-only file system"; `umount /rom` refused
    (pinned); `head` of `bmds.LICENSES`; `bmds.icons` fetched by YMODEM through the VFS
    (794,816 bytes in 4.2 s) and identical to the build's.
  - **Size:** the firmware 1,325,024 bytes (2,736 more than step 1: `RomDisk`, `rom`); the ROM
    805,376 bytes of its 9,600 KB.
