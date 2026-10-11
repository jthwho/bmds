# SIMD: the P4's PIE unit on everything it fits

Planned 2026-10-10. The user's: attack anything we can with SIMD (`efficiency-every-trick`). The
HP cores have the PIE extension (TRM chapter 4, "Xespv2p2"; this board's revision is the
toolchain's `xespv2p1`): eight 128-bit registers (16 × 8, 8 × 16 or 4 × 32 bits), multiply-
accumulate into two 256-bit accumulators (QACC) or a 40-bit one (XACC), loads and stores of 128
bits folded into the arithmetic, unaligned loads, saturation and rounding, and hardware loops
(`Xesploop`). One part of BMDS uses it already: the glyph renderer (`GlyphRenderer::Kernel::Pie`).

## 1. Rules

- **Integer only.** PIE has no floating-point vectors: audio (float throughout), the MP3 and Vorbis
  decoders and the spectrum's float FFT aren't its, unless moved to fixed point (§3, later).
- **A scalar twin for every kernel**, in plain C++, which the host tests run, and which the PIE one
  matches **byte for byte**: the scalar is written to the PIE one's rounding and saturation. The
  board checks the two against each other on random input (`simd test`).
- **The glyph renderer's form:** inline assembly in the C++ file, behind
  `#if defined(ESP_PLATFORM) && defined(__riscv_xespv) && defined(__riscv_xesploop)`, a kernel
  choice for tests, the scalar elsewhere.
- **Only instructions in both v2p1 and v2p2**, each kernel checked on this board.
- **Not in interrupts:** FreeRTOS saves PIE's registers lazily, per task, on its first use (a save
  area per task that uses it, about 170 bytes); an ISR can't take that trap.
- **Measured before and after,** on the board (`simd bench`), recorded here; a kernel that doesn't
  pay is dropped.

## 2. The candidates, by what they cost now

| Where | What | Now (measured) | PIE's fit |
|-------|------|----------------|-----------|
| `ImageScaler::scale` | across: u8 × u16 taps; down: u16 × u16 rows summed | 1920 × 1080 to 1280 × 720: 730 ms | Very good: a row's sum is a vector multiply-accumulate; across, 3- and 4-channel taps |
| TJpgDec | IDCT (8 × 8, 16-bit), YCbCr to RGB, the 1/2-1/4 reductions | 12 MP at 1/4: 5.1 s | Good for the IDCT and colour (a vendored-code patch, §4); Huffman isn't SIMD |
| `Pixels::fromArgb` / `toArgb` | format conversions, alpha blending (÷255) | to measure (every decode into a non-native format) | Good: byte shuffles and u8 multiplies |
| `Painter::image` | icons blended over a colour, per pixel (÷255) | to measure (every redraw of icons) | Good: u8 × u8, 16 pixels a step |
| `ImageScaler::orient` | turns and mirrors | to measure | Fair: memory-bound; 4-pixel blocks |
| `Png::decode` | unfiltering (Up, Sub, Avg, Paeth), rows to ARGB | 1280 × 720: 580 ms in all | Up and the row conversion vector; Sub, Avg and Paeth chain through each pixel (3 or 4 bytes at once); most of the time is inflate (not SIMD: the ROM's `tinfl` is a separate fix) |
| `Png::write` | filter choice (three candidates' sums of absolute values), B, G, R to R, G, B | 1280 × 720: read 62 + filter 83 ms | Very good: `vabs`/sums, shuffles |
| `Png::crc32`, `Inflate::adler32` | checksums | CRC bit by bit today; Adler the usual sums | Not SIMD for CRC: a table first; Adler's sums in PIE |
| `memcpy`/`memset` in internal RAM | the strips, row copies | to measure | 128-bit loads and stores; PSRAM is bus-bound (DMA is better there) |
| `GlyphRenderer` | glyphs to pixels | done (PIE) | — |
| `Fft` (spectrum) | 1024-point float | to measure | PIE has 16-bit FFT instructions: a fixed-point FFT, later |
| `Blake2s`, `ChaCha20` | 32-bit adds and rotates | rarely run (the random pool) | Low priority |

## 3. Steps

1. **The harness:** `simd bench [KERNEL]` (each kernel, scalar and PIE, on fixed buffers in
   internal RAM and PSRAM, timed) and `simd test` (random input, byte-for-byte equality, a few
   seconds); `Simd::Kernel` (the glyph renderer's choice, made general). The baseline table above
   filled in. The cheap non-SIMD fixes found on the way (a CRC table).
2. **The scaler** (the biggest wait the user sees): the down pass and the across pass in PIE.
3. **Pixels and blending:** `fromArgb`/`toArgb` for every format, `Painter::image`'s blend.
4. **TJpgDec:** its IDCT and colour output as PIE (a patch kept in `thirdparty/tjpgd/patches`, the
   C left as the scalar twin).
5. **PNG:** Up and the row conversion in decoding; the filter choice and the B, G, R swap in
   writing.
6. **What's left by the numbers:** `orient`, `memcpy` in internal RAM, a fixed-point FFT for the
   spectrum, and whatever the measurements put next.

## 3a. As built (2026-10-10), measured on the board

| Kernel | Before | After | |
|--------|--------|-------|--|
| `ImageScaler::scale`, 1920 × 1080 to 1280 × 720 | 730 ms | 291 ms | down first (whole input rows: PIE's QACC), then across (each pixel's channels in lanes); the old order filtered every input row across |
| `GlyphRenderer` (80 cells of 8 × 16) | 398 µs scalar | 184 µs PIE | was PIE already; now a branch loop (below) |
| `PngFilter::row`, a 1920-pixel row | 630 µs | 72 µs | scoring: max − min, min(d, 256 − d), summed in XACC; output: a masked wrapping subtraction |
| `Pixels::swap24`, 1920 × 1080 in PSRAM | 223 ms | 180 ms | vunzipt/vzipt; bound by PSRAM there (it also serves rows in internal RAM) |
| TJpgDec, 1920 × 1080 at full size | 963 ms (TJpgDec's output) | 727 ms | its MCU output replaced (patch): planes gathered, PIE YCbCr to RGB straight into the picture |
| TJpgDec, 12 MP photo at 1/4 | 5.09 s | 3.86 s | averaging in YCbCr before converting (TJpgDec converted every pixel, then averaged) |
| `ImageScaler::orient`, 1920 × 1080 a quarter turn | 1525 ms | 277 ms | not SIMD: start-and-steps, per-size copies, 32 × 32 tiles (the column reads thrashed the cache) |
| PNG decode, 1280 × 720 RGB | 581 ms | 283 ms | not SIMD: a loop per filter (was a switch per byte), the ROM's `tinfl` (a weak hook from bmds_ui), straight RGB to B, G, R rows, a CRC table |
| PNG encode, 1920 × 1080 | 1341 ms | 937 ms | the filter and the swap above; the rest is the ROM's deflate |
| Screenshot to PNG, 1280 × 720 | 542 ms | 463 ms | |

`simd test` checks every kernel against its scalar twin on random input (scale: 200 cases;
glyphs 500; swap24 2000; pngfilter 2000 rows; softjpeg 192 decodes of the codec's own JPEGs, every
sampling and reduction), all identical; `simd bench` times them.

**What the board taught (v2p1)** (kept for every kernel to come in `docs/platforms/esp32p4.md`):

- **The hardware loop (`esp.lp.setup`) isn't safe here:** a loop preempted part-way can resume
  wrong (ESP-IDF's `SOC_CPU_HAS_HWLOOP_STATE_BUG` on revisions up to 1; its context-switch
  workaround). The scaler's first hardware-loop version failed `simd test` at random. Kernels use
  branch loops (one `addi` and `bnez` a pass); **the glyph renderer had the same latent bug** and is
  a branch loop now.
- **PIE's registers:** its instructions take only x8-x15 (and x24-x31); in inline assembly the
  `"cr"` constraint, not `"r"`.
- **CFG:** bit 1 turns on misaligned loads and bit 0 misaligned stores (the kernels set and restore
  them); checked by the tests' random offsets.
- **`vsub.u8` saturates**, and `vssubs` is vector-by-scalar: wrapping arithmetic is built from
  max − min and a compare's mask. `vsat.s16 q, q, lo, hi` clamps as expected (its TRM text reads
  oddly). `srcmb` floors (no rounding mode on v2p1): the rounding goes in as one more
  multiply-accumulate (128 × 1, 0x8000 × 1, 512 × 1). `vunzipt.8`/`vzipt.8` split and weave three
  planes of 16 bytes (RGB).
- **Found on the way, fixed: the JPEG encoder corrupted streams at random.** ESP-IDF invalidates the
  output after the DMA but doesn't clean it before; a buffer just allocated could hold another's
  dirty cache lines, which, evicted during the DMA, landed stale bytes in the stream. `encode()`
  writes the buffer's lines back first (and retries with twice the room if the stream comes near
  filling it: the codec stops at the room without saying so).

**Next, measured:**

- **TJpgDec's IDCT** (269 M of the photo's 1831 M cycles) in PIE, or better at 1/2 and 1/4 a reduced
  IDCT from the low frequencies only (libjpeg's scaled IDCT); its Huffman decoding (about 700 M)
  isn't SIMD.
- **PNG decode is memory-bound now:** the raw image goes through PSRAM three times (inflate,
  unfilter, conversion). Inflating through a 32 KB window in internal RAM a row at a time
  (`tinfl` without non-wrapping output) would leave only the output's write.
- **A blend kernel** (`Pixels::fromArgb` over a colour: icons, RGBA PNGs): exact in PIE's terms
  (`((t + 128) × 257) >> 16` is `(t + 127) / 255` for every t), but four planes, their widened
  halves and the constants need more than PIE's eight registers; worth it only measured.
- **The spectrum's FFT** in fixed point (PIE's 16-bit FFT instructions), `memcpy` in internal RAM.

## 4. Vendored code

TJpgDec's MCU output is replaced by a hook (`thirdparty/tjpgd/patches/bmds-mcu-output.diff`, the
README saying so): `tjpgdcnf.h` defines `JD_MCU_OUTPUT` as `bmds_softjpeg_mcu_output` (in
`softjpeg.cpp`, its scalar and PIE halves there). TJpgDec's own `mcu_output` is left in, unused.

## 5. ABI impact

None: kernels inside the system. `simd` is a new shell command (its output frozen once shipped).
