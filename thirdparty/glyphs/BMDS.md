# Glyphs, as BMDS uses them

- **Upstream:** https://github.com/gorango/glyphs (Goran Spasojevic), `@glyphs/core` 0.8.12,
  commit `771993addf00d5a7080fadb763f0da72ddc0f716` (2023-02-21). License: MIT (`LICENSE`,
  upstream's).
- **What is here:** upstream keeps its icons as JSON exported from Figma
  (`packages/svg/core/map.json`), not as files. `tools/import-glyphs` writes the core set's
  coloured variant (`poly`; the other four variants are one light grey meant to be recoloured)
  as `svg/NAME.svg`, 860 icons, the names kebab-case as upstream's own exporter makes them
  (`folderOpen` -> `folder-open`), and `categories` (each icon's upstream categories).
- **`png/16x16/`, `png/32x32/`:** rendered by `tools/render-icons thirdparty/glyphs`, kept through
  Git LFS.
- **On the board:** the `icons-glyphs` distribution package (`dist/icons-glyphs/`) installs the
  `glyphs` theme in `/sd0/p0/share/icons/`, with the freedesktop.org names as links
  (`dist/icons-glyphs/aliases`) and `Inherits=flat-color` for names it lacks. Chosen with
  `IconTheme=glyphs` in `[Panel]` of `/boot/config/panel.conf`. See `devplan/gui.md` §4h.
- **Updating:** change the commit in `tools/import-glyphs`, run it, then
  `tools/render-icons thirdparty/glyphs`.
- Upstream's brands (162 logos: trademarks) and flags (193) are left out.
