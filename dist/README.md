# The BMDS distribution

Third-party programs BMDS ships on the card, built from pinned upstream
releases with the SDK (design: `devplan/distribution.md`).

```bash
. ~/esp/esp-idf/export.sh           # the compiler (and pyserial, for install)
tools/bmds-dist list
tools/bmds-dist build [package...]  # into build-dist/rootfs
tools/bmds-dist package [package...] # into build-dist/packages (.bmpk), with an index
tools/bmds-dist install [package...] # over the serial cable to /sd0/p0/packages, then pkg install
```

`--out DIR` builds into DIR instead of `build-dist`; `--nested` is for running inside another
build (the firmware's, which puts `dropbear` and `sftp-server` in `/rom`): no `build` wrapper,
whose lock the outer build holds, and one job at a time.

## Adding a package

Make `dist/<name>/` with:

- **`package.toml`:** `name` (the directory's), `version`, `description`,
  `license` (SPDX), `source` (the release tarball's URL, or a zip's), `sha256`,
  `license_files` (paths in the source to install under `share/licenses/<name>`),
  optionally `title` (the name the store shows; default its one application's `Name`, else
  the package's), `category` (freedesktop.org's main category; default its application's),
  `homepage`, `revision` (the package's own revision of this upstream version, default 1: the
  package is `VERSION-REVISION`), `depends` (packages it needs, `["dropbear"]` or
  `["busybox (>= 1.37.0-1)"]`), `icons` (16 and 32 pixel PNGs, paths from the repository's root;
  default the icons built into its one application's program), `stack` (bytes the program's stack must have; default the
  kernel's), `display` (true: its programs draw to the screen continuously, so
  the kernel starts them away from the display's core: `BMDS_PROGRAM_DISPLAY`) and `source_dir` (the tarball's top directory, if it isn't
  `<name>-<version>`). A package of data kept in this repository has
  `source_tree` (its directory, from the repository's root) instead of
  `source` and `sha256`, may have `version_file` (a file there holding the
  version) instead of `version`, and `sdk = false` if it needs no compiler.
- **A meta package** (a set, as `bmds-standard`): `package.toml` alone, with `meta = true`,
  `version`, `description`, `title`, `category`, `license` and `depends` (the set). Nothing is
  built; it is packaged from an empty tree, and installing it installs its set, each member
  automatic (`pkg autoremove` takes them when nothing asked for needs them).
- **`build.sh`:** bash, run with `-e` in an empty build directory. It builds
  and installs into `$DESTDIR` (`bin/` for programs, links as real symbolic
  links). It gets `SRC` (the unpacked, patched source), `BUILD` (its working
  directory), `DESTDIR`, `PKGDIR` (this directory), `CC` (the SDK's `bmds-cc`,
  static musl), `CXX` (`bmds-c++`: C++ without exceptions, RTTI or libstdc++), `AR`, `STRIP`, `OBJCOPY`, `HOSTCC`, `BMDS_MAKE` (use it for the main
  build: it paces parallel jobs; it isn't called `MAKE`, which would replace
  make's own in recursive builds), `JOBS`, `BMDS_STACK`, `BMDS_DISPLAY`, `VERSION`, `ROOT`
  (the repository; for its `tools/`). Without the SDK there is no `CC`, `CXX`, `AR`,
  `STRIP` or `OBJCOPY`.
- **`patches/*.patch`** (optional): applied in name order with `patch -p1`.
- **Icons** (the `icons` and `icons-glyphs` packages): the desktop's themes,
  `share/icons/flat-color/` (the default) and `share/icons/glyphs/`, from the PNGs rendered in
  this repository (`thirdparty/flat-color-icons/`, `thirdparty/glyphs/`, `tools/render-icons`;
  Git LFS: run `git lfs pull` before building), installed by `tools/install-icon-theme` with
  their aliases. A package's own icons go in a theme's size directories under `share/icons/`.
  A directory of many files installs slowly: FAT searches a directory's names for each new
  file (Glyphs' 1,720 took most of 7.5 minutes).
- **Launcher entries** (optional): Desktop Entry files installed in
  `$DESTDIR/share/applications/` show in the panel's launcher (`devplan/gui.md` G10, G11;
  BusyBox's `vi.desktop`).
- **Application folders** (`devplan/apps.md`): an application with its own data, rather than a
  Unix tool, installs as one folder, `$DESTDIR/apps/NAME.app/`, holding its program, an
  `app.desktop` (its `Exec` names the program in the folder) and its data; it then shows in the
  launcher and in `/apps` (a command on the `PATH`) by itself (`sdk-examples`' `surfdemo.app`).
  Its program can carry its own icons, shown for the folder, in the launcher and on its windows:
  `sdk/tools/bmds-icon-notes` makes them from 16 and 32 pixel PNGs (`blocks`, `doom`, `luckyduck`).
- **Benchmarks** (optional, `devplan/bench.md` §4f): a descriptor, `$DESTDIR/share/bench/NAME.bench`,
  declares the package's benchmarks for the `bench` command (`benchprog`, `netlat`, `doom`, `coremark`); the
  program prints `@bench` lines (`sdk/include/bmds/benchlib.h`).
- Anything else the build reads (a configuration file, say).

`bmds-dist` strips the programs in `bin/` and in application folders (as the
loader needs: relocations kept), copies the license files, records the package's files in
`etc/dist/<name>.files`, and writes `etc/dist/links.sh` with every link (the
board makes them: emulated links need the FAT System attribute, which a PC
doesn't set).

Programs see POSIX through musl, with BMDS's differences listed in
`sdk/README.md` ("The C library"): programs start others with `vfork` and
`exec` or `posix_spawn` (no `fork`); `/bin/sh` is the BMDS shell (`system`,
`popen`, `#!/bin/sh` scripts).
