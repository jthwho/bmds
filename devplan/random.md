# Random Numbers: the Kernel's Pool

Status: **done** (2026-10-06; verified on the board).

## 1. Goal

Random bytes good enough for keys, for `/dev/urandom`, `/dev/random` and the `getrandom` slot (programs' `getrandom()`, `getentropy()`, `arc4random`), from boot on, never blocking, and fast.

## 2. Facts (ESP-IDF v6.1-dev, checked 2026-10-06)

- The hardware generator (`esp_fill_random`) is truly random only while a main noise source is on. On the P4, with no radio, that is the SAR ADC. IDF's documentation says the P4 keeps it on at application start, but the code doesn't: `bootloader_init.c` turns it on and `bootloader_utility.c` turns it off unconditionally before the app runs, and nothing in the app turns it on again.
- What stays on is the secondary source, jitter of the 8 MHz RC oscillator, mixed in by the hardware. Espressif reports that it passes Dieharder without the main source, but guarantees true randomness only with it.
- `esp_fill_random` waits between words: about 60 KB/s, busy-waiting (measured: 1 MB took 16.9 s and set off the task watchdog).
- `bootloader_random_enable()` / `bootloader_random_disable()` (bootloader_support) turn the SAR ADC source on and off from the app; the ADC can't be used for anything else meanwhile. Nothing in BMDS uses the SAR ADC.

So the hardware is probably good after boot but not guaranteed, and slow. Espressif's advice for this case is a software DRBG seeded from the hardware, which is what Linux does too.

## 3. Design

`Random` (bmds_kernel): one 32-byte key, the pool (`RandomPool`, pure, in bmds_core), under a mutex held for microseconds (a spinlock would turn interrupts off meanwhile, and only exists on the target).

- **Mixing in** (`Random::mix(bytes)`): key = BLAKE2s(key ‖ bytes). BLAKE2s (RFC 7693, in bmds_core) is Linux's choice for the same job.
- **Reading** (`Random::fill(out, n)`): 32 fresh bytes from the hardware generator are mixed in (so its secondary entropy keeps flowing in); then one ChaCha20 block under the key gives the next key (replaced at once: "fast key erasure", so a key stolen later can't recover earlier output) and a key for this read; the output is that read's ChaCha20 stream, made outside the lock. About 0.5 ms of hardware per read, then ChaCha20's speed.
- **At boot** (`Random::init()`, the first kernel stage): the SAR ADC source on, 256 bytes from the generator mixed in (truly random, by Espressif's terms), the source off again.
- **The seed file**, `/boot/random-seed` (64 bytes), as Linux distributions keep one: after `/boot` mounts, its contents are mixed in; the storage service writes a new one 5 s after its first card check (writing it took about 0.35 s on the wear-levelled FAT `/boot` had then, which in the boot stage made boot that much slower), and a shutdown handler writes it again before a restart. A crash in those first seconds can see the same file mixed in twice; the boot's own noise, mixed in first, differs every time. A missing or short file is fine. Anyone can read it (`/boot` has no owners and every user is root), as anything else in `/boot`; a copied `/boot` image doesn't give two boards the same output, since the boot entropy and the hardware's bytes are mixed in too.
- **Users:** `/dev/urandom` and `/dev/random` (64 KB at most a read), the `getrandom` slot (`bmds_k_random`, which read the hardware directly). Nothing blocks or waits for "enough" entropy: the pool is seeded by real noise before anything can ask.

## 4. Budget (estimate)

A few KB of firmware (BLAKE2s, the pool); 36 bytes of `.bss` and the lock; a few ms at boot (256 bytes of the generator); two writes of 64 bytes to `/boot` per boot (littlefs commits them; the flash is written about 0.1 s after the file closes, `storage.md` §13d).

## 5. ABI impact

None: `getrandom` and the device nodes keep their interfaces. `/boot/random-seed` is a new file in `/boot`.

## 6. As built (measured 2026-10-06)

- The bootloader's log confirms the source goes off before the app: `boot: Disabling RNG early entropy source...`.
- The `random` stage takes 4.6 ms (256 bytes with the SAR ADC on). Boot stays at 371 ms since reset; the `boot` stage is 3.9 ms (with the seed write in it, it was 340 ms).
- `/boot/random-seed` differs at every boot; it changes 5 s after boot and at `reboot`.
- `/dev/urandom`: 1 MB in 0.40 s with 4 KB reads, 0.27 s with 64 KB reads (each read mixes 32 bytes of the hardware generator, about 0.5 ms). Reading the generator alone, 1 MB took 16.9 s.
- `progtest random` (`getentropy`, through the `getrandom` slot) prints a different value each run; the signals, spawn, poll and threads suites pass.
- Firmware +3,594 bytes (727,146 to 730,740: BLAKE2s, the pool, the seed file); `.bss` +128 (the mutex and the key).

## 7. Testing

- **Host** (done): BLAKE2s against RFC 7693's `abc` and Python's `hashlib.blake2s` (empty, one block, a block and a byte, 1,000 bytes, fed in 7-byte pieces); ChaCha20 against RFC 8439 §2.3.2; the pool is deterministic from its input, each read gets its own key, input changes everything after, and two kernel reads differ.
- **Board** (done): the boot log shows the seed read; `/boot/random-seed` changes at each boot and at `reboot`; `/dev/urandom` speed against the per-read cost above; the suites still pass.
