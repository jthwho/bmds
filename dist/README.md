# The BMDS distribution

Third-party programs BMDS ships on the card, built from pinned upstream
releases with the SDK (design: `devplan/distribution.md`).

```bash
. ~/esp/esp-idf/export.sh           # the compiler (and pyserial, for install)
tools/bmds-dist list
tools/bmds-dist build [package...]  # into build-dist/rootfs
tools/bmds-dist install             # over the serial cable, into /sd0/p0
```

`--out DIR` builds into DIR instead of `build-dist`; `--nested` is for running inside another
build (the firmware's, which puts `dropbear` and `sftp-server` in `/rom`): no `build` wrapper,
whose lock the outer build holds, and one job at a time.

## Adding a package

Make `dist/<name>/` with:

- **`package.toml`:** `name` (the directory's), `version`, `description`,
  `license` (SPDX), `source` (the release tarball's URL), `sha256`,
  `license_files` (paths in the source to install under `share/licenses/<name>`),
  optionally `stack` (bytes the program's stack must have; default the
  kernel's) and `source_dir` (the tarball's top directory, if it isn't
  `<name>-<version>`). A package of data kept in this repository has
  `source_tree` (its directory, from the repository's root) instead of
  `source` and `sha256`, may have `version_file` (a file there holding the
  version) instead of `version`, and `sdk = false` if it needs no compiler.
- **`build.sh`:** bash, run with `-e` in an empty build directory. It builds
  and installs into `$DESTDIR` (`bin/` for programs, links as real symbolic
  links). It gets `SRC` (the unpacked, patched source), `BUILD` (its working
  directory), `DESTDIR`, `PKGDIR` (this directory), `CC` (the SDK's `bmds-cc`,
  static musl), `AR`, `STRIP`, `OBJCOPY`, `HOSTCC`, `BMDS_MAKE` (use it for the main
  build: it paces parallel jobs; it isn't called `MAKE`, which would replace
  make's own in recursive builds), `JOBS`, `BMDS_STACK`, `VERSION`, `ROOT`
  (the repository; for its `tools/`). Without the SDK there is no `CC`, `AR`,
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
- Anything else the build reads (a configuration file, say).

`bmds-dist` strips the programs in `bin/` (as the loader needs: relocations
kept), copies the license files, records the package's files in
`etc/dist/<name>.files`, and writes `etc/dist/links.sh` with every link (the
board makes them: emulated links need the FAT System attribute, which a PC
doesn't set).

Programs see POSIX through musl, with BMDS's differences listed in
`sdk/README.md` ("The C library"): programs start others with `vfork` and
`exec` or `posix_spawn` (no `fork`); `/bin/sh` is the BMDS shell (`system`,
`popen`, `#!/bin/sh` scripts).
