# Preso: Presentations Written in Markdown

Planned 2026-10-30 with the user (the user: a presentation app in the spirit of Marp, decks written
in Markdown with images, a look set apart from the content, the retro aesthetic kept; built as an
SDK program on the card; the talk about BMDS given on BMDS).

The program is `preso` (the user's name, 2026-10-30).

## 1. Goal

- **A deck is a Markdown file** with its images beside it. Anyone who knows Marp writes one at
  once, and a deck can be previewed in Marp on a PC.
- **The look lives in a theme**, apart from the content: colours, fonts, spacing, headings,
  bullets, code, backgrounds, effects. Retro themes are built in; a deck can bring its own.
- **Presenting is comfortable:** a keyboard, a USB clicker (a keyboard) or a gamepad moves through
  the deck; the speaker's notes show on a laptop's terminal over SSH.
- **Live demos from the deck:** a slide can start a program, and the deck comes back when it ends.
  The talk steps from its slides into the real system and back in one take.
- **An SDK program on the card** (`programs/preso`, `dist/preso`), not part of the OS. What it
  needs that would serve other programs too goes in the kernel or the SDK, as ordinary
  additions (§7).

Not goals: editing decks on the board beyond what the text editor already does; Marp's HTML and
CSS (themes are our own format, §4); PDF or PowerPoint output.

## 2. Building blocks

- **`libbmdsgame`** (`games.md`): the surface (a window or full screen, Alt+Enter), the frame loop
  that only draws on change, the canvas with blending and scaling, images (PNG, JPEG, BMP), fonts
  (`bmds_game_font()` over the font library), the 2D-DMA for big copies, sound.
- **Controls** (`controls.md`): the deck declares actions; keyboard and pad bindings come from
  `controls.conf` and the user's changes.
- **md4c** (MIT, plain C, CommonMark with tables and strikethrough, a callback parser that keeps
  no tree): vendored in `thirdparty/md4c`. A subset parser of our own would grow into it anyway.
- **Fonts on the board:** IBM Plex Sans (regular, bold, italic), Mono (regular, bold), Serif
  (regular), and unscii. A bitmap font has no bold: it is drawn twice, a pixel apart.

## 3. The deck format

Marp's, where Marp has the thing; our extensions are directives Marp ignores.

```markdown
---
theme: vga
paginate: true
footer: BMDS · 2026
transition: wipe
---

<!-- _class: title -->
# BMDS
## Bare Metal Do Stuff

---

# What's in it

* a DOS-like shell        <!-- `*` lists appear one item at a time (Marp's fragments) -->
* windows and a desktop
* programs from the card

![bg right:40%](images/desktop.png)

<!-- Speaker notes: any comment that isn't a directive. -->

---

<!-- run: luckyduck -->
# Lucky Duck
```

- **Slides** are separated by `---` lines. The front matter (the first `---` block) sets
  directives for the whole deck; `<!-- name: value -->` sets one from that slide on, and
  `<!-- _name: value -->` for that slide only (Marp's local directives).
- **Directives:** `theme`, `class`, `paginate`, `header`, `footer`, `backgroundColor`,
  `backgroundImage`, `color`, `transition` (Marp has these); ours: `run` (§6), `sound` (a sound
  played as the slide appears), `align` (vertical: top, middle).
- **Images** take Marp's keywords in their alt text: `bg` (a background), `bg left` and
  `bg right` with a share (`right:40%`) split the slide, several `bg` images tile, `contain`,
  `cover` and a percentage scale, `w:` and `h:` size an inline image. A missing image draws a
  marked box and `preso check` reports it.
- **Markdown:** headings 1-3, paragraphs with bold, italic, code and strikethrough, links (shown,
  not followed), lists with nesting, numbered lists, block quotes, code blocks (highlighted
  later), tables, horizontal rules, inline images. HTML other than comments is shown as text.
- **Fitting:** Marp's `<!-- fit -->` on a heading scales it to the width. Overflow is not
  shrunk silently: it is cut at the slide's edge and `preso check` reports it.

## 4. Themes

A theme is an INI file, as our other configuration (`share/preso/themes/NAME.theme` on the card,
or a path beside the deck). Keys are inherited: a section names only what differs.

```ini
[Theme]
Name=VGA
Size=1280x720          ; the slide's size, scaled to the screen
Palette=ega            ; optional: images and colours mapped to it, dithered
Effects=scanlines

[Slide]
Background=#0000aa
Color=#ffffff
Font=unscii 32
Margin=64 80
Pattern=               ; a tiled PNG, as the desktop's

[H1]
Font=unscii 64
Color=#ffff55
Frame=double           ; a box-drawing frame around the heading

[Bullet]
Glyphs=■ ▪ ·

[Code]
Font=IBM Plex Mono 24
Background=#000000
Color=#aaaaaa

[Class title]          ; <!-- class: title --> slides
Align=middle
H1.Font=unscii 96
```

**Built-in themes** (each one drawn in our own style, nothing copied):
- `vga`: blue and white text mode, unscii, box-drawing frames, the EGA palette;
- `amber` and `green`: phosphor on black, a soft glow, scanlines;
- `classic`: the toolkit's grey 3D look, a title bar on every slide, IBM Plex;
- `bbs`: ANSI art colours, block-shade dividers;
- `paper`: plain, light and readable, for when retro isn't wanted.

**Effects** are drawn after the slide: `scanlines` (darkened alternate rows), `glow` (a blurred
copy of the text added under it), `dither` (images mapped to `Palette` by ordered dithering,
which gives the period look and costs nothing per frame once done).

## 5. Presenting

- **Rendering:** each slide is laid out and drawn into a buffer of its own in PSRAM (1280×720
  RGB565: 1.8 MB), the current one and the next ahead of time, so a step is a copy. A transition
  blends the two buffers.
- **Transitions:** `cut`, `wipe` (a direction), `dissolve` (an ordered-dither mask growing),
  `pixelate`, `type` (the text appears as if typed, with an optional teletype sound).
- **Actions** (`controls.conf`; the user changes them in Controls):

  | Action | Keys | Pad |
  |---|---|---|
  | Next (or next item) | Right, Down, Space, Enter, PageDown | A, right |
  | Previous | Left, Up, Backspace, PageUp | B, left |
  | First, last | Home, End | — |
  | Go to | a number, then Enter | — |
  | Black screen, white screen | B or `.`, W | Select |
  | Overview (a grid of every slide) | Tab | Start |
  | Quit | Escape | — |

  USB clickers send PageUp, PageDown and B, so they work as keyboards do.
- **Notes on a laptop:** the deck writes each slide's notes, number and the next slide's title to
  its standard output. Started over SSH (`ssh root@bmds0 preso talk.md`), the notes land in the
  laptop's terminal while the slides show on the HDMI screen, and keys typed there move the deck
  too. No second display needed, and no new IPC.
- **A timer** in the notes: time since the first slide, and per slide.

## 6. Live demos

`<!-- run: COMMAND -->` on a slide: stepping onto it starts `COMMAND` (vfork and exec through the
shell, so arguments and `terminal -e ...` work), and the deck gets out of the way. When the
program ends, the deck comes back on the slide after. The slide's own content shows while the
program starts, so a title card can introduce the demo.

What the deck needs to get out of the way: today a full-screen surface covers everything. Either
the deck leaves full screen and minimizes its window, or a general call hides a surface and shows
it again (§7). To decide in step 7.

## 7. Additions outside the program

Kept small, and only what other programs would use:
- **Hiding a surface** for a while, if minimizing isn't enough (§6).
- **Bold and italic for the bitmap font** in the font library, if drawing twice looks wrong.
- Anything md4c needs from libc that musl's static build lacks (none expected).

## 8. Also from the same code

- **`preso check DECK`:** parses and lays out every slide without a screen; reports missing
  images, overflowing text, unknown directives and themes. Runs on the host as well (a test).
- **`preso export DECK DIR`:** each slide to a PNG, for the README, a web page or a PDF made
  elsewhere.
- **A Markdown viewer later:** the same layout in a scrolling window opens `.md` files from the
  file browser (`mimeapps.list`), our own docs included. Not in this plan's steps.

## 9. Resource budget

- **Program size:** md4c about 60 KB of code, the deck's own code about as much again; loaded from
  the card, nothing in the firmware.
- **PSRAM:** three slide buffers (current, next, the transition's) 5.5 MB; images decoded once at
  their drawn size; the deck's text a few KB.
- **CPU:** nothing between steps (the frame loop draws only on change). A slide drawn in tens of
  milliseconds ahead of time; transitions at the display's 60 Hz.

## 10. Open questions

- **Code highlighting:** reuse the editor's syntax tables (`textsyntax`), which live in the OS's
  core, not the SDK; or a small scanner of the deck's own. Later.
- **Typing transitions with sound:** fun, possibly too much for a talk. Off by default.

## 11. Steps

1. [ ] md4c vendored; the deck parser (front matter, slides, directives, image keywords, notes)
   into a slide model; host tests.
2. [ ] Layout and drawing with one theme (`paper`): headings, paragraphs, inline styles, lists,
   code, quotes, tables; the frame loop and the step actions; `preso check`.
3. [ ] Images: backgrounds, splits, sizes; the dithering to a palette.
4. [ ] The theme format in full and the retro themes; effects.
5. [ ] Transitions, `*` fragments, the overview, black and white screens, go to.
6. [ ] Notes and keys over the terminal, the timer; pad bindings checked with the user's pad.
7. [ ] Live demos, with whatever surface support they need (§6, §7).
8. [ ] `preso export`; `dist/preso` with an example deck; the BMDS talk itself.
