# Storage: SD Card, Filesystem, Files in the Shell

Status: **step 1 done** (SD driver, verified on the board including hot-plug); steps 2-5 not started (Phase 2).

## 1. Goal

- The microSD card works as the system's disk: mounted at **`/sd`** at boot, readable and writable from any PC (FAT32, and exFAT for the 64 GB+ cards that ship with it).
- **Hot-plug:** insert and remove cards at any time. A removed card fails its open files cleanly; a new one mounts by itself.
- **Files are streams.** An open file is a `Stream` behind a `FileHandle`, exactly like a pipe or the console, so redirection, pipelines and (Phase 3) programs need nothing new.
- **The shell gets files:** `ls cd pwd cat cp mv rm mkdir rmdir df hexdump touch`, `<` `>` `>>` to files, globbing, file tests, `source`, `sh script`, `/sd/autoexec.sh`, path completion.
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
3. **Inserted:** power-cycle the card (GPIO45: 20 ms off, 10 ms on, so a card left in any state after a reboot resets), `sdmmc_card_init` (which switches to high speed), mount `/sd`. Failures are logged and retried on the next edge.
4. **Removed:** unmount `/sd` at once (open files go stale, below), stop the SDMMC host, cut card power.

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

**Recommendation: vendor ChaN's FatFs R0.15 in `thirdparty/fatfs` with our own `ffconf.h` and system glue, instead of IDF's `fatfs` component.**

| | IDF `fatfs` component | Vendored FatFs (recommended) |
|---|---|---|
| exFAT | **Compiled out** (`FF_FS_EXFAT 0` in its fixed `ffconf.h`) | On: 64 GB+ SDXC cards work as they come from the shop |
| Dependencies | Pulls in IDF VFS, SDSPI, wear levelling, a `diskio_sdmmc` that allocates bounce buffers | None: FatFs is plain C over our `BlockDevice` |
| Locks, memory | FreeRTOS mutexes and `malloc` in its port | Our `Mutex`, our `Memory` (LFN buffers in `Psram`) |
| Host tests | Linux port exists, but not with our block device | Builds in the host tests, against `RamBlockDevice` |
| Updates | Comes with IDF | We update a vendored copy (FatFs changes rarely; R0.15 is from 2022) |

Configuration: long file names (255, buffer from `Psram`), UTF-8 API (`FF_LFN_UNICODE 2`), code page 437 for short names, **exFAT on** (64-bit LBA), `FF_FS_REENTRANT` (one `Mutex` per volume), `FF_FS_LOCK` (no file open twice for writing, no deleting an open file), `f_mkfs` (for `mkfs`), trim, fast seek off. `FF_FS_TINY 0`: each open file has its own sector buffer (`Psram`), so files don't thrash one shared window.

**Time stamps** come from the kernel's system clock ([`time.md`](time.md)): `get_fattime()` converts `Clock::now()` to local time in the kernel time zone (FAT stores local time; exFAT also records the UTC offset). An unset clock still counts forward from 2026-01-01, so new files never carry 1980.

**Why not IDF VFS + newlib (`fopen`, `open`)?** It brings a second, global fd table, newlib `FILE` buffers in internal RAM, errno and blocking calls with no timeouts or interrupts, beside the `FdTable`/`FileHandle`/`Stream` model the shell and the Phase 3 ABI already use. Programs will get file syscalls from BMDS (Phase 3); a newlib in a program can sit on those.

## 4. The VFS (`bmds_fs`, portable)

### Paths and mounts

- Absolute paths, `/` separators, UTF-8. FAT names are case-insensitive and case-preserving.
- **Mount table:** `/sd` → the FAT volume on the SD card. `/` is a small virtual directory listing the mount points (and `dev`). `/dev/null` and `/dev/console` (the controlling terminal) are device nodes, so `/dev/null` stops being a special case in the shell.
- **The current directory is per shell** (and per program in Phase 3), not global: the VFS resolves a path against the cwd it's given, normalizing `.` and `..` (`..` never climbs above `/`).

### API (kernel-internal; the Phase 3 syscalls wrap it)

```cpp
namespace Vfs {
        Result<FileHandle::Ptr> open(StringView path, uint8_t flags, StringView cwd);  // Read Write Create Truncate Append Exclusive
        Result<FileInfo>        stat(StringView path, StringView cwd);                // type, size, modified, attributes
        Result<Directory::Ptr>  openDirectory(StringView path, StringView cwd);       // next() -> FileInfo with name
        Error mkdir(...), rmdir(...), remove(...), rename(from, to, cwd);
        Result<FsInfo>          statfs(StringView mountPoint);                         // size, free, cluster size, type
        Error                   setTime(...);                                          // touch
        Result<String>          normalize(StringView path, StringView cwd);
}
```

- **`File`** is a `Stream` (read, write, flush = `f_sync`) plus `seek`, `tell`, `size`, `truncate`. A `FileHandle` owns the open `File` and frees it in `handleClosed()`, so `dup` and `2>&1` share one file offset, as POSIX does. `Stream` gains a `seek()` that returns `NotSupported` for pipes and consoles.
- **Errors:** FatFs results map onto `Error` (`FR_NO_FILE` → `NotExist`, `FR_DENIED` → `NoSpace` or `PermissionDenied`, `FR_EXIST` → `Exists`, `FR_LOCKED` → `Busy`, `FR_WRITE_PROTECTED` → `ReadOnly`, `FR_NOT_READY` → `NoMedia`...). One new code, **`Error::NoMedia`** ("no medium": the card is out), is appended.
- **Removal:** the volume's `FATFS` object is allocated once and kept, so when the card goes, `f_unmount` bumps its id and every stale `FIL` fails FatFs's own validity check (`FR_INVALID_OBJECT` → `NoMedia`). No dangling pointers, no extra bookkeeping. A new card mounts into the same object.
- **Thread safety:** FatFs serializes each volume (`FF_FS_REENTRANT`); a `File` may be used from one task at a time (as any stream).

## 5. Shell

### Commands (formats are an interface: proposals here, fixed when they ship)

| Command | Notes |
|---|---|
| `ls [-l] [-a] [-1] [path...]` | Columns at the terminal width (`COLUMNS`), one per line into a pipe. `-l`: `<type><r><w>  <size>  <yyyy-mm-dd hh:mm>  <name>`, e.g. `-rw-     1234  2026-10-03 14:02  notes.txt`, `drw-        0  …  docs/`. `-a` shows dot and hidden/system files. Colored by type when `colorDepth()` allows. |
| `cd [dir]`, `pwd` | Built-ins: `PWD`, `OLDPWD`, `cd -`; `cd` alone goes to `$HOME` (`/sd`). |
| `cat`, `head`, `wc`, `grep` | Take file arguments (they read stdin today). |
| `cp [-r] src... dst`, `mv src... dst` | `mv` within a volume renames. |
| `rm [-r] [-f] path...`, `mkdir [-p] dir...`, `rmdir dir...`, `touch file...` | |
| `df [-h]` | `Filesystem  Size  Used  Avail  Use%  Mounted on`, as coreutils prints it. |
| `hexdump [-C] [file]` | `-C` is the canonical hex+ASCII layout. |
| `sd [bench [megabytes]]` | Card info, one `Key: value` line each: `Card:` name and type (SDSC/SDHC/SDXC/MMC), `Capacity:` GB and sectors, `Bus:` width and clock, `Maker:` CID manufacturer, OEM, revision, serial, date. No card (or an unusable one): a message on stderr, status 1. `bench` reads from the start of the card, direct and through the bounce buffer, in 64 KB and 512 B requests, and prints MB/s. |
| `mkfs [-t fat32\|exfat] /sd` | Formats the card (asks for `-y` to confirm). |

### Language

- **File redirection:** `<`, `>`, `>>`, `>|`, `<>`, `&>` open real files (relative to the cwd). `set -C` (noclobber) is added with `>|`.
- **Globbing** (pathname expansion): unquoted `*`, `?`, `[...]` in a word expand to the sorted matching paths, component by component, using `Glob`; a leading `.` must be matched explicitly; no match leaves the word as it is (bash's default). Quoted text never globs (the expander already marks it).
- **Tilde:** `~` and `~/...` expand to `$HOME`.
- **File tests:** `test -e -f -d -s -r -w -x -nt -ot` on real files (`-x`: directories and `*.sh`).
- **`source file [args]`** / **`. file`**: run in the current shell. **`sh file [args]`**: run in a subshell with its own positional parameters. (Running `./script.sh` directly, with `#!`, comes with the Phase 3 program model.)
- **`/sd/autoexec.sh`:** after the first mount attempt (or a timeout), the console shell sources it if it exists. Ctrl-C interrupts it, as anything else.
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
| 2 | The system clock ([`time.md`](time.md)): `Clock`, `TimeZone`, the reset record, `date`, `clock`. FatFs vendored (`thirdparty/fatfs`, our `ffconf.h`, `ffsystem`, `diskio`); `bmds_fs` VFS, `File`, `Directory`, mount table, `/dev` nodes; mount `/sd`; host tests on `RamBlockDevice`. |
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

## 9. ABI impact

- **On-disk:** FAT12/16/32 and exFAT as written by FatFs; the mount point `/sd`; `/sd/autoexec.sh`. Later directory conventions (`/sd/bin`, …) belong to the Phase 3 design.
- **Shell:** the new commands, their options and output formats (§5); `cd`/`pwd`; file redirection; globbing; `rz`/`sz` behavior.
- **Errors:** `Error::NoMedia` appended.
- The VFS API is kernel-internal; the Phase 3 file syscalls will be the frozen surface.

## 10. Testing

- **Host:** FatFs + VFS on `RamBlockDevice`: create, read, write, seek, truncate, rename, directories, long and UTF-8 names, full disk, `mkfs` both types, stale handles after an unmount; the shell file commands, redirection and globbing through the run corpus (the bash check runs those scripts in a temporary directory, so they're written relative to the cwd).
- **Board:** card in at boot, inserted and removed while running (with a file open, while writing), `sd` info, throughput (read/write MB/s, direct and bounce paths), FAT32 and exFAT cards from a PC, YMODEM both ways against `lrzsz`.

## 11. Decisions (agreed)

1. **Vendor FatFs with exFAT** rather than IDF's `fatfs` component (§3): exFAT is a must for today's cards.
2. **Unix paths** (`/sd/dir/file`), not drive letters.
3. **YMODEM via `lrzsz`** for serial transfer, for now.
4. **A Linux-style system clock** ([`time.md`](time.md)): a software clock in the kernel (UTC), set and disciplined by sources (`date -s` now; NTP/PTP later), surviving resets through the RTC domain; time zones are presentation.
5. **IDF's check messages stay in development builds.** `CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT` would save 16 KB image-wide (measured); release builds will set it (`docs/BUILDING.md`, Release Builds).

## 12. Open questions

- UHS-I (1.8 V, SDR50/SDR104): worth it for a DOS-like machine? Measure high speed first.
- Read-ahead or write-back caching above FatFs: measure first; FatFs already transfers whole-sector runs directly.
- A `/ram` RAM disk on `RamBlockDevice` (cheap once it exists) for scratch files and tests on the board.
