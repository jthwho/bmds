# FatFs (vendored)

ChaN's FatFs, <https://elm-chan.org/fsw/ff/>, used by `components/bmds_fs` (see
`devplan/storage.md`). License: `LICENSE.txt` (1-clause BSD style).

- **Version:** R0.16 (`ff16.zip`, sha256
  `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`), with ChaN's
  published patches applied in order: `patches/ff16p1.diff` (minimum volume size) and
  `patches/ff16p2.diff` (CVE-2026-6682, -6683, -6687: broken FAT size, exFAT cluster
  count and exFAT label length fields on a crafted card).
- **Changes:** line endings converted from CRLF to LF before patching. Nothing else.
- **Not vendored:** `ffconf.h`, `ffsystem.c` and `diskio.c`, which BMDS supplies
  (`components/bmds_fs/src/fatfs/ffconf.h`, `components/bmds_fs/src/fatfsport.cpp`).

To update: unpack the new release, convert line endings, apply the patches ChaN lists for
it, replace `ff.c`, `ff.h`, `diskio.h`, `ffunicode.c` and the text files here, and compare the new
release's `ffconf.h` with ours for added options.
