# unscii-16

`unscii-16.hex` is the 8×16 variant of Unscii 2.0 by Viznut, from <http://viznut.fi/unscii/>
(downloaded 2026-10-05, SHA-256 `2642c8b748fa81f24d76772d70c55faa720d98fadfec8133daf89136c5c8bfb1`).

Licence, from that page: "unscii-16-full" falls under GPL because of how Unifont is licensed; the
other variants are in the Public Domain. This is `unscii-16`, not `unscii-16-full`: Public Domain.

Format: one glyph a line, `CODEPOINT:BITS` in hex (GNU Unifont's `.hex`); 32 hex digits are an
8×16 glyph (a byte a row), 64 a 16×16 one (two bytes a row); the top row first, the most significant
bit leftmost. `tools/fontconv.py` turns it into the `bmds_ui` glyph table at build time.
