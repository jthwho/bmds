# Storage: SD Card, Filesystem, Files in the Shell

Status: **done** (Phase 2, steps 1-5): SD driver with hot-plug; the clock; FatFs, the VFS, numbered devices and partitions, a run-time mount table with automatic mounting; the file commands; files in the shell language; YMODEM `rz`/`sz`.

## 1. Goal

- The microSD card works as the system's disk: its partitions mounted at **`/sd0/p0`**, `/sd0/p1`... at boot, readable and writable from any PC (FAT32, and exFAT for the 64 GB+ cards that ship with it).
- **Hot-plug:** insert and remove cards at any time. A removed card fails its open files cleanly; a new one mounts by itself.
- **Files are streams.** An open file is a `Stream` behind a `FileHandle`, exactly like a pipe or the console, so redirection, pipelines and (Phase 3) programs need nothing new.
- **The shell gets files:** `ls cd pwd cat cp mv rm mkdir rmdir df hexdump touch`, `<` `>` `>>` to files, globbing, file tests, `source`, `sh script`, `/sd0/p0/autoexec.sh`, path completion.
- **Files over the serial cable:** push and pull files with YMODEM, which picocom, minicom and tio already drive through `lrzsz`.
- **Testable on the host:** the filesystem layer and the file commands run in the host tests against a RAM disk.

## 2. SD card driver (`bmds_storage`, target)

### Hardware (from the board definition)

4-bit SDMMC slot 0 on IOMUX pins (CLK 43, CMD 44, D0–D3 39–42). The IO domain comes from on-chip **LDO channel 4** at 3.3 V. Card power is switched by GPIO45 (active low, on by default). Card detect is GPIO3, whose pull-up is powered by LDO 4, so **LDO 4 comes up before card detect is read**. No write protect.

### IDF usage

| Need | Choice | Why |
|---|---|---|
| SDMMC host, card protocol | IDF `esp_driver_sdmmc` (its `sdmmc_host_*` API, which `sdmmc_card_init` takes) + `sdmmc` (`sdmmc_card_init`, `sdmmc_read/write_sectors`) | DMA and interrupt-driven, no task of its own; card init and the protocol state machine are a lot of tested code. Wrapped behind `BlockDevice`. `CONFIG_SD_ENABLE_SDIO_SUPPORT` is off: memory cards only. |
| IO voltage | `sd_pwr_ctrl_new_on_chip_ldo` (LDO 4) | IDF's own way to own the SD LDO; also what UHS-I 1.8 V switching would use later. |
| Card detect, power | Our GPIO interrupt on GPIO3 (both edges), GPIO45 output | A debounced interrupt: no polling. |

**DMA rule: no allocation per I/O.** `sdmmc_read/write_sectors` allocate an internal DMA bounce buffer on every call whose buffer isn't DMA-capable and aligned. Our block device guarantees they never do: callers' buffers that are DMA-capable (internal DMA memory or PSRAM, which the P4's SDMMC can reach) with a cache-line aligned start and length, as `PsramDma` allocations are, go straight to DMA; anything else is copied through **one bounce buffer allocated at boot** (`CONFIG_BMDS_SD_BOUNCE_BYTES`, default 4 KB of `PsramDma`, so an unaligned multi-sector request still moves 8 sectors per command). The host driver itself doesn't allocate per transaction. FatFs's own window buffer is inside its `FATFS` object and isn't aligned, so metadata goes through the bounce sector; bulk file data read or written in whole sectors into aligned buffers takes the direct path. Measure both.

**Speed:** default speed (20 MHz) for init, then **high speed, 40 MHz, 4-bit** (~20 MB/s on the bus); `sdmmc_card_init` makes the switch when the card supports it. `CONFIG_BMDS_SD_MAX_KHZ` caps it. UHS-I (SDR50/SDR104 at 1.8 V, which the LDO can supply) is a later experiment, recorded as an open question.

### Hot-plug: the storage service task

One task (`storage`, PSRAM stack, low priority) owns the card:

1. Card-detect edge (ISR) → release a semaphore.
2. The task debounces by **waiting on the semaphore with a timeout** (no polling): only when the line has been stable for 50 ms does it act.
3. **Inserted:** power-cycle the card (GPIO45: 20 ms off, 10 ms on, so a card left in any state after a reboot resets), `sdmmc_card_init` (which switches to high speed), read the partition table, mount each FAT partition (§4). Failures are logged and retried on the next edge.
4. **Removed:** unmount everything on the card at once (open files go stale, below), stop the SDMMC host, cut card power.

**The SDMMC host runs only while a card is attached:** it is started fresh for each card and deleted after it. IDF's slot (v6.1) can't be reset to identification settings: `sd_host_slot_configure` marks a width change as applied but never stores the width, so after the first card the slot stays 4-bit for good, along with the D3 handling that puts a new card into SD rather than SPI mode, and the next card times out in `sdmmc_card_init`. (IDF's own mount code tears the host down between cards, so it never meets this.) A fresh slot per card costs a few hundred bytes of internal heap churn per insertion, and frees the host's memory while the slot is empty.

The first mount attempt happens at boot, in the task, so boot isn't held up by card init (~100–300 ms). `Storage::waitForFirstMount(timeoutMs)` lets the shell wait for it before `autoexec.sh`.

### `BlockDevice` (in `bmds_fs`, portable)

```cpp
class BlockDevice {
        virtual uint32_t sectorSize() const = 0;     // 512 for SD
        virtual uint64_t sectorCount() const = 0;
        virtual Error    read(uint64_t sector, WritableByteSpan buf) = 0;   // whole sectors
        virtual Error    write(uint64_t sector, ByteSpan data) = 0;
        virtual Error    sync() = 0;
        virtual Error    trim(uint64_t first, uint64_t count);              // optional
};
```

`read()`/`write()` check the request (whole sectors, in range, medium present) and then call the implementation's `readSectors()`/`writeSectors()`. The destructor is protected and not virtual, as `Stream`'s: devices are never deleted through the base, so no `operator delete` is linked.

`SdCard` implements it on the target; `RamBlockDevice` (a `Psram` buffer) implements it for the host tests and for a possible `/ram` disk later; USB mass storage would be another.

## 3. Filesystem: FatFs, vendored (`bmds_fs`, portable)

**Decision: vendor ChaN's FatFs in `thirdparty/fatfs` with our own `ffconf.h` and system glue, instead of IDF's `fatfs` component.** Vendored: **R0.16** (current upstream; R0.15 was named before checking) with ChaN's patches 1 and 2, which fix CVE-2026-6682/6683/6687 (crash or buffer overflow from crafted FAT/exFAT fields: exactly what a machine that mounts any inserted card must survive). `thirdparty/fatfs/README.md` records the archive hash, the patches and how to update.

| | IDF `fatfs` component | Vendored FatFs (recommended) |
|---|---|---|
| exFAT | **Compiled out** (`FF_FS_EXFAT 0` in its fixed `ffconf.h`) | On: 64 GB+ SDXC cards work as they come from the shop |
| Dependencies | Pulls in IDF VFS, SDSPI, wear levelling, a `diskio_sdmmc` that allocates bounce buffers | None: FatFs is plain C over our `BlockDevice` |
| Locks, memory | FreeRTOS mutexes and `malloc` in its port | Our `Mutex`, our `Memory` (LFN buffers in `Psram`) |
| Host tests | Linux port exists, but not with our block device | Builds in the host tests, against `RamBlockDevice` |
| Updates | Comes with IDF | We update a vendored copy (FatFs changes rarely; R0.16 is from 2025) |

Configuration (`components/bmds_fs/src/fatfs/ffconf.h`, every option with its reason): long file names (255, working buffer from `Psram` per call), UTF-8 API (`FF_LFN_UNICODE 2`, `FF_LFN_BUF 765` so any name fits), code page 437 for short names, **exFAT on** (64-bit LBA, GPT), `FF_FS_LOCK 16` (at most 16 files and directories open; no file open twice for writing, no deleting an open file), `f_mkfs`, `f_utime`, trim, fast seek off. `FF_FS_TINY 0`: each open file has its own sector buffer (`Psram`), so files don't thrash one shared window. **`FF_FS_REENTRANT 0`, unlike the first design:** FatFs's locking doesn't cover `f_mount()` and `f_mkfs()`, and a card can be unmounted while another task is in FatFs, so the BMDS FAT layer makes every FatFs call under one lock of its own (all volumes; FatFs serialized each volume anyway). FatFs compiles warning-free under the BMDS warning set and runs clean under ASan, UBSan and TSan in the host tests.

**The metadata cache** (Phase 3; `src/sectorcache.h`): FatFs keeps one sector of directory and FAT (its window), so a lookup in a large directory read the directory from the card again every time: BusyBox's `bin` (119 entries, 23 sectors on exFAT) cost about 20 ms a lookup, and `ls -l` of it 2.4 s. The port now keeps a copy of every sector FatFs reads into a volume's window (directories, the FAT, exFAT's bitmap, the boot sector; file data never goes there with `FF_FS_TINY 0`): 64 sectors shared by the volumes, least recently used out, 33 KB of `Psram` allocated by `Vfs::init()`. Write-through: every write still goes to the device and updates the copies it covers (whatever buffer it comes from: `dir_clear()` and file data can land on sectors that held directories), a failed write or a trim drops them, and a mount, an unmount or a format drops the drive's. Writes that bypass FatFs (a raw write to a mounted partition's `/dev` node) aren't seen, as FatFs's window doesn't see them either. Read-ahead and write-back stay out (§12): FatFs moves file data in whole-sector runs straight to the caller.

**Time stamps** come from the kernel's system clock ([`time.md`](time.md)): `get_fattime()` converts `Clock::now()` to local time in the kernel time zone (FAT stores local time; exFAT also records the UTC offset). An unset clock still counts forward from 2026-01-01, so new files never carry 1980.

**Why not IDF VFS + newlib (`fopen`, `open`)?** It brings a second, global fd table, newlib `FILE` buffers in internal RAM, errno and blocking calls with no timeouts or interrupts, beside the `FdTable`/`FileHandle`/`Stream` model the shell and the Phase 3 ABI already use. Programs will get file syscalls from BMDS (Phase 3); a newlib in a program can sit on those.

## 4. The VFS (`bmds_fs`, portable)

### Devices, partitions and mounts

**No singletons, even where there is one of something today:** names carry numbers.

- **Block devices register with numbered names**: the SD slot is `sd0` (a second slot or a USB stick would be `sd1`, `usb0`). Each appears as a device node, **`/dev/sd0`**.
- **Partitions are block devices too.** When a medium arrives, BMDS reads its partition table itself (MBR primary entries, or GPT; a medium formatted without one, "superfloppy", is one partition covering it all) and registers each partition, numbered from 0 in table order: **`/dev/sd0p0`**, `/dev/sd0p1`. A partition is a window onto its parent (`PartitionDevice`), so FatFs only ever sees a plain volume (`FF_MULTI_PARTITION 0`) and partitions FatFs can't mount are still listed. Extended (logical) MBR partitions are not read yet.
- **The mount table is run-time state.** `Vfs::mount(path, device)` mounts the FAT/exFAT filesystem on a block device at an absolute path; `Vfs::unmount(path)` undoes it. Up to four mounts (`FF_VOLUMES 4`). A mount point is any absolute path outside `/dev` and outside other mounts; its parents are virtual directories made up from the mount paths, so `/` lists `dev` and `sd0`, and `/sd0` lists `p0`. An unmounted path is gone (`NotExist`); files still open on it fail with `NoMedia`.
- **Automatic mounting:** the storage service mounts every FAT partition of an inserted card at **`/<device>/p<N>`** (`/sd0/p0`), and when the card is pulled unmounts everything on it, its partitions' mounts included, even ones made by hand. Mounting by hand: `mount /dev/sd0p0 /data`, `umount /data` (§5). A configuration file for mounts can come later (with `/sd0/p0/autoexec.sh` able to run `mount` meanwhile).
- **Why numbers, not volume labels:** labels can be empty, duplicated, or contain spaces and slashes, and relabelling a card would break every script. Labels could be added later as extra names (like Linux's `/dev/disk/by-label`).
- `/dev/null` and `/dev/console` (the caller's terminal) are device nodes too, so `/dev/null` stops being a special case in the shell. Block device nodes can't be opened as streams yet (raw access is a later need); `mkfs` names them.

Paths are absolute, with `/` separators, in UTF-8. FAT names are case-insensitive and case-preserving.

- **The current directory is per shell** (and per program in Phase 3), not global: the VFS resolves a path against the cwd it's given, normalizing `.` and `..` (`..` never climbs above `/`).

### API (kernel-internal; the Phase 3 syscalls wrap it)

```cpp
namespace Vfs {
        struct Context { StringView cwd; Stream *terminal; };   // the caller: its cwd, and what /dev/console opens
        Error                   init();                          // the storage lock; boot (Storage::init) or a test
        Error                   mount(StringView path, BlockDevice &);          // up to MaxMounts (4)
        Error                   unmount(StringView path);
        Result<size_t>          automount(BlockDevice &);        // partitions -> /<dev>/p<N>
        void                    eject(BlockDevice &);            // the medium went
        size_t                  listMounts(Span<MountInfo>);     // path, device name, type
        BlockDevice            *findDevice(StringView path, const Context &);   // "/dev/sd0p0"
        Error                   format(BlockDevice &, FsFormat); // Auto, Fat32, ExFat; whole device: a partition table too
        Result<FileHandle::Ptr> open(StringView path, uint8_t flags, const Context &);  // Read Write Create Truncate Append Exclusive
        Result<FileInfo>        stat(StringView path, const Context &);          // name, type, size, modified (UTC), attributes
        Result<Directory>       openDirectory(StringView path, const Context &); // next() -> FileInfo; EndOfFile at the end
        Error mkdir(...), rmdir(...), remove(...), rename(from, to, ...), setModified(path, Timestamp, ...);
        Result<FsInfo>          statfs(StringView path, const Context &);        // type, total, free, cluster size
        Result<String>          normalize(StringView path, StringView cwd);
}
namespace BlockDevices { addNumbered(device, "sd", name); add(device, name, parent); remove; find; nameOf; list; }
namespace Partitions   { read(device, out); attach(device); detach(device); }   // MBR/GPT; "sd0p0"...
```

- **One storage lock** covers the device registry, the mount table and every FatFs call: each VFS call resolves its path and does its work under one hold, so an unmount can't swap the volume under it. FatFs volumes are a pool of four (`FatVolume`, private), each with a `FATFS` object allocated once and kept; a volume mounts whatever device it is given.

- **The caller's context** carries the cwd and the terminal, so `/dev/console` opens the caller's console (and Phase 3 programs will pass their own).
- **A file** is a `Stream` (read, write, flush = `f_sync`, `seek`) behind a `FileHandle`, which frees it in `handleClosed()`, so `dup` and `2>&1` share one file offset, as POSIX does. `Stream` gains `seek()` (`NotSupported` for pipes and consoles); the size is `seek(0, End)`. `Append` writes at the end every time; a short write (full volume) is `NoSpace`.
- **A directory** is a move-only value (no heap pointer type in the API); its FatFs state and an entry buffer live in `Psram` while it is open.
- **Errors:** FatFs results map onto `Error` (`FR_NO_FILE`/`FR_NO_PATH` → `NotExist`, `FR_DENIED` → `PermissionDenied` (`NotEmpty` from `rmdir`), `FR_EXIST` → `Exists`, `FR_LOCKED` → `Busy`, `FR_TOO_MANY_OPEN_FILES` → `TooManyOpen`, `FR_WRITE_PROTECTED` → `ReadOnly`, `FR_NOT_READY`/`FR_NOT_ENABLED`/`FR_INVALID_OBJECT` → `NoMedia`, a disk error with the medium gone → `NoMedia`, `FR_NO_FILESYSTEM` → `NoFilesystem`). Opening a directory is `IsDir`, listing a file `NotDir`. New codes, appended: **`Error::NoMedia`** (step 1) and **`Error::NoFilesystem`** ("no filesystem": the card holds nothing FAT).
- **Removal:** the volume's `FATFS` object is allocated once and kept, so when the card goes, `f_unmount` bumps its id and every stale `FIL` fails FatFs's own validity check (`FR_INVALID_OBJECT` → `NoMedia`). No dangling pointers, no extra bookkeeping. A new card mounts into the same object.
- **Thread safety:** every FatFs call is made under the FAT layer's lock (above); a file may be used from one task at a time (as any stream).
- **Mounting on the board:** the storage service registers the slot as `sd0` at boot; after attaching a card it runs `Vfs::automount()` (2 ms for the Raspberry Pi test card: its FAT32 boot partition mounts at `/sd0/p0`, its ext4 partition `/dev/sd0p1` is listed but not mounted) and logs each mount; before detaching it runs `Vfs::eject()`. A card with no FAT filesystem gets a warning.

### Symbolic links (emulated; Phase 3)

FAT and exFAT have no links (agreed 2026-10-04: emulate them, as Cygwin does on FAT; needed to break BusyBox into its applets, and by POSIX programs generally).

- **On disk** (frozen once shipped; Cygwin's "system file" symlink, so Cygwin and MSYS2 on Windows see them as links too, and Linux sees small files): a regular file with the FAT **System** attribute, holding the cookie `!<symlink>`, the byte order mark `FF FE`, the target in UTF-16LE, and a 16-bit NUL. Read also: no BOM after the cookie means a UTF-8 target (older Cygwin, hand-made links). At most `BMDS_PATH_MAX` bytes of target. A System file without the cookie is an ordinary file.
- **Targets** are BMDS paths: absolute, or relative to the link's directory (`busybox`, `../share/x`): relative links survive the card being mounted elsewhere.
- **Cost: nothing for paths without links.** The VFS doesn't look at path components one by one. FatFs reports "no path" (`FR_NO_PATH`) when a directory along the way is missing or is a file, which is how a link in the middle of a path shows up: only then are the components walked (one `f_stat` each) to find it. A link as the last component shows up as a successful `stat`/`open` of a file with the System attribute: only those are read to check the cookie. Missing files (`FR_NO_FILE`, as in `$PATH` searches) cost nothing extra.
- **Following:** `open`, `stat`, `opendir`, `set_mtime`, `chdir` and running a program follow links, at most 8 in a row (`ELOOP`, `too many levels of symbolic links`). `lstat`, `readlink`, `unlink`, `rename` and `rmdir` act on the link itself (links in the directories along the way are followed). Opening a link with create makes the target. A current directory reached through a link is the physical path (as `cd -P`): relative paths then don't walk the link each time.
- **Listing:** `readdir` calls a file a link (`FileType::Link`, size the target's length if ASCII) by its attributes alone: System, not Hidden (the System files Windows makes, `desktop.ini`, are also Hidden), and small enough. Reading each one made a plain `ls` of BusyBox's 119-entry `bin` take 4.2 s (each read is a lookup in that directory); now 0.05 s. `lstat`, `readlink` and following read the cookie, so a listing may call a non-link a link, never the reverse: `ls -l` reads each to show `-> target` (2.5 s for that `bin`, BusyBox's `ls -l` 4.7 s; with the metadata cache of §3, 0.30 and 0.75 s).
- **FatFs patch:** `f_open` didn't keep the file's attribute in the `FIL`; one line in `thirdparty/fatfs` (`patches/bmds-attr.diff`) does, so opening a file sees the System attribute with no second lookup. The link is read on that same `FIL` inside `Fat::openFile` (closing a handle, or opening the file again, while holding the FAT lock would deadlock or hit FatFs's own locking).
- **Creating:** `symlink(target, path)` writes the file and sets the System attribute (`f_chmod`); `EEXIST` if the name is taken.
- **Kernel API:** `Vfs::symlink`, `Vfs::readLink`, `Vfs::lstat`, `Vfs::realPath`; the others follow as above.
- **ABI:** slots 32 `symlink(target, path)`, 33 `readlink(path, buf, size)` (NUL-terminated, cut at a character boundary, returns the full length, as `readdir`'s name), 34 `lstat`; `BMDS_TYPE_LINK` 4; `BMDS_ELOOP` 40. The SDK's layer maps `symlinkat`, `readlinkat`, `statx(AT_SYMLINK_NOFOLLOW)` (mode `S_IFLNK | 0777`), `O_NOFOLLOW` and `DT_LNK`.
- **Shell:** `ln -s [-f] target name` (`ln` without `-s`: no hard links on FAT), `ls -l` shows `l` and `name -> target`, `test -L`/`-h`, `cd` resolves to the physical path, `rm` removes the link.
- **From a PC:** a link copied onto the card by a PC loses the System attribute unless the PC's tools keep it (Cygwin's do), so the distribution creates its links on the board (`devplan/distribution.md`).

## 5. Shell

### Commands (formats are an interface: fixed as shipped in step 3)

Errors are `command: path: reason`, with POSIX's `strerror` wording ("No such file or directory", "Is a directory", ...: `CommandContext::errorText()`), as the GNU tools print them. Each shell has its own current directory (`ShellState::cwd`, copied into subshells and pipeline stages like the rest of the state), which commands resolve paths against (`CommandContext::vfs()`).

| Command | As shipped |
|---|---|
| `cd [dir \| -]`, `pwd` | Set `PWD` and `OLDPWD`; `cd` alone goes to `$HOME` (`/sd0/p0` on the target), `cd -` to `$OLDPWD` and prints it. Errors: `cd: x: Not a directory`, `cd: HOME not set`, `cd: too many arguments`. The prompt's `\w` shows `$HOME` and below as `~`, as bash does; the target's prompt is `bmds:\w> `. |
| `ls [-la1] [path]...` | Names sorted case-insensitively; directories end in `/`. On a terminal, columns filled top to bottom at the width of `$COLUMNS` (else the console's); one per line otherwise or with `-1`. `-l`: `<type>r<w>-  <size>  <yyyy-mm-dd hh:mm>  <name>`, e.g. `-rw-     32495  2025-04-30 12:45  bcm2710-rpi-2-b.dtb`; type `d`, `-` or `c` (device); `w` is `-` for read-only files; sizes right-aligned to the widest; time in `$TZ` (else the kernel zone), blank when unknown. `-a`: names starting with `.` and hidden or system files too. Directories blue, devices yellow when colors are allowed. GNU's order: the files named, then each directory, with `path:` headers when there are several; status 2 if a path can't be reached. |
| `cat`, `head`, `wc`, `grep` | Take file arguments (`-`, or none, is stdin). `head` with several files prints `==> name <==` headers; `wc` prints a row per file and a `total` row, columns as wide as the largest number (stdin alone keeps 7-wide columns); `grep` prefixes `name:` with several files, and its status is 2 when a file can't be read. |
| `cp [-r] source... target` | Into `target` if it is a directory (required with several sources). Refuses copying a file onto itself or a directory into itself; a directory without `-r` is `x: Is a directory (not copied without -r)`. 32 KB `PsramDma` buffer, so whole sectors go straight to DMA. |
| `mv source... target` | Renames within a filesystem (replacing a file in the way, not a directory); across filesystems copies, then removes the original. |
| `rm [-rf] path...`, `mkdir [-p] dir...`, `rmdir dir...`, `touch file...` | `rm -r` removes trees (mount points are `Device or resource busy`); `-f` ignores missing files. `mkdir -p` makes parents and accepts existing directories. Nothing can be created outside a mounted filesystem (`Permission denied`). `touch` creates, or sets the time to now. |
| `df [-h]` | Coreutils' layout: `Filesystem     1K-blocks  Used Available Use% Mounted on`, or with `-h` `Size Used Avail` in powers of 1024 (one decimal below 10, rounded up); columns sized to fit; `Use%` rounded up. |
| `hexdump [-C] [file]` | The canonical hex+ASCII layout, with runs of identical lines shown as one `*` and the final offset at the end. Plain `hexdump` prints the same (the classic 16-bit-word format is not implemented). |
| `sd [bench [megabytes]]` | Card info, one `Key: value` line each: `Card:` name and type (SDSC/SDHC/SDXC/MMC), `Capacity:` GB and sectors, `Bus:` width and clock, `Maker:` CID manufacturer, OEM, revision, serial, date. No card (or an unusable one): a message on stderr, status 1. `bench` reads from the start of the card, direct and through the bounce buffer, in 64 KB and 512 B requests, and prints MB/s. |
| `mkfs [-t auto\|fat32\|exfat] -y DEVICE` | Without `-y`: `mkfs: this erases everything on /dev/sd0; run again with -y to go ahead`, status 1. A whole device (`/dev/sd0`) is ejected, gets a new partition table with one partition, and is mounted again (`/sd0/p0`); a partition (`/dev/sd0p0`) is unmounted, formatted alone and mounted at its default path. Prints the resulting mounts as `mount` does. |
| `mount`, `mount DEVICE PATH`, `umount PATH` | Without arguments, the mounts, one per line, as on Unix: `/dev/sd0p0 on /sd0/p0 type exfat`. With them, mount or unmount by hand. |

### Language (as shipped in step 4)

Behavior follows bash; the run corpus checks it against bash (each script starts in an empty directory: a fresh temporary directory for bash, a freshly formatted RAM disk for BMDS).

- **File redirection:** `<`, `>`, `>>`, `>|`, `<>` (opens read-write, creating), `&>`, `&>>`, `>&file` open real files relative to the cwd; `/dev/null` is the VFS's device, no longer a shell special case. A target is expanded like an argument (split, globbed) and must come to exactly one word, else `$x: ambiguous redirect` (the word as written). Errors are `path: reason` in `strerror` wording; creating a file outside every filesystem is `Permission denied` (`Vfs::open` now says so too). **`set -C`** / `set -o noclobber` (`C` in `$-`): `>`, `&>` and `>&` refuse an existing regular file (`f: cannot overwrite existing file`); `>|` overrides.
- **Globbing:** unquoted `*`, `?` and bracket expressions expand to the matching paths, component by component (`Glob::expandPaths()`), sorted as `ls` sorts (case-insensitive, then bytewise; the bash check runs in the C locale with names whose orders agree). A leading `.` must be matched explicitly, and `.`/`..` never match; a trailing `/` matches directories only; no match leaves the word as is; unreadable directories match nothing. Results of unquoted expansions glob too (`x='*.txt'; echo $x`), quoted text never does. The expander keeps a pattern copy of a field (quoted specials escaped) only from the first character that needs it, so plain words cost nothing extra. A `[` without its `]` is not magic, so `[ -f x ]` reads no directory.
- **Tilde:** a word starting with unquoted `~` alone or `~/` gets `$HOME` (not split or globbed), in arguments, assignments (`x=~/bin`), redirection targets and `case`. `~user` stays as written; with `HOME` unset, `~` stays.
- **File tests:** `-e -r` exist; `-f -d -c` regular file, directory, device; `-s` non-empty (directories count, as on Unix); `-w` not read-only; `-x` directories and `*.sh` files (FAT has no execute bits; since Phase 3 also programs and `#!` files: `programs.md` §5); `-nt -ot` by modification time, a file that exists being newer than one that doesn't; `-ef` the same normalized path, ignoring case.
- **`source file [args]`**, **`. file [args]`**: run in the current shell; with arguments, they are the positional parameters while it runs; `return` ends it. Errors: `source: filename argument required` (status 2), `.: x: No such file or directory` (1). **`sh file [args]`**, **`sh -c command [name [args]]`**, and `sh` with a script on stdin (not a terminal): a subshell with its own positional parameters; `exit` ends just the script; an unreadable file is status 127. `$0` stays `bmds`. (Running `./script.sh` directly, and `#!` scripts, came with the Phase 3 program model: `programs.md` §5; `sh` also takes `set`'s `-Ceux` options before the script.)
- **`/sd0/p0/autoexec.sh`:** `Shell::spawn()` takes a `Startup` (a wait function and a script); the console shell waits for the first card check (`Storage::waitForFirstMount`, at most 3 s) and then sources the script if it exists, before the first prompt. Ctrl-C interrupts it, as anything else.
- **Path completion:** in argument position, after a redirection, or in command position when the word has a `/`, Tab completes file and directory names: directories get a `/` and no space, special characters are escaped with `\` (the editor's word may contain escaped blanks), `~/` works, dot files are offered only when the word starts with `.`, and ambiguous candidates are listed by their last component.
- **Reading files:** `cat`, `wc`, `head` and `grep` now read through a 32 KB `PsramDma` buffer (was 2 KB on the stack, and 256 B for line readers), so whole sectors go straight from the card in one transfer: `cat` of a 4 MB file 1.56 → 0.35 s, `wc -c` 1.96 → 0.60 s.

## 6. Files over the serial cable: YMODEM (as shipped in step 5)

- **`rz [-y]`** receives into the cwd, **`sz file...`** sends; both speak **YMODEM** batch (block 0 with name, decimal size, octal mtime and mode; 1 KB `STX` and 128-byte `SOH` blocks with CRC-16/XMODEM; an ACK per block), interoperating with `lrzsz` 0.12.21: on the PC `sz --ymodem file...` and `rz --ymodem` (picocom: `--send-cmd "sz -vv --ymodem" --receive-cmd "rz -vv --ymodem"`, then `C-a C-s` / `C-a C-r`). The protocol is `Ymodem::send()`/`receive()` (`bmds/ymodem.h`), pure code over a `Stream` with handler interfaces for the files, so the host tests run both ends against each other over pipes.
- **The line:** the commands refuse to run unless stdin and stdout are the shell's console (`rz: the serial line must be the terminal (no redirections or pipes)`). While a transfer runs the console is in `TermMode::binary()`: no Ctrl-C signal, no newline translation, and two new modes: `colors` off (the color filter would rewrite ESC sequences in the data) and `logs` off (log lines go only to the history; on the line they would corrupt blocks). Nothing is printed during a transfer: per-file lines (`rz: name: N bytes`) and the summary (`rz: 2 files, 71689 bytes in 1.9 s (102 KB/s)`, with `, N retries` when there were any) come afterwards, once the line has been quiet for 250 ms (so the program on the PC has exited and doesn't swallow them). The summary is left out when nothing was transferred.
- **Protocol choices:** the receiver repeats its `C` every second until the sender starts (one minute in all; a sender started later waits at most a second), answers the first `EOT` with `NAK` and the second with `ACK` (as the YMODEM description says; lrzsz resends), ACKs a repeated block, cancels on a wrong block number, and NAKs a damaged block after purging the line until it has been quiet for 200 ms (10 tries per block). Both ends cancel with lrzsz's sequence (8 CAN, 8 BS) and take two CANs in a row from the other end as a cancel; a Ctrl-C byte cancels only while waiting for the other end to start (afterwards a 0x03 is data, and the terminal's Ctrl-X x5 sends CANs). The receiver ACKs a block before storing it, so the sender prepares the next one meanwhile. Messages printed before a transfer contain no capital `C`, which a sender already running would take for the start.
- **Files:** `rz` keeps only the last path component of a name, refuses `.`, `..` and empty names, and replaces an existing file only with `-y` (else `rz: f: File exists (rz -y replaces files)` and the transfer is cancelled); a file whose transfer fails is removed. The size from block 0 cuts the padding; the mtime is set when given. Both commands move data through a 32 KB `PsramDma` buffer, so the card sees whole-sector runs, not 1 KB writes. `sz` checks every file before it starts (`sz: x: No such file or directory`, `Is a directory`).
- **RX ring:** the receiver doesn't read the console while it writes to the card, and USB has no flow control here, so `CONFIG_BMDS_USJ_RX_BYTES` goes from 1 to **4 KB** (`Psram`): a 1029-byte block plus slack. (An existing `sdkconfig` keeps its old value; set it to 4096.)
- **Speed** (board and lrzsz on the PC, 4 MB files): PC → board 300 KB/s (13.6 s); board → PC 650 KB/s of data, but lrzsz's `rz` adds about a second at the start of the batch, after each file and at the end (it does the same against its own `sz`), so small files take seconds. ZMODEM (streaming) would remove the per-block round trip if that is ever too slow.

## 7. Phasing

| Step | Delivers |
|---|---|
| 1 | `BlockDevice`, `RamBlockDevice`; `SdCard` (LDO 4, power, card detect, hot-plug task, high speed, bounce buffer); `Error::NoMedia`; `sd` command; a read benchmark (`sd bench`). |
| 2a | The system clock ([`time.md`](time.md)): `Clock`, `TimeZone`, the reset record, `date`, `clock`. **Done.** |
| 2b | FatFs vendored (`thirdparty/fatfs`, our `ffconf.h`, port in `fatfsport.cpp`); `bmds_fs` VFS, files, `Directory`; the block device registry (`sd0`), partition tables (`sd0p0`), the run-time mount table, automatic mounting (`/sd0/p0`), `mount`/`umount`; `/dev` nodes; `Error::NoFilesystem`; host tests on `RamBlockDevice`. **Done.** |
| 3 | Shell: `cd pwd ls cat cp mv rm mkdir rmdir touch df hexdump mkfs`; text commands on files; a current directory per shell; the VFS as a boot stage. **Done.** |
| 4 | Language: file redirection, globbing, tilde, file tests, `source`/`.`/`sh`, `autoexec.sh`, path completion. **Done.** |
| 5 | `rz`/`sz` (YMODEM); 4 KB RX ring. **Done.** |

## 8. Resource budget (estimates)

| Item | Flash | RAM |
|---|---|---|
| IDF SDMMC host + protocol + GPIO driver (**measured**) | 37 KB code + 11 KB of IDF's log and assert strings | 1.7 KB IRAM (cache sync, GPIO ISR, ISR-safe FreeRTOS calls); 0.8 KB internal heap at boot (host state, DMA descriptors, event queue, TCB) |
| `SdCard` + storage task + `sd` (**measured**) | 4.3 KB | 311 B `.bss`; 6 KB `Psram` stack (~400 B used idle); 4 KB `Psram` bounce buffer; `sdmmc_card_t` in `Psram` |
| FatFs (LFN, exFAT, mkfs) | ~30 KB | `FATFS` ~600 B + LFN buffer 512 B `Psram`; ~600 B `Psram` per open file |
| VFS, `File`, `Directory` | ~6 KB | — |
| Metadata cache (**measured**) | 1.2 KB | 12 B `.bss`; 64 sectors + tags, 33 KB `Psram` from boot |
| Shell file commands, globbing, redirection | ~12 KB | — |
| YMODEM, `rz`/`sz` (**measured**) | 6 KB | 1 KB frame on the stack and a 32 KB `PsramDma` buffer while transferring; RX ring 1 → 4 KB `Psram` |
| **Total** | **~110 KB** (the IDF SD stack is three times the estimate) | **~3 KB internal** |

The metadata cache verified on the board (the exFAT card): `ls -l /sd0/p0/bin` (119 entries, links) 2.36 → 0.30 s, `busybox ls -l` of it 4.22 → 0.75 s; 150 files created, half removed, one renamed, then `umount` and `mount`: the same 75 files and `md5sum`s, read from the card again; BusyBox `tar` round trip; the corpus. Host tests: a device counting reads sees none when 120 names in a directory are looked up a second time; a 600-entry directory (bigger than the cache) listed right after creating and removing half of it, and again after a remount, on FAT32 and exFAT. The image grew 1.2 KB (427.7 → 428.9 KB).

Step 1 measured on the board (32 GB SDHC, 4-bit, 40 MHz): sequential reads of 64 KB **16.3 MB/s direct**, 6.9 MB/s through the 4 KB bounce buffer; 512 B requests 1.3 MB/s on either path (about 0.38 ms of command overhead each, which is also what limits the bounce path: a larger bounce buffer moves more per command). The card is ready 120 ms after boot (50 ms debounce, 30 ms power cycle, init). Card init uses about 1.8 KB of the storage task's stack.

Step 1 measured: the image grew by 58 KB (200 → 258 KB flash), internal RAM free at boot fell by 2.9 KB (610,895 → 607,983 bytes).

Step 5 verified on the board against lrzsz 0.12.21 (Ubuntu's package): round trips of 0 B, 1 B, 1000 B, 1024 B, 300 KB, 1 MB and 4 MB files (PC `sz --ymodem` → `rz`, then `sz *` → PC `rz --ymodem`) came back identical; Ctrl-C cancels `rz` and `sz` while they wait; an existing file is refused (lrzsz reports the cancel, the old file stays) and `rz -y` replaces it. Measured: the image grew by 6.9 KB (389.9 → 396.8 KB; the shell 6 KB); internal RAM 8 bytes (the two new mode flags); the RX ring 3 KB more `Psram`; a transfer takes a 32 KB `PsramDma` buffer while it runs.

Step 4 verified on the board (the exFAT card): writing 4 MB through `>` in 64 KB `echo`s 1.86 s (2.2 MB/s, shell included); `cat big big > big3` (8 MB read and written) 2.06 s; `cp` of 4 MB 1.01 s; reading 4 MB with `cat` 0.35 s (12 MB/s) and `wc -c` 0.60 s, after the 32 KB read buffer (before: 1.56 s and 1.96 s); `grep` over 64 KB lines is CPU-bound (1.76 s for 4 MB). Globs across mounts (`/sd*/p0/w/f`), noclobber, file tests, `source` with `return`, `sh -c`; `autoexec.sh` runs before the first prompt (about 0.4 s after reset, once the card is mounted), and Ctrl-C stops it in a `sleep` or a busy loop, keeping what it changed (its `cd`). Tab completion of paths on the card, `/sd0/` and `~/`. Pulling the card during an endless write loop (`while :; do echo "$s"; done > stream`, 64 KB a write): the write in flight fails after the IDF's 1 s timeouts (about 4 s), and only then does the storage task, which needs the storage lock for the eject, log the removal; later writes fail at once, and the loop runs on. The card mounts again 0.13 s after it is back, and new files work. The file being written comes back empty (0 bytes): FAT writes the size into the directory entry only on close or sync. The clusters it had taken are free again, so no space is lost (`df`: 192 KB used, as before). Nothing tells the loop that its writes fail, since commands ignore write errors (an open item). A busy loop on the console shell (autoexec's or typed) starves IDLE0, so the task watchdog prints a warning after 5 s, as it did before step 4.

Step 4 measured: the image grew by 6.7 KB (383.2 → 389.9 KB). No new internal RAM; a command that reads files takes a 32 KB `PsramDma` buffer while it runs.

Step 3 verified on the board: the read commands on the Raspberry Pi card; then `mkfs -t exfat -y /dev/sd0` (a 30 GB exFAT partition, mounted again at `/sd0/p0`), and `mkdir -p`, `touch`, `cp -r`, `mv`, `rm -r`, `rmdir` and long UTF-8 names (`Grüße aus Zürich ✓ 日本.txt`) on it. Write throughput and pulling a card during a write wait for redirection (step 4), the first way to put data into a file.

Step 3 measured: the image grew by 40.6 KB (342.7 → 383.2 KB), now that the commands use all of FatFs: `bmds_fs` is 35 KB in all (FatFs about 28 KB, in line with the 30 KB estimate; `f_mkfs` alone 4.5 KB), the shell grew 13.5 KB. No new internal RAM.

Step 2b measured: the image grew by 10.9 KB (331.7 → 342.7 KB); the linker still drops the FatFs functions nothing calls yet (reading, writing, `f_mkfs`), so step 3's commands will show its real size. Internal RAM: 1,044 bytes of `.bss`: FatFs's open-object table (`FF_FS_LOCK 16`) 256 B, the device registry (16 entries) 320 B, the mount table 288 B, the partition pool, the volume pool and the storage lock. Moving the registry and mount table to `Psram` at boot would save about 600 B (an open item). Per mounted volume the `FATFS` object (~600 B) is `Psram`; per open file ~600 B and per open directory ~1 KB, `Psram`.

## 9. ABI impact

- **On-disk:** FAT12/16/32 and exFAT as written by FatFs; MBR and GPT partition tables as read here.
- **Names:** device names (`sd0`, `sd0p0`) and nodes (`/dev/sd0`), the default mount paths (`/sd0/p0`), `/sd0/p0/autoexec.sh`. Later directory conventions (`bin`, …) belong to the Phase 3 design.
- **Shell:** the new commands, their options and output formats (§5); `cd`/`pwd`; file redirection; globbing; `rz`/`sz` behavior.
- **Errors:** `Error::NoMedia` and `Error::NoFilesystem` appended.
- The VFS API is kernel-internal; the Phase 3 file syscalls will be the frozen surface.

## 10. Testing

- **Host:** FatFs + VFS on `RamBlockDevice`: create, read, write, seek, truncate, rename, directories, long and UTF-8 names, full disk, `mkfs` both types, stale handles after an unmount; the shell file commands, redirection and globbing through the run corpus (the bash check runs those scripts in a temporary directory, so they're written relative to the cwd).
- **Board:** card in at boot, inserted and removed while running (with a file open, while writing), `sd` info, throughput (read/write MB/s, direct and bounce paths), FAT32 and exFAT cards from a PC, YMODEM both ways against `lrzsz`.

## 11. Decisions (agreed)

1. **Vendor FatFs with exFAT** rather than IDF's `fatfs` component (§3): exFAT is a must for today's cards.
2. **Unix paths** (`/sd0/p0/dir/file`), not drive letters.
3. **YMODEM via `lrzsz`** for serial transfer, for now.
4. **A Linux-style system clock** ([`time.md`](time.md)): a software clock in the kernel (UTC), set and disciplined by sources (`date -s` now; NTP/PTP later), surviving resets through the RTC domain; time zones are presentation.
5. **IDF's check messages stay in development builds.** `CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT` would save 16 KB image-wide (measured); release builds will set it (`docs/BUILDING.md`, Release Builds).
6. **No singletons; numbered devices and partitions** (§4): `/dev/sd0`, `/dev/sd0p0`, partitions mounted at `/sd0/p0` (numbered from 0, as devices are); a run-time mount table with automatic mounting and `mount`/`umount`.

## 12. Open questions

- UHS-I (1.8 V, SDR50/SDR104): worth it for a DOS-like machine? Measure high speed first.
- Read-ahead or write-back caching above FatFs: measure first; FatFs already transfers whole-sector runs directly.
- ~~**A sector cache for directories and the FAT.**~~ Done in Phase 3 (§3): 64 sectors, write-through. `ls -l` of BusyBox's `bin` 2.36 → 0.30 s, BusyBox's own `ls -l` 4.22 → 0.75 s.
- Commands ignore write errors, so `echo x > f` on a full or pulled card succeeds silently. GNU tools report `write error: No space left on device` and fail; `CommandContext::print()`/`write()` should remember the first error and the built-ins return 1.
- The removal of a card is noticed only after the write in flight times out (about 4 s): shorter IDF timeouts, or the card-detect interrupt failing transfers early, would shorten it.
- ~~A `/ram` RAM disk on `RamBlockDevice` for scratch files and tests on the board.~~ Done in Phase 3 step 6: `ram0` at `/ram0/p0`, sparse (`programs.md` §6).
