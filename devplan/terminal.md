# Terminal Model: ANSI/VT and 24-bit Color

Status: **design, not started.** Proposed for Phase 1b (parser, color model, terminal modes) and Phase 4 (screen model and HDMI renderer).

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
- **Ctrl-C** (done in Phase 1b step 2). When `signals` is set, the console's RX path (ISR side) spots `0x03` and raises the console's `Interrupt` instead of queueing the byte. So a running loop or pipeline can be stopped even though nobody is reading input. This resolves the Phase 1 open item.
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

- Fonts and Unicode coverage for HDMI (Phase 4). Wide characters need a font that has them; fall back to a replacement glyph.
- Should the line editor become UTF-8 aware now (cursor movement by codepoint and display width)? Recommended together with `utf8.h`. It is cheap.
