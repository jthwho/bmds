# Scripting: AngelScript, a Runtime with the Whole System, and an IDE

Status: **steps 1-9 built** (2026-10-22): C++ in the SDK; `angel` in the ROM with its core,
the text screen, files and other programs, graphics and input, sound, the network; the patches;
the corpus and the API lock; the editor and the IDE (`editor.md`: `edit`, its AngelScript mode,
`angel --serve` and `--report-fd`); the debugger (`angel --debug`, `editor.md` §17). As built
and measured: §10 to §13, `editor.md` §14 to §16 and §19; the guide's fourteen lessons and the
Serpents and Rooftops games (§14, §15).

## 1. Goal

The user's request (2026-10-21): local programming for children and amateurs, in the spirit of
QBasic and QuickBasic on the PCs of the time. The language should reach the whole system:
keyboard and mouse, network, graphics, sound. The user's choices:

- **A C/C++-like language**, not BASIC.
- **AngelScript** as the language.

What made QBasic work, which this has to keep:

- **One place.** Type the program, press F5, and it runs. Errors point at the line, and help is
  one key away.
- **Statements that do something straight away.** A circle on the screen or a note from the
  speaker takes one line, with no setup code first.
- **No ceiling.** Five lines grow into a game, using the same language.

Separate from the self-hosting C compiler (Phase 8). This is the safe, friendly way in; the C
compiler is for those who outgrow it.

## 2. Decisions (the user's, 2026-10-21)

- The language is AngelScript, not one of our own and not a BASIC.
- It works with the kernel through the public SDK, as an ordinary program. Nothing of
  AngelScript runs in the kernel.
- **The runtime is called `angel`.** It goes in the ROM.
- **Decimal literals default to `float`** (`1.5`), and `1.5d` asks for a `double`. The default
  is a choice made when `angel` is compiled (`ANGEL_FLOAT_LITERALS`), not per script (§4).
- **Top-level statements are allowed.** A script needs no `main()`: the file runs from top to
  bottom. The command-line arguments must be available to it (`args`, §4).

## 3. Facts that shape the design (checked 2026-10-21)

AngelScript 2.38.0 (zlib license) was downloaded and compiled with the SDK's flags. A test
program built with `bmds-cc`, and ran on the board.

- **The language.**
  - C++-like and statically typed, with classes, interfaces, templates (`array<T>`), handles
    (`@`, reference-counted), operators and namespaces.
  - It has no raw pointers and no manual memory management.
  - A script error is a script exception with a line number, never a crash or corrupted memory.
    That is the main reason to prefer it to a C interpreter such as PicoC.
  - Its compiler is in the library: scripts compile to bytecode in milliseconds on the board.
- **It builds with our C++ rules** (`sdk/README.md`: no exceptions, no RTTI, no libstdc++):
  - **The core** (73k lines, `angelscript/source`) uses no STL and no RTTI.
  - **Exceptions:** its `try`/`catch` blocks around calls to registered functions all go with
    `AS_NO_EXCEPTIONS`.
  - **Standard headers:** it includes `<new>` (only for placement new) and `<cstdlib>` (for
    `std::qsort`). `angelscript.h` includes `<type_traits>` for a registration helper,
    `asGetTypeTraits<T>()`.
  - Programs compile with `-nostdinc` (`BmdsProgram.cmake`), so those three headers needed
    shims: 4, 3 and about 25 lines (the last made from GCC's built-ins).
  - **Runtime support:** `operator new`/`delete` and `__cxa_pure_virtual` (one small file).
  - With those, all of the core compiled with no errors. The only warnings were 4 ignored
    `stdcall` attributes, and 129 strict-aliasing warnings at `-Wall` in AngelScript's own code.
    Our build of it will turn those off for that library only.
  - The program linked with no missing symbols and ran.
- **The calling convention.**
  - Native calls into C++ need assembly per platform. AngelScript's RISC-V support is 64-bit
    only (`AS_RISCV64`, `as_callfunc_riscv64.cpp`).
  - On RV32, `as_config.h` falls back to `AS_MAX_PORTABILITY` by itself. Every registered
    function then uses the generic convention (`asCALL_GENERIC`, arguments read from an
    `asIScriptGeneric`).
  - The `autowrapper` add-on makes those wrappers from templates, and has no STL dependency.
- **The add-ons.** Those we would want all use the STL, so we replace them with our own:
  - `scriptstdstring` uses `std::string`;
  - `scriptarray` uses `<algorithm>` and `<string>`;
  - `scriptdictionary` uses `std::map` and `std::unordered_map`;
  - `scriptbuilder`, `debugger` and `contextmgr` use `std::string`, `std::vector` and maps;
  - `datetime` uses `<chrono>`.

  `scriptany`, `scriptgrid`, `scripthandle`, `weakref` and `autowrapper` need no STL. Our own
  `string` is wanted anyway, for UTF-8 and our allocator.
- **Measured on the board** (the program at `-O2`, run from PSRAM as every program is,
  `programs.md` §5):

  | | |
  |---|---|
  | binary, stripped (core, musl, test) | 786 KB text at `-O2`; 651 KB at `-Os` |
  | the core alone | 724 KB of text at `-O2`; 583 KB at `-Os` |
  | engine created, small script compiled | 10–15 ms |
  | the engine's heap after compiling (and its peak) | 28 KB (33 KB) |
  | `for` loop, 1,000,000 times `s += i & 7` | 562 ms; 415 ms without line cues |
  | `fib(24)` (75,000 script calls) | 180 ms, about 2.4 µs a call |
  | 200,000 times `x = x * 0.999 + 1.5`, `double` | 312 ms |
  | the same in `float` (`0.999f`, `1.5f`) | 70 ms |

  - That is about 2.4 million simple statements a second. A game written in script is fine
    when the work per pixel is done in native code behind the API (blits, fills, mixing,
    synthesis). Pixel-by-pixel loops in script are slow.
  - **`double` costs 4.5 times `float`:** the P4's FPU is single precision (`ilp32f`), so
    `double` is done in software. AngelScript's unsuffixed literals (`1.5`) are `double` (§9).
  - **Line cues** (one `SUSPEND` instruction per statement) cost about 25% in tight loops. They
    are what line callbacks, breakpoints and `Abort()` need.
- **What the system already gives a program**, all through the SDK, so nothing here needs a new
  system call to begin with:
  - surfaces (windowed or full screen) and `libbmdsgame`: canvases, blits (scaled, rotated,
    keyed), text, the frame loop, the 2D-DMA;
  - keyboard and pointer events;
  - mixed sound effects and music, files and MIDI out (`libbmdsgame`), the synth
    (`bmds_audio_synth`), speech (`bmds_say`);
  - BSD sockets through musl, files and directories (network mounts included), settings
    (`bmds_sysconf_*`), notifications, processes, images (`bmds_image_decode`).
- **Float literals** (`as_tokenizer.cpp`, `as_compiler.cpp`):
  - The tokenizer makes an unsuffixed decimal literal a `ttDoubleConstant`, and one ending in
    `f` a `ttFloatConstant`.
  - AngelScript's own `AS_USE_DOUBLE_AS_FLOAT` isn't what we want: it makes `double` another
    name for `float`, so there would be no `double` at all.
  - **A 20-line patch does what we want, as a compile-time define**, `AS_FLOAT_LITERALS`, which
    I prototyped on the PC. Unsuffixed literals become `float`, and a `d` or `D` suffix makes a
    `double`; two assertions in the compiler about the suffix's length are adjusted. Built with
    it, `auto a = 1.5` is `float`, `1.5d` and `2e3D` are `double`, and `1.5f` is still
    `float`. Without the define, nothing changes (a `d` suffix is a syntax error, as upstream).
- **Top-level statements** (`as_parser.cpp`, `as_builder.cpp`, `as_compiler.cpp`):
  - The parser's top-level loop (`ParseScript`) accepts only declarations; anything else is
    "unexpected token".
  - `asCCompiler::CompileFunction` already compiles a bare statement block as a function body
    (it is how `ExecuteString` runs a line).
  - `asCBuilder::RegisterScriptFunction` takes its name, return type and parameters as
    arguments, not from the source.
  - Every global variable with an initializer gets an init function of its own
    (`asCGlobalProperty::GetInitFunc()`). The module runs them all before anything else
    (`asCModule::ResetGlobalVars`).

  So a function made of the top-level statements fits in with what is there. What needs care
  is the order of global initialization (§4).
- **`#!` works** (`programs.md` D7), so a script with `#!/rom/bin/angel` at the top runs by name
  from the shell.
- **The GUI toolkit** (`gui.md`, `bmds_gui`) is kernel-side C++ for built-in applications, the
  media player and the system monitor among them. It has a `LineEdit` but no multi-line text
  editor. Phase 8's "on-device text editor" is not built.

## 4. The design

### Where things run

| Piece | Where | What it is |
|---|---|---|
| `angel` | a program (`/rom/bin`, §6) | the runtime: AngelScript, our types, the system API; runs a script, checks one, serves the IDE |
| the IDE (working name *Workshop*) | a built-in application (toolkit) | editor, Run and Stop, errors in place, F1 help, examples |
| `CodeEdit` | the toolkit | a multi-line editor widget: the IDE's, and the system's text editor |

- **A script runs in its own process.**
  - Its memory goes when it ends, and Stop is a signal.
  - A runaway loop can't hang the IDE, and no part of AngelScript is in the kernel.
  - Because Stop doesn't need the VM's cooperation, scripts run **without line cues** (25%
    faster). Cues are turned on only under the debugger (step 8).
- **The IDE never links AngelScript.**
  - While it is open, it keeps one `angel --serve` process. Over a pipe it asks for checks
    (compile and report diagnostics, about every half second of idle typing), completions and
    help text.
  - Run starts a separate `angel` process for the script.

### The runtime: `angel`

- **Usage:**
  - `angel file.as [args...]` runs the script.
  - `angel -c file.as` checks it: diagnostics only, exit status 1 if any.
  - `angel --serve` serves the IDE.
  - `angel --api` prints every registered declaration and its help (the lock test and the
    documentation use it).
- **A script runs from top to bottom** (§4, top-level statements). A script made only of
  declarations, with a `void main()` or `int main()`, runs that instead, so C-style programs work
  too. Having both is an error.
- **The arguments:** `args`, a global `const array<string>`. `args[0]` is the script's path as
  given, then the arguments, as in C's `argv`.
- **The exit status:**
  - `exit(status)` ends the script from anywhere;
  - falling off the end is 0, or `main`'s return value;
  - an uncaught script exception is 1, and its message, file, line and call stack go to stderr.
- **The patches to AngelScript** (AngelScript is vendored, `thirdparty/angelscript`; the
  patches are applied there and kept in its `patches/`). Each is small, sits behind a define of
  its own, and is written to be offered upstream:
  1. **`AS_FLOAT_LITERALS`** (§3). `angel`'s build option `ANGEL_FLOAT_LITERALS` (`float`, the
     default, or `double`) sets it or not. The IDE's `--serve` is the same program, so its
     diagnostics always agree with how scripts run. As built it also settles overloads the way
     float literals need (§10): an integer given to `f(float)` and `f(double)` goes to `float`
     (otherwise `sqrt(2)` is ambiguous), and an overload that would drop a real argument's
     fraction is the last chosen (otherwise `clamp(x, 0, 1)` with a float `x` is the `int` one).
  2. **`AS_TOP_LEVEL_STATEMENTS`:**
     - **Parser:** in the main file, a top-level statement is a statement, not an error.
       - Statements are told from declarations as the parser already does it: `IsVarDecl()`,
         `IsFuncDecl()`, then the other declaration keywords. `x = 5;` and `print("hi");` are
         statements; `int x = 5;` and `Point p(1, 2);` are declarations.
       - The statements go into one statement-block node, in order.
       - In an `#include`d file, statements are an error ("Statements outside functions are
         allowed only in the script itself, not in a file it includes"), so libraries can't run
         code by being included.
       - A malformed function declaration (`int add(int a, int b {`) still gets the
         declaration's errors, not a statement's.
     - **Builder:** that block becomes a function of the module, registered through
       `RegisterScriptFunction` under a name scripts can't write. `angel` runs it.
     - **Globals:** a global declared among top-level statements is initialized **where it
       stands**, not before the script starts. Otherwise `print("Name?"); string name =
       input();` would ask for input before printing the question.
       - Such a global's init function isn't run by `ResetGlobalVars`.
       - The statement block instead holds a node that calls that init function at the
         declaration's place.
       - Until then a function called earlier finds numbers 0 and objects not made yet (a null
         pointer; `angel` adds a note naming the variable that has no value yet), much as
         Python's names don't exist before their assignment runs.
       - `const` globals initialized with constants are still initialized first, so they can
         be used anywhere, in array sizes and default arguments too.
     - About 200 lines over the parser, builder, compiler and module, plus tests: of the patches,
       the one that needs the most care.
- **The builder** is ours, not the STL add-on. It strips a leading `#!` line and handles
  `#include "other.as"`, relative to the including file, once each. Diagnostics are reported as
  `file:line:col: error: message`, the form compilers use, so the IDE, `vi` and editors on a PC
  can all read them.
- **The engine's settings are part of the language** and are fixed once released. Each is set
  explicitly, never left to the library's defaults, which may change between versions:
  - all 39 are in a table in `main.cpp`. Those that differ from AngelScript's defaults:
  - `asEP_USE_CHARACTER_LITERALS` on (`'a'` is a number);
  - `asEP_ALLOW_MULTILINE_STRINGS` on;
  - `asEP_BUILD_WITHOUT_LINE_CUES` on, as above;
  - `asEP_DISALLOW_EMPTY_LIST_ELEMENTS` on (`{1, , 3}` is an error, not a default element);
  - `asEP_MAX_STACK_SIZE` 1 MB and `asEP_MAX_CALL_STACK_SIZE` 50,000 calls, so deep recursion
    gives a clean "stack overflow" error;
  - `asEP_NO_DEBUG_OUTPUT` on.
  - `asEP_REQUIRE_ENUM_SCOPE` stays off, as planned on: C++'s rule for plain enums, so
    `Console::Yellow` works as well as `Console::Color::Yellow`.
- **Memory:** `asSetGlobalMemoryFunctions` points at musl's `malloc` (PSRAM). Scripts' own
  objects come from the same place.
- **Threads:** a script has one thread. Waiting things (frames, keys, sockets, timers) are
  calls that block, or callbacks from the frame loop (below). Threads aren't something to teach
  first, and scripts don't need them for any of this.

### The system API (what scripts see)

Organized as a few namespaces and classes, each one small. Every registration is a row in one
table in the runtime's source, holding the declaration, the wrapper and a line of help. The
lock test, `--api`, the IDE's completions and F1, and `docs/script-api.md` all come from that
table, so they can't disagree.

- **Core** (built, step 2):
  - `print`, `println` (any value: numbers, strings, arrays, enums by name, objects by their
    `string toString()`) and `input()`, `input(prompt)`;
  - `string` (bytes, UTF-8 as it comes: `length`, `substr`, `find`, `findLast`, `contains`,
    `startsWith`, `endsWith`, `replace`, `toUpper`, `toLower`, `trim`, `split`, `insert`,
    `erase`, `+` with numbers); `join`, `parseInt`, `parseFloat`, `parseDouble`,
    `formatFloat(value, decimals)`, `chr`, `ord`;
  - `array<T>` (AngelScript's add-on, sorts made O(n log n)), and later `dictionary`;
  - math as global functions, C's names, `float` and `double` overloads (`sqrt`, `sin`... `pow`,
    `abs`, `min`, `max`, `clamp`, `floor`, `round`), `PI`; `random(low, high)`, `random()`,
    `randomSeed`;
  - `millis()`, `sleep(ms)`;
  - `args`, `env(name)`, `exit(status)`, `assert(condition, message)`, `throw(message)` and
    `getExceptionInfo()` for `try`/`catch`.
- **Text screen** (`Console`, built, step 3): `clear()`, `at(x, y)`, `color(foreground,
  background)` in QBasic's sixteen colours (`Console::Yellow`), `reset()`, `cursor(visible)`,
  `key()` (waits: a character, or a name such as "Up", "Enter", "F1", "Ctrl+A"), `keyNow()`
  (doesn't wait, like `INKEY$`), `width()`, `height()`. VT sequences, and raw mode on the
  terminal the script runs in while keys are read (`input()` and the script's end restore it).
  QBasic's text-mode games (`LOCATE`, `COLOR`, `INKEY$`) translate to these directly.
- **Graphics** (built, step 4): `Window` and `Image`, drawn on with the same methods; there is
  no separate `Canvas` type (as built: one fewer thing to learn).
  - **Immediate (QBasic's way):** `Window w("Title", 640, 480);` then `w.line(...)`,
    `w.fillCircle(...)`, `w.text(...)`, `w.image(...)` appear as they are drawn. Once a script
    calls `w.frame()` (once round an animation's loop) the window is shown only by `frame()`
    and while the script waits for input, so a frame is never shown half drawn; one that stops
    calling it for a tenth of a second is shown as it draws again.
  - **Game:** a script class implementing the interface `Game` with any of `void tick()`,
    `void draw()`, `void event(Event@)` runs in `w.run(game, ticksPerSecond)`: events, ticks at
    the rate (catching up five at most), a draw each frame, until the window closes or
    `w.stop()`. As built it is a method of the window, not `run(game, width, height, ...)`.
  - **Drawing:** `clear`, `plot`, `pixel` (the colour read back, exactly: pictures are 24-bit),
    `line`, `rect`, `fillRect` (solid, or with an opacity), `circle`, `fillCircle`, `ellipse`,
    `fillEllipse`, `polygon` and `fillPolygon` (`{x1, y1, x2, y2, ...}`), `floodFill` (PAINT),
    `text` (any value, 8 × 16 letters times a scale), `image` (at a place, or stretched),
    `imageRotated` (degrees and a scale), `clip`, `save` (PNG).
  - **Colours** are `uint` 0xRRGGBB: `Color::Red` and 17 more, `rgb(r, g, b)`, `hsv(hue,
    saturation, value)`, `mix(a, b, amount)`; `textWidth`, `textHeight`.
  - **`Image`:** `Image(width, height)` blank, `Image(path)` from PNG, JPEG or BMP (transparent
    where the file is), `part(x, y, w, h)` (a sprite from a sheet), `transparent(color)`,
    `opaque()`.
  - New in `libbmdsgame` (`shapes.c`): ellipses outlined and filled, filled polygons, flood
    fill, reading pixels back.
- **Input** (built, step 4): `w.key()` and `w.keyNow()` (as `Console`'s: the character typed,
  or a name), `w.keyDown(name)` (held now: "a", "Space", "Left", "Shift"...), `mouseX()`,
  `mouseY()`, `mouseDown(button)`, `mouseWheel()`, and `Event` (`waitEvent()`, `nextEvent()`):
  `type` (`KeyDown`, `KeyUp`, `MouseMove`, `MouseDown`, `MouseUp`, `Wheel`, `Close`), `key`,
  `text`, `x`, `y`, `button`, `wheel`. `w.closed()` says the user closed it; a script that never
  asks (nor reads events, nor runs a game) ends when its window is closed. Gamepads come later,
  through controls (`controls.md`), so a script names actions rather than buttons.
- **Sound** (built, step 5):
  - `Sound s("boom.wav"); s.play(volume, pan)`, `s.loop()`, `s.stop()`, `s.playing()`,
    `s.length()`: effects loaded whole (WAV, MP3, Ogg Vorbis, FLAC) and mixed by libbmdsgame;
  - `playMusic(path, loop, volume)` (MIDI through the synth, or audio files), `stopMusic()`,
    `musicVolume()`: global functions, not `Music::play` (the same as the rest of the API);
  - `tone(hz, ms, volume)` (a square wave, as the PC speaker's; waits for it, as QBasic's
    SOUND did) and `beep()` (800 Hz for a quarter second, as QBasic's BEEP);
  - `play("T120 O3 L8 CDEFGAB>C")`: QBasic's `PLAY` language (notes A-G with `#`, `+`, `-`,
    lengths and dots, `O`, `<`, `>`, `N`, `L`, `T`, `P`, `MN`, `ML`, `MS`, `MF`, `MB`; `R` for a
    pause as well), plus `I` (a General MIDI instrument; the start: 80, the square lead, the PC
    speaker's sound) and `V` (volume). Octave 3 starts at middle C, as in GW-BASIC; the settings
    carry over from one `play()` to the next. The notes go to `/dev/midi` with their times
    (the kernel sends them then), and `play()` waits until they have played, or with `MB`
    returns at once, the next `play()` starting after them; the script's end waits for them;
  - `say("Hello")` (speech, `speech.md`). Children like a computer that talks back.
  - live notes (the user's ask, 2026-10-22): `noteOn(note, volume)` and `noteOff(note)` (MIDI
    numbers, middle C 60), `instrument(program)`, `allNotesOff()`, sent at once on a channel of
    their own (2; `play()`'s music is on 1), so a keyboard's note sounds from its key down to
    its key up; `Event.repeat` tells a held key's repeats from its press.
- **Files** (built, step 3):
  - `File(path, mode)` ("r", "w", "a"): `readLine`, `readAll`, `eof`, `write` and `writeLine`
    (any value, as `print`), `close`, `isOpen`;
  - global functions: `fileExists`, `isDirectory`, `readFile`, `writeFile`, `appendFile`,
    `removeFile`, `renameFile`, `listDirectory`, `makeDirectory`, `currentDirectory`,
    `changeDirectory`;
  - what fails raises an exception that says why, which `try`/`catch` can handle;
  - paths are the system's, so network mounts work as well.
- **Network** (built, step 6; IPv4):
  - `Socket(host, port)` connects (TCP); it reads and writes as a `File` does (`read(most)`,
    `readLine`, `readAll`, `eof`, `write` and `writeLine` of any value), and `ready(ms)` says
    whether something can be read, so a loop can do other things meanwhile; `address()` and
    `port()` are the other end's;
  - `ServerSocket(port)` listens: `accept()`, `ready(ms)`, `port()` (port 0: any free one);
  - `UdpSocket(port)`: `sendTo(host, port, data)` (broadcast allowed), `receive()`, then
    `sender()` and `senderPort()`;
  - all three: `setTimeout(ms)` (0, the start: no limit; then an exception says it timed out),
    `close()`, `isOpen()`;
  - `localAddress()` (this computer's), `lookup(host)`;
  - `Http::get(url)` and `Http::post(url, body, type)` return the body (an exception for a
    status that isn't 2xx); `Http::request(method, url, body, headers)` returns an
    `HttpResponse` (`status`, `reason`, `body`, `url`, `header(name)`, `headers()`) whatever the
    status; `Http::encode(text)`. Plain HTTP/1.1 in the runtime, over the same connections;
    redirects followed. HTTPS waits on a TLS client (§9).
  - Names changed from the plan's (`send`, `receive`, `receiveLine`): a socket reads and writes
    with the names a `File` uses, so what a child learned for files works here.
- **System:**
  - built (step 3): `run(command)` (its output on the screen; the exit status) and
    `output(command)` (what it printed);
  - built (step 4): `setting(key)` (a setting's value, read only) and `notify(title, text)`.

### The IDE (working name *Workshop*; now `editor.md`'s `edit`)

Superseded in detail by `editor.md` (2026-10-22): the IDE is the AngelScript mode of the system's
one text editor. What follows is the IDE's list as first planned, kept for its intent.


- **The editor** is `CodeEdit`:
  - line numbers, syntax colouring, matching brackets, auto-indent;
  - undo and redo, selection, the clipboard (`clipboard.md`), find and replace;
  - diagnostics shown as wavy underlines and a gutter mark, with the message on hover and in a
    list.
- **Running:**
  - **F5** saves and runs. The script's text output goes to a terminal window the IDE opens
    for it (a pty, `pty.md`), reused from run to run. Its graphics windows are its own surfaces.
  - **Shift+F5** stops it (SIGTERM, then SIGKILL).
  - An uncaught exception's line is shown in the editor.
- **Help:** F1 on a name shows its declaration and help line from `angel --serve`, and
  completion offers the same list. A Help menu leads to the guide and the examples.
- **Files:**
  - New starts from an empty script with a comment saying how to run it (F5); templates for a
    game or a text program are offered beside it.
  - Scripts live in the home (`~/scripts` by default; a setting).
  - `.as` is a file type (`filetypes.md`): double-clicking a script opens it in the IDE, and
    running it from a terminal runs it.
- **Keyboard first** (`controls.md`): every action has a binding, declared in the IDE's
  `controls.conf`.

## 5. Steps

1. **C++ in the SDK** (done 2026-10-21).
   - `sdk/include/c++/`: freestanding `<new>`, `<type_traits>` (from GCC's built-ins),
     `<cstdlib>`, `<cstring>`, `<cmath>` and `<utility>`, only as much as programs need.
   - `crt/cxx.c`: `operator new`/`delete` (all forms), `__cxa_pure_virtual`,
     `__cxa_guard_*` (static locals; as built a compare-and-swap with `sched_yield`, the rare
     wait being short) and `__dso_handle` (`__cxa_atexit` is musl's).
   - `bmds_add_program` with C++ sources, a `bmds-c++` beside `bmds-cc`, and `$CXX` in
     `bmds-dist`.
   - A C++ SDK example, and `sdk/README.md` updated.
2. **The runtime's core** (done 2026-10-21).
   - As built, AngelScript is vendored (`thirdparty/angelscript`, like SVOX Pico), not fetched
     by `dist/`: the host tests need it too, and the patches are then plain to see.
   - The two patches, `AS_FLOAT_LITERALS` and `AS_TOP_LEVEL_STATEMENTS`, each with host tests
     of its own (literals' types, the order of initialization, statements in an included file
     refused, `main` and top-level statements together refused).
   - `programs/angel`:
     - the builder, diagnostics, top-level statements or `main`, `args`, `exit`, exceptions;
     - our `string` and `array<T>` and the registration table;
     - `Math`, `Time`, `print`/`input`, `args`;
     - `-c` and `--api`.
   - The engine settings of §4.
   - Host tests run the runtime on the PC over a corpus of scripts with expected output and
     diagnostics.
   - The lock test on `--api`'s output.
3. **Text screen and system** (done 2026-10-21): `Console`, `File` and the file functions,
   `run` and `output`, and the first examples (`/rom/share/angel/examples`: a number-guessing
   game, a text adventure).
4. **Graphics and input** (done 2026-10-21): `Window`, `Image`, colours, `Event`, `Game` and
   `run()`; `setting()` and `notify()`; ellipses, polygons and flood fill in `libbmdsgame`.
   Examples: a bouncing ball, a paint program, a snake game.
5. **Sound** (done 2026-10-21): `Sound`, `playMusic`, `tone`, `beep`, `play` (the music
   language, on the synth), `say`. A piano example; snake's sounds.
6. **Network** (done 2026-10-22): `Socket`, `ServerSocket`, `UdpSocket`, `Http`. Examples: a
   chat between two computers, fetching a page.
7. **The editor and the IDE** (done 2026-10-22, `editor.md`, the user's direction): one
   application for all text (`edit`), on reusable widgets (`TextEdit`, then `CodeEdit` on top),
   with syntaxes for many languages; the angel IDE is its AngelScript mode (`angel --serve`, Run
   and Stop, diagnostics, F1 and completion, templates, `text/x-angelscript`), C and C++ later.
8. **The debugger** (done 2026-10-22, `editor.md` §17 and §19): breakpoints, stepping, the call
   stack and watching variables, through a line callback in `angel` with line cues on
   (`--debug`). It is controlled by the IDE over a pipe, its events on `--report-fd`'s.
9. **The guide and examples** (done 2026-10-22, §14 and §15): a tutorial in steps, from `println("Hello")` to a game, and
   games of our own in the spirit of Nibbles and Gorillas, newly written. QBasic's own are
   Microsoft's.

Each step finishes with its measurements and examples, and gets a row in the roadmap.

## 6. Resource budget

- **Flash:** about 0.7–0.8 MB for `angel` (the core is 583–724 KB, plus our types and
  bindings).
  - In the ROM (10 MB, `rom.md` §3a; the user's decision) it sits beside BusyBox and Dropbear,
    and is there on every board without a card, as QBasic came with DOS.
  - Which optimization level it ships at (`-O2` is about 140 KB larger than `-Os`) is decided
    by measuring both in step 2.
- **PSRAM:**
  - per running script: the image (as above) plus the engine (about 30 KB) plus the script's
    bytecode and objects;
  - a window's buffers as any surface's;
  - the IDE's `--serve` process: one more image and engine while the IDE is open.
- **Internal RAM:** none of its own. Scripts' images run from PSRAM through the cache like
  every program.
- **CPU:** none while nothing runs. The VM's dispatch is the hot path. If it matters, measure
  whether the loader could put a program's marked hot text in internal RAM (a loader feature
  of its own, not part of this plan).

## 7. ABI impact

Scripts that children and amateurs save have to keep running, so all of the following becomes
userspace ABI, frozen once released (`docs/ABI.md` gets a section for it):

- **The script API:** every registered type, function, method, property and constant,
  append-only. `--api`'s output is the lock (`tests/scripts/api.lock`), and the lock is never
  edited to make it pass.
- **The language:** AngelScript's version, the engine settings, and our two patches' meaning
  (§4). Float literals as `float` and top-level statements are part of the language scripts are
  written in. `ANGEL_FLOAT_LITERALS` is chosen once, before the first release, and the shipped
  `angel` never changes it.
  - An upgrade of AngelScript is an ABI change. It is taken only when the whole corpus of
    scripts, examples and tests included, still compiles and behaves the same.
  - The release is pinned by its `sha256`.
- **`angel`'s command line and exit statuses**, the diagnostic format, and the `--serve`
  protocol (the IDE is built in, but `vi` integrations or other editors may use it).
- **Not ABI:** bytecode. Scripts are kept as source and compiled each run (milliseconds), so the
  bytecode format never needs freezing.

The C SDK itself only grows (step 1's C++ support adds headers and runtime routines, no system
calls). New system calls, if a binding needs one, follow the usual rules.

## 8. Testing

- **Host:**
  - the runtime built for the PC (AngelScript with `AS_MAX_PORTABILITY` there as well, so the
    bindings are the same code), with the system calls behind a thin layer;
  - a corpus of scripts in `tests/scripts/` with expected output, exit status and diagnostics
    (errors included, each at its line);
  - `string`, `array` and the builder (`#!`, `#include` cycles) tested directly;
  - the API lock.
- **Board:**
  - `progtest`-style scripts for the window, sound, files and sockets;
  - the examples run as smoke tests;
  - the measurements of §3 repeated for each step.
- **The IDE:** driven through `win hid` like the other applications (a modifier released at the
  end).

## 9. Open questions

- **The IDE's name.** *Workshop* is a working name. `.as` for scripts is AngelScript's usual
  extension.
- **`double` constants written without `d`.** With float literals, `double x = 0.1;` stores
  the `float` 0.1 widened (0.100000001490116...). One possible fix: the compiler warns when an
  unsuffixed literal meets a `double` and loses digits ("write 0.1d"), which would need the
  literal's text kept until it is converted. Decide with the patch, in step 2.
- **HTTPS.** It needs a TLS client usable from programs (`bmds_crypto` has the primitives). It
  can be added later without changing `Http`'s API.
- **The native RV32 calling convention** (`as_callfunc_riscv32`, a small piece of assembly).
  Worth it only if measurements show the generic wrappers matter. The API's bulk calls should
  make that unlikely.
- **Offering the patches upstream.** Both are behind defines and change nothing without them.
  If AngelScript takes them, we carry nothing.

## 10. As built (steps 1-3, 2026-10-21)

- **Where things are:**
  - `sdk/include/c++/`, `sdk/crt/cxx.c`, `bmds-c++`, `sdk/examples/cxxdemo` (step 1);
  - `thirdparty/angelscript` (2.38.0 and the three patches, `README.md`);
  - `programs/angel` (`main.cpp`: loading, building, running, errors; `api.cpp`: registration
    and `--api`; `string.cpp`, `core.cpp`, `console.cpp`, `files.cpp`, `system.cpp`;
    `examples/`); `dist/angel` builds it (a Makefile, so the build wrapper runs it in
    parallel); `components/bmds_storage` puts it in `/rom/bin` with the examples and
    AngelScript's license;
  - `tests/scripts`: the corpus (`run.py`, 20 scripts with their expected output, errors and
    exit status) and the API lock (`api.lock`, 190 declarations; `apilock.py`), both in the
    host tests' `ctest`.
- **What the corpus caught or settled:**
  - `sqrt(2)` was ambiguous between `float` and `double` once literals were `float`, and
    `clamp(-0.5, 0, 1)` chose the `int` overload (one conversion that drops the fraction cost
    less than two that don't). Both settled in the float-literals patch: integer to `double`
    costs one more than to `float`, and real to integer four times what it did.
  - A global the script hasn't reached is a null pointer to a function that uses it: kept (it
    is the order the script says), with a note naming the variable.
  - AngelScript's `IsFuncDecl()` doesn't always rewind when it fails (when it reaches the end of
    the file); the patch rewinds after it.
  - The `args` array, an application object of a type the script uses, made AngelScript warn
    that the module couldn't be freed: the module is now discarded before it is released.
- **Measured on the board** (`angel` from the ROM, `-O2`):

  | | |
  |---|---|
  | `angel`, stripped | 899 KB of text (AngelScript about 724 KB, our runtime and musl the rest) |
  | `/rom` image | 5.85 MB of 10 MB |
  | memory of a running script (`ps`) | about 190 KB: the program runs in place from the ROM |
  | start, compile and run a one-line script (`time`) | 50 ms |
  | 1,000,000 times `s += i & 7` (a global `s`) | 502 ms |
  | `fib(24)`, 75,000 calls | 172 ms |
  | 200,000 float multiply-adds | 87 ms |

  Built on the PC: `angel` takes a minute with the build wrapper's jobs; nested in the
  firmware's build (one job) about three and a half, only when its sources change.
- **The C++ example** costs about 1 KB over `hello` (the operators and guards).

## 11. As built (step 4, 2026-10-21)

- **A window draws straight into its surface's buffers** (three, 24-bit). One is held to draw
  on; a thread of `angel`'s presents it and takes the next, and reads the window's events into a
  queue (128). The next buffer is brought up to date from the newest shown by the 2D-DMA the
  first time it is drawn on, less what that drawing paints whole, so `clear()` brings nothing
  over. Solid fills (`clear`, `fillRect`) are the 2D-DMA's too; the rest is libbmdsgame's
  drawing on the CPU. On the host a window is only its picture, so the corpus draws and reads
  pixels back (`graphics.as`, `graphicserrors.as`).
- **What changed on the way, measured on the board** (a 640 × 480 window):

  | | full redraw a frame | a ball moved a frame |
  |---|---|---|
  | first: the script's own picture, copied to the buffers by the CPU | 12 frames/s | 59 |
  | drawing in the buffers, fills and catch-up by the 2D-DMA | 45 | 60 |
  | and shown only at `frame()` once the script calls it | 59 | 60 |

  The CPU moves PSRAM at about 28 MB/s each way (`docs/platforms/esp32p4.md`), so the first
  design spent 27 ms clearing the script's picture and as long again copying it. Showing a
  buffer whenever something was drawn also showed frames half drawn: the snake's body was
  missing from a screenshot (its head drawn, then the frame shown before the body).
- **AngelScript gives an application function the reference of a handle argument** (`Image@`,
  `Game@`): it must be released. Not doing so leaked every image drawn and the game passed to
  `run()` (the module warned that it couldn't be freed).
- **Tried on the board** (`win hid`, `win key`, `win pointer`, `win close`, `screenshot`): keys
  with their names, typed text, `keyDown`, mouse motion, buttons and the wheel, Close; `run()`
  at 60 ticks a second (120 in 2.0 s); snake played; paint strokes in two colours; QBasic-style
  drawing shown without `frame()`, then `key()`; images loaded, cut, rotated and saved;
  `setting("clock/zone")`; an exception in `tick()` reported with its call stack.
- **The window stays when the script ends**, until it is closed (the picture is there to be
  seen); `exit()` closes it at once.

## 12. As built (step 5, 2026-10-21)

- **`sound.cpp`:** the PLAY parser is portable (the host corpus checks what it refuses,
  `sound.as`); on BMDS the notes are written to `/dev/midi` as timed events
  (`BMDS_MIDI_EVENTS`, a program change where the instrument changes) and the call sleeps
  until the last has played. The kernel holds 512 events an fd, so a long string's writes wait
  for room. A string is checked whole before any of it is sent: a bad one plays nothing.
- **The mixer** (libbmdsgame's, a real-time client of `/dev/dsp`) opens at the first
  `Sound`, `tone`, `beep` or `playMusic`. A tone is a square wave made for it (a quarter of full
  scale at volume 1, its ends faded over 3 ms against clicks), played as a sound, and freed
  once the mixer has finished with it.
- **Timed on the board:** four quarter notes at T120 in 2.02 s; `MB` returned in 1 ms and the
  next foreground `play()` ended 2.02 s after it began (five eighths and three); `tone(440,
  500)` and `beep()` 0.80 s; a quack's length 0.24 s, playing during it and not after;
  looping and stopping; a MIDI file as music with its volume changed; `say()` 1.5 s. Heard by
  nobody yet (no one at the board): the user's ears are the test that remains.
- **The user's report: the piano flickered** on every key and mouse move. A window paced by
  `frame()` was shown anyway once 0.1 s had passed since the last `frame()`; the piano waits
  longer than that for each event, so its next redraw's `clear()` was shown alone. The time now
  runs from the first drawing not yet shown. The piano also takes the mouse (a click, or a drag
  across the keys) and redraws only when what it shows changes.
- **The user's ask: note on and note off, separately** (`play()` sends whole notes with their
  lengths). Added `noteOn`, `noteOff`, `instrument`, `allNotesOff` and `Event.repeat` (a held
  key's `KeyDown` events repeat; the piano starts a note only on the first). The piano holds a
  note from key down to key up, plays chords, and the mouse holds one while its button is down.
  Watched with `midi monitor midi0`: note on on channel 2 at the press, note off 301 ms later
  at the release; `play()`'s notes on channel 1 beside them.

## 13. As built (step 6, 2026-10-22)

- **`network.cpp`:** POSIX sockets, the same code on the board and the PC, so the host corpus
  runs it over loopback: `network.as` (a server and its client in one script, lines, partial
  reads, `ready()`, timeouts, the end of a connection, datagrams, a refused connection) and
  `http.as` against a small HTTP server `run.py` starts on loopback (`$TEST_HTTP`: a plain page,
  a chunked one, one ended by closing, redirects (301, 303 after a POST, a loop), a missing
  page, an echo of the method, headers and body). `network.as` gives the same output on the
  board.
- **Waiting** is `poll()` with the socket's timeout, so a script waiting for data or a
  connection costs nothing. `Socket(host, port)` connects without blocking and waits at most
  10 s (a dead address fails in 10 s, not after TCP's minutes of retries); `Http`'s waits are
  30 s each. A socket's input is buffered (2 KB) for `readLine()`; a `UdpSocket`'s 64 KB
  buffer is made at its first `receive()`. Writes use `MSG_NOSIGNAL`: a closed connection is an
  exception, not SIGPIPE.
- **HTTP/1.1** with `Connection: close`: one connection a request, the body by
  `Content-Length`, chunks, or to the end; 1xx answers skipped; up to 5 redirects (303, and
  301/302 after a POST, become a GET without the body, as browsers do); an `https://` address is
  an exception saying HTTPS isn't supported yet.
- **On the board** (`fetch.as` and httpbin.org): example.com in 60 ms; redirects, a chunked
  answer, a form posted, 100 KB fetched, a 404 as an exception, a bad host ("Name does not
  resolve") and a dead address (timed out in 10 s). `chat.as` between the board (waiting) and
  the PC (joining), each in a pty: names exchanged, lines both ways, UTF-8, Backspace over an
  `é`, Escape leaving and the other side told.
- **Size:** `network.cpp` is 19 KB of text, musl's resolver (`getaddrinfo` and DNS) about
  10 KB more; `angel` is now 1,002 KB of text.


## 14. Step 9: the guide and the examples (designed 2026-10-22)

**The guide is lessons you run.**
- **Form:** each lesson is a script in `/rom/share/angel/guide`. Its comments teach, its code is
  the example, and it ends with "Try this" changes for the reader to make.
- **Reading and running:** the reader opens a lesson in `edit` and presses F5. It runs where it
  is, unchanged; once changed, Save As keeps the reader's copy in `~/scripts`.
- **Why not a separate help viewer:** the text sits next to the code it explains, and the
  editor, F1 and the debugger work on it.
- **Order:** the lessons go in steps, from `println("Hello")` to a game. Each uses only what the
  ones before it showed.
  1. Hello: `println`, `print`, comments, strings.
  2. Numbers: variables, `int` and `float`, arithmetic, `%`, the maths functions.
  3. Asking: `input`, `parseInt`, `if` and `else`, comparisons.
  4. Loops: `while`, `for`, `break`, a times table, `sleep`.
  5. A guessing game: `random`, putting the first four together.
  6. Functions: parameters, return values, using one many times.
  7. Arrays: lists, `insertLast`, `length`, `foreach`, sorting.
  8. Text: string methods (`split`, `find`, `replace`, `toUpper`), the text screen's colours
     and places (`Console`).
  9. Classes: fields, methods, constructors, a list of objects.
  10. Drawing: `Window`, shapes, colours (`rgb`, `hsv`), text.
  11. Animation: a loop with `frame()`, `keyDown`, moving a player, closing the window.
  12. A game: `Game` with `tick()`, `draw()` and `event()`; a score, lives, sounds, starting
      again.
  13. Sound: `beep`, `tone`, `play`'s music, `say`, `noteOn`.
  14. Files: saving and loading a high score.
- **Lesson 1** also says how the guide works: F5, the next lesson, F1, Save As.
- **Names:** `NN-name.as`, so they sort in order. The menu's text comes from the name:
  `03-asking.as` shows as "3. Asking".

**The editor's Help menu:**
- **Guide** and **Examples** are submenus listing the files, read when the menu opens. A click
  opens the file. `Examples` replaces the file dialog it had.
- **Folders:** both are settings: `editor/angel-guide` (`/rom/share/angel/guide`) and the
  existing `editor/angel-examples`.
- **Menu text from file names:** an example's name is its file's (`snake.as` is "Snake"). A
  lesson's number loses its leading zero, and dashes become spaces.

**New games** (the plan's "in the spirit of Nibbles and Gorillas, newly written": the ideas,
none of QBasic's code, names, levels or art):
- **`serpents.as`:** one or two players (the arrows, and W A S D), each a serpent. It eats the
  numbers 1 to 9 in turn, growing more for higher ones. Levels have walls, which change after a
  9 is eaten. There are lives and a speed that rises.
- **`rooftops.as`:** two throwers on a random skyline take turns, typing an angle and a speed,
  and the throw flies under gravity and the wind.
  - The city is an `Image`, so a hit knocks a round hole in a building, and the flight checks
    the image's pixels.
  - The first to three hits wins.

**Testing:**
- **`angel_examples`** (a host test) checks every example and lesson with `angel -c`. An API
  change that breaks one fails the build's tests.
- **Text lessons' output:** the lessons with only text output (1, 2, 6, 7, 9) run with no input,
  and their output is compared with expected files. A lesson that reads input gets none, so
  `input()` returns "" and the lesson must handle that.

**Budget:** text in the ROM, about 60 KB for the lessons and the two games; nothing in the
firmware but the menus' code.

## 15. As built: step 9 (2026-10-22)

- **The guide:** fourteen lessons in `programs/angel/guide`, in the ROM at
  `/rom/share/angel/guide`, as §14 lists them. Each ends with three things to try. Lesson 1
  says how the guide works.
- **New examples:**
  - `serpents.as`: one or two players, five wall layouts that come round again, numbers worth
    more at higher levels, five lives.
  - `rooftops.as`: the skyline is an `Image` with a see-through colour, so a bomb knocks round
    holes in it, and the flight asks the image's pixels what it hit. It takes four small steps a
    tick, so a fast bomb can't jump over a thin wall. The first to three hits wins.
- **The editor:** Help ▸ Guide and Help ▸ Examples list the folders' scripts by name (lessons
  as " 3. Asking"). The `editor/angel-guide` setting is new, and the Examples file dialog is gone.
- **Tests:** `angel_examples` builds every example and lesson, and runs lessons 1, 2, 6, 7 and 9
  against `tests/scripts/guide/*.out`. `tests/scripts/foreach.as` checks the foreach fix.
- **Found by the lessons:**
  - **`foreach` over a value type** (`string[]`) freed each item twice: AngelScript 2.38.0
    destroyed the items after each time round and again at the loop's end. `continue` also
    skipped their destruction.
    - Fixed by `thirdparty/angelscript/patches/bmds-foreach-items.diff`, which gives the items
      a scope of their own inside the loop's.
    - Script objects didn't show it: their variables are on the heap, and freeing one clears it.
  - **An array of a class needs a constructor without arguments.** Lesson 9 says why.
  - **`foreach` items are read-only:** the array's `opForValue` is `const`. A loop that changes
    the items uses an index.
- **Tried on the board:**
  - The Guide menu, lesson 12 run from the ROM to its game-over screen.
  - Serpents' title, a game, and a life lost at the wall.
  - Rooftops: a throw at 45 and 60 that knocked a hole in a building, and the turn passing over.
- **Checked on the host:**
  - Serpents' five levels, printed as text.
  - Rooftops' hits and its win, by searching for a hitting throw.
- **Left:**
  - **A trap in top-level statements.** A global object declared there *without* an initializer
    (`Game g;`) is constructed when the script is built, before the statements run, so its
    constructor can't use globals set by statements above it (`Window w(...)`, an `Image@`).
    The examples construct their game in `w.run(Game())`, which is safe. The lessons never
    declare such a global, but a reader may. Either initialize them where they stand too, as
    those with an initializer are, or say so in an error. Unscheduled.
