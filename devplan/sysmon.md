# System Monitor: How Busy the System Is

Planned 2026-10-08, not started. The user's request: a way to tell how busy the system is. Today
there is none. `tasks` lists states and stack, `ps` the processes, and `mem` the heaps, but
nothing says where the CPU time goes. The audio engine measures only its own load. This
document designs the measurements, the ways to see them (the shell, programs, the panel and a
window), and their order.

## 1. Goal

- **CPU:**
  - how busy each core is;
  - which tasks and processes use the time;
  - how that changed over the last minutes.
- **Memory:** used, free, the largest block and the low-water mark, for each kind (internal,
  DMA-capable, PSRAM), and per process.
- **Subsystems' own counters in one place:**
  - the audio engine's load and late periods;
  - storage reads and writes;
  - later, the display's frames and the network.
- **Seen four ways, from one source:**
  - `top` in the shell;
  - a system call for programs;
  - a CPU meter in the panel's tray;
  - a System Monitor window.
- **Cheap, and free when nobody looks:**
  - The always-on cost is a counter read per context switch, and a setting turns even that off
    for a boot.
  - Sampling (history for graphs) runs only while someone watches: no polling otherwise.
- **Nothing real-time pays for it** (the user, 2026-10-08): if it costs the audio engine or the
  display measurably, it stays off by default.

## 2. Facts that shape the design (checked 2026-10-08)

- **No CPU time is collected today.**
  - `CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS` is off. `CONFIG_FREERTOS_USE_TRACE_FACILITY` is on,
    which `tasks` uses (`uxTaskGetSystemState()`: state, priority, stack).
  - With run-time stats on, FreeRTOS adds each task's running time at every context switch,
    and IDF can count it on the ESP timer (1 MHz, the default clock) or the CPU clock. Its help
    says the CPU clock "should be disabled for multi-core" because the two cores' counts differ,
    so the ESP timer it is.
  - **The counter's width:** IDF offers a 32- or 64-bit `configRUN_TIME_COUNTER_TYPE`. Its help
    says both clock sources are 32 bits (wrapping in about 4,290 s), but the port's counter is
    `xPortGetRunTimeCounterValue()`, which returns `esp_timer_get_time()` (64 bits) cast to that
    type (`portable/riscv/port.c`). With the 64-bit type it never wraps; the monitor works on
    differences anyway.
  - **Switching it per boot:** FreeRTOS's `tasks.c` calls that function at every context switch,
    and it lives in another file (`port.c`), so the linker's `--wrap` can route the call through
    BMDS: off, the wrapper returns 0 (a load and a branch); on, the timer.
- **Idle time per core:** IDF has `ulTaskGetIdleRunTimeCounterForCore()`
  (`idf_additions.h`). Busy is then 1 − idle ÷ elapsed per core, without walking every task.
- **Interrupts aren't tasks:** run-time stats charge an interrupt's time to the task it
  interrupted. The monitor can't show interrupt time apart. If that matters, ISRs that count
  their own time (as the audio engine counts its fill) can add it later.
- **The cores:** two (SMP, `CONFIG_FREERTOS_UNICORE` off), each at 360 MHz, with a 1 kHz tick.
  Tasks may be pinned or float (`tasks` shows the core, or `-`).
- **Memory:** `Memory::stats(MemSpace)` already gives the total, free, the low-water mark and
  the largest free block for each space (`MemStats`), and `bmds_mem_info` gives it to
  programs.
- **Processes:** the loader owns them (`ps`, `ps -T`). Each has its threads (FreeRTOS tasks), so
  a process's CPU is the sum of its tasks', and its memory is what it allocated
  (`_allocatedBytes`).
- **The panel** has a system tray (`mediaplayer.md` §6e: any program's `TrayIcon`, the volume
  control among them), tooltips, and `panel/items` to choose its items.

## 3. Design

### 3.1 Measuring

- **Turn run-time stats on** (ESP timer clock, 64-bit counters), and measure what they cost:
  - the context switch's extra time, on and off;
  - code size, and the 8 bytes each task's counter adds;
  - the audio engine's fill, as it is the most sensitive.
- **The setting `sysmon/enable`** (bool; the user's idea, 2026-10-08), read once at boot: on,
  the wrapped counter reads the timer; off, it returns 0. Changing it applies at the next boot,
  since turning the clock on mid-run would charge a task with all the time before. Its default
  is decided by the measurement: on, unless real-time work pays measurably.
  - **Off:** `top`, the calls and the window say that CPU time isn't measured this boot.
    Memory and the counters still work. The tray meter shows a dimmed icon, with "CPU time
    isn't measured (sysmon/enable)" as its tooltip.
- **`SystemStats` (kernel, `bmds_kernel`):** takes a snapshot of every task's run-time counter,
  each core's idle counter, the ESP timer, and memory per space. Two snapshots give:
  - each core's busy share;
  - each task's share of a core and its total time;
  - each process's (the loader adds its tasks' up through a hook, as `ps` finds them).
  - A snapshot is taken on demand: `top` and the system call take one, wait, and take another.
    Nothing runs in between.
- **History, only while watched:** a *sampler* keeps the last 120 samples (2 minutes at 1 s)
  of:
  - per-core busy;
  - memory used per space;
  - the subsystems' counters.

  It runs on a kernel timer from the first watcher (the tray meter, the window, `top`) to the
  last. A watcher registers and is told on each sample, as settings watchers are. With no
  watchers it stops, and its memory (a few KB in PSRAM) is freed.
- **Subsystem counters:** a small registry of named counters, each with a name, a unit, and a
  function that reads it. A component adds its own at init, like device files:
  - `audio.dsp0.load` (‰ of a core), `audio.dsp0.late`;
  - `storage.sd0.read` and `.written` (bytes), `.ops`. These are new counters in the block
    device layer, a pair of atomic adds a request.
  - Later: `display.frames`, `net.*`.

  The monitor shows them without knowing them, so a new subsystem needs no monitor change.
- **Stack:** `tasks`' high-water marks, shown beside CPU, so a task near its stack's end is
  seen.

### 3.2 The shell: `top`

- **`top [-d SECONDS] [-n COUNT] [-T]`:** a screen, redrawn every interval (1 s by default) until
  `q` or Ctrl+C:
  - a header: uptime; each core's busy %; memory per space as used/total, the largest block and
    the low-water mark; the process and task counts;
  - the processes sorted by CPU %: pid, name, CPU %, time, memory, threads, state (with `-T`,
    their tasks, and the kernel's own tasks under a "kernel" row);
  - the subsystem counters as a line or two (`audio dsp0 5.6%, sd0 1.2 MB/s read`).
- **Batch mode** (`-n COUNT`, or when output isn't a terminal): plain lines a script can read,
  their format frozen once shipped, like the other commands.
- **`tasks` gains a CPU column** (appended): each task's share since boot, and with `-d`,
  over an interval.
- **`ps` unchanged:** its format is frozen; `top` is where CPU goes.

### 3.3 Programs: system calls (appended)

- **`sys_stats(bmds_sys_stats_t *)`:** one copy of the system-wide view:
  - the cores, each with its busy counter and total;
  - memory per space;
  - the uptime;
  - the counts.
  A program takes two and divides, as `top` does: no sampler needed.
- **`proc_stats(int32_t pid, bmds_proc_stats_t *)`:** a process's CPU time, memory, threads and
  state; iterated by pid (`-1` from the start, as `sysconf_next` iterates).
- **`stats_counters`:** the subsystem counters by index: name, unit, value.
- **Linux's view through the SDK:**
  - `getrusage()` and `times()` from the process's own time;
  - `sysinfo()` from memory and uptime (`linux.c` makes some of these up today);
  - `/proc/stat` and `/proc/meminfo` not now. A `/proc` is its own design, if ever.

### 3.4 The panel: a CPU meter

- **A tray item:** a small graph, each core's busy share over the last minute, drawn into a
  16-pixel-high cell (two thin columns, or one combined, by a setting).
  - **Tooltip:** "CPU 12% · 9% / Memory: 3.1 of 32 MB".
  - **A click** opens the System Monitor.
- **On by default** (the user, 2026-10-08): the panel's default items gain `cpu`. It costs the
  sampler's timer once a second while it is shown.
- **Through `panel/items`:** `cpu`, like `volume`.

### 3.5 The System Monitor window

- **A built-in app, `sysmon`:** using only the public GUI API and the §3.3 calls, like the
  other built-in apps.
- **Contents:**
  - **CPU:** a graph per core over the last 2 minutes; the busy share now.
  - **Memory:** a graph per space; used, free, largest block, low-water mark.
  - **Processes:** a sortable list (CPU %, memory, threads, state) with End Process (`SIGTERM`,
    then `SIGKILL`).
  - **Subsystems:** the counters, each with a small graph.
- **Opened from** the launcher, the tray meter, and a hotkey later (`hotkeys.md`: Ctrl+Shift+Esc,
  as on Windows).

## 4. Phasing

1. **Measure the cost:**
   - run-time stats on (ESP timer, 64-bit), the counter wrapped, `sysmon/enable`;
   - the context switch (on and off), the audio fill and the size measured and recorded here,
     which decides the setting's default.
2. **`SystemStats` and `top`:**
   - the snapshot, CPU per core, task and process;
   - `top`, and `tasks`' CPU column;
   - host tests for the arithmetic (shares, a wrapping counter, tasks that came and went between
     samples).
3. **Counters:** the registry; audio's and storage's counters (new in the block device layer).
4. **The ABI:** `sys_stats`, `proc_stats`, `stats_counters`; `getrusage()`, `times()` and
   `sysinfo()` from them; a `progtest` mode.
5. **The sampler, the tray meter, and the System Monitor window.**

## 5. ABI impact

- **New system calls** (appended): `sys_stats`, `proc_stats`, `stats_counters`, with their
  structs.
- **New shell command:** `top` (its batch output frozen); `tasks` gains a column (appended).
- **Settings:**
  - `sysmon/enable` (per boot);
  - `sysmon/interval` (s, default 1);
  - the meter's graph form;
  - `panel/items`' default gains `cpu`.
- **Counter names** (`audio.dsp0.load`...) are an interface once shipped.

## 6. Decisions (the user, 2026-10-08)

- **The tray meter is on by default.**
- **The interval is 1 s,** a setting (`sysmon/interval`), with 2 minutes of history.
- **`top` first,** then the window.
- **Nothing real-time pays for it.** If the counter costs the audio engine or the display
  measurably, `sysmon/enable` defaults to off; it is a per-boot setting either way.
