# Network Filesystems: Mounts Served by Programs, `sshfs` First

Status: **steps 1 and 2 done**; next, `sshfs` (2026-10-21).

## 1. Goal

The user's request (2026-10-21): BMDS mounts a network filesystem in its VFS, `sshfs` first,
so files move on and off the board without the serial cable or a card reader. A directory on
the user's PC (or any SSH host) appears at a path such as `/net/pc`, and every program, the
shell and the file browser use it as they use the card.

## 2. Facts that shape the design (checked 2026-10-21)

- **sshfs is SFTP over SSH.** The SSH transport (key exchange, ciphers, host and user keys)
  is a lot of code that BMDS already has as a program: Dropbear's client, `dbclient`, built into
  the multi-binary in `/rom/bin` (`DBMULTI_dbclient`). Putting SSH in the kernel would duplicate
  it there; a program can use it as it is.
- **The board already speaks SFTP's server side** (`programs/sftp-server`, fuzzed): the
  messages are known; the client side is their mirror.
- **The VFS serializes everything on one lock** (`Fs::lock()`, `vfs.h`: "filesystem calls are
  thereby serialized"): path resolution, the mount table, every volume call, and every read and
  write of an open file on FAT, littlefs and tmpfs. A volume whose calls wait on a network would
  freeze every file operation in the system while it waits (a Wi-Fi stall, a server gone).
- **A volume** (`bmds_fs/src/fslayer.h`) is the VFS's per-filesystem interface: `stat`, `open`
  (giving a `FileHandle`), `openReader`, `mkdir`, `rmdir`, `remove`, `rename`, `setModified`,
  `writeLink`, `openDirectory` (a `Cursor`), `info`, `map`. Volumes come from static pools and
  are released when unmounted; everything they do expects the storage lock.
- **The mount table** holds 8 mounts (`Vfs::MaxMounts`; `vfs.h`'s comment still said four):
  the card's partitions, `/flash`, `/tmp` take three or more.
- **What else walks every mount under the lock:** the trash index (`/trash` lists each
  filesystem's trash), the time zone database's search, the directory watches.

## 3. The design

### Volumes that lock themselves (step 1)

- A `Volume` may say it **locks itself** (`locksItself()`): it serializes its own calls and is
  never called with the storage lock held. Such a volume is **reference counted**
  (`retain()`, `release()`): the mount holds one, each call under way and each open file or
  directory another; unmounting drops the mount's and the object goes with the last.
- The VFS resolves a path under the storage lock as now. When it reaches a volume that locks
  itself, it **retains the volume, lets go of the storage lock for the call, takes it back**
  and releases the volume; everything around the call (links followed, watches told, identities)
  stays as it is. The mount may have gone meanwhile: the call's result stands, and the next
  resolution finds the mount gone. Local volumes are called exactly as before.
- **Files and directories** opened on such a volume are its own streams and cursors, which take
  its lock, not the storage lock; `Directory` retains the volume while open.
- **Left out of the walks over every mount:** the trash (a network mount has none: a removal
  is a removal), the time zone database's search, the per-mount flush (`Vfs::sync()`).
- **Unmounting** one with calls or files open returns at once; those calls finish against the
  volume, which fails later ones (`NoMedia`, as a pulled card's).
- **Internal API:** `Fs::mountVolume(path, volume)` mounts a volume made elsewhere (the
  program-served volume of step 2; a test volume).
- **Tests (host):** a test volume whose calls block until released: while one is blocked, tmpfs
  works; an unmount during a blocked call returns, the call ends, the volume is freed after.

### Filesystems served by programs (step 2): the design (2026-10-21)

- **One call (slot 123, appended):** `int32_t fs_serve(const bmds_fs_serve_t *config)`: mounts a
  filesystem at `config->path` served by the program, and returns the **server fd**.
  `bmds_fs_serve_t {size, flags (BMDS_O_CLOEXEC), path, type (a name for `df` and the browser:
  "sshfs", at most 15 bytes), max_data (the most bytes a read or write moves, 4096 to 65536;
  0: 32768)}`. The mount follows `Vfs::mount()`'s rules (not over another, not in `/dev` and the
  other fixed trees). Errors: `EINVAL`, `EEXIST`, `EBUSY`, `ENOSPC` (no mount free), `ENOMEM`.
- **Reading the server fd** gives the next request, a whole record (a buffer smaller than
  `BMDS_FS_REQUEST_MAX(max_data)` is refused with `EINVAL`); it waits while there is none
  (`poll()`: readable). **Writing it** takes replies, whole records, one or more a write; a
  reply to no request (an id unknown, or given up) is dropped and counted. **Closing it**
  unmounts the filesystem: requests waiting fail with `ENOMEDIUM` (`NoMedia`, as a pulled
  card's), and so does everything later on files open on it.
- **`length`** is `READ`'s bytes wanted, `WRITE`'s data, and for `READDIR` and `READLINK` the
  most bytes the reply may carry.
- **`bmds_fs_request_t`** (56 bytes), then the path (`path_len` bytes, no NUL, from the mount's
  root: `/` for it, else `/dir/name`), then a second path (`path2_len`: `RENAME`'s and
  `SYMLINK`'s), then `WRITE`'s data (`length`):
  `size` (the record's bytes), `op`, `id` (the reply's), `flags`, `handle` (64-bit: the
  server's, from `OPEN`), `offset` (64-bit), `length`, `path_len`, `path2_len`, `reserved`,
  `time_ns` (64-bit: `SETTIME`'s).
- **`bmds_fs_reply_t`** (32 bytes), then its payload: `size` (the record's bytes), `id`,
  `error` (0, or a negative `BMDS_E*`), `count` (`READ`: the bytes after it, 0 at the end;
  `WRITE`: the bytes written; `READDIR`: the entries after it, 0 at the end), `handle` (64-bit:
  `OPEN`'s), `reserved` (64-bit).
- **`bmds_fs_entry_t`** (32 bytes, then the name, the whole padded to 8 bytes; `STAT` gives one,
  `READDIR` `count`): `size` (with the name and padding), `type` (`BMDS_TYPE_*`), `bytes`
  (64-bit), `modified_ns` (64-bit, UTC), `mode` (the server's permission bits, for show),
  `name_len`.
- **Operations** (`op`):
  | Op | Request | Reply |
  |---|---|---|
  | `STAT` 1 | path | an entry (the link itself: lstat) |
  | `OPEN` 2 | path, `flags` (`BMDS_FS_O_READ` 1, `_WRITE` 2, `_CREATE` 4, `_EXCLUSIVE` 8, `_TRUNCATE` 16) | `handle` |
  | `READ` 3 | `handle`, `offset`, `length` | `count` bytes |
  | `WRITE` 4 | `handle`, `offset`, `length` bytes | `count` |
  | `CLOSE` 5 | `handle` | |
  | `READDIR` 6 | path, `offset` (entries to skip) | `count` entries (0: the end) |
  | `MKDIR` 7, `RMDIR` 8, `REMOVE` 9 | path | |
  | `RENAME` 10 | path, path2, `flags` (`BMDS_FS_REPLACE` 1) | |
  | `SETTIME` 11 | path, `time_ns` | |
  | `STATFS` 12 | | `bmds_fs_space_t {total, free}` (64-bit each) |
  | `TRUNCATE` 13 | `handle`, `offset` (the size) | |
  | `READLINK` 14 | path | `count` bytes: the target |
  | `SYMLINK` 15 | path (the link), path2 (its target) | |
  An op the server doesn't know: `ENOSYS`; later ops are appended.
- **In the kernel (`ServedVolume`, `bmds_fs`, portable):** a volume that locks itself. Each call
  makes a request (an id, a slot of `MaxPending` 16 for callers waiting at once), queues it for
  the server's reads, and waits for its reply up to `netfs/timeout` seconds (a setting, 30:
  then `EIO`, the late reply dropped). Files are its streams: reads come from a read-ahead
  buffer of `max_data` bytes (a sequential reader pays one request a buffer); writes gather in
  one of `max_data` and go at a seek, a read, a truncate, a flush or the close. `STAT` answers
  are kept `netfs/attr-ms` (1000) in a cache of 32 paths, dropped by any change made through the
  mount. Directories are read `READDIR` by `READDIR`.
- **Links:** `STAT` says `BMDS_TYPE_LINK` for a link; the VFS follows it with `READLINK` (a
  path in the same filesystem or not, as any link), and `symlink()` there is `SYMLINK`. Not the
  emulated link files FAT has.
- **The SDK:** the structs, the ops and `bmds_fs_serve()` in `sys.h`; no library yet (`sshfs`
  is the first server).
- **Tests:** host (the volume against a server thread in memory: every op, errors, many callers
  at once, a timeout, the server closing); `progtest netfs` (a server thread of its own serving
  a directory in memory at `/tmp`'s side, then `files`' checks through it).

### `sshfs` (step 3)

- **`sshfs [user@]host:[dir] MOUNTPOINT [-p port] [-o reconnect]`** (a program, `/bin` or the
  card): runs `dbclient` with the `sftp` subsystem (`-s sftp`, to check; else the host's
  `sftp-server` by path) on a pair of pipes, speaks SFTP version 3's client side, and serves the
  mount's requests. Keys and known hosts as `dbclient` has them (the board's
  `~/.ssh`, `net/ssh/` settings).
- **Symbolic links** are the server's own (SFTP's `readlink`, `symlink`), not the emulated
  link files FAT needs.
- **Speed:** measured against `scp` from the same host; read-ahead and write-behind sizes
  chosen by measuring.

### Mounts from the settings (step 4)

- `net/mounts`: mounts made when the network is up (`sshfs pc:/home/user /net/pc`),
  retried with backoff; the file browser shows them with the card's.

## 4. Steps

1. **Volumes that lock themselves:** `locksItself()`, reference counting, the storage lock let
   go around their calls, the walks leaving them out, `Fs::mountVolume()`; host tests.
2. **The program-served volume and its ABI,** with caching; `progtest netfs`.
3. **`sshfs`,** tried against the user's PC; speed measured.
4. **Mounts from the settings,** and the file browser.

## 5. As built: step 1 (2026-10-21)

- **`Volume`** (`fslayer.h`) gains `locksItself()` (false by default) and `retain()`; for such a
  volume `release()` drops a reference. `Fs::mountVolume(path, volume)` mounts one made
  elsewhere, taking the reference given (released if the mount is refused).
- **`vfs.cpp`:** `callVolume()` wraps every call into a volume made with the storage lock held:
  for one that locks itself, it retains it, lets go of the lock, calls, takes the lock back and
  releases it. Wrapped: `stat` (and the per-component stats of link following), `open`,
  `openReader`, `mkdir`, `rmdir`, `remove`, `rename`, `setModified`, the emulated links' reads
  and writes, `openDirectory`, a directory's `next`, `statfs`. `map()` refuses one
  (`NotSupported`: no bytes in place over a network). A `Directory` open on one holds a
  reference and closes without the storage lock.
- **Left out:** the trash index keeps such a mount's slot empty (so the slot numbers hold);
  `Trash::move()` refuses a path on it (`NotSupported`: a removal is a removal);
  `Vfs::MountInfo::network` says which mounts are; `sync()` already skipped device-less
  mounts.
- **Host tests** (`tests/host/netvolume.cpp`): a test volume whose `stat` can be held, and whose
  every call checks from another thread that the storage lock is free: every operation, none
  with the lock held; a `stat` held while a tmpfs file is made, written, read and removed; the
  mount taken away during it (`unmount()` returns, new calls find nothing, the held call
  finishes against the volume, which is freed with the last reference); a refused mount gives
  its reference back. The whole host suite passes; on the board, `progtest files` and `posix`
  and the compatibility corpus.
- **Not yet:** the zone database and the leap-second list read through `closeReader()` under the
  lock; harmless until one is put on a network mount (step 4 can refuse that).

## 6. As built: step 2 (2026-10-21)

- **ABI (slot 123, `fs_serve`):** `bmds_fs_serve_t`, `bmds_fs_request_t`, `bmds_fs_reply_t`,
  `bmds_fs_entry_t`, `bmds_fs_space_t` (`STATFS`'s payload: `bmds_fs_info_t` was taken, by
  slot 111's `fs_info`), `BMDS_FS_STAT`..`BMDS_FS_SYMLINK` (1-15), `BMDS_FS_O_*`,
  `BMDS_FS_REPLACE`, `BMDS_FS_DATA_*`, `BMDS_FS_PATH_MAX`, `BMDS_FS_REQUEST_MAX()`, all in
  `sys.h`; the loader (`fscalls.cpp`) checks them against `ServedFs`'s mirrors and turns a
  server's numbers back into errors with `Process::errorFor()` (`errnoFor()`'s inverse).
- **Kernel (`bmds_fs`, `servedvolume.cpp`, 8.6 KB of code; `bmds/servedfs.h`):** the volume
  and the server's end are one object per mount from PSRAM, its references the mount's and the
  server end's; files, readers and cursors are PSRAM objects of their own, a file's read-ahead
  and write buffers (`max_data` each) allocated when first used. 16 slots for requests
  waiting, a cache of 32 attributes, a 4 KB `READDIR` buffer per open directory.
- **Settings:** `netfs/timeout` (seconds, 30, 1-600), `netfs/attr-ms` (1000, 0-60000).
- **Not yet:** links. `READLINK` and `SYMLINK` are in the ABI, but the VFS doesn't send them yet
  (`symlink()` there gives `ENOTSUP`); step 3 adds them with SFTP's.
- **Tests:** host (`tests/host/servedfs.cpp`): every op against a server thread, writes
  gathered and reads ahead counted, many callers answered out of order, a timeout, the server
  gone (`NoMedia`), bad configs, an unmount the server hears of. Board: `progtest netfs` (its
  own server thread, a tree in memory at `/net/progtest`, `max_data` 8192): the bad configs, a
  100 000-byte file written 1 000 bytes at a time in 12 `WRITE`s and read back in 14 `READ`s,
  times, renames (a directory with what is in it; `EXDEV` to `/tmp`), then `progtest files`'
  checks through it, then the server's fd closed (`ENOMEDIUM`, then the mount gone). The host
  suite, `progtest files` and `posix` on `/tmp`, and the compatibility corpus pass.

## 7. Alternatives (not now)

- **SFTP in the kernel:** the SSH transport twice. No.
- **NFSv3 or 9P in the kernel** (no encryption): fast and simple, but the PC needs a server
  (NFS's needs root), and off the LAN it isn't safe. A second filesystem later, on the same
  step 1 (it locks itself too), perhaps on step 2's ABI as a program.
