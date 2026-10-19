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
  The pool keeps up to two idle workers (a 32 KB stack in PSRAM and 496 bytes internal each) for
  the next pipeline, and since 2026-10-06 frees one idle for 5 seconds (`WorkerPool::Config::
  idleTimeoutMs`): the worker takes itself off the list under the pool's lock and ends with its
  thread detached (`Thread::detach()`: FreeRTOS's timer task frees its stack and record once it
  has parked); the pool's destructor waits for a worker still leaving (`gui.md` §9).
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
  a size change `SIGWINCH`; when it hangs up, `SIGHUP` (then `SIGCONT`) to its session leader and
  its foreground group, and the shell passes it on to its jobs (§3.7.1). The rule
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

### 3.7.1 How it is built (step 4)

- **Signals.** `SIGSTOP`, `SIGTSTP`, `SIGTTIN` and `SIGTTOU` stop by default (blocked: pending
  until unblocked, then they stop at the next system call's end); ignored ones are dropped.
  `SIGCONT` always continues a stopped process, then is dropped by default (caught: pending as any
  other). A stop signal discards a pending `SIGCONT` and `SIGCONT` discards pending stop signals.
  While stopped, other signals stay pending (a terminating one acts at the continue); only
  `SIGKILL` ends a stopped process at once. `Signals` keeps the stopped state; `post()` says
  `Stop` or `Continue` for the sender to act on.
- **Parking.** `Interrupt` gains a hold: `hold()` wakes its waiters, and while held `wait()` and
  `sleep()` park (they neither return nor time out; a semaphore taken meanwhile is given back),
  then go on; `park()` waits out a hold, for code between waits. A stopped process's interrupt is
  held, so every kernel-code thread of it (a shell, its pipeline workers, a call in progress)
  parks at its next wait or check: the shell's interruption checks (`Executor::interrupted()`,
  `CommandContext::isInterrupted()`) park.
- **The handshake** (as the kill's, beside it): the sender holds the interrupt, then suspends the
  task and reads `inKernel`: in program code the task stays suspended (the stop is done); in a
  system call it is resumed and parks in its waits or at `leave()`. `SIGCONT` releases the hold
  and resumes a task suspended for the stop. Senders run under the process table's lock, which
  serializes stops, continues and kills.
- **The parent learns of it:** the table records "stopped by N" or "continued" until the parent
  takes it, releases the parent's semaphore and sends it `SIGCHLD`. `proc_wait` gains
  `BMDS_WAIT_UNTRACED` and `BMDS_WAIT_CONTINUED` (Linux's statuses: `0x7f | sig << 8`,
  `0xffff`), which `linux.c` maps from `waitid`'s `WSTOPPED` and `WCONTINUED`. `ps` shows `T`.
- **A stopped process left alone:** when a process ends, or disowns a child (a shell that exits
  disowns its jobs), its stopped children get `SIGHUP` then `SIGCONT` (Linux's orphaned-group
  rule, per process); a hangup sends `SIGCONT` after `SIGHUP`.
- **Hangups** (as Linux and bash): a terminal's hangup goes to its session leader (its shell) and
  its foreground group, not the whole session; the interactive shell, ending with its terminal
  (or by a signal), sends `SIGHUP` to its jobs (and `SIGCONT` to stopped ones), except those
  `disown -h` kept. A disowned job, one under `nohup` or one under `setsid` (BusyBox's, now
  built, with a patch for its no-MMU re-exec) outlives the terminal; after `exit` the jobs live on
  (no `huponexit`). An interactive shell takes the terminal at each prompt, as bash does, and
  when a read fails because it isn't the foreground (a job's group left empty by `setsid`).
- **Ctrl-Z** is the third signal key (`SIGTSTP` to the foreground group, input before it
  flushed), on every console, with `TermMode::signals`.
- **Background reads:** a console asks a hook (the loader's) before each read: a process of the
  console's session outside its foreground group gets `SIGTTIN` (its group), parks while stopped,
  and checks again once continued; with `SIGTTIN` ignored or blocked the read fails (`EIO`); a
  caught one interrupts it. Kernel tasks and other terminals' processes read as before. Writes
  are allowed (no `TOSTOP`); `SIGTTOU` is only ever sent by `kill`.
- **The launcher:** a request may ask to hear of a stop (`stopped`, an out pid): `run()` then
  returns 128 + the signal (bash's `$?`, 148 for Ctrl-Z) as the program stops, the terminal goes
  back to the caller's group with the caller's modes, and the program stays the caller's child
  (taken with `waitChild()`). `ProcessControl` gains `waitChild()` flags (block, stopped,
  continued), `signal()` (a process or `-group`) and `setForeground()` (the caller's terminal).
- **The shell** (job control on: an interactive shell):
  - a foreground program that stops becomes a stopped job (`^Z`, then `[1]+  Stopped  cmd`,
    status 148) and the rest of the command line is abandoned (a loop ends; bash goes on with the
    commands after a `;`, so `make; ./run` would run `./run`);
  - a foreground pipeline whose last stage is a program runs as a subshell process in a group of
    its own in the foreground, as a background job does, so the whole of it stops and continues as
    one job. One that ends in the shell's own code (a built-in, a function, a loop) runs in the
    shell as before, so lastpipe holds (`cmd | while read x` sets `x`; bash turns lastpipe off
    under job control instead), and can't be stopped: its programs ignore the stop signals. Nor
    can one with a here-document whose body follows the line (it isn't in the job's text);
  - command substitutions run without job control: their programs ignore the stop signals, as
    bash's do (their output goes to memory the shell owns);
  - `fg [%N]` gives the job the terminal, continues it and waits (until it ends or stops again);
    `bg [%N...]` continues jobs in the background (both, and `disown`, take a bare number as a
    job number, as bash: `bg 1`); `jobs` shows `Stopped` (with the signal when it
    isn't `SIGTSTP`: `Stopped(SIGTTIN)`); `kill %N` with `SIGTERM` or `SIGHUP` also continues
    the job, as bash does; ended, stopped and continued jobs are reported before the prompt;
  - `exit` with stopped jobs says "There are stopped jobs." once; a second `exit` ends the shell
    (the kernel then hangs up its stopped jobs, above); `disown -h` keeps a job in the table but
    not to be hung up with the shell;
  - with job control a background job's stdin is the terminal (bash), else `/dev/null`;
  - the interactive shell ignores `SIGTSTP`, `SIGTTIN` and `SIGTTOU`, so what runs in it (a loop
    of built-in commands, `sleep`) can't be stopped, as in bash; Ctrl-C ends it.

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

### 3.10 Threads for programs (step 5)

pthreads for loaded programs, through musl (2026-10-06, the user: "We can start the next step").
A program's threads are FreeRTOS tasks of its process, each with its own record, signal mask and
interrupt; they share everything else.

**What programs get.** musl's pthreads and C11 threads: create, join, detach, exit, mutexes,
condition variables, rwlocks, barriers, semaphores, once, keys, thread-local storage
(`_Thread_local`, so `errno` per thread), `pthread_kill` and `raise` to a thread,
`pthread_sigmask` per thread, `exit()` from any thread ending them all, `posix_spawn` from any
thread, deferred cancellation. Not now: asynchronous cancellation, `sigwait` and friends,
priority-inheritance and robust mutexes across processes, scheduling attributes (`ENOSYS`), the
`setuid` family's all-thread calls (no-ops: one user).

**System calls (appended).**
- `thread_start(const bmds_thread_t *t)` (53): a new thread of the calling process runs
  `t->entry(t->arg)` with `tp` = `t->tls`, the caller's signal mask, and a stack of
  `t->stack_size` bytes that the kernel allocates (at least `BMDS_THREAD_STACK_MIN`, 8 KB: system
  calls run on it). Its id goes to `*t->tid` before it runs; the kernel sets its signal hint word
  (`t->hint`, in the new thread's TLS) and its clear word (`t->clear`). Returns the id. When the
  entry returns, the thread ends with that status (`thread_exit`).
- `thread_exit(int32_t status, void *release)` (54): ends the calling thread. `release`, a block
  from `mem_alloc` (musl's mapping of a detached thread's TLS), is freed as it ends. Once the
  thread is gone, 0 goes to its clear word and one waiter there is woken (Linux's
  `CLONE_CHILD_CLEARTID`: musl's thread-list lock, which `pthread_join` waits for before it frees
  the thread's memory). The last thread's end ends the process with `status`, as `exit()`.
- `futex(word, op, value, timeout_ns, word2, count2)` (55): `BMDS_FUTEX_WAIT` sleeps while
  `*word == value` until woken, the timeout (negative: none), or a signal (`EINTR`), the
  comparison and the queueing atomic against wakes; `BMDS_FUTEX_WAKE` wakes up to `value`
  waiters; `BMDS_FUTEX_REQUEUE` wakes up to `value` and moves up to `count2` more to `word2`.
  Keyed by address: there is one address space.
- `thread_kill(int32_t tid, int32_t sig, uint32_t flags)` (56): `sig` (0: a check) to one thread
  of the calling process; `BMDS_THREAD_WAKE` also ends the call it is blocked in, or its next one
  (`EINTR`, once): musl's `SIGCANCEL`, a real-time signal the kernel doesn't have.
- `thread_clear(volatile int32_t *word)` (57): sets the calling thread's clear word; returns its
  id (`set_tid_address`).
- `proc_id(BMDS_PROC_THREAD)` (3): the calling thread's id (`gettid`). A process's first thread's
  id is its pid; the others' are numbered from 32768 (pids stop at 32767), below 2^30 (musl keeps
  owners' ids in a mutex's lock word with flags above).

**Thread records.** `Running` (the process: its `Process`, image, pid, command, kernel `Thread`s,
the kill) gains `ProgramThread`s, one per program task: its `Task`, id, the kill/stop handshake
flags (`inKernel`, `kill`, `suspendedForStop`), hint word, clear word, vfork children (`current`
and the stack of children: a thread vforks on its own), its `Signals::Thread`, the next record.
The first thread's is part of `Running` (a program without threads pays nothing more); the
others are `Psram` records, their TCBs and stacks the `Task`'s (internal RAM if there is room to
spare, as a program's first stack). FreeRTOS pointer 1, the task's owner, is the `ProgramThread`
for a program task and the `Running` for a kernel `Thread` of the process; both start with the
loader's `ProcessOwner` (the `Running`, the thread or null). `TaskOwner` gains `threadOwner()`:
a `Thread` started from a program task joins its process, not the thread (a built-in program's
pipeline workers).

**Signals per thread** (POSIX's): actions, the process's pending set and stopping are the
process's; the mask, a pending set for signals sent to the thread, and an `Interrupt` are each
thread's (`Signals::Thread`; the first one's, which kernel-code threads share, is in `Signals`).
A signal sent to the process is due for every thread that doesn't block it: the first such
thread (the first thread first) has its interrupt raised, and every thread it is due for has its
hint set; whichever takes it first runs the handler. One that ends the process does so unless
every thread blocks it; a stop holds every thread's interrupt, a continue releases them, a kill
raises them all. `Signals` keeps its threads' list under its own lock (a thread may be attached
while a stop is posted: attaching holds it if the process is stopped). A vfork child runs on its
parent thread with its own `Signals`, as now.

**Ending.** A kill sets every thread's `kill` and runs the handshake on each: those in program
code are ended on the spot, those in a call end as it returns; the last thread to end ends the
process (its status: the first kill's, or the last thread's exit status). Thread stacks are
kernel memory, so no kernel code ever runs on program memory: the process's memory goes as the
last thread ends, while ended threads' tasks may still wait in the stop queue. `exit()` is a kill
of the whole process with the exit status. `exec` from one thread ends the others first and
waits for them, then replaces the program, as Linux's. A thread's end, as the kill's, goes
through the stop queue, which stops its task and frees its record; the `Running` goes with its
last record.

**Shared state is locked.** `Process` gains a lock for its descriptor table (a call holds a
reference to the handle it uses, so another thread's `close` can't free it under a read), its
memory blocks and its directories; a second lock while a relative path uses the working
directory (`chdir` replaces it under that lock); console input (the line editor's state) is taken
in turns, interruptibly. `linux.c` locks its tables (directory descriptors, mappings, signal
handlers) with musl's internal lock, which costs nothing until a second thread starts; its signal
hint and vfork state are thread-local (`vfork.S` reaches them through `tp`); a vfork child gets a
copy of the handler, directory, umask and terminal-mode tables instead of changing the parent's
while its other threads run.

**musl.** `__clone` takes a thread path (`CLONE_VM|CLONE_THREAD|CLONE_SETTLS`) to
`thread_start`; `pthread_create` maps only TLS and thread-specific data (the kernel allocates
stacks) and passes the stack size; the default stack is `BMDS_DEFAULT_STACK_SIZE` (32 KB, musl's
128 KB is too much here) and there are no guard pages (no MMU). A stack given with
`pthread_attr_setstack` holds the TLS only; the thread runs on a kernel stack of that size.
`__unmapself` (a detached thread freeing its own mapping) is `thread_exit` with the mapping;
`__syscall_cp_asm` checks the cancel flag before the call (deferred cancellation, woken by
`BMDS_THREAD_WAKE`). `SYS_exit` ends the thread, `SYS_exit_group` the process; `gettid`,
`set_tid_address`, `tkill`/`tgkill` and the futex go to the new calls.

**Not changed.** Built-in programs' threads stay kernel `Thread`s sharing the process's interrupt
(§3.2). Signals are still taken between calls (programs.md §15): a thread computing without
calls takes its signals at its next call.

**Steps** (all in step 5): (a) `Signals::Thread` and the futex table, host-tested; (b) `Process`
locking; (c) `ProgramThread` records, the handshake per thread, `thread_start`/`thread_exit`/
`thread_kill`/`thread_clear`/`futex`, exec and exit with threads, `ps -T`; (d) the SDK and musl;
(e) `progtest threads` on the board.

### 3.11 Busy programs and the watchdog (2026-10-06)

**Found:** a program that never blocks (`seq 1 300000 > /dev/null`, 5 s) set off ESP-IDF's task
watchdog ("Task watchdog got triggered", CPU 0's registers printed): shells and programs run at
`CONFIG_BMDS_SHELL_PRIORITY` (2) on either core, and the watchdog expects each core's idle task
(priority 0) to run within 5 s. It only warned (no panic is configured), but compiling on the
board will do this all day.

**Decision: the task watchdog no longer checks the idle tasks** (`ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0`
and `_CPU1` off). A busy program is legitimate on a desktop system, as on Unix. Considered:
- *User code at the idle priority (0), time-sliced with the idle task:* IDF fixes
  `configIDLE_SHOULD_YIELD` at 0, so its idle task would take whole time slices (waiting for the
  next tick) beside a busy program: a share of the CPU lost for nothing.
- *A monitor suspending a busy task now and then:* a hack around the scheduler.

**What a busy program still delays** (as before; the warning was the only change): the idle
task's work, freeing the stacks of tasks that ended by deleting themselves, and the timer task's
(priority 1), which reaps detached threads (`Thread::detach()`), until it blocks or ends; on the
core it runs on only, unless two keep both busy. The task watchdog stays on (nothing subscribes
to it yet); the interrupt watchdog (interrupts held off, a stuck ISR: 300 ms) is unchanged.

### 3.12 Real-time threads (2026-10-12)

`netlat` (network.md §30i) showed what programs lack for real-time work: every program thread
runs at its starter's priority (the shell's, 2), beside everything else the shell starts, and a
sleep ends on the 1 kHz tick. The user approved (2026-10-12) Linux's scheduling calls, a band of
kernel priorities for them under the drivers', and a precise sleep for real-time threads.

**What programs get** (musl, as on Linux): `pthread_setschedparam`, `pthread_getschedparam`,
`pthread_setschedprio`, `pthread_attr_setschedpolicy`/`setschedparam`/`setinheritsched`,
`sched_setscheduler`, `sched_getscheduler`, `sched_setparam`, `sched_getparam` (the calling
thread for pid 0, as glibc: musl's are `ENOSYS` stubs, patched), `sched_get_priority_min`/`max`,
`sched_rr_get_interval`, `sched_yield` (which yielded nothing before), `sched_getaffinity` and
`sched_getcpu` (`nproc` was 1). Policies `SCHED_OTHER`, `SCHED_BATCH`, `SCHED_IDLE` (all the
normal priority) and `SCHED_FIFO`, `SCHED_RR` at 1-99, with `SCHED_RESET_ON_FORK`.

**One system call, appended:** `sched(uint32_t op, int32_t tid, bmds_sched_t *param)` (113):
`BMDS_SCHED_GET` fills `param` (policy, priority, the cores it may run on, the core it is on, the
kernel priority it runs at, how often it was throttled); `BMDS_SCHED_SET` sets the policy and
priority; `BMDS_SCHED_YIELD`. `tid` 0 is the caller; otherwise any program thread (a pid is its
process's first thread). `bmds_sched_t` starts with `size`.

**Kernel priorities.** `SCHED_FIFO` and `SCHED_RR` 1-99 map evenly onto
`CONFIG_BMDS_PROGRAM_RT_PRIORITY_LOW`..`_HIGH` (7-18): above the IP stack (6), the window
manager (5), input (4), storage (3) and the shells (2); below the real-time audio clients (19),
the audio device (20), `eth0` (21), `esp_timer` (22) and scan-out (23). So a real-time program
outranks bulk traffic and the desktop, never a driver. A thread's normal priority is its
starter's, as now. A new thread inherits its creator's policy and priority, a process its
starter's (POSIX's default), unless `SCHED_RESET_ON_FORK`. FreeRTOS time-slices equal
priorities on each tick, so `SCHED_FIFO` acts as `SCHED_RR` between threads of one priority;
`sched_rr_get_interval` says 1 ms. A thread an audio device raised (`audio_cycle`) is forgotten
there when its policy changes and raised again at its next cycle.

**Throttling, as Linux's** (`sched_rt_runtime_us` 950000 of 1000000): real-time threads may
take 95 % of each second; past it they run at their normal priority for the rest of the second,
so a runaway loop leaves the shell, the desktop and Ctrl-C 50 ms a second. A timer (`esp_timer`,
every 50 ms) adds up their CPU time only while a program thread is real-time. The budget is one
core's: two real-time threads on the two cores, each half busy, also count as 95 %. Each
throttle counts in the thread's `throttled`.

**Precise sleep.** A real-time thread's sleep (`nanosleep`, `clock_nanosleep`, `usleep`) wakes
from a hardware timer (a GPTimer at 1 MHz) instead of the tick: sleepers on the kernel's own
stack in one ordered list, the timer's alarm set for the first; its interrupt releases the
sleeper's semaphore (a signal still interrupts the sleep). The tick's wait stays behind it as
a fallback. The interrupt isn't cache-safe, so a flash write holds it off (as every other one
that isn't). Other threads' sleeps, and every other timeout, stay on the tick.

**Affinity.** ESP-IDF's FreeRTOS fixes a task's core when it is created, so
`sched_setaffinity` succeeds only with a mask that keeps both cores (`EINVAL` otherwise);
`sched_getaffinity` reports both. *Superseded by §3.12a (2026-10-15):* a program's first thread
is pinned at its start, so it reports that one core. Its own core or all the cores succeed; the
other core is `EINVAL`. `sysconf(_SC_NPROCESSORS_*)` (and `nproc`) still say 2: musl counted the
affinity and said 1 until 2026-10-19, which went unnoticed because the board's `progtest` was
older than the pinning.

**Costs.** Per program thread: 12 bytes more in its record. The timer: one GPTimer (its driver
3.7 KB of flash, 156 bytes of IRAM), its interrupt, taken at a thread's first real-time policy;
the budget's: a 16-entry table and an `esp_timer`. No polling while no thread is real-time.

**Tests.** Host: the priority mapping and the budget (`tests/host/realtime.cpp`). Board:
`progtest sched` (policies set and read back on itself and another thread, inheritance,
`SCHED_RESET_ON_FORK`, a busy real-time thread throttled while the shell still answers, the
precise sleep's lateness), then `netlat -r` repeating the done-when test (network.md §30).

### 3.12a Which core a thread runs on (2026-10-15)

ESP-IDF pins a task to the core it runs on the first time it uses the FPU or PIE (their
registers are saved lazily, so the task's state lives in that core's), and nothing moves it after.
So every program thread ended up wherever it happened to be at its first float: Doom's game and
music threads on one core with the display's conversion one run, spread the next (`games.md` §23).
The user approved (2026-10-15) a way to choose:

- **The loader pins a program's first task** at creation to the core less busy over the last
  window of a second or more before the start (from the idle tasks' run time; shorter gaps reuse
  the last estimate, since every shell command is a program), ties (within 15 points) to core 1,
  where the display's work is; core 0 has the window manager, audio, USB and the network.
- **`BMDS_SCHED_PIN`** (op 3 of slot 113, `bmds_sched_t::cores`, appended): the calling thread
  keeps to one core, moved there if it runs on the other (`Thread::pinCurrent()`: the port's own
  `vPortTaskPinToCore()`, then yields until the other core takes it); only while it isn't pinned
  yet, which is safe because none of its state is in a core's registers then. Pinned already:
  only its own core succeeds. All the cores: a no-op. musl's `sched_setaffinity` and
  `pthread_setaffinity_np` on the calling thread use it.
- **Display programs (2026-10-18, the user's decision):** a program whose note has
  `BMDS_PROGRAM_DISPLAY` (bit 0 of `flags`, appended) always starts on core 0, away from the
  display's presenters and scanout on core 1, whatever the load: a game sharing core 1 waits on
  its own frames' conversion (Doom 44 fps there, 53-58 on core 0; `bench.md` §16-17).
- **libbmdsgame** puts its mixer and music threads on the core its game's thread isn't on, each
  pinning itself first thing in an entry that uses no floats (a body using floats can save float
  registers in its prologue, which would pin it first).
- **Checked:** a thread moved to the other core 50 times in 50; one pinned by a float refused to
  move. Doom: the game's thread on core 1, music and mixer on core 0, about 45% and 64% in four
  runs out of four (before: 64% and 36%, or with everything on core 1 at times), 0 late periods.

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
- Step 5: slots 53-57 (`thread_start`, `thread_exit`, `futex`, `thread_kill`, `thread_clear`),
  `bmds_thread_t`, `BMDS_PROC_THREAD`; the signal mask is per thread (one thread: unchanged);
  `SYS_exit` from musl ends the thread (it ended the program, the same while there is one).
- Real-time threads (§3.12): slot 113 (`sched`), `bmds_sched_t`, `BMDS_SCHED_*`; musl's
  `sched_setscheduler` family works (it was `ENOSYS`), `sched_yield` yields; `sleep` (24) is
  precise for a real-time thread (it was always rounded up to the tick).
- Which core (§3.12a, 2026-10-15): `BMDS_SCHED_PIN`, op 3 of slot 113, appended;
  `sched_setaffinity` on the calling thread keeps it to one core (it was always `EINVAL`).
- Display programs (§3.12a, 2026-10-18): `BMDS_PROGRAM_DISPLAY`, bit 0 of the program note's
  `flags` (all bits were refused before; others still are).

## 5. Costs

- Per process: the table entry grows by the group and session ids, the thread list head, the state
  (a few words); the `Process` as now.
- Per thread: a record in `Psram` (the task handle, its process, the flags, the hint, the vfork
  children: about what `Running` is now, which shrinks to the process part).
- Shells: `/bin/sh` as a process costs a `Process` (fds, the argument block) more than a session.
  `Session` and its reaping go.
- Background jobs: a subshell's thread (its stack, `CONFIG_BMDS_SHELL_STACK_BYTES`, in `Psram`)
  plus its cloned `ShellState`; a single program as now.
- Program threads (step 5): a `ProgramThread` record (`Psram`) and a `Task` (TCB internal, the
  stack where the first thread's would go) per thread beyond the first, and musl's TLS mapping
  (a page, `Psram`) in the program's memory. Per process: three locks (`Signals`' thread list,
  `Process`'s table and path locks), about 280 bytes of `Psram`.
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
- **Step 5, host:** per-thread masks and pending sets (a process signal to the thread that
  doesn't block it, one blocked everywhere stays pending, a thread's own signal, a stop holding a
  thread attached late), the futex table (a changed word, wakes and counts, requeue, timeouts,
  interrupts, stops).
- **Step 5, board:** `progtest threads`: threads counting under a mutex, a condition variable
  ping-pong, join and detach (memory back after), TLS and `errno` apart, `pthread_kill` to a
  thread with a handler, a signal to the process taken by the thread that doesn't block it,
  `exit()` from a thread, `posix_spawn` from a thread, a thread blocked in a read cancelled;
  Ctrl-C, Ctrl-Z and `fg`, and `kill` on a program whose threads compute, sleep and wait; `ps -T`;
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
5. **Program threads** (§3.10): pthreads for loaded programs.

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
- **Step 4** (2026-10-06): stopping and continuing.
  - `Interrupt` gains a hold (`hold()`, `resume()`, `park()`): while held its waits park, neither
    returning nor timing out. `Signals`: the stop signals stop (`Posted::Stop`), `SIGCONT`
    continues (`Continue`), other signals wait for the continue, `SIGKILL` continues to end;
    `stopSignal()` for one blocked until a call unblocks it. The continue raises the interrupt
    for what came meanwhile before it releases the hold (else a woken reader read again first).
  - The loader: the stop handshake beside the kill's (`suspendedForStop`), `leave()` parks,
    stops and continues recorded in the table after the senders' lock and reported to the parent
    (its semaphore, `SIGCHLD`); `proc_wait` and `waitChild()` flags; `ps` state `T`. A stopped
    child is hung up and continued when its parent ends or disowns it. Consoles: Ctrl-Z, and the
    read hook (`SIGTTIN`). `run()` returns on a stop when asked (`Request::stopped`), giving the
    terminal back; `Request::ignoreStops`.
  - The shell: stopped foreground programs and pipelines (those ending in a program run as a
    subshell process) become jobs; `fg`, `bg`; `jobs` shows `Stopped`; changes are reported
    before the prompt; "There are stopped jobs."; `kill %N` continues a stopped job after `TERM`
    or `HUP`; `disown` warns of a stopped job; command substitutions can't stop; background jobs
    read the terminal with job control; the interactive shell ignores the stop signals; the
    shell's interruption checks park. `linux.c` maps `WSTOPPED` and `WCONTINUED`.
  - Host-tested: holds (parking, deadlines and raises while held), the stop/continue rules,
    the table's reports, the shell's stopped jobs, `fg`, `bg`, pipelines as jobs and lastpipe,
    substitutions, the reports and the exit warning.
  - On the board: Ctrl-Z on `busybox sleep` (`$?` 148, `ps` T), `bg`, `kill -STOP %1`
    (`Stopped(SIGSTOP)`), `fg`; Ctrl-Z on a two-program pipeline and on a built-in loop piped to
    a program (the subshell, its pipe thread and its programs all stopped), `fg`, Ctrl-C (`^C`);
    a loop of programs ends at Ctrl-Z; `busybox cat &` and `read x &` stop with `SIGTTIN`, `fg`
    then reads typed lines; Ctrl-Z on a foreground `busybox cat` lets the shell read, `fg` gives
    it the terminal back; `kill %N` ends stopped jobs; a nested `sh` warns once on `exit`, then
    its stopped job is hung up and its running one goes on; BusyBox `hush`: Ctrl-Z, `jobs`,
    `bg`, `fg`, and its stopped job is hung up when it exits. The tasks are the same after rounds of
    stop, continue and kill, and internal RAM; PSRAM is 212 or 220 bytes lower after the first
    rounds (two runs), then unchanged over six more. The image grew 5,902 bytes, `.bss` 76 (6,322 and 76 with the
    hangup work below).
  - Afterwards (asked for): hangups to the leader and the foreground group, the shell hanging
    up its jobs, `disown -h`, BusyBox `setsid` (patched: upstream re-execs on no-MMU with the
    arguments after the options, so the child ran the command without `setsid()`). On the board:
    in a nested `sh`, `kill -HUP $$` ends a plain job and leaves a `disown -h` one and a `setsid`
    one (its own session) running. Found there: a job whose group was left empty (`setsid` typed
    at the prompt: its parent ended before the child left the group) kept the terminal, so the
    serial shell's reads failed (`EIO`, it ignores `SIGTTIN`) and its loop spun on CPU 0; the
    launcher now gives the terminal back once the group is empty, and the shell takes it at each
    prompt and on such a failure.
  - Found: `while :; do :; done` typed alone runs in the interactive shell, which ignores
    Ctrl-Z (as bash); it held CPU 0 until Ctrl-C (the task watchdog complained). Not tried on
    the board: Ctrl-Z typed in a terminal window (the pty uses the same byte check).
- **Step 5** (2026-10-06): program threads (§3.10).
  - `Signals`: `Signals::Thread` (mask, a pending set of its own, interrupt) for each program
    thread, the first one's inside it; `attach()`/`detach()` (a late one held if stopped, raised if
    ending), `postTo()`, `wake()`, `current()`; the mask blocked by every thread kept as one word,
    so the hot paths (`prepare()`, `leave()`) take no lock. `FutexTable` (wait, wake, requeue; each
    waiter's record on its stack). `Process`: a lock for the descriptors, memory blocks and
    directories (calls hold a `FileHandle::Ptr`, `FdTable::ref()`), one for the working directory
    during path calls, console input in turns (interruptible).
  - The loader: `Running` (the process) and `ProgramThread` (each program task, the first inside
    the `Running`), both `ProcessOwner`s (pointer 1); `TaskOwner::threadOwner()`. The handshake,
    stops, hints and vfork children per thread; the last thread to end ends the process; ended
    threads go through the stop queue, which frees each record, the `Running` with the last.
    Kernel-allocated thread stacks (internal RAM if there is room to spare). Slots 53-57,
    `BMDS_PROC_THREAD`; `ps -T` gains a TID column.
  - The SDK: `linux.c`'s tables (directory fds, O_NONBLOCK and VMIN/VTIME, handlers, umask) under
    musl's lock, a vfork child working on a copy; each thread's hint word and vfork depth in TLS
    (16 bytes: musl's built-in TLS still holds the first thread's), the vfork registers in the
    heap block; `__clone`'s thread path, `__unmapself`, the futex, `tkill` (SIGCANCEL wakes),
    `SYS_exit` per thread, `set_tid_address` registering the first thread's hint once musl has set
    `tp` up; `crt/syscall_cp.S` for cancellation points. musl: `pthread_create` maps only TLS,
    thread data and start arguments and passes the stack size; 32 KB default stacks, no guards.
  - Host-tested: per-thread masks and pending sets, a process signal to the thread that doesn't
    block it, blocked everywhere, sent to a thread, stops holding late threads, the first thread
    gone, `wake()`; the futex table (values, order, requeue, timeouts, interrupts, holds).
  - On the board: `progtest threads` passes (four threads counting to 40,000 under a mutex,
    distinct ids, TLS and `errno`; a condition-variable ping-pong of 1,000 rounds; detached
    threads' memory back; 4 KB and 128 KB stacks; a barrier; `pthread_kill` handled on its thread;
    a process signal taken by the thread not blocking it; `posix_spawn` from a thread; a thread
    blocked in a read cancelled; `exit()` from a thread, the first thread ending first, `exec`
    from a thread, `SIGTERM` and `SIGKILL` to a process whose threads compute, sleep and wait),
    three runs leaving free memory and the task count unchanged; `progtest signals`, `spawn` and
    `poll` still pass. `threadspin`: Ctrl-Z stops all four threads (the computing one suspended,
    the others parked), `bg`, `fg`, Ctrl-C (130), `kill %1`; `ps -T` lists them with their ids.
    The image grew 7,448 bytes, `.bss` 96.
  - Found: `exit()` marked the other threads killed without raising their interrupts, so one in
    a futex wait kept the process alive (fixed: every kill raises them). Per-thread vfork
    registers in TLS made every program's TLS 224 bytes, past musl's built-in area (a 4 KB
    mapping per program); they moved to the vfork's heap block. `progtest signals` expected
    `kill(1, 0)` to fail, stale since step 2 made pid 1 a process (root may signal it; `SIGKILL`
    is refused).
  - Not done: asynchronous cancellation, `sigwait`, scheduling attributes, `sched_yield` (a
    no-op). Not tried on the board: `exec` from a thread after the first one ended, a kill while
    a thread runs a vfork child.
- **Real-time threads** (§3.12, 2026-10-12):
  - `sched` (slot 113): `RealTimeThreads` (the loader) keeps each thread's policy, priority,
    `SCHED_RESET_ON_FORK` and normal priority in its record, counts real-time threads (at most
    16) once they run, and puts their tasks at `RealTimeBudget::kernelPriority()` (portable,
    host-tested with the budget). A thread or process inherits at its start; an exec in place
    keeps all of it. Another thread is looked up under the process table's and its process's
    locks, so its task is there; one not yet running is only recorded.
  - `WakeTimer` (`bmds_kernel`): a GPTimer at 1 MHz, sleepers in a list on their stacks, the
    alarm releasing their semaphores from its interrupt; a sleep within 20 µs is spun out; the
    tick's wait stays behind. Taken at the first real-time policy, its interrupt on core 1
    (network.md §30k). The host's sleeps on the tick.
  - The SDK: `linux.c`'s `sched_*`, `sched_getaffinity` (`nproc` 2, was 1), `getcpu`,
    `sched_yield`; musl's four `ENOSYS` stubs make their calls (`sdk/libc/patches/bmds.diff`).
  - On the board, `progtest sched` passes: the calls and their errors, FIFO 50 at kernel
    priority 12, inheritance, `SCHED_RESET_ON_FORK`, explicit thread attributes; 1 ms sleeps 29-32
    µs late at the median, 63-207 µs at worst (the tick's 283-362 and 683-1233); two busy
    `SCHED_FIFO` threads throttled twice in 2.5 s, the normal-priority thread beside them waiting
    at most 456 ms. `progtest threads`, `timers`, `poll`, `rtnet`, `spawn` and `signals` still
    pass. `netlat -r`: network.md §30j. The image grew 11,168 bytes.
  - Found: `progtest rt` (audio) fails now and then with late periods, more often on the last
    commit's firmware (5 of 5 there, 2 of 5 here): not this change; not yet looked into. `progtest signals`
    run by `ssh` without `setsid` ends the session (it signals its own process group).
