# Bench: one system benchmark for all tuning

Status: **steps 1 to 4 built, step 5 begun** (2026-10-17, the user's request; decisions agreed
the same day; §10-§14). Replaces the benchmarks spread across `sd`, `fsbench`, `simd`,
`memtest`, `display`, `win` and `progtest` (§3). Comes before Quake (`games.md`), whose renderer
and presentation need a baseline.

## 1. Goal

- **One place** (the user's): every benchmark the system has, run by one command with one
  output, so a change anywhere can be measured before and after.
- **Extensible** (the user's): a component adds a benchmark next to the code it measures, and a
  program (a game's timedemo, a network test) adds one without touching the firmware.
- **For all tuning** (the user's): results saved, compared run to run with the noise told apart
  from a change, and what the rest of the system suffered meanwhile reported with each result.
- **The old ways end** (the user's): each existing benchmark moves in and its old command or
  subcommand is removed (§3).

## 2. Decisions (the user's, 2026-10-17)

| # | Decision | Agreed | Not chosen |
|---|---|---|---|
| B1 | Where benchmarks live | In the component they measure, registered by a macro into a link-time table, as shell commands are (`BMDS_BENCH`, `.bmds_bench.*`): display benches need the scan's and PPA's internals | all in one component (would need every private header) |
| B2 | Programs' benchmarks | Yes: descriptor files (`*.bench`) naming a command line; the program prints `@bench` lines (§4f) | firmware-registered only (no Doom, no netlat) |
| B3 | Correctness tests (`simd test`, `memtest`'s patterns, `display rows`, `win check`, `blittest`) | Not bench's: benchmarks only. Correctness is a different problem, and those tests stay where they are | one table for both |
| B4 | What to run | Always named: bare `bench` lists the benchmarks; `bench run` needs at least one name, pattern or tag, and runs nothing by default | a default suite |
| B5 | Results | One JSON file a run, written once, on the board in `/home/bench/` by default (the setting `bench/folder`), named by the serial, the OS version, the date and the time; each holds the full version, the system's facts and every sysmon counter's change (§4h) | TSV; a SQLite database (400-500 KB of firmware, one file a power cut can ruin) |
| B6 | Analysis | On the host (`tools/bmds-bench`): previous, best, worst, mean, the history; no combined score. The board's `bench` stays light: it runs, prints and writes (§4g) | comparison on the board; a combined index |

## 3. What exists, and where it goes

Inventoried 2026-10-17: none of it is behind a Kconfig option, there is no shared timing helper
(`msSince()`, `elapsedMs()`, `ms()` and `now_ns()` copied per file; `esp_cpu_get_cycle_count()`
called directly in two places), and every command dispatches its subcommands by hand.

**Moves into `bench`, the old form removed:**

| Now | Becomes | Notes |
|---|---|---|
| `sd bench [MB]` | `storage.sd.read` | raw reads by request size; `sd` keeps its status |
| `fsbench DIR ...` | `fs.smallfiles` (`dir=`, `files=`, `bytes=`, `pace=`) | the stall spinner becomes the general meanwhile probe (§4d) |
| `simd bench [KERNEL]` | `simd.KERNEL` (scale, glyphs, swap24, pngfilter, softjpeg, orient, blend, repeat16) | scalar and PIE times and the ratio; `simd test` and `stress` stay |
| `memtest`'s MB/s | `memory.psram.cpu`, `memory.psram.dma` | `memtest` stays as the correctness test, without its rates |
| `display test`'s and `fill`'s times, `present`, `anim`, `ppa`'s timings (`path`, `soak`, hidden `blitcost`) | `display.draw`, `display.fill`, `display.present`, `display.anim`, `display.ppa.scale`, `display.ppa.path`, `display.ppa.soak`, `display.blitcost` | `display test` and `fill` still draw; `display ppa` keeps its checks; `stats`, `rows`, `try` stay |
| `display stress psram`, `write`, `internal` | `memory.traffic` | memcpy and memset, PSRAM and internal |
| `win bench [CELLS]`, `bench icons`, `textbench`, `shuffle`, `drag`'s and `scroll`'s times | `gui.glyphs`, `gui.icons`, `gui.text`, `gui.shuffle`, `gui.drag`, `gui.scroll` | on test windows of their own; `win drag` and `scroll` still move; `win stats` and `check` stay |
| `progtest synthload`, and the times `sched`, `net`, `mididecode` and `image` printed | `audio.synth.load`, `kernel.sleep`, `net.loopback`, `midi.decode`, `images.codecs` | `benchprog`'s (§12); `progtest` keeps its correctness tests; `rtnet` stays a test (`net.latency` measures the path) |
| `netlat` | `net.latency.*` (peer), a descriptor running `netlat --bench` | `netlat` stays as a tool |

**Stays as it is** (monitors and tools, not benchmarks): `prof`, `stats`, `top`, `tasks`,
`mem`, `display stats`, `win stats`, `audio status`, `synth`'s summary, `image`'s own timing
line, `ping`, `net`, `ptp`, `usb`, `midi`, `keytone` (needs a person), `surfdemo` (a demo),
`tools/bmds-netstress` (a stress test). The devplan documents that quote the old commands get a
line saying which bench replaced them; their recorded results are left as history.

## 4. Design

### 4a. The table

```cpp
BMDS_BENCH(display_ppa_scale, "display.ppa.scale", runPpaScale,
           BenchTag::Screen, "Scales a frame with the PPA at each size and format",
           BMDS_BENCH_PARAMS({"size", "1280x720"}, {"format", "all"}));
```

- `[[gnu::used, gnu::section(".bmds_bench.NAME")]] constexpr Bench`, sorted by the linker
  fragment like commands: no global constructors, nothing in RAM until a run.
- A `Bench`: dotted name, function, tags, one line of help, parameters with defaults (rodata),
  nothing else: benchmarks only (B3).
- Tags: `quick`, `long`, `screen` (takes over or draws on the display), `audio` (makes sound),
  `writes` (writes files, under `dir=`, default `/tmp`), `peer` (needs a host), `program` (runs a
  program). Suites are tags: `@quick`, `@display`, `@all`.

### 4b. The context

The function gets a `BenchContext &`:

- **Parameters:** `ctx.param("size")`, typed helpers; unknown keys are an error before the run.
- **Measuring:** `ctx.measure("read 4096", [&]{ ... })` runs a warm-up, then repeats until a time
  budget or a count (the bench's defaults, `runs=` and `time=` override) and keeps each run;
  or `ctx.report(metric, value, unit, Better::Higher)` for a figure the bench works out itself.
- **Each metric:** median, minimum, maximum, runs, and the spread; a spread over 5% is marked
  noisy in the output.
- **Cancel:** Ctrl-C sets `ctx.cancelled()`; a bench checks it between runs.
- **Timing:** `Stopwatch` (`bmds_kernel`): microseconds from `Detail::monotonicUs()` and cycles
  from a new `Detail::cycles()` (the CPU's counter); the per-file helpers go.

### 4c. Selection and output

- `bench` (alone) and `bench list [PATTERN|@TAG]`: the benchmarks, with their tags and help.
- `bench info NAME`: help, parameters and defaults, tags.
- `bench run [-o FILE] [-n] [-q] PATTERN|@TAG... [KEY=VALUE...]`: at least one name (B4);
  patterns glob the dotted names. Saved to `bench/folder` (§4h) unless `-o` names another file
  or `-n` saves nothing; the file's name is printed at the end.
- On the terminal, a line a metric: `storage.sd.read  4096 B  21.3 MB/s  ±0.4%  (5 runs)`; a
  spread over 5% marked noisy. No comparison on the board (B6).

### 4d. What the system suffered meanwhile

Every sysmon counter (the `Stats` registry: audio, display, compositor, storage, caches, USB,
input, network, ...) is read before and after each bench, and the changes kept in the results
file with that bench; each core's idle from run-time stats and internal RAM's lowest free too.
A few are printed under the bench when they moved (the scan's underruns and stalls, the audio's
late periods). `fs.smallfiles`' spinner (how long the whole system stopped) becomes an optional
probe any bench can ask for (`probe=stall`). This replaces `display ppa`'s "scan meanwhile" and
`fsbench`'s stall line.

### 4e. Where the code goes

- **`bmds_bench`** (new): the table, `BenchContext`, the statistics, the results format and
  `compare` (pure: host-tested), the meanwhile probes and the `bench` command.
- **Each component** keeps its benches in a `bench*.cpp` next to the code, requiring
  `bmds_bench`: `bmds_storage`, `bmds_images`, `bmds_drivers`, `bmds_ui`, later others. The old
  command's dispatch code goes; the measuring code moves.

### 4f. Programs' benchmarks

- **A descriptor** (`NAME.bench`, the `.conf` format the system's files use): the name prefix,
  the command line, tags, help. The folders searched are a sysconf list, `bench/paths`
  (default the ROM's `share/bench` and the card's); a `dist/` package installs its own.
- **The program** prints result lines on stdout, `@bench METRIC VALUE UNIT higher|lower`,
  anything else passes through as text; its exit status is the bench's. The SDK gets
  `libbmdsbench` (a static library, no ABI): a stopwatch on `clock_gettime`, the repeat-and-keep
  loop, and the line printer. `bench` runs it and adds the meanwhile counters.
- **First uses:** `benchprog` (the timing parts of `progtest`, plus §6's program-side benches),
  `netlat --bench`, and Doom's timedemo (`doom -timedemo demo1`, our platform layer printing
  `@bench`; then Quake's).

### 4g. On the host (B6)

`tools/bmds-bench` does all the analysis:

- `run`: runs `bench run` over ssh (for `peer` benches starting the host's side,
  `netlat-reflector`, itself), fetches the new file and prints each metric against the earlier
  runs: `was 20.9 (+1.9%), best 21.4, worst 19.8, mean 20.7 (12 runs)`, a change marked only
  when it is larger than both runs' spread and a threshold (default 3%).
- `pull`: copies new files from the board's results folder into a local store (git-ignored,
  kept by board serial, so several boards' runs stay apart and comparable).
- `history NAME [METRIC]`, `compare RUN RUN`: one metric run by run with each build's version
  and hash, or two runs side by side with their facts' differences (a setting or a clock that
  changed explains many a "regression").
- Earlier runs are matched by benchmark, metric and parameters on the same board. Python's
  `json` reads the files; nothing else is needed (SQLite only if the store grows to need it).

### 4h. The results file (B5)

- **Where:** the sysconf setting `bench/folder`, default `/home/bench` (made when first needed).
- **The name:** `SERIAL-VERSION-DATE-TIME.json`, e.g.
  `e8f60ae2e222-0.1.0-20261017-143205.json` (the chip's serial, the short version
  `VersionString`, local date and time; date before time, so the names sort). Written once, never
  changed: a power cut can lose at most the run being written. No pruning: the user deletes old
  ones.
- **Inside**, one object:
  - `format` (1), `command` (the command line), `start` and `end` (ISO 8601 with the zone).
  - `system`: the serial, the full version (`PROJECT_VER`: `0.1.0+72cba49a`, `-dirty` with
    uncommitted changes), the image's hash (two dirty builds of one commit differ only there),
    the chip's revision, the CPU and PSRAM clocks, the display's mode, uptime, free memory by
    space, the mounted filesystems, every sysconf setting not at its default, the running
    processes and tasks with their CPU time.
  - `benches`: one entry a bench run: its name, the parameters used, the status, a program's
    `core` (its first thread's: §17), `results` (a
    metric each: `name`, `unit`, `better`, `median`, `min`, `max`, `runs`, `noisy`), and
    `counters` (every counter that changed: its name and change; the cores' idle, internal RAM's
    lowest free).
- **Writing:** a small streaming JSON writer in `bmds_core` (escaping, nesting, numbers;
  host-tested), writing to the file as it goes: nothing on the board parses JSON, and no
  document is held in memory.

## 5. ABI

No system calls change. Once released, these are userspace interfaces, frozen as shell commands
and on-disk formats are: the `bench` command's options, the results file's format (fields
appended, never renamed or retyped; `format` raised only for a change readers can't skip), the
descriptor format and the `@bench` line. Bench names and metrics are appended, not renamed. The
removed subcommands (`sd bench`, `fsbench`, `simd bench`, the `display` and `win` timing forms,
`progtest`'s timing tests) were never released; `simd`'s output was declared an interface but
has not shipped.

## 6. New benchmarks (after the move)

- **CPU:** CoreMark (licence to check), single-precision float, PIE against scalar.
- **Memory:** latency and bandwidth for internal RAM, PSRAM (cached, by CPU and by DMA) and flash
  through the cache, by block size; `memcpy` and `memset` at each.
- **Kernel:** a context switch, a mutex handoff, a queue round trip, a system call, a program's
  launch (`cat`, `progtest`), pipe throughput.
- **Storage:** sequential reads and writes through the VFS on FAT, exFAT, littlefs and the RAM
  disk; directory listing.
- **Network:** TCP over loopback and to a peer.
- **Surfaces and games** (what Quake needs): a program's present at L8, RGB565 and RGB888,
  windowed at 1×-3× and full screen, whole frames and small damage, split between the program's
  time and the kernel's conversion; `libbmdsgame`'s fills, blits, sprites, turned and scaled
  blits, text and DMA blits; a column-and-span texturing loop drawing into internal RAM and into
  PSRAM (Doom's and Quake's pattern); Doom's timedemo.

## 7. Steps

1. **The framework:** `bmds_bench`, `BMDS_BENCH`, `BenchContext`, `Stopwatch` and
   `Detail::cycles()`, `bench list|info|run`, the JSON writer, the results files in
   `bench/folder` with the system's facts and every counter's change; host tests for the
   statistics, the JSON writer, the file's contents and its name. One bench moved to prove it
   (`storage.sd.read`).
2. **The firmware's benches moved:** storage and filesystems, SIMD, memory, display, GUI; the old
   subcommands removed; the devplan notes.
3. **Programs' benches:** descriptors, `bench/paths`, `libbmdsbench`, `benchprog`,
   `netlat --bench`, Doom's timedemo; `progtest`'s timing tests removed.
4. **The host tool** (`tools/bmds-bench`: `run`, `pull`, `history`, `compare`) and a first full
   baseline recorded here. (A minimal `run` that prints the comparison comes with step 1, so
   the framework is used as it will be.)
5. **New benchmarks** (§6), the surfaces and games first, for Quake.

## 8. Budgets (estimates; measured as the steps land)

- **Firmware:** the framework and command about 6-8 KB of flash; the moved benches cost what
  they do now, less the hand-written dispatch and copied helpers. A `Bench` entry about 32 bytes
  of rodata.
- **RAM:** nothing while idle; a run keeps its samples and a counter snapshot (the registry's
  values, a few KB) in PSRAM and frees them; the file is written as it goes.
- **Card:** a run's file a few KB to some tens of KB (the counters dominate).

## 9. Open questions

- Whether a run should first quiet the system (close the System Monitor, stop the tray meter)
  or only report what was running. For now: report it (the meanwhile counters, §4d).

## 10. As built: step 1, the framework (2026-10-17)

- **JSON** (`json.md`): `JsonWriter` and `JsonReader` in `bmds_core`, the native JSON the user
  asked for; JSONTestSuite vendored for their tests (`thirdparty/jsontestsuite`).
- **Timing:** `Stopwatch` (`bmds_kernel`): microseconds from `Detail::monotonicUs()` and cycles
  from the new `Detail::cycles()` (`esp_cpu_get_cycle_count()`; nanoseconds on the host).
  `SystemInfo::imageSha256`: the running image's ELF hash.
- **`bmds_bench`:** `BMDS_BENCH` and `BMDS_BENCH_WITH` (a section a benchmark, `linker.lf`:
  `_bmds_bench_start`/`_end`), `BenchContext` (an interface: `param()`, `paramUnsigned()`,
  `measure()`, `report()`, `note()`, `isCancelled()`), `BenchText` (the statistics, patterns,
  tags, the file's name, ISO 8601 times, figures; host-tested), and the command:
  `bench` / `bench list` / `bench info` / `bench run [-o FILE] [-n] [-q]`. `runs=` (1-1000,
  default 5) and `warmup=` (0-100, default 1) are every benchmark's; any other `KEY` must be a
  chosen benchmark's, checked before anything runs. The setting `bench/folder` (default
  `/home/bench`), on the control panel's System page.
- **The file**, written as the run goes (indented a space a level): `format`, `command`, `start`
  and `end` in the shell's zone (`$TZ`, else the system's: `date`'s), `system` (serial, short and
  full version, image hash, IDF, board, chip revision, cores, CPU and PSRAM clocks, uptime,
  mounts, settings not at their defaults (a secret reads `(set)`), tasks with their CPU time),
  `counters` (every sysmon counter at the start), and `benches`: for each, `params` (all, with
  their defaults), `status` (`ok`, `Cancelled`, or the error's name), `elapsed_us`, `results`,
  `notes`, `totals` (each `Total` counter's growth), `levels` (the others that moved, before and
  after), `tasks` (each task's CPU time meanwhile). The judged counters that rose (`Zero` rules:
  underruns, late periods) are printed under the benchmark as `meanwhile:`.
- **`storage.sd.read`** (`bmds_storage/src/benchsd.cpp`): `sd bench` moved in; `mb=` (default 8,
  was 16) a run in 64 KB requests, a sixteenth in 512-byte ones, each aligned and through the
  bounce buffer. `sd` now only shows the card.
- **`tools/bmds-bench`**: `run` (over ssh; fetches the file; each metric against the earlier
  runs with the same benchmark, metric and parameters on that board: previous and change, best,
  worst, mean; `BETTER`/`WORSE` past both runs' spread and 3%), `pull`, `history`, `compare`
  (with the systems' differences). The store is `bench-results/` (git-ignored), a folder per
  serial.
- **On the board:** `storage.sd.read` direct 64 KB 15.9 MB/s (±0.6%), bounced 64 KB 5.6 MB/s,
  512-byte requests 1.36 and 1.33 MB/s; runs a few seconds apart agree within 1%. Ctrl-C stops
  between runs and the file is still whole (`"status": "Cancelled"`). The file is 13-14 KB (the
  counters at the start most of it). Internal RAM the same after five runs.
- **Cost:** the image grew 14,656 bytes (objects: the command 10.5 KB, `BenchText` 3.4 KB,
  `JsonWriter` 2.1 KB, the SD benchmark 1 KB, less `sd bench`'s), against the 8-10 KB estimated; `JsonReader` isn't
  linked until something reads JSON. Nothing in RAM while idle; a run takes about 30 KB of PSRAM
  (samples, metrics, the counter and task snapshots) and gives it back.
- **Next:** step 2, the firmware's other benchmarks moved in (`fsbench`, `simd bench`,
  `memtest`'s rates, `display`'s and `win`'s timings), the old forms removed.

## 11. As built: step 2, the firmware's benchmarks moved in (2026-10-17)

- **27 benchmarks** in five components, each next to the code it measures:
  - `bmds_storage`: `storage.sd.read`, `fs.smallfiles` (was `fsbench`; `dir=` /tmp by default,
    `files=`, `bytes=`, `pace=`, `probe=stall` by default).
  - `bmds_images`: `simd.scale`, `.glyphs`, `.swap24`, `.blend`, `.pngfilter`, `.softjpeg`,
    `.orient`, `.repeat16` (scalar and PIE each, where the build has PIE).
  - `bmds_drivers`: `memory.psram.cpu`, `memory.psram.dma`, `memory.traffic`;
    `display.ppa.scale`, `.ppa.path`, `.ppa.soak`, `display.blitcost` (scratch pictures);
    `display.draw`, `.fill`, `.present`, `.anim` (surface 0).
  - `bmds_ui`: `gui.glyphs`, `gui.icons`; `gui.shuffle`, `.drag`, `.scroll`, `.text` (on test
    windows they open and close).
- **Removed:** `fsbench`; `simd bench`; `memtest`'s rates; `display present`, `anim`, `stress` and
  the times `display test` and `fill` printed; `display ppa`'s timings and its `path`, `soak` and
  `blitcost` forms (`display ppa` keeps the checks: interpolation, the CLUT, the doubling kernel);
  `win bench`, `bench icons`, `shuffle`, `textbench` and the times `win drag` and `scroll`
  printed. Each devplan that quoted them has a note saying what replaced them; their figures
  stay as history.
- **The framework grew:**
  - `BenchContext::reportSamples()`, for a run that gives several metrics at once (`fs.smallfiles`'
    seven, `display.anim`'s three); `runs()` and `warmup()`.
  - A benchmark may set its own defaults for the common parameters (the PPA's `runs=20`, the
    soak's `runs=1 warmup=0`); `bench info` shows them. The common ones in one table: `runs`,
    `warmup`, `probe`.
  - **`probe=stall`** (§4d): a spinner in flash at the idle priority on the core the benchmark
    isn't on; its counts lost against a rate measured first (0.5 s) are reported as `stopped_ms`
    and `stopped_percent`. `fs.smallfiles` on `/flash`: 5.6 ms of pause an operation in bursts,
    3-5.5 ms paced 300 ms apart, as `fsbench` measured (§13d: 4.6 and 4.5 ms).
  - **Skipped, not failed:** a benchmark returning `Busy`, `NotReady`, `NoMedia` or
    `NotSupported` can't run here (surface 0's while the desktop has the display, no card):
    `skipped: REASON`, `"status": "skipped"` with a `reason`, and the exit status unchanged.
  - `Task::currentCore()`.
- **Two crashes found on the way:**
  - **The probe's first two designs:** on core 1 at priority 1 the benchmark's own work counted
    as stopped (15.6 ms an operation, not 4.5); on core 0 the board reset by watchdog with nothing
    on the console. Read with a serial capture: core 1 in the panic handler, core 0 at its vector
    table, `HP_SYS_HP_WDT_RESET`. With a 4 KB stack (from 2) the panic printed.
  - **What it printed: a latent bug in the scanout's interrupt.** `ScanoutIsr::blockSent` and
    `invalidBlock` (IRAM-safe: they run while the flash is written) called an out-of-line
    `std::atomic<bool>::load`, which is in flash; when `/flash`'s writer ran on core 0, the
    interrupt on core 1 took a cache error. Fixed with `__atomic_load_n` (always inline); six
    paced runs on `/flash` since, no reset. Any `/flash` write on core 0 (a `sysconf save`) could
    have hit it. A scan of `.iram0.text` for calls into flash found no other in an interrupt that
    runs with the cache off (`docs/platforms/esp32p4.md` §7). The three resets tripped the crash
    counter: the board started recovery once, and `reboot` there brought the main firmware back.
- **On the board** (the desktop running; the figures agree with those recorded by the old
  commands): `simd.scale` 789 / 301 ms scalar / PIE, `simd.pngfilter` 597 / 70 µs,
  `gui.glyphs` 754 / 403 cycles a cell, `display.ppa.scale` 30.9 ms for Blocks' 2× job,
  `memory.psram.dma` 71 MB/s, `memory.psram.cpu` 31 MB/s each way, `gui.shuffle` 30 ms a change,
  `gui.text` 27 ms for 29 rows of 99 cells. `memory.traffic`'s PSRAM copy is 32 MB/s against
  `display stress psram`'s 102 on 2026-10-05, before the desktop scanned and composed beside it.
  Short benchmarks in internal RAM come out noisy with the desktop up (`simd.repeat16`, parts of
  `display.ppa.path`): their medians hold, the flag says so. Surface 0's benchmarks were only
  seen to skip (the desktop has the display): they weren't run.
- **Changed from the plan:** `display.stress.*` became `memory.traffic`; `gui.glyphs` compares
  the kernels by a hash of their rows (two 61 KB strips no longer fit in internal RAM together);
  `memory.traffic`'s internal copy is 32 KB (two 64 KB blocks didn't fit).
- **Cost:** the image grew 8,080 bytes over step 1 (the measuring moved, each `measure()` an
  instantiation; the hand-written dispatch and printing went).
- **Next:** step 3, programs' benchmarks (descriptors, `libbmdsbench`, `progtest`'s timings,
  `netlat --bench`, Doom's timedemo).

## 12. As built: step 3, programs' benchmarks (2026-10-17)

- **Descriptors** (§4f): `*.bench` files in the folders of the setting `bench/paths` (default
  `/rom/share/bench;/sd0/p0/share/bench`; on the control panel's System page), read each time
  `bench` runs. INI: a `[Bench NAME]` group a benchmark, with `Exec=` (the program, found on
  `$PATH` without a `/`, and its first arguments), `Tags=`, `Help=`, `Param.NAME=DEFAULT;HELP`
  (a common one, `runs` or `warmup`, sets the benchmark's default). Tagged `program`; listed,
  described and run with the firmware's (a name declared twice: the first kept, the second
  reported). Parsed by `BenchText::parseDescriptor()` (host-tested).
- **Running one:** the program gets its `Exec`, then `KEY=VALUE` for each parameter, `runs` and
  `warmup` among them; its standard output on a pipe, read as it comes: `@bench METRIC VALUE
  UNIT higher|lower` lines are a run's figure each, gathered by metric (the program does the
  runs), anything else a note. Exit 0 done, 77 skipped, any other or a signal failed (a note
  says which). A child the shell's process waits for, in a group of its own: Ctrl-C reaches the
  shell, which sends the program SIGINT and keeps reading until it ends (no program left
  behind; the file says `Cancelled`). Needs the shell to be a process (ssh, the terminal); on
  the serial console's shell, skipped. `stall` and the counters apply as to any benchmark.
- **`libbmdsbench`** (`sdk/bench/bench.c`, `bmds/benchlib.h`, linked by `bmds_add_program` and
  `bmds-cc`; host-tested): `bmds_bench_init()`, `bmds_bench_param()`/`_long()`,
  `bmds_bench_now_ns()`, `bmds_bench_result()` (a line, flushed), `bmds_bench_measure()` (the
  warm-up and runs), `BMDS_BENCH_SKIPPED`. Named `benchlib.h`: the firmware has a `bmds/bench.h`,
  and the loader's include paths put both together.
- **`benchprog`** (`programs/benchprog`, `dist/benchprog`: its descriptor and `scale.mid` in
  `share/bench`): `kernel.syscall`, `kernel.sleep`, `programs.launch`, `net.loopback`,
  `audio.synth.load`, `midi.decode`, `images.codecs`. `progtest` lost `synthload` and the times
  `sched`, `net`, `mididecode` and `image` printed; their checks stay.
- **`net.latency`** (`netlat --bench`, `dist/netlat`'s `netlat.bench`): one run of thousands of
  datagrams by default (`runs=1`), each stage's p50, p99, p99.9 and largest, and the losses;
  without `peer=` skipped. `tools/bmds-bench run` starts `tools/netlat-reflector` on the host and
  adds `peer=` (its address towards the board) when a benchmark chosen is tagged `peer`
  (`--no-peer` not).
- **`games.doom.timedemo`** (`dist/doom`'s `doom.bench`; a fifth patch,
  `0005-g_game-timedemo-report.patch`): `doom -timedemo demo1 -nosound`, the result in
  milliseconds as `@bench fps` and `gametic_ms`, then a clean exit (Chocolate Doom ended a timed
  demo with `I_Error`: stderr, a failure's status). Freedoom 2's demo1: 2,664 tics in 46.2 s,
  **57.6 frames a second**, full screen with the desktop up. Doom's start-up lines become notes.
- **On the board:** `kernel.syscall` **8.5 µs** for `getppid` (about 3,000 cycles: worth a look);
  `kernel.sleep` 917 µs late at the normal priority (the tick), 30 µs in real time;
  `programs.launch` 12 ms; `net.loopback` 26.5 MB/s; `audio.synth.load` 493, 918, 1182 and
  1240 µs a 64-frame block with 8, 16, 32 and 48 notes (1333 µs is real time); `midi.decode`
  19× real time; `images.codecs` PNG 1518 / 805 ms to encode / decode 1280×720, JPEG 33 / 31 ms
  with the codec, 275 ms decoded in software. `net.latency` ran against this laptop, on Wi-Fi
  (a 2.3 ms median round trip): the mechanism is shown, not the path; a wired peer is needed for
  its figures. A skipped program (`midi.decode file=/nope.mid`) and Ctrl-C (`Cancelled`) behave.
- **Installed on the card for this:** `benchprog` and the new `netlat` and Doom builds, with
  their descriptors (by copying the packages' files; `tools/bmds-dist install` does the same).
- **Cost:** the image grew 8,160 bytes over step 2 (the catalogue, descriptors, the program
  runner).
- **Next:** step 4, `tools/bmds-bench`'s remaining work and a first full baseline; then step 5,
  new benchmarks (surfaces and games first, for Quake).

## 13. As built: step 4, the host tool, and the first baseline (2026-10-17)

- **The results file** records each parameter's default (`"defaults"`), so a run can be told
  from what it was given.
- **`tools/bmds-bench`:** runs are still matched by every parameter's value, but labelled with
  only those not at their defaults (`fs.smallfiles dir=/flash`, not every parameter
  `net.latency` has); `table [RUN]` prints a run as a Markdown table (the tables below), its
  skipped benchmarks after; a RUN may be `last` or `previous` as well as a file or a name.
- **The baseline** (below): `tools/bmds-bench run --no-peer @all`, about nine minutes, then
  `fs.smallfiles dir=/flash`, on the user's board with the desktop running (nothing else open)
  and this step's build (image 9827b5ec). Kept on the board in `/home/bench` and in the host's
  store: the first runs later ones are compared with.
  - Not measured: surface 0's four (the desktop has the display) and `net.latency` (no peer: a
    wired host running `tools/netlat-reflector` gives it; this laptop is on Wi-Fi).
  - `games.doom.timedemo` came out at 54.2 fps here against 57.6 in step 3's run: one run a
    time, so its spread isn't known; Doom's switch to its own full screen and back showed as a
    scan underrun, a resync and a rebuild in `meanwhile` (the rebuild expected; the underrun,
    as at other mode switches, `games.md` §20a).
  - Noisy figures are short loops in internal RAM beside the desktop (`simd.repeat16`,
    `display.blitcost`, parts of `display.ppa.path`) and sleeps at the normal priority: their
    medians stand.
  - Leads for tuning: `kernel.syscall` (8.5 µs a call), `audio.synth.load` (48 notes take 93 %
    of a block's time), `memory.traffic`'s PSRAM copy (32 MB/s beside the scan and composing),
    `net.loopback` (26 MB/s).

| Benchmark | Metric | Median | Unit | Spread | Runs |
|---|---|---:|---|---:|---:|
| `audio.synth.load` | notes_8_block_us | 490.4 | us | ±15.7% noisy | 5 |
|  | notes_16_block_us | 919.5 | us | ±7.0% noisy | 5 |
|  | notes_32_block_us | 1177 | us | ±0.6% | 5 |
|  | notes_48_block_us | 1233 | us | ±0.6% | 5 |
| `display.blitcost` | start_blit_640x8_888 | 132 | us | ±114.4% noisy | 20 |
|  | start_blit_640x8_565 | 135 | us | ±45.6% noisy | 20 |
|  | start_blit_1280x8_888 | 218 | us | ±7.6% noisy | 20 |
|  | start_blit_1280x8_565 | 222 | us | ±138.1% noisy | 20 |
|  | msync_20k_internal | 4 | us | ±12.5% noisy | 20 |
| `display.ppa.path` | cpu_double_scalar | 10830 | us | ±5.7% noisy | 20 |
|  | cpu_double_pie | 6861 | us | ±10.4% noisy | 20 |
|  | cpu_double_cached | 1010 | us | ±49.7% noisy | 20 |
|  | dma_x2 | 17806 | us | ±8.1% noisy | 20 |
|  | pipelined_x2 | 18252 | us | ±3.5% | 20 |
|  | dma_x1 | 8802 | us | ±0.3% | 20 |
|  | pipelined_x1 | 11779 | us | ±16.9% noisy | 20 |
|  | fetch_to_internal | 4054 | us | ±14.8% noisy | 20 |
|  | fetched_pipelined_x1 | 11440 | us | ±1.8% | 20 |
| `display.ppa.scale` | blocks_565_640x360_x2 | 30912 | us | ±0.3% | 20 |
|  | doom_888_320x200_x3 | 20308 | us | ±1.3% | 20 |
|  | integer_888_320x200_x3 | 17120 | us | ±0.1% | 20 |
|  | solitaire_565_640x480_x1 | 18962 | us | ±2.6% | 20 |
|  | copy_888_1280x720 | 62334 | us | ±0.4% | 20 |
|  | expand_l8_320x200 | 1660 | us | ±20.4% noisy | 20 |
|  | doom_expand_and_scale | 21961 | us | ±0.5% | 20 |
| `display.ppa.soak` | frames_a_second | 32.33 | /s | ±0.0% | 1 |
|  | written_mb_s | 89.39 | MB/s | ±0.0% | 1 |
| `fs.smallfiles` | create_ms | 0.1043 | ms | ±0.2% | 5 |
|  | create_max_ms | 0.113 | ms | ±2.2% | 5 |
|  | rewrite_ms | 0.09905 | ms | ±0.3% | 5 |
|  | rewrite_max_ms | 0.107 | ms | ±1.9% | 5 |
|  | remove_ms | 0.04795 | ms | ±0.7% | 5 |
|  | remove_max_ms | 0.052 | ms | ±3.8% | 5 |
|  | all_ms | 5.446 | ms | ±0.1% | 5 |
|  | stopped_ms | 0 | ms | ±0.0% | 1 |
|  | stopped_percent | 0 | % | ±0.0% | 1 |
| `fs.smallfiles dir=/flash` | create_ms | 16.23 | ms | ±75.1% noisy | 5 |
|  | create_max_ms | 232.5 | ms | ±96.9% noisy | 5 |
|  | rewrite_ms | 16.12 | ms | ±77.5% noisy | 5 |
|  | rewrite_max_ms | 177.8 | ms | ±134.9% noisy | 5 |
|  | remove_ms | 7.332 | ms | ±142.3% noisy | 5 |
|  | remove_max_ms | 10.61 | ms | ±876.5% noisy | 5 |
|  | all_ms | 1133 | ms | ±22.2% noisy | 5 |
|  | stopped_ms | 1989 | ms | ±0.0% | 1 |
|  | stopped_percent | 26.05 | % | ±0.0% | 1 |
| `games.doom.timedemo` | fps | 54.19 | /s | ±0.0% | 1 |
|  | gametic_ms | 18.45 | ms | ±0.0% | 1 |
| `gui.drag` | drag_ms | 500.1 | ms | ±1.7% | 5 |
|  | drop_ms | 33.36 | ms | ±0.0% | 5 |
| `gui.glyphs` | scalar_cycles_a_cell | 808.3 | cycles | ±9.8% noisy | 5 |
|  | pie_cycles_a_cell | 406.9 | cycles | ±3.2% | 5 |
| `gui.icons` | rom_each | 107684 | cycles | ±0.9% | 5 |
|  | psram_each | 48527 | cycles | ±0.5% | 5 |
|  | rom_again | 26702 | cycles | ±2.0% | 5 |
|  | psram_again | 26772 | cycles | ±0.4% | 5 |
|  | internal_again | 26680 | cycles | ±0.1% | 5 |
| `gui.scroll` | scroll_us | 15378 | us | ±0.8% | 5 |
| `gui.shuffle` | change_us | 29339 | us | ±7.4% noisy | 5 |
|  | change_max_us | 66612 | us | ±12.5% noisy | 5 |
| `gui.text` | redraw_us | 27402 | us | ±0.3% | 5 |
| `images.codecs` | png_encode_ms | 1437 | ms | ±0.2% | 5 |
|  | png_decode_ms | 738.8 | ms | ±0.6% | 5 |
|  | jpeg_encode_ms | 32.77 | ms | ±1.5% | 5 |
|  | jpeg_decode_ms | 31.07 | ms | ±0.1% | 5 |
|  | jpeg_software_decode_ms | 244.9 | ms | ±1.1% | 5 |
| `kernel.sleep` | other_late_us | 851 | us | ±24.9% noisy | 5 |
|  | fifo_late_us | 30 | us | ±5.0% | 5 |
| `kernel.syscall` | getppid_ns | 8534 | ns | ±1.1% | 5 |
| `memory.psram.cpu` | write_mb_s | 31.05 | MB/s | ±1.0% | 5 |
|  | read_mb_s | 32.32 | MB/s | ±0.8% | 5 |
| `memory.psram.dma` | copy_mb_s | 71.05 | MB/s | ±1.0% | 5 |
| `memory.traffic` | psram_copy_mb_s | 31.6 | MB/s | ±1.7% | 5 |
|  | psram_set_mb_s | 44.67 | MB/s | ±2.1% | 5 |
|  | internal_copy_mb_s | 532.4 | MB/s | ±0.8% | 5 |
| `midi.decode` | times_realtime | 18.94 | x | ±1.7% | 5 |
| `net.loopback` | tcp_mb_s | 26.32 | MB/s | ±6.8% noisy | 5 |
| `programs.launch` | start_and_exit_ms | 12.16 | ms | ±5.3% noisy | 5 |
| `simd.blend` | psram_scalar_us | 361.9 | us | ±1.3% | 5 |
|  | psram_pie_us | 156.7 | us | ±1.7% | 5 |
|  | internal_scalar_us | 231.8 | us | ±0.2% | 5 |
|  | internal_pie_us | 34.44 | us | ±4.9% | 5 |
| `simd.glyphs` | scalar_us | 383.9 | us | ±2.0% | 5 |
|  | pie_us | 180 | us | ±8.9% noisy | 5 |
| `simd.orient` | quarter_right_ms | 281.3 | ms | ±1.3% | 5 |
| `simd.pngfilter` | scalar_us | 596.8 | us | ±2.6% | 5 |
|  | pie_us | 69.96 | us | ±0.0% | 5 |
| `simd.repeat16` | scalar_us | 9220 | us | ±5.2% noisy | 5 |
|  | pie_us | 1017 | us | ±13.5% noisy | 5 |
| `simd.scale` | scalar_ms | 795.6 | ms | ±0.3% | 5 |
|  | pie_ms | 298.9 | ms | ±1.6% | 5 |
| `simd.softjpeg` | full_scalar_ms | 951 | ms | ±0.1% | 5 |
|  | full_pie_ms | 719.8 | ms | ±0.6% | 5 |
|  | quarter_scalar_ms | 663.8 | ms | ±0.4% | 5 |
|  | quarter_pie_ms | 663.1 | ms | ±0.4% | 5 |
| `simd.swap24` | scalar_ms | 224.3 | ms | ±3.0% | 5 |
|  | pie_ms | 185.5 | ms | ±3.8% | 5 |
| `storage.sd.read` | direct_65536 | 15.94 | MB/s | ±0.4% | 5 |
|  | bounced_65536 | 5.653 | MB/s | ±0.5% | 5 |
|  | direct_512 | 1.372 | MB/s | ±1.1% | 5 |
|  | bounced_512 | 1.363 | MB/s | ±1.2% | 5 |

Not measured: `display.anim`: skipped (Resource is busy); `display.draw`: skipped (Resource is
busy); `display.fill`: skipped (Resource is busy); `display.present`: skipped (Resource is busy);
`net.latency`: skipped (Operation not supported).

- **Next:** step 5, new benchmarks: the surfaces and games first (a program's present at each
  format and scale, `libbmdsgame`'s drawing, a renderer-like loop into internal RAM and PSRAM),
  for Quake; then CPU, memory latency, kernel and storage throughput (§6).

## 14. As built: step 5, part 1: surfaces, games and the system's basics (2026-10-17)

43 benchmarks now (30 in the firmware, 13 from programs). New:

- **`surface.present`** (`benchprog surface`, tagged `screen`: it opens windows and goes full
  screen): six cases, a run presenting flat out for `ms=` (1000) with a 64×64 square moved each
  frame; each case's frames shown a second, the `present` call's time, and the rest of the
  system's CPU a frame shown (both cores' busy time from the counters, less the program's own).
  `case=` picks cases by name.
- **`game.draw`** (`benchprog draw`): `libbmdsgame` into a 640×360 canvas in PSRAM, in L8,
  RGB565 and BGR24: a fill, opaque, keyed, scaled (2×) and turned blits of a 64×64 sprite, a
  translucent fill (not L8), text.
- **`render.spans`** (`benchprog render`): Doom's inner loops, `R_DrawColumn` (a wall's column
  scaled through a colormap) and `R_DrawSpan` (a flat sampled along a row), for a 320×200 frame
  drawn into PSRAM and into internal RAM (`BMDS_MEM_FAST`), the textures in PSRAM.
- **`kernel.pipe`** (`benchprog pipe`): 4 MB through a pipe between two threads.
- **`memory.latency`** (`bmds_drivers`): a chain of dependent loads in a random cycle of 128-byte
  lines, in internal RAM and PSRAM by size.
- **`kernel.switch`** (`bmds_bench`): a task woken by a semaphore and answering by another,
  on the bench's core and on the other.
- **`fs.sequential`** (`bmds_storage`): a file written in 64 KB chunks and synced, then read
  back, through the VFS (`dir=` /tmp, `mb=` 4).

**On the board** (the desktop running):

| | |
|---|---|
| Doom-sized L8 320×200, full screen | 60 fps; `present` 101 µs; the system **10.4 ms** a frame (62 % of a core at 60) |
| Quake-sized L8 320×240, full screen | 60 fps; 114 µs; **12.2 ms** a frame (73 %) |
| L8 320×200 in a window | 60 fps; 332 µs; 5.1 ms a frame |
| Blocks' RGB565 640×360, full screen, all of it changed | **30 fps**: 22.9 ms a frame, more than a frame's time |
| BGR24 maximized, all of it changed | 30 fps; `present` **2.4 ms** (the cache written back); 2.2 ms a frame |
| BGR24 maximized, a square changed | 60 fps; 703 µs; 2.1 ms a frame |
| `game.draw` L8 / RGB565 / BGR24 | fill 45 / 23 / 15 Mpx/s; blit 56 / 23 / 17; **keyed 5.9 / 21.3 / 4.1**; scaled 4.6 / 17.6 / 3.5; turned 3.7 / 7.7 / 2.9; text 11-12 characters a ms |
| `render.spans`, a 320×200 frame | columns 2.6 ms into PSRAM, 2.1 ms into internal RAM; spans 2.9 ms either way |
| `memory.latency` | internal RAM 8.4 ns; PSRAM in the cache (64 KB) 8.5 ns, beyond it (256 KB, 4 MB) **806 ns** a load |
| `kernel.switch` | **21 µs** there and back on one core, 15.6 µs across cores |
| `kernel.pipe` | 151 MB/s |
| `fs.sequential` | /tmp 30 MB/s writing, 56 MB/s reading; the card (`dir=/sd0/p0 mb=8`) 2.6 and **4.9 MB/s** (raw 64 KB reads: 16 MB/s); `/flash` (512 KB) too small for 1 MB: refused, the partial file removed |

**What they say, for tuning (not done here):**

- **Full-screen L8 is affordable but not cheap:** Quake's frame costs 12.2 ms of the system's
  CPU on top of its own rendering; a whole-frame RGB565 picture at 640×360 can't reach 60.
- **Keyed blits in L8 and BGR24** run at a quarter of RGB565's rate: their fast path is missing.
- **PSRAM past the cache costs ~800 ns a dependent load** with the scan and the desktop reading
  it: a renderer's random reads (textures, a z-buffer) should stay within the cache or internal
  RAM. Doom's loops themselves are cheap (5 ms for a frame's walls and floors).
- **A task switch on one core costs more than across cores** (21 against 15.6 µs), and a system
  call 8.5 µs: the context's save and restore (stacks in PSRAM, PIE's and the FPU's registers) is
  the first thing to look at.
- **Reading the card through the VFS** gets a third of the raw rate.
- Doom's and Blocks' switches to full screen show as an underrun, a resync and a rebuild.

**Cost:** the image grew 3,056 bytes (the three firmware benchmarks).

**Still to come (§6):** CPU (CoreMark: its licence to check first), the memory's bandwidth by
block size, a mutex handoff and a queue's round trip, TCP to a peer, directory listing.


## 15. Tuning round 1: system calls and the clock; CoreMark (2026-10-17)

The first leads from §14 that `bench` itself measures: what a system call costs, and what the
system's clock costs underneath it. New benchmarks to see them:

- **`kernel.syscall`** (`benchprog`) reports `getpid_ns`, `getppid_ns` and `clock_gettime_ns`:
  the cheapest call, one that takes the process table's lock, and the clock.
- **`kernel.clock`** (firmware) compares `monotonic_read_ns` (ours) with `esp_timer_read_ns`, which
  is IDF's `esp_timer_impl_get_time()`.
- **`kernel.locks`** (firmware) times an uncontended `Mutex` lock and unlock, with the mutex in
  internal RAM and in PSRAM, and a semaphore given and taken.
- **`cpu.coremark`** (`dist/coremark`) is EEMBC's CoreMark at commit 1f483d5, Apache-2.0. Its core
  files and POSIX port are used as they are. Ours is `ee_printf.c`, which passes the output through
  and adds the score as `@bench` lines: `iterations_s` and `coremark_mhz`, using the clock from
  `bmds_system_info`. `ITERATIONS=0` makes it time itself to the 10 s a valid score needs, so its
  descriptor sets `runs=1 warmup=0`.

**What was changed:**

- **The signal checks every system call makes** on its way in and out (`KernelCall`) read whole
  sets with sequentially consistent atomics, and each of those loads is a fence on RISC-V.
  - `Signals::ending()`, `stopSignal()` and `dueCaught()` now read the pending sets first, with
    acquire loads, and return when nothing is pending, which is the usual case.
  - `ProgramThread::leave()` reads the terminating signal once and passes it to a new
    `isDue(thread, terminate)` rather than reading it again.
  - `prepare()` keeps the one ordering it needs: a sequentially consistent fence between clearing
    the interrupt and looking again.
- **`clock_gettime` makes no `KernelCall`:** reading a clock can't block, be interrupted or touch
  a process. `bmds_k_clock_get` calls `Process::clockGet()` directly.
- **The CRT's `ts_write` divided 64-bit nanoseconds by 10⁹** in software.
  - It now multiplies by a reciprocal, `mul_high64(ns >> 9, 0x44B82FA09B5A53) >> 11`, for
    non-negative values.
  - This was checked exact against the division on 2.2 million values: edges, powers of two and
    random values.
- **`esp_timer_get_time()` is ours across the firmware** (`-Wl,--wrap`, `bmds_kernel`).
  - IDF's version reads the systimer and then converts its 16 MHz ticks to µs with the ROM's
    `systimer_ticks_to_us()`, which is a software 64-bit division.
  - `Detail::monotonicUs()` reads the same counter (a snapshot, then lo/hi/lo) and shifts by 4, in
    IRAM.
  - FreeRTOS's run-time statistics read this clock at every context switch.
  - The boot banner warns if the two clocks ever differ by more than 2 µs; neither the bench runs
    nor the boot banner check showed a difference.

**On the board** (same image apart from the change):

| | before | after |
|---|---|---|
| `getpid()` | 4.2 µs | **2.5 µs** |
| `getppid()` (process table lock) | 9.0 µs | **7.4 µs** |
| `clock_gettime()` | 5.9 µs | **1.8 µs** |
| a clock read in the firmware | 794 ns (IDF's) | **672 ns** |
| `kernel.switch` same core / across | 21.0 / 15.6 µs | 20.4 / 14.6 µs |
| `kernel.pipe` | 151 MB/s | 162 MB/s |
| `cpu.coremark` | | **1062 iterations/s, 2.95 CoreMark/MHz** (-O2, GCC 15.2, code in PSRAM) |

**What's left in those numbers:**

- **The systimer read itself is ~650 ns:** the snapshot, then waiting for the value to be valid.
  This is most of what remains of a clock read. A cheaper clock would mean `mcycle`, which is per
  core and stops in WFI. That is not worth it yet.
- **`getppid()`'s extra 5 µs is the process table's mutex.** An uncontended `Mutex` pair costs
  4.1 µs (`kernel.locks`), a semaphore pair 2.5 µs. Where a mutex's memory is makes no
  difference. The time is inside FreeRTOS's critical sections, which the tick profiler can't
  sample (`docs/platforms/esp32p4.md` §4c).
- **Tried and not kept:** `CONFIG_FREERTOS_IN_IRAM`. It bought 8 % on a switch and a lock, not
  worth its IRAM.
- **Next leads:**
  - The task switch and lock costs, which need cycle counts inside the port's critical sections.
  - The full-screen L8 conversion: 10-12 ms of the system's time a frame.
  - Keyed blits in L8 and BGR24.
  - Reading the card through the VFS.

**ABI:** none changed. The CRT's `ts_write` is inside the program, and `bmds_k_clock_get` keeps its
meaning.

## 16. Tuning round 2: locks, through FreeRTOS's critical sections (2026-10-18)

**Where a lock's time goes.** A temporary benchmark timed each part of a lock in cycles. An
uncontended mutex lock and unlock was 1,459 cycles (4.1 µs). It enters four FreeRTOS critical
sections: the queue's lock on each side, plus `pvTaskIncrementMutexHeldCount()` and
`xTaskPriorityDisinherit()` for priority inheritance. A semaphore give and take enters two. Each
section cost **283 cycles**:

- ESP-IDF's port masks interrupts, then calls `spinlock_acquire()`, which masks them again and
  restores them around its own compare-and-set. `esp_cpu_compare_and_set()` is also a call into
  flash. Leaving undoes both masks again.
- On this silicon (P4 v1.3, `ESP32P4_SELECTS_REV_LESS_V3`) a mask isn't a CSR swap. It is MIE
  off, a memory-mapped read and write of the CLIC threshold, a read back, then MIE on: 63 cycles
  a mask and restore as IDF's functions. The parts alone: the threshold read 10 cycles, a write and
  read back 14, MIE off and on 17, an `lr`/`sc` compare-and-set 21.

**What was changed:** `critical.cpp` (`bmds_kernel`) replaces `xPortEnterCriticalTimeout()` and
`vPortExitCriticalMultiCore()` (`-Wl,--wrap`):

- One mask, an inline `lr.w.aq`/`sc.w`, the nesting count, and on the way out a release store and
  one unmask. All of it is in IRAM.
- The state is IDF's own (`port_uxCriticalNesting`, `port_uxOldInterruptState`, the spinlock's
  owner and count). A section entered by unwrapped code in `port.c` can be left by ours and the
  other way round, and `spinlock_acquire()` used directly on the same lock still works.
- **MIE must stay off around the threshold write.** A first version left it on, reasoning that an
  interrupt saves and restores the threshold. That holds only without hardware nesting, and the P4
  nests in hardware (`SOC_INT_HW_NESTED_SUPPORTED`): `vectors.S` never touches the threshold. An
  interrupt taken after the write but before it counted switched to another task, which then ran
  masked with no section open. The interrupt watchdog reset the board within minutes of
  benchmarking.
- IRAM shrank 452 bytes: the linker drops IDF's two functions, which were bigger.

**On the board:**

| | before | after |
|---|---|---|
| a critical section (enter and leave) | 283 cycles, 776 ns | 178 cycles, **470 ns** |
| `Mutex` lock and unlock | 4.14 µs | **2.78 µs** |
| `Semaphore` give and take | 2.45 µs | **1.81 µs** |
| `kernel.switch` same core / across | 20.4 / 14.6 µs | **16.2 / 11.6 µs** |
| `getppid()` (the process table's mutex) | 7.4 µs | 5.5-5.8 µs |
| `kernel.pipe` | 162 MB/s | 187 MB/s |

**Checked:** every quick benchmark (25), progtest's `threads`, `spawn`, `poll`, `timers`, `time`
and `signals`, Doom's timedemo and `surface.present` ran with no failure or reset.

**What's left in a lock:** of a mutex pair's ~1,000 cycles, about 700 are its four sections, near
their floor now. The rest is FreeRTOS's queue code. The next step would be a mutex of our own, with
a compare-and-set fast path that enters no section when the lock is free, keeping priority
inheritance on the slow path. That is a design of its own, not started.

**Doom's timedemo depends on the core it lands on, not on these changes.** While tuning, Doom
measured 41-44 fps against the 57.6 baseline. A bisect seemed to blame `monotonicUs()` moving to
IRAM, but repeated runs showed the bisect was noise:

- Doom is display-paced: it spends most of its time blocked in presents. On **core 1**, with the
  display's conversion, it got 44 fps three times in three runs. Forced to **core 0** on the same
  firmware, it got 53-58 fps.
- The loader pins a program's first task to core 1 unless core 1 was more than 15 points busier
  over the last second (`processes.md` §3.12a). Cheaper locks and clock reads made core 1 less
  busy, so Doom started landing there.
- `surface.present` (benchprog) shows the same split: 49 against 60 fps.
- A few minutes after a boot Doom also runs a little slower (54 against 57.6).
- Program benchmarks' results therefore depended on where the loader put them. Settled in §17:
  a program that draws flags itself and always starts on core 0, and the results record the core.

## 17. Display programs off the display's core, and the core in the results (2026-10-18)

The user's decision after §16: keep programs that draw to the screen off core 1, and account for
a program's core in the benchmarks.

- **Why core 1 is wrong for them:** a surface's presenter (the L8 or RGB565 conversion, `surface`,
  priority 4) and the scanout run on core 1. A game whose thread shares that core waits for its
  own conversion; on core 0 the two run in parallel. Doom: 44 fps against 53-58 (§16).
- **Telling the loader:** it can't know at a program's start, and a thread can't move once the
  loader has pinned it. So the program says so: `BMDS_PROGRAM_DISPLAY` (bit 0 of the program
  note's `flags`, which were all refused before), from `bmds_add_program(... DISPLAY)`,
  `BMDS_DISPLAY=1` for bmds-cc, or `display = true` in a dist package (the linker script's
  `__bmds_program_flags`). Doom, Blocks, Lucky Duck, `surfdemo` and `bounce` set it.
- **The loader** (`coreForProgram()`, `processes.md` §3.12a) starts a flagged program on core 0
  always (the one `DisplayCore` isn't), whatever the load; any other as before. libbmdsgame's
  mixer and music threads then go to core 1, as they pin themselves to the game's other core.
- **Benchmarks:** `ProgramLauncher::Request::core` reports the core a program's first thread
  got. `bench` prints `on core N` for a program's benchmark and writes `"core"` in its record
  (appended to the format). `tools/bmds-bench run` marks a result whose program ran on another
  core than the run it is compared with `OTHER CORE (core 0, was 1)` instead of judging it
  better or worse; `compare` adds the same, `history` and `table` show the core.
- **`surface.present`** runs `benchprog-display`: the same program built with the flag, so it
  measures where a game runs. `benchprog`'s other benchmarks keep the ordinary choice.
- **Measured:** Doom's timedemo on core 0, 57.8 and 57.3 fps (two runs); `surface.present`'s
  full-screen Doom size at 60 fps (49 on core 1). Blocks: the game's thread on core 0, its
  audio on core 1. `kernel.syscall` (unflagged) still on core 1. Host tests cover the flag's
  parsing (an unknown bit is still refused).
