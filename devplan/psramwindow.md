# PSRAM Window: Large Allocations Through the MMU

Planned 2026-10-28 (the user's question: can the MMU deal with memory fragmentation?).

## 1. Goal

A large PSRAM allocation succeeds whenever enough memory is free, however the heap is cut up.
The MMU makes scattered 64 KB physical pages look like one range of addresses. BMDS already does
this for programs' images (`programs.md` §14.2, `imagewindow.cpp`); this plan makes it the way
every large `Psram` allocation is made, through `Memory::alloc()`, so no caller changes.

Not a goal: small allocations (they stay on the heap and can still fragment it, as nothing can
be moved), internal RAM (the MMU doesn't translate it), demand paging (§2).

## 2. Facts

- **What broke** (`synthmodule.md` §2a, 2026-10-28): after a 24.5 MB font load failed and the
  font cache gave back 19 MB, PSRAM had 30 MB free and no block over 13 MB, until a reboot.
  Earlier, a program's 25.4 MB allocation failed the same way (`programs.md` §14.1: freed memory
  lay apart from the largest block).
- **The MMU** (`programs.md` §2): external memory only, 64 KB pages, 1,024 entries for PSRAM
  addresses `0x48000000`-`0x4C000000` (64 MB), one table for both cores. An entry is a physical
  page and a valid bit: no protection, no address-space id. A miss is a cache-error panic, not a
  restartable fault, so every page is mapped before it is used.
- **The addresses:** IDF's PSRAM heap maps all 32 MB linearly at `0x48000000`-`0x4A000000`. The
  other 32 MB of addresses are free; the image window reserves 16 MB of them at `0x4A000000`
  (`CONFIG_BMDS_IMAGE_WINDOW_MB`), and 16 MB are unused.
- **Already solved, in `imagewindow.cpp` and `docs/platforms/esp32p4.md` §5:**
  - writing entries with the caches on, under a spinlock, for addresses nobody uses yet;
  - reading an entry back through the index/content registers, not `esp_mmu_vaddr_to_paddr()`
    (which turns the caches off and asserts on a PSRAM stack);
  - **the caches are tagged by address, not by page,** so the same physical page under two
    addresses has two sets of lines. Before mapping: the page's lines under its heap address
    dropped (an earlier owner's dirty line would land on it later). After mapping: the range's
    stale lines dropped. Before unmapping: the range invalidated (a dirty line written back to
    an unmapped address goes nowhere).
- **Getting physical pages from the heap:** a 64 KB-aligned block is physical pages (the heap's
  mapping is linear). But a block a page leaves a gap just under 64 KB before each (TLSF's header
  sits in the previous page): 11 program instances of 16 single-page blocks cut the largest free
  block from 23.6 to 1.1 MB (§14.2). Pages have to be taken in runs.
- **TLSF writes the last word of an allocated block** (the next block's `prev_phys_block`),
  through the cache, now and then (`display.md` §5c; `Memory`'s `dmaSize()` adds a line for DMA
  buffers). A run whose last page is mapped elsewhere must not end where the heap writes.
- **DMA:** the SDMMC writes through window addresses correctly (the §14.2 spike); BMDS's SD
  driver uses `Memory::isExternal()`, which knows the image window. IDF's
  `esp_ptr_external_ram()` and `esp_ptr_dma_ext_capable()` are false for window addresses; the
  swap chain's 2D-DMA and the crypto engine check those and fall back to the CPU. The other DMA
  engines (AXI-GDMA, 2D-DMA, the DSI scan, JPEG, PPA) are untried through the window.
- **Who frees:** `Memory::free()` is the only caller of `heap_caps_free()` in BMDS's code, so
  one place can tell a window address from a heap one.

## 2a. Measured (2026-10-28, step 1)

On the board at 360 MHz, desktop up, with three benches added for this (`memory.window.mapping`,
`.speed`, `.largest`, in `bmds_loader/src/benchwindow.cpp`; the 16 MB of addresses the image
window leaves, reserved on first use).

**Through the window is the same memory at the same speed.** 8 MB, the same physical pages
through their heap addresses and through window addresses:

| | Heap addresses | Window addresses |
|---|---|---|
| CPU write (written back a chunk at a time) | 65-67 MB/s | 65-67 MB/s |
| CPU read (from PSRAM) | 126-129 MB/s | 126-131 MB/s |
| AXI GDMA copy | 71.5 MB/s, 0 words wrong | 71.9 MB/s, 0 words wrong |

Written through one address and read through the other (each side's lines dropped between): 0
words wrong. **The AXI GDMA takes window addresses** (`esp_async_memcpy()` accepted them) at full
speed.

**Mapping is cheap, once two IDF costs are avoided:**

| µs | 64 KB | 1 MB | 8 MB |
|---|---|---|---|
| Map, as `ImageWindow` does it (IDF's entry writes, the caches a page at a time) | 16 | 214 | 1,694 |
| Map, entries written directly, the caches in one call each | — | 19 | 45 |
| Unmap, the caches in one call (entries still IDF's) | 10 | 20 | 93 |
| MMU entries, IDF's `mmu_ll_write_entry()` | 5 | 67 | 535 |
| MMU entries, written directly | 2 | 5 | 24 |
| Invalidating the range, a page at a time | 8 | 106-117 | 830 |
| Invalidating the range, one call | 7 | 7 | 7-11 |
| An aligned heap block (the gathering, one run) | 3-4 | 3-4 | 4 |

- **`mmu_ll_write_entry()` costs ~4.2 µs an entry:** it reads the flash encryption's eFuse three
  times (once for the value, twice for an anti-fault check). Reading it once and writing the
  index and content registers directly: **~0.19 µs an entry** (8 MB, 128 entries, in 24 µs under
  one hold of the lock; 16 a hold, 31 µs).
- **Invalidating by address costs ~7 µs whatever the range** (64 KB or 8 MB): the cache is swept,
  not the range, so the cost is the cache's size. A page at a time pays the call's ~6.5 µs a page.
  Checked, not assumed: 64 KB read through the heap's addresses (cached, clean), rewritten
  through the window, then the heap's 8 MB invalidated in one call: 0 stale words.
- So mapping 8 MB costs about 45 µs and freeing it about 93 µs (25 µs or so once the entries'
  clearing is written directly too): **about 6 µs a megabyte.**

**The largest free block from TLSF's bitmaps is exact enough and free:** with 665 blocks (47
free) in the PSRAM heap, IDF's walk took **144 µs** under the heap's lock; the bitmaps (the highest
first-level list, then its highest second-level one; 19 lists of 32) **about 0.2 µs**, and gave the
same 23,592,960 bytes (a list's lower bound: within 1/32 of the true size).

**Large allocations in a few minutes of use** (`log/debug Memory`, added for this: each block of
64 KB or more, its space and its caller): a program run is a 64 KB aligned private page
(`ImageWindow::map`) and 64 KB task stacks (`Task::start`); a font's samples 5.76 MB
(`PsramDma`, the synth's loader); a screenshot 2.76 MB (the frame) and 164 KB (the PNG: internal
RAM refused it, then PSRAM); `log`'s and the bench's buffers 64-70 KB. A day of use is still to
log; the switch is there for it.

**What follows for the design:**
- **The window is cheap: use it** (§7). `AlwaysBytes` at **256 KB**: above, ~6 µs a megabyte is
  noise beside filling the block. Between 64 and 256 KB, the heap unless §3c's fragmentation
  rules say otherwise, so the many 64 KB blocks (stacks, a program's pages) don't churn the
  window's addresses.
- **L from TLSF's bitmaps** (§3c), behind an accessor with the layout checked at build time.
- **`PsramWindow` writes entries itself** (the encryption state read once) and invalidates whole
  ranges in one call; `ImageWindow` gains both when it moves onto it (for BusyBox's 9 pages,
  about 0.1 ms of a ~10 ms launch today, estimated from these figures).
- **Not everything may go:** blocks handed to IDF code that checks addresses itself stay on the
  heap until audited: task stacks (the cache-disabling paths check whether a stack is in PSRAM by
  its address range, and a window stack would pass that check wrongly), and drivers whose DMA
  checks use `esp_ptr_external_ram()` or `esp_ptr_dma_ext_capable()`. `Memory::alloc()`'s
  callers that need that say so (a flag), rather than the window guessing.

## 2b. As built (2026-10-28, steps 2-5)

- **`PageWindow` and `PageRuns`** (`bmds_core`, host-tested: first fit and last fit, the longest
  free range, growth in place, gathering from a cut-up fake heap, too many runs, a 2,000-step
  soak). Found by the tests: the first run's size has to leave two pages (the heap's header in
  front, the line after), or every first try fails and halving gives too little.
- **`PsramWindow`** (`bmds_kernel`, boot stage `window` right after the console): all 32 MB of
  free addresses (512 pages) at `0x4A000000`, entries written directly (16 a hold of the
  spinlock), whole-range invalidation, the heap's pages checked in order at boot.
- **`Memory`:** `alloc(space, bytes, Options)` (alignment, `Lifetime::System` from the top,
  `heapOnly`), `realloc()` (in place in the window, else one copy), `windowUsage()`, and
  `isExternal()` knowing the whole window (`setProgramWindow()` gone). The policy of §3c with
  `AlwaysBytes` 256 KB and L from TLSF's bitmaps (checked against IDF's walk at boot; the walk if
  they disagree). Task and thread stacks are `heapOnly`, and so are programs' private pages
  (`ImageWindow` maps them itself).
- **`ImageWindow`** takes its ranges from `PsramWindow` (programs' share capped by
  `CONFIG_BMDS_IMAGE_WINDOW_MB`), its entries written directly and its ranges invalidated whole:
  `programs.launch` 5.8 ms.
- **DMA** (step 4, `memory.window.dma`): the DW-GDMA, the 2D-DMA, the PPA and the JPEG codec
  copy both ways between window and heap buffers bit for bit (the AXI GDMA and the card before).
  So `PsramDma` goes through the window too and the contiguous reserve is 0; the swap chain's
  image check is `Memory::isExternal()` (it refused window images); the SD card sends window
  buffers straight to DMA only when the L2's line aligns them. Surface 0's buffers are
  `Lifetime::System`: the display scans them from the window's top (`0x4BAA0000`), 0 underruns
  under `memory.traffic`. The crypto engine still checks IDF's way (window buffers by the CPU).
- **Programs' `mem_realloc`** and the synth's `realloc` are `Memory::realloc()`.
- **Shown:** `mem`'s `window` line (pages used, the longest free range, blocks and runs, the
  policy's choices, fallbacks, growth), and `mem.window` in the system's counters.
- **Measured** (`memory.window.mapping`, `.fragment`):

  | | 64 KB | 1 MB | 8 MB |
  |---|---|---|---|
  | `Memory::alloc()` through the window | (heap) | 38 µs | 85 µs |
  | `Memory::free()` of it | 3 µs | 21 µs | 54 µs |

  The heap cut into 1 MB holes (13.4 MB free, its largest block 3 MB): a 6.7 MB block in 6 runs,
  144-230 µs, every word right. `realloc` from 1 to 8 MB a megabyte at a time: 7 of 7 in place.
- **Today's failure, again:** the font cache holding 19 MB (FluidR3's piano and strings,
  TimGM6mb), an 18.9 MB font loaded: the first try out of memory, the cache emptied, the second
  through the window (18.6 MB in 1.39 s). Before, the second failed on the cut-up heap.

## 3. The design

### 3a. One window for both

`ImageWindow`'s address bookkeeping and MMU primitives move to `bmds_kernel` as **`PsramWindow`**:

- all 32 MB of free addresses reserved at boot (`esp_mmu_map_reserve_block_with_caps()`, as
  now), a bitmap of 512 pages, first fit;
- `map(pages[], count)` → an address: a range found, each entry written, the stale lines dropped;
  `unmap(address, count)`: invalidated, entries made invalid, the range freed;
- `contains(p, bytes)`, for `Memory::free()` and the DMA checks.

The image window becomes a user of it (its own policy, shared pages, stays in the loader).
`CONFIG_BMDS_IMAGE_WINDOW_MB` becomes a limit on what programs' images may take of it, not a
separate reservation. The portable part (the bitmap, first fit, the run gathering of §3b) goes in
`bmds_core`, host-tested against a fake page table.

### 3b. Large allocations

`Memory::alloc(MemSpace::Psram, bytes)` (and `allocAligned()` with an alignment up to 64 KB)
chooses the heap or the window by **§3c's policy** (the user, 2026-10-28: the window when mapping
is cheap; the choice by the heap's fragmentation, not a fixed size). Through the window:

1. **Gather runs:** the pages needed, as a few 64 KB-aligned heap blocks, the largest
   the heap has first, then smaller, until enough. Each run is `k × 64 KB + one cache line`: the
   heap's write into the last word lands in that extra line, never in a mapped page. The space
   before each run's start stays ordinary free heap for small blocks.
2. **Map** the runs' pages side by side in the window (§3a), and keep a small record (the runs'
   heap addresses, the page count) in a table indexed by the range's first page.
3. If the window has no range that long, or the heap no pages, give back what was taken and try
   the heap's plain block; if that fails too, the reclaimers run (as now) and both are tried once
   more. A plain heap allocation that fails tries the window the same way. The policy picks the
   first route, never whether an allocation succeeds.

`Memory::free()` of a window address: invalidate, unmap, free the runs. Freeing gives contents
away, so nothing is written back.

The record costs a few words a run, in internal RAM: a run list of up to 16 entries an allocation
(more runs than that, and gathering stops: so cut up a heap is better reported as `NoMem`).

### 3c. The policy: heap or window

The signals, all cheap:
- **F,** the heap's free bytes: exact, O(1) (`heap_caps_get_free_size()`).
- **L,** its largest free block. IDF's `heap_caps_get_largest_free_block()` walks every block
  under the heap's lock, too slow for each allocation. Instead, one of (step 1 picks):
  - **TLSF's own bitmap:** the highest first-level bit set is the largest free block's size
    class, so L is known to within a factor of two (a lower bound) in O(1). Needs a small
    accessor into IDF's TLSF control structure (its layout pinned by a build-time check);
  - **the sysmon's sample:** its once-a-second pass already gathers the heap's figures; the
    allocator reads the last one, refreshed after any failed allocation.
- **W,** the window's longest free range: its own bitmap, O(1) kept up to date.
- **The map's cost,** from step 1, as a size `AlwaysBytes` above which the window is always used.

For a request of `r` bytes, the window when **all** of:
1. **`r` is at least a page** (64 KB);
2. **rounding wastes at most an eighth of `r`:** `roundUp(r, 64 KB) - r ≤ r / 8` (a 192 KB block,
   three exact pages, qualifies; a 200 KB one, 56 KB of waste, doesn't);
3. **the window has room:** `W ≥ roundUp(r, 64 KB)`, and its longest range isn't short (below a
   quarter of its free pages: its own addresses fragmenting, so large blocks go to the heap while
   it can take them);

and **any** of:
4. **`r` doesn't fit the heap comfortably:** `r > L / 2` (L being a lower bound);
5. **the heap is already cut up:** `L < F / 2`;
6. **the heap's reserve would go:** `L - r` below `ContiguousReserve`, the contiguous space
   `PsramDma` buffers need while their engines aren't verified through the window (§3f: a
   full-screen switch's double-buffered surface, two blocks of about 2.76 MB at 720p RGB888);
   0 once they are;
7. **`r ≥ AlwaysBytes`** (from step 1; if mapping is cheap, a few hundred KB).

Otherwise the heap. Every choice is counted (`mem`'s window line: by heap, by window, and which
rule chose), so the rules can be tuned from what a day of use shows.

### 3d. Growing in place

`Memory::realloc()` (new; the synth's and programs' `mem_realloc` use their own copies today)
grows a window allocation by gathering pages for the difference and mapping them after it, when
the addresses after it are free: no copy, the pointer kept. Otherwise a new range and one copy, as
any realloc. Shrinking unmaps and frees whole runs from the end.

### 3e. DMA

- `Memory::isExternal()` already knows the image window; it knows the whole window. The swap
  chain's and the crypto engine's checks move to it **once each engine is verified** through the
  window (step 4), so nothing quietly falls back to the CPU and nothing gets an address it can't
  reach.
- **`PsramDma` stays on the heap** until the engines its buffers go to (the scan, the 2D-DMA, the
  card, the I²S) are verified (the display's first: §3f); then it uses the window too, its runs
  aligned to `DmaAlign` as now.
  Until then a large `PsramDma` request can still fail on a cut-up heap.

### 3f. Blocks for the system's life, and the display's

The display is the largest user of `PsramDma`, and of two kinds (`surface.cpp`,
`programsurface.cpp`):
- **for the system's life:** surface 0's two buffers (2.76 MB each at 720p RGB888), made at
  `display start` and kept. A permanent block doesn't fragment by itself, but where it lands
  matters: in the middle of the heap, it halves the largest free block until a reboot;
- **late, again and again:** a game's full-screen surface (Alt+Enter) starts a second
  double-buffered `Surface`, about 5.5 MB in two blocks; `display start` after a stop and
  `display try` make surface 0's again. These need contiguous `PsramDma` on a heap that has aged:
  exactly what fails on a cut-up heap.

So:
- **The display's engines are verified first** (step 4: the DW-GDMA scan, the 2D-DMA, the PPA).
  Every PSRAM access already goes through the MMU (the heap's addresses are pages mapped in
  order), so the scan through window addresses should run as fast; `display stress` and the
  underrun counters show whether it does. Once it does, surfaces are window allocations like any
  other, and where their pages lie stops mattering.
- **A lifetime hint** in the kernel's `Memory` (not the ABI): `Memory::alloc(space, bytes,
  Lifetime::System)`. In the window, system blocks take addresses from its top down, and
  everything else first fit from the bottom, so permanent ranges never sit between ranges that
  come and go (the window's own addresses fragment less). Users: surface 0, the synth module's
  samples, the image cache's long-lived entries. Through the heap (before the engines pass), the
  hint does nothing TLSF can act on, so it matters there only by the next point.
- **Until the scan passes:** surface 0 keeps its buffers across `display stop` and `start`
  (reused when the new mode fits), and the policy's reserve (§3c rule 6) is a full-screen
  switch's two blocks, so large non-DMA blocks don't eat the contiguous space a switch needs.

### 3g. What callers see

Nothing changes in the API: a pointer, contiguous, usable until `free()`. Differences:
- a large block's size is rounded up to 64 KB (on average 32 KB more);
- its heap address is never visible (the pointer is the window's);
- `mem` and `Memory::stats(Psram)` count window allocations in use, and report **the largest
  allocation that would succeed now** (the window's longest free range against the heap's pages
  in runs), not only the heap's largest block. `mem` gets a `window` line (pages used, ranges,
  runs, the longest free range).
- **The policy's counts survive resets** (built 2026-10-29): what `mem`'s `history` line shows,
  for judging the thresholds over days of use. The same counts as the `window` line, summed over
  boots, with peaks (window pages used, blocks, the most runs one block took), the heap's lowest
  largest free block the policy saw (and its free bytes then), large requests that failed, the
  boots and the time since it started (the RTC timer, which keeps counting across resets).
  A `KeptRecord` in RTC_NOINIT memory (`keptstate.md` §2c: two checked copies, written
  alternately). The LP RAM is slow (about 500 ns a word), so an allocation only marks the
  history changed and the `kept` thread saves it every 10 s (`sysmon/keep-interval`) and before
  a clean restart: a hardware reset or a panic loses at most that. A power-up, a firmware with
  another layout, a boot through the recovery firmware or `mem clear` starts it again; the boot
  log says which. Checked through `reboot`, a USB control-line reset and `reboot panic`.

## 4. Costs and limits

- **Rounding:** 64 KB pages; the policy's rule 2 keeps the waste under an eighth of a block.
- **The window's own addresses can fragment.** 32 MB of addresses for 30 MB at most of memory,
  shared with programs' images, and only large blocks in it: it fragments far less than the
  heap did, but it can. Watched by `mem`'s longest free range; if it bites, the window takes more
  addresses (the image window's limit lowered).
- **Map and unmap cost** time proportional to size: the entries (microseconds) and the cache
  invalidation (to measure: about a page's worth of `esp_cache_msync()` each). Fine for
  long-lived blocks (fonts, a module's samples, a game's zone, decode buffers), not for blocks
  made and freed in a hot loop, which shouldn't be large anyway.
- **Internal RAM** keeps its rule: static, at boot.
- **Speed through the window:** the same cache and PSRAM; expected identical, measured in step 1.

## 5. Steps

1. **Measure.** Done (§2a): the same speed through the window, the GDMA too; mapping ~6 µs a MB
   with entries written directly and whole-range invalidation; L from TLSF's bitmaps in 0.2 µs
   against IDF's 144 µs walk. Planned as: reading and writing through a window range against
   the heap's; the cost of mapping and unmapping 64 KB, 1, 8 and 24 MB (entries,
   the caches' invalidation); the large allocations a day of use makes (sizes, lifetimes: fonts,
   programs, decoders); reading L from TLSF's bitmap against the sysmon's sample. Settle
   `AlwaysBytes` and how L is read.
2. **Done (§2b).** `PsramWindow` in `bmds_kernel`, the bookkeeping in `bmds_core` (host tests:
   first fit, runs gathered from a fake heap, growth, a soak); `ImageWindow` moved onto it.
3. **Done (§2b).** Large `Psram` allocations through it (§3b, the policy §3c), `mem`'s line, the
   counters. On the board, the font cache's failure repeated (an 18.9 MB font after 19 MB of
   others given back) and `memory.window.fragment`, which cuts the heap up on purpose. Still to
   come: the policy's choices counted over a day of use (`mem.window`).
4. **Done (§2b).** DMA verified engine by engine through window addresses (`memory.window.dma`:
   the DW-GDMA, the 2D-DMA, the PPA, JPEG; the AXI GDMA and the card before; I²S never sees PSRAM,
   its buffers are IDF's in internal RAM); the swap chain's check moved to `Memory::isExternal()`;
   `PsramDma` through the window, the policy's reserve 0.
5. **Done (§2b).** `Memory::realloc()` with growth in place (§3d); programs' `mem_realloc` and the
   synth's allocator on it.

So `synthmodule.md` §3b's arena taken at start went: the module allocates its samples when it
loads a performance, as anything else does.

## 6. Budget (estimates)

- **Firmware:** about 3 KB (the window and the runs; `ImageWindow` loses as much as moves).
- **RAM:** the bitmap and the records, under 2 KB of internal RAM; the 32 MB of addresses were
  half reserved already.
- **CPU:** none on small allocations (one compare); on a large one, the policy's few compares,
  then the gathering and the map, measured in step 1.

## 7. Decisions (the user, 2026-10-28)

- **The window when mapping is cheap** (step 1 measures it): it is (§2a), so the window, from
  256 KB always and below that by §3c.
- **No fixed threshold:** the choice follows the heap's fragmentation (§3c).
- **System-lifetime blocks considered** (the user's question: the display's, made early and
  kept): a lifetime hint, the display's engines verified first, and until then surface 0's
  buffers kept and a reserve for a full-screen switch (§3f).
