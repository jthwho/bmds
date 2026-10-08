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

**Faster lookups (design, 2026-10-08; the user asked for "as much efficiency" as can be had).**
Measured on the exFAT card:

| What | Time |
|---|---|
| A one-sector read (`sd bench`, 256 of them) | 390 µs each |
| A sector in 64 KB reads | 32 µs |
| `du -s` of the icon theme's `32x32` (361 entries, about 85 sectors of directory) | 3.81 s, 10.5 ms a lookup |
| `ls -l` of it | 3.36 s |
| BusyBox `cp -r` of `16x16` (361 files) to `ram0` | 16.5 s |
| The file browser's copy of `share` (2,508 items) to `ram0` | 119 s |

The cache holds 64 sectors, and a lookup scans the folder from its start. A folder bigger than the
cache evicts its own first sectors before the next lookup reaches them: least recently used is the
worst policy for a repeated scan. So each lookup reads about half the folder from the card, one
sector a command. The RAM disk's metadata shares the cache too, though reading it costs no more
than the copy.

- **Read-ahead for metadata.** A window miss reads the sector and up to 7 after it (4 KB, not past
  the device's end) in one command, into a buffer aligned for DMA so it isn't bounced. Every sector
  of the run is kept. Directories and the FAT lie in runs, so a scan needs an eighth of the
  commands, at about 0.6 ms a run instead of 3.1 ms for eight single reads. Coherence is as now:
  every write goes through `disk_write()`, which updates the copies it covers.
- **A bigger cache that finds a sector in O(1).** 256 sectors (128 KB of `Psram`, 0.4% of it),
  found through a hash of drive and sector. That holds a folder of about a thousand files on exFAT,
  where an entry set takes three or four 32-byte entries.
- **Scan resistance.** A segmented LRU: a sector read enters a probation list; a hit moves it to a
  protected list of at most three quarters of the slots, whose oldest drops back to probation.
  Room is taken from probation first. A folder bigger than the cache then churns probation and
  leaves the FAT and the folders in use alone.
- **Memory devices aren't cached.** `BlockDevice::isMemory()` (false by default; true for
  `RamBlockDevice`, forwarded by partitions). Reading a RAM disk costs a copy, the same as the
  cache, so caching it only took room from the card.
- **Measured after each change**, with the same commands.

*As built (2026-10-08):* as above, then two more findings.

- **FatFs's lookups were most of what was left.** A host profile under callgrind of 300 long names
  on a FAT RAM disk, then on the board:
  - **exFAT:** `dir_find()` loaded and checked the sum of every entry block before the one sought,
    then compared the name hash. A BMDS patch (`thirdparty/fatfs/patches/bmds-exfat-find.diff`)
    reads the hash in the window first and passes over a block that doesn't match. A block not
    plainly whole in one sector is loaded as before.
  - **FAT:** long names are compared a character at a time, each folded by `ff_wtoupper()`'s table
    walk. A second patch (`bmds-ascii-upper.diff`) answers ASCII at once.
  - **The RAM disk:** every sector read asked `isPresent()` and `sectorCount()` at the partition
    and at the disk, each under the disk's mutex: about 40% of the profile. They are atomics now;
    the read itself still takes the mutex.
- **`time CMD > FILE` redirects `time`'s output, not the command's.** `time ls DIR > /dev/null`
  timed 361 names written to the console (0.58 s); `ls` itself takes about 17 ms. The table below
  uses commands whose output is one line.
- **Results** on the exFAT card, the same firmware but the cache:

  | What | Before | Read-ahead, 64 sectors | 256 sectors | And the patches |
  |---|---|---|---|---|
  | `du -s` of `32x32` (361 entries) | 3.72 s | 1.49 s | 0.58 s | **0.23 s** |
  | `cp -r` of `16x16` (361 files) to `ram0` | 15.8 s | 9.5 s | 3.4 s | **1.4 to 1.9 s** |
  | `cp -r` of it within `ram0` (FAT16) | 2.84 s | | | **1.53 s** |
  | `du -s` of it on `ram0` | 0.48 s | | | **0.27 s** |
  | `rm -r` of it on `ram0` | 1.78 s | | | **0.97 s** |
  | The file browser's copy of `share` (2,508 items) to `ram0` | 119 s | | | **21 s** |

  Copies and removals within the card barely changed: `cp -r` 5.4 to 3.7 s, `rm -r` 1.6 s, for 361
  files. What is left there is small writes, about 10 ms a file, which a write-back cache of
  metadata would batch; that trades what a pulled card or a power cut loses, so it is the user's to
  decide (§12).
- **Host tests:**
  - a folder of 2,000 names bigger than the cache, on another disk, read through once, leaves a
    folder in use cached (no reads);
  - every one of those names is found (16-bit hashes collide among 2,000), a missing one isn't,
    and case is folded;
  - a remount's listing reads ahead (at least four sectors a request).
  - The whole suite also passed under ASan and UBSan. That found two old faults, fixed: the
    littlefs test kept a file's config on a returned stack frame, and `TerminalScreen::setTitle()`
    passed a null pointer to `memcmp()` for an empty title.
- **Memory:** 128 KB of sectors, 4.5 KB of tags and 4 KB for the run read ahead, all `Psram`
  (33 KB before). Static RAM: the cache object is 36 bytes (12 before). `bmds_fs` is 89,616 bytes of
  code (+1,494).

**Writing metadata back (design, 2026-10-08; the user chose an ordered write-back with a 1 s
window over write-through).** A file copied within the card costs about 10 ms: four single-sector
writes (the directory entry at its making, the allocation bitmap or FAT, the data, the entry again
at its close), each about 2 ms on this card; removing one costs two. In a burst most of those land on
the same sectors: a bitmap sector maps 4,096 clusters, a directory sector holds about five exFAT
entry blocks.

- **What is held:** writes from FatFs's window (directories, the FAT, exFAT's bitmap, FAT32's FSInfo
  and the boot sectors) to a device that isn't memory, on FAT16, FAT32 and exFAT. File data, which
  never passes through the window, is written at once, so it always reaches the card before the
  metadata that points at it. FAT12 volumes (floppy-sized) and `f_mkfs()`, whose buffer isn't the
  window, stay write-through.
- **Where:** in the metadata cache, on a list of its own that making room never takes from, at most
  64 sectors. Reads find the newest copy there. A data write over a held sector (a freed directory
  cluster used again for data within the window) supersedes it: the device has the newest bytes.
- **When it is written:**
  - a second after the first sector was held, by a flusher thread (a kernel thread started at boot,
    its stack in PSRAM; it sleeps on a semaphore until a write is held: no polling);
  - when 64 are held;
  - before an unmount (`umount`, a card going), a format and a remount;
  - on `Vfs::sync()`: the new shell command `sync`, a new system call (slot 66, `int32_t sync(void)`,
    which the Linux layer's `sync`, `syncfs`, `fsync` and `fdatasync` use), and `reboot`.
  - Not at a file's close: FatFs syncs at every close, and that would undo the batching.
- **In an order that survives a cut**, so a power cut or a pulled card during a flush leaks space at
  worst. A directory entry never points at clusters marked free. A flush writes in three phases:
  1. the allocation sectors (FAT and bitmap) with every cluster used either before or now marked
     used: a bitmap sector OR'd with its original, a FAT entry freed now kept as it was;
  2. the directory sectors;
  3. the allocation sectors as they are, freeing what was freed; then FSInfo (a hint) and the boot
     sectors.
  The original of each held allocation sector is kept for phase 1, from the cache's clean copy
  (FatFs always reads a FAT or bitmap sector before changing it), in a pool of 64 sectors.
- **What a cut still costs:** the changes of the last second, as if they hadn't been made; space
  allocated in a flush cut before phase 3 stays marked used until a PC's disk check. A rename is two
  directory writes, as it is now.
- **A failed flush** (the card gone) drops what was held: the error comes after the operation that
  made the change returned, so the next `Vfs::sync()` reports it (as POSIX's `fsync` reports a failed
  write-back).

*As built (2026-10-08):* as above, with these differences and findings.

- **A cluster freed and used again within the window** was the old file's on the card still: a write
  into it (data, or a new folder's zeroed sectors) before the free reached the card left the old
  entry pointing at the new bytes, a cross-link, if cut there. The cut test below found it at its
  first write. Now each held allocation sector keeps a mask of the entries freed since the last flush
  (sticky, even if allocated again), and a write into such a cluster, held or direct, flushes first:
  the removal reaches the card, in order, before the cluster is reused. The check is a hash lookup per
  cluster written, and only while some held sector frees something.
- **A broken exFAT entry block** (one whose sectors were cut apart: a new file's entry in one sector,
  its name in the next) made FatFs end the directory there with `FR_INT_ERR`. Write-through can leave
  one too. A FatFs patch (`bmds-exfat-broken.diff`) passes over it, as Windows does; the file in it is
  lost and its space leaked. The same test found that the lookup patch above trusted a block's count
  of secondary entries when skipping it, and so skipped a block made after a broken one (a file listed
  but not found); the skip now stops at an entry that isn't a secondary.
- **No logging:** `bmds_fs` is portable and the kernel's log isn't built for the host; a lost
  write-back is reported by the next `sync` instead.
- **The flusher is started at boot** (`Fat::init()`), because a thread started from a program's task
  belongs to the program and would end with it. `Thread::sleep()` is new (FreeRTOS's delay, or the
  host's).
- **The system call:** `bmds_sync()`, slot 66; the Linux layer's `sync`, `syncfs`, `fsync` and
  `fdatasync` use it (they returned 0). `reboot` syncs before restarting.
- **Host test** (`Vfs: metadata written back survives a cut at any write`): on FAT32 and exFAT, a burst
  removing ten files, filling and emptying a folder of 30 long names, and making twenty files, synced
  at four different points. Every write it makes is recorded; then for each prefix of them (a cut
  after each write) the card is mounted fresh and checked: every listed file opens and reads back
  right (a new one may be shorter), and a 16 KB file written afterwards overwrites none of them (no
  cross-link). About 3,000 cuts in 27 s. With the skip fix reverted it fails at cut 234; before the
  mask, at cut 1.
- **On the board** (the exFAT card, 361 files of the icon theme):

  | What | Write-through | Written back |
  |---|---|---|
  | `cp -r` within the card | 3.7 s | 2.3 s |
  | `rm -r` | 1.6 s | 0.82 s |

  Less than estimated: `rm -r` is now two lookups a file (0.64 ms each in that folder), and each copied
  file still writes its data at once. A file written and `reboot` at once is there after. Three hard
  resets right after a burst of changes left the card mounting and every file whole: the last
  second's changes gone, one new file empty (made, its size not yet written).
- **Memory:** 64 KB of `Psram` for the originals and the masks; the flusher's control block and stack
  (internal RAM free after boot 556 bytes lower; 4 KB of `Psram`).

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
- `/dev/null` and `/dev/console` (the caller's terminal) are device nodes too, so `/dev/null` stops being a special case in the shell. **`/dev/zero`** (reads fill with zeros) and **`/dev/urandom`** and **`/dev/random`** (reads come from the kernel's random pool, `random.md`: ChaCha20 keyed from a pool seeded by the chip's noise source at boot, `/boot/random-seed` and the hardware generator on every read; the generator alone gives about 60 KB/s, busy-waiting, and 1 MB from it set off the task watchdog; never waiting, as Linux's `/dev/random` no longer does once seeded; at most 64 KB a read) join them; writes to all three vanish. They are streams without state shared by every caller (4 bytes each), listed in `/dev` before the block devices. Block device nodes can't be opened as streams yet (raw access is a later need); `mkfs` names them.

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
| Metadata cache (**measured**; 2026-10-08: 256 sectors, read-ahead, hashed) | 2.0 KB | 36 B `.bss`; 256 sectors, tags and a read-ahead run, 137 KB `Psram` from boot |
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
- ~~Read-ahead above FatFs.~~ Done for metadata (§3, 2026-10-08): runs of 8 sectors on a miss.
- ~~Write-back for metadata.~~ Done (§3, 2026-10-08, the user's choice): held up to a second, written
  in an order a cut can't turn into a cross-link.
- ~~**A sector cache for directories and the FAT.**~~ Done in Phase 3 (§3): 64 sectors, write-through. `ls -l` of BusyBox's `bin` 2.36 → 0.30 s, BusyBox's own `ls -l` 4.22 → 0.75 s.
- Commands ignore write errors, so `echo x > f` on a full or pulled card succeeds silently. GNU tools report `write error: No space left on device` and fail; `CommandContext::print()`/`write()` should remember the first error and the built-ins return 1.
- The removal of a card is noticed only after the write in flight times out (about 4 s): shorter IDF timeouts, or the card-detect interrupt failing transfers early, would shorten it.
- ~~A `/ram` RAM disk on `RamBlockDevice` for scratch files and tests on the board.~~ Done in Phase 3 step 6: `ram0` at `/ram0/p0`, sparse (`programs.md` §6).

## 13. `/boot`: a persistent partition on the flash (design, 2026-10-05; built)

*Its format changed on 2026-10-07: littlefs through an ordered write cache instead of FAT on
wear levelling (§13d). The rest of this section is the FAT design as it was built.*

**Goal** (the user's): files and settings that persist without a card, on a partition of the
board's flash that is **always** mounted at `/boot`. `autoexec.sh` moves there: the system starts
the same with or without a card, and a script that wants the card checks for it itself.

**Facts:** 16 MB of flash; the table has `nvs` (24 KB at 0x9000) and the 4 MB factory app at
0x10000 (the firmware is 597 KB today), so about 11.9 MB is free. The VFS mounts FAT on any
`BlockDevice` through FatFs, with 512-byte sectors (`FF_MAX_SS`). ESP-IDF's `wear_levelling`
gives a sector device over a flash partition (IDF's own flash FAT is FatFs on it); `spiffs` has no
directories; LittleFS isn't in IDF (a managed component).

**Decisions (proposed):**
- **FAT on IDF's wear levelling.** A `FlashBlockDevice` (`bmds_storage`) puts the `BlockDevice`
  interface over `wl_read`/`wl_write`/`wl_erase_range` with 512-byte sectors in wear levelling's
  safe mode (a 4 KB flash sector is rewritten through a spare, so a power cut doesn't lose the
  sector). The VFS mounts it as it mounts a card: one file system implementation (FatFs) for
  everything, the same names, links and tools, and little new code. FAT's own metadata isn't
  power-safe (a cut during a write can leave a lost cluster or a stale directory entry, as on a
  card); settings are written rarely, and FatFs mounts what it finds. If that proves not enough,
  LittleFS (power-safe by design, a second implementation in the VFS) is the alternative.
- **The partition:** `boot, data, fat, 0x410000, 4M` in `partitions.csv`, after the app. That
  leaves 7.9 MB for later (a second app slot for updates over the air, 4 MB, or a larger `/boot`).
  Changing the table means flashing it once (`idf.py flash` writes it); `nvs` keeps its place.
- **Names, no singletons:** the device is `flash0` (`/dev/flash0`: the partition named `boot`,
  through wear levelling), mounted at `/boot` (not `/flash0/p0`: the path is the point, and fixed
  as `/sd0/p0` isn't). The mount is made at boot, before the shell, and can't be unmounted.
- **Two scripts, as Unix has them** (agreed with the user, 2026-10-05): `/boot/autoexec.sh` runs
  once, in the first shell, at startup (what is done once: `display start`, `win start`...);
  `/boot/profile` is read by **every** interactive shell as it starts (the serial console's, each
  terminal window's), so the environment set there (`HOME`, `PATH`, the prompt) reaches every
  shell. A variable set in `autoexec.sh` stays in the first shell, as on Unix. The shell's
  built-in defaults stay as fallbacks (`HOME=/boot`, `PATH=/bin:/sd0/p0/bin`), so a missing or
  broken profile still gives a working shell.
- **First boot:** a partition without a file system is formatted (FatFs's choice by size: FAT12),
  and the two scripts written. `/boot/profile`:

  ```sh
  # /boot/profile: read by every interactive shell as it starts.
  export HOME=/boot
  export PATH=/bin:/sd0/p0/bin
  ```

  `HOME` on the card instead is one line: `export HOME=/sd0/p0`, or only when there is a card:
  `[ -d /sd0/p0 ] && export HOME=/sd0/p0`. `/boot/autoexec.sh` keeps today's behavior:

  ```sh
  # /boot/autoexec.sh: run by the first shell at startup.
  # Run the SD card's autoexec.sh too, if there is a card.
  if waitmount /sd0/p0 3000; then
          [ -f /sd0/p0/autoexec.sh ] && . /sd0/p0/autoexec.sh
  fi
  ```

- **Startup:** the first shell sources `/boot/autoexec.sh` at once; it no longer waits for a card
  (`Shell::Startup::wait` and `Storage::waitForFirstMount` in `main` go). `waitmount PATH MS` (a
  new shell command) waits up to `MS` for a file system at `PATH` and fails if none comes, so
  scripts wait for the card only when they want it.
- **`HOME`** becomes `/boot` (it is `/sd0/p0`, chosen because `autoexec.sh` lived there), and
  `PATH` `/bin:/sd0/p0/bin` (the built-ins first: a program on the card can't take a built-in's
  name; the order is the profile's to change).
- **The firmware's image** doesn't touch the partition: reflashing the app keeps `/boot`. A later
  `idf.py` target can build a FAT image from a directory of defaults (`fatfsgen.py`) for a fresh
  board.
- **Defaults from the repository** (2026-10-06, `gui.md` G14): the files of `boot/` (the two
  scripts, `config/panel.conf`, `applications/terminal.desktop`) are embedded in the firmware
  (`tools/embedfiles.py`) and installed at each boot: a default not on `/boot` and not listed in
  `/boot/.defaults` is written and listed (one is already there: listed, kept). So a new
  release's defaults reach a `/boot` made before it, and a default the user deletes stays
  deleted (its line removed from `.defaults` brings it back). Writing happens once per new
  default (1.0 s for two files and the list); otherwise the list is read (4.3 ms for the stage).

**ABI impact:** the path `/boot`, `/boot/autoexec.sh` and `/boot/profile` and their roles,
`waitmount`, the defaults of `HOME` and `PATH`. Recorded
with an `ABI:` line.

**Budget (estimate):** code for the block device and the wear levelling (IDF's: a few KB of flash);
RAM: the FatFs volume (about 1 KB, internal) and wear levelling's state (a few hundred bytes plus a
sector buffer). The partition is 4 MB of flash.

**Testing:** host: the shell reading the profile at start (interactive shells only; `sh -c` and
scripts don't), its variables reaching the session, a missing or failing profile leaving the
defaults. Board: first boot formats and writes the default scripts; `HOME` changed in the profile
reaching a new terminal window's shell; files written survive a reset and a reflash of the
app; `df /boot`; the boot with and without a card (the SD's script runs, or the wait times out
after 3 s and the system is up without it); a power cut during writes, a few times, then a mount
and a read of everything (the measure of whether FAT on wear levelling is enough).

**As built** (2026-10-05):
- **`BootDisk`** (`bmds_storage`): the boot stage `boot`, after `vfs`. `FlashBlockDevice` over
  `wl_read`/`wl_write`/`wl_erase_range`; `Vfs::format()` on the whole device (a partition table
  with one partition, which FatFs finds by itself when mounting the whole device) and `Vfs::mount()`
  at `/boot`; `Vfs::pin()` makes `umount` refuse it (`Busy`). `Vfs::isMounted()` and
  `Vfs::mountWaiters()` (a `WaitList` released after each mount and unmount) serve `waitmount`.
  (Since 2026-10-07 `waitmount` also stops when nothing is on its way: `sysconf.md` §13.)
- **Two findings on the board:**
  - **A flash operation needs an internal stack.** It turns off the cache PSRAM is reached through,
    and IDF asserts (`esp_task_stack_is_sane_cache_disabled`) when the calling task's stack is in
    PSRAM: the shells', programs' and the storage task's are. The device runs an operation on the
    caller when its stack is internal, else hands it to its worker task (`flash0`, 3 KB internal
    stack) and waits. Buffers in PSRAM are fine: IDF's flash driver bounces them.
  - **Each 512-byte write costs a whole flash sector.** Wear levelling's 512-byte mode erases and
    rewrites the 4 KB sector for every sector written (twice in safe mode): creating a tiny file
    took 0.95 s, writing 8.9 KB (`seq 1 2000`) 5.22 s. The device now keeps a write-back cache of
    one 4 KB block (in PSRAM): a block's sectors are flushed together, in one aligned erase and
    write (wear levelling erases an aligned flash sector without its read-modify-write), on
    `sync()` (FatFs's, at each close), when another block is needed, or after a second without
    writes. Measured: a tiny file 0.30 s, an append 0.15 s, `seq 1 2000` 0.80 s, removing three
    files 0.45 s (1.56 s before).
- **Verified on the board:** the first boot formatted `/dev/flash0` (4040 KB) and wrote the two
  scripts; the boot then ran `/boot/autoexec.sh`, which waited for the card and ran its
  `autoexec.sh` (the desktop and a terminal came up as before); `HOME` and `PATH` as the profile
  sets them; `df` shows `/dev/flash0` at 3.9 MB; `umount /boot` refused; `waitmount` returns 0
  for a mounted path, 1 after the timeout (10.04 s for 10 s), and ends at once on Ctrl-C; a file
  written survived resets and a reflash of the app. Three resets in the middle of a
  large write (`seq 1 200000`): `/boot` mounted each time, the other files whole, the file being
  written empty (FAT records it at its close), no space lost. A chip reset isn't a power cut (the
  flash chip finishes an erase under way), so pulling the power during writes is still to be done
  by hand.
- **Writes blinked the screen** (the user saw it): each one shut out the scan's interrupt for longer
  than a frame. The scan's interrupts are now IRAM-safe (`display.md` §5c''): 0 underruns since.
- **Size:** `/boot` uses 3 KB of 3,984 KB with the scripts and a note. The worker's 3 KB stack and
  the cache's 4 KB (PSRAM) are the RAM it costs, with FatFs's volume.

### 13a. Writing `/boot` in the background (the user's request, 2026-10-06)

**Why:** a close on `/boot` waited for the flash. FatFs syncs the device at each close, and a
small file touches the FAT, its directory and its data, each usually in another 4 KB block: the
one-block cache flushed (erase and write, about 0.1 s each, twice in safe mode) as each next
block was needed and at the sync. The panel saving where an icon was dropped held its lock for
that long (0.35 s), so the desktop wasn't drawn and the pointer left a trail. The card isn't the
answer (settings must not need it); a cache in the kernel that writes in the background is.

**Design (`FlashBlockDevice`):**
- **Eight cached blocks** (32 KB of PSRAM), the least recently used reused. A write goes into the
  cache (a block not there is read in first, unless written whole); reads take cached blocks
  from it, others from the flash (reads don't fill it: FatFs's metadata has its own cache, §3).
- **`sync()` doesn't wait:** it asks for the dirty blocks to be written 100 ms from then (a
  close's syncs and the next file's coalesce); a dirty block not synced is written after a second
  of no writes, as before. The worker (`flash0`) writes them, a block at a time, taking the
  device's lock for each, so readers and writers come between. A block needed while all eight
  are dirty is made room for by writing the oldest at once, as before.
- **Every write and sync goes through the worker** (it keeps the deadline), reads only when the
  caller's stack is in PSRAM (as before: a flash operation needs an internal stack).
- **Durability:** what was written is on the flash within about 0.1 s of its close (1 s without
  one). A restart (`reboot`, `esp_restart()`) writes everything first (an IDF shutdown handler);
  a power cut or a crash in that window loses it, as the last write on a card without its sync
  would; FAT's metadata was never power-safe here (§13).
- **Seen 2026-10-07: a reset in the middle of a block's write loses the whole 4 KB sector,**
  other files in it included. A block is erased, then written (`writeOut()`), and wear
  levelling's safe mode keeps only its own state. A hardware reset about a second after a
  flash came while the boot stage was installing a new default (`.defaults` rewritten). After
  it, `.defaults` and `applications/terminal.desktop`, which shared its sector and wasn't being
  written, were both all 0xFF. Deleting both and rebooting reinstalled them. `/boot` is
  written at every boot (`random-seed`, 5 s in), so a reset then risks the same.
  - The fix to decide: write each block to a spare sector first, with a sequence number and
    a checksum, and copy it into place after. On mount, finish a copy that was cut off. That
    is twice the flash writes per block. FAT's sectors (the FAT itself, directories) have the
    most to lose.
- **Seen again 2026-10-07: three directories' first clusters erased.**
  - **What was found:** `/boot/config`, `/boot/desktop` and `/boot/applications` each listed 32
    nameless entries before their real ones.
  - **The image:** saved with `esptool read_flash 0x410000 0x400000` as
    `build/boot-damaged-2026-10-07.img`, kept for this work.
  - **What it shows:** each directory's first cluster (clusters 7, 11 and 17; FAT12, 1 KB
    clusters) is all 0xFF, with `.`, `..` and the first entries in it. Entries in the clusters
    chained after it survive, which is why `ls` showed garbage, then the rest.
  - **Context:** every file there is dated 16:00 (the clock unset). `/boot/.trash`, made
    later, is intact.
  - **Found (2026-10-07): cut-off block writes, not a fault in how a directory is made.** The
    image was decoded through wear levelling's map (its state: dummy page 34, no moves yet; the
    FAT partition at sector 63 of the logical device, so logical page N is sector 8N):
    - **The directories were older than the damage.** `profile` and `autoexec.sh` hold clusters
      2 and 3, from the first format, so `/boot` wasn't made again. Clusters 18, 19, 20 and 22
      are allocated in the FAT but in no directory: an old `desktop/terminal.desktop`,
      `wm.conf`, `clipboard.conf` and `desktop.conf`, whose entries were in the erased
      clusters. The files listed now were installed again afterwards, into the clusters
      chained after the 32 0xFF entries.
    - **No `.` entry for clusters 7, 11 or 17 is anywhere in the partition,** so the
      directories' data wasn't written to the wrong place.
    - **Each damaged page holds a file written at boot.** Page 15 (clusters 6 to 9) holds
      cluster 6, an old 64-byte random seed now lost. Page 16 (10 to 13) holds `.defaults`
      (13). Page 17 (14 to 17) holds `random-seed` (15) and an older seed (14). `random-seed`
      is rewritten about 5 s into every boot, which also erases and rewrites the FAT's page and
      the root directory's page. A reset at that moment, as in the first case, loses whichever
      page was between its erase and its write. The other sectors in those pages were written
      again later, as files reused the space; the directory clusters never were.
    - **Wear levelling's own recovery isn't involved:** its transaction page (logical page
      1010) is erased, so nothing was pending.
  - **The fix:** §13b.
- **The flash still stops the caches while it erases or writes** (code and PSRAM out of reach,
  the other core paused; IDF yields every 20 ms of an erase). In the background, that is a short
  pause per block where nothing waits for the whole write. Flash auto-suspend (IDF, P4-capable)
  would remove it, but it is qualified per flash chip and marked "read docs first": not now.

**As built** (2026-10-06):
- Measured on the board: a tiny file 0.02 s (0.30 s before), an append 0.02 s (0.15 s),
  `seq 1 2000` into a file 0.05 s (0.80 s), removing a file 0.02 s (0.45 s). A file written just
  before `reboot` was there after it (the shutdown handler).
- **The pause remains, moved to the background:** `seq 1 300000 > /dev/null` took 5.16 s alone
  and with a loop writing ten small files to the RAM disk, 7.28 s with the same loop writing to
  `/boot`: about 0.21 s of the whole system stopped for each small file (its blocks erased and
  written twice, wear levelling's safe mode, the caches off). Nothing waits for it any more, but
  the screen can't be drawn while it lasts.
- **Auto-suspend isn't available for this flash:** the board's chip is a Winbond W25Q128
  (`ef 4018`); IDF's Winbond driver enables suspend for the W25Q64 (`ef 4017`) only. Enabling
  it would need our own chip driver (`SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST`) on an unqualified
  part: not done.

### 13b. Block writes that survive a reset (design, 2026-10-07; the fallback to §13c)

**Why:** §13a's two cases. A 4 KB block is erased, then written. A reset between the two
loses everything in it, including files that weren't being written. Wear levelling's safe mode
protects only partial-sector erases, which the cache never makes. `/boot` is written at every
boot (`random-seed`, and `.defaults` when there's a new default), so routine resets during
development hit it.

**Rejected alternatives:**
- **LittleFS for `/boot`:** designed to survive power loss. But it means a new on-disk format,
  a migration of the user's files, an outside component, and code size still to measure. Kept
  as the long-term option if FAT's other weaknesses start to matter.
- **A journal at the end of the device:** the FAT partition fills the logical device to its
  last sector, so it would need a smaller filesystem and a migration.

**Design (in `FlashBlockDevice`, under `/boot`'s existing format):**
- **Where the journal lives: the gap between the MBR and the partition.** FatFs puts the
  first MBR partition at sector 63 (`N_SEC_TRACK`), so logical pages 1 to 6 (sectors 8 to 55)
  belong to no partition. They are all 0xFF in the image: nothing has ever written there.
  GRUB's core image lives in the same gap on PCs. Wear levelling maps these pages like any
  other, so their wear is spread across the partition.
  - The device refuses writes there (`ReadOnly`), so a raw writer (§14's `fdisk`, a future
    `dd`) can't overwrite the journal.
  - If the table on the device doesn't leave pages 1 to 6 free, the journal is off. A line
    in the log says so, and the device writes as before.
- **Which writes use it:** only a block whose new contents need an erase. If the new data
  only clears bits of what is on the flash (`new & old == new`, as when a file is written into
  free, erased clusters), the block is programmed without an erase. A cut then leaves only
  the sectors being written unfinished, as on any disk.
- **How a journaled block is written:**
  1. Erase a journal data page, round-robin over pages 1 to 4, and write the new 4 KB there.
  2. Append a 32-byte record to the active record page (5 or 6) by programming only, with no
     erase. It holds a magic number, a sequence number, the target block, the data page, a
     CRC-32 of the data and a CRC of the record.
  3. Erase the target block and write it.
- **Record pages:** each holds 128 records. When the active one is full, the other is erased
  and becomes active. The newest record is always in one of the two, so a cut during that
  erase loses nothing.
- **On mount, before FatFs reads anything:**
  - Find the newest valid record (highest sequence number, both CRCs good).
  - If its data page still matches the data CRC and the target block differs from it, copy
    the data page into the block again.
  - Only the newest record can be unfinished, because blocks are written one at a time in
    order. Redoing it is harmless: rewriting the same contents changes nothing.
- **What it doesn't fix:** FAT's metadata still isn't updated atomically *across* blocks. A
  cut between the FAT's block and a directory's block can leave lost clusters, as clusters 6,
  14 and 18 to 22 are now. That wastes space but loses nothing else. A check that frees lost
  clusters at mount (`fsck`-like) can come later.
- **Cost:**
  - A journaled block costs one more 4 KB erase and write, plus a 32-byte program. §13a
    measured about 0.21 s of whole-system pause per small file, mostly FAT and directory
    blocks; those are journaled, so expect up to about double. Programmed-only blocks cost
    less than today.
  - The pause stays in the background (§13a's worker), so nobody waits for it.
  - RAM: none new. The cache's slot is the source; mount recovery uses one cache slot.
  - Wear: the journal pages are erased once per journaled block, and wear levelling spreads
    that over the partition.
- **Also:** `random-seed` is rewritten in place (opened without truncating), so a boot no
  longer reallocates its cluster or rewrites the FAT. Its directory entry's modification time
  still changes, so the root directory's block is still journaled.
- **Testing:**
  - On the host: the journal's logic over a simulated NOR flash that can be "cut" at any
    erase or program step. The test checks that after recovery every block is either old or
    new, never erased.
  - On the board: a loop that writes files, with `reboot` and a hardware reset at random
    moments, followed by a check of the tree.

### 13c. littlefs for `/boot`: measured (2026-10-07)

**Why:** §13b fixes erased blocks, but FAT's metadata still isn't atomic across blocks, and every
small file still costs several erases. littlefs (copy-on-write, power-loss safe by design, its
own wear levelling) might fix both. These measurements decide it; `/boot` isn't changed yet.

**Vendored:** littlefs v2.11.3 in `thirdparty/littlefs` (BSD-3-Clause), built with `LFS_NO_MALLOC`
(every buffer supplied) and its messages off. Its own code shadows variables, so `-Wshadow` is
off for its two files only.

**Power cuts, on the host** (`tests/host/littlefs.cpp`):
- **The simulator:** a NOR flash of 64 blocks. An erase sets 0xFF, a program can only clear bits,
  and either can be cut. A cut program leaves its first bytes written and one byte half done; a
  cut erase leaves garbage.
- **The workload:** files rewritten in place, replaced by renaming a new file over them, and
  removed. Sizes are inline (in the directory), one block, and chains of blocks. The test cuts
  at every flash operation in turn (about 600 points). After each cut it mounts, checks every
  file, carries on, remounts and checks again.
- **The result:** every file holds its last closed contents or the ones being written. littlefs
  never programmed a byte that wasn't erased. A one-off run 5 times longer, with two other seeds
  (about 1,400 cut points each), also passed.
- **littlefs's rules (from the test):**
  - A new file's entry is committed when it is opened with `O_CREAT`, and its data at close. A
    cut in between leaves it empty, as POSIX's `creat()` would.
  - A file that is rewritten keeps its old contents until it is closed. Renaming a new file over
    an old one is atomic.

**On the board** (`lfsbench`, a prototype command):
- **The setup:**
  - littlefs uses a scratch region of the flash past the last partition (0x810000, 4 MB, 1024
    blocks of 4 KB), so `/boot` wasn't involved.
  - The configuration: read and program sizes 16, caches 512 bytes, lookahead 128 bytes,
    `block_cycles` 500.
  - FAT was measured on `/boot` itself, in `/boot/.lfsbench`, which is removed afterwards.
- **How the pause is measured:** a loop running from flash on the other core counts while the
  files are written. The counts lost against an idle half second are the time the whole system
  was stopped.
- **Reads must be mapped:** with `esp_flash_read`, every read turned the caches off. littlefs
  reads in small pieces, so 60 file operations made 12,263 reads and 639 ms of pause. Reading the
  region through the cache (`spi_flash_mmap`, kept coherent by IDF's flush after erases and
  programs) cut the pause from 783 ms to 238 ms.

| 20 files, one operation every 300 ms | Pause per operation | Time to close (average, longest) |
|---|---|---|
| littlefs, 200-byte files | 4.5 ms | 7.2–10.8 ms, 131 ms |
| FAT on `/boot`, 200-byte files | 179 ms | 2.4–2.5 ms, 15 ms |
| littlefs, 4,000-byte files | 39 ms | 58–65 ms, 159 ms |
| FAT on `/boot`, 4,000-byte files | 208 ms | 8–11 ms, 79 ms |

- **How to read the table:**
  - FAT's close returns before anything reaches the flash (§13a's worker writes it afterwards),
    so FAT looks fast to close, then stops the system for longer.
  - littlefs's close returns when the file is on the flash.
  - The longest littlefs closes include block erases (about 45 ms each) when a directory is
    compacted or a block is allocated.
- **Other numbers:**
  - 150 small files in one directory, back to back: 3.6 ms of pause per operation. Creating
    them took 18.6 ms each on average, rewriting 7 ms, removing 1.6 ms.
  - Formatting took 91 ms (two erases). Mounting took 0 to 1 ms.
- **Code:** littlefs's `.text` is 20.0 KB and its rodata 0.7 KB (the input sections, so an upper
  bound). `lfsbench` adds 3.1 KB.

**What this means for `/boot` (to decide):**
- **Writing small files stops the system about 40 times less** (4.5 ms against 179 ms per
  operation), and about 5 times less for 4 KB files.
- **Every close is durable on the flash.** FAT's background writes leave it durable only about
  0.1 s later, and not atomic.
- **The cost:** a close waits for its own flash operations, a few milliseconds. It waits about
  130 ms when a block must be erased: on a PSRAM stack, the caller waits for the flash worker as
  it does today.
- **The integration (to design if chosen):**
  - A `LittleFsVolume` beside `FatVolume` in the VFS.
  - Reads by `memcpy` from the mapped partition, which works on any stack. Only programs and
    erases go to the internal-stack worker, which replaces §13a's write-back cache.
  - Modification times and the hidden and read-only flags as littlefs attributes. Symbolic
    links emulated as on FAT.
  - The partition table entry changed, and `/boot`'s files moved over at the first boot.

### 13d. `/boot` on littlefs (design, 2026-10-07; the user chose littlefs and asked for a write cache)

**1. Filesystems behind an interface.** The VFS calls FatFs directly today (`vfs.cpp`,
`fatfile.cpp`, `zoneinfo.cpp`). A `Volume` interface (private to `bmds_fs`) goes between them:
- **What it has:**
  - `typeName()`, `info()`
  - `stat()`, `open()` (a `FileHandle`, with the link and no-path reports `Fat::openFile()` has
    now)
  - `mkdir()`, `rmdir()`, `remove()`, `rename()`, `setModified()`
  - directory cursors: `openDirectory()`, `next()`, `close()`
  - plain reads for link files and the zone database: `openReader()`, `readAt()`,
    `closeReader()`
  - `writeLinkFile()`
- **Paths are relative to the volume** (`""` for its root, else `/dir/file`). Every call runs
  under the storage lock, as now.
- **What stays in the VFS:**
  - Everything above the filesystems: the mount table, `/dev`, `/bin`, `/share/zoneinfo`,
    following links, identities, the change hook.
  - Link files keep one format on both filesystems: a System file holding Cygwin's cookie (§4).
- **"No path":** an operation that fails because a directory along the path is missing, or is a
  file, reports it (FatFs's `FR_NO_PATH`; littlefs's `NOTDIR`, or `NOENT` when the parent isn't a
  directory). The VFS then looks for a link along the way, as now.
- **Which filesystem mounts:**
  - A device that is a flash (`BlockDevice::flash()` returns its `FlashDevice`) mounts littlefs,
    and `Vfs::format()` makes littlefs on it.
  - Every other device mounts FAT, as now.
  - `FatVolume` becomes the FAT implementation, and the shared parts of `fatlayer.h` (the lock,
    the registry, the change hook) move to `fslayer.h`.

**2. `FlashDevice`** (`bmds_fs`, portable): a NOR flash as littlefs sees it.
- **Its calls:** `blockBytes()`, `blockCount()`, `read(address, buffer)`,
  `program(address, data)`, `erase(block)`, and `flush(timeoutMs)`, which waits until
  everything written is on the flash.
- **Rules:** `read()`, `program()` and `erase()` may be called from any task on any stack, and
  their effects are seen by later reads at once.
- **On the host:** `RamFlashDevice`, a simulated NOR (§13c's semantics), for the VFS tests.

**3. `LittleFsVolume`** (`bmds_fs`, portable, littlefs v2.11.3):
- **littlefs's configuration (§13c's):**
  - read and program sizes 16, caches 512 bytes (PSRAM), `block_cycles` 500
  - lookahead of one bit per block
  - `LFS_NO_MALLOC`: every buffer comes from the volume. A file's 512-byte buffer is allocated
    with the file (PSRAM).
- **Modification time and attributes** are littlefs attributes: `'t'` (int64 nanoseconds,
  UTC) and `'a'` (the FAT attribute byte).
  - A file written gets its new time in the same commit as its data: the attribute is given to
    `lfs_file_opencfg()` and set just before the close.
  - A new directory gets one at `mkdir()`.
  - An entry without `'t'` shows time 0 (unknown), as now.
- **Busy, as on FAT** (FatFs's `FF_FS_LOCK`): littlefs lets a file be open twice, each handle
  with its own view, and the last close wins. The volume keeps a table of open files instead:
  - a file open for writing can't be opened again;
  - a file open at all can't be opened for writing, removed or renamed;
  - each of those is `Busy`.
- **Names:** littlefs's are case-sensitive and FAT's aren't. `FileInfo::id` folds case
  (§4), so on littlefs `A` and `a` share an id. Noted, not fixed: `/boot`'s names come from
  this project.
- **The rest, against POSIX and the VFS's contract:**
  - `rename()` onto an existing name is `Exists`, as on FAT (littlefs would replace it).
  - Seeking past the end and writing fills zeros (littlefs does it natively).
  - `truncate()` grows with zeros.
  - `statfs` gives type `littlefs`, the block as its allocation unit, and free space from
    `lfs_fs_size()`.

**4. The write cache: `FlashPartition`** (`bmds_storage`), the `FlashDevice` for an IDF
partition. This answers "can littlefs have FAT's write cache?" Yes, safely, because it keeps
order.
- **An ordered queue, not a block cache:**
  - `program()` and `erase()` append the operation to a FIFO in PSRAM (64 KB) and return.
  - A worker task (`flash0`, internal stack) does the operations in order, one at a time.
  - Nothing is reordered or merged. The flash therefore always shows a state littlefs itself
    passed through, and a cut only loses the queue's tail. §13c's test proves littlefs survives
    that: it is a cut at an earlier operation.
  - FAT's cache could reorder blocks; this one can't, so littlefs's guarantees hold.
- **Reads see the queue:**
  - `read()` copies from the memory-mapped partition, then applies the queued operations that
    overlap, oldest first (an erase as 0xFF, a program as its bytes), under the device's lock.
  - The worker removes an operation only after doing it. A read in between applies it twice,
    which gives the same bytes.
  - While the flash is busy the caches are off and the other core is paused, so a half-done
    operation is never read.
- **No stack restriction any more:** only the worker touches the flash, and reads are a
  `memcpy`. littlefs runs on the caller's task, PSRAM stack or not, and §13a's forwarding of
  every request to the worker goes away.
- **A full queue:** the writer waits for room (a semaphore the worker releases), as FAT's cache
  waits when all eight blocks are dirty.
- **Durability:**
  - A close doesn't wait: the file is on the flash a few milliseconds later, or about 0.1 s
    when a block is erased. That is the same window as FAT's cache today, but now always
    consistent.
  - `fsync` (`Stream::flush()`) waits for the queue (`flush()`), so a program that asks for
    durability gets it. FAT's didn't.
  - A restart writes the queue out first (the shutdown handler, as now).
- **Bad blocks:** an operation that fails in the worker is logged and the queue moves on.
  littlefs's own relocation of bad blocks can't act on it, because the error comes after the
  call returned. NOR flash rarely fails a program, and the IDF reports it the same way today
  (§13a).
- **What it costs:**
  - 64 KB of PSRAM for the queue.
  - The worker's 3 KB internal stack, which already exists.
  - The mapping: 4 MB of the flash MMU's address space, held while `/boot` is mounted.
- **Gone:** wear levelling (littlefs levels wear itself), the 8-block write-back cache, and
  the partition's 512-byte sector view.

**5. Moving `/boot` (`BootDisk::init()`):** *(the move was built, used once on the user's board,
then removed at the user's word, since nothing on that `/boot` mattered: a partition without
littlefs is now simply formatted; see the as-built notes)*
- **The partition table:** `boot` becomes `data, littlefs` at the same offset and size.
  `BootDisk` finds it by label, so a board still carrying the old table works too.
- **The new layout:** a littlefs superblock (`"littlefs"` at offset 8 of block 0 or 1) is
  mounted as is.
- **Moving the old layout's files:** anything else that wear levelling can mount as FAT is
  copied over:
  1. Mount the FAT, through a read-only wear-levelling device, at `/boot`.
  2. Read the whole tree into PSRAM: files with their times and attributes, links as link
     files, at most 2 MB. Unreadable entries (§13a's 0xFF ones) are skipped and logged.
  3. Unmount it, format littlefs, mount it at `/boot`, and write the tree back.
- **Otherwise:** format a new `/boot`, as on a new board.
- **Risk:** a cut during the move (a second or two, once) leaves a `/boot` without the files
  not yet written back. The defaults are reinstalled at the next boot; the user's own files
  would be lost.
- **After:** `.defaults` is copied like any file, so nothing is reinstalled. The random seed
  works as before.

**6. Tests:**
- **On the host:** the VFS tests that cover files, directories, holes and truncation,
  identity, the change hook, links, times and a full disk run on littlefs too, on a
  `RamFlashDevice` mounted beside the FAT RAM disk. New tests: Busy, `fsync`, and the ordered
  queue's read overlay (as a portable class, `FlashQueue`, used by `FlashPartition` and tested
  on the host).
- **On the board:**
  - `/boot` moved with its files.
  - The desktop, configs and `.trash` work.
  - `lfsbench fat` replaced by measuring `/boot` itself through the VFS.
  - A reset during a write loop leaves `/boot` mountable and every file whole.

**As built** (2026-10-07):
- **The interface:** `Volume` (`fslayer.h`) with `FatVolume` and `LittleFsVolume` behind it.
  `vfs.cpp` and `zoneinfo.cpp` no longer call FatFs. All 341 tests from before passed unchanged
  once FAT was behind it.
- **Host tests** (`tests/host/lfsvfs.cpp`, 348 tests in all, quiet):
  - Files, directories, holes, truncation, Busy, links, times, identity, stale files after an
    unmount, and a full flash, on littlefs in the VFS.
  - `FlashQueue` around its ring 20,000 times against a model.
  - A write queue cut off every 7th step of 300 file rewrites and renames: 43 cuts, more than
    half of them dropping queued writes. Each cut image mounts, and every file is a whole
    version or empty (made, not yet written).
- **Moving the user's `/boot`** (backed up first as `build/boot-before-littlefs-2026-10-07.img`):
  - 19 of 19 files and links moved: the configs, the desktop's items, `.trash` and the `sd`
    link, with their times. The 96 garbage entries of §13a's damaged directories were skipped,
    so those directories are clean now.
  - It took 0.7 s, once. The main task had 916 bytes of stack left on that boot, and 1,528 on a
    normal one, whose whole init took 44 ms.
- **`fsbench /boot`** (20 files, 300 ms apart; FAT's from §13c in brackets):

| | Pause per operation | Close, average | Close, longest |
|---|---|---|---|
| 200-byte files | 4.5 ms (179 ms) | 5.2–9.3 ms | 68 ms |
| 4,000-byte files | 43.6 ms (208 ms) | 3.4–7.1 ms | 7.4 ms |

  - **The delay matters:** with the worker starting at once, closes averaged 11–14 ms (59–63 ms
    for 4,000-byte files), because an erase stops both cores and so held up the close that was
    still running. Waiting `DelayMs` (100 ms) after the last write, as FAT's cache did, lets
    the close finish first. The pause is the same either way.
  - **Bursts:** 20 small files back to back cost 4.6 ms of pause per operation. 100 in one
    directory cost 5.5 ms; creating them averaged 35 ms, the longest 0.82 s (littlefs
    compacting a big directory).
  - **`metadata_max` 1024 was tried and rejected:** compacting every 1 KB took 35 ms of pause
    per operation.
- **Resets:** 32 hardware resets at random moments of a write loop (`seq` output into four
  files). `/boot` mounted after every one without being formatted, and every file was a whole
  `seq` output.
- **The desktop** reads its icons and their places from the moved `/boot`. A rename by F2
  renamed the file and rewrote `desktop.conf`.
- **Sizes:**
  - `df` shows 4,096 KB, since wear levelling's sectors are gone.
  - The firmware is 904,240 bytes; the last FAT-only build was 868,704. That includes littlefs,
    the move and `fsbench`.
  - `fsbench` replaced the prototype `lfsbench`, which is gone. The scratch region
    past the last partition still holds the bench's old data, which nothing uses.
- **The move removed** (the same day, the user's call): a partition without littlefs is formatted
  and given the defaults, as on a new board. Wear levelling and its `sdkconfig.defaults` lines
  went with it; the firmware is 892,160 bytes. The board's partition was erased with
  `esptool erase-region 0x410000 0x400000`; the next boot formatted it (one line, "isn't
  littlefs: formatting it") and installed the ten defaults and a new random seed.

## 14. Editing partitions: `fdisk` (planned, 2026-10-06)

**Why.** `mkfs` writes a new table with one partition (§5), but nothing changes a table: adding a second partition, resizing, or changing a type takes a PC. It is rarely needed, so it isn't built into the firmware: it is a program on the card.

**The tool: BusyBox's `fdisk`**, an applet of the BusyBox already installed (not built in; it costs only BusyBox's image, size to measure). It is off in our build today (`CONFIG_FDISK`, `FEATURE_FDISK_WRITABLE`).
- It edits MBR tables: primary, extended and logical partitions, and types.
- `FEATURE_GPT_LABEL` only shows GPT tables. `mkfs` writes GPT on cards from 128 GB, so editing those needs another program later. util-linux's `sfdisk`/`fdisk` (C, MBR and GPT, through libfdisk) is the candidate when it is wanted; `gdisk` is C++.

**What the kernel must add:**
- **Raw block devices as streams.** `/dev/sd0` and `/dev/sd0p0` can't be opened yet (§4: "raw access is a later need"). They become readable and writable at any byte offset (whole sectors read, changed and written back), and seekable. The size comes from `lseek(SEEK_END)`, which is BusyBox's fallback when its `BLKGETSIZE64` `ioctl` fails, so no new slot is needed. Sectors are 512 bytes.
- **Mounted filesystems stay safe.** A write to a whole device is refused (`Busy`) in sectors that belong to a mounted partition, and so is any write to a mounted partition's own node. The table's sectors (the MBR, extended boot records, GPT headers) lie outside every partition, so `fdisk` can write a table while `/sd0/p0`, where BusyBox itself lives, stays mounted. Reads are always allowed.
- **The new table takes effect** when the handle that wrote to the device closes. The kernel then reads the table again:
  - partitions that aren't mounted are registered anew (new ones appear and are mounted if they hold FAT; removed ones go);
  - a mounted partition whose entry changed keeps its old extent until it is unmounted, with a warning in the log. This is Linux's "the kernel still uses the old table", which `fdisk` prints itself when its `BLKRRPART` fails.
  - Removing and inserting the card, or a reboot, reads everything again.
- **Extended partitions** (logical, inside an MBR extended partition) aren't read by BMDS yet (§4). `fdisk` can make them, so reading them comes in the same step.

**ABI impact:** append-only. Block device nodes, refused today (`NotSupported`), open as streams. No new slot.

**Testing:**
- **Host** (`RamBlockDevice`): raw reads and writes through the VFS at odd offsets; writes refused inside mounted partitions and allowed in the table's sectors; the re-read on close (a new partition registered and mounted, a mounted one kept); logical partitions read.
- **Board** (a scratch card): `fdisk` adds a second partition; `mkfs /dev/sd0p1`; it mounts at `/sd0/p1`; a GPT card is listed.
