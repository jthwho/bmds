# ESP-IDF versions

BMDS builds with one pinned ESP-IDF commit, unmodified (`docs/BUILDING.md`). This records each move
and what it cost; the platform's lessons are in `docs/platforms/esp32p4.md` ("Moving to ESP-IDF
v6.1-dev-9279").

## 1. v6.1-dev-1 → v6.1-dev-9279 (2026-11-21)

`ff97953b32a` → `827cc6229fb`, GCC 15.2 → 16.1, mbedTLS 3 → 4.1. Wanted for ADC streaming
(`devplan/io.md` §15.8), which turned out to fail for a reason of BMDS's own and ESP-IDF's order of
writes, not this version (§15.9 there).

- **Code:** renames and the HAL's split into `esp_hal_*` components (each component's
  `PRIV_REQUIRES`); clock sources acquired and released in pairs (SPI holds its source while
  started); SPI's DMA through the new `spicommon_dma_desc_setup_link()`; LEDC's group argument;
  PPA's CLUT calls; DW-GDMA's renamed burst fields; the TLS code on PSA (`tlsserverkeys.cpp`'s key
  made by `psa_generate_key()`, hashes by `psa_hash_compute()`, the bench's ChaCha20-Poly1305 by
  `psa_aead_encrypt()`); Ethernet's descriptors 32 bytes (packed; reached uncached as before).
- **Silent breakage, found:** the cross-core interrupt wasn't installed at all (§ platform notes:
  ESP-IDF's file registers it and is no longer linked); BMDS registers its own. Every `--wrap`
  target and every start-up registration was checked against the old image.
- **Configuration:** pre-v3.0 silicon and newlib pinned in `sdkconfig.defaults` (the defaults
  flipped); GDMA's control functions in IRAM (PARLIO's IRAM interrupts select them, 4.7 KB); the
  bootloader 23,520 bytes (23,904 before; the limit is 24,576). `tools/gen-iochip.py` reads four
  counts from the `esp_hal_*` headers and carries the strapping pins (ESP-IDF's GPIO page no longer
  lists them).
- **Size:** internal RAM +7.3 KB static (+5.6 KB of it IRAM), flash +51 KB (mostly PSA's layer).
  The largest free internal block at rest is 48 KB (63,488 recorded before): `memory.latency`'s
  64 KB case, `render.spans` and four other benchmarks need a 64 KB block and fail, as they would
  have with 63,488.
- **Checked on bmds0:** it boots and confirms; the quick benchmarks against history (cross-core
  switch 8.66 µs, was 8.68; system calls 30 % faster; PSRAM and DMA as every run before); `memtest`
  of all PSRAM at 1.8 V; the DMA engines through the window; ADC readings and streams; PWM; SPI
  (FIFO and DMA, loopback); UART (64 KB loopback at 921600); Ethernet, SSH, TLS's ciphers.
- **PARLIO, by A/B against the old version** (`devplan/io.md` §15.9): the receiver's ring nodes
  no longer end on cache lines (fixed: whole-line syncs), its edge polarity was fixed upstream
  (BMDS's `edge=` now physical), and its DMA bursts are off as before. Then 10 MHz loopback passed
  12 in 12; 20 MHz overruns the reader on either version (a capacity limit, not this move's).
