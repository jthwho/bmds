# Flat Color Icons, as BMDS uses them

- **Upstream:** https://github.com/icons8/flat-color-icons, version 1.0.2, commit
  `1bf90d5ff118bc6690120ff9fdfe234565b7e414` (2026-07-01). `svg/`, `LICENSE.md` and `README.md`
  are upstream's, unchanged. License: MIT, or icons8's "Good Boy License" (`LICENSE.md`).
- **`png/16x16/`, `png/32x32/`:** rendered from `svg/` by `tools/render-icons` (rsvg-convert),
  committed through Git LFS. Run it again after changing `svg/`.
- **On the board:** the `icons` distribution package (`dist/icons/`) installs them as the
  `flat-color` theme in `/sd0/p0/share/icons/`, with the freedesktop.org names the launcher's
  entries use as links (`dist/icons/aliases`). See `devplan/gui.md` §4h.
- **Updating:** replace `svg/` (and the two upstream files) with a newer release's, update the
  commit above, and run `tools/render-icons`.
