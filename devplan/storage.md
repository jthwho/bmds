# Storage: SD Card, Filesystem, Files in the Shell

Status: **steps 1, 2a, 2b done** (SD driver with hot-plug; the clock; FatFs, the VFS, numbered devices and partitions, a run-time mount table with automatic mounting and `mount`/`umount`); steps 3-5 not started (Phase 2).

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

## 5. Shell

### Commands (formats are an interface: proposals here, fixed when they ship)

| Command | Notes |
|---|---|
| `ls [-l] [-a] [-1] [path...]` | Columns at the terminal width (`COLUMNS`), one per line into a pipe. `-l`: `<type><r><w>  <size>  <yyyy-mm-dd hh:mm>  <name>`, e.g. `-rw-     1234  2026-10-03 14:02  notes.txt`, `drw-        0  …  docs/`. `-a` shows dot and hidden/system files. Colored by type when `colorDepth()` allows. |
| `cd [dir]`, `pwd` | Built-ins: `PWD`, `OLDPWD`, `cd -`; `cd` alone goes to `$HOME` (`/sd0/p0`). |
| `cat`, `head`, `wc`, `grep` | Take file arguments (they read stdin today). |
| `cp [-r] src... dst`, `mv src... dst` | `mv` within a volume renames. |
| `rm [-r] [-f] path...`, `mkdir [-p] dir...`, `rmdir dir...`, `touch file...` | |
| `df [-h]` | `Filesystem  Size  Used  Avail  Use%  Mounted on`, as coreutils prints it. |
| `hexdump [-C] [file]` | `-C` is the canonical hex+ASCII layout. |
| `sd [bench [megabytes]]` | Card info, one `Key: value` line each: `Card:` name and type (SDSC/SDHC/SDXC/MMC), `Capacity:` GB and sectors, `Bus:` width and clock, `Maker:` CID manufacturer, OEM, revision, serial, date. No card (or an unusable one): a message on stderr, status 1. `bench` reads from the start of the card, direct and through the bounce buffer, in 64 KB and 512 B requests, and prints MB/s. |
| `mkfs [-t fat32\|exfat] /dev/sd0[pN]` | Formats a whole card (a new partition table with one partition) or one partition (asks for `-y` to confirm). |
| `mount`, `mount DEVICE PATH`, `umount PATH` | Without arguments, the mounts, one per line, as on Unix: `/dev/sd0p0 on /sd0/p0 type exfat`. With them, mount or unmount by hand. |

### Language

- **File redirection:** `<`, `>`, `>>`, `>|`, `<>`, `&>` open real files (relative to the cwd). `set -C` (noclobber) is added with `>|`.
- **Globbing** (pathname expansion): unquoted `*`, `?`, `[...]` in a word expand to the sorted matching paths, component by component, using `Glob`; a leading `.` must be matched explicitly; no match leaves the word as it is (bash's default). Quoted text never globs (the expander already marks it).
- **Tilde:** `~` and `~/...` expand to `$HOME`.
- **File tests:** `test -e -f -d -s -r -w -x -nt -ot` on real files (`-x`: directories and `*.sh`).
- **`source file [args]`** / **`. file`**: run in the current shell. **`sh file [args]`**: run in a subshell with its own positional parameters. (Running `./script.sh` directly, with `#!`, comes with the Phase 3 program model.)
- **`/sd0/p0/autoexec.sh`:** after the first mount attempt (or a timeout), the console shell sources it if it exists. Ctrl-C interrupts it, as anything else.
- **Path completion:** Tab completes file and directory names in argument position (directories get a trailing `/`).

## 6. Files over the serial cable: YMODEM

- **`rz`** (receive into the cwd) and **`sz file...`** (send) speak **YMODEM** (batch, file names and sizes, 1 KB blocks, CRC-16), compatible with `lrzsz`. From picocom: `C-a C-s` runs `sz` on the PC; minicom and tio have the same.
- During a transfer the console is in raw mode: `TermMode::signals` off (a 0x03 byte is data, not Ctrl-C) and `nlToCrNl` off. Ctrl-C can't abort it, so YMODEM's own cancel (CAN CAN) and a timeout do.
- The receiver drains the console while it writes to the card, but the USB RX ring (1 KB today, no flow control) must hold a whole 1 KB block plus slack: **`CONFIG_BMDS_USJ_RX_BYTES` goes to 4 KB** (`Psram`).
- Expected speed: YMODEM waits for an ACK per 1 KB block; at USB full speed's 1 ms frames, a few hundred KB/s. ZMODEM (streaming) is the upgrade path if that's too slow.

## 7. Phasing

| Step | Delivers |
|---|---|
| 1 | `BlockDevice`, `RamBlockDevice`; `SdCard` (LDO 4, power, card detect, hot-plug task, high speed, bounce buffer); `Error::NoMedia`; `sd` command; a read benchmark (`sd bench`). |
| 2a | The system clock ([`time.md`](time.md)): `Clock`, `TimeZone`, the reset record, `date`, `clock`. **Done.** |
| 2b | FatFs vendored (`thirdparty/fatfs`, our `ffconf.h`, port in `fatfsport.cpp`); `bmds_fs` VFS, files, `Directory`; the block device registry (`sd0`), partition tables (`sd0p0`), the run-time mount table, automatic mounting (`/sd0/p0`), `mount`/`umount`; `/dev` nodes; `Error::NoFilesystem`; host tests on `RamBlockDevice`. **Done.** |
| 3 | Shell: `cd pwd ls cat cp mv rm mkdir rmdir touch df hexdump mkfs`; text commands on files. |
| 4 | Language: file redirection, globbing, tilde, file tests, `source`/`.`/`sh`, `autoexec.sh`, path completion. |
| 5 | `rz`/`sz` (YMODEM); 4 KB RX ring. |

## 8. Resource budget (estimates)

| Item | Flash | RAM |
|---|---|---|
| IDF SDMMC host + protocol + GPIO driver (**measured**) | 37 KB code + 11 KB of IDF's log and assert strings | 1.7 KB IRAM (cache sync, GPIO ISR, ISR-safe FreeRTOS calls); 0.8 KB internal heap at boot (host state, DMA descriptors, event queue, TCB) |
| `SdCard` + storage task + `sd` (**measured**) | 4.3 KB | 311 B `.bss`; 6 KB `Psram` stack (~400 B used idle); 4 KB `Psram` bounce buffer; `sdmmc_card_t` in `Psram` |
| FatFs (LFN, exFAT, mkfs) | ~30 KB | `FATFS` ~600 B + LFN buffer 512 B `Psram`; ~600 B `Psram` per open file |
| VFS, `File`, `Directory` | ~6 KB | — |
| Shell file commands, globbing, redirection | ~12 KB | — |
| YMODEM | ~4 KB | 1 KB block buffer while transferring; RX ring 1 → 4 KB `Psram` |
| **Total** | **~110 KB** (the IDF SD stack is three times the estimate) | **~3 KB internal** |

Step 1 measured on the board (32 GB SDHC, 4-bit, 40 MHz): sequential reads of 64 KB **16.3 MB/s direct**, 6.9 MB/s through the 4 KB bounce buffer; 512 B requests 1.3 MB/s on either path (about 0.38 ms of command overhead each, which is also what limits the bounce path: a larger bounce buffer moves more per command). The card is ready 120 ms after boot (50 ms debounce, 30 ms power cycle, init). Card init uses about 1.8 KB of the storage task's stack.

Step 1 measured: the image grew by 58 KB (200 → 258 KB flash), internal RAM free at boot fell by 2.9 KB (610,895 → 607,983 bytes).

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
- A `/ram` RAM disk on `RamBlockDevice` (cheap once it exists) for scratch files and tests on the board.
