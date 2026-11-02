# Packages: `pkg` and `.bmpk`

Status: **planned** (2026-10-30, §16 decided). Replaces the card zip of `release.md` §3 and folds in
its step 6 (first boot).

## 1. Goal

An apt/dpkg experience on the board. Every program and data set on the card is a package
(`busybox`, `luckyduck`, `freedoom`...). The user installs the ones they want, or a meta package
that pulls in a set (`bmds-standard`, the default install). Packages come from a repository on
the network (HTTPS: GitHub, or anyone's server) or from files put on the card. Installing,
upgrading and removing are done by the board, which also makes the links a PC can't.

**What it fixes:** the card zip can't carry symbolic links (279 of them, made afterwards by
`etc/dist/links.sh`). Unzipping on Windows puts the files in a folder instead of at the card's
root, and macOS adds `._*` files. Nothing checks the files arrived whole. One program can't be
updated or removed on its own.

**Not goals:** shared library versioning (SDK programs are static, §5); maintainer scripts
(links and directories are data, §3); conffile handling (settings live in sysconf under `/cfg`,
never in packages); being anyone's official repository (§6).

## 2. Decisions (the user's, 2026-10-30)

- Individual packages with dependencies, and meta packages for sets; apt/dpkg as the model.
- From the card or from the network (HTTPS; GitHub-hosted for the project's own).
- Names: the command `pkg`, the file `.bmpk`.
- Several trusted keys: anyone can build a firmware that trusts their own key, and sign packages
  with it. `keys/release.pub` became `keys/trusted.pub` and the acceptance modes "dev" (the dev
  key and the trusted keys) and "trusted" (the trusted keys only,
  `CONFIG_BMDS_UPDATE_KEYS_TRUSTED`): done with this plan, no compatibility shim.
- The user starts with an SD card in the board. The board must be able to partition and format
  it (§10), so the card needs no preparation on a PC.

## 3. The package: `NAME_VERSION.bmpk` (an on-disk format: frozen once shipped)

The container is `.bmup`'s (`update.md` §2), the same code for the head, the sections and the
signature, with its own magic and header:

- **Header** (little-endian, `uint32_t size` first): magic `BMPK`, format 1, flags; the name
  (lowercase letters, digits, `-`, `+`, `.`; at most 32 bytes); the version (at most 32 bytes,
  §4); the architecture (`esp32p4`); the least `BMDS_ABI_VERSION` it needs; the installed size;
  the section count.
- **Sections** {type, offset, size, BLAKE2s-256}, unknown types skipped (append only):
  `1` the manifest, `2` the data, `3` the description (UTF-8, shown by `pkg info`), `4` the
  icons (square PNGs, 16 and 32 pixels as the theme's, at most 64 and 16 KB; added 2026-10-30,
  §18).
- **The signature:** Ed25519 over the header and the table, plus the **key id**: the first 8
  bytes of the BLAKE2s-256 of the public key, so the board finds the key without trying every
  one, and says which signed (§6). `.bmup` gets the key id too (a new field, format 1 is not
  shipped yet).
- **The manifest** (UTF-8 text, a line per entry, tab-separated, parsed in `bmds_core`):
  - the fields: `depends` (§5); `title` (the name people see, "Lucky Duck"), `summary` (one
    line), `category` (freedesktop.org's main categories, as the launcher's: `Game`, `Utility`,
    `System`...), `license` (SPDX) and `homepage`, for the store (§18); `stack` and `display`
    (as `package.toml`'s, for the loader's later use). What kind a package is needs no field: a
    meta package has no files, a data package's architecture is `all`;
  - `f PATH MODE SIZE BLAKE2S`: a file, its data next in the data section;
  - `d PATH`: a directory (made if missing, removed with the package if empty);
  - `l PATH TARGET`: a symbolic link (`symlink()`, FAT's emulated links, `storage.md` §4).

  Paths are relative to the install root (§7), never absolute, never `..`.
- **The data:** each file in turn, in manifest order, cut into blocks of 64 KiB, each block raw
  DEFLATE on its own with a 4-byte compressed size first (a block that doesn't shrink is stored,
  flagged by the size's top bit). The board inflates with `bmds_core`'s existing one-shot
  `Inflate::raw()` into a 64 KiB buffer: bounded memory (two 64 KiB `Psram` buffers) for files of
  any size (`freedoom`'s WADs). Independent blocks lose a few percent of compression; to measure
  on the real packages.

A meta package is a package with no files: a manifest of `depends` only.

## 4. Versions

Debian's rules: `UPSTREAM-REVISION` (`1.37.0-1`), compared as `dpkg --compare-versions` does
(digits as numbers, letters before non-letters, `~` before anything). `REVISION` counts the
recipe's changes for the same upstream release; a new upstream restarts it at 1. In
`package.toml`: `version` as now, plus `revision` (default 1). The comparison is in `bmds_core`,
host-tested against a table of dpkg's results.

## 5. Dependencies

SDK programs are statically linked against musl, so there are no shared libraries to version.
A dependency is a program another calls (`sshfs` on `dropbear`'s `ssh`), or data (the synth
programs on `soundfont-timgm6mb`), or a meta package's members.

- `depends`: a comma-separated list, each `NAME` or `NAME (>= VERSION)`. No alternatives,
  conflicts, provides or recommends to start with (none is needed by today's packages; each is
  append only later).
- **Solving** is a walk: install a package's missing or too-old dependencies first; refuse with
  a message naming the chain when a repository can't satisfy one. Cycles refused.
- **Manual and automatic**, as apt marks them: what the user named is manual, what came in as a
  dependency is automatic. `pkg remove` refuses a package something installed needs (naming
  them), unless they're removed too; `pkg autoremove` removes automatic packages nothing needs
  any more. Removing `bmds-standard` and then `autoremove` takes the set away.
- The ABI: a package needing a newer `BMDS_ABI_VERSION` than the firmware's is refused ("needs a
  newer BMDS: update the firmware"), and is never offered by `upgrade`.

## 6. Keys and trust

- **The keyring:** the firmware's built-in keys (`keys/dev.pub` in the dev mode, and
  `keys/trusted.pub`), plus keys added on the board: `pkg key add FILE`, `pkg key list`,
  `pkg key remove ID`, kept in `/cfg/pkg/keys` (a key file is `keys/`'s format: `ed25519 HEX
  COMMENT`). Added keys count for packages only; firmware updates keep the keys they were built
  with (a firmware can replace the keyring, so its trust comes from the build).
- **Each source names its keys** (apt's `signed-by`): an index is accepted only if signed by a
  key its source names; with none named, any key in the keyring.
- **Dev-signed packages**, the setting `pkg/dev` (sysconf): `allow` (the default: anyone can make
  and share packages), `ask` (`pkg` asks, saying that anyone could have signed it; `--yes` says
  yes), `refuse`. A firmware in the trusted mode refuses them whatever the setting.
- **Said everywhere:** `pkg info` and the install's confirmation name the key: its comment from
  the keyring and whether it is the dev key, a built-in trusted key, or an added one.
- **What a signature means:** a dev signature shows a package arrived whole and was made by the
  tools, not who made it; trust is the HTTPS server and the sources the user chose. A trusted
  key's signature shows who made it.

## 7. On the card

Every path is a setting (sysconf, `paths-are-settings`):

| Setting | Default | What |
|---|---|---|
| `pkg/root` | `/sd0/p0` | where packages install (`bin`, `apps`, `share`, `etc`) |
| `pkg/db` | `/sd0/p0/var/lib/pkg` | the database: `status` (each installed package: version, manual or automatic, the key that signed) and `NAME.list` (its entries with their hashes) |
| `pkg/cache` | `/sd0/p0/var/cache/pkg` | downloaded indexes and packages; `pkg clean` empties it |
| `pkg/sources` | the project's repository | one source a line: `URL [signed-by=ID,...]`; `file:/sd0/p0/packages` is a folder |
| `pkg/dev` | `allow` | §6 |

`etc/dist/*.files` and `links.sh` go: the database replaces them (not released, no migration).

**Installing one package**, so power loss leaves something `pkg` can finish:
1. check the signature, the header (architecture, ABI) and every section's hash, streaming from
   the file, before writing anything;
2. write `pkg/db/NAME.pending` (the version being installed);
3. each file to `PATH.pkgnew`, checking its hash, then `rename()` over the old (POSIX `rename`
   replaces, `storage.md` §16); directories and links;
4. remove the old version's entries the new one dropped;
5. write `NAME.list` and `status` (each written whole and renamed), remove `NAME.pending`.

A `.pending` left by a power cut makes the next `pkg` command reinstall that package before
anything else (`pkg` says so). A file in use can't be replaced (FatFs refuses, `Busy`): `pkg`
names the program to quit. To check: whether the loader keeps a program's file open while it
runs.

A file in the root that no package owns is never touched. A package file the user changed is
replaced on upgrade and removed on removal, as dpkg does without conffiles; `pkg verify` lists
changed files first.

## 8. Repositories

A repository is a static folder: `index.bmpi` and the packages. Any web server or a card folder.

- **`index.bmpi`:** the same container again (magic `BMPI`), signed. Its first section is text, a
  stanza per package (Debian's `Packages` file): `Package`, `Version`, `Architecture`, `ABI`,
  `Depends`, `Title`, `Summary`, `Category`, `License`, `Homepage`, `Size`, `Installed-Size`,
  `BLAKE2s` (of the whole `.bmpk`), `File` (a URL relative to the index's, or absolute),
  `Description` (one line). Its second is every listed package's icons, each named, so the store
  shows what isn't installed (about 2 KB a package). Several versions of a
  package may be listed; the newest that suits the firmware's ABI wins.
- **Checked:** the index's signature against its source's keys; then each package's hash against
  the index, and its own signature against the keyring (§6), so a file copied from a repository
  onto a card is still checked on its own.
- **The project's repository on GitHub:** the index on GitHub Pages
  (`https://OWNER.github.io/NAME/packages/index.bmpi`), beside the installer, deployed by an
  Actions workflow (no binaries in a `gh-pages` branch); the packages as release assets, named
  absolutely by the index. A release's packages stay downloadable for good, old versions included.
  The mirror at git.howardlogic.com, or anyone's, is another line in `pkg/sources`.
- **From the card:** a folder of `.bmpk` files with or without an index. `pkg install FILE`
  installs a file; `pkg index DIR` (on the board) writes an unsigned index for a folder, which a
  `file:` source accepts because its packages are checked one by one anyway. A release also
  publishes `bmds-X.Y.Z-packages.zip`: every package and the index, a repository to unzip onto a
  card for boards without a network (a Debian CD).

## 9. The command (shell interface: frozen once shipped)

| Command | What |
|---|---|
| `pkg update` | fetch every source's index into the cache |
| `pkg list [--installed\|--upgradable] [PATTERN]` | name, version, installed version, one-line description |
| `pkg search WORDS` | names and descriptions |
| `pkg info NAME\|FILE` | everything: version, size, depends, signer, description, files (`--files`) |
| `pkg install NAME\|FILE...` | with dependencies; shows what will happen and the download size, asks (`-y`) |
| `pkg remove NAME...` | refuses what others need, unless they go too |
| `pkg autoremove` | automatic packages nothing needs |
| `pkg mark auto\|manual NAME...` | as a dependency (`autoremove` may take it), or asked for (added with step 5) |
| `pkg upgrade` | every installed package to the newest its ABI allows |
| `pkg verify [NAME...]` | each file's hash against the database: changed and missing files |
| `pkg clean` | empty the cache |
| `pkg index DIR` | write `DIR/index.bmpi` (unsigned) for a folder of packages (added with step 6) |
| `pkg icon NAME [--size N] -o FILE` | a package's icon, installed or from the lists (added with step 4) |
| `pkg key add\|list\|remove` | §6 |
| `pkg --source SOURCE ...` | one source more than `pkg/sources`, for this command (added with step 10) |

Progress goes to the terminal as a line per package; errors through `Error` with the package
named. Exit status 0, or 1 with nothing changed past the failing package.

**Machine-readable output (the user's, 2026-10-30), for the store (§18) and for scripts:** every
command takes `--json`: JSON Lines on standard output, one object a line, each with `"type"`.
Records: `package` (name, version, installed version, title, summary, category, license,
homepage, size, installed size, depends, signer and its kind, manual or automatic, upgradable),
`file` (`info --files`, `verify`), `plan` (what `install`/`remove`/`upgrade` will do, before
asking: `--json` never asks, so it goes with `-y` or `--dry-run`), `progress` (package, step,
bytes done and total), `done` and `error` (package, `Error`'s name and words). Field names and
record types are append-only, as the shell interface is; consumers skip what they don't know.
Built-in apps parse it with `bmds_core`'s `JsonReader`. `pkg icon NAME [--size N] -o FILE`
writes a package's icon (installed: from the database, which keeps it; else from the cached
index).

## 10. Partitioning and formatting the card

**There already:** `mkfs [-t auto|fat32|exfat] -y /dev/sd0` gives a whole card a new partition
table with one partition (MBR, or GPT from 128 GB), formats it and mounts it at `/sd0/p0`;
`mkfs /dev/sd0p0` formats one partition (`storage.md` §5; verified on the board with a 30 GB
card, 2026-10-04). Automatic format is FAT32 to 32 GB, exFAT above. BMDS reads MBR and GPT.

**Missing, and part of this plan:**
- **A card the board can't use says so on the screen**, not only in the log (today: "holds no FAT
  or exFAT filesystem (mkfs formats it)"). A window: the card's size, what was found (no
  partition table; partitions it can't read, such as a Raspberry Pi card's ext4; a full card that
  isn't BMDS's is fine and left alone), and **Format the card...**, which asks once more, naming
  the size and that everything on it goes, then runs the same `Vfs::format()` as `mkfs`.
- **Tests on scratch cards** (never the user's card): a zeroed card; a Raspberry Pi card
  (MBR, FAT + ext4); a superfloppy (no table); exFAT at 64 GB; GPT at 128 GB or more if one is to
  hand. Each: found, said, formatted, mounted, `pkg install bmds-standard`.
- **Editing partitions** (several partitions, keeping some) stays `storage.md` §14: BusyBox's
  `fdisk`, a package, not needed to start.

## 11. Building packages (host)

- **`tools/mkpkg`** (Python, the standard library only, as `mkupdate`; the signing shared with
  it): `mkpkg make --name --version --root DIR [--depends ...] [--key K] OUT.bmpk`, `mkpkg info
  FILE` (checks and prints), `mkpkg index DIR [--key K] [--base-url URL]` (writes `index.bmpi`).
  Its Ed25519 and BLAKE2s are `mkupdate`'s.
- **`tools/bmds-dist package [NAME...]`:** each recipe's staged files (already kept apart to write
  `NAME.files`) become `build-dist/packages/NAME_VERSION-REVISION.bmpk`; then the index.
  `package.toml` gains `revision`, `depends`, `category`; meta packages are recipes with
  `meta = true` and `depends` only (`dist/bmds-standard`).
- **`tools/bmds-dist install`** (the serial cable) sends packages and runs `pkg install` on the
  board, instead of copying a tree and running `links.sh`.
- **`tools/release`:** the card zip becomes the packages, as release assets, plus the packages
  zip (§8) and the index for Pages.

## 12. Fetching (HTTPS)

The HTTP/1.1 client planned in `update.md` §17 (postponed then): `http://` and `https://`
(mbedTLS over `Socket`, IDF's certificate bundle, the clock from NTP), redirects (GitHub's release
assets redirect to `objects.githubusercontent.com`), `Content-Length` or chunked, `Range` to
resume a download. Parsing in `bmds_core`, host-tested and fuzzed; the connection in `bmds_net`;
mbedTLS's allocations in `Psram`. Downloads go to `NAME.part` in the cache, renamed when whole and
checked. `update fetch` comes with it (§17's design). Flash estimated at about 200 KB, measured
before it lands.

## 13. The first run

With a card in and no packages installed (no `pkg/db/status`), the desktop shows a window:
- the card unusable: §10's;
- otherwise: **Install the standard set** (`bmds-standard` from the configured sources), **Choose
  packages...**, or **Not now**. Without a network, it says so and looks in the card's `packages`
  folder (the packages zip unzipped) before giving up.

The store (§18) follows `pkg`, as the Updater followed `update`; this window becomes its welcome
page.

## 14. Code and budget (estimates, measured as each step lands)

- **`bmds_core`** (portable, host-tested, fuzzed): the `.bmpk`/`.bmpi` head (shared with
  `UpdatePackage`), the manifest, index and icon readers, version comparison, the dependency walk.
- **The kernel** (`bmds_pkg`, new): the database, installing and removing, the sources, the `pkg`
  command. In the firmware, not a program: fetching needs mbedTLS, which is there already.
- **Flash:** `pkg` about 25 KB; the HTTPS client about 200 KB (§12). **RAM:** nothing resident;
  while working, two 64 KiB blocks and the index (`Psram`).

## 15. Steps

1. [x] The key rename (§2).
2. [ ] The card: the unusable-card window and **Format the card...**; the scratch-card tests (§10).
3. [x] The format: `bmds_core`'s parsers and version comparison with host tests and fuzzing;
   `tools/mkpkg`; the key id in `.bmup` too. Done 2026-10-30 (§17).
4. [x] `pkg` from files (done 2026-10-30, §19, tested on the board):
   `install FILE`, `remove`, `list`, `info`, `verify`, the database, links,
   the pending-install recovery, `--json`, `pkg icon`; `bmds-dist package` (titles, summaries,
   categories and icons from each app's `app.desktop` and program icon notes) and `install`
   through `pkg`.
5. [x] Dependencies, manual and automatic, `autoremove`; meta packages; `dist/bmds-standard`.
   Done 2026-10-30 (§20), tested on the board.
6. [x] Repositories from the card: `file:` sources, `pkg index`, `update`, `upgrade`, `search`.
   Done 2026-10-30 (§21), tested on the board.
7. [x] Keys: the keyring, `pkg key`, `signed-by`, `pkg/dev`. Done 2026-10-30 (§22), tested on the
   board.
8. [x] HTTPS fetching (§12), the network sources, `update fetch`. Done 2026-11-01 (§23), tested on
   the board against real servers; the first install from a network repository comes with step 9's.
9. [ ] Publishing: `tools/release`'s packages, the packages zip, the index on GitHub Pages.
10. [x] The first run window (§13). Done 2026-11-01 (§24), tested on the board.
11. [ ] The store (§18).

## 16. Decided (the user's, 2026-10-30)

- **Firmware through `pkg upgrade`** (the `.bmup` listed in the index like a package): later,
  after step 9.
- **The first release** ships packages from the card: steps 1 to 7 and 9's packages zip and
  release assets. The network (step 8, and the index on GitHub Pages) comes in the release after.

- **A store app**, and packages carrying what it shows: a title, a summary, a category, the
  licence, a homepage and icons, in the package and (icons included) in the index. Added to the
  format before anything shipped (§18).
- **The store drives `pkg` through its machine-readable output** (`--json`, §9), not a package
  API: built-in apps use only public interfaces, and this one is scriptable too.

## 17. As built: step 3, the format (2026-10-30)

- **`SignedContainer`** (`bmds/signedcontainer.h`): the head every kind shares. A 24-byte prefix
  (magic, format, the header's size, flags, the section count, the **key id**: the first 8 bytes
  of BLAKE2s-256 of the public key), the kind's own fields, the section table, the signature.
  `verify()` tries only the keys whose id matches and says which one signed.
  `UpdatePackage` is built on it: its header grew from 136 to 144 bytes (format still 1), and
  `update.cpp` takes the signer's index from `verify()` instead of trying keys one by one. A board
  on older firmware reads new `.bmup`s as damaged and needs one flash over the cable.
- **`PkgFile`** (`bmds/pkgfile.h`): the `.bmpk` head (128 bytes: name, version, architecture,
  least ABI, installed bytes, when made) and the `.bmpi` head (72: when made, the package count, a
  label), each field checked; the data blocks (`readBlock()`, `decodeBlock()` through the existing
  `Inflate::raw()`). Names: lowercase letters, digits, `+ - .`; architectures: lowercase letters
  and digits (`esp32p4`, `all`).
- **`PkgVersion`**: Debian's rules, dpkg's `verrevcmp()` over views; epochs allowed. Host-tested
  against 39 pairs answered by the host's own `dpkg --compare-versions` (1.22).
- **`PkgManifest`, `PkgDepends`, `PkgIndex`**: line readers that hand out views into the text.
  Paths are refused if they leave the root (absolute, `.`, `..`, empty components), hold anything
  FAT can't (`" * : < > ? \ |`, control characters, malformed UTF-8), or are too long (512, 255 a
  component). An unknown one-letter entry kind is `NotSupported` (a newer format); unknown fields
  are skipped. Numbers are written one way only (no leading zeros or signs; lowercase hex).
- **Blocks cost nothing measurable:** on BusyBox, Dropbear and the TimGM6mb SoundFont (7.9 MB),
  64 KiB blocks compress to 6,506,954 bytes against 6,502,128 for whole-file DEFLATE (+0.1%):
  DEFLATE's window is 32 KiB anyway.
- **`tools/mkpkg`** (`make`, `info`, `index`, `extract`) and **`tools/bmdscontainer.py`**, the
  container in Python, now also under `tools/mkupdate`. `mkpkg make` walks a tree (directories
  before their contents, sorted), stores a block that doesn't shrink, and refuses paths, names,
  versions and dependency lists the board would refuse. `info` decodes every file against its
  size and hash; `extract` gives back the same tree, links included.
- **Tests:** `tests/host/pkg.cpp` (a package, a meta package and an index made by `mkpkg` in
  `tests/host/pkg`; each head fault; blocks; paths; manifest, dependency and stanza lines, good
  and refused); `tests/host/update/test.bmup` remade; `updatepackage.cpp` for the new layout and
  the key id. **Fuzzing:** `pkg_fuzz`, 35.5 million inputs in 15 minutes (three jobs), nothing
  found; `updatepackage_fuzz` again, 947,000 in 5 minutes, nothing found.
- **Size:** nothing in the firmware calls the package code yet, so the linker leaves it out; it
  arrives with `pkg` (step 4).
- **The store's metadata (2026-10-30, the same day):** the manifest fields `title`, `summary`,
  `license`, `homepage` and `category` as freedesktop.org's main categories (the index's
  `Category` is 1 to 32 letters, digits and `-`); the index stanzas' `Title`, `Summary`,
  `License` and `Homepage`. The icons section (`PkgFile::IconReader`): entries of a name (none in
  a package, a package's in an index), u16 width and height, u32 bytes, the PNG, whose IHDR must
  say the same square size (1 to 64, at most 16 KB). `mkpkg make --title --summary --license
  --homepage --icon`; `mkpkg index` copies each package's icons into the index (2 KB for the test
  package's 16 and 32 pixel `idea`). Host tests for both sections and the new fields; `pkg_fuzz`
  reads icons too: 20.1 million inputs in 15 minutes, nothing found.

## 18. The store (plan, 2026-10-30)

A built-in app, **Software**, in the launcher's System category: what can be installed, what is,
and what to upgrade, on top of `pkg --json` (§9).

- **Browse:** the configured sources' packages as a grid of icons and titles, by category (the
  launcher's: System, Utility, Development, Graphics, AudioVideo, Game, Other), with a search
  field over names, titles and summaries. Meta packages ("sets") at the top.
- **A package's page:** the icon at 32, the title, the summary, the description, the version
  (and the installed one), the download and installed sizes, the licence, the homepage, what it
  needs, and who signed it, the dev key said as such (§6). **Install** / **Remove** /
  **Upgrade**, with what else that brings in or takes out (`pkg ... --dry-run --json`) shown
  before going ahead.
- **Installed** and **Upgrades** views, **Upgrade all**, and **Refresh** (`pkg update`).
- **Progress** from `progress` records, in the page and on the panel button while it works; the
  window can close while a job goes on (it is `pkg`'s, not the app's).
- **The welcome page** is §13's first-run window: the standard set, or choosing.
- **Icons** come from `pkg icon` (or the cached index), decoded with the existing PNG code, and
  kept for the session.
- Built only on the public GUI API and `pkg`, as every built-in app (no OS code knows it).

## 19. As built: step 4, `pkg` from files (2026-10-30)

- **`bmds_pkg`** (new component): `Packages` (`packages.h`), the engine, over the VFS and
  `bmds_core` alone; `PkgCommand` (`pkgcommand.h`), the command, text or `--json`; both built for
  the host tests too. `pkgsystem.cpp` (target only) declares the settings `pkg/root`
  (`/sd0/p0`) and `pkg/db` (`/sd0/p0/var/lib/pkg`), passes the keys the firmware accepts
  (`UpdateKeys`), `CONFIG_IDF_TARGET` and `BMDS_ABI_VERSION`, and registers `pkg`. `pkg/cache`,
  `pkg/sources` and `pkg/dev` come with the steps that use them.
- **The database:** per package `NAME.status` (field lines: version, architecture, ABI, title,
  summary, category, licence, homepage, depends, sizes, when made, signer and key id, automatic,
  counts), `NAME.manifest` (as installed), `NAME.description`, `NAME.icons`, and `NAME.pending`
  while it changes. One file a record, each written whole (`.new`, renamed), so no shared file
  can be half written; the status last, being what says a package is installed.
- **Installing** as §7: everything checked first (signature, architecture, ABI, every section's
  hash, the manifest whole and its sizes adding up, dependencies installed and new enough, paths
  named twice, files another package owns (directories may be shared), unowned files in the way,
  free space), then `NAME.pending`, each file to `PATH.pkgnew` with its hash checked, all renamed
  into place, links made, what the old version had and the new doesn't removed (emptied
  directories deepest first), the records. A failure before the renames removes every
  `.pkgnew` and the pending record: nothing changed. Paths are compared case-blind, as FAT
  names are. A downgrade is said; a reinstall of the same version is allowed.
- **Unowned files in the way:** taken over silently when they hold the package's bytes (a link,
  its target); else refused, naming the file, unless `--overwrite`. `bmds-dist install` passes
  `--overwrite`: a card filled before packages holds their files, owned by none.
- **Power cuts:** every changing operation first finishes what `NAME.pending` says (install the
  named file again, or finish a removal). A file in use (`Busy`) leaves it for the next run; any
  other failure (the file damaged or gone) gives up and says so, so one bad record can't block
  `pkg` for good.
- **One operation at a time** (`Busy` for a second); `list`, `info`, `verify` and `icon` read
  only and don't recover.
- **The command:** `list`, `info NAME|FILE [--files]`, `install [-y] [--dry-run] [--overwrite]
  FILE...` (plans every file first, then asks once), `remove [-y] [--dry-run] NAME...` (refuses
  what a staying package needs, naming it; all named go together), `verify [NAME...]`, `icon NAME
  [--size N] -o FILE`. Installing by name answers that it comes with repositories (step 6).
  `--json` records: `package`, `description`, `file`, `plan`, `progress` (`checking`, `writing`,
  `replacing`, `removing`, with done and total), `note`, `done`, `problem` (verify), `error`
  (`Error`'s name and the words); `--json` never asks.
- **`bmds-dist package [NAME...]`:** each staged package (`build-dist/stage/NAME`) through
  `mkpkg` into `build-dist/packages/NAME_VERSION-REVISION.bmpk`, then `index.bmpi`. The recipe's
  new keys (`dist/README.md`): `title`, `category`, `homepage`, `revision`, `depends`, `icons`.
  Without them: the title from its one application's `app.desktop`, the category from its main
  category, the icons from its program's icon notes (blocks, doom and luckyduck have 16 and 32).
  Data packages are `all`. Every recipe has a title and category now; doom depends on freedoom,
  sshfs and ssh-copy-id on dropbear. The 16 built packages: 33 MB (freedoom's 57.6 MB in 21.8).
- **`bmds-dist install [NAME...]`:** sends the packages, each after what it depends on, to
  `/sd0/p0/packages` with YMODEM and runs `pkg install -y --overwrite` on them. The rootfs and
  `links.sh` stay until the release moves to packages (step 9).
- **Tests:** `tests/host/packages.cpp`, on a tmpfs with packages built in the test (signed by a
  trusted test key) and `mkpkg`'s (the dev key): install, list, info, verify finding a changed
  file and a missing link, remove (emptied directories go, shared ones stay); dependencies needed
  first and kept after, removed together; the icon nearest a size; upgrade (replaced, added,
  dropped files), dry run, downgrade; another package's file refused; an unowned identical file
  taken over, a different one refused then `--overwrite`n; damaged, unsigned, foreign, newer-ABI
  and non-package files refused leaving nothing; an installation and a removal cut short and
  finished, and one whose file is gone given up; the JSON records. Clean under ASan, UBSan and
  leak checks, which found a `memcpy` from null (zero bytes) in tmpfs's first growth of an empty
  file: fixed.
- **Size:** the firmware grew 37.1 KB (2,800,496 to 2,837,616 bytes; 25 KB estimated). No
  internal RAM at rest; working buffers in `Psram` (two 64 KiB blocks, the manifest).
- **On the board** (the user's card, 2026-10-30): `bmds-dist install` sent and installed all 16
  in 12 min 42 s, nearly all of it YMODEM over the serial console (33 MB); `pkg install` of
  netlat takes 0.4 s, `pkg verify` of everything (74.4 MB) 42 s. Remove, reinstall, a
  damaged file refused, a changed file found and an `--overwrite` all behaved. Found and fixed:
  `remove --dry-run` (and the plan before asking) didn't check what needs a package, only the
  removal did (`Packages::checkRemove`, now run on every name before the plan); `list`'s
  columns sized to the longest name and version (coremark's and doom's overran them). Seen, not
  `pkg`'s: the apps service rescans as an install renames its files in, so mid-install it warns
  of an `.app` without its `app.desktop` and then finds it; files get 1980 (FAT) or 1955
  (tmpfs) dates when the clock isn't set (no network).


## 20. Step 5: dependencies, automatic packages, meta packages (plan, 2026-10-30; built as planned)

Before repositories (step 6), the packages to choose from are the files given and the folders
they are in: a card's `packages` folder (`bmds-dist install`'s, or the packages zip unzipped) is
then already a repository without an index, and `pkg install /sd0/p0/packages/bmds-standard_*.bmpk`
installs the standard set. Step 6 adds indexes and names as further places to look; the walk
stays.

- **The walk** (`bmds_core`, `PkgSolver`, pure, host-tested and fuzzed): given what is installed
  (name, version, depends, automatic), the candidates (name, version, depends) and which
  candidates were asked for, it returns the candidates to install in order, each after what it
  needs, each marked asked-for or automatic. A dependency already installed in a version that
  suits stays as it is (nothing is upgraded unasked); else the newest candidate that suits is
  taken. Refused, naming the chain: a dependency nothing provides (`NotFound`, naming it, so the
  caller can look further and walk again), a cycle, a package given twice, and a plan that would
  leave an installed package's dependency unmet (a downgrade below what another needs). An
  explicit stack, not recursion: a crafted chain can't overflow the command's stack.
  `unneeded()`: the automatic packages nothing staying needs, through any chain.
- **The engine:** `Packages::peekFile()` reads what the walk needs (the head, the signature, the
  architecture and ABI, the manifest and its hash; not the data, whose hash `installFile()`
  checks); `planFiles()` peeks the files given, walks, and on a missing dependency looks in the
  files' folders for `NAME_*.bmpk` (each peeked; one that doesn't suit is skipped, and said if
  nothing else is found) and walks again. `InstallOptions::earlier`: the plan's packages before
  this one count as installed for a dry run's dependency check, so `--dry-run` of a set checks
  every package whole. `setAutomatic()` (`pkg mark`), `unneeded()`.
- **The command** (append-only additions to §9): `install` plans first, the dependencies marked
  "(dependency)", then asks once; `remove` then says which automatic packages nothing needs any
  more; `autoremove [-y] [--dry-run]`; `mark auto|manual NAME...`. JSON: `plan` gains
  `automatic` and `file`.
- **Meta packages** were already possible (a package of `depends` alone). `dist/bmds-standard`:
  a recipe with `meta = true`; `bmds-dist` doesn't build it and packages it from an empty tree.
  `bmds-dist install NAME` sends NAME and what it needs, and names only NAME to `pkg`, so the
  rest is automatic as on a real install.
- **Budget:** about 6 KB of flash; nothing resident; the walk's lists in `Psram`.

**As built (2026-10-30):**
- **`PkgSolver`** (`bmds_core`): `plan()` and `unneeded()` as above; refusals in words with the
  chain ("freedoom (>= 0.13-1), needed by bmds-standard -> doom, isn't installed or given",
  "a dependency cycle: a -> b -> a", "installing lib 1 would break app, which needs lib (>= 2)",
  "app needs lib (>= 2); 1 is given", "lib is given twice (1 and 2)"). The same file named twice
  is once.
- **The engine:** `peekFile()`, `planFiles()` (the folder search, a name looked for once; what
  was skipped said with its file: "lib_3.bmpk: lib needs a newer BMDS..."), `InstallOptions::
  earlier`, `setAutomatic()`, `unneeded()`. A package asked for that came as a dependency is asked
  for from then on (apt's way); a dependency upgraded keeps its mark.
- **The command:** `install` plans every time (also with `-y`) and says "(a dependency)" in the
  plan and the `done` lines (JSON: `automatic` in `plan` and `done`, `file` in `plan`); `remove`
  and its dry run name what would be needed no more (`note` records); `autoremove`; `mark`.
- **`bmds-dist`:** `meta = true` recipes (not built, packaged from an empty tree);
  `dist/bmds-standard` 1-1: busybox, dropbear, sftp-server, tzdata, icons, soundfont-timgm6mb,
  angel, blocks, luckyduck, doom (and so freedoom): 71.3 MB installed from 31.7 MB of packages.
  Left out: the benchmarks (benchprog, coremark, netlat), sdk-examples, icons-glyphs. `install
  NAME` sends NAME and what it needs and names only NAME to `pkg`.
- **Tests:** the walk's table (order, automatic marks, what installed suits, the newest that
  suits, the chain, cycles, twice, too old, a breaking downgrade, a 5,000-long chain) and
  `unneeded()`; the engine and command on a tmpfs with a folder of packages (a meta package's
  set found and marked, a dry run of the set writing nothing, `mark`, `remove` naming what is
  needed no more, `autoremove`, skipped and missing dependencies, a breaking plan refused, the
  JSON). Clean under ASan and UBSan. `pkg_fuzz` walks every index's stanzas: 11.0M inputs in 15
  minutes, nothing found (the fuzzer needed the host's memory seam and an assert hook, as
  `List` came in).
- **On the board** (the user's card, 2026-10-30): `bmds-standard` over an installed card
  installs only itself (0.4 s); with doom and freedoom removed, installing doom's file found
  freedoom in `/sd0/p0/packages` and installed it as a dependency (58.6 MB in 57 s; the dry run,
  hashing freedoom's 21.8 MB file whole, 10 s); `remove doom` named freedoom as needed no more and
  `autoremove` took it. The card was left as found (all asked for; `bmds-standard` removed, its
  file kept in `packages`).
- **Size:** the firmware grew 11.2 KB (2,837,616 to 2,848,848 bytes; 6 KB estimated: the
  folder search and the command's new paths cost more than the walk).

## 21. Step 6: repositories from the card (plan, 2026-10-30; built as planned)

Names instead of files: `pkg update` reads every source's index into the cache, and `install
NAME`, `upgrade`, `search`, `list`, `info NAME` and `icon NAME` use what it read. Only `file:`
sources this step; HTTPS (step 8) and `signed-by` (step 7) add to the same code.

- **Settings:** `pkg/sources`, `;`-separated sources (as `net/mounts`), checked when set: now
  `file:/ABSOLUTE/FOLDER`; default `file:/sd0/p0/packages` (the project's HTTPS repository joins
  the default with step 8). `pkg/cache`, default `/sd0/p0/var/cache/pkg`.
- **The cache:** a list per source, `CACHE/lists/ID.bmpi` with `ID.source` naming it (ID: 16 hex
  digits of the source's BLAKE2s). `update` writes them whole (renamed into place) and removes
  those of sources no longer configured; `clean` empties the cache.
- **A `file:` source's index:** the folder's `index.bmpi` when it lists exactly the folder's
  packages (each `File` a name there, with its size, and every `.bmpk` listed); else the board
  indexes the folder itself into the cache and says the folder's index is missing or out of date.
  Signed or not: a file's packages are checked one by one when installed (§8). Its whole-file
  hashes aren't checked against the index for `file:` sources (the package's own signature and
  section hashes are; a download's hash will be, step 8).
- **Unsigned containers:** an index made on a board has a zero key id and signature
  (`SignedContainer::isUnsigned()`); a `file:` source accepts it, a network source won't.
  `bmds_core` gains the writer (`SignedContainer::build()`, `PkgFile::buildIndex()`), host-tested
  by reading back what it writes.
- **`pkg index DIR`** writes `DIR/index.bmpi` the same way (unsigned; `mkpkg index` signs).
  Per package: the head, the manifest, the description and the icons are read, and the whole
  file hashed in one pass (its data is checked when installed).
- **Planning** becomes one function for files and names: the candidates are the files given (and
  their folders, §20) and every cached list's packages that suit (architecture, ABI). A name asks
  for its newest; one installed in that version already is said ("already the newest") and left.
  `upgrade` asks for the newest of every installed package that has a newer one, keeping each
  one's mark; new dependencies come automatic. A list's planned package is then peeked: the file
  must be there and be what the list says (else "the list is out of date: pkg update"), and the
  plan shows who signed it.
- **The command** (§9's, append-only from here): `update`, `index DIR`, `search WORDS...` (every
  word in the name, title, summary or description, case-blind), `list [--installed|--upgradable]
  [PATTERN]` (installed and available together; `--installed` is what `list` was), `install
  NAME|FILE...`, `upgrade [-y] [--dry-run]`, `clean`; `info NAME` and `icon NAME` for a package
  not installed, from the lists. JSON: `package` records gain `available` (the newest version
  in the lists) and `upgradable`; `update` writes `source` records.
- **`bmds-dist install`** also sends the folder's `index.bmpi`, so `pkg update` takes it as is.
- **Code:** the repository side in `bmds_pkg/src/repositories.cpp`; what it shares with the
  engine in a private header. **Budget:** about 12 KB of flash; nothing resident; lists read into
  `Psram` while a command runs (the 17-package list is 9 KB).

**As built (2026-10-30):**
- **`bmds_core`:** `SignedContainer::buildUnsigned()` and `isUnsigned()`; `PkgFile::buildIndex()`
  and `appendIcon()`. Host-tested by reading back (head, hashes, stanzas, named icons, a refused
  signature); `pkg_fuzz` writes every stanza text it reads as an index and reads it back.
- **`bmds_pkg`:** `repositories.cpp` (sources, the cache, `update()`, `indexFolder()`,
  `available()`, `availableIcon()`, `clean()`, and `plan()` for files, names and upgrades, which
  replaced step 5's `planFiles()`); `packagesdetail.h`, the helpers it shares with the engine
  (`Packages::Detail`, formerly the engine's anonymous namespace). `Planned::upgrade`: asked for
  by `upgrade`, so the engine keeps its mark while the plan doesn't call it a dependency. The
  settings `pkg/cache` and `pkg/sources` (checked by `checkSources()`); `Config::now` dates an
  index (0 while the clock isn't set).
- **The command:** as planned. `list` shows a line per package: name, version (`1-1 -> 1-2` when
  upgradable), state (`installed`, `auto`, `upgradable`, `available`), summary; `install NAME`
  that is installed in the newest version says so and does nothing; `info NAME` reads a package
  not installed from its file (so who signed it is known).
- **Tests:** the sources checked; a folder without an index indexed here, then with one made by
  `pkg index` taken as is, then out of date again; `list` (all, `--installed`, `--upgradable`, a
  pattern), `search`, `info` and `icon` of packages not installed; a meta package installed by
  name; "already the newest"; an upgrade keeping an automatic mark; a list whose file is gone or
  holds another version; a folder's non-package left out of its index; the JSON (`source`
  records, `available` and `upgradable`); `clean`. Clean under ASan and UBSan. `pkg_fuzz` 3.1M
  inputs in 15 minutes, nothing found.
- **On the board** (the user's card, 2026-10-30): `pkg update` takes the folder's `index.bmpi`
  (17 packages) in 0.1 s; with a package added (netlat 1-2) it indexes the folder itself,
  hashing all 18 files (33 MB), in 13.3 s; `list --upgradable`, `upgrade` (keeping netlat asked
  for), `search card games`, `info` and `install` of `bmds-standard` by name, "already the
  newest", `pkg index` of a small folder. The card was left as found (netlat 1-1, `bmds-standard`
  removed, the folder's index restored); the cache now holds the folder's list.
- **Size:** the firmware grew 23.2 KB (2,848,848 to 2,872,048 bytes; 12 KB estimated: `list`,
  `search` and `info` over the lists, and the index writer, cost more than planned).

## 22. Step 7: keys (plan, 2026-10-30; built as planned)

- **The keyring:** the built-in keys (`UpdateKeys`: keys/trusted.pub's, and keys/dev.pub's in the
  dev mode), each with its comment from its file (the build passes them on), then the added keys:
  one file each in `pkg/keys` (a setting, default `/cfg/pkg/keys`), `ID.pub` in keys/'s format
  (`ed25519 HEX COMMENT`). Every package check (`load()`) verifies against the whole keyring and
  records who signed: `trusted` (built in), `dev`, or `added` (new in the status file and the
  JSON). Firmware updates keep the built-in keys alone (§6).
- **`pkg key list`** (id, kind, comment; JSON `key` records), **`pkg key add FILE`** (a key
  file's first key line; refused: a key already built in or added, and the dev key, so a
  trusted-mode firmware can't be given it back), **`pkg key remove ID`** (added keys; an ID or a
  unique prefix of at least 4 digits). The firmware knows the dev key in both modes for this
  (`UpdateKeys::devKey()`); a dev key put in `pkg/keys` by hand is skipped.
- **Naming the signer:** `pkg info` and the plan say "signed by COMMENT (a built-in trusted
  key)", "(an added key)", "the dev key (anyone could have)", or "key ID (no longer in the
  keyring)" for an installed package whose key was removed.
- **`pkg/dev`:** `allow` (default), `ask`, `refuse`. `refuse`: the engine refuses a dev-signed
  package wherever it reads one (planning, installing, a power cut's recovery). `ask`: a plan
  holding one asks its own question naming them ("anyone could have made them"), which `-y`
  answers. A trusted-mode firmware has no dev key to accept them with.
- **`signed-by`:** a source may name keys: `file:/FOLDER signed-by=ID[,ID...]`. Such a source's
  index must be signed by one of them (an unsigned index, or a folder indexed on the board, is
  refused, naming the keys; `mkpkg index --key` signs), and a package planned from it must have
  the whole-file hash its index lists (checked when planned: key, then index, then package).
  Without `signed-by`, a `file:` source takes any index (§21); a network source (step 8) will
  take one signed by any key in the keyring. A list's id covers the whole source text, so a
  changed `signed-by` takes effect at the next `update`.
- **`bmds_core`:** `SignedContainer::parseKey()` (a key line: the hex, the comment), host-tested
  and fuzzed.
- **Budget:** about 5 KB of flash; nothing resident.

**As built (2026-10-30):**
- **The build:** `bmds_update_keys()` also writes `BMDS_UPDATE_KEY_COMMENTS` (each key's comment,
  a C string; `file(STRINGS)` hands a `;` over as `\;`, put back) and `BMDS_UPDATE_DEV_KEY`
  (keys/dev.pub's key in either mode); `UpdateKeys::comment()` and `devKey()`.
- **`bmds_core`:** `SignedContainer::parseKey()`: `ed25519`, 64 hex digits of either case, then
  a comment of clean UTF-8 (at most 255 bytes). Host-tested; `pkg_fuzz` reads every line as one.
- **`bmds_pkg`:** `keys.cpp` (`keyring()`, `addKey()`, `removeKey()` by id or a unique prefix,
  found by its key whatever the file is called, `describeSigner()`; `verifyHead()` and
  `verifyWith()` for the engine and the sources). `Signer::Added`. `load()` verifies against the
  keyring and refuses a dev-signed package under `pkg/dev refuse` (`PermissionDenied`). Sources
  read by `parseSource()` (`signed-by=`, at most 8 ids); a strict source's index is verified at
  `update` and its packages' whole-file hashes when planned. Settings `pkg/keys` (`/cfg/pkg/keys`)
  and `pkg/dev` (`allow|ask|refuse`).
- **The command:** `key list|add|remove` (JSON `key` records, `done` for add and remove); the
  signer named in plans ("signed by Claude's test key (an added key)") and `info` ("Signed: the
  dev key (anyone could have; key 1edc0f2cc301cc37)"); JSON `signer` may be `added`, and `package`
  records gain `keyName`. `pkg/dev ask`: after "Go ahead?", "Signed by the dev key, so anyone
  could have made them: NAMES / Install them anyway?".
- **Tests:** the keyring listed; a package of an unknown key refused, then installed once its key
  is added, named in the plan, `info` and JSON; add refusing a key twice, the dev key and a file
  without a key; remove refusing a built-in key, a short id and an unknown one; a dev key put in
  `pkg/keys` by hand skipped; a removed key's package said to be signed by a key no longer in the
  keyring; `pkg/dev` refuse and ask (answered no, then yes; `-y`; none asked of trusted ones);
  `signed-by`: no index or the board's refused, mkpkg's dev-signed index taken, another key's
  refused (and the old list gone), a tampered package refused by its hash, then the good one
  installed; the setting's refusals. Clean under ASan and UBSan. `pkg_fuzz` 4.2M inputs in 15
  minutes, nothing found.
- **On the board** (the user's card, 2026-10-30): `key list` with the built-in dev key's comment;
  a package signed by a key made on the PC refused, its key added, installed and named, removed;
  the dev key refused by `key add`; `pkg/dev refuse` refusing netlat (dev-signed) and not the
  added key's package; `signed-by=1edc0f2cc301cc37` taking the folder's index (17 packages) and
  another id refusing it; a malformed `signed-by` refused by `sysconf set` (as "Invalid value":
  sysconf's checks can't give a reason). Settings put back unsaved (`setdef`), the test key, its
  folder and `/cfg/pkg` removed.
- **Size:** the firmware grew 9.7 KB (2,872,048 to 2,881,728 bytes; 5 KB estimated).

## 23. Step 8: fetching over HTTP(S) (plan, 2026-10-31; built as planned)

As §12 and update.md §17 planned, with what the code since has decided:

- **`bmds_core`, `Http`** (`bmds/http.h`; host-tested and fuzzed, `http_fuzz`): URLs (`http:` and
  `https:`, host, port, path; a reference resolved against a base, RFC 3986's way, for redirects
  and an index's `File`), the request (`GET`, `Host`, `User-Agent`, `Range: bytes=N-`,
  `Connection: close`), and the response parser: the status line, the headers it needs
  (`Content-Length`, `Transfer-Encoding: chunked`, `Location`, `Content-Range`), the body by
  length, by chunks or to the end, handed out as views of what was fed (no copy), a line at most
  16 KB and a head at most 64 KB.
- **`bmds_net`, `Http::get()`** (portable, `httpclient.cpp`): one request a connection over
  `Socket` (resolve, connect, send, parse), up to 5 redirects, a `Range` to resume (a server that
  answers 200 instead restarts the body: the sink is told where the body starts), timeouts
  (connect and each read, 30 s), Ctrl-C through the socket's interrupt. Its surroundings are
  passed in (`Http::Environment`: resolve, prepare a socket, open TLS), so the host tests run it
  over `lwipwire`'s two interfaces against a server thread. **TLS** (`httptls.cpp`, target only):
  mbedTLS 3.6 over the socket (its BIO), IDF's certificate bundle, the host name checked (SNI too),
  TLS 1.2; refused while the clock isn't set ("HTTPS checks certificates against the time"). A
  failed check said in words (expired, not trusted, the wrong name). mbedTLS's allocations go
  through `Memory` in `Psram` (`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`; nothing else uses mbedTLS at run
  time).
- **Packages:** `Config::fetch` (a function; the firmware's is `Http::get()`, the host tests' read
  a tmpfs "web"). Network sources `https://HOST/PATH [signed-by=...]` (and `http:`): `update`
  fetches `PATH/index.bmpi` into the cache (`ID.part`, renamed once checked); its signature must
  be by a key the source names, or else by any key in the keyring: never unsigned. A list's
  `File` resolves against the index's URL. `install`/`upgrade` plan as before, the network ones
  unread (who signed them shows after they arrive), say what will be downloaded, ask, then fetch
  each into `CACHE/packages/NAME_VERSION.bmpk` (`.part` resumed by `Range`, the whole-file hash
  checked against the signed list, then peeked as a list's file is), then `pkg/dev ask`, then
  install. Fetched packages stay until `pkg clean` (apt's way: a reinstall needs no network).
  `Step::Fetching` for progress.
- **`update fetch [--check] [URL]`** and the setting `update/url` (update.md §17's design), through
  a weak hook the main firmware defines (recovery has no network, and no room for TLS).
  The Updater's buttons wait for the store (step 11).
- **The default source** stays `file:/sd0/p0/packages` until the project's index has a public URL
  (GitHub Pages, step 9; the user names the account).
- **Budget:** measured on the build: TLS, X.509 and the certificate bundle were estimated at about
  200 KB of flash (1.1 MB of the 4 MB app partition is free); a connection's RAM (mbedTLS's 16 KB
  in, 4 KB out, the certificate chain) in `Psram` while it lasts; nothing resident.

**As built (2026-10-31):**
- **`bmds_core`:** `Http` (`http.h`): `parseUrl()`, `resolve()` (RFC 3986 §5.4's examples pass),
  `buildRequest()`, `ResponseParser` (also a 416's `Content-Range: bytes */SIZE`). `fetch.h`: the
  `FetchSink` and `FetchFunction` that `pkg` and `update` share, and the weak `systemFetch()`.
- **`bmds_net`:** `Http::get()` (`httpclient.cpp`, portable) and `Http::openTls()` (`httptls.cpp`,
  target): a 16 KB receive buffer in `Psram`; TLS refused while the clock isn't set; certificate
  failures in words; `esp_mbedtls_mem_calloc/free` through `Memory` (`CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC`,
  `CONFIG_MBEDTLS_TLS_CLIENT_ONLY`). PSRAM isn't encrypted on this chip: a session's keys sit in
  it while it lasts (the packages are signed whatever the transport; HTTPS is for servers that
  require it).
- **`bmds_pkg`:** `fetchsystem.cpp` (target) defines `systemFetch()`: `Http::get()` with
  `NetStack::resolve()`, `Http::openTls()` and `User-Agent: BMDS/VERSION`. Network sources in
  `repositories.cpp` (`updateNetwork()`, remote packages in the lists and the plan,
  `fetchPlanned()`, `clean()` also of `CACHE/packages`); `Step::Fetching`. The plan names what
  will be fetched and from which host ("install lib 2, 5 bytes, 431 to fetch from web"); JSON
  `plan` gains `fetch`, and `signer` is empty until fetched; then "fetching NAME VERSION (N
  bytes)", the dev question, installing.
- **`update fetch [--check] [URL]`** and `update/url` (`updatecommand.cpp`); `Update::checkHead()`
  shares the head's checks with `check()` (split out of `checkPackage()`). Recovery links it with
  `systemFetch` null: "this firmware can't fetch (recovery has no network)".
- **Tests:** `tests/host/http.cpp` (URLs, references, the request, responses whole and a byte at a
  time, interim, ranges, 416, refusals, limits); `tests/host/httpclient.cpp` (`Http::get()` over
  lwipwire against a server thread: 100 KB, a relative redirect, a range kept, one ignored, a
  file whole already, 404, a body cut short, not HTTP, a redirect loop, no TLS, no such host, a
  connection refused; lwIP's heap back where it was); `packages.cpp` with a fake web on the tmpfs
  (an index signed by the test key: update, a dry run fetching nothing, the plan, install, kept
  in the cache, a fetch cut short and resumed from its `.part` (the server asked from byte 100),
  a server ignoring the range; refused: a package changed since its index was signed, an unsigned
  index, one signed by a key not in the keyring, one not by the keys named, a 404, no fetching).
  `http_fuzz` (new): 20.5M inputs in 15 minutes, nothing found.
- **Size:** the firmware grew 201 KB (2,881,728 to 3,082,880 bytes; 200 KB estimated): mbedTLS's
  TLS 88 KB (69 KB of it the certificate bundle), its crypto 78 KB, X.509 8 KB; the client and
  `pkg`'s network code the rest. About 0.7 KB of internal RAM (mbedTLS's static data). Recovery:
  1,245,616 bytes of its 1,441,792.
- **Found on the way:** lwIP's heap was allocated 4 bytes short on the board and in the host
  harness (its end marker past the block; network.md, "lwIP's heap: its real size"), found when
  AddressSanitizer first ran lwIP; fixed (`BMDS_LWIP_HEAP_BYTES`), the host's `MEM_ALIGNMENT` 8.
  The whole host suite under the sanitizers also stopped at two test helpers (an `int` sum that
  overflows in `bench.cpp`, a `memcpy` from an empty vector in `decoder.cpp`): fixed. It then
  stops in dr_flac (misaligned 16-bit stores into a buffer the audio code passes): not this
  step's, left for the audio code. Packages, HTTP, sockets and the network's tests (92 cases) are
  clean.
- **On the board, without its network** (the cable unplugged, 2026-10-31): `update fetch` without a
  URL, and with one ("the network isn't up (no DNS server yet)", after a first wording of "Not
  ready"); a network source beside the card's in `pkg update` (the card's updated, the network's
  error said, the status 1).
- **On the board, on the network** (2026-11-01):
  - `update fetch --check` of a GitHub release asset: github.com's redirect to
    release-assets.githubusercontent.com, both chains checked against the bundle, the head read
    and refused ("not an update package"). badssl.com: wrong.host refused for its name;
    untrusted-root, self-signed and expired refused as not from a trusted authority (the bundle
    finds no root for expired's chain before its dates are looked at). example.com's 404, and an
    unknown host, said as such.
  - A whole fetch: 33.6 MB in 38.5 s (0.87 MB/s), its SHA-256 the PC's. Cut to 20,000,001 bytes
    as a `.part`, fetched again: resumed through the redirect (the storage host's 206), the same
    hash. The PC fetched the same in 3.7 s; the card writes 3.6 MB/s. The limit is TCP's window
    (network.md's 16 segments, 23 KB, no scaling) over a ~25 ms round trip: 0.93 MB/s at most.
  - `pkg update` with an `https:` source beside the card's: the card's updated, the network's 404
    said once.
  - Fixed from it: IDF's bundle logged to the console in a command's output (its tag now off: the
    error says it); a refusal by the bundle came as "the TLS handshake failed (mbedTLS -0x3000)"
    (mbedTLS turns a callback's refusal into `X509_FATAL_ERROR`, flags -1), now in words; the
    report of a head that isn't a package printed empty fields and the redirect's signed URL, now
    the URL asked for and the verdict; `pkg update` named a network source's URL twice (the
    fetcher's message names it, and the host tests' fake fetcher now does too).
  - The clock after plugging the cable in: DHCP's address 29.6 s after link-up (lwIP's discovers
    at 0, 2, 6, 14 and 30 s, the fifth answered: a switch port's spanning tree holds frames 30 s),
    NTP's step 32 ms after the address. After a reset, address 0.27 s and NTP 0.30 s from boot.

## 24. Step 10: the first run (plan, 2026-11-01; built as planned)

§13's window, for a card that is usable (step 2 does the unusable one).

**The program:** `/bin/welcome [--first-run]`, a built-in GUI program in a new component,
`bmds_software`, where the store (§18, step 11) will live too. It drives `pkg` as the store will:
`/bin/sh -c 'pkg ... --json'` started through the program's launcher, its JSON Lines read on a
worker thread and handed to the window on the loop (`PkgJob`, shared with the store). Toolkit
only; no OS code knows it.

**Starting it:** the default `/cfg/autoexec.sh` runs `welcome --first-run &` after the desktop,
when a card is mounted (either branch: the card's own `autoexec.sh` or the built-in desktop).
With `--first-run` it leaves quietly (status 0, no window) when:
- the setting **`pkg/welcome`** (new, bool, default on) is off;
- `pkg list --installed --json` lists anything (a card with packages isn't new);
- the window system isn't running.
Without `--first-run` it always opens (for trying it, and later from the store).

**The window:**
- **Start page:** "Welcome to BMDS"; "Nothing is installed on the card yet." The standard set's
  summary (its package's), how many packages and how much room it takes (from
  `pkg install --dry-run --json bmds-standard`), and where they come from. Buttons:
  **Install the standard set**, **Choose packages...**, **Not now**. A box **Show this when
  BMDS starts** (ticked); unticking it sets and saves `pkg/welcome`.
- **While it looks:** `pkg update --json`, then the dry run. A source that failed (a `source`
  record with an `error`: no network, a server down) is said in a line. If the standard set
  isn't found and **`PKG_ROOT/packages`** (the packages zip unzipped onto the card) exists and
  isn't a configured source already, everything is done again with **`--source
  file:PKG_ROOT/packages`** (a new `pkg` option: one source more, for this run only), and the
  window says it uses the card's folder. Found nowhere: it says so, names the sources and the
  folder, and only **Not now** works.
- **Choose packages...:** every package not installed (`pkg list --json`, with the same
  `--source`), a box each with its title and size and its summary below, sets first; the
  standard set's members ticked. **Install** (with the count and size ticked) and **Back**.
- **Installing:** `pkg install -y --json ...`: a bar over the packages planned (each `done`
  record one step on, `progress` records in between), "Installing NAME (N of M)...". At the
  end: "Installed. The new programs are in the launcher." and **Close**; or the `error`
  record's text, with **Close** and the start page's buttons back. The window can't be closed
  while `pkg` runs (as the Updater while installing).

**`pkg --source SPEC`** (shell interface: added): adds SPEC to `pkg/sources` for this command;
its list is cached like any source's (`pkg clean` empties it). For the window and for scripts.

**Tests:**
- **Host:** `PkgJob`'s line parsing into records (every record type `pkg` writes, unknown fields
  and types skipped, a line cut at the end); `pkg --source` in the packages tests (a folder not in
  `pkg/sources` found through it, not after).
- **Board**, never on the user's own database: `pkg/root`, `pkg/db` and `pkg/cache` set (not
  saved) to `/sd0/p0/claude/firstrun/...`; `welcome --first-run` opens; the standard set from
  `/sd0/p0/packages` (as `pkg/sources`, then through the fallback with `pkg/sources` pointed at
  a missing folder); Choose packages with two picked; `--first-run` leaving quietly once
  something is installed and with `pkg/welcome` off; screenshots of each page. The settings set
  back afterwards, the scratch folders removed.

**As built (2026-11-01):**

- **`bmds_software`:** `/bin/welcome` (`welcomeapp.cpp`) and `PkgJob` (`pkgjob.h`): `/bin/sh -c
  'pkg --json ...'` started with `waited`, its stdout a pipe read on a worker thread, each line
  handed to the loop and waited for (the window reads records as `pkg` writes them), the exit
  status from `waitChild()`. The records are read by **`PkgRecord`** (`bmds_pkg`, portable,
  host-tested): every type and field `pkg` writes, unknown ones passed over.
- **The window's work:** `pkg update`, `pkg list` (what is offered, the set's summary and
  members), `pkg install --dry-run bmds-standard` (the plan's count and size); then `pkg install
  -y`. A first run asks `pkg list --installed` first and shows nothing if anything is installed.
- **Fixed in `pkg` on the way:**
  - **A dry run read every package's data** to hash it: 13.5 s for the standard set's 68 MB before
    the window could say anything. It now loads them `Check` (every check but the data's hash,
    which installing does anyway): 0.69 s. A host test: a package with damaged data passes the
    dry run and the install refuses it.
  - **`install -y --json` wrote no `plan` records** (only a question or a dry run did), though §9
    says `--json` writes the plan first with `-y`, as `remove` and `autoremove` already did. The
    window's bar had nothing to count. Host-tested.
- **Fixed in the toolkit:** a **default button's text** started a column late (its outline's pixel,
  rounded up to the next column) while its size hint counted from the edge, so a long label lost
  its last character ("Install the standard se"). The text now starts from the button's edge.
- **Paragraphs:** `BoxLayout` doesn't ask height for width, so the window's paragraphs are a
  `TextBlock` whose hint is its height at the width it was given (the column laid out twice).
- **`pkg/welcome`** (bool, on): the box **Show this when BMDS starts** sets and saves it.
- **The default `/cfg/autoexec.sh`** runs `welcome --first-run &` after the desktop. The defaults
  installer doesn't replace an existing `autoexec.sh`, so a board set up before this keeps its
  own until the user takes the new one (removing `/cfg/autoexec.sh` and its line in
  `/cfg/.defaults` brings it in at the next boot).
- **On the board**, on scratch `pkg/root`, `pkg/db` and `pkg/cache` under `/sd0/p0/claude` (set,
  not saved; reverted afterwards, the folders removed; the user's 16 packages untouched):
  - `welcome --first-run` on an empty database: the start page within 4 s ("12 packages, taking
    68.0 MB"); **Install the standard set**: all 12 installed in about two minutes, the bar by
    package, "Installed: 12 packages, 68.0 MB.";
  - `--first-run` again: gone in 0.05 s (something installed); with `pkg/welcome` off, at once;
  - forced (`welcome`) on that card: "It is installed already", that button off; **Choose
    packages...** lists only what isn't installed; two ticked (712 KB), installed;
  - `pkg/sources` pointed at a missing folder, the packages in `PKG_ROOT/packages`: "From the
    card's folder ...: the sources didn't answer", the standard set offered from it;
  - the folder taken away too: "The standard set wasn't found in the sources (pkg/sources: ...) or
    in ...", only **Not now** works.
- **Not tried:** a real network source failing (none is configured until step 9); the window at a
  real boot with a new card (it runs from the new `autoexec.sh`, which this board doesn't have).

## 25. Step 9: publishing (plan, 2026-11-01)

**Where (the user's, 2026-11-01):** the repository **`jthwho/bmds`** on GitHub, created by the user.
It isn't a mirror: `git.howardlogic.com` stays the only home of the work, and `tools/release
--publish` pushes the release's commit and tag to GitHub and makes the release with `gh`. One
repository serves the code, the Releases (the files) and Pages (the installer and the package
list, deployed from Actions, so no `gh-pages` branch).

- Pages: `https://jthwho.github.io/bmds/`: the installer (`installer/`), `firmware/latest.json`
  and the latest release's flash images, `packages/index.bmpi`.
- Releases: `https://github.com/jthwho/bmds/releases/download/vX.Y.Z/...`: everything else.

**What a release holds now** (`release.md` §3, the card zip replaced by packages):

| Asset | What |
|---|---|
| `NAME_VERSION.bmpk` | every package `bmds-dist package` makes |
| `index.bmpi` | the package list, signed (the dev key), each `File` an absolute URL to this release's asset; the Pages workflow puts it at `packages/index.bmpi` |
| `bmds-X.Y.Z-packages.zip` | `packages/`: the packages and their index with relative names, to unzip onto a card for a board without a network (`file:/sd0/p0/packages`, the first-run window's fallback) |
| `bmds-X.Y.Z-p4r1-flash.zip`, `bmds-X.Y.Z-p4r1.bmup` | as before |
| `bmds-p4r1.bmup` | the same update package under a name without the version, for `releases/latest/download/` |
| `bmds-X.Y.Z-sources.zip`, `SHA256SUMS`, the notes | as before |

**Defaults:**
- `pkg/sources`: `https://jthwho.github.io/bmds/packages;file:/sd0/p0/packages` (the network, and
  the packages zip unzipped onto the card).
- `update/url`: `https://github.com/jthwho/bmds/releases/latest/download/bmds-VARIANT.bmup`, the
  variant the firmware is built for (`p4r1`, or `p4r3` without `ESP32P4_SELECTS_REV_LESS_V3`).

**`tools/release`:**
- `--repo` defaults to `jthwho/bmds` (`$BMDS_GITHUB_REPO` overrides), and `--remote` to `github`
  (a git remote the user adds once: the script says how if it is missing).
- Builds the packages (`bmds-dist build`, `package`) instead of the card zip; the second index
  with `mkpkg index --base-url`.
- `--publish`: checks `gh auth status`, pushes `HEAD` to the remote's `main` and the tag (never
  forced), then `gh release create vX.Y.Z --verify-tag` with every asset.
- **`tools/release site --from DIR --out SITE`:** the Pages site from a release's files (the flash
  zips unpacked into `firmware/VERSION/VARIANT/`, `latest.json`, `index.bmpi` into `packages/`) and
  `installer/`. The workflow runs it; so can anyone, to look at a site before publishing.

**`.github/workflows/pages.yml`:** on a published release (and by hand): checks out the tag,
downloads the release's flash zips and `index.bmpi` (`gh release download`), runs `tools/release
site`, and deploys with GitHub's Pages actions (`configure-pages`, `upload-pages-artifact`,
`deploy-pages`). The repository's Pages source must be "GitHub Actions" (the user, once).

**Checked here:** a `--dev` release (all assets made, the indexes checked by `mkpkg info`), and its
site (`tools/release site`) laid out as the installer expects. **First on GitHub:** the push, the
release and the workflow can only be tried there; the first real release (`release.md` step 8)
or a trial the user agrees to.

**As built (2026-11-01), all but the GitHub side:**
- `tools/release` as planned; `.github/workflows/pages.yml` as planned.
- **A `--dev` run** made every asset: 19 packages (from `bmds-standard`'s 560 bytes to
  `freedoom`'s 21.8 MB), `index.bmpi` (11 KB, each `File` at
  `https://github.com/jthwho/bmds/releases/download/v0.1.0/...`, checked by `mkpkg info`),
  the packages zip (33.9 MB: `packages/` and its relative index), the flash zip, the `.bmup` under
  both names, the sources zip. `tools/release site` laid its site out: `installer/`'s pages,
  `firmware/VERSION/p4r1/` (the images, `flash.json`), `firmware/latest.json`,
  `packages/index.bmpi`.
- **On the board:** `pkg/sources` and `update/url` show the new defaults (nothing saved on this
  board overrides them); `pkg update` says the project's index answers 404 (nothing published
  yet) and reads the card's folder as before.
- **Not tried until the GitHub side exists:** `--publish` (the push and `gh release create`), the
  workflow, and a board reading the published index and downloading a package from a release.
  `gh` isn't installed on this PC yet.

**GitHub set up (2026-11-01, the user's go-ahead):**
- `gh repo create jthwho/bmds --public` (homepage `https://jthwho.github.io/bmds/`), the remote
  `github` (`git@github.com:jthwho/bmds.git`, SSH as `gh` is set up), and `main`'s whole history
  pushed (255 commits; the 2,475 Git LFS objects, 9 MB, uploaded with it). Actions are on.
- Pages: `POST repos/jthwho/bmds/pages build_type=workflow`.
- **The `github-pages` environment lets only `main` deploy** by default, and a release's workflow
  runs on its tag (`refs/tags/vX.Y.Z`), so its deploy would be refused. A tag rule `v*` was added
  (`POST .../environments/github-pages/deployment-branch-policies name=v* type=tag`).
- The workflow file isn't on GitHub until the commit holding it is pushed (the first release's
  `--publish` does that). Nothing in `installer/` is in LFS (`actions/checkout` doesn't fetch LFS
  files by default; the site would get pointers if it ever were).
- **Network installs from a GitHub release, tried (2026-11-01, the user's go-ahead):** a temporary
  pre-release `pkg-test` on `jthwho/bmds` held four packages (coremark, dropbear, ssh-copy-id,
  tzdata: 660 KB) and an index naming them by their download URLs (`mkpkg index --base-url`). On
  the board, on a scratch `pkg/root`/`db`/`cache` with `pkg/sources` set (not saved) to the
  release's download folder: `pkg update` 1.5 s ("4 packages"); `pkg install -y ssh-copy-id`
  fetched it and its dependency dropbear through GitHub's redirect and installed both in 4.6 s;
  tzdata and coremark in 3.7 s; `pkg verify`: every file matches. Then the settings reverted, the
  scratch folder removed, and the pre-release deleted with its tag (`gh release delete
  --cleanup-tag`). Still to try with the first release: the index from Pages.
