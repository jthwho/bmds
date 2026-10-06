# Processes, threads and jobs

Design, 2026-10-06, agreed with the user: "a process should be a collection of one or more tasks
(threads)", and background jobs (`&`) need processes that can be managed. Builds on
`programs.md` §13 (process creation), §15 (signals), §17 (`/bin`) and `pty.md`.

## 1. Goal

Every piece of running work outside the kernel belongs to a process, which can be listed, signalled,
waited for, stopped and continued, and whose resources all go when it ends. A process is a pid, its
resources, and one or more threads (FreeRTOS tasks). On top of that: process groups, a foreground
group per console, and job control in the shell (`&`, `jobs`, `fg`, `bg`, `wait`, `kill %N`,
Ctrl-Z).

Yardstick: `sleep 10 &` then `jobs`, `fg`, Ctrl-Z, `bg`, `kill %1`; `ps` shows every shell, program
and terminal with their threads; killing `terminal` removes its window, its shell and every task
they had; free memory and the task list are the same after any of it.

## 2. What exists (2026-10-06)

- **Loader programs are processes, with one task each.** `Running` (`bmds_loader/src/running.h`)
  is the program's task, its `Process` (fds, memory, directories, cwd, arguments, `Signals`), its
  pid and job, its image, and the vfork children running on its stack. FreeRTOS thread-local
  pointer 1 holds it; system calls find it there. `ProcessTable` (32 entries, `Psram`) keeps pid,
  parent, job, zombie status. Programs from files and `/bin` entries started as programs (`/bin/sh`,
  `terminal`, applets exec'd by programs) all go this way.
- **Kill** is a handshake per task: the killer suspends it and reads `inKernel`; in program code it
  is ended at once, in a system call it is resumed and ends as the call returns. A built-in program
  is "in the kernel" for good, so it is only ever ended through its interrupt, cooperatively.
- **Shell sessions are not processes.** The serial shell and every terminal's shell are `Session`s
  (`Shell::spawn()`, a bare `Task`), all posing as parent `ShellPid` (1), which can't be waited for
  or signalled. Built-in commands run on the session's task; earlier pipeline stages run on its
  `WorkerPool`'s `Thread`s. None of these appear in the process table.
- **The terminal's shell belongs to nobody.** `/bin/terminal` is a process; its shell session
  (`termN`) is a task it started with `Shell::spawn()`. If `terminal` were killed instead of closed,
  `termN` would be left behind.
- **Jobs** are the pid of the shell's program a process descends from; process groups are jobs
  (`getpgid`), and `setpgid`/`TIOCSPGRP` succeed without effect. Ctrl-C reaches a job through the
  launcher waiting for it (`waitForJob()`), not through the console. Stop signals are dropped
  (§15: "job control is out of scope"; this plan brings it in).
- **No `&`, no `ps`.** `tasks` lists FreeRTOS tasks.

## 3. Design

### 3.1 The model

- **Process:** a pid, a parent, a process group and a session (§3.6), its resources (`Process` as
  now), its threads, its image (a loaded program's; none for kernel code), its wait status once
  ended. Shared by its threads: memory, fds, cwd, signal actions, the console.
- **Thread:** one FreeRTOS task in one process. Its own: stack, signal mask, interrupt, the
  kill/stop handshake flags (`inKernel`, `kill`, `stop`), the SDK's hint word, and its vfork
  children (they run on its stack). Thread-local pointer 1 holds the thread record, which points at
  its process. `Running` splits into these two records.
- **Kernel tasks** (`wm`, `usb0`, `storage`, `flash0`, `scanout`, IDF's) belong to no process, as
  Linux's kthreads: started with `Task`, never killed by signals, not listed by `ps` (`tasks` still
  shows everything).
- **Which a new task is.** `Task` stays the kernel task. `Thread` (the kernel's joinable task, used
  by `WorkerPool`) starts in the caller's process when the caller has one, and joins its thread
  list; started from a kernel task it is a kernel thread. This is why a driver started lazily from a
  shell command (`win start` starting `wm`) stays a kernel task: it uses `Task`. The layering:
  `bmds_kernel` keeps an opaque per-task owner pointer and two hooks (a thread starts in the
  caller's owner, a thread ends); `bmds_loader`, which owns processes, installs them. The kernel and
  the shell stay host-testable (a `thread_local` there).

### 3.2 Ending

- **A process ends** when its last thread ends, or at `exit()`, or when a signal ends it. Ending
  every thread: those in program code are ended at once (the handshake, per thread); those in kernel
  code have their interrupt raised and end themselves; the process's resources are released when
  the last one is gone, then it becomes a zombie for its parent as now.
- **Kernel-code threads end cooperatively.** A built-in program's threads can't be stopped by force
  (one may hold a lock), so a built-in program must end its threads before `main` returns (as a
  `std::thread` must be joined). If it doesn't, they are interrupted and the process waits for them;
  `ps` shows it as ending. Built-in programs block only through interruptible waits
  (`Interrupt::wait()`), so this is prompt.
- **pid 1** (§3.4) can't be killed or stopped: it is the system's shell.

### 3.3 Built-in programs and the terminal

- Built-in programs start helper threads with `Thread`, so they are part of the process. A program
  never calls `Task` (that makes kernel tasks) or `Shell::spawn()`.
- **The terminal's shell becomes its child process:** `terminal` starts `/bin/sh -i` on the pty's
  console with `ProgramLauncher::start()` (parent: `terminal`), not `Shell::spawn()`. Closing the
  window hangs up the pty, so the shell gets end of input (and `SIGHUP`, §3.6) and ends; `terminal`
  waits for it, then ends. Killing `terminal` ends both: the shell gets `SIGHUP` when its controlling
  terminal goes.
- **`sh -i`, an interactive shell as a program.** `/bin/sh` today reads a script from stdin when it
  has no arguments. With `-i`, or with no script and a terminal as stdin (as POSIX says), it runs the
  prompt, line editor and history of a session, sources `/boot/profile`, and ends at `exit` or end
  of input. `-l` (a login shell) also runs `/boot/autoexec.sh` (the serial shell's startup).

### 3.4 Shells are processes

- **pid 1 is the serial shell**, started at boot by `main` through the loader as `/bin/sh -l` on
  the serial console. `ShellPid` stops being a pretend parent: it is that process. Orphans are not
  reparented to it (they are freed when they end, as now).
- `Shell::spawn()`, `Session`, and the reaping of ended sessions go: every shell is `/bin/sh`.
- The launcher's two waits (`!fromProcess` with Ctrl-C forwarding, and the plain one) become one:
  every caller is a process; Ctrl-C reaches the foreground group from the console (§3.6).

### 3.5 What runs where in a shell

- **Built-in commands** run on the shell's thread, as now.
- **Pipeline stages** that are built-in commands run on the shell's `WorkerPool` threads: threads
  of the shell's process. Program stages are child processes, as now.
- **A background job** (`cmd &`, any command: a simple command, a pipeline, a compound command) is
  one new process, a subshell: the kernel starts a process that runs kernel code (a cloned
  `ShellState` on a new thread, as `( … )` runs today, but in its own process with its own fds and
  group). Its pipeline stages are its threads, its programs its children. A single program (`sleep
  10 &`) is started directly, without the subshell. The new process gets its own process group
  (§3.6) and is not in the foreground.
- **A foreground pipeline** of programs gets its own process group, which the shell puts in the
  console's foreground while it runs and takes back after.

### 3.6 Process groups, sessions, the foreground

As POSIX has them, for one user:
- **Process groups:** a pgid per process; `setpgid`, `getpgid` take effect. A group is what the
  shell calls a job. `BMDS_PROC_JOB` returns the pgid (it already says "its process group").
- **Sessions:** a shell started on a console makes a session (`setsid`) with that console as its
  controlling terminal; its children inherit both.
- **The foreground group** is the console's (`TIOCSPGRP` and `TIOCGPGRP` take effect; `tcsetpgrp`).
  The console sends, to its foreground group: Ctrl-C `SIGINT`, Ctrl-\ `SIGQUIT`, Ctrl-Z `SIGTSTP`,
  a size change `SIGWINCH`; when it hangs up, `SIGHUP` to its session (then `SIGCONT`). The rule
  that a Ctrl-C after an unanswered one sends `SIGKILL` (§15) moves with it. `bmds_console` sees
  only an opaque target and a hook the loader installs.
- **Background reads:** a process outside the foreground that reads its controlling terminal gets
  `SIGTTIN` (stopping it); writes are allowed (no `TOSTOP`).
- **Interactive shells** catch `SIGINT` (it interrupts the command running in the shell, as
  Ctrl-C does now), and ignore `SIGTSTP`, `SIGTTIN`, `SIGTTOU` and `SIGTERM`, as bash does.

### 3.6.1 How it is built (step 2)

- **The table** keeps a group and a session per entry. `add()` takes each as "the parent's", "a
  new one (led by the new process)" or an id to join; a new pid is never one in use as a group or
  session id. `setGroup()` and `newSession()` follow `setpgid()` and `setsid()`'s rules (the
  caller or its child, in the caller's session; a group leader can't make a session).
- **The console** holds its session and its foreground group (opaque ids, 0: none) and a pending
  signal set. Ctrl-C, Ctrl-\, a size change and a hangup add `SIGINT`, `SIGQUIT`, `SIGWINCH`,
  `SIGHUP` to the set and call a hook (ISR-safe; the USB console's Ctrl-C comes in its receive
  interrupt). Without a hook (the host tests), Ctrl-C raises the console's interrupt as before.
- **Delivery** is the loader's: its hook queues the console for a kernel task (`sig`, above the
  shells' priority, so a busy program can't hold Ctrl-C up), which takes the set and sends each
  signal to the console's foreground group (`SIGHUP`: to its session). A Ctrl-C after one that no
  process took, or that is still pending, sends `SIGKILL` to the group instead.
- **Launching:** a request may make a new session on its console (`session`: the shells the
  system starts, pid 1 and each terminal's) or name a `JobGroup`: the first program of a pipeline
  starts the group, the others join it, and with `foreground` the console gets it. Without either,
  the program is in its caller's group.
- **Giving the terminal back:** when the last process of the foreground group ends, the console's
  foreground goes to its parent's group (the shell that started it), or to the session's. When a
  session leader ends, its console has no session or foreground any more, and the foreground group
  gets `SIGHUP`.
- **Built-in programs and signals:** a built-in program can set a signal to `Interrupt` (raise its
  process's interrupt, nothing pending: the kernel-code form of catching it) or `Ignore`, through
  `ProcessControl`, which the loader passes in the program's context. Pid 1 gets only the signals
  it set an action for (as Linux's init); `kill` of pid 1 with `SIGKILL` or `SIGSTOP` is refused.
- **The interactive shell** (`/bin/sh -i`, or no script on a terminal) sets `SIGINT` to
  `Interrupt` and ignores `SIGTERM`, `SIGQUIT` (the stop signals in step 4), and starts each
  foreground pipeline of programs in a `JobGroup`. A program killed by `SIGINT` (status 130)
  interrupts the shell as Ctrl-C would, as bash does: a loop of programs stops. Pid 1 never ends:
  `exit` gives a new prompt.
- **System calls:** `proc_group(op, pid, pgid)` (get or set a group, get a session, make a
  session) and `term_foreground(fd, pgid)` are appended; `linux.c` maps `getpgid`, `setpgid`,
  `getsid`, `setsid`, `TIOCGPGRP`, `TIOCSPGRP` and `TIOCGSID` to them.

### 3.7 Stopping and continuing

- `SIGSTOP`, `SIGTSTP`, `SIGTTIN`, `SIGTTOU` stop a process by default; `SIGCONT` continues it (and
  is delivered to a handler if caught). Each thread stops at a safe point:
  - in program code: suspended at once (the handshake, with a stop flag beside the kill flag);
  - in a system call: at `leave()`, where it parks until continued;
  - in kernel code (built-ins, shells): in its next interruptible wait or interruption check, which
    parks until continued. A stopped process's blocking calls don't time out while stopped.
- The parent learns of it: `SIGCHLD`, and `proc_wait` with a new flag reporting stopped and
  continued children (`WUNTRACED`, `WCONTINUED` through musl's `waitid`). The shell prints
  `[1]+  Stopped  sleep 10`.

### 3.8 The shell's job control

- A job table per shell: number, pgid, state (running, stopped, done), the command text. `%N`,
  `%%`, `%+`, `%-`, `%string` in `fg`, `bg`, `kill`, `wait`, `disown`.
- `jobs [-l]`, `fg [%N]` (makes it the foreground group, continues it, waits), `bg [%N]` (continues
  it in the background), `wait [%N|pid]`, `kill [-SIG] %N|pid`, `disown`. `$!`.
- Done jobs are reported before the next prompt (`[1]+  Done  sleep 10`), as bash does.
- A shell that exits with stopped jobs warns once (bash's "There are stopped jobs.").

### 3.8.1 How it is built (step 3)

- **`&`** ends an and-or list as `;` does; the parser wraps the and-or in a `Background` node with
  its text.
- **A lone program** (a simple command without assignments whose name is a plain word, not a
  function or a built-in command, found as a program file or a `/bin` program the shell doesn't run
  itself) is expanded and redirected by the shell as any command is, then started by the launcher's
  `start()` with `waited`: it stays the shell's child (a zombie once it ends, until the shell takes
  it), in a new group not in the foreground. `$!` is its pid.
- **Anything else is a subshell process.** The launch request names kernel code to run instead of
  a file (`builtin`, with `data` for it): the loader starts it as a process, as it starts `/bin/sh`.
  It is a new `Shell` with a copy of the state (variables, functions, positional parameters,
  options, `$$`, the directory), which parses the job's text again and runs it, not interactively,
  and ends with its status (`ps` shows `sh -c TEXT`). Its pipeline stages are its threads, its
  programs its children, in its group: the job's. A here-document whose body follows the line is
  outside that text, so such a job is refused (a lone program with one works).
- **Every job has a group of its own**, in a non-interactive shell too (POSIX keeps the shell's
  group and ignores `SIGINT` and `SIGQUIT` there; the effect, the terminal's Ctrl-C not reaching
  the job, is the same). Its stdin is `/dev/null` until step 4, which gives it the terminal and
  `SIGTTIN`; its stdout and stderr are the command's.
- **Taking ended jobs:** `ProcessControl::waitChild(pid, block, status)` (blocking: the process's
  interrupt stops it) and `disownChild(pid)`. The shell waits only for its jobs' pids, never for
  any child: a pipeline stage's program on another thread is its own launcher call's to wait for.
  Ended jobs are taken before each prompt (an interactive shell reports them, as bash: `[1]+  Done
  sleep 10`, `Exit 2`, `Terminated`), before a new job starts, and by `jobs` and `wait`.
- **The job table** is the `ShellState`'s and isn't copied into a subshell's (`( … )`, a pipeline
  stage, a background subshell start with none). When a state with jobs goes (a `( sleep 9 & )`),
  its jobs are disowned: freed when they end, as orphans. 32 processes at most (the table's
  capacity) bounds the jobs.
- **Commands:** `jobs [-l|-p]`; `wait [%N|pid...]` (none: all jobs, status 0; else the last one's;
  127 for one that isn't a job; Ctrl-C ends it with 130); `kill %N` (the job's group); `disown
  [%N...]`. Job specs: `%N`, `%%` and `%+` (the newest), `%-` (the one before), `%name` (its
  command starts with), `%?text` (contains). An interactive shell prints `[N] pid` as a job starts.
  `$$` is the shell's pid (a subshell keeps its parent's), `$!` the newest job's.
- **`fg` and `bg`** come with stopping (step 4).

### 3.9 Seeing processes

- `ps`: pid, parent, group, state (running, sleeping, stopped, zombie, ending), threads, memory the
  process holds, its name and arguments. `ps -T` lists each thread (task name, state, free stack).
  The data comes from the table under its lock; nothing is allocated per call beyond the output.
- `kill`, as a shell command, gains pids of any process and `%N`; the kernel's `proc_kill` stops
  refusing pid 1 only where it says so (`SIGKILL`/`SIGSTOP` to pid 1 stay refused).
- Later, if wanted: `/proc/N` files. Not now.

### 3.10 Threads for programs (later)

pthreads for loaded programs: appended system calls (thread create, exit, join; a futex), a thread
record per task as above, TLS blocks per thread (musl's), `errno` per thread (it already is, through
TLS). Signals to the process go to one thread that doesn't block them. Designed when a program needs
it (a toolkit, a compiler); the model above is built for it, nothing more is done now.

## 4. ABI impact

ABI 1 has not shipped, but these are recorded as `ABI:` lines because they change behavior:
- `proc_wait` gains flags for stopped and continued children (new bits; old callers unaffected).
- `setpgid`, `setsid`, `getsid`, `TIOCSPGRP`/`TIOCGPGRP` take effect; `BMDS_PROC_JOB` is the
  pgid (as documented).
- The stop signals stop instead of being dropped; `SIGHUP` and `SIGTTIN` are sent. A program that
  relied on `SIGTSTP` being dropped behaves as on Linux instead.
- Possibly appended slots: `proc_setpgid`, `proc_setsid` (if the existing ones can't carry it).
- Shell: new `jobs`, `fg`, `bg`, `wait`, `disown`, `ps`; `&` accepted; `kill` takes `%N`; `$!`.
- `/bin/sh -i`, `-l`.
- `tasks` unchanged.

## 5. Costs

- Per process: the table entry grows by the group and session ids, the thread list head, the state
  (a few words); the `Process` as now.
- Per thread: a record in `Psram` (the task handle, its process, the flags, the hint, the vfork
  children: about what `Running` is now, which shrinks to the process part).
- Shells: `/bin/sh` as a process costs a `Process` (fds, the argument block) more than a session.
  `Session` and its reaping go.
- Background jobs: a subshell's thread (its stack, `CONFIG_BMDS_SHELL_STACK_BYTES`, in `Psram`)
  plus its cloned `ShellState`; a single program as now.
- No polling anywhere: stops and continues park on semaphores; the console signals from its input
  path.
- Each is measured as it lands (`idf.py size`, `mem` before and after).

## 6. Testing

- **Host:** the table with groups and sessions (setpgid rules, orphaned groups), thread lists
  (attach, detach, last thread ends the process), the stop/continue state machine and wait statuses
  (`WUNTRACED`, `WCONTINUED`), the console's foreground signalling (Ctrl-C, Ctrl-Z, hangup to the
  session), the shell's job table and `%` specs, `&` parsing and `$!`.
- **Board:** the yardstick in §1; `yes > /dev/null &` then `kill %1`; Ctrl-Z on a program computing,
  on one blocked in a read, on a built-in (`seq 1 1000000 | cat`), then `fg`; a background `cat`
  reading the terminal stops (`SIGTTIN`); closing a terminal window with a background job (`SIGHUP`
  ends it); killing `terminal` from the serial shell; BusyBox `hush` job control; `ps`, `ps -T`;
  `tasks` and `mem` unchanged after each.

## 7. Steps

1. **Processes own threads.** The kernel's task owner (thread-local pointer 1, opaque to
   `bmds_kernel`) and its two hooks; `Thread` joins the caller's process; a process ends once its
   threads have; `ps`, `ps -T`, and `kill` (any pid) as shell commands. `Running` keeps the main
   thread's fields and gains the thread list; the per-thread fields (the handshake, the hint, vfork)
   move into thread records when programs get threads (step 5), the only threads that need them:
   kernel-code threads share the process's interrupt and end cooperatively.
2. **Groups, sessions, the foreground; shells are processes.** Real pgids and sessions; the
   console's foreground group and its signals (Ctrl-C, Ctrl-\, `SIGWINCH`, hangup), replacing the
   launcher's Ctrl-C forwarding. Only then can the serial shell be a process: until the console
   signals its foreground group, the kernel shell's wait is what turns Ctrl-C into `SIGINT`. `/bin/sh
   -i` and `-l`; pid 1 is the serial shell, started through the loader; `Shell::spawn()` and
   `Session` go; the terminal starts `/bin/sh -i` as its child (a `Thread` of its own runs it).
3. **Jobs.** `&` (a program directly, anything else as a subshell process), the job table, `jobs`,
   `wait`, `kill %N`, `$!`, done-job reports.
4. **Stopping.** Stop signals, the stop handshake and parking, `SIGCONT`, `SIGTTIN`, `WUNTRACED`;
   Ctrl-Z, `fg`, `bg`.
5. **Program threads** (§3.10), when wanted.

## 8. Open questions

- Should a background job's built-in pipeline stages be threads of its subshell (as designed) or
  each a process (as POSIX shells fork them)? Threads cost less and nothing tells them apart except
  `ps`; recommended: threads.
- `nohup` and `disown` cover a job outliving its terminal; is a `/boot` daemon story (`autoexec`
  starting services) wanted later?

## 9. As built

- **Step 1** (2026-10-06): processes own threads.
  - `TaskOwner` (`bmds_kernel`, portable): a task's owner in FreeRTOS thread-local pointer 1 (a
    `thread_local` on the host), with `attach()`/`detach()`. `Thread::start()` attaches the new
    thread to its starter's owner (the record already holds the member: no allocation), and the
    thread detaches once its entry has returned, before `join()` can free it. `Task` never joins.
  - `Running` is the `TaskOwner` of its process: a thread list under its own lock, a count, and
    `ending`; `end()` first refuses new threads and, if a built-in program returned with threads
    running, raises the process's interrupt, logs it and waits for them. It keeps its command line
    (48 bytes, cut) for `ps`. Each `Running` grows by two 92-byte sync objects and that line, in
    `Psram`.
  - `ProcessTable::forEach()` (pid order, zombies included); `Loader::listProcesses()`,
    `listThreads()` (task name, state and free stack read under the locks that keep the tasks
    alive) and `Loader::signal()`; `Running::sendSignal()` shares its code.
  - `ps [-T]` and `kill [-s SIG | -SIG] [--] PID|-JOB... | -l` (`bmds_loader`, shell commands and
    so `/bin` applets; ours win over BusyBox's `kill` on `PATH`).
  - Host-tested: threads started by a process's task join it, threads they start too, kernel
    threads stay ownerless, an ending owner refuses a start (nothing runs); `forEach()` order and
    zombies.
  - On the board: `/bin/sh -c "sleep 30 | sleep 30"` (started through BusyBox's `env`, so a
    process) showed as pid 3 with 2 threads, `ps -T` listing `sh` and its `+pipe` worker; `kill 3`
    ended it and both tasks went; `kill -KILL 2` ended `terminal` and closed its window; `kill 1`
    "not permitted", `kill 99` "no such process". The image grew 3,360 bytes, `.bss` 12.
  - Found: `/bin/sh` typed in a shell runs in that shell (it is the `sh` built-in), not as a
    process; step 2 changes that when shells become processes. Not exercised on the board: a
    built-in program returning with its threads still running (the warning path).
- **Step 2** (2026-10-06): groups, sessions, the foreground; shells are processes.
  - `ProcessTable`: a group and a session per entry (`add()` with "the parent's", "new", or a
    group to join), `setGroup()`, `newSession()`, pids from 1 skipping ids in use as a group or
    session. `KernelParent` replaces the pretend `ShellPid` for what kernel code starts and waits
    for. `Signals` gains the `Interrupt` action; `ProcessControl` (`bmds_kernel`) lets a built-in
    program set it (and `Ignore`), see its pid and whether it is ending.
  - Consoles keep their session, foreground group and a pending signal set; Ctrl-C, Ctrl-\, a
    new size and a pty's hangup go through the hook to the loader's `sig` task (8 KB `Psram`
    stack, the shells' priority + 1, a 16-entry queue). The launcher's Ctrl-C forwarding is gone:
    `run()` has one wait, which a caught signal doesn't end.
  - Launch requests carry `session` or a `JobGroup`; `start()` without either gives a group of its
    own, not in the foreground (`win term`'s terminals don't get the serial console's Ctrl-C).
    When a foreground group's last process ends, the terminal goes to its parent's group; a
    session leader's end clears the terminal and hangs up its foreground group.
  - `/bin/sh -i` and `-l` (`Shell::interact()`); typed `sh` with a terminal is a process of its
    own; `/bin/sh` and `/bin/bash` by path (and so `#!/bin/sh`) run as processes, not in the
    shell. Pid 1 is `/bin/sh -l` started by `main` (at the shells' priority); `Shell::spawn()`,
    `Session` and `Startup` are gone. The terminal runs `/bin/sh -i` with the launcher on a
    `Thread` of its own and joins it.
  - System calls `proc_group` (51) and `term_foreground` (52); `linux.c` maps the group calls and
    `TIOCGPGRP`, `TIOCSPGRP`, `TIOCGSID`. `ps` shows PGID and SID.
  - Host-tested: the table's groups and sessions (setpgid's rules, setsid, a group outliving its
    leader), the `Interrupt` action, a pty's signals through the hook.
  - On the board: `ps` shows pid 1 (`sh -l`, session 1), `terminal` in its own group of session
    1, its `sh -i` leading session 3; Ctrl-C at the prompt, on `sleep` (a built-in), on a
    program (130), on a loop of programs (the loop stops, 130), in the serial shell and in a
    terminal; a second Ctrl-C kills a program ignoring `SIGINT` (137); Ctrl-\ ends a program
    (131) and not the shell; `busybox sleep 2 | busybox sleep 2` share one group; closing a
    terminal window with a program or a pipeline running ends all of it, as do `exit` and
    `kill -KILL`; `kill -KILL 1` is refused, `TERM` and `HUP` to pid 1 do nothing, `exit` there
    gives a new prompt. Free memory the same over terminal cycles. The image grew 3,460 bytes,
    `.bss` 232.
  - Found: BusyBox `hush` runs and takes Ctrl-C, but its job control stays off: its
    `fcntl(0, F_DUPFD, 255)` fails (`EINVAL`: fewer descriptors), so `setpgid` and `tcsetpgrp`
    from a program are host-reviewed only, not exercised on the board. (Wrong cause: BusyBox
    was linked with an SDK build from before step 2, where they did nothing; see step 3.)
- **Step 3** (2026-10-06): jobs.
  - The parser makes `&` a `Background` node (the and-or list and its text); `&` separates as `;`
    does. A lone program is started by the launcher's `start()` with `waited` (the shell's child,
    kept once it ends); anything else is a subshell process: the request names kernel code
    (`builtin`, `data`), which the loader runs as it runs `/bin/sh` (`ps`: `sh -c TEXT`), a new
    `Shell` over a copy of the state, with a pipeline worker's stack (32 KB, `Psram`). A built-in
    program is "in the kernel" from its creation, so a kill before it runs still lets it free its
    data.
  - `ProcessControl` gains `waitChild()` and `disownChild()`. `JobTable` in `ShellState` (not
    cloned; a table that goes disowns its running jobs); `jobs [-l|-p]`, `wait`, `disown`, `kill
    %N`; `$!`; `$$` is the shell's pid (it was always 1). Ended jobs are reported before the
    prompt by an interactive shell, which also says `[N] pid` as a job starts (not in its
    subshells or `sh -c`).
  - Host-tested: the corpus (`&` in lists, groups, loops; `a & ;` and `& a` are errors, as in
    bash), lone programs against subshells, `wait` statuses and its interruption, job specs,
    `disown`, a subshell's jobs disowned, here-documents.
  - On the board: `busybox sleep 5 &` (pid, `jobs -l`, `$!`), built-ins, loops, functions and a
    two-program pipeline in the background (`ps` shows the subshell with its `pipe` thread and
    both programs in its group); `wait`, `wait %N`, Ctrl-C ending a `wait` (130, the job goes on);
    Ctrl-C at the prompt and in a terminal leaves jobs alone; `kill %N` (`Terminated` reported);
    closing a terminal window ends its jobs (`SIGHUP`); a job started in `( … )` is freed when it
    ends; `sh -c '… & … wait $!'`. Free memory and tasks are the same after each round. The
    image grew 7,074 bytes, `.bss` none.
  - Found and fixed: step 2 had dropped `Loader::begin()`'s check of a failed load (it went on
    with no image). The SDK build that BusyBox was linked with predated step 2, so its `setpgid`
    and `tcsetpgrp` did nothing; rebuilt, BusyBox `hush` has job control on the board (its jobs in
    groups of their own, a pipeline's in one, Ctrl-C to the foreground job only, `kill %1`). It
    still prints `fcntl(0,F_DUPFD,255): Invalid argument` as it starts (16 descriptors here) and
    goes on with a lower one.
