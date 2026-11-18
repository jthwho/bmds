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
  and `/boot`, `/sd0/p0` and `/ram0/p0` use three (`/tmp`, a tmpfs, replaced `/ram0/p0`: storage.md §15). `/bin` and `/share/zoneinfo` are virtual.
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
| `ota_1` | app, ota_1 | 0x410000 | 1.375 MB | the recovery firmware (§3b; 2 MB until 2026-10-14) |
| `otadata` | data, ota | 0x570000 | 8 KB | which firmware boots |
| (spare) | | 0x572000 | 56 KB | so `boot` starts on a 64 KB page |
| `boot` | data, littlefs | 0x580000 | 512 KB | `/boot`, read and write (§3h) |
| `rom` | data, 0x40 | 0x600000 | 10 MB | `/rom`, read-only (§3c) |

- **Since 2026-10-14** (`update.md` §19): `ota_1` cut to the recovery firmware's measured size
  and half again; `otadata` and `/boot` moved down with it, and the ROM got the rest.
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
- **The format, `BMRO`** (little-endian, our own: simple to map, nothing to unpack). **Format 1,
  below, was replaced by format 2 on 2026-11-18 (§12c, §12m):** a flat table in a log of tree
  versions, so files can be added and removed.
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
/rom/share/leap-seconds.list                  IANA's leap seconds, 5 KB (network.md §31e, 2026-10-13)
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

- **The bundle in `/rom`** (`/rom/share/zoneinfo.bin`, `Vfs::RomZoneDatabase`) as well as on the
  card: while no file is at the path `clock/tzdata` names (no card), the ROM's is used, read
  through its volume's reader (a copy of the few bytes asked for, no I/O). So time zones work
  without a card. An empty `clock/tzdata` is none at all, not the ROM's.

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
  source (`flat-color`, `glyphs`, or `bmds`: BMDS's own drawings, `rom/icons/svg`, rendered into
  `rom/icons/png` by `tools/render-icons rom/icons`) and the source icon:

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
- **A General MIDI synth from the ROM** (2026-10-22): no small SoundFont has clean rights, so
  the built-in default is FM with a 12 KB bank in `/rom` (`audio.md` §26).

## 7. Budget (estimates)

- **Measured 2026-10-22:** 5.94 MB used; about half of each program file is relocations and
  symbols the loader doesn't apply (1.69 MB of 3.47 MB). Ways to shrink them: `programs.md` §20.
- **angel moved to the card (2026-11-18,** `scripting.md` §2): the ROM went from 8,557,991 to
  5,660,903 bytes. Programs in `/rom` are copied into PSRAM to run, as on the card, so a program
  belongs here only if the system needs it without a card (the SSH tools, `tlsclient`).
- **The ROM's programs packed (2026-11-18,** `programs.md` §20): 5,660,903 to 4,858,919 bytes.
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
3. **Icons in place** (done, 2026-10-09): `IconTheme::find(IconId)`, the new order, `bmds` the
   default theme, the OS's code moved to ids; drawing from flash measured (§11). The user looks
   over the theme on the board.
4. **The time zone database** from `/rom` without a card (done, 2026-10-09, §11).
5. **Then the media player** (`mediaplayer.md` step 3) uses the `media-*` names.
6. **Later, each with its own plan:** the recovery firmware and `reboot recovery`; updates;
   programs' read-only `mmap()` (§6), after the store-to-flash check.
7. **A ROM that changes** (planned 2026-11-18): files added and removed, updates file by file
   (§12).

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
- **Step 3** (2026-10-09):
  - **`bmds/iconid.h`** is generated by `bmds_core`'s build (firmware and host) from
    `rom/icons.list`, a public header of every component above it.
  - **`IconTheme`:** `DefaultTheme` is `bmds`; the pack is mapped (`Vfs::map()`) and opened the
    first time it is needed. `find(IconId, side)` with `bmds` chosen is an index into a table of
    `Icon`s (PSRAM, 8 bytes an id and size, made the first time) pointing into the ROM. With
    another theme, the id's name is looked for in it (and what it inherits) and kept pinned (not
    dropped until `setTheme()`), else the ROM's. `find(name, side)`: kept, `/boot/icons`, the
    chosen theme on the card, the ROM's (kept as a record pointing into the ROM, so the next
    lookup is the list's), `hicolor`. `isPermanent()` tells an icon in the ROM.
  - **`FileIcons`** uses the ROM's icons where they are (no PSRAM copy), and has `named(IconId)`.
  - **Moved to ids:** the folder windows' tool bar and tree, the panel's volume icon (its
    `speaker` fallback gone), the desktop's New menu and `unknown`, the file dialog's places
    (`/rom` shown as `media-flash`, `/` as `computer`), `MessageBox`, `FileIcons`' `unknown`,
    `forProgram()`'s `application-x-executable`, `/bin/widgets`' tree. Names stay where they come
    from text (`Icon=`, file types, `panel/launcher/icon`).
  - **`panel/icon-theme`'s default is `bmds`** (was `flat-color`).
  - **Drawing from flash, measured** (`win bench icons`: 154 icons of 32 × 32 blended into
    internal RAM as `Painter::image()` does, twice, the same within 0.3%):

    | | Cycles an icon | µs at 360 MHz |
    |---|---|---|
    | each once, from the ROM (flash) | 106,400 | 295 |
    | each once, from PSRAM | 48,500 | 134 |
    | one again and again (cached), from flash, PSRAM or internal RAM | 27,500 | 76 |

    The first drawing of a 32-pixel icon costs 0.16 ms more from flash than from PSRAM (a
    16-pixel one a quarter of that), once; after that the source doesn't matter. Decoding a PNG
    costs far more, so **icons are drawn where they lie** and nothing is copied. What does cost
    is the blend itself, 27 cycles a pixel (`blendOver()`'s divisions for partial alpha): a
    candidate for PIE's SIMD or premultiplied pixels later (`efficiency-every-trick`).
  - **On the board:** the folder window (tool bar, tree, files), the panel, the desktop and the
    file dialog drawn from the ROM; `panel/icon-theme` set to `flat-color` (whose card install
    lacks `audio-volume-high`: the ROM's shown) and to `glyphs` (the launcher's and terminal's
    icons switch to the card's glyphs), and back to `bmds`.
  - **The firmware:** 1,327,776 bytes (2,752 more: the pack's lookups, `win bench icons`),
    warning-free; the host tests pass.
- **Step 4** (2026-10-09):
  - **The build** puts `share/zoneinfo.bin` in the ROM, built by `tools/tzbundle` from
    `thirdparty/tzdata` with the host's `zic` (0.3 s, the same bytes each time); without `zic`
    the ROM has none and CMake says so. The ROM is 1,039,872 bytes now.
  - **The VFS:** the zone database's file is opened at the path set, and when that is
    `NotExist`, `NoMedia` or `NotDir`, at `Vfs::RomZoneDatabase`. `ZoneDatabaseInfo::fromRom`
    (appended) tells which; `clock tzdata` says "PATH: not there: the ROM's, /rom/share/
    zoneinfo.bin: tzdata 2026e, 597 zones" then.
  - **Host test** (456 cases pass): the test image with the real bundle mounted at `/rom`, a
    path with nothing there: the ROM's used (`fromRom`, 597 names, `Europe/Berlin`'s rules,
    `/share/zoneinfo/Europe/Berlin` a file); an empty path: none; the ROM's own path named:
    not `fromRom`.
  - **On the board:** `clock/tzdata` set to a missing file, then the card unmounted with the
    default path: `clock tzdata` names the ROM's, `TZ=Europe/Berlin date` gives CET and
    `TZ=America/New_York date` EST, `ls /share/zoneinfo/Asia` lists the zones; the card mounted
    again: its own database.
- **The user's look** (2026-10-09):
  - **Copy and paste** were glyphs' `copy` and `paste`, two orange-yellow shapes alike at 16
    pixels. Neither set has a clear copy: **`edit-copy` is BMDS's own**, the first of
    `rom/icons/svg` (flat-color's document twice, the copy in front, blue as the file icons);
    **`edit-paste` is glyphs' `note-clipboard`** (a white board, blue lines, an orange clip).
    `mkrom.py` takes a third source, `bmds` (`rom/icons/png`, its license the project's).
  - **Icons in the menus:** the folder windows' (File, Edit, Go, Help, the New and Arrange
    submenus, an item's and the background's context menus: Open, Open in New Window, New,
    Delete, Rename, Close, Cut, Copy, Paste, Select All, Refresh, Back, Forward, Up, Home,
    Desktop, About), the desktop's (Applications, New, Paste; an icon's Open, Cut, Copy, Rename,
    Delete), the terminal's (Copy, Paste, Select All) and `/bin/widgets`'. Items with a check
    mark keep it instead. A disabled item's icon is drawn dimmed. The folder windows' New
    submenu is filled at each showing, and the desktop's icon menu is made again after a new
    theme, so no menu holds an old theme's icons.
  - **`WindowManager::MenuItem` gained `icon`** (appended; a program's menu, `showMenu()`); the
    terminal finds its three on its own thread at start (the menu shows on the window manager's).
  - Tried on the board: an item's menu, the Edit menu (Paste dimmed), the desktop's menu and the
    terminal's menu, in screenshots.
  - **Window controls, BMDS's own** (the user's ask): `window-minimize`, `-maximize`, `-restore`,
    `-shade` (Roll Up), `-unshade` (Roll Down), appended, and `window-close` redrawn (a red
    square, a white cross; was glyphs' `x`): windows with a blue title bar and a light body, the
    arrows slate. The window manager maps the pack itself at `start()` (before its lock: the VFS
    has its own) and gives its window menu these (Always on Top keeps its check).
  - **The tray's speaker, BMDS's own** (the user: "a red speaker and doesn't look right"):
    `audio-volume-high`, `-medium`, `-low`, `-muted` redrawn: a slate speaker, blue waves, a red
    cross for muted. Drawn on the 16-pixel grid (a 48 viewBox: 3 units a pixel; edges on pixel
    boundaries, one-pixel strokes centred on pixels, 45° ends): arcs and three waves smeared at
    16 pixels, so high has two waves, medium one, low none. **`VolumeItem` shows the level's
    icon** (muted or at the floor, below −40 dB, below −15 dB, else high) instead of a red
    slash drawn over one speaker (its drawing code gone).
  - Tried on the board: the tray at −9, −30 and −50 dB and muted, and a folder window's window
    menu, in screenshots. The firmware is 1,328,784 bytes, the ROM 1,067,328 (159 icons).

**Benchmarks moved (2026-10-17):** `fsbench` became `fs.smallfiles` and `win bench icons` became
`gui.icons` (`devplan/bench.md` §11). The figures above were taken with the old commands.

## 12. A ROM that changes: files added and removed (plan, 2026-11-18)

The user's idea (2026-11-18): files added to and removed from `/rom` while BMDS runs (through a
command of its own, not `cp`), the ROM keeping what makes it useful: every file's bytes at one
address, mappable. For what: programs and data that need no card, then installable kernel
extensions (their own plan, later). Updates become file by file, and **the update always wins**:
a system file the user changed or replaced is written back, so an update leaves a known good
system. The user's decisions through the discussion (2026-11-18): changes take effect at the next
boot; a flat table of full paths; each file's own alignment, up to the MMU's 64 KB page; files
of an alignment kept together; files moved while running (§12d); recovery applies updates, no
space reserved for the user's files, `rom install` only (§12i); `rom` shows everything (§12h).

### 12a. What stands in the way today (read 2026-11-18)

- **The kernel's uses already find files by name.** Nothing holds an offset into the ROM: the
  icon pack (`IconTheme`, the window manager), fonts (`SystemFonts`), voices, the gamepad
  database and the synth's bank call `Vfs::map(path)` when first needed; the time zones and leap
  seconds read through the volume's reader. **But they keep the span for the boot:** an erase
  under one is a crash or wrong data. Hence §12b.
- **The tie to the firmware is the whole image's hash** (`RomDisk`, `romhash.h`): one yes or no.
  It becomes per file (§12f).
- **`RomImage` is a tree** checked whole at mount (`open()`): adding a file means rewriting the
  tree. Hence a flat table (§12c).
- **Files have one time (the build's).** Each entry gets its own. (An id per file isn't needed,
  corrected while building step 1: the VFS gives every file, the ROM's too, an id from its path
  (`vfs.cpp`), so the image cache (`ImageCache::Key`) keeps the ROM's programs already; and a
  file can't change within a boot (§12b), while the cache starts empty at each.)
- **Writing flash stalls everything** reading it (auto-suspend off, `update.md` §16): the ROM's
  erase took 17 s of an update. Writes from the main firmware happen when nothing is playing, as
  `/flash`'s are held (`cfg.md`), with progress shown for a large file.
- **The main firmware can't write the ROM it has mounted** (§3b) holds for the tree booted; it
  can write the space no tree uses (§12b).

### 12b. Changes take effect at the next boot

- **The tree a boot starts with is the only one it sees.** `RomVolume` stays a read-only view of
  one fixed table, so every span `Vfs::map()` gave stays good until the reboot: no reference
  counts, no stale handles, no generations. The kernel's lifetime of a mapped file is the boot.
- **A change writes a pending tree.** The next boot takes the newest good tree. Each install,
  removal or move builds on the pending tree if there is one.
- **The space rule, all the allocator needs:**
  - **Usable now:** space neither the booted tree nor the pending one uses.
  - **Usable after the next boot:** space only the booted tree uses (files replaced, removed or
    moved).
  - **Files written since boot** are in no booted tree, so nothing maps them: they can be moved,
    rewritten or erased at once (a file replaced twice frees its first copy at once).
- **`rom` says what is pending:** the files to be added, replaced and removed at the next boot.
  `rom discard` drops the pending tree.
- **The cost:** a program installed in the ROM runs after a reboot. Kernel extensions need one
  anyway; a program wanted at once goes on the card.

### 12c. The format: BMRO format 2

Nothing is released, so format 1's reader goes (no compatibility shim): the first format 2 image
comes by a flash over the cable or an update.

- **The partition:** a header (magic, format, the partition's layout), the directory area (§12e)
  at its start, the files' space after it. Every place is an offset in the partition, which
  starts on a 64 KB page (§3a), so an offset aligned to 64 KB is a page of flash.
- **A tree version** (one table): a header (its sequence number, the firmware build it needs
  (§12f), the time it was written, its size, a CRC-32 of all of it), then the entries, then the
  paths.
- **Entries, sorted by path** (bytes, as `memcmp`; strictly increasing): the path's offset and
  length (the whole path from the root, at most 255 bytes, no `.` or `..` parts, no empty
  parts), the file's offset and size in the partition, its alignment (a power of two, as its
  log2: 6 is 64 bytes, 12 a page, 16 the MMU's page), flags (System: a link, as today; User: the
  user's, else the system's), its time, its BLAKE2s-256. The byte layout: `tools/mkrom.py`'s
  header comment.
- **Directories are implied:** one exists while a path is under it. No empty directories (an
  install makes the directories its path needs, a removal of the last file takes them); a
  directory's time is the newest under it. A path that is a file and a directory both (`a` and
  `a/b`) is refused at mount.
- **Finding a path** is one binary search. **Listing a directory:** every path under `d/` is one
  run of the sorted table (any common prefix is): found by a binary search, walked, a part with
  no `/` after it a file, else its first part a directory, named once and its run skipped by
  another search. Listings aren't rare (`PATH` has `/rom/bin`, completion lists it, fonts,
  voices, benchmarks and patterns scan folders), and this keeps them cheap.
- **Checked at mount:** the CRC, the order, every entry's path and bytes inside the partition,
  no two files overlapping, no file in the directory area, the file-or-directory rule. Nothing
  after it reads outside. The hashes aren't checked at boot; `rom check` does (§12f).
- **`RomImage` stays pure** (`bmds_core`, host-tested, fuzzed): the reader, and a writer of tree
  versions that recovery and the main firmware both use; `mkrom.py` writes format 2.

### 12d. Space: an allocator for flash

It is `malloc` with three differences: freed space comes back at the next boot (§12b); flash is
written only where erased, 4 KB at a time; and blocks **can** move, since the tree is the only
pointer to them, so fragmentation is temporary.

- **Two areas growing toward each other:**
  - **Pages, from the end of the partition down:** files aligned to 64 KB, each taking whole
    pages, its last page padded and shared with nothing (an MMU mapping shows whole pages, so
    it mustn't show a neighbour's bytes).
  - **Sectors, from after the directory area up:** files aligned to 4 KB or less.
  - **Free space** is the gap between them and the holes that the space rule (§12b) frees.
- **Erasing decides sharing:** a sector holding files of two installs can't be erased when one
  goes. So **files written together share sectors** (packed at their alignment: the firmware's
  image, an update's set, the files of one `rom install`), and each batch starts on a fresh
  sector; a file installed alone takes whole sectors (at most 4 KB lost until compacted).
- **Grouped by lifetime, too:** what is written together tends to go together. An update's
  system files are one batch and the user's installs others, so the user's changes don't leave
  holes among the system's, nor an update among the user's.
- **Placing:** first fit in the holes usable now, else the gap's edge for its area; refused if
  neither fits (§12g says what then).
- **Compaction runs while the system runs** (`rom compact`): plan the packed layout (the system's
  files first, then the user's), copy the files that move into space usable now, write the
  pending tree pointing at the copies; the old places free at the next boot, so the boot only
  takes the tree (no copying, no longer boot). It needs free space as large as what moves (the
  ROM is 4.86 MB of 10). A power cut leaves the old tree or the new, as any change. Recovery may
  compact as part of an update, in the same way.
- **Who chooses an alignment:** `rom install --align SIZE`; a program may ask in its own note
  (an appended flag, when there is a user: extensions); `mkrom.py`'s `--mappable` becomes
  `--align SIZE=GLOB`. The default is 64 bytes (a cache line).
- **Wear** doesn't matter at the rate files are installed; the directory area rotates (§12e).

### 12e. The directory area: a log of tree versions

- **A few erase sectors at the partition's start** (the size from the largest table expected: a
  few hundred files are a few KB of paths and entries: 124 KB, after the header's own sector, so
  the files start at 128 KB; the firmware's 78 files take a 7 KB version), written as a log: a new
  version goes after the last, and a sector is erased for reuse only when it holds neither the
  booted version nor the newest.
- **Two pending changes in a row** each write a version, so a power cut while writing the second
  leaves the first (two slots would lose both).
- **At boot:** the newest version whose CRC holds and whose firmware requirement this firmware
  meets (none named, or this one); failing that the newest good one of all, with a warning
  (changed while building step 2 from "the one booted last", which would have needed a record
  written at every boot); failing that no `/rom` and a warning, as a blank partition today.
- **Writing a version** is the commit: the files it points at are written and read back first.

### 12f. Hashes, the firmware and `rom check`

- **The firmware embeds a manifest** (path, size, hash for every system file) in place of one
  content hash, so `rom` lists which system files differ (changed, replaced, missing) rather than
  saying yes or no; `rom check` hashes every file against its entry (and the system's against the
  manifest).
- **A tree version may name the firmware it needs** (its build id). An update writes the ROM's
  part tied to the new firmware, so if `ota_0` didn't make it, the old firmware boots with the
  old tree. Today a cut between the two writes leaves them mismatched.
- **The ROM's files are untrusted input now:** every reader the kernel has (the icon pack, fonts,
  voices, gamepads, the synth's bank, the zones) must refuse damaged files without crashing; to
  check one by one (most check already). A bad system file that crashes the boot sends the board
  to recovery (the crash counter), which can restore the system files (§12g).

### 12g. Updates, file by file

The package's ROM section (`update.md` §2, type 2) becomes a manifest and the files (its format
to design with the step; the section type is new, appended, since `BMRO` format 1 isn't what it
holds).

- **Applying it:**
  - A file whose hash matches already is skipped: most updates write little.
  - A system file differing, missing, or changed or replaced by the user is written from the
    package and marked the system's: **the update always wins.**
  - A system file the new manifest doesn't list is removed.
  - **The user's files are kept,** unless one sits at a path the system now claims: the system's
    file wins and the update names the user's it displaced.
  - All of it is one tree version, so the ROM is the old set or the new one, never half.
- **Who applies it:** recovery, as now (the user's, §12i Q1): with nothing mapped it may also
  write over the old system's space and compact. It checks first that the new system's files fit
  beside the user's (none reserved, Q2): if not, nothing is written and it says how much is
  missing.
- **Restoring the system files** is applying the last package's ROM part again (`update apply`
  of the package kept on the card), from the main firmware or from recovery.
- **A full flash over the cable** (`idf.py flash`, the web installer) writes a fresh partition:
  the user's files go, as today.

### 12h. The `rom` command

The user's (2026-11-18): **"full visibility into all the rom metadata and details."** Every field
the format holds is shown by some form, nothing summarized away; each form's output is lines of
`Name: value` (as `rom info` today) or a table with a header, the same order every time, so it is
both read and parsed (`--raw`: exact bytes and offsets rather than KB and dates). Appended to §5's
forms:

- **Changing it** (each a pending change, §12b; only an update writes the system's files):
  - `rom install [--align SIZE] SRC... DEST`: one batch (a `DEST` ending in `/` a directory).
    Refused as not fitting with how much is needed, and whether compacting and rebooting would
    make room.
  - `rom remove PATH...`, `rom compact`, `rom discard`.
- **The whole of it:**
  - `rom` (`rom info`): the partition (label, offset in flash, size); the format; the booted
    tree version (sequence, time, firmware build it needs, entry and file counts) and the pending
    one if any; space used, usable now, usable after the next boot, in holes, in padding; the
    largest file that fits now (a sector-area file and a page-area one); the manifest's match
    (system files good, differing, missing); the firmware running (main or recovery).
- **The files:**
  - `rom ls [-l] [-R] [--pending] [PATH]`: the booted tree (or the pending one); `-l` adds every
    entry field: the owner (system, user), flags, size, alignment, offset in the partition and in
    flash, id, time, and the hash's first 16 hex digits.
  - `rom stat PATH...`: one entry, every field in full (the whole hash, the space it takes with
    its padding, the sectors or pages it covers, the mapped address `Vfs::map()` gives, the
    manifest's hash for a system path and whether it matches, and its state: as booted, replaced
    or removed in the pending tree, or new).
- **The space:**
  - `rom map [--sectors]`: the partition in order from start to end: the header, the directory
    area, each file (path, owner, offset, size, padding), each hole (and whether it is usable now
    or after the next boot), the gap between the two areas; `--sectors` one line a 4 KB sector
    (free, erased, a file's or several files', booted, pending).
- **The directory area:**
  - `rom log`: each tree version in the log, oldest first: where it sits, sequence, time, size,
    entry count, firmware build needed, its CRC good or bad, and which is booted, pending, or
    superseded; the sectors erased and free for the next.
- **Differences:**
  - `rom diff [--manifest | --pending]`: the system's files against the firmware's manifest
    (changed, replaced by the user, missing, extra), or the booted tree against the pending one
    (added, replaced, removed, moved): what an update or the next boot would change.
  - `rom manifest`: the firmware's manifest itself (path, size, hash).
- **Checking:**
  - `rom check [PATH...]`: every file's (or these files') hash against its entry, and the
    system's against the manifest; each bad file named, the time taken.

### 12i. Decided (the user, 2026-11-18)

- **Q1. Recovery applies an update's ROM part** (as recommended): nothing is mapped there, so it
  may write over the old system's space and compact as it goes, however full the ROM is. The
  main firmware doesn't stage it (§12g).
- **Q2. No space reserved for the user's files:** an update may use the whole ROM. The user's
  files stay, and an update that can't fit beside them is refused before anything is written,
  saying how much is missing (the user removes files and tries again).
- **Q3. `rom install` only:** `/rom` stays read-only to the VFS; no `cp` into it.

### 12j. ABI impact

- **Kept once shipped:** `BMRO` format 2 (the partition, the tree version, the entry: version
  fields to grow), the `rom` command's new forms, the update package's new ROM section type.
  Format 1 and the old section type were never released (`update.md` §2).
- **Not frozen:** the allocator's choices (where files go), `RomImage`'s and `RomVolume`'s
  interfaces (kernel classes).
- **None in the SDK** until programs' read-only `mmap()` (§6) or extensions use it.

### 12k. Budget (estimates)

- **Flash:** the header's sector and the log, 128 KB of the 10 MB; alignment's padding at most
  64 KB a file aligned to the MMU's page, 4 KB a lone install until compacted.
- **RAM:** the booted table is read in place (mapped); a change builds the new table in PSRAM
  (a few KB) and frees it.
- **Boot:** the log's versions' headers and the chosen table's check: a millisecond or two.
- **Firmware:** the writer, the allocator, `rom`'s new forms; the manifest (a few KB for a few
  hundred files) in place of a 32-byte hash.

### 12l. Steps

1. **Format 2, host-tested** (done, 2026-11-18, §12m): `RomImage` (reader, writer, checks),
   `mkrom.py`, `RomVolume` on the flat table (lookups, listings, links), times per file; a
   libFuzzer target.
2. **The firmware on format 2** (done, 2026-11-18, §12n): the directory log at boot, the
   manifest, and `rom`'s viewing forms (`info`, `ls`, `stat`, `map`, `log`, `diff`, `manifest`,
   `check`; §12h); the readers checked as untrusted input (§12f). Flashed by an update package.
3. **Changes while running:** the allocator, `rom install`, `remove`, `discard`, pending trees,
   tested on the board across reboots and with resets during writes (as `/flash`'s were).
4. **Compaction** (`rom compact`), with resets during it.
5. **Updates file by file:** the package's new section, `mkupdate`, recovery applying it,
   restoring the system's files; a user's file replaced and restored by an update on the board.
6. **Then, each with its own plan:** programs' read-only `mmap()` (§6); files mapped through the
   MMU at 64 KB; kernel extensions.

### 12m. As built: step 1, format 2 (2026-11-18)

- **`RomImage`** (`bmds_core`): `open()` checks the header (format 2, its CRC, the log in whole
  sectors before the files' space, reserved bytes 0), then takes the version with the largest
  sequence among those whose header is sane and CRC holds, checking it whole (every path valid
  and in strictly increasing order, flags, alignment, offset and size inside the partition past
  the log, times a `Timestamp` holds, no path both a file and a directory); a version whole but
  wrong falls back to the next newest. `find()` walks the path a part at a time (each part along
  the way a directory: `noPath` as before), then one binary search for a file, or the run of a
  directory (`Node`: `[first, end)` and its prefix); `nextChild()` lists a directory, skipping
  each subdirectory's run with a search for its path and `'0'` (the byte after `/`).
  `contentHash()` (each file's path, flags, size and hash: not places or times) is what the
  firmware compares until the manifest (step 2); `verifyFile()`, `verify()`.
- **The writer:** `writeHeader()`, `versionBytes()`, `writeVersion()` (sorted `NewEntry`s,
  checked as `open()` checks them but for their places). Tested to make `mkrom.py`'s header and
  version byte for byte, and a version it writes after mkrom's to be the tree.
- **`mkrom.py image`:** format 2; `--align SIZE=GLOB` (largest match wins; default 64 bytes) in
  place of `--mappable`; `--log-bytes` (124 KB). Files aligned to 64 KB are placed first, each
  on its own pages, then the rest by alignment. Unused bytes are 0xff. Every file has the
  image's time (the newest source, or `SOURCE_DATE_EPOCH`), as before.
- **`RomVolume`:** files have their entry's time, directories the newest time under them;
  `statfs` gives the log and the files' bytes. **`RomDisk`** compares the content hash and logs
  the file count; **`rom`** says format 2, the files, the tree version and when it was written,
  and `rom check` hashes each file.
- **Tests** (`tests/host/rom.cpp`, 10 cases): the fixture gained `docs.txt` (it sorts between
  `docs`'s run and `a.txt`: the listing skips runs correctly), `--align 4k=images/*` and
  `64k=a.txt`. Lookups, listings, alignments, the content hash; damage refused (the header,
  the log's bounds, a file cut short, each entry field with the CRC made right, a doubled `/`, a
  `..`, a file and a directory at one path); the newest good version chosen (by sequence, not
  place; a cut newer one or a whole but wrong one falls back; an empty tree); 3,000 random
  damages with the CRC made right; the writer.
- **`rom_fuzz`** (`tests/fuzz/rom.cpp`, seeds by `make-rom-seeds.sh`): CRCs made right before
  each run. It found within seconds that a time near `INT64_MAX` overflowed `Timestamp`'s
  nanoseconds (undefined behaviour); times outside a `Timestamp`'s range are now refused (a host
  test each for an entry's and a version's). Then 5.2 million runs in 5 minutes, nothing.
- **Size:** `romimage.cpp`'s code 1,812 → 4,939 bytes (the writer, unused by the firmware until
  step 3, is dropped by the linker). The firmware's ROM is 4,987,075 bytes (+128 KB: the header's
  sector and the log), 78 files, its tree version 7,087 bytes.
- **A format 1 ROM isn't read** by this firmware, so firmware and ROM go together (an update
  package or `idf.py flash` writes both).
- **On the board** (bmds0, 2026-11-18, `build update` and `tools/bmds-update`; recovery wrote the
  ROM): mounted 23 ms after reset ("4739 KB, 78 files"), `rom` says format 2, tree version 1,
  matching the firmware; `rom check` hashed the 78 files (4,739 KB) in 1,384 ms, all good. `find
  /rom` gave the image's 72 files, 6 links and 30 directories exactly; `sha256sum` of every file
  matched the image built on the host. Directories show the newest time under them; missing paths,
  a path through a file and a prefix of a name (`/rom/bi`) are "No such file"; `mkdir`, `touch`
  and `rm` are "Read-only file system". What maps or reads the ROM worked: the desktop (its
  pattern, the icon pack, the fonts), `say` with the ROM's voice, `font list` and `font draw` from
  `/rom/share/fonts`; and its programs: `dropbear` serving (`/rom/bin/dropbear`, a link), `scp`
  both ways (200 KB, identical), `sftp` listing `/rom/bin`, `dropbearkey`, `ssh -V`, `tlsclient`
  to example.com.

### 12n. As built: step 2, the firmware on format 2 (2026-11-18)

- **The tree at boot:** `RomImage::open(bytes, firmware)` prefers the versions for this firmware
  (none named, or its build id: the app's ELF SHA-256), else the newest good of all. `RomDisk`
  gives the VFS the build id (`Vfs::setRomFirmware()`) before mounting, so the volume and `rom`
  choose alike; a tree for another firmware used anyway is a warning.
- **The manifest** (`bmds/rommanifest.h`): `mkrom.py image --manifest-header` writes
  `romfiles.h` (`RomManifestFiles`: path, size, flags, hash; sorted) in place of the content hash
  (`RomImage::contentHash()` is gone). `RomManifest::compare()` walks the tree and the manifest
  together: changed, replaced (the user's file at a system path), missing, extra, and the user's
  own files counted. At boot: "the system's 78 files as built", or a warning with the counts.
  `RomManifest::compareTrees()` (added, removed, replaced, moved) is `rom diff --pending`'s.
- **The log read whole:** `RomImage::logRecord()` says what each sector of the log starts: a
  version (good, or whole but wrong), a damaged start, an erased sector, or anything else.
- **The space** (`bmds/romspace.h`, `RomSpace`): the regions in order (header, log, each file
  with the trees using it, the free between), each sector's state (used now, used after the next
  boot, how many files), and the totals: free now and after the next boot (whole sectors), the
  largest run and run of 64 KB pages, holes, padding, overlaps (a file over another of its own
  tree: two trees may share bytes under two names).
- **`rom`** (`romcommand.cpp`), each form `Name: value` lines or a table with a header, `--raw`
  for exact bytes and Unix times, options anywhere: `info` (the partition, the log, the tree and
  the pending one, the space, the manifest's counts, the build running), `ls [-lR] [--pending]
  [PATH]` (`-l`: owner, kind, size, alignment, offset in the partition and in flash, time, hash),
  `stat PATH...` (every field: the link's target, the sectors and how many are shared, the
  padding after it, the address it is mapped at, the whole hash, the manifest's verdict, its
  state in the pending tree), `map [--sectors]` (every region; or runs of sectors alike, free
  ones told erased or not), `log` (each version's place, sectors, sequence, time, files,
  firmware, and booted, pending, superseded, passed over or refused), `diff [--manifest |
  --pending]` (exit 1 if the system's files aren't the manifest's), `manifest`, `check [PATH...]`
  (a directory checks what is under it).
- **The readers as untrusted input** (§12f), checked one by one: fonts (`fontface_fuzz`) and the
  FM bank (`fmsynth_fuzz`) were fuzzed already; the leap seconds too (`leapseconds_fuzz`).
  **`romdata_fuzz`** (new) takes the icon pack (every name and pixel inside it), the zone bundle
  (every entry found again, its rules) and the gamepad database: 1.2 million runs in 5 minutes,
  nothing. `rom_fuzz` now also walks the log, lays the space out (covering the partition once)
  and compares the tree with itself: 5.7 million runs, then 2 million after the fix below,
  nothing. Programs are the loader's,
  checked as before. **Not safe: the speech voices.** SVOX Pico parses its resource files with
  little checking (`pico_loadResourceInPlace()`), and the kernel's speakers load them; a damaged
  or hostile voice file could crash the kernel. This isn't new (voices come from the card too,
  `speech/path`), and isn't fixed here: hardening Pico, or loading only voices whose hashes are
  known (the manifest's for the ROM's), is the user's to choose.
- **Found while building:** `RomSpace` merged a file's booted and pending copies by place and
  size alone, so two files at one place could split them; it merges by path too now (a host test
  of a rename sharing bytes).
- **Size:** the firmware 3,327,104 → 3,348,288 bytes (+21 KB): `rom`'s forms 12.4 KB of code and
  text, `RomDisk` 7.5 KB (the manifest 3.4 KB and its paths), `RomSpace` 3.2 KB, the comparisons
  1.2 KB.
- **On the board** (bmds0, by update package): "tree version 1, the system's 78 files as built"
  at boot (22 ms after reset); every form run: `info` (and `--raw`), `ls` (`-l`, `-R`, `-lR`),
  `stat` of a file, a link and a directory, `map` (157 regions), `map --sectors` (872 free sectors
  still hold the format 1 ROM's bytes, 470 are erased: what step 3's allocator must erase before
  writing), `log` (one version of 2 sectors, booted; 29 erased), `diff` (all as built, exit 0),
  `diff --pending` and `ls --pending` (none), `manifest`, `check` (78 files, 4,740 KB in 1,386 ms;
  a directory and a file: 12 files), and usage errors (exit 2). A pending tree and a tree for
  another firmware can't be made on the board until step 3; the host tests cover them.

