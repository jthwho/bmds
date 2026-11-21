# ESP32-P4: tricks and pitfalls

What working on the ESP32-P4 (its cores, its accelerators, its toolchain and ESP-IDF) has taught,
kept so it isn't learned twice. **Read before low-level work on this chip**: vector kernels, DMA,
caches, the accelerators, ESP-IDF's drivers. Board facts (pins, wiring, the board's own gotchas)
are `docs/HARDWARE.md`'s; designs and measurements in full are in the devplans cited.

Each entry says what is true, how it was found, and where it is used. **Add to it** when something
costs a debugging session or a measurement surprises; correct it when a newer chip revision or
ESP-IDF changes the answer. Our silicon is **v1.3** (`docs/HARDWARE.md`); several entries are about
revisions before 3.

## 1. The PIE vector unit (`devplan/simd.md`)

**The documentation is for a newer revision.** The TRM's chapter 4 describes `Xespv2p2` (revision
3); this chip and the toolchain are `xespv2p1`. Instructions differ in operands (v2p2 adds `sat`
and `rm`), some exist only in one, and semantics differ (below). The TRM is the reference for what
an instruction means, the assembler for whether it exists here (an "illegal operands" error), and
the board for what it does: **check every kernel on the board** (`simd test`).

- **Only x8-x15 and x24-x31 are PIE's address registers.** In inline assembly use the `"cr"`
  constraint (x8-x15), never `"r"`: GCC picks any register and the assembler refuses it.
- **Misaligned access is a CFG setting.** By default a 128-bit access silently clears its low 4
  address bits. `esp.movx.r.cfg`/`esp.movx.w.cfg`: bit 1 makes loads misaligned-safe, bit 0 stores
  (as ESP-DSP sets them).
- **CFG is the core's, not the task's: set it once, never around a kernel.** FreeRTOS's lazy PIE
  switch saves Q0-Q7, QACC, UA_STATE, XACC, SAR, SAR_BYTES and the FFT width, but not CFG. Our
  kernels used to read CFG, set the two bits and put the old value back; two tasks interleaving on
  a core could put back a stale 0 under the other mid-loop, whose misaligned loads and stores then
  quietly lost their low address bits: pictures right in shape, colours scrambled in columns (the
  screenshots of `games.md` §20b, §22). Found by `simd stress` (a value of CFG's own didn't
  survive the switches; with the kernels as they were, a blend case failed within 20 s). Now
  `Simd::setUp()`, a boot stage, sets CFG to 3 on both cores (from a short task pinned to each)
  and nothing writes it again; it reads 3 across millions of switches, PIE's disable and re-enable
  at each switch included (`devplan/simd.md` §6).
- **`esp.vsub.u8` (and `vadd`) saturate** on v2p1, they don't wrap. `esp.vssubs.u8` is vector by
  scalar register, not vector by vector. Wrapping byte arithmetic: `|a − b|` as
  `vmax − vmin` (exact), then a `vcmp` mask to choose `d` or `256 − d` (`~d + 1`, safe for d ≥ 1).
  PNG's filters do this (`pngfilter.cpp`).
- **`esp.srcmb.*.qacc` floors** (no rounding mode on v2p1). Round by adding the half as one more
  multiply-accumulate before shifting out: a vector of 128 (or 0x8000, or 512) times a vector of
  ones. The scalar twin adds the same half.
- **`esp.vmul.u16` shifts by SAR**, so `(t × 257) >> 16` divides by 255 exactly in one
  instruction: `((t + 128) × 257) >> 16 == (t + 127) / 255` for every t in 0-65025
  (checked exhaustively).
- **Three-plane shuffles exist:** `esp.vunzipt.8 qa, qb, qc` splits 48 bytes (16 RGB pixels) into
  three planes; `esp.vzipt.8` weaves them back in any order. With `vunzip.8` (two planes) these
  are the only byte shuffles: there is no general permute.
- **Widening and narrowing:** `esp.vldext.u8.ip qa, qb, ptr, imm` loads 16 bytes as two vectors
  of 8 u16. Narrow u16 lanes (0-255) to bytes with `esp.vunzip.8 q, qz` against a zero register;
  the zero register stays zero (its odd bytes are the lanes' high bytes, all zero).
- **`esp.vsat.s16 q, q, lo, hi`** clamps to `[lo, hi]` as you'd hope; its TRM pseudo-code reads as
  if it didn't.
- **Accumulators:** QACC (two 256-bit halves) holds 8 lanes of 64 bits for 16-bit MACs or 16 of 32
  for 8-bit ones; XACC is one 40-bit sum of all lanes (`esp.vmulas.u8.xacc` with a vector of ones
  sums 16 bytes). There is one QACC: a kernel needing two accumulations runs them one after the
  other. **`esp.mov.u8.qacc q`** loads QACC from 16 bytes (zero-extended): a start value, such as
  a rounding constant, without a multiply-accumulate, or one pass's result as the next pass's
  start. The TRM's v2p2 `srcmb` also writes its shifted result back into QACC; what v2p1's does
  isn't checked, so kernels don't rely on QACC after a `srcmb` (they reload it).
- **Exact ÷255 in u8 lanes:** `(t + 127) / 255 == (t + 128 + ((t + 128) >> 8)) >> 8` for t up to
  65025: two QACC passes of u8 products, 16 lanes at a time (the alpha blend, `pixels.cpp`).
- **No floating-point vectors.** Audio and the float decoders aren't PIE's.
- **Registers live between asm statements**, as GCC doesn't know they exist: constants can be
  loaded once and used by later statements, if no PIE-using function is called between.
- **PIE's 128-bit accesses need L2 RAM or PSRAM: not the LP RAM, not the TCM.** ESP-IDF's P4
  heap layout says so ("TCM and RTCRAM memory regions cannot satisfy 128-bit data access"), yet it
  registers the LP RAM (32 KB) with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` as a last resort, and
  the TCM likewise. So `heap_caps_malloc(INTERNAL | 8BIT)` returns LP RAM once the L2 RAM is
  short, and a PIE kernel over it runs ~20× slower and reads garbage. Found as PNG screenshots
  streaked down the columns (filtering 2-6 s instead of 0.03), with Doom or a full-screen surface
  holding internal RAM: the PNG writer's rows had landed there. `MALLOC_CAP_SIMD` would exclude
  both, but ESP-IDF registers its PSRAM malloc reserve (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`,
  32 KB of L2 RAM) without it; every byte of L2 RAM is DMA-capable and neither LP RAM nor TCM is,
  so BMDS's internal spaces ask for **`MALLOC_CAP_DMA`** (`boarddef.h`). Plain `malloc()` can
  still get LP RAM: fine for scalar code, never for a buffer a kernel touches.
- **Lazy save, per task:** FreeRTOS saves PIE's registers the first time a task uses PIE (a trap);
  **never use PIE in an interrupt handler** (it aborts).

## 2. The hardware loop: don't

**`esp.lp.setup` (Xesploop) isn't safe on revisions before 3.** A hardware loop preempted part-way
can resume wrong: ESP-IDF's `SOC_CPU_HAS_HWLOOP_STATE_BUG` and its context-switch workaround. The
scaler's first version failed `simd test` at random cases (2026-10-10); with a branch loop
(`addi` and `bnez`, one more instruction a pass) every run passed, and it was no slower. The glyph
renderer had used it since it was written: the same latent bug, fixed then. **Use branch loops**
until silicon v3; then test again.

## 3. Caches and DMA

- **Before a DMA reads memory the CPU wrote: write the cache back** (`esp_cache_msync`,
  `ESP_CACHE_MSYNC_FLAG_DIR_C2M`). **Before a DMA writes memory: make sure no dirty line covers
  it**, then invalidate after. A buffer just allocated may hold another's dirty lines; one evicted
  while the DMA writes lands stale bytes in the middle of the result. This corrupted JPEG encodes
  at random (2026-10-10): ESP-IDF's JPEG encoder invalidates its output only after the DMA. Our
  `JpegCodec::encode()` writes the buffer back before (`devplan/simd.md` §3a).
- **A cache operation runs with interrupts off:** sync large buffers in pieces (64 KB in the
  display and screenshot code).
- **The CPU's PSRAM traffic can't be ranked below the 2D-DMA's** (the caches bypass the
  interconnect): drawing is in internal RAM and copied by DMA (`devplan/display.md` V9).
- **PSRAM is slow for the CPU in bulk**, though it runs at 200 MHz (hex mode): the CPU gets about
  70 MB/s for a copy-like loop (a 1080p RGB swap, 12 MB of traffic: 223 ms scalar, 180 ms with PIE,
  which does 16 pixels a step) and about 84 MB/s reading (the alpha blend from PSRAM: 145 µs an
  icon against 36 µs from internal RAM). The display scans surface 0 from the same PSRAM. A kernel
  that reads and writes PSRAM is memory-bound long before it is compute-bound: measure the scalar
  and PIE times before tuning instructions. Column-order access thrashes the cache (turning a
  picture: 1525 ms by rows, 277 ms in 32 × 32 tiles).
  The same from a userspace game (2026-10-15, `luckyduck.md` §14): `memcpy` of a 1280×720 RGB565
  frame PSRAM to PSRAM 64 ms (~28 MB/s each way), a 100×140 keyed card blit 0.8 ms, a redrawn
  pixel ~230 ns with two layers (each a source read, the destination's line filled and written
  back): at 1280×720, what a frame touches decides its rate, not the code.
  Measured again in Lucky Duck's Video Poker (2026-10-16, `luckyduck.md` §23):
  - The felt restore (a row copy PSRAM to PSRAM) cost about 180 ns a pixel during a deal, with the
    scan and the window manager's composing reading PSRAM too.
  - Filling those rows (writing only) was no faster: the cache reads a line in before it is
    written, so a write costs nearly what a copy does.
  - The ways out were touching fewer pixels (no felt under opaque cards; no copy where a frame
    repaints anyway) and not the instructions. A turned blit's 64-bit steps and per-pixel tests
    cost little next to its memory.
  Then the 2D-DMA took the copies (`games.md` §25, 2026-10-16): a program's felt restore and its
  double buffer's catch-up copy by `BMDS_SURFACE_BLIT`. Video Poker's busiest seconds went from
  about 5 skipped ticks to one in 25 seconds.
  - **The cache around a DMA write into a CPU-drawn buffer:** write back and invalidate each
    piece's byte range before (unaligned is allowed for C2M), invalidate after, rounded out to
    128-byte lines (M2C refuses unaligned ranges; buffers aligned and padded to lines keep that
    safe).
  - Nearby rows' ranges are merged into one operation, since each costs 4-5 µs.
  - **The L1's lines are 64 bytes, the L2's 128: write back and invalidate the same whole L2
    lines.** A write-back of an exact, unaligned range writes back the L1's 64-byte lines it
    touches. An invalidate rounded out to 128 bytes then drops the other half of a line, dirty in
    the L1 with the CPU's writes. Found by `blittest` (`games.md` §25g): a sprite lost 32 pixels
    of a row now and then.
  - **Internal RAM a DMA reads is cached too:** a fill's line written by the CPU must be written
    back.
- **A big table read from flash competes with every other flash read:** SVOX Pico synthesizing flat
  out (into a file) from its 1.4 MB voice mapped in `/rom` slowed the icon blend from the ROM
  296 → 380 µs an icon (28 %); with the voice copied into PSRAM, 322 µs (9 %). Speaking at real
  time (a sixth of that work) made no difference measurable: 332 against 346 µs
  (`devplan/speech.md` §12, 2026-10-12).
- **A software prefetch doesn't help:** a scalar load 192 bytes ahead in the blend's loop changed
  nothing (2026-10-10).
- **Use the L2 cache's 128-byte line** (`CONFIG_CACHE_L2_CACHE_LINE_128B`, in
  `sdkconfig.defaults` since 2026-10-18; the same 128 KB). Streaming through the PSRAM fetches
  half as many lines: Doom's timedemo 55 → 65 fps, PSRAM copies +69 %, the card through the VFS
  +64 %, drawing +20-39 %, only a random dependent load 6 % slower (`devplan/bench.md` §19).
  An earlier try had `reconfigure` put the 64-byte line back; with the line in
  `sdkconfig.defaults`, a fresh configuration keeps 128 (checked). **Anything aligned "to a cache line"
  must use 128**: `Memory::DmaAlign`. Two places had 64 and broke when the line grew: the
  hardware JPEG decoder refused the buffers (ESP-IDF checks the cache's real line), and the
  SoundFont loader's DMA-friendly placement silently fell back to the bounce buffer.
- **The two cores share one L1 data cache** (their instruction caches are their own): what one
  core writes, the other reads coherently through the cache. A write-back is for a DMA, never for
  the other core.
- **A write-back costs what it writes:** 64 KB of dirty lines in PSRAM ~250 µs, clean ~5 µs, an
  invalidate ~7 µs (`memory.cache`). Each runs with interrupts masked for its whole length
  (ESP-IDF holds a critical section around it), so a big dirty one is an interrupt latency.
- **The kernel runs cold after a program's start:** the firmware runs from flash (DIO, 80 MHz)
  through the 128 KB L2, and a launch's 128 KB copy of private pages and the program's run push it
  out. In `exec()` a `Vfs::stat()` took 1,183 µs, then 360 µs each repeated straight after; every
  step of a launch pays the same way (`devplan/programs.md` §19). QIO flash (since 2026-10-19)
  halves each miss's transfer: `programs.launch` 8.6 → 6.6 ms, a BusyBox launch 8.6 → 6.5 ms;
  benchmarks that run warm (syscalls, TCP loopback, the synth, the codecs, CoreMark) didn't move.
- **Flash at 120 MHz hangs at start-up on this board** (2026-10-19, P4 v1.3, IDF v6.1-dev, QIO):
  `CONFIG_ESPTOOLPY_FLASHFREQ_120M` (experimental, behind `IDF_EXPERIMENTAL_FEATURES`; the image
  header still says 80m and the app switches after tuning) built with `SPI_FLASH_HPM_ENA` and
  `SPI_FLASH_HPM_DC_DISABLE` (the defaults turn IDF's `#warning`s into errors). The app stopped
  after `flash HPM: High performance mode of this flash model hasn't been supported` and `MSPI
  Timing: Enter flash timing tuning`, silent from then on (USB download mode still worked: the
  80 MHz build flashed back). The W25Q128JV's quad reads are rated to 133 MHz, so the hang is in
  the tuning or the board's signal path at 120 MHz, not settled. Stays at 80 MHz.
- **QIO images say DIO:** with `CONFIG_ESPTOOLPY_FLASHMODE_QIO`, ESP-IDF still writes `dio` in the
  image headers (`esptool --flash-mode dio`); the bootloader switches the flash to quad as it
  starts (`boot: SPI Mode : QIO`, then the app's `spi_flash: flash io: qio`). The header's byte
  isn't the way to check.
- **Code written through the data cache must be written back before it runs:** each core's
  instruction cache fetches through L2 from memory, not from the (shared) L1 data cache. Private
  program pages written through their window addresses and left dirty ran libgcc's `lui` as the
  file had it (a load from the bare offset); writing back the range below the writable data fixed
  it (`devplan/programs.md` §19).
- **The LP RAM (RTC memory, 0x50108000) isn't cached:** the PMA entry that makes memory
  cacheable stops at `SOC_PERIPHERAL_LOW` (0x50000000; `cpu_region_protect.c`), so a store there
  reaches it at once, and RTC_NOINIT data survives any reset short of power-up with no write-back
  (unlike PSRAM's `.noinit`, which the log writes back before a software reset and loses on a
  hardware one). The records kept across resets use it (`devplan/keptstate.md`); a USB
  control-line reset, `reboot` and a panic all kept them (2026-10-29).
- **But the LP RAM is slow: about 500 ns a 32-bit store, reads too** (`memory.rtc`,
  2026-10-29): a 100-byte line copied in takes 14.5 µs, 4 KB 518 µs. Keep records in ordinary RAM
  and copy them in now and then (`Kept`), never per event. For the log, writing a line's cache
  lines back to PSRAM is cheaper: 2.8 µs by the ROM's address calls, 3.8 µs through
  `esp_cache_msync()` (which holds the cache's spinlock: use it whenever the other core runs, as
  the ROM's calls and IDF's share the cache's operation registers).
- **The bootloader must fit 24 KB** with the partition table at 0x8000 (`check_sizes.py` fails the
  build at 0x6000). `CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC` took ours 16 bytes over; dropping
  `CONFIG_BOOTLOADER_COMPILE_TIME_DATE` gave back 64.
- **The AXI-GDMA is no faster way to copy PSRAM to PSRAM in 64 KB pieces:** copying a launch's
  two private pages with `esp_async_memcpy()` (32 KB transfers, a zero page as the bss's source)
  took ~8 ms against the CPU's 2.1 ms. The steps after it ran ~0.6 ms faster with the caches left
  alone, which doesn't pay for it (2026-10-19, `devplan/programs.md` §19).
- **A whole 640×360 L8 picture copied PSRAM to PSRAM by the CPU, each frame, is ~5 ms** beside a
  full-screen presenter (demo1's boot: 10.5 ms a frame with it, 5.0 with the 2D-DMA copying it,
  `bmds_game_dma_copy()`, core 0 66 % → 20 %). The 2D-DMA, unlike the AXI-GDMA above, wins for
  a picture a frame: it runs while the CPU draws (2026-11-07, `devplan/demo.md` step 6).
- **Texture lookups at every pixel of the screen are memory-bound:** a full-screen rotozoom
  (`bmds_canvas_texture_affine()`, ~9 instructions a pixel) took 21 ms with a 64 KB texture, 16 ms
  with 16 KB, beside FM and the presenter (the shared L1 data cache); a texel for each 2×2
  pixels, two rows of word stores, 9.5 ms (2026-11-07, `devplan/3d.md` step 4).
- **Internal RAM's uncached alias** (`CACHE_LL_L2MEM_NON_CACHE_ADDR`) is no way to skip a
  write-back: writing a full-screen strip through it took twice as long (5.7 ms against 2.8 with
  the write-back, `display.expand`).

## 4. The accelerators (`devplan/images.md`)

- **JPEG decoding is baseline only**, and it writes whole pictures with each side rounded up to
  the MCU (8 or 16, by the sampling: not always 16). ESP-IDF refuses pictures whose width × height
  isn't a multiple of 8. Output buffers must be cache-line aligned in address and size.
- **ESP-IDF's YUV-to-RGB in the 2D-DMA is video's limited range** (Y 16-235), which stretches a
  JPEG's full-range colours. Only RX channel 0 converts colour. BMDS loads full-range coefficients
  by wrapping `dma2d_configure_color_space_conversion` at link time (`-Wl,--wrap`); `Decode::
  limitedRange` keeps ESP-IDF's for video.
- **Ask the JPEG decoder for a format it makes itself** (BGR24, RGB24, RGB565; gray for a gray
  JPEG), laid out as `image_info` says (`align`, `stride`, `bytes`): it then decodes straight
  into the program's buffer. Any other format (BGRA32) is decoded into a buffer of the codec's
  own and converted, so the picture's memory is needed twice. A 1920×1080 JPEG asked for as BGRA32
  failed with `-BMDS_ENOMEM` while the desktop ran: PSRAM had 6 MB free and its largest block was
  4 to 6 MB, well under the 8 MB plus the codec's 6 MB. Preso asks for BGR24, and RGB565 if that
  doesn't fit (found 2026-11-03, `devplan/preso.md` §14).
- **The JPEG encoder stops at its output buffer's end and reports success**: a stream that fills
  the buffer may be cut. Give it room, and treat a stream near the end as cut.
- **The PPA** (to explore further): its scale factors are 16ths (no exact sizes); its full-range
  YUV input refuses 4:2:2 on this revision and never completes on 4:2:0 (it writes the picture,
  then no done interrupt). Its blocking mode then waits forever: use non-blocking mode with a done
  callback and a timeout.
- **2D-DMA:** 3 TX and 2 RX channels on this revision; TX 0-2 convert colours
  (`devplan/display.md`). Its colour conversions are RGB888 to and from RGB565 and YUV: nothing
  narrows 32-bit pixels to 24.
- **The 2D-DMA's TX and RX descriptors needn't have the same shapes:** only the bytes must agree.
  A TX descriptor a row naming the same source row k times, under one RX block k times taller,
  repeats rows down without the CPU copying them (`BlitChain::addRepeated()`, as fills send one
  line a row). Windows scaled up draw this way: a k-th of the strip expanded and written back
  (`devplan/games.md` §24).
- **The 2D-DMA's PSRAM reads cost; its descriptors don't** (`display.fillcost`,
  `devplan/wallpaper.md` §6f, 2026-10-26): 1280×692 RGB888 into PSRAM, the scan running, took
  15.1 ms from one 3.8 KB row in internal RAM sent once a row (692 transmit descriptors), and
  24.0 ms from a source in PSRAM whatever its shape (1280×12 rows a descriptor, 256-pixel chunks
  of 64 rows, or a 24 KB pattern tile). Keep fill sources in internal RAM. (A pattern's tile is
  in PSRAM for its size, and still beats the CPU making rows: §6e.) The desktop's solid colour
  that looked slow was drawn twice.
- **The PPA's scaler (SRM) interpolates, and is slow** (`devplan/games.md` §10, measured
  2026-10-14 with `display ppa`, the scan running): a 1-pixel checkerboard scaled 2× comes out
  gray (bilinear: its input macro blocks are 18×18 for 16×16, a pixel around), and no register
  selects nearest neighbour, so pixel art blurs at any factor. ~30-34 Mpixel/s written when
  scaling up, 14.8 at 1× (a 1280×720 RGB888 copy: 62 ms). GRAY8 is chip revision 3 only. Fine
  for photos and video; not for a game's frame.
- **The blender's CLUT** (L8 through a 256-entry palette to RGB888; IDF leaves L8 out) is exact
  and ~39 Mpixel/s: 1.65 ms for 320×200. Its **bypass mode with only the background channel
  never completes**, and a 2D-DMA job that never completes stays in the pool: every later job
  times out until a restart. Keep the foreground channel (an invisible A8 plane). **It keeps up
  with a 1280×360 frame at 60 Hz** (2026-11-06, `devplan/games.md` §27): fed indices already
  repeated across (PIE `vzip.8`, cheap) from internal RAM in ~12-row jobs, it expands a 640×360 L8
  game's whole frame every frame, and the presenter's CPU fell from ~16.8 to ~6.6 ms a frame; the
  CPU's own lookups had been ~12 ms with PSRAM shared by the scan, the 2D-DMA and the program.
  The CPU's remaining cost is reading the program's frame from PSRAM (~43 MB/s under that load).
- **A stuck 2D-DMA job can be ended: `dma2d_force_end()`, but only once it has its channels**
  (found 2026-10-15, `devplan/windows.md` §4k, after a display `BlitChain` job never finished
  and froze the desktop). It decides "in flight" by whether `trans->rx_chan`'s channel is busy,
  and `rx_chan` is only set when the job is picked. Before that it still names the previous job's
  channel, which another job may now hold, and ending that would stop the wrong transfer. Note
  in `on_job_picked` that the job was picked, and end only those. A job still queued can't be
  withdrawn. `BlitChain` does this for any job older than 250 ms.
- **IDF's 2D-DMA job costs more than its pixels when they are few** (`devplan/windows.md` §4n):
  the queue, the channels connected and set up in `on_job_picked`, the callbacks registered, the
  cache syncs and the semaphore wait: ~150 µs a job measured, 44% of the window manager's CPU when
  each GUI fill ran its own. Put many pieces in one job.
- **A job can't keep its channels without an end of frame:** with the last transmit descriptor's
  `suc_eof` clear, the receive channel raises `DESC_EMPTY` at the chain's end even when every byte
  arrived, and IDF's interrupt handler frees the channels on it (it frees on SUC_EOF, ERR_EOF,
  DESC_ERROR and DESC_EMPTY alike). `on_desc_done` doesn't help.
- **In one 2D-DMA job, the transmit side can read ahead of the receive side's writes:** a piece that
  reads what an earlier piece of the same job writes (a read-modify-write of an edge, a scroll)
  may read the old pixels. Run the job before.
- **Copies into the display's PSRAM buffers take x and widths in 4-pixel columns** (rows of RGB888
  start and end on 4 bytes; `Surface::Align`). A picture off the columns isn't copied at all:
  `Painter::pixels()` refused it with `Invalid`, so a canvas in a window's 6-pixel margin showed
  nothing while one at an aligned x worked (it cost a debugging round, 2026-10-25). Its edges'
  few columns are now patched (`Surface::patch()`: read, changed and written back by two jobs).
- **A surface present without damage costs about 12 ms at 1280×720**: damage 0 means "all of it",
  and the whole picture goes to the display. A program animating a part of the screen should pass
  its damage (up to 64 rectangles). And a `BMDS_SURFACE_BLIT` piece the height of the screen costs
  its rows' cache operations, so a frame that writes only what changed since its buffer last held
  one (the frame two before, with two buffers) is cheaper still. Found with Preso's transitions,
  20 frames a second to 48 (2026-11-03, `devplan/preso.md` §14).
- **What PSRAM takes, with the 720p scan reading 166 MB/s:** the 2D-DMA writes ~150 MB/s
  (RGB565 widened to RGB888: 50 Mpixel/s, a 1280×720 frame in 18.3 ms, over a 60 Hz frame's
  16.7 ms). The CPU reads it at only ~70-80 MB/s through the cache (doubling 640×360 RGB565
  with PIE: 1 ms with the source cached, 6.6 ms from PSRAM). Design for fewer bytes: rows
  repeated by the scan's descriptors rather than written twice, damage rather than whole frames,
  sources brought into internal RAM by DMA rather than read by the CPU.
- **Memory-bound work doesn't scale across the two cores; compute-bound work does** (measured
  2026-11-06, `devplan/3d.md` §13): splitting libbmds3d's triangle rasterizing between the cores
  took 12.3 ms to 10.1, but splitting its vertex transforms and triangle queueing (reads and
  writes of records in PSRAM, little arithmetic) gained 3%: the cores share one L1 data cache and
  contend for it. Split the arithmetic, not the copying.
- **The TCM against the L1, for data read every sample** (2026-11-07, `devplan/audio.md` §26): the
  TCM has no cache in front of it, so another core's traffic can't empty it, but a read there is
  slower than an L1 hit. FM's waveforms (read every sample, ~38 lines an operator a block, missed
  beside a busy core) in the TCM, with its voices' flags packed, took 32 voices 663 → 481 µs a
  block under load; its mix and sum (written every sample, a few
  lines that stay cached within a block) lost 5-10% there. Put in the TCM what is read all the
  time and can't stay cached; leave what a block keeps hot in cached RAM. And a block's scan of
  every voice's flag in cached RAM was a miss a voice beside a game: keep such flags packed.
- **The L1 data cache's manual preload doesn't pay for small reads** (2026-11-07,
  `devplan/audio.md` §22): `Cache_Start_L1_DCache_Preload()` of a wavetable voice's next ~128
  bytes of samples while the voice before rendered, then `Cache_L1_DCache_Preload_Done()` and
  `Cache_End_L1_DCache_Preload()`, made the synth 11% slower alone and 9% under load. It is one
  preload at a time for the whole chip; ESP-IDF itself uses the L2's for a whole bounce buffer.
- **The busy benches vary by launch** (2026-11-07): `synth.wavetable.voices busy=1` gave 866 to
  1,151 µs for one build across launches, ~1% within one. Compare kernel changes over several
  rounds after each flash.
- **A task's block costs more than it looks** (2026-11-07, `devplan/demo.md` §8c): the presenter
  waiting on a 2D-DMA fetch and then the PPA blocked twice a job, 60 times a frame; the task
  switches and their critical sections took back most of what the fetch saved (core 1's presenter
  57 → 55%). Waiting first on the longer job, after which the shorter is done and its wait doesn't
  block, gave 57 → 41%.
- **Whether a task has used the FPU or PIE** (2026-11-07, `devplan/processes.md` §3.12a): a task's
  coprocessor save area (`pxPortGetCoprocArea(task, false, FPU_COPROC_IDX)`, at the top of its stack)
  has `sa_enable`, a bit per coprocessor set by the port's trap on its first use and never cleared.
  Its affinity alone doesn't say: a task created pinned (`xTaskCreateStaticPinnedToCore`, as the
  loader places a program's first thread) looks the same as one pinned by a float, and only the
  latter must stay. `Thread::pinCurrent()` reads the bits.
- **Branches a candidate cost more than the candidates** (2026-11-07, `devplan/3d.md` §13): the
  light table's search went outwards from a brightness choosing the nearer side each step; two
  plain loops (up, then down) compare a fifth more candidates and take 23% less time on the board
  (36 → 28 ms), half on the host. The same lesson as the spans': a few-pixel span's call and
  checks cost more than its pixels.
- **A program's timings move up to ~10% from one launch to the next** (found 2026-11-06,
  `devplan/3d.md` §13 step 3): the same `benchprog` binary, pinned to one core, ran a frame in 9.5
  ms on one launch and 10.5 on the next, while runs within a launch agreed to ~2%; probably where
  its buffers and code land in PSRAM. Compare builds A/B, alternated over several launches,
  pinned, averaged: single launches made changes of no effect look ±0.5-1 ms.
- **musl's `sinf` and `cosf` work in doubles** (found 2026-11-06, `devplan/demo.md` §10): the
  reduction and the polynomial (`__rem_pio2f`, `__sindf`) are double-precision code, which this
  core does in software (`__muldf3`, `__subdf3`): 640 calls a frame were a quarter of a demo
  part's time. In a hot loop use a table (BMDS Demo #1's `dsin`: 4,096 floats) or a polynomial in
  floats.
- **Float to 64-bit integer is a library call through doubles** (`__fixsfdi` and
  `__fixunssfdi` call `__muldf3`, `__subdf3`...): a few per triangle cost 3-4 ms a frame in
  libbmds3d. Float to int32 is one instruction; keep fixed-point values in 32 bits (offsets from
  a nearby origin), and `nm` the library for `U __*df*` helpers after touching hot code.
- **Two cores reading PSRAM slow each other, and the display's scan makes it worse** (measured
  2026-10-28, `synth.module`, `devplan/synthmodule.md` §2a): wavetable voices reading 13 MB of
  samples cost about 10 µs a 64-frame block on one core, cache misses and all; on both cores at
  once, 7-30 % more each headless, 34-55 % more with the 720p scan running. Samples that fit
  the cache lose 9 %. A two-core figure holds only for the display state it was measured in.
- **PSRAM fragments:** after a large allocation failed and the font cache freed 19 MB, 30 MB were
  free but no block over 13 MB until a reboot. Take large buffers once, early (the synth module's
  arena), rather than counting on a big block late.
- **The DW-GDMA clears each descriptor's valid bit when it finishes the block**, and has no
  setting that stops it: a circular list has to be made valid again as it runs (the scanout does
  it from its block interrupts). `dw_gdma_ll_lli_set_block_markers(lli, en_intr, is_last,
  is_valid)` takes the interrupt first: passing the flags in another order marks descriptors
  invalid (it cost a test run of stalls, 2026-10-14). Its descriptors are written through
  their uncached addresses, ~240 cycles a store as measured: re-validating a 720-line list
  costs ~175,000 cycles a frame even as one store a descriptor.
- **Internal DMA RAM fragments: big blocks fail long before the total runs out.** With the
  desktop, a terminal and a program's surface open, a 46 KB block (720 DW-GDMA descriptors) wasn't
  to be had though more than that was free; IDF's `dw_gdma_new_link_list()` asks for one block.
  The scanout builds its list from blocks of 64 descriptors (4 KB), linked, and makes the new list
  before the link comes down, so a shortage changes nothing on the screen (2026-10-14).

## 4a. The Ethernet MAC (`devplan/network.md`)

- **One DMA channel each way, no AV queues.** The DMA feature register (`EMAC_HWFEATURE_REG`)
  reads `4b056725` on our chip (MAC version `00001137`): no AV feature, no additional channels,
  although ESP-IDF's `emac_reg.h` lists `CHANNEL1*`/`CHANNEL2*` registers with credit-based
  shapers (the GMAC's full configuration, not this one's). Real-time frames get no hardware queue
  of their own: the driver bounds how much bulk traffic sits ahead of them (§5.2 of the plan).
- **Also from that register:** no multicast hash filter (only the perfect address filters), no
  layer 3/4 filter, no flexible PPS, no RMON counters; IEEE 1588-2008 timestamps, checksum
  offload both ways, the alternate (enhanced) descriptors, RMII. Read it with `net`.
- **Eight address filters beyond the station's**, though the register file has room for 15
  (`emacaddr[15]`). Found by writing a value to each filter's low half, the last first, and
  reading them back (2026-10-11): `emacaddr[0..7]` (addresses 1-8) hold theirs, the rest read 0.
  ESP-IDF's `EMAC_LL_MAX_MAC_ADDR_NUM` (8) is right. The driver probes at start
  (`EthernetDevice::multicastFilters()`) and uses them for multicast groups; with more groups it
  sets "pass all multicast" (frame filter bit 4) and IP drops what nobody joined.
- **The receive FIFO is 256 bytes**, too small for store-and-forward (ESP-IDF turns it off):
  frames pass through as they come, and a bad one arrives with its error in the descriptor.
- **The transmit FIFO is about 256 bytes too, so checksum insertion works only for small frames.**
  With store-and-forward on and TDES0's CIC at 3, TCP segments arrived with good checksums up to
  264-byte frames and bad ones beyond (counted by the PC's `InCsumErrors` in `/proc/net/snmp`,
  bisected by payload size, 2026-10-11); the descriptor reported no error. Receive checking
  works at every size: in the MAC's loopback (`gmacconfig.loopback`; RMII's clock comes from the
  PHY, so it works without the line) good UDP frames up to 1400 bytes came back checked in RDES4,
  bad ones with RDES0's error summary set, and a UDP checksum of 0 passed (`devplan/network.md`
  §29).
- **Don't let the EMAC flush frames when the receive ring is empty:** with the operation mode's
  DFF clear (flushing on, ESP-IDF's choice), a ring run dry under a flood can wedge the receive
  path for good: the debug register reads 0x340 (receive FIFO full, the read controller "reading
  frame status"), the DMA waits with every descriptor, and everything after counts as FIFO
  overflow. A 5 ms stall of the receive task in a flood did it; stopping and restarting receive
  didn't clear it. With DFF set, a frame waits for a descriptor and later ones overflow; 20 stalls
  of 3-50 ms didn't wedge it (`devplan/network.md` §30b).
- **A receive interrupt a frame starves the receive task in a burst of small frames:** 64-byte
  frames at line rate come every 6.7 µs, and the handler on the task's core took half its time.
  Mask receive interrupts from the first until the task has drained the ring (status bits latch).
- **Descriptors through the uncached view:** internal RAM is also mapped uncached at its address
  + `SOC_NON_CACHEABLE_OFFSET` (0x40000000; ESP-IDF's GDMA lists use it). The EMAC driver reads
  and writes its descriptors there (the DMA gets the ordinary address), so no descriptor needs
  `esp_cache_msync` (4-5 µs each, interrupts masked); a `fence` orders the owner bit. The lines
  must be written back and invalidated once before, and never touched through the cache after.
- **The EMAC's DMA can't send from PSRAM.** Frames read from PSRAM underflowed (TDES0 bit 1) once
  longer than the FIFO, every one, aligned or not; the same frames from internal RAM (even at a
  2-byte offset) never did. Transmit buffers, and anything sent in place, must be internal
  (lwIP's heap is, for that: `devplan/network.md` §29).
- **Descriptors are 64 bytes**, one L1 line each, so each is synced alone; the timestamps are in
  words 6 and 7 (digital rollover: seconds and nanoseconds).
- **ESP-IDF's `emac_hal.c` is built only with its Ethernet driver**; BMDS uses `hal/emac_ll.h`
  (header-only) and `soc/emac_periph.h`'s pin tables, and does the bring-up itself.
- **Start the MAC and its DMA on link-up, stop them on link-down** (ESP-IDF's order; transmit:
  MAC then DMA, the other way round can hang the transmitter).
- **The 1588 clock's PPS doesn't reach a pin before chip revision 3** (ours is v1.3). The MAC
  makes it: HP_SYSTEM's read-only copy (`sys_gmac_ctrl0.sys_ptp_pps`) toggles as `pps_ctrl` says.
  But GPIO matrix signal 243 (`EMAC_PTP_PPS_PAD_OUT_IDX`) never toggles on any pad, whichever
  output enable is chosen. A scan of all 256 matrix signals against that copy found none; the
  same scan did find I²S's while audio played. ESP-IDF's `esp_eth_mac_set_pps_out_gpio()` (6.0
  on) documents "from silicon revision 3", and its table maps the signal to nothing before
  (2026-10-12). The board's PPS is made by a timer instead (`devplan/network.md` §31b).
- **The MAC's PPS settings, measured:** the flexible mode's enable bit doesn't stick (no flexible
  PPS, as the feature register says). Fixed mode 0 is one 1588 clock period wide, at the second.
  Under digital rollover, setting 1 is "1 Hz" made from bit 29 of the nanoseconds: it rises at
  .537 s and falls on the second.
- **Stepping the 1588 clock:** the update registers take the step's seconds and nanoseconds as
  they are, with the subtract bit for a negative step (ESP-IDF's `emac_hal_ptp_adj_inc`).
  Linux's stmmac writes 10^9 - ns for a subtraction under digital rollover; here that lands about
  a second out. Each of 10 steps of ±250 ns to ±2 s landed within the 1 µs a read can show
  (2026-10-12).
- **HP_SYSTEM has a live copy of the 1588 time bus:** `sys_gmac_ctrl2` is the seconds and
  `sys_gmac_ctrl1` the nanoseconds. Two reads, cheaper than the MAC's own registers
  (`ptpTime()` takes 360-400 ns); back to back with them it agrees to within that.
- **The 1588 clock and the system timer count the same crystal:** as PTP master following the
  NTP-disciplined system clock, ptpclock0's correction settled at -7040 ppb against the system
  clock's -7084 ppb (2026-10-13). So one clock following the other takes its rate as it is and
  corrects only the offset (`ptpservice.cpp`); a PI from scratch on reads a microsecond apart
  swung the follower's rate by 1-2 ppm.

## 4a'. The MIPI-DSI link (`devplan/display.md` §5e)

- **Give the lanes margin over the pixels.** The host sends video in burst mode (IDF's choice and
  ours); with the lane rate exactly the pixels' (720p60 RGB888 on 2 lanes: 960 Mbps) the HDMI
  monitor went black and resynced now and then, and showed banding, with no underrun counted.
  Espressif's FAQ asks about 20% over. Ask for a rate the PHY's PLL makes exactly (on revision
  1.x its reference is PLL_F20M: a multiple of 40 Mbps, or of 20 with another divider) and one that
  makes the line a whole number of lane byte clocks (`laneMbps / 8 / pixelMHz × htotal`).
- **`vid_hline_time` is the whole line**, in lane byte clocks: IDF's LL sums the four parts given
  to it, so rounding each part separately can make the link's line longer or shorter than the
  pixels'. Round the total and give the difference to the front porch.
- **INT_ST0 and INT_ST1 of the DSI host clear when read.** Read them in one place (the scan reads
  them once a frame) and hand the result on; a dump that reads them steals the flags.
- **The DSI host's and bridge's registers fault with their clocks off**, like the DW-GDMA's.
- **Don't disable the DW-GDMA in mid-block to restart the scan.** The bridge's FIFO keeps the old
  frame's pixels (they come out first: an underrun), and with it drained the restarted stream
  still stuck in 7 of 8 tries. Let the DMA halt itself at a frame's end (the first descriptor
  made not valid while the last block is sent), drain the FIFO, turn the DPI output off in the
  vertical blank, and turn it on again only once the FIFO is mostly full (§5f): the DSI link
  stays up and the monitor keeps its picture.
- **The scan can run on black with no PSRAM reads** (`devplan/vnc.md` §5a, measured 2026-11-16):
  each DW-GDMA descriptor's source increment off (`ctrl_lo.sinc` = fixed,
  `dw_gdma_ll_lli_set_src_burst_mode()`), every block reading one 8-byte word of zeros in internal
  RAM. The DMA takes fixed-address bursts from memory: 60 frames a second, 0 stalls or underruns,
  the bridge's output aligned after 62 changes back and forth (the §5f restart). With the scan
  dark, PSRAM-to-PSRAM DMA copies went from 72 to 125 MB/s, the CPU's PSRAM writes and reads from
  39 and 41 to 49 and 47 MB/s, and a 1280x692 fill from PSRAM from 23.9 to 14.8 ms: what 720p's
  166 MB/s of scan reads cost everything else. (Two underruns in 20 changes made while a VNC
  server was busy encoding: resynced; a longer FIFO fill before relighting is to try.)

## 4b. The crypto engines (`devplan/network.md` §26-27)

- **ESP-IDF's crypto DMA can't share the engine** (2026-11-16, `devplan/network.md` §27a): its
  shared channels (`esp_crypto_shared_gdma.c`) ignore `gdma_connect()`'s error and then poll for
  the transfer's end with no timeout, so another GDMA channel connected to the AES engine hangs the
  next mbedtls AES call (TLS) forever, at 100% of a core. Connect ours per call and disconnect
  after; free ESP-IDF's (`esp_crypto_shared_gdma_free()`, not under the crypto lock) when they
  hold it.
- **Register mode beats ESP-IDF's DMA for SSH's sizes** (our own beats both from 1 KB, below).
  The CPU writing each block to the engine and reading it back: AES-128 about 15.7 MB/s at any
  size, SHA-256 and SHA-512 41-44 MB/s even for one 64-byte block, from PSRAM. ESP-IDF's
  mbedtls port (DMA): AES 4.1 MB/s at 64 bytes, 28 MB/s at 16 KB (66 MB/s from internal RAM);
  SHA 2.3 and 41 MB/s. Its DMA path also allocates descriptors and alignment buffers on every call
  (`esp_aes_dma_core.c`), and its AES-GCM managed only 4 MB/s. Measured with a temporary shell
  command; used by `bmds_crypto`.
- **AES by DMA: the cost is the cache operations, not the engine.** Our own AXI-GDMA pair (an
  end-of-frame interrupt wakes the task) reached 25.6 MB/s at 4 KB and 32.8 at 16 KB from PSRAM;
  at 1 KB the fixed cost (about 30 µs: each `esp_cache_msync` 4-5 µs however short, plus
  `gdma_reset`/`gdma_start` and the wake) makes it register mode's equal, so `bmds_crypto` uses DMA
  from 1 KB. `AES_INC_SEL` 1 is the 128-bit counter increment (ESP-IDF sets 0, INC32); checked
  against register mode with counters carrying past 32 and 64 bits.
- **The SHA engine's chaining value is the digest's bytes:** its `H` registers hold the words
  big-endian, as the digest is written (ESP-IDF `memcpy`s the state out as the hash). Writing
  a value there and continuing (`sha_ll_write_digest`, `sha_ll_continue_block`) resumes any
  message from any state, the first block included.
- **`sha_hal_hash_block` doesn't wait for the engine:** wait for `sha_ll_busy()` to clear before
  writing the next block's text (ESP-IDF's block loop relies on reading the digest, which waits).
- **The fixed costs are the lock and the clock, not the engine:** ESP-IDF's
  `esp_crypto_sha_aes_lock` took 4.9 µs to take and give, a BMDS `Mutex` 4.3 µs; switching an
  engine's clock on (which resets it) and off 3.8 µs; reading its enable bit 0.09 µs. A
  64-byte SHA-256 call cost 12.6 µs; leaving the clock on after the first use made it 9 µs
  (`HP_SYS_CLKRST.peri_clk_ctrl25` is checked each call, since ESP-IDF's users switch it off).
  Through the system call it is about 13 µs, so callers give it whole packets.
- **ESP-IDF's AES-GCM does GHASH in software** (`esp_gcm_ghash`'s `gcm_mult`, 4-bit tables and
  64-bit arithmetic; the chip has no GHASH instruction or carry-less multiply) **unless every
  buffer is DMA-reachable:** then the engine does all of it, but only 4.95 MB/s a 16 KB record
  against the partial path's 4.31. Shoup's 8-bit tables in 32-bit words (`bmds/ghash.h`) do GHASH
  at 8.5 MB/s and the record at 6.15 (devplan/network.md §33). mbedTLS's ChaCha20-Poly1305 is
  slower still here: 3.66 MB/s. Measured with `bench net.tls.aead`.
- **The ECC engine's P-256 point multiplication: 4.5 ms** (libtomcrypt's software: 105 ms);
  **the MPI engine's 2048-bit modular exponentiation: 179 ms** (65537: 16 ms). Not used:
  Monocypher's X25519 takes 6.7 ms in software, and clients prefer 25519.
- **No engine helps 25519,** but the implementation matters more than the chip: TweetNaCl's
  X25519 took 75 ms and an Ed25519 signature check 252 ms, Monocypher's 6.7 and 9.7 ms
  (both as built: TweetNaCl `-Os` in Dropbear, Monocypher `-O2`).

## 4c. FreeRTOS on two cores: what a lock costs (`devplan/network.md` §28)

- **A mutex or semaphore operation costs microseconds, more when contended:** a BMDS `Mutex`'s
  lock and unlock took 4.3 µs uncontended (ESP-IDF's crypto lock 4.9 µs). Under a TCP transfer,
  with the network task on one core and the sender on the other, the time inside FreeRTOS's queue
  operations (interrupts masked, spinning on the kernel's lock) was 40% of BusyBox `nc`'s samples.
  Fewer wakes and fewer lock handoffs per byte paid more than faster code.
- **What a critical section costs, and why:** on this silicon (v1.3, `ESP32P4_SELECTS_REV_LESS_V3`)
  masking interrupts isn't a CSR swap. The CLIC threshold is a memory-mapped register: MIE off,
  read it, write it, read it back, MIE on, 63 cycles a mask and restore through IDF's functions.
  IDF's `portENTER_CRITICAL` masks, then `spinlock_acquire()` masks and restores again around a
  compare-and-set that is a call into flash. Leaving repeats it. That totals 283 cycles a
  section. A mutex pair enters four, a semaphore pair two. BMDS's own section (`critical.cpp`, one
  mask, an inline `lr`/`sc`) takes 178 (`devplan/bench.md` §16).
- **The P4 nests interrupts in hardware, so nothing saves the threshold for a task:** with
  `SOC_INT_HW_NESTED_SUPPORTED`, `vectors.S` leaves the threshold alone. Raising it must be done
  with MIE off until the read back. An interrupt taken after the write, before it counts, can
  switch tasks, and the next task then runs masked with no critical section open. That ends in an
  interrupt-watchdog reset ("Reset: interrupt watchdog"), within minutes under load.
- **A contended mutex gives no bound on a high-priority waiter's wait:** a released FreeRTOS
  mutex goes to whoever takes it next, and a task already running on the other core usually
  wins. Under a UDP flood, with the network task and `eth0` each taking the EMAC driver's transmit
  mutex a frame, a priority-20 sender waited up to 2.6 ms though no holder kept it over 127 µs
  (`devplan/network.md` §30a). What a real-time task shares with bulk work must be a short
  spinlock, with the slow work outside it.
- **Keep a real-time task off a busy driver's core:** a priority-20 sender sending every 1 ms under
  a line-rate flood waited up to 250-350 µs to queue a frame on the core the EMAC's interrupt and
  task (21) ran on, under 45 µs on the other. An interrupt is allocated on the core that
  allocates it; the EMAC task is pinned to it, real-time senders to the other
  (`devplan/network.md` §30c).
- **A sequentially consistent atomic load is a fence:** `std::atomic`'s default order compiles to
  `fence rw,rw` around each load on RV32. The signal checks every system call makes read several
  of these. Acquire loads and an early return when nothing is pending took `getpid()` from 4.2 to
  2.5 µs (`devplan/bench.md` §15). Keep the default only where a store must be ordered before a
  later load (one explicit `std::atomic_thread_fence`).
- **A program's core decides display-paced results:** Doom's timedemo got 44 fps on core 1 (where
  the display's conversion runs) and 53-58 on core 0, with the same firmware; the loader's choice
  flipped with small changes in each core's load. A program that draws now says so
  (`BMDS_PROGRAM_DISPLAY`) and always starts on core 0, and `bench` records each program's core
  (`devplan/bench.md` §16-17). An unflagged program's result can still move with its core: look
  at the `core` in the results before blaming a change, and run it more than once.
- **Seeing it:** the profiler samples at the tick, which can't interrupt a critical section, so
  time inside one is charged to `vPortExitCriticalMultiCore` with the caller (`xQueueGenericSend`:
  a give or unlock; `xQueueSemaphoreTake`: a take or lock) as its return address.
- **The interrupted context is at `pxTopOfStack`:** ESP-IDF's RISC-V interrupt entry stores the
  task's stack pointer in its TCB when the interrupt didn't nest, and the frame there starts with
  `mepc`, then `ra` (`riscv/rvruntime-frames.h`). A tick hook reads them (`sysmon.md` §9);
  `xPortInterruptedFromISRContext()` is the nesting depth (above 1: the tick interrupted a handler).
- **On SMP, `xTaskGetTickCountFromISR()` takes the kernel's lock** (it is not a plain read):
  called at every switch it cost what a systimer read does. `xPortInIsrContext()` masks
  interrupts by the threshold (~64 cycles) to read one word; BMDS's is MIE off (~17). Both are
  called far more than their names suggest: every safe critical section asks the latter twice
  (`devplan/bench.md` §18).
- **FreeRTOS's mutex lets a give through when nobody holds it** (`configCHECK_MUTEX_GIVEN_BY_OWNER`
  accepts a holder of NULL): an extra unlock passes silently. BMDS's own mutex ignores and logs it
  (`devplan/bench.md` §21).
- **A board that panics under a test looks like a hang from the host:** the ssh session just
  drops. Check `uptime` and the log's `Reset:` line first; the panic's message is on the USB
  console (pyserial's defaults open it without a reset). Hours went into a "hang" that was a
  reboot (`devplan/bench.md` §21).
- **Every yield is ESP-IDF's cross-core interrupt**, to the yielding core itself or the other,
  and its handler and sender each took a spinlock for the reason word: two critical sections a
  switch. `crosscore.cpp` replaces the module (an atomic reason word) by `--wrap`.
- **The cycle counter stops while its core waits for an interrupt** (WFI, which only the idle task
  does): fine for counting a task's run time between idle stretches, with the timer read again
  when the idle task is switched out (`cputime.cpp`).
- **The EMAC's sent interrupt on every frame** woke the driver's task 72 µs a frame; asking for it
  only now and then (a descriptor's IC bit) and reclaiming in the sender removed most of that.
- **A task's core is fixed at its creation:** ESP-IDF's FreeRTOS has no `vTaskCoreAffinitySet()`
  (only the Amazon SMP kernel does, with `configUSE_CORE_AFFINITY`), so a thread can't be moved
  after it starts; `sched_setaffinity` refuses (`devplan/processes.md` §3.12).
- **Sleeping to a time finer than the tick:** a GPTimer's alarm releasing a semaphore from its
  interrupt woke a priority-12 program thread 29-32 µs after its time at the median, 63-155 µs at
  worst, against the tick's 360 µs and 680-900 µs (`progtest sched`, 1 ms periods). Its
  interrupt isn't cache-safe (`CONFIG_GPTIMER_ISR_CACHE_SAFE` off), so it is held off during a
  flash write and may touch memory in PSRAM (`waketimer.cpp`).
- **A GPTimer's interrupt lands on whatever core registers its callbacks:**
  `gptimer_register_event_callbacks()` allocates it on the calling core, and an unpinned caller
  (the first program thread to ask for `SCHED_FIFO`) may be on either. On core 0, beside the EMAC's
  interrupt, `eth0` (21) and `esp_timer` (22), a 1 ms real-time sender under a flood and TCP both
  ways woke 116-124 µs late at the 99th percentile and up to 0.6-1.2 ms; registered on core 1
  through `esp_ipc_call_blocking()`, 51-66 µs and 253-455 µs, over five boots. The handler in
  IRAM at interrupt level 3 bought only 3-4 µs at the median: not kept. `esp_intr_dump()` into an
  `fmemopen()` stream shows which core has what (`devplan/network.md` §30k).
- **A lock bulk work holds for long is a real-time thread's wait, priority inheritance or not:**
  a `SCHED_FIFO` receiver whose datagrams came lock-free through its flow still took lwIP's core
  lock at every `poll()` and every receive that found nothing, to look at lwIP's own queue. TCP
  holds that lock for whole copies of its send buffer, so the receiver's delay from the driver
  was 1.5 ms at the 99th percentile and 3.4 ms at worst; read without the lock (an atomic count,
  the wake a latched semaphore), 112-136 µs and 0.37-1.1 ms (`devplan/network.md` §30k).
- **A GPTimer can't take its clock undivided:** its prescaler is 2 to 65536, so asking for
  40 MHz from the 40 MHz crystal hits an assert in `timer_ll_set_clock_prescale` (a boot loop,
  2026-10-12). PLL_F80M halved gives the same 25 ns counts, and the PLL is a whole multiple of
  the crystal.
- **`gptimer_get_raw_count()` costs about 1.8 µs:** it latches the count, then waits for the
  timer's clock domain. To pair the count with another clock, write the latch
  (`update.tx_update`) yourself between two reads of that clock, and wait afterwards: the PPS's
  pairing went from a 1.9 µs bracket to 200-240 ns.
- **Routing a peripheral output through the GPIO matrix:** `gpio_set_direction()` puts the pad
  back to the plain GPIO output, so set the direction first and connect the signal after. With
  `CONFIG_HAL_GPIO_USE_ROM_IMPL`, `gpio_matrix_output()` leaves the output enable to the
  peripheral.
- **Where the I/O facts come from** (`devplan/io.md` §3.1, 2026-11-20): `gpio_sig_map.h` names the
  matrix's signals (input 0-255, output 0-255, `SIG_GPIO_OUT_IDX` 256 the plain GPIO output; a few
  names say no direction, `DBG_CH0_CLK_IDX`, and `SIG_IN_FUNC250-255` are both); `io_mux_reg.h`
  names each pad's IO_MUX functions (`FUNC_GPIOn_NAME`, function 1 the GPIO on every pad) and
  also lists pads 55 and 56, which aren't GPIOs (`SOC_GPIO_PIN_COUNT` is 55). Its functions are
  identical in `hw_ver1` (our v1.3) and `hw_ver3`. The analog functions, LP IOs (0-15), strapping
  pins (34-38) and the USB-JTAG note come only from ESP-IDF's GPIO documentation table
  (`docs/en/api-reference/peripherals/gpio/esp32p4.inc`, one per chip). ESP-IDF 6.1 no longer
  states some counts in `soc_caps.h` (I2S, PCNT units): `tools/gen-iochip.py` names where its
  numbers come from and checks the rest against `soc_caps.h`.
- **Pads at reset** (`io` on bmds0, 2026-11-20): MCU_SEL is 0 on every pad, and function 0 is a
  peripheral's on some: GPIO2, 4 and 5 (and 3 until the SD card's detect takes it) are JTAG's
  `MTCK`, `MTMS`, `MTDO` (`MTDI`), and 37/38 UART0's (the ROM's console, pull-ups on). Elsewhere
  function 0 is `GPIOn_0`, which drives nothing. The LP IO mux's registers (`LP_IOMUX.pad[n]`,
  whether a pad belongs to the LP domain) are gated by `HP_SYS_CLKRST.soc_clk_ctrl3
  .reg_iomux_apb_clk_en`, on from reset and never turned off by ESP-IDF; `LP_GPIO.clk_en` is the
  LP GPIO's own clock, not that gate.
- **The ETM makes hardware-timed edges:** a GPTimer's alarm event connected to a GPIO toggle task
  changes the pin at the match, with no interrupt in between.
- **Opening the console's USB serial port with DTR and RTS set low first reset the chip**
  (pyserial, 2026-10-11): the boot log showed `rst:0x17 (CHIP_USB_UART_RESET)`. Opening it with
  pyserial's defaults doesn't: `tools/bmdsboard.py` (`bmds-run`, `bmds-compat`) left the uptime
  running (2026-10-12). The cause (2026-10-28, two more unwanted resets): the kernel raises both
  lines on open, and pyserial then applies `dtr` before `rts`, so `dtr = False; rts = False` passes
  through DTR low with RTS high, the state USB-Serial/JTAG resets the chip on. Leave both alone.
  Capturing a panic still means having the port open before it happens.
- **A reset with DTR still high lands in the ROM loader** (2026-10-30): DTR high is the boot pin
  held low. `esptool --before no-reset --after hard-reset` on a port opened with the lines raised
  pulses RTS alone, so the chip restarts into download mode: a silent console, no shell. A reset
  meant to start the firmware clears both lines first, then pulses RTS with DTR low.
- **The RESET button drops the USB serial device; an RTS reset doesn't** (2026-10-30): RESET (the
  EN pin) restarts the whole chip, USB-Serial/JTAG included, so the host sees a disconnect and a
  new device. If a program still holds the old `ttyACM0`, the board comes back as `ttyACM1`
  (`/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_MAC-if00` follows it). It is also
  a power-on reset: the RTC memory's kept records (`devplan/keptstate.md`) start again. A reset
  through RTS keeps the device and the port.

- **A task's first float decides its core for good** (2026-11-06, `devplan/demo.md` §10): ESP-IDF
  saves the FPU's (and PIE's) registers lazily, so a task is pinned to whichever core it is on the
  first time it uses either (`Thread::pinCurrent()` says so). An unpinned real-time thread that
  starts by doing floating point, such as a synth's `fm rt`, lands on a core by chance and stays.
  BMDS Demo #1's plasma with FM playing ran at 43 fps in one launch and 54 in the next for that
  reason alone (the synth beside the drawing on core 0, or beside the presenter on core 1). The
  mixer (`audio`, priority 20) does the same. Now placed (the user's choice): the synths' real-time
  threads on core 1 (`Thread::pinCurrent()` before their first float), the mixer on core 0. Plasma
  full screen, two launches each:

  | | synths core 0 | synths core 1, mixer core 1 | synths core 1, mixer core 0 (kept) |
  |---|---|---|---|
  | FM | 49-52 fps | 40-41 | 41-45 |
  | wavetable | 50-54 | 48-49 | 57-58 |

  On core 0 a synth takes its time from the drawing; on core 1 the wavetable fits beside the
  presenter and FM doesn't, though it uses no more CPU (24% against 20%): not explained yet (FM's
  voices are in the TCM; the presenter's frames are missed with core 1 at only 62-64%). An earlier
  reading of FM on core 1 at 59-60 fps was wrong: FM had no memory to start then, and was silent.
- **Whole L8 frames full screen, beside a synth: what bounds them** (2026-11-06, `devplan/demo.md`
  §10 step 4, each measured on the board with a temporary probe in the presenter, then removed):
  - **The PPA's CLUT has a floor of ~9 ms a frame at 1280×360:** 51 Mpixel/s, the same into
    internal RAM as into PSRAM (`bench run display.ppa.scale`: `expand_l8_1280x12_internal` 303 µs,
    `_psram` 297 µs), so its own rate, not bandwidth. A conversion's wall time: 10.5 ms with no
    music (60 fps), 13.4-15.5 ms with a synth on core 1 (40-42 fps). Waiting for the swap: ~0.3 ms.
  - **The synth's real-time thread (19) preempts the presenter (`surface`, 4)** between PPA jobs,
    and the PPA idles. Raising the presenter to 20 gave 56-60 fps, but the synth missed 11,267
    periods in two minutes (`audio.dsp0.client_late`; a real-time client must finish a period
    before the mixer's next, 5 ms; a late one is dropped: silence). Not usable.
  - **Fetching the strip's rows by the 2D-DMA** (into the strip's tail, repeated forward in place)
    cut the presenter's CPU from 33% to 23% of core 1 but not the conversion's wall time.
  - **Internal RAM's uncached address (`CACHE_LL_L2MEM_NON_CACHE_ADDR`, +0x40000000) faults on
    PIE:** `esp.vld.128` there is a load access fault (MTVAL 0x8ff86340), a panic. Scalar code only.
  - **The synth on core 0 instead:** the presenter keeps up, the drawing pays (core 0 92-100%):
    demo1's tunnel 41 → 50 fps, fire 40 → 47, balls 43 → 48, solids 37 → 42, stars 55 → 58, but
    plasma 60 → 52. **Splitting the parts' rows across both cores then gained nothing** (core 1
    74-77% busy, the same frame times): the loops are memory-bound (the shared L1, above).
  - **What is left: the synth's own cost**, 35-42% of a core in demo1's busiest section (FM,
    `renderVoice` half of it), several times what its instructions should take. **Not its
    waveforms' reads from flash** (2026-11-07): with all eight copied into internal RAM while FM is
    open (the user's choice: 16 KB, given back when the synth closes after 5 s of quiet), FM still
    took 42% of core 1 in drop B and frame rates moved within a launch's noise (tunnel 41 → 44,
    solids 38 → 40). The cost is somewhere else in the voice loop; profile it per voice next.
  - **What fixed it: the synth renders ahead** (2026-11-07, `devplan/audio.md` §27): a program
    whose music is scheduled allows the port latency (`BMDS_MIDI_ALLOW_LATENCY`; demo1 250 ms), and
    the synth writes a PCM ring at priority 3, below the presenter, instead of meeting each 5 ms
    period at 19. Frame rates beside FM: plasma 59, stars 60, tunnel 59, fire 51, balls 56,
    solids 41 (the last two their own drawing's now). **But at 60 fps the presenter takes 57 % of
    core 1** (tunnel; `Pixels::repeat8`, the CPU's doubling, 61 % of that, memory-bound) and FM
    31 %: below the presenter the synth fell behind for good (24-110 underruns in 14 s, whatever
    the ring). It now raises itself above the presenter (6) while its ring is under a quarter
    full, back to 3 at half: raised 5 times in the whole demo, no underrun. **The ring's time is a
    poor clock:** a PCM stream's position is stamped when the mixer takes frames, a millisecond or
    more of jitter; placing events by it each period put some a period late. A timeline advanced a
    period a period, eased to the measurement, re-anchored only on an underrun: none late.
- **Internal RAM goes to thread stacks first** (2026-11-06): the loader puts a program thread's
  stack (32 KB by default) in internal RAM while 64 KB more stays free. Three default threads (a
  program's main, libbmds3d's helper, the sequencer's player) took ~96 KB; full screen then takes
  ~90 KB more (the strip, the scan's descriptors), and a synth starting after that found no 4 KB
  for its real-time stream or stack ("fm: real-time audio: Out of memory"); started before, it was
  full screen that failed. The stacks used 10, 3 and 3 KB: libbmds3d's helper now asks for 12 KB,
  the player 8 KB (the least), and demo1 20 KB (`stack =` in its recipe). With full screen and FM
  playing, 18 KB of internal RAM stays free, against 9 KB with full screen alone before.
  **A display program's stacks now leave 128 KB more** (`DisplayReserveBytes` in the loader,
  2026-11-07): BMDS_PROGRAM_DISPLAY programs go full screen after they start, and often play music;
  their stacks took internal RAM while it looked free, and FM later found no 2 KB block for its
  waveforms. demo1's stacks now go to PSRAM (its drawing times unchanged); with full screen and FM
  playing, 35 KB of internal RAM stays free (lowest 32 KB), against 18.5 (lowest 5.6) before.
- **A thread with no deadline belongs on the busy drawing core, not the "free" one** (2026-11-07,
  `devplan/games.md` §28): libbmdsgame's ahead mixer (the game's priority, a 170 ms ring) was put
  on the core the game doesn't run on. That core holds the presenter (4) and a synth rendering
  ahead (raised to 6), so the mixer went without for the ring's whole length mid-line: the
  speech crackled. On the game's own core at the same priority, FreeRTOS's time slicing (1 kHz
  tick) gives it the CPU within a tick or two of being ready, and the drawing loses only the
  mixing. It can't float between cores: its first floating point pins it (above). A stream that
  starts after silence plays from its first period, so a writer with no deadline writes a lead of
  silence first.

## 4d. The full-speed USB PHYs (`devplan/audio.md` §23)

- **Two full-speed PHYs, two controllers on them:** USB-Serial/JTAG on PHY 0 (GPIO24/25, the
  board's USB-C) and the full-speed OTG controller (`USB_WRAP`, TinyUSB's root port 0) on PHY 1
  (GPIO26/27), by default. `LP_SYS.usb_ctrl` swaps them: `sw_hw_usb_phy_sel` = 1 (software
  chooses), then `sw_usb_phy_sel` = 1 (OTG on PHY 0). ESP-IDF never swaps them, and its
  `usb_wrap_ll_phy_select()` has no `break` after `case 0`, so asking it for PHY 0 sets PHY 1.
- **Measured on the board (2026-10-20,** a temporary command writing the register):
  - Swapping while running takes USB-Serial/JTAG off the USB-C at once (the PC's `303a:1001` and
    `/dev/ttyACM0` go); swapping back brings it back as `/dev/ttyACM0`.
  - **The swap survives `esp_restart()` and a panic's reset** (the LP domain isn't reset): the
    board came up with it still set and no console on the USB-C. Whoever sets it must clear it
    first thing at boot.
  - **RESET (CHIP_PU) clears it,** and BOOT held through RESET brings the ROM's download mode up on
    the USB-C with the swap set before (esptool connected).
- **D+ outlasts a restart:** the full-speed controller's pad settings survive `esp_restart()`
  and a panic's reset, and USB-Serial/JTAG pulls D+ up too, so swapping back with the device
  attached, or after a reset, showed the computer no disconnect: it kept the old device and
  never enumerated USB-Serial/JTAG. Force D+'s pull-up off (`USB_WRAP.otg_conf`'s pull
  override), wait 20 ms, then swap (`usbdevice.cpp`, 2026-10-20).
- **Leaving download mode needs RESET too:** a reset over USB-Serial/JTAG (esptool's
  `--after hard_reset`, `rst:0x17 CHIP_USB_UART_RESET`) keeps the boot mode latched from BOOT
  (`boot:0x305 DOWNLOAD`), so the ROM waits for a download again. Only a chip reset samples the
  strapping pins again.

## 5. ESP-IDF

- **Wrapping a driver function** is legitimate and cheap: `target_link_libraries(lib INTERFACE
  "-Wl,--wrap=fn")`, then `__wrap_fn` calls `__real_fn` and adjusts (`jpegcodec.cpp`).
- **`--wrap` reaches only calls from other object files:** a call within the file that defines
  the function never goes through the symbol, so it can't be redirected. IDF's `panic_print_char`
  has no global symbol left in the ELF at all. `panic.c` prints the panic's headline and an
  abort's message through its own `panic_print_str`, so wrapping the print functions missed
  exactly those lines; `esp_panic_handler` is wrapped to write them (`log.cpp`,
  `devplan/logging.md` §9). Check with `objdump -d` that the call sites go to `__wrap_`.
- **A panic's report goes only to the console** unless something copies it: on ssh alone, a
  panic looks like a hang. BMDS writes it into the log's history, which survives the reset, and
  the next boot puts it in `system.log` (`devplan/logging.md` §9; `reboot panic` tests it).
  `tools/bmds-panic system.log` turns it into a backtrace with that build's ELF.
- **`esp_log_timestamp()` runs 0.58 s ahead of `esp_timer`** on this board: its base is the
  cycle count at the scheduler's start divided by today's clock rate, though the boot ran at other
  rates, and then it adds the tick count, under the kernel's lock. The systimer starts with the
  application: a line at `[0.574]` was at 4 ms of `esp_timer`. Converting the old log stamps
  with `esp_timer`'s clock made every time in the files 0.58 s late. Log stamps come from
  `Detail::monotonicUs()` now (`log.cpp`).
- **Mapping PSRAM pages yourself** (`devplan/programs.md` §14.2, `imagewindow.cpp`):
  - **Reserving the addresses:** `esp_mmu_map_reserve_block_with_caps()` refuses
    `MMU_MEM_CAP_EXEC` together with `WRITE` or `8BIT` (`ESP_ERR_INVALID_ARG`). Asking for
    `EXEC | 32BIT` gets addresses that are the same for data on the P4, so programs read and
    write their pages there. IDF's PSRAM heap takes `0x48000000`-`0x4A000000`, and the block
    comes after it, at `0x4A000000`.
  - **Don't call `esp_mmu_vaddr_to_paddr()`** from a task whose stack is in PSRAM: it turns the
    caches off to read the MMU's table and asserts in
    `spi_flash_disable_interrupts_caches_and_other_cpu` (`esp_task_stack_is_sane_cache_disabled`).
    It did so on the first `exec` after boot, three times running, and the board started its
    recovery firmware. Read the entry instead, through the same index/content register pair the
    writes use, under a spinlock: `mmu_ll_read_entry()` & `SOC_MMU_PSRAM_VALID_VAL_MASK` is the
    physical page.
  - **`mmu_ll_write_entry()` costs ~4.2 µs an entry** (measured 2026-10-28, `memory.window.mapping`,
    `devplan/psramwindow.md` §2a): it reads the flash encryption's eFuse three times an entry.
    Read it once (`mmu_ll_cache_encryption_enabled()`) and write `SPI_MEM_S_MMU_ITEM_INDEX_REG`
    and `..._CONTENT_REG` directly (page | `SOC_MMU_PSRAM_VALID` | `SOC_MMU_ACCESS_PSRAM`, and
    `SOC_MMU_PSRAM_SENSITIVE` if encrypted): ~0.19 µs an entry.
  - **`esp_cache_msync()` invalidating PSRAM costs ~7 µs whatever the size** (64 KB or 8 MB):
    the cache is swept, not the range. Invalidate a range in one call, not a page at a time
    (~6.5 µs a call). Checked: 8 MB in one call left 0 stale words.
  - **The AXI GDMA reads and writes window addresses** (pages mapped outside the heap's range)
    at full speed, correctly (`esp_async_memcpy()`: 71.9 MB/s, as through the heap's). So do the
    DW-GDMA (memory to memory, and the display's scan of surface 0 from the window: 0 underruns
    under `memory.traffic`), the 2D-DMA, the PPA and the JPEG codec, both ways, bit for bit
    (`memory.window.dma`, 2026-10-28). Every DMA reaches PSRAM through the same MMU.
  - **IDF's address checks don't know window addresses:** `esp_ptr_external_ram()` and
    `esp_ptr_dma_ext_capable()` are false for them. The SD host's alignment check then asks
    internal RAM's alignment (BMDS asks the L2's line itself); IDF's PPA driver's own operations
    (`ppa_do_*`) refuse them (BMDS drives the PPA itself). `Memory::isExternal()` knows both.
  - **DW-GDMA's event callbacks must be in IRAM and their argument in internal RAM**
    (`dw_gdma_channel_register_event_callbacks()` refuses others with `ESP_ERR_INVALID_ARG`), and a
    test whose copy never finished hung in `dw_gdma_channel_abort()` (it waits for the channel to
    stop): disable such a channel instead.
  - **Writing entries with the caches on is safe** for addresses nobody uses yet. IDF's own
    mappings stall the other core by an interrupt, which can't land inside a critical section,
    and the flash's partitions use the other MMU (`SPI_MEM_C`, not `SPI_MEM_S`).
- **PSRAM keeps its contents across a reset** (only a power cut loses them):
  - `EXT_RAM_NOINIT_ATTR` (`CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY`) places a variable
    at the start of PSRAM, `0x48000000`. Without that setting, the attribute falls back to
    internal `.noinit`.
  - **But:** the timing tuning for 200 MHz writes a 128-byte test pattern at physical
    `0x80..0xff` at every boot (`MSPI_TIMING_PSRAM_TEST_DATA_ADDR`). Keep 256 bytes unused at
    the front.
  - **And:** a reset drops what the caches hold dirty, and IDF's `esp_restart_noos()` writes back
    only the L1. Write back the L1 and then the L2 yourself (`Cache_WriteBack_Addr`) before a
    reset (`log.cpp` wraps `esp_restart_noos`).
- **Read the driver before trusting it with a DMA:** what it syncs, when, and what it reports at
  a buffer's end (the JPEG encoder above, the USB-Serial/JTAG FIFO in `docs/HARDWARE.md`).
- **A single-sector card read is the card's time, not the host's:** ~445 µs a sector, of which
  the CMD17 is ~430 µs and the CMD13 ESP-IDF sends after it ~24 µs; the bus moves 512 bytes in
  ~26 µs at 40 MHz. Dropping the CMD13 gained under 1%. Only fewer reads help (`devplan/storage.md` §3).
- **ESP-IDF's SD writes spin the CPU while the card programs** (v6.1, `devplan/storage.md` §17):
  `sdmmc_wait_for_idle()` sends CMD13 back to back for the 0.4-1.6 ms a 16 KB write keeps the card
  busy, yielding only after 100 ms, and every command re-applies the bus width (with D3's pin: the
  IOMUX, pull, input and drive strength, each in a critical section), the sampling mode, the delay
  phase (asking the clock tree) and line, and reads the clock dividers back. A 16 KB `dd` to the
  card kept `dd` at 25% of a core. Found with `prof` during a download.
  - **A GPIO interrupt works on a pad the SDMMC owns:** the pad's GPIO input still reads D0, so
    `gpio_set_intr_type()`, `gpio_isr_handler_add()` and `gpio_intr_enable()` (never
    `gpio_config()`, which takes the pad back) give an edge the moment the card releases the line.
    Waiting on it made writes 10% faster than the spin, at 10% of a core.
  - **The controller's busy-clear interrupt isn't usable alongside ESP-IDF:** it shares bit 13
    with the start-bit error, which the transfer state machine treats as an error.
  - **Sub-tick sleeps between looks lost:** the wake timer's sleeps came back about 75 µs late on
    average, so polling at 50 µs was 2.5% slower than the spin.
  - **UHS-I on this board** (`devplan/storage.md` §18): a card left at 1.8 V by a reset can't be
    reset by GPIO45's power switch in any safe time. The pull-ups on LDO 4 keep it alive through
    its pins for well over 100 ms (and turning LDO 4 off doesn't help: GPIO45's pad seems to be in
    its domain, so the switch can't be held off). After about 1 s off it does reset, but its inrush
    when switched back on **browns the board out**, in a loop until reflashed. Such a card doesn't
    offer S18A in its OCR but takes SDR50 (offered only at 1.8 V): match it by setting LDO 4 to
    1.8 V and tuning. And ESP-IDF answers a UHS mode the card refuses with the default speed
    (20 MHz) and `ESP_OK`, not an error: check `card->max_freq_khz`.
  - **Unaligned pieces cost a quarter more:** 16,000-byte writes of a file take 26% longer than
    16 KB ones (a single-sector write for the partial sectors each time); gather them
    (`BlockWriter`).
- **`rmt_transmit()` starts the hardware at its end, about 73 µs after it is called:** it first
  fills the channel's memory through the encoder. A stream's start stamped before the call was
  80 µs early against what a receiver measured back; stamped when it returns, 7 µs (the
  receiver's interrupt and a tick). `rmtports.cpp`, `devplan/io.md` §13.2.
- **A flash write starves an RMT stream fed from the channel's memory:** 24 symbols a half (some
  10 ms of LTC) don't cover an erase's pause, during which the RMT's interrupts (in flash) can't
  refill it. Held writes avoid most pauses; for the rest, the sending DMA channel's buffer (up to
  2046 symbols in internal RAM) carried a stream through two forced 256 KB writes. ESP-IDF's DMA
  encoder fills a whole half at a time, with filler if the source has less, so a DMA stream must
  start with a buffer's worth waiting (`devplan/io.md` §13.5).
- **The RMT's ticks and the monotonic clock keep step:** both come from the 40 MHz crystal (the
  RMT through PLL_F80M, the systimer directly); a stream's frames timed afresh after ten minutes
  sat where its tick count said within a µs. A `gptimer` at 1 MHz keeps step with both too.
- **A `gptimer` alarm at interrupt level 1 lands 2 to 19 µs late; at level 3, 2 to 8** (31 µs
  once, in the first half minute after a boot). Timed outputs (the RMT's mark pulse) ask for 3, the
  highest a C handler may take (`devplan/io.md` §13.4).
- **ESP-IDF's partial RMT receive hands a block over one block late:** with the buffer half the
  channel's memory, the callback for a block comes at the next threshold, just before the
  driver copies the block after it; the time of that callback is when the *next* block ended,
  so a block's first edge is the previous callback's time less its ticks, and a run's first
  block waits for the second (or the run's end, less the quiet time) to be timed.
- **ESP-IDF's SPI master with DMA allocates per transaction unless buffers are whole cache
  lines:** a buffer must be DMA-capable and aligned to the cache line (64 bytes here) in address
  *and length*, or `setup_priv_desc()` takes a `heap_caps_aligned_alloc()` bounce buffer for that
  transaction; `SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL` only turns that into `ESP_ERR_INVALID_ARG`. A
  4-byte register read can't meet it, so SPI ports run with DMA off (the controller's 64-byte
  buffer, nothing allocated) until GDMA on `spi_hal` of our own (`devplan/io.md` §11.5).
- **SPI by DMA without ESP-IDF's per-transaction allocations:** keep ESP-IDF's bus
  (`spi_bus_initialize()` with DMA) and run the transactions on `spi_hal` with buffers of your own
  (`spicommon_dma_desc_setup_link()`, `gdma_reset()`/`gdma_start()`, `spi_hal_hw_prepare_tx/rx()`,
  the transaction-done interrupt from `spicommon_irqsource_for_host()`), as ESP-IDF's SPI slave
  does. Sync your buffers' whole lines; switch a direction's DMA off (`spi_ll_dma_*_enable()`) when
  a piece doesn't use it, or one goes through the FIFO. `spi_hal_init()` sets the done status:
  clear it before enabling the interrupt (`spiports.cpp`, `devplan/io.md` §14).
- **Interrupts wait up to 600 µs at times:** a UART's FIFO fill on entry to its interrupt (level
  1) showed waits of 410 µs on a quiet system and 590 µs during a flash write, most likely cache
  write-backs (§3: interrupts masked throughout). A CPU-drained 128-byte FIFO can't cover that
  above about 1 Mbaud (256 µs at 5 Mbaud dropped a byte a megabyte); UART input above 1 Mbaud
  goes by UHCI's DMA (`uartconsole.cpp`, `devplan/io.md` §14.3). Measure with the FIFO's fill
  on entry: bytes past the threshold times a byte's time.
- **A flash write loses a UART's input unless its interrupt is cache-safe:** the FIFO (128 bytes:
  11 ms at 115200) can't wait out a cache-off window, and 2 to 5% was lost at every baud. Those
  windows last at most some 20 ms (`CONFIG_SPI_FLASH_ERASE_YIELD_DURATION_MS`): an IRAM interrupt
  draining into internal RAM covers them, but the reader can't run for longer (about 85 ms at
  1 Mbaud), so the receive ring needs that much too (64 KB, PSRAM).
- **UHCI receive as an endless ring:** ESP-IDF's GDMA, receiving, goes on after a descriptor
  closed by UHCI's idle EOF into the next, and gives each back with its owner bit cleared and its
  length written; a circular list of descriptors with owner checking on never needs re-arming.
  Its descriptors are in internal RAM, behind the L1 cache: read and write them through the
  uncached alias (`+ SOC_NON_CACHEABLE_OFFSET`, as `gdma_link` does), `next` and `gdma_start()`
  the cached address. Separator characters must be off (`uhci_ll_set_seper_chr()`), and the idle
  EOF follows the UART's `rx_idle_thrhd` in bit times (`uart_ll_set_rx_idle_thr()`). ESP-IDF's own
  UHCI driver ends a transfer at each idle EOF (re-armed from a task) and counts one callback as
  one node (`uartconsole.cpp`).
- **ESP-IDF's SPI master leaves pins as nothing:** removing a device turns its CS output off
  (`spicommon_cs_free_io()`: the line floats, and an active-low device reads that as selected),
  and `spi_bus_free()` turns the bus's outputs off and leaves the input selectors it set. A
  device's mode and clock are fixed when it is added, so changing them means a CS glitch. SPI
  ports drive their chip selects as GPIO outputs and save and put back pads and selectors
  (`spiports.cpp`). With the matrix, a master's MOSI isn't routed back in, so 3-wire reads and
  an internal loopback need the D or Q input routed from the MOSI pad by hand.
- **`spi_device_transmit()` costs about 37 µs a transaction** beyond the bits (its queue, the
  interrupt, the waiting task woken), measured in 64-byte pieces at 1-80 MHz.
- **ESP-IDF's I²C master writes the device's address itself:** after every start of a
  transaction it runs (`s_i2c_start_end_command`), defined operations
  (`i2c_master_execute_defined_operations`) included, it puts the device's address byte (read
  or write by the next command) ahead of the operations' own. A raw transfer that writes its own
  address bytes needs a device added with `I2C_DEVICE_ADDRESS_NOT_USED` (0xffff), or the wire
  carries two address bytes and nothing acknowledges. Found when `/dev/i2c-N`'s forced read of
  the bridge got no answer; used by `I2cBus::transfer()` (`devplan/io.md` §11.4).
- **`esp_random()` costs ~56 µs a word, and more:** it waits 14 × the CPU's MHz APB cycles
  since the last word (the P4's tested sampling rate), and asks the clock tree for the CPU and APB
  frequencies on every call, which reads the PLL's settings over the analog I2C bus in critical
  sections (`regi2c_ctrl_read_reg_mask` in profiles). 32 bytes took ~0.9 ms. For a word now and
  then, read `WDEV_RND_REG` directly (`Detail::randomWord()`); the pool takes 32 paced bytes at
  most once a second (`devplan/random.md` §3).
- **`esp_timer_get_time()` divides in software:** it reads the systimer, then converts its 16 MHz
  ticks to µs with the ROM's `systimer_ticks_to_us()`, a 64-bit division with no hardware help.
  BMDS wraps it (`__wrap_esp_timer_get_time` in `platform.cpp`) with a shift by 4: 794 → 672 ns.
  The rest is the systimer itself. The snapshot and the wait for a valid value take ~650 ns, so a
  clock read is never cheap here (`devplan/bench.md` §15).
- **Revision macros:** some of ESP-IDF's workarounds depend on `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`
  and on chip-revision macros; check which branch a workaround is in before relying on it.
- **Anything that writes the flash or turns the caches off needs an internal stack:** called from
  a task whose stack is in PSRAM (programs', the shell's), `esp_ota_set_boot_partition()` asserted
  in `spi_flash_disable_interrupts_caches_and_other_cpu` (`esp_task_stack_is_sane_cache_disabled`).
  So do `esp_partition_erase_range/write/read`. `Thread::runOnInternalStack()` runs such work on a
  kernel thread with an internal stack and waits (`devplan/update.md` §11).
- **Flash auto-suspend faults reads on this board** (P4 v1.3, W25Q128JV, IDF v6.1-dev): with
  `CONFIG_SPI_FLASH_AUTO_SUSPEND`, a cache read of flash (code, rodata, mapped `/rom`) while an
  erase or a program was suspended raised an instruction or load access fault on the reading
  core, on the first ~1 MB write in each of four runs under load (a loop reading `/rom`), with
  sector erases and with IDF's suspend-status check and a 100 µs tSUS too; idle, 20 writes passed.
  Off (IDF's default): none in 12 under the same load. Off since 2026-10-14
  (`devplan/update.md` §16): flash writes pause everything not in internal RAM, audio included.
- **The second-stage bootloader can't fall back to `ota_1` on this IDF (v6.1-dev, ff97953b):**
  `bootloader_flash_read_allow_decrypt()` invalidates the cache at `FLASH_MMAP_VADDR` after moving
  its read window, which is at `FLASH_READ_VADDR`. With `ota_0`'s first page zeroed, `ota_1`'s
  header was read from the cache as zeros ("invalid magic byte"), though the flash held `e9`.
  Upstream master invalidates `FLASH_READ_VADDR` (2026-10-13). Normal boots read one image and
  don't see it (`devplan/update.md` §11).
- **The main task's stack is 3584 bytes unless set**, and BMDS boots on it (every stage, then
  `app_main()` returns and IDF frees it). A blank card's first boot overflowed it, at every boot,
  in both firmwares: Cfg's copy of `/flash/cfg` creates files, and FatFs's new directory entry
  reads through the SD driver into IDF's transaction code, 3952 bytes deep at most. A normal boot
  used 3360. Found 2026-11-02 testing the first run on a formatted card (a stack protection fault
  in task "main"); 6144 since (`sdkconfig.defaults`), which costs internal RAM only while booting.
  The boot report logs the high-water mark ("Main task stack high water"): check it after adding
  boot work.
- **mbedTLS ignores certificates' dates unless `CONFIG_MBEDTLS_HAVE_TIME_DATE=y`.** IDF's default
  is off ("valid from" and "valid to" are skipped), so an expired certificate passes a handshake
  with `MBEDTLS_SSL_VERIFY_REQUIRED`, and a client's own "has expired" message can never be seen.
  Found 2026-11-16 reading the configuration for programs' TLS (`devplan/network.md` §35); on
  since. It checks against libc's `time()`, which the kernel's `Clock` sets
  (`Detail::setLibcTime()`): before the clock is set, every certificate would be "not yet valid",
  so `TlsSession` refuses to verify until then.
- **IDF's certificate bundle installs its own verify callback** (`esp_crt_bundle_attach()` calls
  `mbedtls_ssl_conf_verify()`); setting another replaces it, and the bundle then trusts nothing.
  `esp_crt_verify_callback()` is extern though not in the header: a callback of one's own calls it
  first (`tlssession.cpp`). Its refusal is a fatal return; ignored, the `NOT_TRUSTED` flag it
  leaves fails the handshake as `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED` with the flags readable.
- **IDF's bundle refuses a root a server sends cross-signed by an older one** (found
  2026-11-16, `devplan/network.md` §35l): smtp.gmail.com sends GTS Root R4 signed by "GlobalSign
  Root CA", which the bundle no longer has, though it has GTS Root R4. The bundle's check looks
  only at the top certificate's issuer. IDF's `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY`
  answers it with a CA callback, but then mbedTLS ignores roots given any other way (a program's,
  `net/tls/ca`), and its made-up candidate leaks its `subject_raw` (`calloc`'d, never freed by
  `mbedtls_x509_crt_free()`) at each lookup. `tlssession.cpp` instead trusts a certificate whose
  subject and public key are one of the bundle's roots, read from the bundle's own format
  (`_binary_x509_crt_bundle_start`; esp_crt_bundle.c's comment gives it).
- **TLS 1.3 in mbedTLS costs 44.5 KB of flash** (ephemeral key exchange only, no PSK; measured
  2026-11-16, two fresh builds from `sdkconfig.defaults`) and selects
  `MBEDTLS_SSL_KEEP_PEER_CERTIFICATE`: every session keeps the server's chain while it lasts.
- **A restart can fault in the ROM's cache write-back** (seen once, 2026-11-02, cause unknown):
  `reboot` from the shell ended in a store access fault in `Cache_WriteBack_All_Gid` on core 1
  (MEPC `0x4fc10a60`, MTVAL `0x3ff100a0`: the cache's own registers), which `esp_restart_noos`
  calls before turning the L2 off; logged as a panic, it counted toward the three crashes in a
  row that start recovery. About eight restarts around it were clean. Decode such addresses
  with IDF's ROM ELF (`~/.espressif/tools/esp-rom-elfs/*/esp32p4_rev0_rom.elf`): the firmware's
  own ELF knows nothing at `0x4fc...`, and the stack dump below it held an earlier boot's frames
  (PSRAM keeps its contents over a software reset).

## 5a. Traps, the PMP and the stack guard (`devplan/programs.md` §21)

Learned 2026-11-13 with a spike (wrappers that logged and fell through) and `progtest faults`.

- **IDF locks 14 of the 16 PMP entries at boot** (`esp_cpu_configure_region_protection()`,
  `cpu_region_protect.c`); measured 2026-11-14 with a program reading the CSRs, our config
  (MEMPROT on, PSRAM XIP off): 0-8 and 11-15 locked, **9 and 10 unlocked and unused** (only the
  XIP branch sets them). The locked ones give U-mode the same rights as M-mode, all internal RAM
  (5: RW to `SOC_DRAM_HIGH`) and the peripherals (15: RW) included, so IDF's layout can't
  isolate a program; **no entry covers PSRAM**, so U-mode is denied it unless an unlocked entry
  grants it. All 16 PMA entries are locked. Everything runs in M-mode.
  - **`CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE` decides whether that function runs at all**,
    in the bootloader and in the app (`bootloader_init_mem()`, both cores). With it on, the
    bootloader already locks 0, 1, 2 and 15. With it off, IDF sets neither PMP nor PMA.
  - **The core has U-mode:** `misa` 0x40901125 (U and X set), `mstatus.MPP` holds 0, `MPRV`
    is writable, `mcounteren` is 0 (U-mode can't read the counters until it is set).
  - **The HP peripheral PMS (APM) is at its reset value**, every bit allowed in both modes for
    both cores; IDF doesn't touch it on the P4. Its PSRAM, flash and L2MEM bits gate only
    accesses that bypass the cache, so it can't keep a program out of RAM, only out of
    peripherals.
  - **IDF assumes every task runs in M-mode:** `rtos_int_exit` (`portasm.S`) returns the
    interrupted task's `mstatus` even after switching tasks, so the next task inherits its MPP.
- **U-mode, measured** (2026-11-14, a spike program: interrupts masked by the CLIC threshold, its
  own `mtvec`/`mtvt`, PMP entry 9 granting PSRAM, `mret` into U and `ecall` back; core 1):
  - `ecall` from U is cause 8; a load nothing in the PMP covers (TCM) is a load access fault
    with the address in `mtval`; privileged CSRs (`mstatus`, `pmpcfg2`), `mpccr` (0x7e2) and
    `wfi` are illegal instructions, and **`mtval` holds the instruction bits** for them.
  - **The FPU, PIE and the hardware loops all work in U-mode** once enabled (IDF enables them
    lazily in M-mode; the `mstatus.FS` and PIE/HWLP states carry over).
  - `cycle` traps in U while `mcounteren` is 0 and reads the count with it set.
  - **MPRV works:** M-mode with `MPRV` set and MPP U takes U-mode's PMP checks (a TCM load
    faulted, the program's own memory didn't), so the kernel can copy from user buffers with
    the user's rights.
  - **The APM gates U-mode peripheral access:** clearing a core's `UM` bit for a peripheral
    (`PMS_COREn_UM_HP_PERI_PMS_REG1`, bit 25 for the PMS itself) made U-mode loads from it a
    load access fault (cause 5, address in `mtval`), M-mode unaffected.
  - **The hardware stack guard watches SP in U-mode too:** running U code on a buffer outside
    the task's stack bounds latched the guard's interrupt, which fired when unmasked.
  - **The CLIC threshold is memory-mapped on this chip revision** (`CLIC_INT_THRESH_REG`,
    0x20800008, bits 31:24, per core); `mintthresh` (0x347) is an illegal instruction before
    rev 2. It sits in PMP entry 0's region, which IDF makes RW for U-mode too.
  - **A U round trip** (`mret` in, `ecall` back, 14 registers saved and restored) took 166 cycles.
- **BMDS sets the PMA and PMP now** (`devplan/protection.md` §12, 2026-11-14): IDF's region
  setup off (`CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n`), `bmds_board`'s `regionprotect.cpp`
  instead. What it took:
  - **The bootloader configures core 0 alone.** An older bootloader's locks are on core 0's CSRs
    only, and core 1 starts with the ROM's state: decide on core 0 (the core stage, core 1 paused)
    for both. IDF used to set core 1's from `call_start_cpu1`; now core 1 runs on the ROM's state
    until the secondary stage's first hooks, which was fine.
  - **The PMA's granularity is 128 bytes** (TOR addresses read back with the low 5 bits clear),
    and `_iram_text_end` is 128-aligned while MEMPROT is on. **Never round a code/data boundary:**
    rounding the IRAM split up to 4 KB made the start of `.data` read-only (the first store, a
    spinlock in the stack guard's init, faulted). Write the exact address and read it back.
  - **The ROM's PMA state on core 1 has no free entry** (a probe for one found none), so nothing
    can be measured on a spare entry there.
- **The CLIC's vector table load is an instruction fetch** (2026-11-14, `devplan/protection.md`
  §13): with `mtvt` pointing at a table in internal data RAM (read/write, not executable under
  BMDS's PMA), the first hardware-vectored interrupt was an instruction access fault with `mepc` =
  `mtval` = the table's entry. Keep the table in executable memory (IDF keeps its own in IRAM's
  code, `.exception_vectors_table.text`).
- **The hardware stack guard rules out swapping SP with `mscratch` at trap entry:** SP outside the
  running task's bounds for one instruction latches the guard (it watches SP every cycle, in any
  mode), and the interrupt fires once unmasked: "Stack protection fault" in `ipc0`. Swap a
  general register instead; move SP only with the guard's monitor stopped
  (`ESP_HW_STACK_GUARD_MONITOR_STOP_CUR_CORE`, as `portasm.S` does at a switch).
- **Every trap must leave through its own `mret`, with its own `mcause`** (rev 1.3, 2026-11-14,
  `devplan/protection.md` §14; it cost most of a day). On this CLIC an `mret` whose
  `mcause.interrupt` doesn't match the entry type of the trap it leaves corrupts the CLIC's
  state, and nothing reports it:
  - an extra `mret` with a stale interrupt cause in `mcause` (left by an interrupt handled
    earlier) left the core taking **no more interrupts**: the program ran on in U-mode, no tick,
    then the watchdog;
  - the same `mret` with `mcause` written 0, or an exception trap entered but never left by an
    `mret` (a system call that went back to U-mode through a second trap), left the core never
    again **delivering illegal-instruction exceptions**. IDF enables the FPU and PIE lazily
    through exactly that exception, so the next task's first FP or PIE instruction (`flw`, `fsw`
    in `vfork`, PIE in `TextBlend`) stalled the core silently, seconds later and in code that had
    nothing to do with it; ecall and interrupts still worked.
  IDF never meets it (every trap exits by its own `mret`, `mcause` restored from the frame).
  Zephyr met the second form (zephyrproject-rtos/zephyr#114028, fixed by PR #114272: "an `mret`
  whose mcause.interrupt does not match the current trap's entry type corrupts the CLIC
  state"). BMDS's U-mode return ends with an `ecall` from M-mode so that its `mret` leaves a
  trap of its own, and a system call leaves its `ecall` trap by `mret` before the slot runs.
- **How that was found**, for the next silent hang: "Reset: watchdog" (not "interrupt watchdog")
  means the interrupt watchdog's panic never ran, so a core stopped taking interrupts. A ring of
  events per core in LP RAM (`RTC_NOINIT_ATTR`, written from the trap path, printed at the next
  boot) showed the last traps. A task on each core watching the other core's tick count, reading
  `ASSIST_DEBUG_CORE_n_RCD_PDEBUGPC/SP` when it stops, gave the stuck instruction. That register
  is **the PC of the last exception** (IDF enables its recording), not a live PC. The ROM's
  reset banner (`Core0 Saved PC`, `Core1 Saved PC`) on the serial console agrees with it.
- **An interrupt taken in U-mode fetches its vector with M-mode's rights:** U-mode needs no PMP
  grant for the `mtvt` table (measured: a U-mode loop took every tick for 3 s without one).
- **`mcounteren` works:** with CY and IR set, U-mode reads `cycle` and `instret`; `wfi` and
  every other CSR are illegal instructions there (SIGILL).
- **A 4 KB-aligned input section in flash text breaks the link:** it raises `.flash.text`'s
  alignment, which moves it off the rodata range IDF reserves for it (`ld: section .flash.text
  ... overlaps section .flash_rodata_dummy`). Align with an ldgen fragment instead
  (`ALIGN(4096, pre, post)` in `bmds_loader/linker.lf`), and give the section a name the default
  `.text.*` rule doesn't take first.
- **A scalar load right after a PIE store can read stale data:** `esp.vst.128.ip` then `lw` of
  the same bytes read the old value, in M-mode and U-mode alike; a `fence` between them fixed
  it (the U-mode spike above).
- **A trap handler that returns resumes the program.** `vectors.S` sends unhandled exceptions to
  `panic_from_exception` (`.equ` for `xt_unhandled_exception`) and the panic interrupts (the stack
  guard, the interrupt watchdog, cache errors) to `panic_from_isr` (`panicHandler`), both with the
  saved frame (`RvExcFrame`, 0x94 bytes of fields, 160 pushed). If either returns, the registers,
  `mepc` included, are restored from the frame and `mret` runs. `-Wl,--wrap` reaches both (the
  `.equ` relocates against the real symbol; checked in the disassembly), before IDF stalls the
  other core. Shown by resuming past an `ebreak`.
  - **SP is not restored from the frame** (`vectors.S` pops it by its size: "MTVEC and SP are
    assumed to be unmodified"), **nor is `mstatus`**: to resume on another stack, point `mepc` at
    a stub, pass the new SP in a register (`a0`), and clear `mstatus.MPIE` in the handler, so
    that `mret` leaves interrupts off until the stub has moved SP (`faults.cpp`).
  - **The top of a task's stack isn't free:** the port puts the coprocessor (FPU, PIE) save area
    and the TLS block there (`pxPortInitialiseStack()`). A fresh SP for a task is the one its
    entry function began with, not `pxEndOfStack`.
  - **Nor is its bottom** (2026-11-15, `devplan/protection.md` §21): the FPU's and PIE's saved
    registers go in areas the port carves from the stack's lowest addresses, lazily (when another
    task takes the unit on that core), moving the TCB's stack start above them
    (`xTaskGetStackStart()`; `vPortCleanUpCoprocArea()` puts it back). The stack probe painted
    down to the allocation's bottom and so over them: the task's next FPU restore loaded `fcsr`
    `0xa5a5a5a5`, a rounding mode (5) that doesn't exist, and from then every FP instruction using
    the dynamic mode raised illegal-instruction; the port's lazy-enable trap saw the task already
    owning the FPU and returned, and the instruction trapped again, forever. It looks like a hang
    at one PC (the profiler's samples all on one `fdiv`) with interrupts and other tasks fine:
    unlike §5a's CLIC hang, the core still takes the exception, again and again. Anything that
    writes a task's stack below its SP must stay above its start plus the areas' room (under 512
    bytes; one may be carved while the writer is preempted).
- **What the faults look like** (frame values, at the wrapper):

  | Fault | `mcause` | `mtval` |
  |---|---|---|
  | load from 0, from 0x46000000 (a PMA hole) | 5, load access fault | the address |
  | store to 0 | 7, store access fault | the address |
  | call through a null pointer | 1, instruction access fault | 0; `mepc` 0, the caller in `ra` |
  | `unimp` | 2, illegal instruction | **0**: the P4 doesn't record the instruction |
  | `ebreak` (`c.ebreak`) | 3 | 0 |
  | stack guard | 27 (`ETS_ASSIST_DEBUG_INUM`, after `vectors.S` takes 16 off the CLIC's 43) | 0 |

  Address 0 faults: the first PMA entry blocks 0x0-0x20000000. **Misaligned loads don't trap**:
  the hardware does them (an `lw` at offset 1 read the right bytes).
- **The stack guard's interrupt goes to both cores.** It is one interrupt source
  (`ETS_ASSIST_DEBUG_INTR_SOURCE`), routed to the same interrupt number on each core by
  `esp_hw_stack_guard_init()`, level-triggered. Both cores enter `panicHandler` together; IDF
  parks the one that didn't fire (`esp_hw_stack_guard_get_fired_cpu()`, `busy_wait()`). Printing
  from both at once interleaves the output character by character. A handler that recovers must
  clear the latched status (`assist_debug_hal_sp_int_clear()`, public in `hal/assist_debug_hal.h`)
  and let the other core return.
- **The guard is asynchronous, and it pushes the frame below the stack.** The bound is the task's
  `pxStack` exactly. A recursion of 272-byte frames ran one more call (its stores below the bound
  included) before the interrupt was taken: SP was 544 bytes below the bound, then the trap pushed
  its 160 bytes below that (704 bytes in all, into whatever lies under the stack); other runs
  stopped 288-304 bytes past. A frame larger than whatever slack lies below goes past it in one
  instruction. BMDS keeps 1 KB below program stacks for this (`devplan/programs.md` §21). GCC 15's
  `-fstack-clash-protection` works on RISC-V but probes in 4 KB steps.
- **Cost:** ~65 cycles an interrupt (`devplan/bench.md` §22).

**A trap storm looks like a silent watchdog reset.** IDF's `_panic_handler` starts by pushing its
frame at SP. Entered with an SP no store can use (outside RAM, in IRAM's code, a PMA hole), every
store faults, the fault re-enters `_panic_handler`, and so on, forever: no output, and once the other
core is stalled too, only the hardware system watchdog ends it (`rst:0x7 (HP_SYS_HP_WDT_RESET)`, the
ROM's `Core0 Saved PC` and `Core1 Saved PC` both `_panic_handler`'s first instruction). Seen three
times in two days of protection work (`devplan/protection.md` §17), each around installs and U-mode
benchmarks; not reproduced in 17 cycles of the same with the console captured. BMDS's exception
entry now checks SP for an M-mode trap against where stacks can be (TCM, PSRAM, internal RAM past
`_iram_end`, LP RAM: the RTC heap holds task stacks too, so leaving it out crashed every boot on
the emergency stack): a bad one goes to a 2 KB emergency stack a core, its mepc, mcause, mtval, SP
and ra kept in RTC memory and reported at the next boot, and IDF's panic then prints (a third storm
on a core resets). `progtest faults badsp` in M-mode tries it.

**Interrupt masking is expensive here** (rev 1.3: the CLIC's threshold is memory-mapped, §5a):
FreeRTOS's `xTaskGetCurrentTaskHandle()` masks interrupts around one array read (~150 cycles), and a
critical section costs ~1,000 cycles end to end. Clearing `mstatus.MIE` for a few instructions does
the same job for two. Fenced atomics (acquire loads, seq_cst) cost ~25 cycles each. Both mattered on
every system call's path (`devplan/protection.md` §18).

**Capturing the console across resets:** the USB serial port stays enumerated across a software
reset but not every reset; a capture must reopen the port when it goes, or it silently stops at the
reset it was meant to catch (lost three captures before this was noticed).

**`CONFIG_APP_REPRODUCIBLE_BUILD` maps less than it seems** (IDF v6.1-dev, 2026-11-15,
`devplan/debuginfo.md` §13): `-fdebug-prefix-map` for IDF, the project's folder, the build and
each component's folder, by their text. A component folder given with `..` in it
(`EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components`) matches nothing: normalize it
(`get_filename_component(... ABSOLUTE)`). Sources outside every component folder
(`thirdparty/...`, named by a component's CMakeLists) are covered only if they are under the
project's folder, and `COMPILER_HIDE_PATHS_MACROS` hides `__FILE__` only under the project and IDF:
the recovery project (`recovery/`) left both with this machine's paths until it added its own
`-ffile-prefix-map` for the repository, set before `project()` so IDF's maps, appended during it,
still win. Checked by building in two worktrees at different paths and comparing the bytes.

## 6. The ROM

- **Has:** miniz's `tdefl` (PNG writing) and `tinfl` (inflate: PNG decoding, through
  `bmds_inflate_fast`, a weak hook so `bmds_core` stays free of ESP-IDF), CRC-32
  (`esp_rom_crc32_le`). **Hasn't:** TJpgDec (other ESP32 chips' ROMs do; vendored in
  `thirdparty/tjpgd`).
- **Using a ROM function from a pure component:** declare a weak `extern "C"` hook in the pure
  code, defined by a target component; the host build falls back to the portable code.

## 7. The compiler

- **`-O2` for inner loops:** the build's `-Os` leaves them several times slower; hot files set
  `-O2` in their component's `CMakeLists.txt`.
- **Float registers run out in big functions:** inside TinySoundFont's `tsf_voice_render`, GCC
  kept the filter's coefficients in integer registers and moved them across (`fmv.w.x`) every
  sample; the same loop in a function of its own (called once a block) went from 58 to 53
  cycles a sample. Read the disassembly of a hot loop (`riscv32-esp-elf-objdump -d` on the ELF,
  `--start-address` at the symbol `nm` gives).
- **A loop for each case, not a branch per element:** PNG's unfiltering (a loop per filter) and
  the synth's voice loop (filter and looping as constants) both gained from it.
- **Integer-to-float conversions are costly:** the voice loop gained 15 % by interpolating in
  integers and converting once instead of converting three values.
- **No double-precision FPU:** a `double` is software (the ROM's libgcc; `tools/bmds-prof` shows
  the samples as `??`, called from the code that uses the doubles). SVOX Pico's fast exponential
  was double: a tenth of its synthesis; in `float`, 11 % faster (`devplan/speech.md` §12). Look
  for `double` and unsuffixed constants (`0.5`) in vendored DSP code. musl's `sinf`, `cosf` and
  `powf` work in `double` inside, so calling them per point (a circle's 125) is software
  arithmetic: Preso turns a step instead (`ellipsePoints()`, two sines a circle) and cubes its
  easing by hand (`devplan/preso.md` §14, step 6).
- **Mixing RGB565 pixels:** spread a pixel as `(p | p << 16) & 0x07E0F81F` and its three
  channels mix in one multiply by a 5-bit opacity, then fold back (`m | m >> 16`). Preso's
  per-pixel `Canvas::blend` (unpack, a general colour mix, repack, through calls) took 5.6 ms for
  a 120-pixel circle filled half transparent on a PSRAM canvas; mixed this way, 2.3 ms
  (`core/raster.cpp`). 32 levels show no banding on antialiased edges.
- **`#pragma GCC unroll 4` didn't help** the voice loop (68 cycles a sample either way).
- **`std::atomic<bool>::load` isn't always inlined at `-Os`,** and an out-of-line copy is a weak
  function in flash. The scanout's DW-GDMA interrupt (`BMDS_ISR_ATTR`, IRAM-safe: it runs while
  the flash is written, the cache off) called it, so when `/flash`'s write worker ran on core 0
  and the interrupt came on core 1, core 1 took a **cache error** (`Guru Meditation ... Cache
  error`, MEPC in flash at `std::atomic<bool>::load`). Latent since the scanout's `_halt` flag;
  found when `bench`'s stall probe moved the write worker onto core 0 (2026-10-17,
  `devplan/bench.md` §11). In IRAM code use the builtins (`__atomic_load_n`), which always
  inline. To check a build: disassemble `.iram0.text` and look for calls into flash,
  `riscv32-esp-elf-objdump -d -C -j .iram0.text build/bmds.elf`, then each `jalr` whose target is
  a `0x40...` address. Only interrupts allowed while the flash is busy matter (the DW-GDMA's and
  I2S's here: `CONFIG_DW_GDMA_ISR_IRAM_SAFE`, `CONFIG_I2S_ISR_IRAM_SAFE`).
- **`spi_flash_cache_enabled()` read in an IRAM-safe ISR is stale by the time it is used.** Nothing
  holds a flash operation off while such an interrupt runs on the other core: IDF stalls that core
  at task level only, so its IRAM-safe interrupts keep coming, and the cache can be suspended
  between the check and the access it guards. (`cache_hal_suspend()` also stops the cache before
  it clears the flag the function reads.) The scanout's interrupt checks it once at its start
  (`notify`, `scanout.cpp`) and calls `Surface::notifyFromIsr()`, which is in flash, some
  microseconds later: on 2026-11-15 an update's flash write on core 0 caught one between the two,
  and core 1 took a cache error at `notifyFromIsr` (RA just after `surface.frameFromIsr(notify)`;
  core 0 in `spi_flash_hal_disable_auto_suspend_mode`). A check-then-act on the cache's state
  isn't a guard: code an IRAM-safe interrupt may reach must be in IRAM and touch only internal
  RAM, or the flash writer must wait for it. **Fixed** (0.3.1) by the second: `FlashCache`
  (`bmds_drivers`, `bmds/flashcache.h`) wraps IDF's
  `spi_flash_{disable,enable}_interrupts_caches_and_other_cpu`, marking the flash busy and waiting
  for the interrupts inside before the cache is suspended; an interrupt counts itself, then reads
  the flag (seq_cst both sides). **Reproduced in seconds** by `surfdemo` (a present a frame, so the
  interrupt wakes waiters) beside a shell loop writing 64 KB to `/flash` with `sync`: 0.3.0
  panicked within 20 s; with the fix, 740 rounds (15 minutes) clean, 60 Hz held, no underruns.
  The writes themselves hold surfdemo to ~6 presents a second (52 without): the cache is
  suspended for the erases, and it runs from PSRAM.
- **A panic on a task with a small stack can hang in the handler,** which runs on that stack:
  the console shows nothing and the chip resets later by the hardware watchdog
  (`rst:0x7 (HP_SYS_HP_WDT_RESET)`, the other core's saved PC in `panic_handler`'s busy-wait).
  The same crash on a 4 KB stack printed the cache error above. Read the console with a bounded
  pyserial loop (opening the USB serial port resets the chip, so start reading before the
  failure).

- **`ld --section-ordering-file` doesn't move sections between output sections** (binutils
  2.45, esp-15.2.0): its rules only order input sections inside the output section the main
  script already assigns them to. To gather chosen input sections elsewhere, rename them in
  copies of their objects (`objcopy --rename-section`) and link the copies
  (`sdk/tools/bmds-link`, `devplan/programs.md` §14.3).
- **The toolchain's libgcc is medlow:** its 64-bit division helpers and `__clzsi2`/`__ffssi2`
  reach `__clz_tab`, and `__divdf3`/`__divtf3` their jump tables, with `lui` (`R_RISCV_HI20`),
  though BMDS programs build `-mcmodel=medany`. Each is a patch the loader applies.

## 8. Measuring

- **Cycles:** `csrr rd, mcycle` around a section (360 MHz on this silicon). Remove the
  instrumentation after. A `printf` in the synth, running in a background `play`, never showed on
  the console; `bmdsInfo()` did (read with `log show`).
- **`bench run simd`** times each vector kernel against its scalar twin (`devplan/bench.md`;
  `Stopwatch` for microseconds and cycles); **`simd test`** checks them byte for byte on random
  input (sizes, offsets, formats). A kernel goes in with both, and a `simd.*` benchmark.
- **Compare against an exact reference, not a library's idea of it:** Pillow's "box" filter isn't
  an area average (`devplan/images.md`).
- **CPU spent on a job:** the board's busy time before and after (`stats`' `cpu.core0.busy_us`
  and `cpu.core1.busy_us`), divided by the work (ms per MB). One look at `top` during a transfer,
  or the `IDLE` tasks' totals in `tasks` read over an SSH login, were off by up to a second a run.
  Where it goes: `prof start` ... `prof stop; prof dump`, then `tools/bmds-prof`
  (`devplan/sysmon.md` §9).
- **Hot state beside a busy core: the TCM.** The cores share one L1 data cache, so another core's
  memory traffic (a game drawing, a big copy) empties it, and whatever every sample touches is
  fetched again and again. The FM synth's voices in internal RAM made it three times slower beside
  Doom (late audio periods); in the TCM (`MemSpace::Fast`, 8 KB, about 7.7 KB free; no cache in
  between, not for PIE's 128-bit accesses), 20 % faster under a synthetic load and no more late
  periods. Its lookup tables in flash or PSRAM made no difference either way (`devplan/audio.md`
  §26, step 4). Measure with `bench run synth.fm.voices busy=1`.
