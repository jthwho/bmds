# Bench: one system benchmark for all tuning

Status: **steps 1 to 3 built** (2026-10-17, the user's request; decisions agreed the same day; §10-§12). Replaces the
benchmarks spread across `sd`, `fsbench`, `simd`, `memtest`, `display`, `win` and `progtest`
(§3). Comes before Quake (`games.md`), whose renderer and presentation need a baseline.

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
  - `benches`: one entry a bench run: its name, the parameters used, the status, `results` (a
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

