# ChatView: Chat and Log Text for Programs

Planned 2026-11-16 for the IRC client (bmds-apps' `devplan/irc.md`: the client lives in the
bmds-apps repository and is released after the next BMDS release, which brings TLS for programs,
network.md §35, and this class). Approved as proposed by the user (2026-11-16), the name too.

Status: **steps 1 to 3 built** (§7, §8, §9), and the input line's additions (§10); step 4 is
the IRC client's (bmds-apps). Finding text (`ChatView.find`) and a line's pasted lines
(`LineEdit.pasteLines`) came with the client's step 5: `gui.md` §4v.

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
| **Methods** | `append(String line)`; `appendMany(List)`; `clear()`; `setMarker(String words)` (a rule under the last line read); `clearMarker()`; `scrollToEnd()`; `copy()` |
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

## 5. Budget

Estimated: about 20 KB of flash in the GUI service; per view, its lines (2000 at ~100 bytes:
~200 KB at the most, in PSRAM) and the shown lines' layout. Measured (§8): 19 KB of flash; per
view 11 KB of PSRAM for laying out a line, and for 2000 lines 72 KB of slots and counts besides
their bytes.

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

## 8. As built: step 2, the widget and the class (2026-11-16)

- **`bmds/chatview.h`, `chatview.cpp`** (`bmds_gui`): `ChatView`, a `ScrollArea`.
  - **Lines:** `ChatLines` holds each line's bytes. One line at a time is laid out (parsed and
    wrapped) into an 11 KB scratch in PSRAM, kept until another line needs it. Painting lays out
    each line it shows. Every line is wrapped again when the width, the font or the gutter's
    column changes (the gutters are measured again only for a font). The vertical bar's room is
    always kept, so the bar coming and going doesn't wrap everything.
  - **Fonts:** the widget's font, its bold and italic, and the `mono` role at its size, as the
    library opens them (one it can't is the nearest it has). Baselines line up; the row is as
    tall as the tallest.
  - **Measured in 1/64 pixels** (`Font::layout()`'s pen), not `textWidth()`'s pixels, which are
    rounded up piece by piece: the sums drifted right of where the painter, drawing a font's
    pieces in one call, put the glyphs, about 8 px across a row, and underlines and hits fell a
    character off (seen on the board). The model doesn't care what its units are
    (`Measure`'s doc says so).
  - **Drawing:** a row a font at a time: the pieces side by side in one font go in one
    `Painter::text()` call, with a run of colours each, cut at the selection's ends.
    `Painter::text()` starts a font's text on a 4-pixel column and fills its box, so the model
    gained `Measure::align` (and `FontFlags`): a run whose font differs from the one before it
    starts on a column. That leaves a gap of up to 3 px after a change of font (`italic ,`).
    Backgrounds of their own are filled the row's height. Underlines (links too) and strikes are
    1 px fills. Links are in `Highlight`, the selection in `Highlight`/`HighlightText`, the field
    in `FaceHighlight`.
  - **Scrolling:** at the end, it follows. A scroll of the user's (the bar, the wheel, the keys)
    decides `followEnd` and tells `scrolledBack`. Scrolled back, the line at the top, its row
    and the pixels into it are kept, and put back at the top after any change (lines appended
    or dropped, a rewrap). Its own scrolls are marked, so they don't count as the user's.
  - **The marker:** a row of its own before the line after it, a rule in `Highlight` with its
    words at the text's column. Its line dropped takes it.
  - **Selection:** a drag (scrolling past the top or bottom edge), a double click a word (the
    gutter's end ends one too), a triple a line, Shift+click. `selectedText` is the gutter, a
    tab, the text; lines are joined by newlines. It becomes the primary selection.
    Ctrl+C/Ctrl+Insert copy, Ctrl+A selects all. A click without a drag (3 px) on a link,
    released on it, tells `linkActivated` with the link's text. The pointer is the arrow over a
    link and the I-beam elsewhere.
  - Keys: Up, Down, Page Up/Down, Home, End.
- **Model changes:** `Measure::align`, `FontFlags`, `gutterWidth()`. Wrapping measures each word
  and each run of spaces once, run by run, and places them from those widths. It used to measure
  each word four times (a row's fit, this place's fit, the break loop, placing it); a word broken
  across rows is still measured where it goes. On the board that halved the cost.
- **`GuiClipboard::forget()`** withdraws `copy()`'s providers too, not only `offer()`'s.
  `TextEdit` (and `ChatView`) call it when destroyed. Before, a `TextEdit` destroyed while its
  selection was the primary one left the clipboard a pointer to it.
- **The class:** `ChatView` appended to `classes.def` as §2 has it, plus `clearMarker()`.
  `setMarker` takes its words. `palette` is 99 `#rrggbb` strings (fewer set the first ones and
  the rest go back to IRC's; an empty list sets all of IRC's). `appendMany` lays out 32 lines at
  a time. `maxLines` is 1 to 100,000. `gui.lock` and angel's `api.lock` (angel binds every class)
  have the new lines appended; `guiclasses.h` and `docs/gui-classes.md` are regenerated.
- **Tests:**
  - Host: a case for the column at a font change (and `gutterWidth()`). 7 cases, 273
    assertions; all 13 suites pass.
  - `chatmodel_fuzz` also checks, with `align` 4, that pieces don't overlap and that a font
    change starts on the column: 342,156 runs in 3 minutes, and 316,800 more after the
    measure-once change, nothing found.
  - The board (`/sd0/p0/claude/chattest.as`, angel): styles, colours, reverse, mono, links, the
    gutter's column widening for a longer nick, hanging rows, a long word broken, the marker,
    the wheel and Home/End with `scrolledBack` both ways, link clicks, a selection across a
    gutter read back as `12:03 carol<tab>A long...`, a narrower window rewrapped with the top
    line kept, `font = "mono"`, `wrap = false` with the horizontal bar.
  - `chatperf.as` (2000 lines): `appendMany` 405 ms, a full rewrap 326 ms (wrap off: 474 ms),
    an append while following ~1.3 ms past the round trip. Before the measure-once change these
    were 721, 651 and ~5.6 ms.
- **Open:** a rewrap of 2000 lines takes about a third of a second, which is fine for the
  window manager's one resize at the end of a drag; a live resize would want only the shown
  lines rewrapped. Nothing caches the shown lines' layouts between paints yet.

## 9. As built: step 3, the gallery and guitest (2026-11-16)

- **`guigallery`'s Chat tab:** a `ChatView` (500 lines kept) fed a styled line every 1.5 s by a
  timer: bold, italic, underline, strike, mono, the colours, an RGB colour, reverse, links, a
  long line wrapped, gutters of several widths, and a line with none. Buttons stop and start the
  lines, mark what was read, and go to the end; a line typed in the field below is appended as
  `me`. The view's `linkActivated` and `scrolledBack` go to the status bar like every other
  event. On the board: a link clicked showed `linkActivated ... https://example.com/docs`, the
  wheel `scrolledBack: 1`, "To the end" `scrolledBack: 0`, the marker landed under the last line
  read, and a typed line came out with its link.
- **`guitest`:** `ChatView` is in the Describe list (made by `Create`), with its palette's tag,
  `appendMany` and both events' values described. It checks the defaults (`maxLines` 2000,
  `followEnd`, `wrap`, an empty `selectedText` and `font`), every method, and that the client's
  own changes send no event. It checks `maxLines`' range (0 and 100001 are `BadValue`, and the
  value is kept), and the palette: IRC's 99, the first ones set and the rest IRC's, upper case
  read back lower, a bad digit, a short one and 100 entries refused with the palette kept, and
  an empty list back to IRC's. It also checks that `followEnd` can't be set (`BadAccess`), and
  appends 200 lines at once past 50 kept, then clears them. `guitest: ok` on the board. The
  events are the user's (a scroll, a click), which `guitest` can't make, so the gallery is
  their test.


## 10. The input line's additions (2026-11-17)

The IRC client's input line needs Tab completion and the history on Up and Down (bmds-apps'
irc.md §6), which a `LineEdit` couldn't give: Tab moved the focus, Up and Down did nothing
unwrapped, and the cursor's place wasn't readable. Appended for the same release as `ChatView`:

| Kind | Name | Type | What |
|---|---|---|---|
| property | `LineEdit.cursor` | Int, RW | The cursor's place, a byte offset (past the text: `BadValue`) |
| property | `LineEdit.keys` | List, RW | Keys the program takes: `Tab`, `Up`, `Down`, `PageUp`, `PageDown` (another name: `BadValue`, the list kept) |
| event | `LineEdit.keyPressed` | On, (Atom key, Int modifiers) | A taken key pressed without Ctrl or Alt (Shift reported): told instead of its use. The modifiers as `Canvas.key` gives them |

- **Natively:** `LineEdit::setKeysTaken()` and `KeyTaken`, `cursor()` and `setCursor()`; the
  taken keys are checked first in `keyPressed()`, so the window's Tab never sees them. Ctrl+Tab
  and Ctrl+PageUp/Down still reach a `TabWidget` around it (`keyBeforeChild`, which comes first).
- **Tested:** `guitest` describes the three, moves the cursor (past the end and below 0
  `BadValue`), sets the keys and reads them back, refuses `Enter` keeping the list, and expects
  no event from the program's own changes. guigallery's Chat line takes Tab, Up and Down and shows
  each in the status bar with the cursor: on the board, Tab after "hello al" was told (cursor 8)
  and the focus stayed in the line.
- **ABI:** `classes.def`, `gui.lock`, angel's `api.lock` (5 lines).
