# Phase 1: Serial Shell

Status: **implemented and verified on hardware** (Olimex ESP32-P4-PC, chip revision v1.3).

## 1. Goal

An interactive shell over the USB-C port (USB-Serial/JTAG), usable from any terminal emulator:

- a **console abstraction** the shell runs against, so the same shell later runs on the HDMI console (Phase 4/5) and several shells can run at once;
- an **interrupt-driven serial console** on the USB-Serial/JTAG peripheral;
- a **line editor** with cursor movement, editing keys and history;
- a **shell core**: commands registered at compile time (a rodata table), argument parsing, exit codes;
- the first **built-in commands**: `help`, `version`, `uptime`, `mem`, `tasks`, `log`, `reboot`;
- a **log history** in PSRAM, since a USB serial port almost always misses the boot messages (the host opens the port after the board has booted).

## 2. Interface

### `bmds_core` additions (host-tested)

| Header | Provides |
|---|---|
| `bmds/staticobject.h` | `StaticObject<T>`: aligned storage for one kernel-lifetime object, constructed explicitly at boot (`create()`), never destroyed. Constant-initialized, so a kernel singleton costs no global constructor. |
| `bmds/bytering.h` | `ByteRing`: lock-free single-producer/single-consumer byte FIFO over caller-provided storage (power-of-two size). Producer and consumer may be an ISR and a task, on either core. Not a template, so there is one copy of the code. |

### `bmds_kernel` additions

| Header | Provides |
|---|---|
| `bmds/mutex.h` | `Mutex` (static FreeRTOS mutex), `lock(timeoutMs)`, `tryLock()`, `unlock()`, `Mutex::Locker`. |
| `bmds/semaphore.h` | `Semaphore` (static counting semaphore): `acquire(timeoutMs)`, `tryAcquire()`, `release()`, `releaseFromIsr()`. |
| `bmds/spinlock.h` | `SpinLock` (`portMUX_TYPE`) with `Locker` and `lockFromIsr()`/`unlockFromIsr()`. |
| `bmds/task.h` | `Task`: starts a kernel-lifetime FreeRTOS task with its TCB in `Internal` memory and its stack in a chosen `MemSpace`. |
| `bmds/deadline.h` | `Deadline`: one timeout shared by several waits (`Mutex::lock(const Deadline &)`, `Semaphore::acquire(const Deadline &)`), so a call that waits more than once still honors its `timeoutMs`. (Phase 1b made these headers FreeRTOS-free; see `shell-language.md`.) |
| `bmds/system.h` | `System::info()` (version, build, board, chip), `System::uptimeUs()`, `System::restart()`. |
| `bmds/log.h` | New: `Log::init()`, `Log::setSink()`, `Log::writeRaw()`, `Log::copyHistory()`. |
| `bmds/boot.h` | `Boot::Stage`. `Boot::run(Span<const Stage>)` runs the kernel stages and then the system stages that `main` supplies. `Boot::waitUntilComplete()` lets tasks started by stages wait for the boot report to finish. |

`FromIsr` functions request a context switch with `portYIELD_FROM_ISR()`. On the ESP-IDF RISC-V port, that only sets a flag; the switch happens once, at interrupt exit. This already gives the "single yield at the end of the ISR" behavior that CODING_STANDARDS asks of the interrupt dispatcher, so Phase 1 doesn't need a dispatcher.

### `bmds_console` (new component)

| Header | Provides | Host-tested |
|---|---|---|
| `bmds/console.h` | `Console`, the terminal endpoint interface (below). | yes (interface only) |
| `bmds/lineeditor.h` | `LineEditor`: pure input-byte → edit-state machine; writes its echo to a `Console`. | yes |
| `bmds/usbserialconsole.h` | `UsbSerialConsole`: the USB-Serial/JTAG console singleton. | no |

```cpp
class Console {
        public:
                enum Capability : uint32_t { Ansi = 1u << 0, Color = 1u << 1 };
                struct Size { uint16_t columns; uint16_t rows; };

                virtual const char *name() const = 0;
                virtual uint32_t capabilities() const = 0;
                virtual Size size() const = 0;

                // Waits until at least one byte is available. Single reader.
                virtual Result<size_t> read(WritableByteSpan buf, unsigned int timeoutMs) = 0;
                virtual Result<size_t> tryRead(WritableByteSpan buf) = 0;

                // Writes all of data, or returns the count written with Error::Timeout.
                // One write call is atomic with respect to other writers.
                virtual Result<size_t> write(ByteSpan data, unsigned int timeoutMs) = 0;
                virtual Error flush(unsigned int timeoutMs) = 0;

                Result<size_t> write(StringView text, unsigned int timeoutMs);
};
```

Console text output uses `'\n'` line endings. A console that needs something else (a serial terminal wants CR LF) translates as it writes.

### `bmds_shell` (new component)

```cpp
struct Command {
        const char *name;   // what the user types
        const char *usage;  // argument synopsis, e.g. "[level info|warn|err]"
        const char *help;   // one line
        int (*run)(Shell &shell, Span<char *const> args);   // args[0] is the name
};

#define BMDS_SHELL_COMMAND(Name, Fn, Usage, Help)  // registers a Command in rodata

class Shell {
        public:
                static constexpr int ExitSuccess = 0, ExitFailure = 1, ExitUsage = 2, ExitNotFound = 127;
                static Span<const Command> registeredCommands();      // target only
                static Error spawn(Console &console, const char *name); // target only: task + shell

                Shell(Console &console, Span<const Command> commands);
                void start();                 // print the first prompt
                void feed(char c);            // one input byte; runs a command when a line completes
                int execute(char *line);      // tokenize in place and run
                [[noreturn]] void run();      // read the console forever

                void print(StringView text);
                void printf(const char *fmt, ...);
                int usage();                  // print the current command's usage, return ExitUsage
};
```

Phase 1b changed the command signature to `int (*run)(CommandContext &ctx)`: commands write to stdout/stderr through the context instead of the shell, and `Shell::init()` opens fds 0–2 on the console. See [`shell-language.md`](shell-language.md) ("As built (step 3)").

`tokenizeArgs(char *line, Span<char *> argv) -> Result<size_t>` (`bmds/args.h`) splits a line in place: whitespace separates arguments, `"..."` quotes with `\"`/`\\` escapes, `'...'` quotes literally, a backslash outside quotes escapes the next character, and `#` at the start of an argument begins a comment. An unterminated quote returns `Error::Invalid`; too many arguments returns `Error::Full`. (Phase 1b replaced it with `parsePipeline()`, `bmds/pipeline.h`, which keeps these word rules and adds pipes and redirections.)

## 3. Design

### Composition

`main.cpp` is where the system is put together. It passes its stages to `Boot::run()`: `console` (`UsbSerialConsole::init`) and `shell` (`Shell::spawn(UsbSerialConsole::instance(), "shell")`). The kernel never depends on the console or the shell. A later HDMI console and a second shell are just more stages.

A shell task waits in `Boot::waitUntilComplete()` before its first prompt. It runs at a higher priority than the boot task, so otherwise its prompt lands in the middle of the boot report. The wait is a binary semaphore used as a gate: each waiter takes it and puts it straight back. It isn't an event group, because event groups link in the FreeRTOS timer service task (2 KB stack plus TCB, about 2.8 KB of internal RAM; measured). IDF's `main_task: Returned from app_main()` line is set to warning level for the same reason.

### USB-Serial/JTAG console

The peripheral has 64-byte RX and TX FIFOs and a full-speed CDC-ACM endpoint. ESP-IDF's ROM, bootloader and VFS console already use it in polled mode for early boot and panic output. From the `console` boot stage onward, `UsbSerialConsole` owns it at runtime. Its two `ByteRing`s (`CONFIG_BMDS_USJ_TX_BYTES` / `_RX_BYTES`, 1 KB each) are allocated from `Psram` by `init()`. The ISR is not in IRAM, so it only runs with the cache enabled and may touch PSRAM.

- **TX**:
  - Writers copy into the TX ring, translating `\n` to `\r\n`, while holding a `Mutex`. That makes each write call atomic with respect to other writers.
  - Writers then enable the `SERIAL_IN_EMPTY` interrupt. The ISR moves up to 64 bytes into the FIFO and flushes. When the ring is empty, it flushes once more, sending a zero-length packet so the host releases a final full-size packet, and disables the interrupt.
  - When the host picks up that ZLP, the raw interrupt status is re-armed, so the next enable fires immediately. This is the scheme IDF's driver uses; it is subtle, and the comments in the code say why each step exists.
- **TX with no host reading**:
  - With the port closed, the host never picks up packets and the ring fills.
  - A writer waits up to its timeout, then marks the console *stalled*.
  - `init()` starts out stalled if IDF's boot output is still sitting in the TX FIFO. Otherwise every boot would pay one log timeout (20 ms, measured) when no terminal is attached.
  - A stalled write is queued whole if it fits, and otherwise dropped, without waiting. The backlog the host eventually sees is complete lines (the first ring-full of output), not fragments.
  - The ISR clears *stalled* as soon as the host takes a packet.
  - So a disconnected terminal costs at most one timeout, never one per line. Measured: after closing the port mid-output and reopening it, the shell answers in 51 ms.
- **RX**:
  - On `SERIAL_OUT_RECV_PKT`, the ISR always drains the FIFO into the RX ring at once, and releases a `Semaphore` the reader waits on. The waiting reader uses no CPU.
  - There is **no flow control** toward the host. Input that arrives while the ring is full is dropped and counted (`rxDroppedBytes()`).
  - The 1 KB ring holds a paste of that size while a command runs. Measured: 140 commands pasted in one write (980 bytes) all ran.
  - A file transfer protocol (Phase 2) needs its own acknowledgements.
  - **Hard-won:** the first design held input back by leaving a packet in the FIFO with the interrupt masked, so the host would be NAKed. On the ESP32-P4, a packet left in the RX FIFO is **lost when the ISR sends a TX packet** meanwhile.
    - Instrumented: data was available before the TX write and gone right after it, in every run. The raw RX status bit stays set, but the FIFO is empty.
    - Pastes stopped at exactly the ring size. IDF's driver never sees this because it always drains RX immediately.
- **Locking**:
  - Interrupt-enable read-modify-writes happen from both tasks and the ISR, possibly on different cores, so they are done under a `SpinLock`.
  - The ring indices are atomics with acquire/release ordering.
- **IDF logging** (`ESP_LOG*`) is redirected into `Log` (`esp_log_set_vprintf`), so IDF lines reach the history and the same ring in order. Stray `printf()` to stdout from IDF code still goes through IDF's polled VFS path, which can interleave with ours at packet granularity. BMDS code never uses stdout.

### Log path

- `Log::init()` is the first thing `Boot::run()` does.
  - It creates the log mutex and allocates the history (`CONFIG_BMDS_LOG_HISTORY_BYTES`, 8 KB, `MemSpace::Psram`).
  - It redirects IDF logging.
- Before `init()`, lines go to stdout as in Phase 0. That only happens in very early boot.
- Each line is formatted on the caller's stack (unchanged). Then, under the log mutex, the line is:
  - appended to the history, an overwrite-oldest text ring;
  - handed to the *sink*, a plain function pointer that the console stage sets. The serial sink writes with a 20 ms timeout, so a stalled terminal drops output instead of blocking the kernel.
- Holding the mutex across the sink keeps the console order identical to the history order.
- Lock order is always log mutex → console mutex. The console never logs while holding its own mutex.
- `Log::copyHistory()` snapshots the history, starting at the first complete line, for `log show`.

### Line editor

The line editor is a pure state machine: `feed(char)` returns `None`, `Line`, `Cancel` or `Eof`. It is fixed-size, with no allocation:

- `LineMax` is 255 characters plus NUL.
- History is a 1 KB buffer of NUL-separated entries; the oldest entries are evicted.
- A scratch copy keeps the in-progress line while browsing history.

| Keys | Action |
|---|---|
| printable | insert at cursor (bell when full) |
| Enter (CR or LF; LF after CR ignored) | finish line; add non-empty, non-duplicate lines to history |
| Backspace (DEL/BS), Ctrl-H | delete before cursor |
| Delete (`ESC[3~`), Ctrl-D | delete at cursor; Ctrl-D on an empty line is EOF |
| ←/→, Ctrl-B/Ctrl-F | move one character |
| Ctrl-←/→, Alt-B/Alt-F | move one word |
| Home/End (`ESC[H`, `ESC[1~`, `ESC[7~`, `ESC OH`, ...), Ctrl-A/Ctrl-E | line start/end |
| ↑/↓, Ctrl-P/Ctrl-N | history |
| Ctrl-K / Ctrl-U / Ctrl-W | kill to end / to start / previous word |
| Ctrl-C | cancel line (`^C`) |
| Ctrl-L | clear screen, redraw |
| Tab | reserved for completion (ignored) |

Echo uses relative cursor movement (`ESC[nD`, `ESC[nC`, `ESC[K`), so the editor doesn't need to know the prompt width:

- Typing at the end of the line echoes just the character.
- Any other edit redraws from the changed position to the end of the line.

### Shell

- **Command table:**
  - `BMDS_SHELL_COMMAND` puts each `Command` in its own input section, `.bmds_cmd.<name>`.
  - **Hard-won:** nothing refers to a command by symbol, so a static archive drops an object file that holds only commands. `bmds_shell` is registered with `WHOLE_ARCHIVE`. Other components adding commands need the same, or must define commands in a file that is linked anyway.
  - `bmds_shell/linker.lf` collects them with `SORT(name)` into one contiguous array in `flash_rodata`, bounded by `_bmds_cmd_start/_end`.
  - The table is therefore sorted by name at link time. `help` lists in order, and lookup is a binary search.
  - Any component can add commands, with no global constructors and no RAM cost.
  - The `Shell` constructor asserts that the table is sorted and has no duplicates.
- **Execution:** the editor's buffer is tokenized in place (`MaxArgs` = 16), then the command runs on the shell's task.
  - Unknown command: `name: command not found`, exit code 127.
  - The exit code is kept in `lastExitCode()` (later `$?`).
  - Ctrl-C interrupts a running command (Phase 1b; see [`shell-language.md`](shell-language.md)): the command sees `isInterrupted()`, the shell echoes `^C`, and the exit code is 130.
- **Output:** `printf` formats into a 256-byte stack buffer; longer output is truncated, so commands print line by line. The write timeout is 100 ms; a stalled console fails fast, as described above.
- **Instances:** `Shell::spawn()` allocates the `Shell` (about 1.7 KB) in `Psram` and starts a `Task` running `run()`:
  - stack: `CONFIG_BMDS_SHELL_STACK_BYTES` (64 KB since Phase 1b step 5a), `Psram` (Phase 1: 4 KB `Internal`; the deepest use then was 2,036 bytes, by `tasks`/`log show`);
  - priority: `CONFIG_BMDS_SHELL_PRIORITY` (2);
  - core: none (no affinity).

### Built-in commands

| Command | Output |
|---|---|
| `help [command]` | Command list (name + one-line help), or one command's usage. |
| `version` | `BMDS <ver> (build <git>, ESP-IDF <ver>)`, `Board: <name>`, `Chip: ESP32-P4 rev vX.Y, N cores @ M MHz`. |
| `uptime` | `up <d>d <hh>:<mm>:<ss>.<mmm>` |
| `mem` | One row per MemSpace: total, free, min free, largest free block (bytes). |
| `tasks` | One row per task: name, state, priority, core (`-` = any), stack high-water (bytes free). Needs `CONFIG_FREERTOS_USE_TRACE_FACILITY`. |
| `log` | Level and the debug modules (on/off). |
| `log level info\|warn\|err` | Set the level. (Debug output is controlled per module, not by level.) |
| `log on\|off <module>...\|all` | Enable/disable debug modules. |
| `log show` | Print the log history. |
| `reboot` | Flush the console, restart. |
| `term [color [none\|16\|256\|truecolor] \| size [COLSxROWS]]` | `terminal: <name>`, `color: <depth>`, `size: <cols>x<rows>`; sets the depth, or the size (`size` alone asks the terminal and prints `<cols>x<rows>`). (Phase 1b) |
| `sleep <seconds>` | Wait; fractions allowed (`0.5`), up to a week. Ctrl-C stops it (exit 130). (Phase 1b) |

Text built-ins (Phase 1b, `textcommands.cpp`). They read stdin and follow the GNU tools' output formats; file arguments arrive with storage (`-` means stdin):

| Command | Output |
|---|---|
| `echo [-neE] [word]...` | The words, space-separated, as bash's builtin (`-e` escapes: `\a \b \c \e \f \n \r \t \v \\ \0nnn \xHH`). |
| `cat [-]` | stdin, unchanged. |
| `head [-n count]` | The first `count` (10) lines. Stops reading early, so the writer gets a broken pipe. |
| `wc [-lwc]` | Lines, words, bytes; one count bare, several padded to 7 columns (GNU). |
| `grep [-civnqF] pattern` | Matching lines; `-c` count, `-n` `N:` prefix, `-q` status only. Exit 0 = selected, 1 = none, 2 = error. Patterns are a POSIX BRE subset (`bmds/regex.h`); unsupported syntax (`\(`, `\{`, `[:class:]`, GNU `\|`) is an error, not a literal, so it can be added later without changing any pattern's meaning. `-e`/`-E` are not there yet. |

Language built-ins (Phase 1b step 5): `:` `true` `false` `test`/`[` `break` `continue` `return` `exit` `eval` `set` `shift` `export` `readonly` `unset` `local` `printf` `read` `type` `command`, behaving as bash's (see `shell-language.md`). `exit` at the prompt ends the input; the console shell stays.

Command line syntax (Phase 1b step 3; superseded by the full language in step 5a, see `shell-language.md`): `a | b`, `a |& b`, and redirections `<`, `>`, `>>`, `>|`, `n>&m`, `n<&m`, `n>&-`, `&>`, `&>>`, `>&file`, applied left to right as in bash. The only file until storage is `/dev/null`. `;`, `&&`, `||`, `&` and `<<` are rejected ("unsupported operator").

## 4. IDF usage

| Need | Choice | Why |
|---|---|---|
| USB-Serial/JTAG | **Own driver on `hal/usb_serial_jtag_ll.h`** + `esp_intr_alloc` | IDF's `usb_serial_jtag` driver heap-allocates two FreeRTOS ringbuffers, a mutex and a semaphore, and links the general-purpose ringbuffer code. It blocks every writer for its full timeout when the host isn't reading. Ours uses a static singleton with two PSRAM rings, never allocates after `init()`, fails fast when stalled, and keeps whole lines in the backlog. It also establishes the ISR + semaphore driver pattern for later drivers. |
| Early boot / panic output | IDF ROM/VFS console (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG`), unchanged | Works before our driver exists and after a crash. |
| IDF component logging | `esp_log_set_vprintf` → `Log` | One ordered output path; IDF lines also land in the history. |
| Task list | `uxTaskGetSystemState` (`CONFIG_FREERTOS_USE_TRACE_FACILITY=y`) | A few bytes per TCB. Run-time stats (CPU %) stay off: they add a timer read to every context switch. Revisit with a `top`. |

## 5. Resource budget

Measured (`idf.py size`, ESP-IDF v6.1-dev, `-Os`, debug log and asserts compiled in), against the Phase 0 baseline:

| Item | Phase 0 | Phase 1 | Delta |
|---|---|---|---|
| Image size | 173,176 | 187,362 | +14,186 |
| DIRAM (static) | 65,376 | 66,000 | +624 |
| Code by component | | `bmds_console` 4.3 K, `bmds_shell` 3.4 K, `bmds_kernel` 3.4 K (was ~1 K) | |
| BSS by component | | `bmds_console` 345, `bmds_kernel` 226 | |

Runtime, from the boot report:

| Memory | Phase 0 free | Phase 1 free | Used by |
|---|---|---|---|
| `Internal` | 611,991 | 606,843 | −5,148: shell stack (4,096) + TCB, console and log state |
| `Psram` | 33,551,868 | 33,539,980 | −11,888: log history (8,192), console rings (2 × 1,024), shell session |

- Boot: BMDS init 1.2–1.5 ms; reset to `app_main` unchanged (~265 ms).
- Stages: `console` 0.1 ms, `shell` 0.2–0.3 ms.
- Tasks added: one (`shell`).
- An idle system wakes for nothing: the shell blocks on the RX semaphore, and the TX interrupt is off whenever there's no output.

## 6. ABI impact

Shell command names, arguments and output formats are userspace interfaces under Prime Directive 1 once shipped. No release has shipped yet. The formats above are chosen to be kept: stable column order, numbers in plain bytes, no decorative output. New fields go at the end.

## 7. Testing

- **Host:**
  - `ByteRing` (wrap, full/empty, contiguous spans);
  - `StaticObject`;
  - `tokenizeArgs` (quotes, escapes, comments, limits);
  - `LineEditor` against a fake console (insert, cursor movement, kill keys, history navigation and eviction, escape sequences, CR LF handling, echo bytes);
  - `Shell` (dispatch, not-found, usage, exit codes, sorted table, help output).
- **Target** (pyserial, done):
  - every built-in;
  - editing sequences (arrows, Ctrl-U, history recall);
  - quoting and comment errors;
  - pastes of 40 and 140 commands;
  - output with the port closed mid-command, then recovery;
  - `reboot` with flush;
  - boot capture with and without a host reading.

## 8. Open questions / future work

- No RX flow control (see the hard-won note). Revisit if the hardware turns out to support it some other way.
- Async log lines arriving while a line is being edited garble the prompt line. Fix: the editor clears and redraws around sink output (needs a hook between `Log` and the active editor).
- Lines longer than the terminal width: relative cursor movement doesn't cross wrapped rows. Query the width (`ESC[6n`) or scroll the line horizontally.
- `top` with CPU %, if run-time stats prove cheap enough.
