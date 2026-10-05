# The BMDS distribution

Third-party programs BMDS ships on the card, built from pinned upstream
releases with the SDK (design: `devplan/distribution.md`).

```bash
. ~/esp/esp-idf/export.sh           # the compiler (and pyserial, for install)
tools/bmds-dist list
tools/bmds-dist build [package...]  # into build-dist/rootfs
tools/bmds-dist install             # over the serial cable, into /sd0/p0
```

## Adding a package

Make `dist/<name>/` with:

- **`package.toml`:** `name` (the directory's), `version`, `description`,
  `license` (SPDX), `source` (the release tarball's URL), `sha256`,
  `license_files` (paths in the source to install under `share/licenses/<name>`),
  optionally `stack` (bytes the program's stack must have; default the
  kernel's) and `source_dir` (the tarball's top directory, if it isn't
  `<name>-<version>`).
- **`build.sh`:** bash, run with `-e` in an empty build directory. It builds
  and installs into `$DESTDIR` (`bin/` for programs, links as real symbolic
  links). It gets `SRC` (the unpacked, patched source), `BUILD` (its working
  directory), `DESTDIR`, `PKGDIR` (this directory), `CC` (the SDK's `bmds-cc`,
  static musl), `AR`, `STRIP`, `OBJCOPY`, `HOSTCC`, `BMDS_MAKE` (use it for the main
  build: it paces parallel jobs; it isn't called `MAKE`, which would replace
  make's own in recursive builds), `JOBS`, `BMDS_STACK`, `VERSION`.
- **`patches/*.patch`** (optional): applied in name order with `patch -p1`.
- Anything else the build reads (a configuration file, say).

`bmds-dist` strips the programs in `bin/` (as the loader needs: relocations
kept), copies the license files, records the package's files in
`etc/dist/<name>.files`, and writes `etc/dist/links.sh` with every link (the
board makes them: emulated links need the FAT System attribute, which a PC
doesn't set).

Programs see POSIX through musl, with BMDS's differences listed in
`sdk/README.md` ("The C library"): programs start others with `vfork` and
`exec` or `posix_spawn` (no `fork`); `system`, `popen` and `/bin/sh`
scripts wait for step 8b.
