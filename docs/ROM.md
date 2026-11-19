# The ROM (`/rom`): files in flash, used where they lie

`/rom` is a tree of files kept in the board's flash, in its own 10 MB partition. It holds what
BMDS needs whether or not there is a card: the icon theme, the fonts, the time zone database,
the speech voices, the synth's bank, the desktop patterns, the gamepad database, the SSH tools.
The user can add files of their own too.

Its point, beyond being there without a card, is that **every file's bytes are already at an
address**. The partition is mapped through the flash cache once, at boot, and never unmapped, so
code can use a ROM file's bytes in place instead of reading them into RAM. A 900 KB icon theme
costs no PSRAM; a 1 MB speech voice costs none either.

This document is for developers: what the ROM is good for, how to keep data out of RAM with it
(in the kernel and in programs), how files get in, and what to watch out for. The design and its
history are in `devplan/rom.md` (§12 for the ROM as it is now).

## At a glance

| | |
|---|---|
| **Where** | The flash partition `rom` (10 MB, at 0x600000), mounted at `/rom` at boot, before anything reads it. |
| **What it costs at rest** | Nothing in RAM: the files are read through the flash cache. |
| **Read** | Like any file (`open`, `read`), or in place: `Vfs::map()` in the kernel, `mmap(PROT_READ)` in a program. |
| **Written** | Not through the VFS (`EROFS`). The firmware's files come with the firmware; the user's go in with `rom install` (a new tree each change, taking effect at the next boot). |
| **Lifetime of a mapped file** | The whole boot: the tree a boot starts with never changes under it. |
| **See what's there** | `rom` (and `rom ls -l`, `rom stat`, `rom map`, ...: below). |

## Keeping things out of RAM

### In the kernel: `Vfs::map()`

```cpp
auto [bytes, err] = Vfs::map("/rom/share/icons/bmds.icons", Vfs::Context());
if (err.isOk()) {
        // bytes.data() is in flash, mapped: read it as memory, for the rest of the boot.
}
```

- **What it returns:** a `ByteSpan` over the file's bytes in the partition's mapping. Nothing is
  copied and nothing needs freeing. The span stays good for the whole boot: the booted tree is
  never written, even when the user installs or removes files (those changes are a new tree the
  next boot takes).
- **What else it can return:** `NotSupported` for a file on a volume that can't map (the card,
  `/flash`, a network mount), or for a ROM file stored compressed (§ Compressed files below);
  `IsDir`, `NotExist` as `stat` would.
- **The pattern to follow:** map if you can, read into PSRAM if you can't, as the speech voices
  do (`components/bmds_speech/src/voices.cpp`, `readVoiceFile()`):

  ```cpp
  auto [mapped, merr] = Vfs::map(path, context);
  if (merr.isOk()) return mapped;               // in place: nothing to free
  if (merr != Error::NotSupported) return merr; // not there, a directory...
  // else: stat, allocate in PSRAM, read it whole, and free it when done
  ```

  So a file the user put on the card instead (a newer theme, another voice) still works, at the
  cost of a copy, and the ROM's own file costs nothing.
- **Who uses it today:** the icon theme (`IconTheme`, the window manager), the system fonts,
  the speech voices (handed to Pico in place), the gamepad database, the synth's FM bank. The
  time zone database and leap seconds are read through the ROM volume's reader (a copy of the
  few bytes asked for).

### In a program: `mmap()`

A program's read-only `mmap()` of a `/rom` file gives the file's bytes in flash, no copy, when
the file is **aligned to 4 KB** in the ROM:

```c
int fd = open("/rom/share/mydata.bin", O_RDONLY);
struct stat st;
fstat(fd, &st);
const unsigned char *p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
close(fd); // the mapping stays
/* ... read p ... */
munmap((void *)p, st.st_size);
```

- **Nothing changes in the program:** the C library tries `bmds_file_map` (slots 137-138) first
  and falls back to a copy, as `mmap` always did, when the file can't be mapped in place: not in
  `/rom`, not aligned to 4 KB, mapped with `PROT_WRITE`, or the program already holds four in-place
  mappings (each takes one of the program's memory-protection regions). Code that works with a
  copy works with the mapping, and the other way round.
- **It behaves as on Linux:** the address is a page's; the bytes after the file's end in its last
  page read as 0; `mprotect(PROT_WRITE)` on it is `EACCES`; **a store into it kills the program**
  (SIGSEGV, exit 139), as writing a read-only mapping does.
- **Making a file mappable:** give it a 4 KB alignment when it goes in (`rom install --align 4k`,
  or `mkrom.py`'s `--align 4k=GLOB` for the firmware's files). Such a file owns its last 4 KB
  page, with zeros after its end, which is what lets the kernel map it.
- **Check it on the board:** `progtest rommap PATH` maps a file eight times and says which
  mappings were in place, then checks the bytes, the tail and that `mprotect` is refused.

### What the ROM doesn't save: programs

**A program in `/rom/bin` is copied into PSRAM to run**, as one on the card is (the image cache,
`devplan/programs.md` §14): it isn't run in place. So the ROM saves no RAM for programs; it
saves card space, and it means the program is there without a card. Put a program in the ROM
for that reason (the SSH tools are there so a board without a card can still be reached), not to
save memory. The ROM's programs are also packed (their relocations replaced by a patch list,
`sdk/tools/bmds-pack`): they have no symbols, so debug with the build tree's copy (`bmds-fault`
and `bmds-prof` find it by its build id).

### What it costs to read from flash

Reading through the flash cache is a little slower than PSRAM on a cache miss, and both are much
slower than internal RAM. For data read once or rarely (a font's glyphs as they are first drawn,
a voice's tables) that doesn't matter. For something read in a hot loop, measure: copying a small
hot table into internal RAM can still be right. The icon theme was measured both ways
(`devplan/rom.md` §3f, §11) and is drawn straight from flash.

## Getting files in

### The firmware's own files

The firmware's ROM image is built by `tools/mkrom.py` at the firmware's build
(`components/bmds_storage/CMakeLists.txt`) and written with it (`idf.py flash`, or an update
package). Two ways to add a file:

- **Put it under `rom/files/`** in the repository: it goes into the ROM at the same path
  (`rom/files/share/foo.txt` is `/rom/share/foo.txt`).
- **Name it in `bmds_storage`'s ROM step**, as the fonts, speech, patterns and the packed SSH
  programs are (`--file DEST=SRC`, `--dir DEST=SRC`), when it is built or comes from elsewhere.

Every file the firmware ships is a **system file**, listed in the firmware's manifest
(`romfiles.h`: path, size, hash). At boot the ROM is compared with it ("the system's 78 files as
built"); `rom diff` names any that differ. Give a file an alignment with `mkrom.py image
--align SIZE=GLOB` (64 bytes unless told otherwise; 4k to make it mappable by programs; 64k for a
file that must own whole 64 KB pages).

Keep it lean: the ROM shares 10 MB of flash, and what is in it is copied to every board by every
update that changes it.

### The user's files

```
rom install [--align SIZE] SRC... DEST    add or replace (DEST a directory with several sources)
rom remove [-r] PATH...                   remove (a directory's files with -r)
rom discard                               drop the changes not yet booted
rom compact [--full]                      gather the free space
rom export [-f] DIR / rom import DIR      the user's files to a folder and back
```

- **Each change takes effect at the next boot.** It is written as a new tree version, and the
  next boot takes it; until then `/rom` shows the tree booted (`rom diff --pending` lists what will
  change, and the desktop shows a "ROM changes pending" notification). This is what lets kernel
  code keep its mapped spans for the whole boot.
- **Changes are safe against power cuts:** new bytes go only into space no current tree uses, are
  read back, and only then is the version that points at them written. A cut at any moment leaves
  the old tree or the new one, whole.
- **Writing flash stalls everything reading it**, so changes wait while audio is playing.
- **Space comes back after a reboot:** a removed or replaced file's place is free once the tree
  that held it is no longer the booted one. `rom compact` fills holes with files from the top
  (`--full` repacks everything after the first hole).
- **The user owns every file they install**, including one that replaces a system file. A user's
  file can be replaced by an update (below), never by anything else.

## Updates and the user's files

An update package's ROM is merged file by file, in recovery:

- System files that differ are written, those the new firmware no longer ships are removed, the
  rest stay where they are.
- **The update always wins**: a user's file at a path the update claims is replaced, so an update
  leaves a known good system. It is **backed up to the card first** (`update/rom-backup`, default
  `/sd0/p0/rom-backup/VERSION/PATH`), unless it is byte for byte the update's file; if the backup
  can't be made, the ROM is left alone. The update's result says what was backed up
  (`update status`).
- The user's other files are kept. Afterwards recovery compacts the ROM.
- **A full flash** (`idf.py flash`, the web installer's Fresh install or Reinstall) writes the ROM
  whole: the user's files are lost. Save them first with `rom export DIR`, or update on the board
  instead (`update fetch`, the Updater).

## Seeing what is there

`rom` shows every field the ROM holds; `--raw` gives exact bytes, offsets and Unix times.

| | |
|---|---|
| `rom` | The partition, the log of tree versions, the tree booted and any pending one, free space now and after the next boot, holes and padding, the manifest's verdict. |
| `rom ls [-lR] [--pending] [PATH]` | Files and directories; `-l` adds owner, kind, size, alignment, offset in the partition and in flash, time, hash. |
| `rom stat PATH...` | One file in full: a link's target, how it is stored, its sectors, the padding after it, the address it is mapped at, its hash, the manifest's verdict, what the pending tree does to it. |
| `rom map [--sectors]` | Where every byte of the partition goes, region by region or sector by sector. |
| `rom log` | Every tree version in the log, and which is booted, pending or superseded. |
| `rom diff [--manifest \| --pending]` | What differs from the firmware's manifest, or what the next boot changes. |
| `rom manifest`, `rom check [PATH...]` | The firmware's list of system files; each file's hash checked. |

## Things to keep in mind

- **Don't hard-code `/rom` paths.** Find files through a setting whose default names the ROM's
  copy and the card's (`font/paths` is `/rom/share/fonts;${HOME}/Fonts`, `speech/path` is
  `/rom/share/speech:/sd0/p0/share/speech`), so the user can add or override files without the
  ROM.
- **Treat ROM files as input that can be damaged or replaced.** The user can install a file over
  a system file, and flash can rot. Check formats as you parse them (the icon pack, the zone
  bundle and the gamepad database are fuzzed: `tests/fuzz/romdata.cpp`); a kernel crash at boot
  ends in recovery, which can restore the system's files.
- **Expect a file to be missing.** A board may have no ROM (a blank or damaged partition): fall
  back, as everything does today when there is none.
- **Compressed files are coming, not here.** Entries already carry a compression method and the
  size as read (`devplan/rom.md` §12r), but nothing writes compressed files yet. When it does, a
  compressed file won't map (`Vfs::map()` and `bmds_file_map` say `NotSupported`): keep anything
  meant to be used in place uncompressed.
- **The kernel's mapped spans are for the boot**: never cache a ROM pointer across something that
  remounts it (nothing does today; the ROM is pinned).

## Where to read more

- `devplan/rom.md`: the design (§12 for the ROM that changes: the format §12c, space §12d, the
  log §12e, hashes and the manifest §12f, updates §12g, the `rom` command §12h) and what was built
  and measured (§12m-§12r).
- `devplan/update.md` §21: updates file by file.
- `devplan/programs.md` §12, §14: programs' memory and the image cache.
- `devplan/extensions.md`: kernel extensions, which would be installed into the ROM (planned).
- `components/bmds_core/include/bmds/romimage.h`, `romeditor.h`, `romspace.h`,
  `rommanifest.h`: the format's reader and writer, the editor, the space, the manifest.
- `sdk/include/bmds/sys.h`: `bmds_file_map`, `bmds_file_unmap`.
