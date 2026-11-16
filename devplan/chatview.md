# ChatView: Chat and Log Text for Programs

Planned 2026-11-16 for the IRC client (bmds-apps' `devplan/irc.md`: the client lives in the
bmds-apps repository and is released after the next BMDS release, which brings TLS for programs,
network.md §35, and this class). Approved as proposed by the user (2026-11-16), the name too.

Status: **step 1 built** (§7).

## 1. Why

The GUI service has no class for text that arrives a line at a time with styles and links.
`Terminal` takes ANSI colours (monospace, no links, a wrap that neither indents nor reflows when
the window narrows); `TextEdit` is one style; a `Canvas` can draw anything, but then every
program writes its own layout, selection, scrolling and links. A chat client needs styled lines,
wrapped with a hanging indent, links to click, a marker for "new since you looked", selection to
copy, and a view that follows the end unless scrolled back. So do a log viewer, sysmon's log, a
build's output.

## 2. The class

`ChatView`, a widget the GUI service draws (`bmds_ui`), appended to as a program gets lines:

| | |
|---|---|
| **Methods** | `append(String line)`; `appendMany(List)`; `clear()`; `setMarker()` (a line under the last one read); `scrollToEnd()`; `copy()` |
| **Properties** | `maxLines` (the oldest dropped past it; default 2000), `followEnd` (read only: at the end), `wrap` (Bool, default on), `palette` (List: the 99 colours, overridable), `selectedText` (read only); its font is every widget's `font` (`ui` by default, `mono`...) |
| **Events** | `linkActivated(String url)`, `scrolledBack(Bool)` (left the end, or reached it again) |

- **A line's styling is IRC's own codes** (mIRC's, what a client parses anyway and a log can keep):
  `\x02` bold, `\x1d` italic, `\x1f` underline, `\x1e` strike, `\x11` monospace, `\x16` reverse,
  `\x03FG[,BG]` a colour of 99, `\x04RRGGBB[,RRGGBB]`, `\x0f` all off. Anything else below 0x20
  but a tab is shown as nothing. Text is UTF-8.
- **A tab splits the gutter from the text** (a time and a nick, say): continuation lines indent
  past the gutter, and the gutters line up at the widest seen so far.
- **Links:** `http://`, `https://`, `irc://`, `ircs://` and `www.` found by the widget, underlined;
  a click sends `linkActivated` (the program opens it: the desktop's `open` service, or a buffer of
  its own for `irc://`).
- **Scrolling:** at the end, new lines scroll it; scrolled back, it stays put (`scrolledBack`), and
  `scrollToEnd()` or End returns. The marker is drawn as a rule with the words the program gives
  in `setMarker` (none: a plain rule).
- **Selection** by the mouse across lines; `copy()` and Ctrl+C put the plain text (codes removed,
  gutters and text tab-separated) on the clipboard.
- **Memory:** each line kept as its bytes (the codes a byte or a few), in PSRAM, laid out only when
  it is shown and the layout cached for what is on the screen; `maxLines` bounds it.

## 3. ABI

A class appended to `sdk/gui/classes.def` (its C wrappers generated into `bmds/guiclasses.h`, its
reference into `docs/gui-classes.md`) and to the GUI protocol's lock: append only, as every class.
A program that needs it on an older firmware finds the class missing (`Describe`) and says so.

## 4. Steps

1. **The model and layout, portable** (`bmds_core` or the GUI's portable part): the codes parsed
   into runs, links found, wrapping with the gutter, selection's text; host-tested, fuzzed.
2. **The widget** in the GUI service: drawing, scrolling, the marker, selection, the events,
   `classes.def`, the lock.
3. **`guigallery`** gains a tab (lines arriving on a timer, styles, links, a marker); `guitest`
   covers its properties and events.
4. Then the IRC client (bmds-apps) uses it.

## 5. Budget (estimates)

About 20 KB of flash in the GUI service; per view, its lines (2000 at ~100 bytes: ~200 KB at the
most, in PSRAM) and the shown lines' layout.

## 6. Decided (the user's, 2026-11-16)

- The class as above, not the client on `Terminal` or a `Canvas` (bmds-apps' `devplan/irc.md` I1).
- `ChatView` as its name.

## 7. As built: step 1, the model (2026-11-16)

- **`bmds/chatmodel.h`, `chatmodel.cpp`** in `bmds_gui`'s pure sources (built for the host too):
  - `ChatText::parse()`: the codes into at most 64 runs (past them the style stays: no text lost),
    the first tab the gutter (later ones spaces), other control characters dropped, UTF-8 as it
    came; links at a word's start (after a space or `(<["'`), ending at a space, `<>"` or a
    control, trailing `.,;:!?'` and a bracket the link didn't open left out, at most 8; the runs
    cut at the links, each told its link. `plain()` (gutter, tab, text) for copying, `linkAt()`,
    `paletteColour()` (mIRC's 16 and the 83 extended).
  - `ChatText::wrap()`: through a `Measure` of the caller's (width and fitting, a run's style
    flags given: bold or monospace may be another font), the gutter at 0 and the text and every
    continuation row from the view's column (from 0 when the column is past two thirds of the
    width); breaks at spaces, a break's spaces dropped; a word wider than a row broken at the
    edge, from the gutter's row rather than leaving it bare; a row never empty but an empty
    line's. Writes rows and pieces (a run's part, at its x) when given room, counts rows always.
  - `ChatText::hit()`: the byte nearest a point on a row (a tie: the left).
  - `ChatLines`: a ring of lines in PSRAM (`maxLines`, the oldest dropped), numbered from the
    first ever, each with its rows and its first row, so the view finds the line at a row by a
    binary search; `setRows()` and `restack()` after a rewrap; `setMaxLines()` keeps the newest.
- **Tests:** 6 host cases (styles, colours, control characters, the gutter, plain text, too many
  runs; links and their runs; wrapping with a gutter, hanging rows, long words, the narrow case,
  bold measuring wider, UTF-8; hits; `ChatLines`); `chatmodel_fuzz` (runs covering the text,
  links inside it, every byte but a break's spaces placed once in order at four widths, hits in
  range): 495,202 runs in 3 minutes, nothing found.

