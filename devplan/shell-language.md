# Shell Language and Standard I/O

Status: **in progress** (Phase 1b). Steps 1 (containers), 2 (streams, pipes, Ctrl-C), 3 (commands on stdio, pipelines, redirection) and 4 (lexer, parser, syntax tree) done. File redirection, globbing and scripts on disk land with Phase 2; external programs in pipelines land with Phase 3.

## 1. Goal

- **The Unix I/O model.** Every command (built-ins now, programs from Phase 3) runs with **stdin, stdout and stderr** as file descriptors 0/1/2, which may be a console, a pipe, a file or a memory buffer. The shell supports **redirection** and **pipelines** on top of that.
- **A bash-modeled language.** We implement the subset that carries most of bash's everyday value: variables and parameter expansion, quoting, command substitution, arithmetic, `if`/`while`/`until`/`for`/`case`, functions, `&&`/`||`, and the standard built-ins. Scripts written in this subset should run unchanged under bash. The reverse is not a goal.
- **Ctrl-C works.** It stops a running loop or pipeline.

## 2. Standard I/O model (kernel)

### Streams and file descriptors

```
fd table (per job)  ──►  FileHandle (refcounted "open file description")  ──►  Stream
  0 stdin                  holds: Stream*, flags (read/write/append)          Console, Pipe,
  1 stdout                                                                     MemoryStream,
  2 stderr                                                                     NullStream, File (Phase 2)
  3..15 (n>&m, later open())
```

- **`Stream`** (kernel-internal interface; `Console` becomes one):
  - `read(buf, timeoutMs)` returns 0 bytes with `Error::EndOfFile` at end.
  - `write(data, timeoutMs)` returns `Error::BrokenPipe` when no reader is left. This is a new `Error` code, appended.
  - `isTerminal()`, and `close()` called on the last reference.
- **`FileHandle`**: an intrusively refcounted wrapper shared by every fd that refers to it (`dup`, `2>&1`, inherited by pipeline stages). Closing the last reference closes the stream. That last close is what produces EOF on a pipe.
- **`FdTable`**:
  - 16 slots (`FdTable::MaxFds`), copied cheaply when a command starts: one refcount increment per open fd.
  - Redirections are applied to the copy, so the shell's own table is never disturbed.
- **`Pipe`**:
  - A `ByteRing` in `Psram` (`CONFIG_BMDS_PIPE_BYTES`, 4 KB) with two semaphores.
  - A reader blocks while the pipe is empty and writers remain; a writer blocks while it is full and a reader remains.
  - Reads and writes take an `Interrupt` (below), so a blocked stage can be stopped.
- **`MemoryStream`**: a growable `Psram` buffer. It captures the output of `$(...)` and holds here-documents.
- **`NullStream`**: `/dev/null`.

### As built (step 2)

- **The kernel's synchronization headers are FreeRTOS-free.** `Mutex` and `Semaphore` hold the `StaticSemaphore_t` as opaque storage (92 bytes on the target, checked by `static_assert`; the separate handle is gone, saving 4 bytes per object), and `Deadline` holds an absolute `esp_timer` time instead of FreeRTOS tick state. `bmds_kernel` is now dual-mode like the console and shell: `Interrupt`, the streams, `FdTable` and `Pipe` build for the host tests too, over `src/host/platform.cpp` (`std::mutex`/`condition_variable`). Pipes, interrupts and (later) whole pipelines are tested on the host with real threads, including under ThreadSanitizer.
  - **Hard-won:** the host `Mutex` is a binary semaphore, not `std::timed_mutex`. A timed lock goes through `pthread_mutex_clocklock`, which ThreadSanitizer doesn't intercept, so it reports a bogus "unlock of an unlocked mutex".
  - **Hard-won:** GCC's `-Wnon-virtual-dtor` treats any `friend` as making a protected destructor accessible, so `Stream::handleClosed()` is public (documented as FileHandle's callback) rather than reached through a friend.
- **`FileHandle::Ptr`** is an intrusive counted reference; copying never fails, so `FdTable` is an ordinary copyable value (16 slots, 64 bytes on the target). Handles are 12-byte `Psram` allocations made when a file is opened.
- **`Pipe`** is one `Psram` allocation (object + ring, `CONFIG_BMDS_PIPE_BYTES`), with reader and writer mutexes (the ring is single-producer/single-consumer) and two binary "event" semaphores. It frees itself when both ends are closed.
- **`MemoryStream`** is a `String`; **`NullStream`** is a constant-initialized singleton.
- **`Interrupt` wakes blocked waiters without owning a FreeRTOS object:** a waiter publishes the semaphore it is about to block on in one of 8 slots, and `raise()` releases every published semaphore. ESP-IDF forbids FreeRTOS calls inside a critical section, so the slots are atomics with a "raisers busy" count that a leaving waiter drains before its semaphore can be destroyed; a task-context `raise()` suspends the scheduler around the releases so the drain is always short. Waits therefore cost nothing extra when no interrupt comes. `Interrupt::sleep()` is an interruptible delay.
- Measured: image +1.4 KB, static DIRAM +32 bytes (the console's `Interrupt` and flush marker, less the dropped handles). Pipes and fd tables add nothing until step 3 uses them.

### Interrupts (Ctrl-C)

Each job has an `Interrupt`, an atomic flag plus a wakeup. The console raises the foreground job's `Interrupt` from its RX path (see `terminal.md`). Then:

- blocking stream operations return `Error::Interrupted`;
- the interpreter checks the flag between commands and on every loop iteration, and unwinds with status 130, as bash does;
- built-ins with long loops (`log show`, later `cat`) check it too.

### What commands see

```cpp
class CommandContext {                  // replaces (Shell &, Span<char *const>)
        public:
                Span<char *const> args() const;
                Stream &in();  Stream &out();  Stream &err();
                Environment &env();             // variables (read/write for the last pipeline stage)
                bool isInterrupted() const;
                void printf(const char *fmt, ...);      // to out()
                void eprintf(const char *fmt, ...);     // to err()
                int  usage();
};
```

- Built-ins write to `ctx.out()`, never to "the console". So `mem | grep Psram` and `version > /sd/v.txt` just work.
- Error messages go to `ctx.err()`.
- `BMDS_SHELL_COMMAND` stays; only the function signature changes. That is fine pre-release: commands are a kernel-internal API, while their names and output formats are what's frozen.

### Pipelines: execution model

- `a | b | c` runs the stages **concurrently**, connected by pipes, as bash does.
- The **last stage runs on the shell's own task** (bash's `lastpipe`), so `cmd | while read x; do ...; done` can set variables.
- **Earlier stages run on worker tasks:**
  - Workers come from a pool: created on demand, with up to 2 idle ones kept.
  - Each has an 8 KB stack in **`Psram`** (`CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` is already on), so a pipeline costs internal RAM only for TCBs (~350 B per stage).
  - A worker stage gets a **snapshot of the variables**, a cheap copy, so changes it makes are discarded. That matches bash subshell semantics.
- **Status:**
  - The pipeline's status is the last stage's (`set -o pipefail` is optional, later).
  - When a downstream stage exits, upstream writers get `BrokenPipe` and stop.
- **Constraint on PSRAM-stack tasks:** they must not perform SPI-flash writes. NVS and OTA work goes to a helper task. Recorded in CODING_STANDARDS.

### As built (step 3)

- **`Thread`** (kernel, portable header): a task that ends. `join()` waits for the entry function, then deletes the task and frees its stack and control block. The task parks itself (`vTaskSuspend`) after signalling, and `join()` deletes it only once it is seen suspended, so a task is never deleted while still running on the other core and no FreeRTOS idle-task cleanup is involved. Programs (Phase 3) will be Threads too.
- **`WorkerPool`** (kernel): starts workers on demand up to `maxWorkers` (7 for the shell: `MaxStages` − 1), keeps up to `maxIdle` (2) for reuse, and joins the rest. Measured: a worker stage peaks at ~1.9 KB of its 8 KB `Psram` stack (`CONFIG_BMDS_SHELL_WORKER_STACK_BYTES`).
- **`CommandContext`** replaces `(Shell &, Span)`: `args()`, `read()` (stdin), `write(fd)`, `print`/`printf` (stdout), `eprint`/`eprintf` (stderr), `isInterrupted()` (Ctrl-C *or* stdout became a broken pipe, the SIGPIPE stand-in), `usage()`. Built-ins send errors to stderr. No `Environment` yet; it comes with variables in step 5.
- **Output never times out at the caller.** Commands write with `WaitForever` (a full pipe holds them back; Ctrl-C releases them). The serial console decides by itself when nobody is reading: output that makes no progress for `UsbSerialConsole::StallMs` (100 ms) marks it stalled, whatever the caller's timeout.
- **The runner** (`Shell::runPipeline`): each stage gets a copy of the shell's `FdTable` (fds 0–2 share one console handle), then its pipe ends, then its redirections left to right. Only the stages hold pipe ends, and a stage closes its table as soon as its command returns, which is what produces EOF downstream and `BrokenPipe` upstream. A stage that can't start (unknown command, failed redirection, no worker) closes its ends too, so its neighbors finish. The last stage runs on the shell task; the status is its exit code.
- **Interim parser.** `parsePipeline()` handles words, `|`, `|&` and redirections, and rejects `;`, `&&`, `||`, `&`, `<<` with "unsupported operator". It replaces Phase 1's `tokenizeArgs()` and is itself replaced by the step 4 lexer/parser; the runner and `Redirect`/`Stage` model stay.
- **Text built-ins** `echo`, `cat`, `head`, `wc`, `grep` (see `shell.md`), so pipelines are useful before storage: `mem | grep Psram`, `log show | grep -c E`, `help | wc -l`.
- Measured: image +14.1 KB (parser 1.5 KB, runner 1 KB, text built-ins ~3 KB, regex 0.7 KB, plus CommandContext, Thread, WorkerPool and pipes now linked in). Static RAM unchanged. Two idle workers hold their control blocks in internal RAM. Internal RAM is unchanged across 30 pipelines on the board. Shell task: 1.6 KB of its 4 KB stack free with the last stage on it (moves to 16 KB `Psram` in step 5).
- **Phase 3 implication:** a pipeline of two external programs needs two programs loaded at once. The executable format and memory layout must allow several resident programs (position-independent code, per-program heaps). If that turns out too costly, the fallback is DOS-style: program → `MemoryStream` → program, run one after the other. Decide in the Phase 3 ABI design; this plan works either way.

## 3. Language subset

Modeled on bash. Anything not listed is out of scope until someone needs it.

| Area | Included | Deferred / excluded |
|---|---|---|
| Lexing | words, `'…'`, `"…"`, `\`, `#` comments, line continuation, multi-line input with the `PS2` prompt | `$'…'` ANSI-C quotes (later, cheap), brace expansion `{a,b}`, tilde expansion |
| Lists | `;`, newline, `&&`, `\|\|`, `!`, `{ …; }`, `( … )` (runs on the same task with saved and restored variables) | `&` background jobs, job control (`jobs`/`fg`/Ctrl-Z), coprocesses |
| Pipes and redirection | `\|`, `\|&`, `<`, `>`, `>>`, `>\|`, `<>`, `2>`, `2>>`, `&>`, `&>>`, `n>&m`, `n<&m`, `>&-`, `<<EOF`/`<<-EOF` here-docs, `<<<` here-strings. Without a filesystem (until Phase 2), only `/dev/null` works as a file target | process substitution `<(…)`, noclobber (`set -C`) |
| Parameters | `NAME=value`, `VAR=x cmd` prefix assignments, `$NAME`, `${NAME}`, `${NAME:-w}` `:=` `:+` `:?`, `${#NAME}`, `${NAME#p}` `##` `%` `%%` (glob patterns), `$?`, `$#`, `$@`, `$*`, `$0`–`$9`, `${10}`, `$$`, `export`, `unset`, `local`, `readonly`, `shift` | arrays, associative arrays, `${NAME/pat/rep}` (later, cheap), `${!NAME}` indirection |
| Expansion | command substitution `$(…)` (and backquotes), arithmetic `$((…))`, word splitting on `IFS`, quote removal, `"$@"` semantics | pathname globbing `*.txt` (Phase 2, needs the filesystem) |
| Arithmetic | 32-bit signed integers: `+ - * / % ** << >> & \| ^ ~ ! && \|\| == != < <= > >= ?: =  += -= *= /= %= ++ --`, parentheses, variables, `(( … ))` as a command | 64-bit integers, floating point |
| Compound | `if/then/elif/else/fi`, `while`, `until`, `for x in …; do`, `for ((i=0;i<n;i++))`, `case … in pat\|pat) …;; esac` with glob patterns, `break [n]`, `continue [n]` | `select`, `;&`/`;;&` fallthrough |
| Functions | `name() { …; }`, `function name { …; }`, positional parameters, `local`, `return [n]`, recursion (depth-limited) | |
| Tests | `test` / `[ … ]`: `-z -n = != -eq -ne -lt -le -gt -ge ! -a -o`, `( )`; file tests `-e -f -d -s -r -w` (Phase 2) | `[[ … ]]` with `=~` regex (later; regex is costly), `-nt`/`-ot` |
| Built-ins (language) | `echo` (`-n -e`), `printf` (`%s %d %i %u %x %o %c %%`, widths), `read` (`-r -p`, IFS splitting), `true`, `false`, `:`, `exit`, `return`, `set` (`-e -u -x`, `--`, positional), `shift`, `export`, `unset`, `local`, `readonly`, `eval`, `source`/`.`, `type`, `command` | `trap`, `getopts` (later), `alias`, `history` expansion (`!!`) |
| Interactive | `PS1`/`PS2` with prompt escapes (`\w \h \$ \e \[ \] \n`) and colors, Tab completion of commands and variables (paths in Phase 2) | |
| Startup | `/sd/autoexec.sh` when present (a nod to DOS), Phase 2 | |

Behavior follows bash where the subset overlaps. Where we diverge, it is listed in this table or in tests.

## 4. Interpreter design

```
line editor ─► Lexer ─► Parser ─► AST ─► Executor ─► CommandContext ─► built-in / function / program
                 ▲        │ "need more input" → PS2 continuation
                 └────────┘                   Expander: parameters, $(…), $((…)), splitting, quotes
```

- **Lexer.** It is context-sensitive, as bash's is: reserved words only in command position, and operators. Here-document bodies are collected after the line that introduces them.
- **Parser.** Recursive descent over the POSIX shell grammar subset. It produces a compact AST:
  - Node kinds: `Simple`, `Pipeline`, `AndOr`, `List`, `If`, `While`, `For`, `ArithFor`, `Case`, `Group`, `Subshell`, `FunctionDef`, `Arith`.
  - Words are kept as segment lists (literal, single-quoted, double-quoted, parameter, command substitution, arithmetic), so quoting survives until expansion.
  - An incomplete construct returns `Error::NotReady` ("need more input"), which drives `PS2` continuation in the interactive shell.
- **Memory.**
  - AST and expansion temporaries are allocated from a **bump arena** in `Psram` (64 KB chunks, grown on demand), released wholesale after each top-level command.
  - **Function definitions are stored as source text** and reparsed on call. That is simpler than keeping AST lifetimes and costs nothing that matters at shell speeds.
  - Variables live in a `Psram` hash table owned by the shell, with a scope stack for `local`.
- **Strings and lists** (step 1, done). `bmds_core` now has `String`, `List<T>` and `Arena`, all `MemSpace`-aware (default `Psram`) and host-tested:
  - **Allocation failure is reported, never hidden.** Every operation that may allocate returns `Error` (`NoMem`, object unchanged). The containers are **move-only**: a copy is an allocation, so it is explicit (`clone()` returns `Result`). Constructors never allocate. A runaway script (`x=$x$x` in a loop) ends with "out of memory" in that command rather than a reboot.
  - **`String`** is 24 bytes on the target and stores up to 15 bytes inline (most shell words never touch the heap). Longer text grows by half again in 16-byte block steps. Operations whose argument is a view of the same string (`s.append(s)`) are safe. Read-only operations stay on `StringView`.
  - **`List<T>`** is contiguous (converts to `Span`), owns no block while empty, supports move-only `T`, and keeps `emplace` atomic: the new element is built in the new block before the old ones move, so arguments may refer to existing elements.
  - **`Arena`** is a chunked bump allocator with `mark()`/`rewind()`, `reset()` (keeps one chunk) and lazy first allocation. It only accepts trivially destructible objects.
  - **`Memory` moved to `bmds_core` as a link seam** (declared there, defined by the platform, like `assertFailed`): the kernel defines it on the target; the host tests define it with allocation counting (leak checks) and failure injection (every `NoMem` path is tested). The host tests also pass under ASan/UBSan.
  - Measured on the target: `String` 1.5 KB, `Arena` 0.6 KB of code; `List` is header-only.
- **Stack.**
  - The parser and executor recurse, so the shell task stack moves to **16 KB in `Psram`**. That frees the current 4 KB of internal RAM.
  - Nesting depth and function recursion are bounded (`CONFIG_BMDS_SHELL_MAX_DEPTH`, 64 and 100), and exceeding either is an error rather than a stack overflow.
- **As built (step 4).**
  - `Parser` (`bmds/parser.h`) and the tree (`bmds/syntax.h`). `next()` returns one complete command at a time (up to an unquoted newline, with any compound command or here-documents it opens), so a script runs up to its first syntax error, as in bash. Incomplete input returns `NotReady`; a trailing `\`-newline counts as incomplete (interactive behavior; `bash -n` on a whole file would accept it).
  - The lexer is driven by the parser: `((` is an arithmetic command only in command position, reserved words are recognized by the parser only where a command starts, and here-document bodies are read when the lexer reaches the newline after the operator. `$((…))` is arithmetic only if its first `)` at depth 0 is followed by another; otherwise it's a command substitution (bash's rule, checked against bash).
  - Nodes, words and redirections are arena objects linked through `next`; text points into the source (no copies, except backquote bodies with escapes and joined here-doc delimiters). Function definitions keep their body's source text (`FunctionNode::source`, including trailing redirections) for reparsing.
  - Errors carry a format and a token and are worded as bash words them (`appendSyntaxError()`); the line number comes from the offset.
  - **Corpus** (`tests/host/corpus/syntax.txt`, ~95 entries): each snippet with its expected tree, `incomplete`, or bash's error message and line. `check_bash.py` (a ctest, when bash is installed) runs every `ok`/`error` entry through `bash -n` and fails on any disagreement. A further test parses every prefix of every entry and 3,000 random token soups, under ASan/UBSan.
  - Known divergences: a `case` inside `$(…)` (the pattern's `)` ends the substitution: parentheses are counted, not parsed); `$"…"` locale strings (taken as `$` + a quoted string); `|&` is supported (the table above lists it as deferred); `${x:offset}` and other unsupported `${…}` forms are parse errors ("bad substitution") where bash accepts them.
  - Size: `parser.cpp` 12.7 KB, `syntaxdump.cpp` 2.7 KB of code (the dump is for tests; nothing on the target calls it yet, so neither is linked until step 5).
  - **Hard-won:** `std::string_view::substr` in `StringView::mid()` made the never-called parser link libstdc++'s exception runtime (+5.7 KB flash, +198 B DIRAM) because `bmds_shell` is linked whole-archive. `StringView` slicing is now throw-free, and `cmake/check_map.cmake` fails the build if the runtime is ever linked again.
- **Executor.**
  - It walks the AST. `set -e`/`-u`/`-x` behave as in bash, with `-x` tracing to stderr.
  - `break`/`continue`/`return`/`exit` propagate as control-flow results, not exceptions (`-fno-exceptions`).
- **Lookup order:** functions → built-ins → (Phase 3) programs on `PATH`.

## 5. Phasing

| Step | Delivers | Depends on |
|---|---|---|
| 1 | `String`, `List<T>`, arena allocator in `bmds_core`, with host tests | — |
| 2 | `Stream`, `FileHandle`, `FdTable`, `Pipe`, `MemoryStream`, `NullStream`, `Interrupt`; `Console : Stream`; Ctrl-C in the USJ RX path | 1 |
| 3 | `CommandContext`; built-ins ported to stdout/stderr; worker pool; pipelines and fd redirection (`2>&1`, `/dev/null`) | 2 |
| 4 | Lexer + parser + AST (host-tested against a corpus of bash snippets) | 1 |
| 5 | Executor: variables, expansion, `$(…)`, `$((…))`, compound commands, functions, language built-ins, `PS1`/`PS2`, completion | 3, 4 |
| 6 | Terminal model: `Color`/`TextStyle`/`Sgr`/`VtParser`/UTF-8, `ColorDepth`, `term` built-in, colored prompt and log levels (`terminal.md`) | 2 |
| Phase 2 | file redirection, globbing, file tests, `source` from SD, `autoexec.sh`, path completion | storage |
| Phase 3 | programs as commands and pipeline stages, fds and the environment in the ABI | loader |
| Phase 4 | `TerminalScreen` + HDMI renderer | display |

## 6. Resource budget (estimates)

| Item | Flash | RAM |
|---|---|---|
| `String`/`List`/arena | 2.1 KB measured (`List` header-only) | — |
| Streams, pipes, fd tables, worker pool | ~5 KB | per pipe: 4 KB `Psram` ring; per worker: 8 KB `Psram` stack + ~350 B internal TCB |
| Lexer + parser | ~12 KB | AST in the `Psram` arena |
| Executor + expansion + arithmetic | ~15 KB | variables in `Psram` |
| Language built-ins (`test`, `printf`, `read`, …) | ~8 KB | — |
| Shell task | — | 16 KB `Psram` stack (−4 KB internal compared with today) |
| **Total** | **~45 KB** | internal RAM falls compared with Phase 1 |

For scale: dash, a full POSIX shell, is about 100 KB on x86-64. A Kconfig switch (`CONFIG_BMDS_SHELL_SCRIPTING`) can leave out the language (steps 4–5) for a minimal build.

## 7. ABI impact

- **Programs (Phase 3)** receive fds 0/1/2, `argc/argv` and the exported environment (`envp` or `bmds_getenv`), and use `bmds_read/write/close/dup2/isatty`. The fd numbering and the standard-stream convention are frozen from the first program.
- **The shell language subset** is a shell interface. Once shipped, a script that runs keeps running. Syntax may be added, never changed.
- **Built-in output formats** (Phase 1) are unchanged. They now go to stdout, and messages go to stderr.

## 8. Testing

- **Host:**
  - the lexer and parser against a snippet corpus, with expected ASTs;
  - the expander and executor against expected output, using a fake console and in-memory streams;
  - a differential test against real bash: run each corpus script under `bash --posix` and under our interpreter, compare stdout and status;
  - pipes with a host threading shim;
  - `FdTable` refcounting and EOF propagation.
- **Target:**
  - pipelines across built-ins;
  - Ctrl-C out of `while true; do :; done` and out of a blocked pipeline;
  - deep recursion hitting the depth limit cleanly;
  - stack high-water marks.

## 9. Open questions

- Should `( … )` subshells run on a worker task, for true isolation, or on the same task with saved and restored state? Recommended: same task. It is cheaper, and nothing can tell the difference without background jobs.
- Background jobs (`&`) need job control on the console. Defer until there's a need, probably with programs in Phase 3.
- Program pipelines and concurrent residency (section 2), to be decided in the Phase 3 ABI design.
