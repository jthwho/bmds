# `/cfg` on the card, `/boot` renamed `/flash`

**Why** (the user's, 2026-10-14): with flash auto-suspend off (`update.md` §16), every write to the
board's flash stops both cores' caches for the operation (a 4 KB sector erase: 45 ms typical,
400 ms at most), and the audio engine's task (flash code, PSRAM rings) with them. Updates write
flash rarely, with audio stopped. In everyday use the flash writes come from `/boot`: the volume
control saves its setting at every change, the panel's other settings, the random seed. A card
is written by DMA with the caches on, so configuration that lives on the card doesn't stall
anything.

## 1. Decided (the user's, 2026-10-14)

- **`/boot` is renamed `/flash`:** what is left on it isn't about booting. It is named for its
  medium, as `/rom` is, and the two pair: the board's flash, read-only and writable.
- **`/cfg`, like `/home`, leads to one of two places:** the card's configuration directory when a
  card is there, else `/flash`'s. **Everything** configurable is under it: the settings store,
  the secrets (the password's hash, the SSH host keys), `config/*.conf`, `mimeapps.list`, the
  applications, `profile` and `autoexec.sh`, the random seed.
- **No syncing.** The two copies are independent: `/flash`'s is whatever the user put there, and
  the user decides if and when to update it. A card from another board brings that board's
  configuration, as the user means it to.

## 2. The plan

- **Where on the card:** `cfg/` at the root of the card's first partition (`/sd0/p0/cfg`). It
  can't be a setting, since the settings are inside it; like `/flash`, the path is the point.
- **Chosen once, at boot:** the storage stage moves ahead of `sysconf`, and `sysconf` waits for the
  first card check (`Storage::waitForFirstMount()`, bounded: about 300 ms, measured first; the
  card mounted ~150 ms after the storage stage on the board). Settings are then read once, from
  the right place, before the network and the rest apply them.
- **A card inserted or removed later doesn't move `/cfg`** until the next restart: settings
  changing underneath a running system (the hostname, the network, the password) is worse than
  waiting. While the card is gone, saving a setting fails (`NoMedia`), and the value in memory
  stays; `cfg` (the command) says where `/cfg` leads and whether it is reachable.
- **Writes on the card are made safe for a power cut:** a new file is written beside the old one
  and renamed over it (FAT and exFAT aren't power-safe; littlefs was).
- **What stays on `/flash` only:** the update records (`/flash/update/`: the intent, the result,
  the crash marker). Recovery and the crash counter need them before or without a card, and they
  are written only during an update (audio stopped) or after a crash.
- **The defaults** (the repository's `boot/`, renamed `cfg/`) are installed into whichever `/cfg`
  is in use, as G14 does now for `/boot` (each file not there and not listed in the manifest).
- **Updating `/flash` by hand:** `cp -r /cfg/. /flash/cfg/` works; a command (`cfg save`) may
  wrap it (§3).
- **The rename:** `BootDisk` → `FlashDisk`, `/dev/flash0` stays; the partition's label `boot` →
  `flash` (its contents stay where they are); every path constant and setting default under
  `/boot`; the repository's `boot/`; the docs. Nothing has shipped: no compatibility shims. Saved
  settings on the user's board that name a `/boot` path are checked first, and fixed by hand.
- **`/home`** keeps working as now (the `home` setting, default `/flash/home` → now in `/cfg`).

## 3. Decided too (the user's, 2026-10-14)

- **A new card** (no `cfg/` yet) starts as a copy of `/flash/cfg`, made once as the directory is
  created, then independent (the same host key, password and settings; not a sync).
- **Without a card, `/flash`'s writes wait while audio plays:** the write queue (`FlashPartition`)
  is held while an audio stream is open, and written when audio goes idle, when it reaches a cap
  (a share of its 64 KB), on `sync` or `fsync`, or at a restart. Reads already see the queue, so
  nothing else notices; a change made during playback isn't safe from a power cut until then.
- **A card inserted or removed while running** moves `/cfg` at the next restart (§2).

## 4. Steps

1. The rename: `/boot` → `/flash` (code, settings' defaults, the partition's label, `boot/` →
   `cfg/` in the repository, the docs); the board's saved settings checked first.
2. `/cfg`: the storage stage before `sysconf`, the bounded wait, the choice, the copy for a new
   card, the defaults installed there; every configuration path moved under it; writes on the
   card by rename; the `cfg` command (where it leads, reachable or not).
3. `/flash`'s writes held while audio plays.

## 5. As built (2026-10-14)

- **The rename:** the partition's label is `flash`, mounted at `/flash` (`FlashDisk`, was
  `BootDisk`; `/dev/flash0` as before). The repository's `boot/` is `flash/`: `flash/cfg/` (the
  scripts, `config/`, `applications/`) is installed in `/cfg`, the rest (`home/Desktop`) in
  `/flash`, each with its own `.defaults` manifest (`FlashDefaults::install()`). The update's
  records are `/flash/update/`; the default home `/flash/home`.
- **`/cfg`** is a second root link in the VFS beside `/home` (`Vfs::setCfg()`; `/home`'s code made
  general: a `RootLink` node, rewritten as `/home` is), with no fallback: while its filesystem
  isn't mounted, nothing below it exists. Mounting at `/cfg` is refused, as at `/home`.
- **`Cfg::init()`** (`bmds_storage`), a boot stage after `storage` (moved up, before `sysconf`) in
  both firmwares: waits for the card's first check (at most 2 s), uses `/sd0/p0/cfg` when the card
  is mounted (made as a copy of `/flash/cfg` when missing), else `/flash/cfg`; installs the
  defaults there; reads the random seed from it (moved from `BootDisk`), and writes it a few
  seconds after boot and at shutdown. The `cfg` command says where it leads and whether it is
  there.
- **Paths moved to `/cfg`:** the store (`/cfg/sysconf`), `config/{wm,clipboard}.conf`,
  `config/mimeapps.list`, `applications` (the panel's default path), `profile`, `autoexec.sh`,
  `ssh` (`net/ssh/host-keys`), `icons` (the user's icon directory), `random-seed`.
- **The store is written beside itself** (`sysconf.new`, flushed) and renamed over; a store found
  only as `.new` (a cut between the two) is renamed back and read. While the card `/cfg` leads to
  is out, a save fails with `NoMedia` and the value stays in memory.
- **`FlashWrites`** (`bmds_kernel`): a counted hold and a wait list. The audio device holds it
  from its start to its stop (3 s after the last stream). `/flash`'s worker (`FlashPartition`)
  doesn't write while it is held, unless its queue is half full (32 KB), somebody waits for it
  (`sync`, `fsync`, a restart's shutdown handler), or the hold goes.
- **Measured on the board:**
  - **Boot:** the `cfg` stage takes 129 ms (it waits for the card's first check), so reset to
    shell went from 644 ms to 767 ms; the first boot with a new card also copied `/flash/cfg`
    (455 ms).
  - **A write to `/flash` during a tone** (at −40 dB): 0 late periods; written once the tone
    stopped, and there after a hard reset.
  - **234 KB copied to `/flash` during a tone** (over the cap, so written then): 249 late
    periods, one fill stalled 47.6 ms. That is the stall the hold avoids.
  - **Durability:** a write then `sync` during a tone survived a hard reset during the tone; an
    unsynced one didn't (held), as planned.
  - **The card gone** (`umount /sd0/p0`): nothing below `/cfg`, `cfg` says so, and a save fails
    with `NoMedia`; mounted again, all back.
  - **Recovery** chose the card's `/cfg` too; two updates through `tools/bmds-update` (a changed
    build, then the real one) went through the new `/flash/update`.
- **Moving the user's board:** `/boot`'s configuration moved into `/boot/cfg` on the running
  system, the two default scripts replaced with the new ones, the manifest split; then one USB
  flash (the partition's label). The first boot copied it to the card. Moving `/boot/ssh` stopped
  new SSH logins until the restart (the server reads its host keys per login), so it was pointed
  at the new place for that session (`sysconf set`, not saved). `docs/BUILDING.md` has the steps.
- **A consequence to know:** with the card out, new SSH logins fail until a restart, since the
  host keys are on the card. Sessions already open stay.
