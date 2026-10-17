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
- **The L2 cache's 128-byte line** (half the misses for streaming) couldn't be chosen with the
  128 KB L2 on this ESP-IDF: `reconfigure` put `CONFIG_CACHE_L2_CACHE_LINE_64B` back each time,
  with or without the size line. The likely cause: the choice and the line size's value share the
  name `CACHE_L2_CACHE_LINE_SIZE` in `esp_system/port/soc/esp32p4/Kconfig.cache`. Not tried
  further. BMDS's DMA allocations are already aligned to 128 bytes (`memory.cpp`).

## 4. The accelerators (`devplan/images.md`)

- **JPEG decoding is baseline only**, and it writes whole pictures with each side rounded up to
  the MCU (8 or 16, by the sampling: not always 16). ESP-IDF refuses pictures whose width × height
  isn't a multiple of 8. Output buffers must be cache-line aligned in address and size.
- **ESP-IDF's YUV-to-RGB in the 2D-DMA is video's limited range** (Y 16-235), which stretches a
  JPEG's full-range colours. Only RX channel 0 converts colour. BMDS loads full-range coefficients
  by wrapping `dma2d_configure_color_space_conversion` at link time (`-Wl,--wrap`); `Decode::
  limitedRange` keeps ESP-IDF's for video.
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
- **The PPA's scaler (SRM) interpolates, and is slow** (`devplan/games.md` §10, measured
  2026-10-14 with `display ppa`, the scan running): a 1-pixel checkerboard scaled 2× comes out
  gray (bilinear: its input macro blocks are 18×18 for 16×16, a pixel around), and no register
  selects nearest neighbour, so pixel art blurs at any factor. ~30-34 Mpixel/s written when
  scaling up, 14.8 at 1× (a 1280×720 RGB888 copy: 62 ms). GRAY8 is chip revision 3 only. Fine
  for photos and video; not for a game's frame.
- **The blender's CLUT** (L8 through a 256-entry palette to RGB888; IDF leaves L8 out) is exact
  and ~39 Mpixel/s: 1.65 ms for 320×200. Its **bypass mode with only the background channel
  never completes**, and a 2D-DMA job that never completes stays in the pool: every later job
  times out until a restart. Keep the foreground channel (an invisible A8 plane).
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
- **What PSRAM takes, with the 720p scan reading 166 MB/s:** the 2D-DMA writes ~150 MB/s
  (RGB565 widened to RGB888: 50 Mpixel/s, a 1280×720 frame in 18.3 ms, over a 60 Hz frame's
  16.7 ms). The CPU reads it at only ~70-80 MB/s through the cache (doubling 640×360 RGB565
  with PIE: 1 ms with the source cached, 6.6 ms from PSRAM). Design for fewer bytes: rows
  repeated by the scan's descriptors rather than written twice, damage rather than whole frames,
  sources brought into internal RAM by DMA rather than read by the CPU.
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

## 4b. The crypto engines (`devplan/network.md` §26-27)

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
- **Seeing it:** the profiler samples at the tick, which can't interrupt a critical section, so
  time inside one is charged to `vPortExitCriticalMultiCore` with the caller (`xQueueGenericSend`:
  a give or unlock; `xQueueSemaphoreTake`: a take or lock) as its return address.
- **The interrupted context is at `pxTopOfStack`:** ESP-IDF's RISC-V interrupt entry stores the
  task's stack pointer in its TCB when the interrupt didn't nest, and the frame there starts with
  `mepc`, then `ra` (`riscv/rvruntime-frames.h`). A tick hook reads them (`sysmon.md` §9);
  `xPortInterruptedFromISRContext()` is the nesting depth (above 1: the tick interrupted a handler).
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
- **The ETM makes hardware-timed edges:** a GPTimer's alarm event connected to a GPIO toggle task
  changes the pin at the match, with no interrupt in between.
- **Opening the console's USB serial port with DTR and RTS set low first reset the chip**
  (pyserial, 2026-10-11): the boot log showed `rst:0x17 (CHIP_USB_UART_RESET)`. Opening it with
  pyserial's defaults doesn't: `tools/bmdsboard.py` (`bmds-run`, `bmds-compat`) left the uptime
  running (2026-10-12). Which line change USB-Serial/JTAG takes for its reset wasn't pinned down.
  Capturing a panic still means having the port open before it happens.

## 5. ESP-IDF

- **Wrapping a driver function** is legitimate and cheap: `target_link_libraries(lib INTERFACE
  "-Wl,--wrap=fn")`, then `__wrap_fn` calls `__real_fn` and adjusts (`jpegcodec.cpp`).
- **Read the driver before trusting it with a DMA:** what it syncs, when, and what it reports at
  a buffer's end (the JPEG encoder above, the USB-Serial/JTAG FIFO in `docs/HARDWARE.md`).
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
  for `double` and unsuffixed constants (`0.5`) in vendored DSP code.
- **`#pragma GCC unroll 4` didn't help** the voice loop (68 cycles a sample either way).

## 8. Measuring

- **Cycles:** `csrr rd, mcycle` around a section (360 MHz on this silicon). Remove the
  instrumentation after. A `printf` in the synth, running in a background `play`, never showed on
  the console; `bmdsInfo()` did (read with `log show`).
- **`simd bench`** times each vector kernel against its scalar twin; **`simd test`** checks them
  byte for byte on random input (sizes, offsets, formats). A kernel goes in with both.
- **Compare against an exact reference, not a library's idea of it:** Pillow's "box" filter isn't
  an area average (`devplan/images.md`).
- **CPU spent on a job:** the board's busy time before and after (`stats`' `cpu.core0.busy_us`
  and `cpu.core1.busy_us`), divided by the work (ms per MB). One look at `top` during a transfer,
  or the `IDLE` tasks' totals in `tasks` read over an SSH login, were off by up to a second a run.
  Where it goes: `prof start` ... `prof stop; prof dump`, then `tools/bmds-prof`
  (`devplan/sysmon.md` §9).
