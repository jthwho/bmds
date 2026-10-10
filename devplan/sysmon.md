# System Monitor: How the System Is Behaving

Planned 2026-10-08, revised 2026-10-09 (§8); steps 1 and 2 done 2026-10-09. The user's request:
a way to tell how busy the system is, then (2026-10-09) "a fairly technical tool w/ all the
details (down to all the information we get from the audio and video subsystems). It should
highlight anything abnormal."

Today `tasks` lists states and stack, `ps` the processes, `mem` the heaps, and `audio status`,
`display stats`, `win stats`, `midi status` and `usb stats` each print their own counters. Nothing
says where the CPU time goes, nothing puts the counters in one place, programs can read only the
audio and MIDI ones, and nothing says which numbers are bad. This document designs the
measurements, one way to read them all, the rules that call a number abnormal, and the ways to see
them (a window, the panel's tray, the shell, programs).

## 1. Goal

- **CPU:**
  - how busy each core is;
  - which tasks and processes use the time;
  - how that changed over the last minutes.
- **Memory:** used, free, the largest block and the low-water mark, for each kind (internal,
  DMA-capable, PSRAM, TCM), the caches, and per process.
- **Every subsystem's own counters, all of them, in one place:**
  - audio: the device, its clock, its streams, the synths, MIDI;
  - display: the scanout, the surface, the compositor;
  - storage, USB, input, the console; later the network.
- **Anything abnormal highlighted:** each counter says what normal is; the monitor colours what
  isn't, and keeps a log of problems so a one-off glitch (a single late audio period) is seen even
  though a 1-second graph would hide it.
- **Seen four ways, from one source:**
  - a System Monitor window;
  - a CPU meter in the panel's tray that turns amber or red on a problem;
  - `top` and `stats` in the shell;
  - system calls for programs.
- **Cheap, and free when nobody looks:**
  - The always-on cost is a counter read per context switch, and a setting turns even that off
    for a boot.
  - Sampling (history for graphs, the rules) runs only while someone watches: no polling
    otherwise.
- **Nothing real-time pays for it** (the user, 2026-10-08): if it costs the audio engine or the
  display measurably, it stays off by default.

## 2. Facts that shape the design (checked 2026-10-08 and 2026-10-09)

### 2.1 CPU time

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
  interrupted. The scanout's ISR already times itself (`Scanout::Stats::isrCyclesMax`); §3.2
  adds totals for it and the audio DMA's, the two busiest.
- **The cores:** two (SMP, `CONFIG_FREERTOS_UNICORE` off), each at 360 MHz, with a 1 kHz tick.
  Tasks may be pinned or float (`tasks` shows the core, or `-`).

### 2.2 What the subsystems count today

All kernel-side; programs reach only the audio and MIDI ones (`audio_status`, `audio_cycle`,
`midi_status`, `audio_synth_info`, `audio_decode_info`) and their own memory (`mem_info`).

- **Audio device** (`AudioDevice::Status`, reset at each start): state, rate, period, MCLK asked
  and set, APLL, the measured rate (milli-Hz, over a window), volume, muted, held, streams,
  real-time clients, exclusive; since start: periods, late periods (each played silence), fill
  time last and largest, frames the limiter reduced, periods mixed and their total time (the
  load), system sounds dropped, time waited for real-time clients. Per stream a line
  (`describe()`): queued, underruns, or real-time late/skipped/throttled.
- **Codec** (`AudioCodec::Fault`): the last fault's error, register, values written and read.
- **Synth** (`Synth::Stats`, `SynthFile::Info`): voices now, largest and peak, notes at the
  voice limit, events queued, queue full, ignored, render CPU ‰ now and largest, the font's size.
- **MIDI** (`MidiRouter::Counters`, per port): events in and out, dropped, parse errors, device
  errors, the last event's time.
- **Scanout** (`Scanout::Stats`, since start): frames begun, blocks, swaps, stalls (an invalid
  descriptor), underruns (the DSI bridge ran dry), resyncs, rebuilds, failures, the ISR's
  largest cycle count; the pixel clock, the bridge FIFO.
- **Surface** (`Surface::Stats`): presents, shown presents, forwards (rectangles copied after a
  swap) and their pixels and time, time drawing waited for a swap.
- **Compositor** (`WindowManager::Stats`): rounds, damage pieces, redraws, 2D-DMA calls,
  fallbacks, errors, round time total, largest and last, redraws deferred; the window list.
- **Memory** (`MemStats`): per space total, free, largest block, low-water mark; the caches
  (`Memory::Reclaimer::Usage`: programs, SoundFonts) with hits, misses and reclaims; per process
  the bytes and blocks outstanding.
- **USB** (`UsbHost::Stats`): devices added and gone, HID reports, keyboards, mice.
- **Console:** `UsbSerialConsole::rxDroppedBytes()`, counted but shown nowhere.
- **Storage:** card facts (`SdCard::Info`) and `statfs`, but **no I/O counters** anywhere in the
  block device layer, the sector cache or the flash queue.
- **Input:** events without a sink are dropped uncounted.
- **Clock:** `ClockStatus` (source, slew remaining, frequency ppb).

### 2.3 What the GUI and settings offer

- **Built-in apps use only public APIs** (the prime directive), so the window reads everything
  through system calls, never `WindowManager::stats()` or `AudioDevice` directly.
- **Widgets:** `TabWidget`, `DetailsView` (sortable columns), `TreeView`, `ProgressBar`,
  `StatusBar`, `Splitter`, `TrayIcon` (with its own 16 × 16 pixels). The `Painter` fills,
  draws text, cells and images; **it has no lines**, so graphs are columns of filled rects.
- **Timers:** `GuiApplication::startTimer()` (16 a program).
- **Settings:** sysconf holds 256 declared parameters in all, so a key per counter is out; a
  string may be 1,024 bytes.

## 3. Design

### 3.1 Measuring CPU

- **Turn run-time stats on** (ESP timer clock, 64-bit counters), and measure what they cost:
  - the context switch's extra time, on and off;
  - code size, and the 8 bytes each task's counter adds;
  - the audio engine's fill, as it is the most sensitive.
- **The setting `sysmon/enable`** (bool; the user's idea, 2026-10-08), read once at boot: on,
  the wrapped counter reads the timer; off, it returns 0. Changing it applies at the next boot,
  since turning the clock on mid-run would charge a task with all the time before. Its default
  is on: the measurement below found no real-time cost.
  - **Off:** the CPU counters read as unavailable; `top`, the calls and the window say that CPU
    time isn't measured this boot. Memory and the other counters still work.
- **Measured (2026-10-09, step 1):** a temporary `switchbench` bounced a task notification
  between two tasks pinned to one core at priority 18, 100,000 rounds; an MP3 (44.1 kHz, resampled
  to 48 kHz) played for 30 s with the panel running.

  | Build | Switch, core 1 | Switch, core 0 | Audio load | Fill, largest | Late |
  |---|---|---|---|---|---|
  | A: run-time stats compiled out | 7.79 µs | 8.08 µs | 309 µs (6.1 %) | 965 µs | 0 |
  | B: compiled in, `sysmon/enable` on | 8.73 µs | 8.97 µs | 313 µs (6.2 %) | 867 µs | 0 |
  | B: compiled in, `sysmon/enable` off | 7.97 µs | 8.23 µs | 322 µs (6.4 %) | 892 µs | 0 |

  - **A context switch costs about 0.9 µs more** while counting (a read of the ESP timer's
    64-bit count at each switch), and 0.17 µs with the setting off (the rest of FreeRTOS's
    bookkeeping). At a few thousand switches a second that is well under 1 % of a core.
  - **The audio engine pays nothing measurable:** its load moved 309 → 313 → 322 µs, inside the
    run-to-run spread (the "off" run was the slowest), with no late periods.
  - **Size:** 178 bytes of internal RAM (the 8-byte counter in each task's TCB and the wrapper,
    which is in IRAM), 14 bytes of flash.
  - **So `sysmon/enable` defaults to on.**
  - **If it ever matters:** the core's own cycle counter is cheaper to read than the ESP timer,
    but it is 32 bits (wrapping in 12 s at 360 MHz, which a task running alone that long would
    break) and differs between the cores.
- **Per core:** each core's busy time, from its idle task's counter and the timer, is a counter
  (`cpu.core0.busy_us`) like any other (§3.2), so graphs and rules need nothing special.
- **Per task and process:** each task's run-time counter, with its stack's high-water mark,
  through `task_list` (§3.6); a process's time is its tasks' summed (the loader finds them, as
  `ps -T` does), plus the time of its tasks already ended (kept in the process when one ends).

### 3.2 Counters: one registry of named, self-described values

- **A counter** has a dotted name, a unit, a kind and a rule, and a function that reads it
  (`Stat::Desc`, `bmds_core/stat.h`).
  - **Kinds:** `Total` (running, only grows; a restart may reset it: shown as a total and a
    rate), `Level` (a value now), `Peak` (a running largest), `State` (a number with labels,
    `stopped|running|faulted`), `Info` (a fact: a rate, a size).
  - **Units:** count, bytes, µs, ‰, Hz, mHz, ppm, frames, cycles, dB, ns, ppb.
  - **Flags:** `KeepHistory` (the sampler graphs it), `Adjusted` (its group sets its levels from
    what it knows: a period's length, a space's total).
  - **Not measured now** (CPU time off this boot, a clock not yet measured, a device gone): the
    value `Stat::Unavailable`, never judged.
- **Registered by their components** (`Stats::Group`, `bmds_kernel/stats.h`): a `constexpr`
  table of descriptions (rodata), a name (`audio.dsp0`, at most 23 bytes) and one function that
  reads them all at once, from one snapshot. A group is a few words embedded in its component
  (about 56 bytes; some live in internal RAM for their ISRs); what the registry keeps for it
  (each counter's judge, its history) is allocated in PSRAM when it is first sampled, so nothing
  is spent until someone watches. Instances that come and go (synths, MIDI ports, block devices,
  caches) register and unregister with their object, and the registry's generation number
  changes so a reader walking the list starts again.
- **Ids:** a group's serial number (16 bits) and the counter's index (8 bits); a group added
  again gets a new serial, so an id never names another counter.
- **Read under the component's own locks,** from a reader's call or the sampler, never from a hot
  path: the counters stay the atomics their components already keep. **Lock order:** the
  registry's lock is held while a group's read runs, so a component never adds or removes a
  group while holding a lock its read takes.
- **New counters** where there were none (an atomic add each, off the hot paths), as built:
  - **Block devices:** reads, writes, bytes each way, errors (a medium removed not counted),
    time in requests, and erases, in `BlockDevice::read()`/`write()` (whole devices and
    partitions both: `storage.sd0` and `storage.sd0p0`) and in the flash's own calls, which
    littlefs uses (`storage.flash0`). A program queued to the flash counts as written when
    queued.
  - **Input:** keys, pointer events, and events with nowhere to go (no window manager).
  - **The audio device's streams' problems,** since boot: underruns, real-time clients' late,
    skipped and throttled periods (a tally each stream adds to its device's).
  - **ISR time:** the scanout's block interrupt adds its cycles to a 32-bit total, widened by
    the reader (`display0.scanout.isr_us`). The audio DMA's interrupt only queues a buffer and
    wakes the engine, too little to count.
  - **The console:** the USB serial's dropped bytes, counted already, registered.
- **The names** are an interface once shipped (append only); §6 lists them.
- **Not yet:** filesystems' size and free space (`fs.*`): FAT's free count can scan the whole
  FAT, too slow for a sample under the registry's lock. The window asks `statfs` itself.

### 3.3 Abnormal: rules from the source, overrides in settings

As built (step 2): `Stat::Judge`, `Stat::ProblemLog` and `Stat::Overrides` in `bmds_core`,
host-tested.

- **Each counter brings its rule** (the component knows what normal is):
  - **Zero:** any increase is a problem, a warning or a critical one as the counter says (late
    audio periods, underruns, scanout stalls, a full synth queue, MIDI parse errors, storage
    errors). A problem for `hold` seconds after the last increase (default 60), then normal
    again; the episode stays in the log.
  - **Above / Below:** a warning and a critical level, out of normal for `hold` seconds (so a
    one-sample spike isn't a problem), counted from the start of the interval in which it went
    out; once out, it follows the value at once (critical to warning, back to normal). A
    `Total` is judged by its rate a second: a core busy over 90 % (900,000 µs a second) for 5 s
    warns, over 98 % is critical. One hold serves both levels.
  - **A `Peak` above a level** is an event, like `Zero`: it rose past the level at some moment
    (the audio fill's largest past half a period warns, past 0.8 is critical; a compositor round
    past two frames warns).
  - **Drift:** a magnitude either side of zero (the audio clock's error, in ppb).
  - **State:** a state's bit in the warning or critical mask (`faulted` is critical).
  - **None:** shown, never judged.
  - A group may set levels from what it knows (`Adjusted`: a period's length, a frame's, a
    space's total), so the rules are absolute values when read.
- **Health:** Normal, Warning (amber), Critical (red).
- **Overrides** in one setting, `sysmon/limits` (string, ≤ 1,024 bytes, followed live):
  `PATTERN:SETTING[,SETTING];...`. A pattern is a counter's name with `*` matching anything
  (`audio.*.late`, `*.underruns`); a setting is `off`, `warn` or `crit` (any increase is that
  problem), `rule=NAME`, `warn=N`, `crit=N` (`none`: unused) or `hold=S`. The last matching
  entry wins. Entries not understood are skipped and logged ("sysmon/limits: 1 entry not
  understood").
- **Stacks** are judged by whoever lists the tasks (§3.6: the window, `top`): under 512 bytes
  free warns, under 256 is critical. Not by the sampler, since reading every stack's high-water
  mark means scanning it.
- **The problem log:** episodes, not events. When a counter goes out of normal an episode opens
  (when its problem began, the counter, its health, the value); it counts the further increases
  (an event rule) or the samples out of normal (a level rule), takes the worst health, and closes
  when the counter is normal again. The registry keeps the last 64 (PSRAM, allocated with the
  first), each change numbered, so a reader asks for the changes after the last it saw. A run of
  late periods is one entry, not hundreds.
  - **Found on the first look:** a `Zero` counter already above zero, or a `Peak` already past
    a level, is logged as a closed episode that happened "before" anyone watched. It is news,
    not an alarm: it doesn't turn the tray meter amber.
  - **Seen:** a viewer marks the changes it has shown as seen; the tray meter's colour is the
    worst of the open episodes and those not yet seen.
- **When rules run:** at each sample while anyone watches (§3.4). When the last watcher goes,
  the open episodes close and the rules pause, keeping each counter's last value: at the next
  look an event counter's increase meanwhile is a problem then, and no rate is taken over the
  gap. Nothing runs to look for problems nobody is watching for.

### 3.4 The sampler: history while watched

- **Runs from the first watcher to the last:** a thread (`stats`, priority 3, a 4 KB stack in
  PSRAM) waits `sysmon/interval` (1 s by default, 100 ms to 60 s) on a semaphore, reads every
  group, runs the rules and keeps history; the last `unwatch()` wakes it to end. Programs will
  watch with `stats_control` (§3.6); `stats -w` does from the shell.
- **History:** the last 120 samples of each counter flagged `KeepHistory`: a `Total`'s increase
  each interval, else its value, 32 bits each, kept per group (about 30 such counters now:
  under 15 KB), freed when the last watcher goes.
- **Memory while added but never watched:** none beyond the group itself.

### 3.5 Seeing it

#### The System Monitor window (`sysmon`)

- **A built-in app** using only the public GUI API and §3.6's calls, refreshed by a 1-second
  timer only while it is shown (minimized or hidden: stopped).
- **Problems are shown in place:** a value out of normal is drawn amber or red with a mark, its
  row and its tab's title too, so a problem on the Audio tab shows from any tab.
- **Tabs:**
  - **Overview:** a graph per core; memory per space as bars; the headline numbers (audio load
    and late periods, frames a second, the compositor's round time, storage throughput); the
    **Problems** list (open episodes first, then past ones, with times, counts and the value),
    with Clear (forgets past ones, as seen).
  - **CPU:** the cores' graphs and interrupt time; the tasks (name, process, core, priority,
    state, CPU %, CPU time, stack free), sortable.
  - **Processes:** pid, parent, name, state, CPU %, CPU time, memory, threads; End Process
    (`SIGTERM`, then `SIGKILL`).
  - **Memory:** per space total, used, free, largest block, low-water mark, a graph; the caches
    (size, items, hit rate, reclaims); memory per process.
  - **Audio:** per device its state, rate, period and latency; the clock (MCLK asked and set,
    APLL, measured rate and ppm); load and fill graphs, fill largest, late periods, limiter,
    real-time clients and their waits; the codec's fault; the streams (each with its counters);
    the synths (voices, peaks, notes at limit, queue, CPU); the MIDI ports.
  - **Display:** the mode and pixel clock; frames a second; the scanout's problem counters
    (underruns, stalls, resyncs, rebuilds, failures), ISR time; the surface (presents, shown,
    forwards, waits); the compositor (rounds, pieces, redraws, DMA calls, fallbacks, errors,
    round time graph, deferred); the windows.
  - **I/O:** each block device's throughput and operations, busy %, errors; the filesystems
    (size, free); USB devices and HID; input and console drops.
  - **Counters:** every counter as a tree (name, value, rate, unit, rule, level), for anything
    the other tabs don't lay out.
- **A `Graph` widget** in `bmds_gui` (public, reusable): samples drawn as 1-pixel columns (no
  line primitive needed), one or more series, an optional warning and critical line drawn
  across, the newest at the right.
- **Opened from** the launcher, the tray meter, and later a hotkey (`hotkeys.md`:
  Ctrl+Shift+Esc). A second start raises the window (a `sysmon` service, as `mplayer` has).

#### Step 4 as built (2026-10-10)

- **`components/bmds_sysmon`,** `/bin/sysmon`, an ordinary GUI program on the public toolkit and
  the kernel's public classes, as `mplayer` is (built in only to have it at hand):
  - **`Source`:** every read the window makes, one function per §3.6 call (counters listed and
    read, history, problems, watching, seen, clear, status, tasks, a process, a signal). The
    built-in's `KernelSource` answers from `Stats`, `TaskList` and `Loader`; a program built from
    the SDK would answer from the calls, nothing else changing.
  - **`Monitor`** (pure, host-tested): the counters' catalogue (listed again when the generation
    changes), each tick's values and health in one `read`, rates from two reads, lookups by
    name, the worst health under a name pattern (a tab's), and the values' text with their units
    (`1.25 MB`, `3.4 ms`, `12.5 %`, a state's label).
  - **`SysmonWindow`:** the tabs, refreshed by a 1-second timer while the window is shown and not
    minimized; only the current tab's widgets are filled, but every tab's title is marked (`Audio
    !`) from the one read. Graphs are set from the registry's history each tick (so they follow
    `sysmon/interval`), a `Total`'s increases turned into a rate a second.
- **Tabs as built:**
  - **Overview:** CPU 0 and CPU 1 graphs (busy %, the levels drawn), memory per space as bars, the
    headline line (audio load and late periods, frames a second, the compositor's slowest round,
    storage throughput), the Problems list (health, state, counter, began, lasted, count, value)
    and Clear. Shown, the problems up to now are marked seen.
  - **CPU:** the two graphs; the tasks (name, process, core, priority, state, CPU %, CPU time,
    stack never used), sortable, a stack under 512 bytes amber, under 256 red.
  - **Processes:** pid, command, state, CPU %, CPU time, memory, threads, parent; End Process
    (`SIGTERM`, then `SIGKILL` if it is still there 2 s later).
  - **Memory:** per space total, used, free, largest, low-water; internal and PSRAM graphs; the
    caches.
  - **Audio, Display, I/O:** their graphs (mixing load and fill; frames a second and the round
    time; read and write throughput) over the counters of their groups (`audio.*`, `synth.*`,
    `midi.*`; `display*`, `wm*`; `storage.*`, `usb*`, `input`, `console.*`): counter, value,
    rate, help.
  - **Counters:** every counter: name, value, rate, unit, rule, warning, critical, health.
- **Toolkit additions** (`bmds_gui`, public): `Graph` (series of samples as 1-pixel columns, runs
  of equal height merged into one fill; the levels as lines; a caption; a border in the health's
  colour) with its pure `GraphLayout`; `ItemModel::emphasis()`, a row drawn amber or red by
  `DetailsView`; `WindowManager::isMinimized()`.
- **Opened from** the launcher (`sysmon.desktop`, icon `utilities-system-monitor`); a second
  `sysmon` raises the first (the service `sysmon`).

#### The panel's tray meter

- **A `TrayIcon` in the panel** (category System), as the volume control is: a 16 × 16 graph of
  each core's busy share over the last 16 samples (two thin series, or combined, by a setting).
  - **Problems:** its border turns amber or red while a problem is open, and stays so until the
    Problems list is seen (opening the monitor at Overview clears it).
  - **Tooltip:** "CPU 12% · 9% / Memory: 3.1 of 32 MB", then the newest problem
    ("Audio: 3 late periods, 12 s ago").
  - **A click** opens (or raises) the monitor, at Overview when a problem is shown.
  - **CPU time off this boot:** a dimmed icon, "CPU time isn't measured (sysmon/enable)", still
    turning amber or red for the other counters.
- **On by default** (the user, 2026-10-08): `panel/items`' default gains `cpu`.

#### The shell

- **`top [-d SECONDS] [-n COUNT] [-T]`:** a screen, redrawn every interval (1 s by default) until
  `q` or Ctrl+C:
  - a header: uptime; each core's busy %; memory per space as used/total, the largest block and
    the low-water mark; the process and task counts; open problems;
  - the processes sorted by CPU %: pid, name, CPU %, time, memory, threads, state (with `-T`,
    their tasks, and the kernel's own tasks under a "kernel" row);
  - a line of headline counters (`audio dsp0 5.6% 0 late · 60 fps · sd0 1.2 MB/s`).
- **`stats [-w SECONDS] [PATTERN | rules [PATTERN] | problems | clear]`** (built in step 2, ahead
  of the rest, to see the counters on the board): `-w` watches for that long first, so rates,
  health and problems are current.
  - **Counters:** `COUNTER VALUE RATE/S HEALTH`, one a line in registration order; a value with
    its unit (or its state's label, `-` if unavailable); the rate for a `Total`; the health,
    `-` while nothing watches.
  - **`rules`:** `COUNTER KIND RULE WARN CRIT HOLD HELP`, the rules in force (adjusted, then
    overridden).
  - **`problems`:** `HEALTH STATE AGO LASTED COUNT COUNTER (FIRST to LAST)`, the log in the
    order of its changes; `STATE` is `open`, `closed` or `before`.
  - **`clear`:** forgets the closed problems.
- **Batch mode** (`-n COUNT`, or output not a terminal): plain lines a script can read, their
  format frozen once shipped, like the other commands.
- **`tasks` gains a CPU column** (appended). **`ps` unchanged** (its format is frozen).

### 3.6 Programs: system calls (appended)

Slots 80-87 (step 3); check `bmds_sys_has(BMDS_SYS_STATS_LIST)`. Every struct starts with
`uint32_t size`; an array of them takes its stride from the first entry's `size`, as `io_poll`'s
does. The kernel's enums are the ABI's numbers (`Stat::Unit`, `Kind`, `Rule`, `Health`), pinned by
`static_assert`s.

- **`stats_list(uint32_t index, bmds_counter_t *)`:** the counter at `index` (registration
  order): its id, the registry's generation, name, unit, kind, flags, rule, the rule's health,
  hold (the event default filled in), warning and critical levels as in force, state labels and
  help. No value: it reads nothing, so listing every counter costs no group a read. A reader
  lists again when the generation changes. `-BMDS_ENOENT` past the last.
- **`stats_read(const uint32_t *ids, uint32_t count, int64_t *values, uint8_t *health,
  int64_t *time_ns)`:** several counters at one moment (a group read once for its run of ids),
  `BMDS_COUNTER_UNAVAILABLE` for one gone; `health` (optional) each one's at the last sample, or
  `BMDS_HEALTH_UNJUDGED` while nobody watches. Rates are the reader's: two reads and their times.
- **`stats_history(uint32_t id, int64_t *values, uint32_t max, int64_t *newest_ns)`:** a
  counter's samples, the oldest first (a `Total`'s increase each interval, else its value, kept
  as 32 bits), and when the newest was taken.
- **`stats_problems(uint64_t after, bmds_problem_t *out, uint32_t max)`:** the episodes
  changed after change `after`, in the order of their changes (a reader passes the last one's
  `changed` next time, so it sees new episodes and the open ones' updates).
- **`stats_control(uint32_t op, uint64_t value)`:**
  - `BMDS_STATS_WATCH` (1 or 0): the calling process watches (the sampler runs) or stops; once
    per process, carried across `exec`, dropped when it ends.
  - `BMDS_STATS_SEEN`: the problems up to change `value` were shown (the tray stops alarming).
  - `BMDS_STATS_CLEAR`: forgets the closed problems.
- **`stats_status(bmds_stats_status_t *)`:** the generation, counter count, watchers, interval,
  whether CPU time is counted and whether the caller watches, the cores, the newest problem
  change, the unseen problems' worst health (the tray's colour), and the last sample's time.
- **`task_list(bmds_task_info_t *out, uint32_t max)`:** every task BMDS started (its `Task`s and
  `Thread`s, which register themselves) and the system's own (the idle tasks, the timer service,
  `ipc0`/`ipc1`, `esp_timer` if present): name, pid and tid (0: the kernel's), state, priority
  and base priority, core, stack never used, CPU time, FreeRTOS's task number. Returns how many
  there are (more than `max`: the rest left out).
  - **Not `uxTaskGetSystemState()`:** on this SMP port it holds the kernel's spinlock, with
    interrupts masked, while it scans every task's stack: measured 2.7-3.2 ms on the board
    (2026-10-09; the shell's 64 KB stack in PSRAM is most of it), which a once-a-second window
    must not cost the audio engine. The task list's own lock is a mutex; each task's state comes
    from `vTaskGetInfo()` without the stack scan (a short critical section), and the stack is
    scanned afterwards with interrupts on, while the list's lock keeps the task from being
    deleted.
- **`proc_info(int32_t pid, uint32_t flags, bmds_proc_info_t *)`:** process `pid`, or with
  `BMDS_PROC_NEXT` the first after it (0: from the start): pid, parent, group, session, state,
  threads, memory, CPU time, start time, a zombie's status, the command line. Returns its pid,
  or `-BMDS_ESRCH`.
  - **A process's CPU time:** its tasks' counters summed, plus those of its ended threads (taken
    as each ends), plus the program's before an `exec`. A vfork child's time is its parent's
    (it runs on the parent's task). A zombie keeps the time it had when it ended.
- **Stacks are judged by the reader,** not the sampler (§3.3): scanning every stack each second
  inside the registry's lock is the cost `task_list` avoids. `bmds/sys.h` gives the levels:
  under `BMDS_TASK_STACK_WARN` (512) free warns, under `BMDS_TASK_STACK_CRIT` (256) is critical.
- **Linux's view through the SDK** (step 6): `getrusage()` and `times()` from the process's own
  time; `sysinfo()` from memory and uptime (`linux.c` makes some of these up today). A `/proc` is
  its own design, if ever.

## 4. Phasing (the window early: the user, 2026-10-09)

1. **Measure the cost** (done 2026-10-09, §3.1: on by default): run-time stats on (ESP timer,
   64-bit), the counter wrapped, `sysmon/enable`; the context switch (on and off), the audio fill
   and the size measured and recorded here, which decides the setting's default.
2. **The registry, the rules and the log** (done 2026-10-09; `stats` brought forward from step 6):
   - the pure parts in `bmds_core`, host-tested: counters' rates and wraps, the rules with
     `hold`, episodes, `sysmon/limits` parsing and matching, the history ring;
   - the kernel's registry, sampler and problem log;
   - every subsystem's counters registered (§6), and the new ones (block devices, input, ISR
     time, the console).
3. **The system calls** (done 2026-10-10, §3.6): `progtest stats`, `tasks` and `procs` pass on
   the board, as do the loader's other suites (spawn, threads, signals, poll, scripts).
4. **The window** (done 2026-10-10, §3.5's step 4): the `Graph` widget, then `sysmon` with every
   tab. Seen on the board: `yes` on core 1 turns its graph's border red and marks CPU, Overview
   and Counters "(!!)", its problem red in the list; End Process ends it (a zombie keeping its
   1:08 of CPU time); the Overview shown marks the problems seen; closing the window ends its
   watch; a second `sysmon` raises the first.
5. **The tray meter** with its problem colours; `panel/items` gains `cpu`.
6. **The shell:** `top`, `tasks`' CPU column; `getrusage()`, `times()` and `sysinfo()`. `tasks`
   lists from the kernel's task list (the user, 2026-10-10), not `uxTaskGetSystemState()`, its
   format unchanged.

## 5. ABI impact

- **New system calls** (appended, slots 80-87): `stats_list`, `stats_read`, `stats_history`,
  `stats_problems`, `stats_control`, `stats_status`, `task_list`, `proc_info`, with their structs
  and constants.
- **Counter names, units, kinds and state labels** are an interface once shipped.
- **New shell commands:** `top`, `stats` (their batch output frozen); `tasks` gains a column.
- **Settings:** `sysmon/enable` (bool, per boot, default on), `sysmon/interval` (ms, 100 to
  60,000, default 1,000), `sysmon/limits` (string, default empty), the meter's graph form;
  `panel/items`' default gains `cpu`.
- **A new built-in program:** `sysmon`, with `sysmon.desktop`.

## 6. The counters (as built, step 2)

`N` is an instance's number. "Zero" holds for 60 s; "peak above" is an event held 60 s. Each
counter's help line is in its description (`stats rules`).

| Group | Counters (kind, unit) | Rules |
|---|---|---|
| `cpu.coreN` | `busy_us` (total µs, history) | above 900,000 / 980,000 a second, 5 s |
| `mem.SPACE` (`internal`, `internal_dma`, `psram`, `psram_dma`, `fast`) | `total` (info), `free`, `min_free`, `largest`, `used` (history) (bytes) | `free` below a tenth / 3 % of the total, 5 s; `largest` below 16 K / 4 K (internal), 8 K / 2 K (internal DMA), 512 K / 128 K (PSRAM), 5 s |
| `system` | `uptime_us`, `tasks` (history), `cpu_counted` (state `no\|yes`) | none |
| `clock` | `source` (state `none\|manual\|rtc\|ntp\|ptp\|gps\|host`), `slew_ns`, `frequency_ppb` | none |
| `audio.dspN` | `state` (`stopped\|running\|faulted`), `rate_hz`, `period_frames`, `mclk_asked_hz`, `mclk_hz`, `apll_hz` (info), `measured_mhz`, `clock_ppb`, `periods`, `late`, `fill_us`, `fill_us_max` (peak), `mix_us` (total, history), `mixed_periods`, `limited` (frames), `streams` (history), `realtime`, `waited_us`, `sounds_dropped`, `volume_db`, `muted`, `underruns`, `client_late`, `client_skipped`, `client_throttled`, `held`, `exclusive` | `state` faulted critical; `clock_ppb` drift 100,000 / 500,000, 10 s; `late` zero critical; `fill_us_max` peak above ½ / 0.8 of a period; `mix_us` above 500,000 / 800,000 a second, 5 s; `limited`, `sounds_dropped`, `underruns`, `client_*` zero warning |
| `synth.N` | `voices` (history), `peak_voices`, `max_voices`, `notes_at_limit`, `queued`, `queue_full`, `ignored`, `frames`, `cpu` (‰, history), `cpu_max`, `rate_hz`, `presets`, `font_bytes` | `notes_at_limit`, `queue_full` zero warning; `cpu` above 500 / 800, 5 s; `cpu_max` peak above 900 / 1,000 |
| `midi.portN` | `kind` (`usb\|serial\|virtual`), `events_in` (history), `events_out`, `dropped`, `parse_errors`, `device_errors` | `dropped`, `parse_errors` zero warning; `device_errors` zero critical |
| `displayN` | `running`, `width`, `height`, `pixel_hz`; `scanout.frames` (history), `.blocks`, `.blocks_per_frame`, `.swaps`, `.stalls`, `.underruns`, `.resyncs`, `.rebuilds`, `.failures`, `.isr_us` (history), `.isr_cycles_max`, `.dma_item`, `.bridge_fifo`; `surface.presents` (history), `.shown`, `.forwards`, `.forward_pixels`, `.forward_us`, `.forward_us_max`, `.wait_us`, `.wait_us_max` | `stalls`, `underruns`, `failures` zero critical; `resyncs`, `rebuilds` zero warning; `forward_us_max`, `wait_us_max` peak above 2 / 4 frames |
| `wmN` | `rounds` (history), `pieces`, `redraws`, `dma_calls`, `fallbacks`, `errors`, `round_us` (history), `round_us_max`, `last_round_us`, `deferred`, `windows` | `errors` zero critical; `round_us_max` peak above 33,333 / 100,000 µs |
| `storage.DEV` (`sd0`, `sd0p0`, `ram0`, `ram0p0`, `flash0`, `rom0`) | `reads`, `writes`, `read_bytes`, `written_bytes` (history), `errors`, `io_us`, `erases` | `errors` zero critical |
| `cache.ITEMS` (`programs`, `fonts`) | `bytes` (history), `items`, `reclaimable`, `hits`, `misses`, `reclaims` | none |
| `proc` | `count` (history), `zombies`, `stopped` | `count` above 24 / 30 (of 32) |
| `usbN` | `added`, `gone`, `hid_reports` (history), `keyboards`, `mice` | none |
| `input` | `keys`, `pointer_events` (history), `dropped` | none (events drop when no window manager runs: not abnormal) |
| `console.usbserial` | `rx_dropped` (bytes) | zero warning |

- **Numbered, not singletons** (the user's rule): `displayN`, `wmN`, `usbN`, `audio.dspN`,
  `storage.DEV`, as the devices are named. `system`, `clock`, `input`, `proc` and the console
  are one each by nature.
- **Measured on the board (2026-10-09):** an idle desktop's cores 0.2 % and 0.01 % busy; `yes`
  holds core 1 at 1,000,000 µs a second, critical after 5 s; an Ogg file 72,000 µs a second of
  mixing (7 %), no late periods; the scanout 60 frames a second, its interrupt 0.17 % of a core
  (35 µs at most); the first full-screen round at start 69 ms (logged "before", as it is).
- **Later:** per-stream counters (the device's tallies stand in for them now), filesystems
  (§3.2), the network.

## 7. Decisions (the user)

- **2026-10-08:** the tray meter is on by default; the interval is 1 s, a setting, with 2
  minutes of history; nothing real-time pays for it (`sysmon/enable` per boot, its default from
  the measurement).
- **2026-10-09:**
  - **Named counters** behind a few system calls, not a struct per subsystem: a new subsystem or
    field needs no new call.
  - **Rules from the source, overridable** in settings (`sysmon/limits`).
  - **Alerting outside the window:** the tray meter turns amber or red. No balloons, no log
    lines.
  - **The window early:** after the registry and the calls, before `top`.

## 8. Revisions

- **2026-10-10, after step 4 (the user's look):** a fill batch with two rectangles ending in one
  4-pixel group lost the first's pixels there (both read the group before either wrote it):
  `Surface::fillEdges()` now sends the batch first, which also fixes neighbouring fills elsewhere;
  a graph's columns share its whole width (3 or 4 pixels each); a counter's first sample after a
  pause starts its history at the next one (it covered the whole pause, a spike); a details
  view's header text stops short of its button's edge.
- **2026-10-10, step 4 as built:** the window reads through a `Source` (the kernel's classes
  for the built-in), so it could be built from the SDK unchanged; tab titles marked `(!)` and
  `(!!)`; the problems' values are the counter's own (a total's, for a rate rule); a process's
  memory is what its program allocated, so built-ins show 0 B.
- **2026-10-10, step 3 as built:** a running task's counter lacks its current stretch (FreeRTOS
  adds it at the next switch), so the run-time clock's reader records each core's switch time and
  the task list adds the stretch; the sampler thread is the kernel's even when a process's task
  starts it (`Thread::Config::kernel`), since a watching program (or the shell's `stats -w`) took
  it into its own process and waited for it at exit.
- **2026-10-09, step 3:** the calls as designed for building (§3.6): `stats_control` (watch,
  seen, clear) and `stats_status` instead of `stats_watch` alone; `stats_list` without a value
  and `stats_read` with each counter's health; `proc_info` (by pid, or the next) instead of
  `proc_stats`; `task_list` fills an array from BMDS's own task list, since FreeRTOS's listing
  masks interrupts for about 3 ms; stacks judged by the reader, not the sampler.

- **2026-10-09, step 2:** the counters' names as built (§6): numbered groups (`display0`, `wm0`,
  `usb0`), the audio clock's error in ppb (`clock_ppb`) for its precision, the device's stream
  tallies instead of per-stream counters, no `fs.*` yet; one hold for both levels; a `Peak` above
  a level judged as an event; episodes found on the first look logged as "before", not alarms;
  the rules paused when nobody watches; a group's judges made at its first sample; `stats` built
  ahead of step 6.
- **2026-10-09:** from a CPU monitor to the whole system's behaviour: the counter registry
  (§3.2) replaces the planned `sys_stats` and `stats_counters` calls; rules, overrides and the
  problem log are new (§3.3); new counters for block devices, input, ISR time and the console;
  the window gains Audio, Display and I/O tabs; the phasing puts the window before `top`.
