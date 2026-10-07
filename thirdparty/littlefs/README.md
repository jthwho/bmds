# littlefs (vendored)

The littlefs project's power-loss-resilient filesystem for flash,
<https://github.com/littlefs-project/littlefs>, used by `components/bmds_fs` for `/boot`
(`devplan/storage.md` §13c, §13d). License: `LICENSE.md` (BSD-3-Clause).

- **Version:** v2.11.3 (commit `6cb4e86540eca0d9ba62500a298385c9d863c8be`), on-disk
  format 2.1. sha256:
  - `lfs.c` `a36d6a095785ddea9571d541d68d3e4ef01d5b255a99d17d3f07fb6ea60ea132`
  - `lfs.h` `b1befd7288d08815accc8f9af744c55686c0b9e3ac0061c32ceee38a1b3eb96d`
  - `lfs_util.c` `f2fbde533670560434bd9f5a547174cc7c5a4670a02c47b4bd85180dced8b2ec`
  - `lfs_util.h` `548d46aa524dc7449e16739286c1a422a52f9de727ff0be0c2ffc5593f5ca981`
- **Changes:** none.
- **Built with:** `LFS_NO_MALLOC` (every buffer is supplied) and `LFS_NO_DEBUG`,
  `LFS_NO_WARN` and `LFS_NO_ERROR` (no messages: errors are returned). The code shadows
  variables, so `-Wshadow` is off for these two files only.
- **Not vendored:** the tests, scripts, benchmarks and the block-device examples (`bd/`).

To update: copy `lfs.c`, `lfs.h`, `lfs_util.c`, `lfs_util.h` and `LICENSE.md` from the new
release's tag, then run the host tests (`tests/host/littlefs.cpp` cuts the flash at every
operation, `tests/host/lfsvfs.cpp` runs it in the VFS).
