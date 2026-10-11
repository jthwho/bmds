# Pseudo-terminals

Designed 2026-10-05 (the user's request, with `/bin` in `programs.md` §17 and the terminal as an
ordinary application, `windows.md` §8 step 9). Not started.

## 1. Goal

A terminal emulator is an ordinary program: it opens a pseudo-terminal, starts a shell (or any
program) on one end, and shows what comes out of the other. Nothing in the kernel or the shell
knows what a terminal emulator is. Today the terminal hands the shell a `ScreenConsole`, a console
wired straight into its screen model; that is what goes.

## 2. Facts that shape the design

- **`Console` already is the tty.** Its modes (line or raw, echo, `ICRNL`, the line editor in each
  `Process`), Ctrl-C raising its interrupt (`SIGINT` to the job, `programs.md` §15), its size and
  `resized()` (the launcher's `SIGWINCH`), and its readiness for `poll` (§16) are all in the base
  class or around it. A pty's program end is a `Console`, and gets all of it unchanged.
- **The program launcher takes the console with the request**
  (`ProgramLauncher::Request::console`), so whoever starts a program says which console is its
  terminal.
- **The terminal must read output while the shell runs**, so starting the shell can't wait for it
  to end; the launcher only has `run()` (start and wait) today.
- **Output speed is measured:** `time seq 1 3000` into a shown terminal is 0.51 s today
  (`windows.md` §9, 8d), the screen model taking output directly on the writer's task.

## 3. Design

- **A `Pty` is a numbered pair** (`bmds_console`): `Pty::open()` gives the next free one, `ptyN`
  (no singletons). Up to 8.
  - **The program's end, `PtyConsole`:** a `Console` named `ptyN`, reachable as `/dev/ptyN` (the
    VFS's device directory, as `/dev/console`). Its writes go into the output ring; its reads come
    from the input ring; input pushed into it goes through the console's usual path (Ctrl-C
    raising the interrupt and dropping what came before it, as `ScreenConsole::push()` does now).
  - **The emulator's end, `PtyMaster`:** `read(span, timeoutMs)` takes output (waiting while
    there is none: a semaphore, no polling), `write(span)` adds input, `setSize(columns, rows)`
    sets the console's size (so `resized()`, so `SIGWINCH`), `readiness()` and `readinessWaiters()`
    as a `Stream` (`poll`). Built-in programs use it as a C++ object; programs built separately get
    it through the SDK's `posix_openpt`, `grantpt`, `unlockpt`, `ptsname` and the `/dev/ptmx`
    convention when the program ABI does (later, `programs.md`).
- **Rings:** output 4 KB, input 512 bytes (the console's input size today), in internal RAM if the
  measurement asks for it, else PSRAM. A writer that fills the output ring waits for the reader:
  output is never dropped (a terminal that stops reading stops its program, as on Linux).
- **Hangup:** when the emulator closes its end, the program's reads end (end of input) and its
  console reports `HungUp`; the shell exits on it. When the program's end has no users left (the
  shell exited), the emulator's reads end once the ring is empty, so it knows to close.
- **Starting a program without waiting:** `ProgramLauncher` gains `start()` (the same request;
  returns a handle) with `wait(handle, timeoutMs)` and `signal(handle, sig)`; `run()` becomes
  `start()` then `wait()`. This is `posix_spawn` and `waitpid` in the kernel's own terms, for
  built-in programs that start others.
- **Cost:** one copy of output through the ring, and a wake of the emulator's task per burst; the
  emulator then applies the burst to its screen model. Measured against the 0.51 s above; the
  ring's size and place are chosen by that measurement.

## 4. Testing

- **Host:** the pair (output in order, a full ring making the writer wait, input reaching reads,
  Ctrl-C raising the interrupt, the size and `resized()`, both hangups, readiness and its
  notifications), with fake tasks as the existing console tests do.
- **Board:** the terminal on a pty: commands, `vi`, `less`, Ctrl-C, `SIGWINCH` on a resize, `exit`
  closing the window, the window closing hanging up the shell; `time seq 1 3000`; free memory and
  tasks the same after opening and closing terminals.

## 5. Steps

1. `Pty`, `PtyConsole`, `PtyMaster` and their host tests; `/dev/ptyN`.
2. `ProgramLauncher::start()`, `wait()`, `signal()`.
3. The terminal on a pty (with `windows.md` §8 step 9): `ScreenConsole` no longer used by it.

## 6. As built (2026-10-06)

- `Pty` (the emulator's end, a `Stream`; the design's `PtyMaster`) and `PtyConsole` in
  `bmds_console`, 8 numbered pairs as static objects; each open allocates its rings (4 KB output,
  512 bytes input) in the space the caller names (the terminal: PSRAM). Host-tested
  (`tests/host/pty.cpp`): order, a full ring holding the writer, Ctrl-C, sizes, both hangups,
  readiness.
- The terminal's shell is not a launched program: `Shell::spawn()` runs a session on the pty's
  console (as the serial shell's), which costs no process and no program load. `Shell::run()`
  returns at the end of input (the hangup).
- `ProgramLauncher::start()` exists (the loader's detaches the process; nobody waits) for
  `win term`; `wait()` and `signal()` wait for a user. `/dev/ptyN` and `/dev/ptmx` wait for the
  program ABI to need them.
- Measured: `time seq 1 3000` 0.45 s, against 0.51 s with `ScreenConsole` (`windows.md` §9).

## 7. Programs' pseudo-terminals (2026-10-11, `network.md` §24-25)

- `/dev/ptmx` and `/dev/pts/N` for programs (Dropbear's sessions): `Pty::openCounted()` makes a
  pair that finishes on its own when its ends' users (handles, and processes that have the program
  end as their terminal) are gone; the terminal application's pairs keep their explicit calls.
- A session leader takes a pty as its terminal (`TIOCSCTTY`, or an open without `O_NOCTTY`):
  `Process::setConsole()`. Slot 110, `term_ctl`, carries the pty and terminal operations.
