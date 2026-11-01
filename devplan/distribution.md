# Distribution: the programs BMDS ships on the card

Status: **structure in place** (2026-10-04), one package: BusyBox. The rest gets added as needed; kernel support and drivers come first (`roadmap.md`).

## 1. Goal

A small, reproducible "distribution": the third-party programs BMDS runs, built from pinned upstream sources with the SDK, installed on the card as a root filesystem with the usual layout. Adding a program is adding a recipe: a few lines of metadata and a build script, as Buildroot packages or Alpine's APKBUILDs.

Not goals: a package manager on the device, binary packages, dependency solving beyond build order. (Later, maybe: a package index the board can fetch from once there is networking.)

## 2. Layout

**In the repository** (`dist/`):

```
dist/README.md                  how to add a package
dist/<package>/package.toml     name, version, source URL, sha256, license, license files, description
dist/<package>/build.sh         configure, build and install into $DESTDIR (bash, run by bmds-dist)
dist/<package>/patches/*.patch  applied in order with patch -p1 (optional)
dist/<package>/...              anything else the build uses (BusyBox: its .config)
tools/bmds-dist                 the builder and installer
```

No sources in the repository: `bmds-dist` downloads each release once into `~/.cache/bmds-dist/sources/`, checks its sha256, and unpacks a fresh copy for every build. The recipe, its patches and its configuration plus the pinned upstream release are the corresponding source (which GPL programs such as BusyBox require to be offered with their binaries).

**On the card** (the root is the first partition, `/sd0/p0`, which is already `$HOME` and whose `bin` is the default `$PATH`):

```
bin/                            programs and their links (busybox, and awk -> busybox ...)
etc/dist/<package>.files        what each package installed (for updates and removal)
etc/dist/links.sh               the links to make, as ln -sf commands (see §4)
share/licenses/<package>/       each package's license files
```

## 3. Building: `tools/bmds-dist build [package...]`

1. **The SDK:** configures `build-dist/sdk` from `sdk/examples` with the toolchain file and builds the target `bmds_libs` (musl's `libc.a`, `libbmds.a`), which also writes `bmds-cc`.
2. **Each package**, in the order given (all of them by default):
   - fetch and check the source, unpack it into `build-dist/src/<package>-<version>`, apply the patches;
   - run `build.sh` with: `SRC` (the source), `BUILD` (an empty build directory, for out-of-tree builds), `DESTDIR` (`build-dist/rootfs`), `PKGDIR` (the recipe), `CC` (`bmds-cc`), `AR`, `STRIP`, `OBJCOPY`, `HOSTCC`, `BMDS_MAKE` (the `build` wrapper when it is on `PATH`, which paces parallel jobs; else `make -jN`; not `MAKE`, which would replace make's own in recursive builds), `JOBS`, and `BMDS_STACK` from the recipe;
   - afterwards: every ELF program in `$DESTDIR/bin` is stripped as the loader needs (`objcopy --strip-unneeded`, which keeps the relocations), the license files are copied, and the package's file list and links are recorded.
3. **The result**, `build-dist/rootfs`, is the card's tree. Links are real symbolic links on the PC.

The builder uses Python's standard library only (3.11 or newer, for `tomllib`).

## 4. Installing

*Since 2026-10-30 through packages (`packages.md` §19): `bmds-dist package` makes a `.bmpk` of
each, and `bmds-dist install` sends them and runs `pkg install` on the board, which makes the
links itself. What follows is how it was, kept while the release still ships the tree.*

- **Over the serial cable:** `tools/bmds-dist install [-p port] [--root /sd0/p0]` creates the directories, sends the files with YMODEM (as `bmds-run` does), then has the board make the links (`sh etc/dist/links.sh`). A full BusyBox install takes about 10 s.
- **With the card in a PC:** copy `build-dist/rootfs` onto it, links excepted, then run `sh /sd0/p0/etc/dist/links.sh` once on the board. Emulated links need the FAT System attribute, which tools on a PC don't set (Cygwin's `cp` excepted); the board makes them itself.
- Installing again replaces files; links are remade with `ln -sf`.

## 5. Packages

| Package | Version | What | Notes |
|---|---|---|---|
| `busybox` | 1.37.0 | Unix tools: coreutils, `sed`, `awk`, `vi`, `less`, `diff`, `find`, `grep`, `tar`, compressors, `hush` | `CONFIG_NOMMU`; applets that need Linux interfaces BMDS lacks are off (§6). One binary, a link per applet (119). Patch: `timeout`'s no-MMU watcher re-execs with `-p` before the duration (upstream's never saw it, so nothing was ever timed out). Patch: `vi`'s registers `"+` and `"*` are `/dev/clip0` and `/dev/clip0p`, as Vim's are the clipboard and the primary selection (ordinary registers where those files don't exist; `clipboard.md` C12). `less` keeps upstream's line limit (`FEATURE_LESS_MAXLINES`; the config had 0, which showed one line). The BMDS shell's built-ins (`ls`, `cat`, `echo`...) still come first; `busybox ls` or the full path reaches BusyBox's. |
| `dropbear` | 2025.88 | SSH: the server, the client (`ssh`, `dbclient`), `scp`, `dropbearkey` | One program (`dropbearmulti`) with links. Built into `/rom` by the firmware's build (`network.md` §24a) as well as here. Patch: the server's accept loop `vfork`s and execs itself (re-exec mode) instead of forking; patch: sessions get no fixed `PATH`. Passwords from `/etc/shadow`. |
| `sftp-server` | 1 | Our SFTP server (`programs/sftp-server`), Dropbear's `sftp` subsystem | In `/rom` too; fuzzed (`tests/fuzz`). |
| `netlat` | 1 | Real-time network latency (`programs/netlat`) against `tools/netlat-reflector` | `network.md` §30i. |

Candidates, roughly in order: a scripting language (Lua or MicroPython), `zlib`/`xz` as libraries, `make`, `sqlite`, TinyCC (the first step to self-hosting), games.

## 6. BusyBox's configuration

`dist/busybox/config` is a complete `.config`: everything off (`allnoconfig`), then the applets and features that build against the SDK and work on BMDS. Process creation and signals (steps 8 and 9) turned on `xargs`, `find -exec`, `time`, `nohup`, `kill`, `timeout` and `watch`. Off, and why:
- needs Linux interfaces: `mount`, `df` (`statfs`), `ps`/`top`/`free`, `pidof`, `killall` (`/proc`), `init`, modules, networking, `login` and users, `dmesg`, consoles and keyboards;
- `CONFIG_BUSYBOX_EXEC_PATH`, used to re-run itself: `/sd0/p0/bin/busybox`.

## 7. Tests

`bmds-dist build` on the PC (it runs the SDK build and every recipe); on the board, the corpus of `tests/compat` stays for ABI checks, and the distribution gets a smoke test (`tools/bmds-dist install`, then a few applets through their links).
