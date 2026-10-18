# Bench: one system benchmark for all tuning

Status: **step 1 built** (2026-10-17, the user's request; decisions agreed the same day, §10). Replaces the
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
| `display test`, `fill`, `present`, `anim`, `stress`, `ppa` (`path`, `soak`, hidden `blitcost`) | `display.draw`, `display.fill`, `display.present`, `display.anim`, `display.stress.*`, `display.ppa.*`, `display.blitcost` | `display stats`, `rows`, `try` stay |
| `win bench [CELLS]`, `bench icons`, `textbench`, `shuffle`, `drag`, `scroll` | `gui.glyphs`, `gui.icons`, `gui.text`, `gui.shuffle`, `gui.drag`, `gui.scroll` | `win stats` and `check` stay |
| `progtest sched`, `rtnet`, `net`, `synthload`, `mididecode`, `image`'s time | `kernel.sleep`, `net.rt.roundtrip` (peer), `net.loopback`, `audio.synth.load`, `midi.decode`, `images.decode` | a program bench (`benchprog`, §4f); `progtest` keeps its correctness tests |
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

