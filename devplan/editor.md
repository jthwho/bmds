# The Editor: Text Widgets, Syntax, and One Application for All Text

Status: **steps 1-5 built** (2026-10-22; 4b waits), then the debugger (§17) and remembered window
placement (§18): `TextDocument`, `TextEdit`, `FindBar`, `edit`, the languages and `CodeEdit`,
`TerminalWidget`, the angel IDE (§14 to §16), and its debugger (§19). Designed 2026-10-22. Scripting's step 7 (`scripting.md`
§5), widened by the user: the editor is the system's one place for text, of which the angel IDE
is one mode.

## 1. Goal

The user's direction (2026-10-22):
- **Re-use:** a text editor widget, and a code editor widget built on top of it.
- **Many syntaxes:** plan for highlighting (and more) in many languages, not only AngelScript.
- **One stop for text:** the same application edits a plain text file, is the angel IDE, and
  later a C/C++ IDE, when the system has a compiler of its own (Phase 8).

And from the scripting plan (`scripting.md` §4, "The IDE"): QBasic's spirit (F5 runs, errors in
place, F1 help), keyboard first (`controls.md`), the IDE never linking AngelScript.

## 2. Layers

| Layer | Where | What it is | Used by |
|---|---|---|---|
| `TextDocument`, `TextCursor` | `bmds_core` (pure, host-tested) | the text, edits, undo, find | every text widget |
| `Syntax`, `SyntaxScanner` | `bmds_core` (pure, host-tested) | languages as data; a line's styles from its text and the state the line before left | `CodeEdit`, a viewer, a diff, the terminal's `less` one day |
| `TextEdit` | `bmds_gui` | a multi-line text editor widget: drawing, the cursor and selection, keys, the mouse, the clipboard, undo | any program with a text area (notes, a log view, a mail body) |
| `CodeEdit : TextEdit` | `bmds_gui` | the gutter, colours from a syntax, brackets, indentation, comments, diagnostics, completion | the editor, a script console |
| `FindBar` | `bmds_gui` | find and replace on any `TextEdit` | the editor and any program with a `TextEdit` |
| `TerminalWidget` | `bmds_gui` | a terminal in a widget: a pty, the screen, drawing, selection | the editor's output pane; then `/bin/terminal` itself |
| `edit` | a built-in application | documents in tabs, files, menus, settings, languages' support | users |
| `LanguageSupport` | `edit` | what a language adds beyond colours: checks, completion, help, run, build | AngelScript first, C/C++ later |

The toolkit's widgets know nothing of any language's tools: a `CodeEdit` takes a `Syntax` for
its colours and hands out hooks (diagnostics to show, completions to offer); the application
fills them from its `LanguageSupport`. So a `CodeEdit` in another program (a script console in
a game, a config editor in the control panel) gets the same editing without the IDE.

## 3. `TextDocument` (bmds_core)

- **Lines:** an array of lines, each its bytes (UTF-8 as it comes, no terminator) in a PSRAM
  block of its own with room to grow, plus 16 bits of state for the syntax scanner and flags.
  Edits touch the lines they change; a 10,000-line file costs its bytes plus about 24 bytes a
  line. Chosen over a gap buffer or a piece table: the highlighter, the drawing and the cursor
  all work a line at a time, and a line array makes each of those O(1) to reach.
- **Positions:** `{line, byte}`; columns as drawn come from the text (wide glyphs two cells,
  tabs to the next multiple of the tab width), computed by the widget, which knows the font.
- **What the file was:** line endings (LF or CRLF, the first seen; mixed files keep each line's
  own), a final newline or not, a UTF-8 BOM or not: all kept on save. Invalid UTF-8 is kept
  byte for byte and drawn as the replacement glyph; NUL bytes the same.
- **Edits:** `insert(position, text)`, `remove(range)`, `replace(range, text)`; each returns the
  changed range and is told to observers (`Changed`: the range of lines before and after), so
  views redraw what changed and the scanner rescans from the first changed line.
- **Undo and redo:** a log of edits (position, removed text, inserted text), grouped: typing
  merges into one step until a pause (1 s), a move, or a word's end; a paste, a replace-all or
  an indent is one step. The cursor's position before and after is kept with each step, so undo
  puts it back. The log's size is limited (a setting; 4 MB by default), the oldest steps going
  first. The point the file was saved at is marked, so undoing back to it is "not modified".
- **Find:** forward and back from a position, case-sensitive or not, whole words, wrapping;
  regular expressions later (none in `bmds_core` yet). Replace one, replace all (one step).
- **Loading and saving:** from bytes (a file read whole; a size limit from memory, refusing
  with a message rather than failing midway). Saving writes a temporary file beside it and
  renames it over the old (power-safe on littlefs and FAT, `storage.md`), keeping the old
  file's mode.
- **`TextCursor`:** a position, an anchor (the selection's other end), the column it wants
  (kept through Up and Down), moves by character, word, line, page, document; several cursors
  on one document (a split view later, multiple carets perhaps).

## 4. `TextEdit` (bmds_gui)

A `ScrollArea` drawing a `TextDocument`'s visible lines with the fixed-cell font (the
terminal's: unscii, wide glyphs as two cells).
- **Keys:** arrows, Home and End (Home toggles between the first non-blank and column 0),
  Page Up and Down, Ctrl with them by word and to the ends, Shift with any of them to select;
  Backspace, Delete, Ctrl+Backspace and Ctrl+Delete by word; Enter; Tab and Shift+Tab (indent
  and unindent a selection); Insert for overwrite; Ctrl+A, Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z,
  Ctrl+C, Ctrl+X, Ctrl+V, Shift+Delete and Shift+Insert. The bindings are the toolkit's
  (`controls.md`), so an application or the user can change them.
- **The mouse:** a click places the cursor, a drag selects, a double click a word, a triple a
  line; Shift+click extends; the wheel scrolls; the middle button pastes the primary selection
  (as the terminal does); dragging a selection moves it (Ctrl copies) and dropping text or files
  works (`clipboard.md` §5).
- **The clipboard:** `GuiClipboard`; a selection is the primary selection, as in the terminal.
- **Drawing:** only the lines the clip shows; each line's runs of style (from the subclass, or
  plain), the selection, the cursor (a bar; a block in overwrite), blinking with the toolkit's
  cursor timer. Long lines scroll sideways. Word wrap is designed in (a line maps to one or more
  rows; positions convert through that map) and comes in step 2 for plain text.
- **For subclasses:** `styleLine(line, runs)`, `gutterWidth()` and `paintGutter()`,
  `decorations(line)` (underlines, marks), `keyPressed` before the defaults (auto-indent,
  completion keys), `textInserted()` (a typed `}` dedents), and the document's `Changed`.
- **Read-only** mode for viewers. Callbacks: changed, cursor moved, modified.

## 5. Syntax (bmds_core)

- **A language is data:** a `Syntax` holds its name, file name patterns (`*.as`, `*.c`,
  `Makefile`) and MIME types (`filetypes.md`), its line comment and block comment pair, string
  delimiters and their escape, character literals, multi-line strings (AngelScript's `"""`),
  numbers (C's forms: hex, binary, octal, exponents, suffixes), keyword lists by style (keyword,
  type, constant), a preprocessor's prefix (`#` at a line's start), the characters of an
  identifier, brackets, case sensitivity, and the indentation rules (indent after `{`, dedent at
  `}`; Python's `:` one day).
- **`SyntaxScanner`:** one table-driven scanner for all of them. Given a line's text and the
  state the line before ended in (in a block comment, in a multi-line string), it gives style
  runs and the state this line ends in. Kept per line in the document, so an edit rescans its
  line and goes on only while the end state changes (typing `/*` recolours the rest; most
  typing rescans one line). A large file's first scan goes as far as the view needs and
  continues on idle.
- **Styles:** normal, keyword, type, constant, number, string, character, comment,
  preprocessor, operator, bracket match, error; a **colour scheme** maps them to colours.
  Schemes: `classic` (the toolkit's light 3D look), `qbasic` (light grey on blue, as QBasic's
  editor), `dark`. A setting chooses.
- **The first languages:** plain text, AngelScript, C and C++, shell, INI and desktop entries
  (and `sysconf`'s files), Makefile, JSON, Markdown, diff. Those a table doesn't fit (Markdown's
  headings and emphasis, diff's lines, Makefile's recipes) get a small scanner function of their
  own behind the same interface.
- **Later, languages from files:** `/rom/share/syntax/*.syntax` and the user's own (an INI form
  of the same fields), so a language can be added without new firmware. The built-in tables are
  that structure, so the file reader only fills it.

## 6. `CodeEdit` (bmds_gui)

- **The gutter:** line numbers, a mark for a diagnostic (and a breakpoint, step 8 of
  `scripting.md`), a click selecting the line.
- **Colours** from its `Syntax` and the scheme; the current line's background; the bracket
  matching the one at the cursor.
- **Indentation:** Enter keeps the line's indentation, adding a level after an opening bracket;
  a typed closing bracket at a line's start dedents it; tabs or spaces and the width are
  settings (per language defaults: AngelScript and C 8 spaces, as this codebase; Makefile tabs).
- **Comments:** Ctrl+/ comments or uncomments the selected lines with the language's line
  comment.
- **Diagnostics:** given as a list (line, column, length, severity, message): a wavy underline,
  the gutter's mark, the message as a tooltip on hover, F8 and Shift+F8 to step through them.
- **Completion:** on a key (Ctrl+Space) or after a `.`, the application's list in a popup
  (name, kind, declaration, help), filtered as typing goes on; Enter or Tab takes one.
- **Help:** F1 asks the application for the name at the cursor.

## 7. `TerminalWidget` (bmds_gui)

The terminal's parts in a widget: a `Pty`, a `TerminalScreen`, the drawing (`TerminalView`'s
cells), selection and copy, paste, the keys sent to the pty, a scroll-back, a command started on
the pty (`ProgramLauncher`, a session of its own) and told when it ends. The editor's output pane
is one. Then `/bin/terminal` moves onto it (step 4b), so there is one terminal implementation;
until then the two share `TerminalScreen` and `TerminalView` as now.

## 8. The application: `edit`

`edit [FILE...]`, `/bin/edit`, a built-in application on the public toolkit like the others.
Working name "Edit" (QBasic's editor was DOS's EDIT); `scripting.md`'s *Workshop* was the IDE's
working name and this replaces it, open until the user settles it (§13).
- **Documents in tabs** (`TabWidget`), one window; Ctrl+Tab between them; a second window with
  File ▸ New Window. A file opened twice is one document.
- **Files:** New (from a template, `templates.md`: an empty text, an angel script, an angel
  game), Open (`FileDialog`), Save, Save As, Save All, Close, Revert, recent files. A file
  changed on disk (`DirectoryWatch`) is reloaded if unmodified, else asked. Closing a modified
  one asks Save, Discard or Cancel (`MessageBox`).
- **Menus:** File; Edit (undo, redo, cut, copy, paste, select all, find, replace, find next and
  previous, go to line, indent, comment); View (line numbers, word wrap, whitespace, the output
  pane, the scheme); Language (found from the file's type, or chosen); Run (for a language that
  runs: Run F5, Stop Shift+F5, Check, Arguments...); Help (F1 at the cursor, the language's
  guide and examples).
- **The status bar:** line and column, the language, line endings, overwrite, modified.
- **The output pane** (below a splitter, hidden until wanted): a `TerminalWidget` for a run (the
  program's own terminal: input works, `Console::` works, its colours show), and a list of the
  diagnostics (a click goes to the line).
- **The default for text:** `mimeapps.list`'s `text/plain` and the other text types open in
  `edit`; `vi` stays, for the terminal (`filetypes.md`). `.as` is `text/x-angelscript`, opened in
  `edit`, and run (not opened) from a terminal or by double click with Ctrl.
- **Settings** (`sysconf`, `editor/...`): the scheme, tab width and tabs or spaces (per language
  where they differ), line numbers, word wrap, the undo log's size, the scripts' folder
  (`~/scripts`), the examples' folder (`/rom/share/angel/examples`).
- **Keys:** every action bound, declared in its `controls.conf` (`controls.md`).

## 9. `LanguageSupport`

An interface in `edit`; each language that has tools implements it. All but `syntax()` are
optional.
- `syntax()`: its `Syntax`;
- `check(document)`: diagnostics, asked after half a second of no typing and on save;
- `complete(document, position)`: candidates;
- `help(document, position)`: a declaration and its help text;
- `run(document)` and `stop()`: a command on the output pane's terminal, its end, and where it
  failed (file, line, message), shown in the editor;
- later `build()`, `debug()` (`scripting.md` step 8).

**AngelScript** (the first):
- **`angel --serve`**, one process while a script is open: requests on its stdin, answers on
  its stdout, a line protocol (a header line `check PATH LENGTH`, then that many bytes: the
  unsaved text, so diagnostics follow typing; `complete PATH LINE COLUMN LENGTH` and the text;
  `help NAME`; answers in `angel -c`'s diagnostic format and tab-separated fields, ended by
  `end`). The same program as runs scripts, so its diagnostics agree with them
  (`scripting.md` §4). The protocol is ABI once shipped (`scripting.md` §7).
- **Completion:** the registered API (`--api-help`, read once), the script's own globals,
  functions and classes (from the check's compile), and members after `.` of a variable whose
  declared type is known; locals' types later.
- **Run:** F5 saves and starts `angel --report-fd 3 FILE` on the output pane's pty, a pipe on
  its fd 3 carrying what failed (a compile error or an uncaught exception's file and line) in
  machine form, so the editor goes to the line. Stop: SIGTERM, then SIGKILL after a second.
  Windows the script opens are its own, as from a terminal.
- **Help:** F1 shows the declaration and help line from `--api-help`; Help ▸ Examples opens
  `/rom/share/angel/examples` (read-only there: Save As to the scripts' folder).

**C and C++** (later, with a compiler on the device): the same `Syntax` already colours them;
check through the compiler's `-fsyntax-only`, run through build then run, errors in the GCC
format the diagnostics list already reads.

## 10. Budget

- **Flash:** the widgets and the scanner perhaps 40 KB, the languages' tables a few KB each,
  `edit` perhaps 60 KB; the schemes and templates bytes.
- **RAM:** a document its bytes plus about 24 bytes a line, and its undo log (4 MB at most);
  the widgets their state and one row of cells in internal RAM for drawing (as the terminal's);
  `angel --serve` one more angel process (about 200 KB) while a script is open.
- **CPU:** nothing idle (the cursor blink a timer; no polling); typing rescans a line and redraws
  a line; a check every half second of idle typing (a compile of the script, milliseconds).

## 11. ABI

Frozen once shipped: `edit`'s command line, its settings, `text/x-angelscript`, `angel --serve`'s
protocol and `--report-fd`, the `.syntax` file format when it comes. The toolkit's C++ classes
aren't userspace ABI (built-in applications only), as the rest of `bmds_gui`.

## 12. Steps

1. **`TextDocument` and `TextCursor`** (host-tested: edits, line endings kept, undo grouping and
   the saved mark, find and replace, UTF-8 at the edges, large files).
2. **`TextEdit`, `FindBar`, and `edit` for plain text:** tabs, files, menus, the status bar,
   word wrap, settings, `controls.conf`, `edit` the default for text. Closes Phase 8's
   "on-device text editor".
3. **Syntax and `CodeEdit`:** the scanner and the first languages (host-tested), the schemes,
   the gutter, brackets, indentation, comments; languages found by file type.
4. **`TerminalWidget`** and the output pane; then **4b**, `/bin/terminal` moved onto it.
5. **AngelScript's support:** `angel --serve` and `--report-fd`, diagnostics as you type, Run and
   Stop, the failing line, F1 help, completion, templates and examples, `text/x-angelscript`.
6. **Later:** languages from `.syntax` files, regular expressions in find, split views, C and C++
   with the compiler.

## 13. Open questions

- **The name.** "Edit" (`/bin/edit`) as a working name; *Workshop* was the IDE's.
- **The scheme by default:** `classic` to match the desktop, or `qbasic` for nostalgia.

## 14. As built: steps 1 and 2 (2026-10-22)

- **`TextDocument` and `TextCursor`** (`bmds_core`, host-tested: `tests/host/textdocument.cpp`,
  13 cases): loading and saving give back the same bytes (LF, CR LF, mixed, no final newline, a
  byte order mark, invalid UTF-8 and NUL); edits across lines with their changes told; undo
  grouping (a word and its space a step, a pause, a move, the saved step never merged into);
  the saved mark through undo, redo and a step dropped by the log's limit; find with each flag
  and replace-all as one step; words, characters and columns (tabs, wide characters, a stray
  continuation byte); the cursor's moves; running out of memory midway changing nothing; a
  100,000-line file.
- **A loaded file is one block:** its lines point into the file's bytes until each first grows
  (then it gets a block of its own), and the line array is made the exact size. On the board a
  50,000-line file (889 KB) costs 1.70 MB of PSRAM, its bytes and 16 bytes a line (2.77 MB with
  a block for every line and the array doubled), and opens in 0.6 s from `/tmp`. `edit` itself
  is 80 KB.
- **`TextEdit`** (`bmds_gui`) on `ScrollArea`: rows drawn as `TextCell`s (`Painter::cells()`, one
  row's cells in internal RAM), text on a 4-pixel column, typing redrawing only its line
  (`GuiWindow::invalidate()`); the keys, the mouse (double and triple clicks, dragging past an
  edge scrolling on a timer, the gutter selecting lines), the clipboard, the primary selection
  offered once and made when asked, drops. Not yet: word wrap (after the IDE: code doesn't wrap),
  moving a selection by dragging it.
- **`FindBar`**: find (Match case, Whole word, wrapping), replace and Replace All, go to line;
  its row takes only the room its message needs.
- **`TabWidget::removeTab()`**, added for closing documents.
- **`edit`** (`bmds_edit`, `/bin/edit`): tabs, New, Open, Save (to `.NAME.edit-save` beside the
  file, renamed over it), Save As, Save All, Revert, Close, Quit; Save, Discard or Cancel for a
  modified document, one at a time when quitting; the File, Edit, View (line endings) and Help
  menus, the Edit menu's items enabled as they apply; the status bar (line and column, INS or
  OVR, LF or CRLF); `editor/tab-width`, `editor/insert-spaces`, `editor/auto-indent`, followed
  live; `edit.desktop`, and `edit` before `vi` for text in the default `mimeapps.list` (a
  `/cfg` that has the file already keeps it: defaults only add files).
- **Keys:** hard-wired as the media player's and the line editor's are; `controls.conf` for the
  editor's actions waits until applications declare theirs (`controls.md`).
- **Found on the board:** a file handle's `read()` ends with `EndOfFile`, not 0 bytes; the first
  `edit FILE` said "Can't open: End of file".
- **The window manager closes a window before telling it** (its close button): `edit` with
  unsaved changes opens again where it was and asks. A close request a window can refuse would
  be better (a toolkit addition, later).
- **Tried on the board** (`win key`, `win hid`, screenshots): typing, tabs, auto-indent, Shift+Home,
  undo, Save (the bytes checked with `od`, no temporary file left), find, go to line, closing a
  modified document with the window's button (asked, No, quit, the file untouched), a 50,000-line
  file paged through and Ctrl+End.


## 15. As built: step 3 (2026-10-22)

- **`Syntax`** (`bmds/textsyntax.h`, `bmds_core`: not `syntax.h`, the shell's; the style enum is `SyntaxStyle`, `TextStyle` being
  the terminal's attributes already): nine languages, AngelScript, C, C++, shell, INI (`.conf`,
  `.desktop`, `.list`), JSON, Makefile, Markdown, diff. Six are tables for one scanner (line and
  block comments, strings with their escapes, `"""` strings across lines, C's numbers, three word
  lists by binary search, a word before `(` a function, `#include <...>`, shell variables, JSON
  keys, INI sections and keys); Markdown, diffs and Makefiles have scanners of their own (a
  Makefile's recipe lines are the shell's). A `#!` line is a comment. `Syntax::forFile()`: the
  file's name, else the interpreter its `#!` line names (`/usr/bin/env` looked past).
  Host-tested (`tests/host/syntaxscan.cpp`): every word list sorted, every language's colouring,
  states across lines, the span limit.
- **`CodeEdit`** (`bmds_gui`) on `TextEdit` (new hooks: `paintOver()`, `cursorMovedHook()`,
  `columnX()`, `relayoutContent()`):
  - each line's end state kept in the document; an edit rescans from its line and stops at the
    first line after it whose end state is as before; lines are scanned once as drawn;
  - three schemes (`classic`, `qbasic`, `dark`), followed live from `editor/scheme`;
  - the gutter: line numbers (`editor/line-numbers`), the current one brighter, a diagnostic's
    mark; the current line's background;
  - the bracket at (or before) the cursor and its match, brackets in strings, characters and
    comments not counted;
  - Enter: the indentation as far as the cursor, a level more after an opening bracket, a closing
    bracket right after the cursor to a line of its own; a closing bracket typed first on a line
    lined up with its opening line (the same undo step as the typing); Ctrl+/ toggles the line
    comment at the block's least indentation; a Makefile indents with tabs;
  - diagnostics (for step 5): a wavy underline, the gutter's mark, the message as a tooltip,
    moved with their lines by edits, `nextDiagnostic()`.
- **`edit`:** the language from the file (on open and Save As), View ▸ Language to choose,
  View ▸ Colours and Line Numbers (they set the settings, saved), Edit ▸ Toggle Comment, the
  language in the status bar; a file it can't write (`/rom`'s) is read-only ("(read-only)" in
  the title; Save is Save As).
- **Tried on the board:** `piano.as` and `snake.as` coloured; a block typed with its
  indentation (the bytes checked); two lines commented with Ctrl+/; the bracket match skipping a
  commented-out `{`; the three schemes switched live with `sysconf set` (then `setdef`).

## 16. As built: steps 4 and 5 (2026-10-22)

- **`TerminalWidget`** (`bmds_gui`): a pty a run, the screen kept between runs (`clear()`), drawn
  with `TerminalView` as the terminal is; a reader thread (the output onto the screen, a redraw
  posted) and a runner thread (the command started with `ProgramLauncher::start()` as a session
  of its own, `waitChild()` for its status, then the pty told); `Command::extraFds` (descriptors
  past 2: Run's report pipe); `stop()` (SIGTERM, then SIGKILL); keys through `TerminalKeys`,
  Shift+Page Up and Down, selection (words, lines), Ctrl+Shift+C and V, the middle button; the
  terminal's colours. **Step 4b waits:** `/bin/terminal` moves onto it once the widget has what
  the application has besides (OSC 52, drops, the right button's menu, the bell, titles).
- **`angel --serve`** (`programs/angel/main.cpp`): one engine, requests on stdin (`check PATH
  LENGTH` and the text, `api`, `quit`), each answer's lines then `end`: `diag SEVERITY FILE LINE
  COLUMN MESSAGE`, `symbol KIND NAME DECLARATION FILE LINE` (the script's functions, globals,
  classes with their members, enums), `api KIND NAME DECLARATION HELP` (362 records). A check
  builds and runs nothing: globals aren't initialized (`asEP_INIT_GLOBAL_VARS_AFTER_BUILD` off),
  so `Window w(...)` opens no window. The entry checks (statements and a `main()`, nothing to
  run, `main`'s signature) are made once for running and checking (`entryOf()`).
- **`angel --report-fd N`**: every diagnostic also as `SEVERITY FILE LINE COLUMN MESSAGE` on N, and
  an uncaught exception as `exception ...`. All diagnostics go through one `diagnostic()`.
- **Host-tested** (`tests/scripts/serve.py`, ctest `angel_serve`): the API's kinds, a method with
  its help; a check's errors and where, no symbols from a failed build, symbols (a class, its
  method and property, a variable, an included function with its file and line); main with
  statements; a missing include; an unknown request; `--report-fd` with an exception, a compile
  error, a bad descriptor.
- **`LanguageSupport`** (`bmds_edit`): check, complete, help, type names, the run command and its
  report read; **`AngelSupport`** the first. Its `angel --serve` starts at the first script
  opened (pipes for stdin and stdout, stderr to `/dev/null`), its answers read on a thread of
  its own and handed to the loop; the API asked first; a check at a time (one under way: asked
  again after 300 ms); the last successful build's symbols kept for completion while the text
  has errors.
- **`edit`'s AngelScript mode:**
  - **checks** half a second after the typing stops, and on open: the errors underlined, marked
    in the gutter, in the status bar ("2 errors, 0 warnings (F8)") and the Problems list (a click
    goes there); F8 and Shift+F8 step through them; an included file's errors at the top, saying
    where;
  - the API's types and the script's classes coloured as types (`CodeEdit::setTypeNames()`);
  - **F5** saves and runs `angel --report-fd 3 FILE` in the Output pane (its input typed there),
    an untitled script named first (Save As in `editor/scripts`, `${HOME}/scripts`, made if
    missing), then run; where it failed, the cursor goes and the message shows; **Shift+F5**
    stops it (again: kills it), "-- stopped --"; Run's keys work while the Output has the keys
    (`keyFirst()`);
  - **F1**: the declarations and help of the name at the cursor in the Help pane (`w.fillRect`:
    `w`'s type found);
  - **Ctrl+Space**: a menu of the names that fit, members after `.` and `::`; the chosen one
    replaces what was typed. A variable's type comes from the last successful check, else from
    the text (the nearest `Type name` before the cursor), so it works while the script has
    errors;
  - File ▸ New from Template (a text document, an AngelScript program, an AngelScript game, each
    checked to compile), Help ▸ Examples (`editor/angel-examples`), Help ▸ Help on the Word;
  - the pane (Output, Problems, Help) under a splitter, hidden until wanted, F12 toggles it.
- **`text/x-angelscript`** (`.as`) in the file types; `text/*` opens in `edit`.
- **Measured on the board:** `angel -c` of a 90-line example 0.12 s with the program's start;
  the server is started once, so a check while typing is the compile only.
- **Tried on the board:** two errors underlined after opening (and their gutter marks, the status);
  a run reading a name typed in the Output and failing on line 4 (the cursor there, the message
  in the status bar); F1 on `input` (both overloads); Ctrl+Space after `w.fi` (fillRect,
  fillCircle, fillEllipse, fillPolygon), Enter inserting it; the game template saved by F5's
  Save As and run; an endless loop and the game stopped with Shift+F5 from the Output pane.
- **Found on the board:** the server's first answer is the API's: unrecorded, it was taken for
  the first check's ("No problems" for a script with two errors); Ctrl+Space and Ctrl+F5 were
  shadowed by the Ctrl branch of the window's keys (now one table); Shift+F5 went to the
  running program through the Output's terminal.
- **Left:** a local variable's type for completion (the server knows globals; the text heuristic
  finds `Type name` declarations); word wrap; moving a selection by dragging; regular
  expressions in find; `.syntax` files; C and C++ with a compiler (Phase 8); the debugger
  (`scripting.md` step 8) on this pane.

## 17. The debugger (`scripting.md` step 8; designed 2026-10-22)

**What the writer gets** (QBasic's and Visual Studio's habits, keyboard first):
- **Breakpoints:** F9 toggles one on the cursor's line, as does a click in the gutter. A red
  dot marks it. Breakpoints move with their lines as the text is edited. They last while the
  document is open.
- **Starting:** F5 runs as before. If the script has breakpoints, F5 runs it under the debugger
  instead. F11 (step into) and F10 (step over) start it under the debugger stopped at its first
  line. Ctrl+F5 always runs it without the debugger.
- **While stopped:**
  - The line about to run is marked by a yellow arrow and a yellow background. The editor shows
    it, opening an included file if needed.
  - F5 continues, F10 steps over, F11 steps into, Shift+F11 steps out, and Shift+F5 stops.
  - Pause (the Pause key, or Run ▸ Pause) stops a running script at the next line it runs.
- **F10 while no session runs:** it keeps opening the menu bar. It is a step key only while a
  debugging session is under way: the menus are still Alt's.
- **An uncaught exception** stops the script where it was raised, before it unwinds, so its
  variables can be seen. Continuing ends the script as before: the report, the cursor on the line.
- **The Debug tab** in the pane:
  - **Call stack:** a list, the innermost frame first. Picking a frame shows its line and its
    variables.
  - **Variables:** a tree.
    - The roots, in order: the watches, the frame's locals (and `this` in a method), then
      Globals.
    - Globals is expanded at first for the top level, where a script's variables are globals.
    - Objects, arrays and handles expand to their members and elements. A value shows as
      `name: type = value`.
    - What was expanded stays expanded from one stop to the next.
  - **Watches:** Debug ▸ Add Watch (Shift+F9) watches the selection, or the name at the cursor.
    Delete on a watch's row removes it.
- **Menus:** a Debug menu with the actions and their keys; Run keeps Run, Stop and Check.

**The protocol** (ABI once shipped, as `--serve`'s; `docs/ABI.md`):
- **Starting it:** `angel --report-fd 3 --debug 4 FILE`.
  - The script is built with line cues, and runs only once the IDE says so.
  - Commands come on descriptor 4, a line each. Events and answers go on the report
    descriptor, with the report's own lines (`error`, `exception`).
  - Two descriptors, not a third: the terminal widget passes two.
- **Commands, the IDE to angel:**
  - `break FILE LINE` sets a breakpoint, and `clear FILE LINE` clears one. Neither is answered.
    They may come while the script runs.
  - `go`, `step`, `next` and `out` resume: continue, step into, step over, step out.
  - `pause` stops at the next line run.
  - `stack`, while stopped, answers `frame LEVEL FUNCTION FILE LINE` lines and then `end`.
  - `vars LEVEL PATH`, while stopped, answers `var NAME TYPE VALUE CHILDREN` lines and then
    `end`.
    - LEVEL is a frame's, or `g` for the globals.
    - An empty PATH lists the frame's variables; otherwise PATH's members or elements are listed.
    - A path is a name followed by `.member` and `[index]` parts.
    - CHILDREN is how many there are to expand (0 for a plain value).
    - A failure answers `error MESSAGE`, then `end`.
  - `value LEVEL PATH` answers one `var` line (a watch), or `error MESSAGE`, then `end`.
- **Events, angel to the IDE:** `stopped REASON FILE LINE [MESSAGE]`.
  - REASON is `break`, `step`, `pause` or `exception`; an exception's line includes its message.
- **Waiting for commands:** angel waits for a resuming command before the first line, and
  after each `stopped`. Until the IDE has sent one, the script doesn't run at all.
- **The IDE closes the channel:** the script runs on to its end undebugged.

**In `angel` (`programs/angel/debug.cpp`):**
- **The line callback:** it is set on the script's context only under `--debug`. Contexts
  nested in it (games' `tick()` and `draw()`, events) share it.
- **The fast path:** with no breakpoints and no step pending, it counts down. Every 256 lines
  it polls the command descriptor (`poll()` with a zero timeout), so `pause` and new
  breakpoints take effect.
  - A script blocked in `input()` or a game's wait stops at its next line, not before.
- **Breakpoints:** at most 64, each a file and a line.
  - Files are compared by their canonical paths (`realpath`).
  - A breakpoint stops when its line is entered from another one or another depth, not on each
    statement of the line.
- **Stepping** compares against where the stop was:
  - **Into:** any other line, or any other depth.
  - **Over:** a lower depth, or another line at the same depth.
  - **Out:** a lower depth.
  - Depth is the call stack's size, nested calls included.
- **Values are described without running script code:** `toString()` isn't called (it would
  run the script under its own debugger).
  - Primitives, strings (quoted, escaped) and enums are shown as values; handles are followed.
  - Arrays show their first elements (`[1, 2, 3, …]`), script objects a summary of their first
    properties (`{x: 1, y: 2}`), and registered objects `<Window>`.
  - At most 80 bytes a value, and 500 children a list.
- **Exceptions:** the exception callback stops with `exception` unless the script will catch
  it (`WillExceptionBeCaught()`).

**In `edit`:**
- **`LanguageSupport::debugCommand(path, reportFd, commandFd, argv)`:** a language that can
  debug fills in the command; the others answer `NotSupported`.
- **`DebugSession`** (`components/bmds_edit/src/debugsession.*`):
  - A reader thread splits the event descriptor into lines and posts them to the GUI loop.
  - The session keeps the report's lines for the run's end, as Run's report.
  - Answers are matched to the questions asked, in order.
- **`CodeEdit`** (the toolkit, for any language):
  - `setBreakpointsEnabled()` lets gutter clicks toggle breakpoints.
  - `toggleBreakpoint()`, `breakpoints()` and the `BreakpointsChanged` callback: the lines move
    with edits, as diagnostics' do.
  - `setExecutionLine()` draws the arrow and the background.
- **`TreeView`:** gains rows without children (`TreeRows::setLeaf()`), so a plain value has
  no box.

## 18. Remembered window placement (2026-10-22; the user's request)

- **The window manager:**
  - `WindowManager::Placement`: the normal (restored) rectangle and whether the window is
    maximized.
  - `placement(id)` returns it while the window is open, and `ClosedAt` passes it at the close.
    A snapped window's normal rectangle is the one from before it snapped.
- **`WindowPlacement`** (`bmds_ui`, pure, host-tested; `WindowManager::Placement` is it):
  `parse()` and `format()` read and write the setting's text, `X Y W H` and then `maximized`
  when it was.
- **`GuiWindow`:**
  - `placement()`: live while open, the last one after it closed.
  - `show(const Placement &)`: opens at the normal rectangle, then maximizes if it was. A
    rectangle off the work area is moved back onto it, and made no larger than it.
- **`edit`** declares the setting `editor/window` (empty by default: the centred default
  place). It saves the setting when its window closes, by the close button or File ▸ Exit, and
  uses it when it starts.
- **Other applications** use the same calls when they want it; `edit` is the first.

## 19. As built: the debugger and placement (2026-10-22)

- **Files:**
  - `programs/angel/debug.cpp`: the line and exception callbacks, the commands, describing values.
  - `components/bmds_edit/src/debugsession.*`: the protocol's client.
  - `components/bmds_edit/src/editdebug.cpp`: the Debug tab, breakpoints, stops and watches.
  - In the toolkit: `CodeEdit`'s breakpoints and line about to run, `TreeView::setHasChildren()`
    and `setRowData()`, and `WindowPlacement` in `bmds_ui`.
- **Keys as built:**
  - F5 runs, or continues from a stop; Ctrl+F5 runs without the debugger.
  - Check moved from Ctrl+F5 to Ctrl+F7.
  - F9 toggles a breakpoint, Ctrl+Shift+F9 clears them all, and Shift+F9 adds a watch.
  - F10, F11 and Shift+F11 step; Pause pauses.
- **The pane:** a stop shows the Debug tab and raises the editor over the program's windows.
  Resuming shows the Output, where a running script's input is typed.
- **Tests:**
  - `tests/scripts/serve.py` checks the protocol: a breakpoint, the stack, a frame's variables,
    the globals, members, elements, paths and their errors, step over and out, the stop on an
    exception and then the report, a step from the first line, a pause, and running on once the
    editor's end closes.
  - `tests/host/gui.cpp` checks leaf rows and the placement's text.
- **Tried on the board:**
  - A breakpoint in a function; expanding the globals and an object, which stayed expanded over
    a step; step over, step out, clearing the breakpoints, and continuing to an uncaught
    exception, which stopped with the variables there; a watch.
  - Snake under the debugger, paused in `draw()`, the stack showing the nested
    `Window::run()`.
  - The editor closed while a script was stopped: both ended.
  - Placement: maximized, quit, reopened maximized; restored to the place before; moved and
    closed by the window manager, reopened there; a saved place off the screen brought back onto
    it.
- **Found on the way:**
  - **A header name clash from step 7.** `bmds_core`'s `bmds/syntax.h` shadowed the shell's
    `bmds/syntax.h` in components that include both. An incremental build hid it until
    `windowmanager.h` changed. The editor's is now `bmds/textsyntax.h`.
  - **Closing a tab** moved only some of each later document's fields, losing its `id` and
    read-only state. Whole documents move now.
  - **F5 at a stop** after all breakpoints were cleared said "running already". It continues now.
  - **The Pause key** goes to the window that has the keys. While a game's window has them,
    pause from the editor's window or its Debug menu.
- **Behaviour of the line cues:**
  - Stepping out of a function stops at the caller's next cue. In a `for` loop, that can be the
    increment rather than the rest of the calling line.
  - F11 at the start stops at the first statement with a cue. Since the top-level fix
    (`scripting.md` §15), a global declared among the statements is initialized where it
    stands, so F10 steps over each one.
- **Left:**
  - Showing a value when the pointer is over its name.
  - Conditional breakpoints and hit counts.
  - Editing a value while stopped.
  - Breakpoints kept with the file between sessions.
- **Later, with the guide** (`scripting.md` §14, §15): Help ▸ Guide and Help ▸ Examples are
  submenus of the folders' scripts (`editor/angel-guide`, `editor/angel-examples`), not a file
  dialog.
