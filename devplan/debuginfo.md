# Debug information on the card

Status: **planned** (2026-11-15, the user's request: "we've got plenty of space on the SD card").

## 1. Goal

A crash can be read on the board: a program's fault and a kernel panic named down to function,
file and line, with no host that still has the matching build. Today that takes the host
(`tools/bmds-fault`, `tools/bmds-panic`, `tools/bmds-prof`) and the very build that ran: a board in
the field, or a build since replaced, gives addresses only. It is also a step towards self-hosting
(`VISION.md`): a debugger on the board needs the same files.

**Not goals:** a debugger (GDB on the board) now; unwinding by DWARF call frame information on the
board (the log's scan of the stack stays, `protection.md` §21); debug information in the update
package; anything in the kernel beyond logging which build ran.

## 2. What is there (measured 2026-11-15)

- **Programs** are built with `-g` (`BmdsProgram.cmake`, `bmds-cc`, `protection.md` §22-§23). The
  unstripped build (`<name>.debug.elf`, `busybox_unstripped`...) stays on the host; the card's copy
  is stripped (`objcopy --strip-unneeded`). Nothing ties the two: the host tools find a debug ELF by
  file name and trust it.
- **The firmware** records its ELF's SHA-256 (IDF's `app_elf_sha256`, `SystemInfo::imageSha256`);
  `bmds-panic` matches `build/bmds.elf` by the version string in the boot banner.
- **Faults** log the program's path, its registers and a trace of image offsets (`faults.cpp`);
  **panics** reach the log after the reset (`logging.md` §9): IDF's register dump and stack memory.
- **Sizes** (`objcopy --only-keep-debug`; zlib with `--compress-debug-sections`):

  | ELF | whole | debug only | zlib |
  |---|---:|---:|---:|
  | the firmware (`bmds.elf`) | 62.1 MB | 56.6 MB | 24.2 MB |
  | angel | 37.7 MB | 37.5 MB | 31.7 MB |
  | BusyBox | 13.3 MB | 13.2 MB | 11.3 MB |
  | dropbear | 11.0 MB | 10.9 MB | 9.0 MB |
  | progtest | 6.6 MB | 6.5 MB | 5.5 MB |

  Everything the project builds: a few hundred MB, small beside a card's 32 GB. zlib saves little
  but on the firmware (the programs' DWARF may be compressed already: to check, §8 step 2).

## 3. Pairing: a build-id in every binary

The one thing that must not go wrong: debug information from another build names wrong functions,
confidently, which is worse than none.

- **Programs:** `-Wl,--build-id=sha1` in both links (`BmdsProgram.cmake`, `bmds-cc`): a GNU note,
  20 bytes, the linker's hash of the output. `bmds.ld` puts `.note.gnu.build-id` in the note
  segment after the BMDS note (`KEEP`), so the card's copy carries it (36 bytes in the image). The
  loader already skips notes it doesn't know (`programimage.cpp`); `MaxNoteBytes` (512) holds both.
  An executable-format change by addition only (a standard note old loaders skip): `ABI:` line.
- **The firmware:** its `imageSha256` (no change to IDF's link): the boot banner logs its first 8
  bytes, so a panic recovered into the log names the build that panicked.
- **Faults log the build:** the loader keeps the image's build-id (a pointer into the image, the
  note found while it parses them), and the fault report's first line gains it
  (`progtest.elf[268] build 3f2a...: Segmentation fault at +0x13ecc ...`). The file on the card may
  have been replaced since; the build-id says which one ran.
- **The host tools check it** (`bmds-fault`, `bmds-prof`; `bmds-panic` by the firmware's SHA):
  a debug ELF whose build-id differs from the log's is refused with a warning, not used.

### 3a. How an id is made, and reproducible ids (the user's, 2026-11-15)

**The linker makes it** (`--build-id=sha1`): the SHA-1 of the finished output file, the note's own
bytes counted as zeros. Same inputs, same id; any change to any section, the debug sections
included, a new one. That is what pairing needs: two builds with the same code but different line
tables are different builds to a debug file. Whatever a build embeds changes it too (BusyBox's
build date): harmless. `md5` (weaker), `uuid` (random: never reproducible) and an explicit `0x...`
are the linker's other choices; `sha1` is GNU's default and what GDB and debuginfod expect. It is
an identity, not a security check. The firmware's identity is IDF's `app_elf_sha256` instead (the
whole ELF's SHA-256, §3).

**Source paths go in by fixed names.** DWARF records each source file's absolute path
(`/home/jth/src/esp-pc/sdk/tests/progtest.c`), and those bytes feed the hash. So the same source
built in another folder or on another machine gets another id, and every published debug package
would carry the builder's home folder. Built with `-ffile-prefix-map=OLD=NEW`, which rewrites the
paths in the debug information and in `__FILE__` alike, files are named as if the project were `/`:

- **The project:** its root as `/` (`/minesweeper/main.c`); this repository's, for the
  distribution, likewise (`/thirdparty/monocypher/...`);
- **The SDK** as `/sdk` (in this repository, its real place), **the build folder** as `/build`
  (musl's generated headers: `/build/bmds-libc/include`), **the compiler's headers** as
  `/riscv32-esp-elf`;
- **The distribution's sources** (unpacked under `build-dist/src/`, `build/rom/dist/src/`) as
  `/dist/src/NAME-VERSION`, its build folders as `/dist/build/NAME`, the same whichever build
  unpacked them;
- **The firmware:** IDF's `CONFIG_APP_REPRODUCIBLE_BUILD` does the same for its ELF (paths become
  `/IDF`, `/COMPONENT_...`); on since 2026-11-15, with the ROM's time fixed (§13a-b);
- **The toolchain's own libraries** (libgcc: `/builds/idf/crosstool-NG/...`) are as Espressif built
  them: left alone.

*Changed 2026-11-15:* the names were to be relative (`sdk/...`, `./...`). Tried, `addr2line`
printed `build/sdk/tests/progtest.c`: DWARF takes a relative name as relative to the compile
directory, itself mapped (`/build`), and tools join the two. Absolute names aren't joined; IDF's
reproducible build uses them for the same reason.

Then a build is the same everywhere: same sources, same toolchain and flags, same ids, so a debug
package built by anyone serves everyone's identical build. The host tools put the root back to show
a file (`bmds-fault`, `bmds-prof` and `bmds-panic` take `--source-root`, default the repository;
GDB: `set substitute-path`), and `debug` on the board shows the relative path as it is. Checked by
building the SDK examples in two different folders: the ids must match.

## 4. The files on the card

GDB's layout, so GDB finds them too when it comes (`set debug-file-directory`):

- `DIR/.build-id/ab/cdef...debug` for a program: the build-id's first byte, then the rest, in hex;
  the file is `objcopy --only-keep-debug` of the unstripped build (symbols and DWARF, no code).
- `DIR/firmware/0123456789abcdef.debug` for the firmware: its SHA-256's first 8 bytes. The ROM's
  programs are programs (`.build-id/`).
- **The setting** (paths are settings): `debug/paths`, `Config::Type::Paths`, default
  `/sd0/p0/lib/debug`; searched in order, so a user's own folder can come first.
- Not compressed (§7); a compressed file is read the same way once §10 is built.

## 5. Packages

Debug files are ordinary packages (`packages.md`), never in the update package: an update stays
12.8 MB, and nobody downloads hundreds of MB they don't want.

- **The distribution's:** `bmds-dist package` makes `NAME-dbg_VERSION-REVISION.bmpk` beside each
  package with programs: its files under `lib/debug/.build-id/`, `depends NAME (= VERSION)`,
  category `Development`. Not in `bmds-standard`. `pkg install busybox-dbg`, as Debian's
  `-dbgsym`.
- **The firmware's:** `build update` also makes `bmds-dbg_VERSION.bmpk` from `build/bmds.elf` and
  the ROM's programs, published beside each release. Its version is the firmware's; `pkg` doesn't
  know the firmware's version as a dependency, so a mismatch is caught by the SHA, not refused at
  install (the decoder says "no debug information for this build").
- **A program built by hand** (the SDK examples, the user's own): the board command adds one
  (`debug add FILE.debug.elf`: reads its build-id, writes the debug-only part where §4 says);
  `tools/bmds-run` and `bmds-dist install` may send it along when asked (`--debug`).

## 6. Reading them on the board: `debug`

A program (SDK, C, in its own package `debug-tools`), not a kernel command: the kernel keeps
logging addresses and build-ids, costs nothing when nothing faults, and the log stays readable
without any debug files.

- `debug fault [--all]`: the last fault (or all) in the log (`log show`'s lines, or a file): each
  trace entry named, as `tools/bmds-fault` prints it.
- `debug panic`: the panic recovered into the log: MEPC and RA named, then what looks like a
  return address in its stack memory (the same scan as faults'); IDF's GDB-unwound backtrace stays
  the host's (`bmds-panic`).
- `debug name PROGRAM ADDRESS...`: addr2line's job, for anything else (`prof dump`'s samples: §8
  step 5).
- `debug add FILE`, `debug list` (what is there, by build), `debug check` (files no installed
  program or firmware matches: leftovers).
- **How it reads DWARF:** symbols from `.symtab` (function names; sorted once into a table of
  addresses), files and lines from `.debug_line` (the line program, DWARF 5, streamed: no tree
  in memory), inlined callers from `.debug_info` left for later (addr2line's `-i`: a parser of
  DIEs is the large part). Memory: the symbol table of one file at a time, in PSRAM; nothing held
  between runs.

## 7. Decisions (the user's, 2026-11-15)

1. **Not compressed** at first. Compressed files are planned for later, more than one kind (§10).
2. **The firmware's debug package is published with each release**, beside the update package.
3. **Inlined callers on the board: later** (§6); files and lines first.
4. **The command is `debug`**, and it is planned as the home of other kinds of debugging, not only
   naming addresses (§11): its structure leaves room for them from the start.

## 8. Steps

1. **Build-ids** (§3): the links, `bmds.ld`'s note, the loader keeping it, the fault line and the
   banner, the host tools' check. Check: every program still loads and the card copies differ by
   the note alone; a mismatched ELF refused by `bmds-fault`.
2. **Debug files** (§4): first reproducible ids (*done 2026-11-15, §13*; §3a: `-ffile-prefix-map`
   in `BmdsProgram.cmake` and `bmds-cc`, the distribution's sources mapped by `bmds-dist`, the
   firmware's by IDF's option, the host tools' `--source-root`; checked by two builds in different
   folders giving the same ids),
   then `objcopy --only-keep-debug` per program in the SDK build and `bmds-dist`; the `debug/paths`
   setting. Measure whether the toolchain already compresses the programs' DWARF (§2's zlib column
   suggests it), for §10.
3. **Packages** (§5): `NAME-dbg` from `bmds-dist package`; `bmds-dbg` from `build update`;
   published by `tools/release`.
4. **`debug`** (§6, built as §11's frame): symbols, then `.debug_line`; `debug fault` against
   `tools/bmds-fault`'s output for the same log (they must agree, a host test on the same files).
5. **Panics and the profiler:** `debug panic`; `debug name` taking `prof dump`'s output.

## 9. Budget

- The kernel: a pointer per running program (the build-id in its image), a few dozen bytes of
  code, a longer log line per fault and in the banner. Nothing at run time.
- The card: what is installed, by choice (§2's sizes).
- `debug`: a program on the card, its memory while it runs (one symbol table, a line program
  streamed).

## 10. Compressed debug files (later)

Not now (§7), but nothing above may assume a file is plain. The ELF way is per section: a section
with `SHF_COMPRESSED` starts with an `Elf32_Chdr` (`ch_type`, `ch_size`, `ch_addralign`), the
compressed bytes after it. `ch_type` names the kind: `ELFCOMPRESS_ZLIB` (1) and `ELFCOMPRESS_ZSTD`
(2) are the standard ones, and binutils makes either (`objcopy --compress-debug-sections=zlib` or
`=zstd`).

- **`debug`'s reader opens a section through one call** that gives a byte stream whatever its
  storage: plain, or a decompressor chosen by `ch_type`. A file whose section uses a kind not built
  in says so ("compressed with zstd, which this `debug` doesn't read") and is skipped, not misread.
- **Kinds:** zlib first (an inflater is small; the kernel's PNG decoding may already hold one to
  share: to check), zstd after (better ratio and faster to read; its decoder is larger). Either is
  a decompressor behind the same interface; more kinds append.
- **Streaming:** the line program and the symbol table are read front to back, so a decompressor
  never needs the whole section in memory, only its window (zlib's 32 KB; zstd's frame window,
  bounded when the files are made: `objcopy` writes them, the build picks the level).
- **Where it pays:** the firmware's file (56.6 → 24.2 MB with zlib). The programs' files barely
  shrink (§2): if the toolchain already compresses their DWARF, `debug` needs this to read them at
  all, which makes zlib the first kind whatever is decided about the card.
- **Packages** carry the files as built; `pkg`'s own container compression is a separate question
  (`packages.md`).

## 11. `debug` as the home of debugging (planned frame)

The user's direction (2026-11-15): `debug` will do other kinds of debugging, not only §6's. So it
is a command with subcommands from the start, each in its own source file over shared parts:

- **The shared parts:** finding a build's debug file (§3-§4), symbols and lines (§6), reading
  compressed sections (§10), reading the log, naming an address in a running process (its image's
  base from `proc_info`, its build-id from §3).
- **First (this plan):** `fault`, `panic`, `name`, `add`, `list`, `check`.
- **Later, each a plan of its own when wanted** (none is promised here; the frame must not rule
  any out):
  - **A live program:** its threads, each one's state and where it stands (a name for its PC);
    needs a slot to read another process's registers and memory, with the permission rules that
    implies (`protection.md`: today only the kernel reads a program's memory).
  - **Core dumps:** a faulting program's registers and memory written to the card (a setting
    turns it on, its folder a setting), read by `debug core` and by GDB on the host.
  - **A GDB remote stub:** `debug serve PID` speaking GDB's protocol over a socket, so the host's
    GDB debugs a program on the board (breakpoints by `ebreak` in its image, single steps by the
    trigger unit); the hardware parts are `protection.md` §11's open question.
  - **Watching:** the kernel's counters (`stats`), the profiler (`prof dump` named on the board),
    system calls a program makes (a trace, like `strace`).
- **The interface:** `debug SUBCOMMAND ...`, `debug help` listing them; machine-readable output
  (`--json`, as `pkg` has) for the GUI tools to build on. A shell interface: frozen once shipped.

## 12. As built: step 1, build-ids (2026-11-15)

- **Links:** `-Wl,--build-id=sha1` in `BmdsProgram.cmake` and `bmds-cc`; `bmds.ld` keeps
  `.note.gnu.build-id` right after the BMDS note, so the PT_NOTE segment holds both (0x48 bytes)
  and the card's copy carries it (`objcopy --strip-unneeded` keeps an allocated note). The card
  copy and the debug ELF have the same id (progtest `180b364f...`). Documented in `bmds/program.h`
  as an optional note (appended; loaders skip it).
- **The loader** reads it with the notes (`ProgramImage::Info::buildIdOffset`/`buildIdBytes`: name
  "GNU", type 3, at most 32 bytes) and keeps where it is in the image (`Running`, 8 bytes).
- **The log:** a fault's report has `  PID: build <id>` after its first line; the boot banner ends
  `elf <its SHA-256's first 8 bytes>` (checked against `sha256sum build/bmds.elf`).
- **The profiler** keeps each module's id (20 bytes, copied at its first sample) and `prof dump`'s
  module lines end with it (a seventh field, appended).
- **The host tools refuse another build's ELF:** `bmds-fault` and `bmds-prof` by the build-id
  (`tools/bmdself.py`: an ELF's build-id, and the program lookup the two shared, now one, which
  also takes a `.debug.elf` for a `name.elf`: `bmds-prof` named none of progtest's samples
  before); `bmds-panic` by the banner's SHA when it has one, else the version string as before.
- **Checked:** the suites (148) and the compat corpus pass; a rebuilt BusyBox and a GUI program
  with the note run; a wrong `--program` ELF and a wrong banner SHA are each refused or warned of.

## 13. As built: reproducible ids (step 2's first part, 2026-11-15)

Found when bmds-apps' release workflow built Minesweeper: the same code as a local build, 20 bytes
apart, all of them the build-id (the debug information's paths).

- **`BmdsProgram.cmake`** maps the SDK (`/sdk`), the build folder (`/build`), the toolchain's root
  (`/riscv32-esp-elf`) and the project (`/`, unless it is inside the SDK: the examples keep
  `/sdk/examples`). GCC uses the last map that matches, so they are given shortest path first: a
  nested folder (an SDK downloaded into the build folder, a build folder in the project) gets its
  own name. **`bmds-cc`** gets the same list (`BMDS_CC_PREFIX_MAPS`), after the caller's
  `BMDS_PREFIX_MAPS` (new, in the environment: OLD=NEW, space-separated), so a caller's folder
  holding the SDK leaves the SDK its names.
- **`bmds-dist`** gives each package `BMDS_PREFIX_MAPS`: the repository as `/`, its recipe folder,
  its sources (`/dist/src/NAME-VERSION`, or their path here) and its build folder
  (`/dist/build/NAME`).
- **`tools/bmds-fault --source-root DIR`** (repeatable; default this repository) shows each file
  under the first root holding it (`tools/bmdself.py`'s `rooted()`); a name under none stays.
- **Checked,** each route built twice in different folders, the second through a symlinked
  toolchain: the SDK examples (48 ELFs), `bmds-dist`'s netlat and dropbear, and bmds-apps with its
  SDK inside its build folder: byte-identical, build-ids included, and no path of the building
  machine left in the debug ELFs (only Espressif's `/builds/...` in libgcc). A fault on bmds0
  decoded by `bmds-fault` names `<repo>/sdk/tests/progtest.c:4789`.
- **Still open:** the rest of step 2 (the firmware: §13b).

### 13a. The firmware's `CONFIG_APP_REPRODUCIBLE_BUILD`, checked (2026-11-15)

- **What it does** (IDF's `tools/cmake/prefix_map.cmake`): `-fdebug-prefix-map` for IDF (`/IDF`),
  the project (`/IDF_PROJECT`), the build (`/IDF_BUILD`), each component
  (`/COMPONENT_<NAME>_DIR`) and the toolchain (`/TOOLCHAIN`), writing `build/gdbinit/prefix_map`
  (GDB's `set substitute-path` back); and it forces `APP_COMPILE_TIME_DATE` and
  `BOOTLOADER_COMPILE_TIME_DATE` off. `__FILE__` is already hidden (`COMPILER_HIDE_PATHS_MACROS`).
- **What it costs:** nothing in the image. BMDS reads only the app description's version, ELF
  SHA and IDF version, never its date and time; the bootloader's date is already off
  (`sdkconfig.defaults`), and the bootloader built with it is byte-for-byte 0.3.1's (no
  bootloader question for boards). `bmds-panic`'s backtraces would name `/COMPONENT_..._DIR/...`:
  it should read `gdbinit/prefix_map` beside the ELF and put the real folders back.
- **Measured:** the app and bootloader built from one commit in two worktrees at different paths
  with it on: the bootloader identical, `bmds.elf` 62 bytes apart, all of them `RomContentHash`
  (`.flash.rodata`, and its value in `.debug_info`): the firmware pins the ROM's hash, and the ROM
  isn't reproducible (`romimage.h`'s `buildTime` in its header; BusyBox's build date). No host path
  left in the ELF. So the firmware is reproducible once the ROM is.

### 13b. As built: the firmware reproducible (2026-11-15)

- **On:** `CONFIG_APP_REPRODUCIBLE_BUILD=y` in `sdkconfig.defaults` (and the two developer
  `sdkconfig`s, which keep their own value).
- **The ROM's time is the commit's:** `components/bmds_storage` runs `mkrom.py image` and the
  ROM's `bmds-dist` with `SOURCE_DATE_EPOCH`, the caller's or else `git log -1 --format=%ct`,
  taken at configure (mkrom used the newest input file's mtime). `tools/release` sets it for
  everything it builds (BusyBox's build date honours it too). `rom` on the board says
  `Built: <the commit's time>`.
- **The recovery firmware:** its project is `recovery/`, so IDF's maps missed the repository's
  `thirdparty/` sources and the components' `__FILE__`, and `EXTRA_COMPONENT_DIRS` had a `..`
  that matched no component map. `recovery/CMakeLists.txt` normalizes it and maps the repository
  as `/BMDS` (before `project()`, so IDF's maps win where they match). `esp32p4.md` §5.
- **`tools/bmds-panic`** puts the real folders back: IDF's `gdbinit/prefix_map` beside the ELF,
  `/IDF_BUILD`, `/IDF_PROJECT` (its `CMakeCache.txt`), `/IDF` (`IDF_PATH`) and `/BMDS`.
- **Checked:** the whole firmware from one commit in two worktrees at different paths, with no
  `SOURCE_DATE_EPOCH` given: `bmds.bin`, `bmds.elf`, `rom.bin`, the bootloader and the recovery
  firmware byte-identical, no host path left, the bootloader still 0.3.1's. Installed on bmds0:
  it runs, `rom` names the commit's time and matches the firmware; a deliberate panic
  (`progtest faults syscall` in M-mode) decoded by `bmds-panic` with real paths.
