# Network Filesystems: Mounts Served by Programs, `sshfs` First

Status: **step 1 done** (2026-10-21); step 2 next.

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

### Filesystems served by programs (step 2)

- **The ABI** (appended, designed in detail at step 2): a program opens a **server fd** and
  mounts it at a path (a new call, or a `mount` operation on the fd); the kernel's volume for the
  mount turns each operation into a request record read from that fd, and waits for the
  program's reply record written to it (request id, operation, path, offsets, attributes; data
  after the header). Requests from several callers are outstanding at once (ids), so a slow
  request doesn't hold others.
- **Caching in the kernel:** attributes and directory entries for a short time (1 s, a
  setting), whole blocks read ahead (64 KB) and writes gathered (64 KB) before they go to the
  server, so `ls` and copies don't pay a round trip per call.
- **The end:** the server closing its fd (or dying) unmounts it; its files fail `NoMedia`.
- **Tests:** `progtest netfs`, a server of its own in memory: every operation, errors, many
  callers at once, the server dying.

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

## 6. Alternatives (not now)

- **SFTP in the kernel:** the SSH transport twice. No.
- **NFSv3 or 9P in the kernel** (no encryption): fast and simple, but the PC needs a server
  (NFS's needs root), and off the LAN it isn't safe. A second filesystem later, on the same
  step 1 (it locks itself too), perhaps on step 2's ABI as a program.
