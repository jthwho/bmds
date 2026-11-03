# md4c (vendored)

Martin Mitáš's Markdown parser, <https://github.com/mity/md4c>, used by Preso
(`programs/preso`, `devplan/preso.md`): CommonMark with tables and strikethrough, a callback
parser that keeps no tree. License: MIT (`LICENSE.md`).

- **Version:** 0.5.2, the tag `release-0.5.2` (`md4c-release-0.5.2.tar.gz` from GitHub's
  archive, sha256 `55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21`): its
  `src/md4c.c`, `src/md4c.h` and `LICENSE.md`, unchanged. `md4c-html` and `entity.c` (HTML
  output and its entity table) aren't taken: Preso translates the few entities a slide needs
  itself.
- **Changes:** none.
- **Built with** Preso's own flags; md4c's warnings are left to it (`-w`), as AngelScript's.
