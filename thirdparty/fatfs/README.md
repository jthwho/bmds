# FatFs (vendored)

ChaN's FatFs, <https://elm-chan.org/fsw/ff/>, used by `components/bmds_fs` (see
`devplan/storage.md`). License: `LICENSE.txt` (1-clause BSD style).

- **Version:** R0.16 (`ff16.zip`, sha256
  `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e`), with ChaN's
  published patches applied in order: `patches/ff16p1.diff` (minimum volume size) and
  `patches/ff16p2.diff` (CVE-2026-6682, -6683, -6687: broken FAT size, exFAT cluster
  count and exFAT label length fields on a crafted card).
- **Changes:** line endings converted from CRLF to LF before patching, and four BMDS patches, in
  this order:
  - `patches/bmds-attr.diff`: `f_open` keeps the file's attribute in `FIL.obj.attr` (FatFs left
    it unset for files), so the VFS sees a System file, which may be an emulated symbolic link,
    without a second lookup (`devplan/storage.md` §4).
  - `patches/bmds-ascii-upper.diff`: `ff_wtoupper` answers ASCII without walking its table.
    Long names are compared a character at a time, both sides folded, on every entry a lookup
    passes; the table maps nothing below U+0080 but `a` to `z` (`devplan/storage.md` §3).
  - `patches/bmds-exfat-find.diff`: `dir_find` on exFAT reads each entry block's name hash in the
    window (its stream extension entry, the block's second) and passes over a block that doesn't
    match without loading it and checking its sum. FatFs loaded and checked every block before the
    one sought. A block not plainly whole in one sector is loaded as before (§3).
  - `patches/bmds-exfat-broken.diff`: a broken entry block (a write cut off between its sectors)
    is passed over by listings and lookups, as Windows does, instead of ending the directory with
    `FR_INT_ERR`. The lookup's skip of a block stops at an entry that isn't one of its secondaries:
    trusting the count went past a block made after a broken one (§3, found by the cut test).
- **Not vendored:** `ffconf.h`, `ffsystem.c` and `diskio.c`, which BMDS supplies
  (`components/bmds_fs/src/fatfs/ffconf.h`, `components/bmds_fs/src/fatfsport.cpp`).

To update: unpack the new release, convert line endings, apply the patches ChaN lists for
it and `patches/bmds-*.diff`, replace `ff.c`, `ff.h`, `diskio.h`, `ffunicode.c` and the text
files here, and compare the new release's `ffconf.h` with ours for added options.
