# Terminal Model: ANSI/VT and 24-bit Color

Status: **Phase 1b part done** (color model, SGR, VT parser, UTF-8, color filter, terminal modes, `term`); the screen model and HDMI renderer are Phase 4.

## 1. Goal

Make ANSI/VT control sequences and 24-bit color first-class across BMDS:

- **One output format everywhere.** Programs, the shell and the kernel write standard ANSI/VT text (UTF-8 plus ECMA-48/xterm escape sequences, including SGR truecolor `ESC[38;2;r;g;bm`). BMDS has no private color API inside the byte stream.
- **Each console renders it as well as it can:**
  - The serial console passes the bytes through, and the terminal emulator on the PC renders them.
  - The HDMI text console (Phase 4) parses and renders them itself, with full 24-bit color.
- **Programs can ask what the console supports**: color depth, size, whether it is a terminal at all. They degrade gracefully, and helpers do the degrading for them.
- **Terminal modes** ("termios-lite"):
  - Ctrl-C interrupts the foreground job.
  - Programs such as a future editor can switch to raw input.

## 2. Interface

### `bmds_core` (pure, host-tested)

| Header | Provides |
|---|---|
| `bmds/color.h` | `Color`: `Default`, `Indexed(0..255)` or `Rgb(r,g,b)`, in 4 bytes. The xterm 256-color palette as constexpr rodata. `toIndexed256()`, `toIndexed16()` (nearest match). |
| `bmds/textstyle.h` | `TextStyle`: foreground and background `Color`, plus attribute flags (bold, dim, italic, underline, blink, reverse, invisible, strikethrough). |
| `bmds/sgr.h` | `Sgr::transition(from, to, depth, out)`: emits the shortest SGR sequence that changes the style, downgrading colors to `depth` (`None`, `Ansi16`, `Ansi256`, `TrueColor`). `Sgr::apply(params, style)`: parses SGR parameters into a style, for the renderer and the filter below. |
| `bmds/vtparser.h` | `VtParser`: an ECMA-48/DEC state machine (the Paul Williams model): ground, escape, CSI entry/param/intermediate, OSC string, DCS passthrough/ignore. Also a UTF-8 decoder. It reports actions to a handler: `print(codepoint)`, `execute(c0)`, `csiDispatch(params, intermediates, final)`, `escDispatch`, `oscDispatch`. Fixed-size state (about 64 bytes), no allocation. Malformed input never stalls it. |
| `bmds/utf8.h` | UTF-8 encode/decode helpers, and `displayWidth(codepoint)` (wide CJK/emoji = 2, combining = 0) from a compact rodata range table. |

### Console additions (`bmds_console`)

```cpp
enum class ColorDepth : uint8_t { None, Ansi16, Ansi256, TrueColor };

struct TermMode {                  // termios-lite, per console
        bool signals = true;       // ^C raises interrupt on the foreground job (ISIG)
        bool crToNl  = true;       // input CR -> NL (ICRNL)
        bool nlToCrNl = true;      // output NL -> CR NL (ONLCR), the serial console's current behavior
        bool colors = true;        // SGR colors fitted to colorDepth() (Phase 2: off for file transfers)
        bool logs = true;          // log lines shown (Phase 2: off for file transfers; the history keeps them)
        static constexpr TermMode binary();  // all off: bytes pass untouched (rz/sz)
};

class Console : public Stream {    // Stream: see shell-language.md
        ColorDepth colorDepth() const;
        void       setColorDepth(ColorDepth);    // serial: the host's terminal can't be detected
        Size       size() const;                 // serial: probed with ESC[18t, falling back to ESC[6n; default 80x24
        TermMode   mode() const;
        void       setMode(const TermMode &);
        Interrupt &interrupt();                  // ^C, raised from the RX path; see shell-language.md
};
```

- **Capabilities.** `Capability::Color` (pre-release) is replaced by `ColorDepth`. `Ansi` stays: every BMDS console speaks ANSI, and a future pipe or file is the non-terminal case (`isTerminal()`).
- **Serial color depth.**
  - Default `TrueColor` (`CONFIG_BMDS_USJ_COLOR_DEPTH`). Every current terminal emulator on Linux, macOS and Windows renders it.
  - Changed at runtime with the `term` built-in: `term color none|16|256|truecolor`.
  - The shell exports it to programs as `COLORTERM=truecolor` and `TERM=xterm-256color`, the conventional variables. It also honors `NO_COLOR`.
- **Ctrl-C** (done in Phase 1b step 2). When `signals` is set, the console's RX path (ISR side) spots `0x03` and raises the console's `Interrupt` instead of queueing the byte. So a running loop or pipeline can be stopped even though nobody is reading input. This resolves the Phase 1 open item. Programs get `SIGINT` from the loader, which waits on that interrupt (`programs.md` §15).
  - **Like a tty, Ctrl-C flushes pending input.** Bytes received before it are discarded: the rest of its USB packet in the ISR, and what the RX ring already holds by a discard position (`ByteRing::writePosition()`/`discardTo()`) that the reader applies, since only the reader may consume. Bytes after it are kept. So `junk^Cuptime⏎` arriving in one packet cancels `junk` and runs `uptime`, and a line typed ahead of Ctrl-C never runs.
  - `Console::read()` reports a raised interrupt before returning input, so a Ctrl-C is handled ahead of keys typed after it. At the prompt the shell turns it into the editor's line cancel (`^C`). During a command, the shell echoes `^C` when the command returns and sets the exit code to 130.
  - Only `TermMode::signals` exists so far; `crToNl`/`nlToCrNl` come with step 6.
- **Raw mode.**
  - A program that wants raw keys clears `signals`/`crToNl` while it runs. The shell restores the mode when the program exits.
  - The line editor already treats the console as raw: it is readline-like and lives in the shell, not in the console.

### Downgrading color

Most code should just write ANSI and let the console deal with it. Two mechanisms:

1. **Producers that know the depth.** Built-ins and kernel code that format their own colors call `Sgr::transition(..., console.colorDepth(), ...)`, or the shell's `printStyled()` helpers. These emit only what the console supports, and nothing at all at `None` or when stdout isn't a terminal (the `ls --color=auto` rule).
2. **A filter for producers that don't.** A program that hardcodes truecolor on a 16-color console goes through a `ColorFilter` stream. It is a `VtParser` that rewrites SGR parameters to the console's depth and passes everything else through unchanged.
   - It is installed by the console only when the depth is below `TrueColor`, so it costs nothing in the common case.
   - The serial console at `TrueColor` never parses its output.

### Log colors

The history stays plain text, so `log show | ...` stays parseable. The console sink colors the level letter by depth: `W` yellow, `E` red, `D` dim. This is skipped at `None`.

## As built (Phase 1b step 6)

- **`bmds_core`:** `utf8.h` (decode, encode, a byte-at-a-time `Utf8Decoder`, `displayWidth()` from two binary-searched range tables: ~180 zero-width and ~60 wide ranges, an approximation of Unicode good for cursor arithmetic); `color.h` (`ColorDepth`, the 4-byte `Color` with xterm's palette computed, not stored, `downgrade()` to the nearest cube/gray-ramp or ANSI color; `TextStyle`); `vtparser.h` (`VtParser`, the Williams state machine with UTF-8 in the ground state, `:` sub-parameters, private markers, OSC up to 64 bytes, DCS/SOS/PM/APC consumed, CAN/SUB/ESC abort; `Sgr::apply()` and `Sgr::transition()`, which picks the shorter of an incremental change and a reset-and-set).
- **`ColorFilter`** (`bmds_console`, pure): text and other sequences pass byte for byte; each SGR is replaced by `transition(sent, requested, depth)`; sequences longer than 96 bytes pass through untouched. The serial console makes one (in PSRAM) the first time its depth drops below truecolor, so at truecolor nothing is parsed.
- **`Console`:** `colorDepth()`/`setColorDepth()` replace the `Color` capability; the size is stored (`setSize()`); `TermMode` has `signals` and `nlToCrNl` (ONLCR, for raw programs). `crToNl` was dropped: the line editor already takes CR, LF and CR LF, and a raw program wants the bytes the key sent.
- **Serial console:** initial depth from `CONFIG_BMDS_USJ_COLOR_DEPTH` (truecolor); `writeParts()` writes several pieces as one atomic write (used for colored log lines without a stack buffer); filtered writes return all-or-nothing counts.
- **Log levels:** the console sink colors the level letter of BMDS lines (`[t] L …`) and ESP-IDF lines (`L (ms) …`): E red, W yellow, D dim; nothing at depth `None`. The history stays plain text.
- **`term`** (`termcommand.cpp`): `term` shows terminal, color depth and size; `term color none|16|256|truecolor`; `term size COLSxROWS`, or `term size` alone asks the terminal (`ESC [ 18 t`, reply `ESC [ 8 ; rows ; cols t`, 500 ms). It keeps `COLORTERM`, `COLUMNS` and `LINES` up to date. The shell exports `TERM=xterm-256color` and, at truecolor, `COLORTERM=truecolor`.
- **`CommandContext::colorDepth()`:** `None` unless stdout is a terminal and `NO_COLOR` is unset, for commands that color their own output.
- **Prompt:** on the target `PS1` is `\[\e[1;32m\]bmds\[\e[0m\]> ` (bold green); the filter fits it to the terminal.
- **Line editor:** UTF-8 aware: characters are assembled before insertion (bytes ≥ 0x80 used to be dropped), the cursor moves and deletes by character, and terminal cursor movement counts display columns (a CJK character or emoji moves two). **Hard-won:** an edit must move the terminal cursor before changing the buffer, because the column distance is measured over the text the cursor crosses.
- **Tested:** UTF-8 edge cases, palette downgrades, SGR round trips over attribute/color combinations, a VT sequence corpus fed whole and byte by byte, the filter at each depth (also split byte by byte), editor UTF-8, `term` including a probe reply. On the board: the depth switch rewriting truecolor to `91`/`1;34`, `none` keeping bold, `café 日本` editing, dim `D` log lines.
- Size: image +9.5 KB; static RAM +8 bytes.

## Terminal size, `clear`, `reset` (Phase 3)

Programs need the terminal's size (BusyBox's `vi` and `less` used 80x24 whatever the window). The `Console` is where it lives (`size()`, `setSize()`), and every console fills it its own way; programs read it with `TIOCGWINSZ` (the SDK's layer) or `bmds_term_get`, the shell exports `COLUMNS` and `LINES`.

- **Serial:** a terminal can't tell the device when its window changes, so the device asks. `Console::requestSize()` writes `ESC [ 18 t`; `Console::read()` takes the answer, `ESC [ 8 ; rows ; cols t`, out of the input wherever it comes among typed keys (holding a partial answer across reads, so readers taking a byte at a time get it too) and sets the size. Only while a request is outstanding (2 s) is anything held, so a real Escape key is never kept long; with no request the input passes untouched (the fast path, as for `rz`). The shell asks at every prompt, so a resize is seen by the next command; after three unanswered requests (a terminal that doesn't do it, a script on the port) it stops, until `reset` or `term size`. `term size` asks and waits for the answer; `term size COLSxROWS` sets it. Consoles that can be asked say so (`Console::SizeQuery`).
- **Display console (Phase 4):** its own grid, set when the mode or font changes.
- **SSH (Phase 7):** the client's window size from `pty-req` and `window-change`.
- **Changes:** `Console::sizeGeneration()` counts them; the shell updates `COLUMNS`/`LINES` before each typed line when it changed. `Console::resized()` is raised too, and the loader sends the job it waits for `SIGWINCH` (`programs.md` §15). A serial terminal's size is only learned when the shell (or `term size`) asks, so a resize mid-program reaches it from consoles that know their size (the display).
- **`clear`:** home, erase the screen and the scrollback (`ESC [ H ESC [ 2 J ESC [ 3 J`), to standard output.
- **`reset`:** the terminal's full reset (RIS, `ESC c`: modes, character sets, colors, the screen), the console's modes as a session starts them, and the size asked for again.
- **Tested:** the answer taken out among keys, a byte at a time and in chunks, a lone ESC and broken answers coming through, an answer that never comes (held bytes released), no filtering without a request; `term size` asking and failing; `COLUMNS`/`LINES` following; `clear`, `reset`. On the board: the answer typed into the middle of a command (`ec<answer>ho`), programs seeing 132x40, asking stopping after three unanswered requests, `reset` asking again.

## 3. Design (Phase 4: screen model and renderer)

`TerminalScreen` (pure, host-tested) consumes `VtParser` actions and maintains:

- **Cells:** codepoint (21 bits) + attributes + foreground + background, as 12 bytes per cell in `Psram`. At 1920×1080 with an 8×16 font that is 240×67 cells, about 190 KB, doubled for the alternate screen.
- **Cursor and SGR state:** cursor position, pending-wrap flag, scroll region, the current SGR style, saved cursor, tab stops.
- **Supported sequences** (the xterm subset real programs use):
  - Cursor movement: CUU/CUD/CUF/CUB/CNL/CPL/CHA/CUP/HVP/VPA.
  - Erase and edit: ED/EL/ECH, ICH/DCH/IL/DL, SU/SD.
  - Margins and attributes: DECSTBM, SGR (16/256/truecolor, plus the attributes).
  - Save/restore and modes: DECSC/DECRC, DECTCEM (cursor show/hide), DECAWM (autowrap), alternate screen 1049, bracketed paste 2004.
  - Reports: DSR 5/6 and DA. Replies go back into the console's input.
  - OSC 0/2 (title) is accepted and ignored.
- **Damage tracking:** a dirty-row bitmap. The renderer (2D-DMA/PPA) redraws only damaged rows into the framebuffer.

The serial console doesn't need any of this; it is the HDMI console's job. Keeping `TerminalScreen` pure means it can be tested on the host long before the display works.

## 4. IDF usage

None. Everything here is pure C++ in `bmds_core`/`bmds_console`, except the RX-path Ctrl-C hook in each console driver.

## 5. Resource budget (estimates)

| Item | Cost |
|---|---|
| `Color`/`TextStyle`/`Sgr`/palette | ~2 KB flash (palette is computed, not stored, apart from the 16 base colors) |
| `VtParser` + UTF-8 | ~2.5 KB flash; ~64 B state per instance |
| `displayWidth` table | ~2 KB rodata |
| `ColorFilter` | ~1 KB flash; only instantiated below `TrueColor` |
| `TerminalScreen` (Phase 4) | ~6 KB flash; cells in `Psram` (above) |

## 6. ABI impact

- Programs see **ANSI text on fd 1/2** and query the terminal through syscalls designed in Phase 3. The model:
  - `bmds_isatty(fd)`;
  - `bmds_term_info(fd, &info)` with a `size`-prefixed struct holding columns, rows and color depth;
  - `bmds_term_set_mode(fd, flags)`, which rejects unknown flags.
- The supported escape-sequence subset becomes a compatibility surface once the HDMI console ships. Sequences may be added, never dropped.
- `term` built-in output and arguments are a shell interface.

## 7. Testing

Host tests:

- the `VtParser` state machine against a sequence corpus (split at every byte boundary, malformed input, overlong parameters, UTF-8 edge cases);
- `Sgr` round trips (apply → transition → apply);
- downgrade nearest-match tables;
- `ColorFilter` output;
- `TerminalScreen` against expected cell grids, including the scrolling, wrapping and erase cases from vttest-style scripts.

## 8. Open questions

- ~~Input lost on the USB serial console under two-way traffic~~ (the 65th byte of a typed line, a stalled YMODEM transfer): fixed. ESP-IDF's TX FIFO write on the ESP32-P4 reads the shared data register, which pops an input byte; `devplan/shell.md` has the details.

- Fonts and Unicode coverage for HDMI (Phase 4). Wide characters need a font that has them; fall back to a replacement glyph.
- Line editor UTF-8: done in Phase 1b.
