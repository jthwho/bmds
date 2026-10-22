# Scripting: AngelScript, a Runtime with the Whole System, and an IDE

Status: **steps 1-4 built** (2026-10-21): C++ in the SDK; `angel` in the ROM with its core,
the text screen, files and other programs, graphics and input; the patches; the corpus and the
API lock. As built and measured: §10 and §11. Next: step 5, sound.

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
- **Sound:**
  - `Sound s("boom.wav"); s.play(volume, pan)` (voices, looping);
  - `Music::play("song.mid")` (MIDI through the synth, or audio files);
  - `beep()` and `tone(hz, ms)`;
  - `play("T120 O4 L8 CDEFGAB>C")`: QBasic's `PLAY` language on the synth, with an
    instrument chosen by General MIDI number;
  - `say("Hello")` (speech, `speech.md`). Children like a computer that talks back.
- **Files** (built, step 3):
  - `File(path, mode)` ("r", "w", "a"): `readLine`, `readAll`, `eof`, `write` and `writeLine`
    (any value, as `print`), `close`, `isOpen`;
  - global functions: `fileExists`, `isDirectory`, `readFile`, `writeFile`, `appendFile`,
    `removeFile`, `renameFile`, `listDirectory`, `makeDirectory`, `currentDirectory`,
    `changeDirectory`;
  - what fails raises an exception that says why, which `try`/`catch` can handle;
  - paths are the system's, so network mounts work as well.
- **Network:**
  - `Socket` (TCP `connect` and `listen`/`accept`, `send`, `receive`, `receiveLine`, UDP
    `sendTo` and `receiveFrom`, timeouts);
  - `Http::get(url)` and `post`, plain HTTP over `Socket` in the runtime. HTTPS waits on a TLS
    client (§9).
- **System:**
  - built (step 3): `run(command)` (its output on the screen; the exit status) and
    `output(command)` (what it printed);
  - built (step 4): `setting(key)` (a setting's value, read only) and `notify(title, text)`.

### The IDE (working name *Workshop*)

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
5. **Sound:** `Sound`, `Music`, `tone`, `beep`, `play` (the music language, on the synth), `say`.
6. **Network:** `Socket`, `Http`. Examples: a chat between two boards, fetching a page.
7. **`CodeEdit` and the IDE:**
   - the widget, which also closes Phase 8's "on-device text editor" with a plain editor
     application;
   - then the IDE: `angel --serve`, Run and Stop, diagnostics, F1 and completion, templates, the
     `.as` file type.
8. **The debugger:** breakpoints, stepping, the call stack and watching variables, through a
   line callback in `angel` with line cues on. Controlled by the IDE over the same kind of pipe
   as `--serve`.
9. **The guide and examples:** a tutorial in steps, from `println("Hello")` to a game, and
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
