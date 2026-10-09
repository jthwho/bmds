# Audio: Devices, the Engine, Program Interfaces, MIDI

Status: **designing** (2026-10-08). The user's request: "I'd really like the audio subsystem to be
very capable, since there's many things I'd like to do with it". Agreed with the user the same
day: the engine (§5), OSS compatibility (§6.2), a native API (§6.1), decoding as a library task
(§7), MIDI as first-class (§8), capture last (§10).

## 1. Goal

The user's requirements:
- **Very low latency processing:** a future guitar effects pedal or similar. The user plans a
  version of the board with a better ADC and DAC; for now the I²S side is what matters.
- **Playing WAV, Ogg and MP3,** and capturing later.
- **A synth core** that takes MIDI and makes audio. MIDI is first-class: over USB and the serial
  port (later in the plan), and between programs.
- **AES67** once the network stack exists. The user has AES67 work on the ESP32-P4 to bring in
  then.
- **A full API for audio development,** matched to how the hardware works.
- **OS basics:** the terminal's bell, audio feedback, volume control with a control in the panel's
  system tray.
- **Every format a sound card takes:** from 8-bit mono at 8 kHz to 16-bit stereo at 48 kHz, and
  everything between.
- **Every path checks errors and reads whatever status the hardware reports,** to make debugging
  easier.

The user has speakers on the jack and says how things sound. Nothing records yet: capture, and
anything that needs it, waits until the user has a loopback cable (§10).

## 2. Facts that shape the design (checked 2026-10-08)

### The codec: Everest ES8311

From the *ES8311 User Guide*, rev 1.00 (Sep 2018; the user has the PDF). **It is marked
confidential, so it isn't committed.** This document cites its sections instead.
- **Mono codec:**
  - one DAC with a differential headphone and line output for 16 or 32 Ω (§11);
  - one ADC behind a 0 to 30 dB PGA, for a microphone; not recommended for line input (§10.1).
- **Sample rates and ports:**
  - DAC rates 8 to 96 kHz (§1).
  - One serial port: I²S, left-justified, DSP-A or DSP-B; 16, 18, 20, 24 or 32-bit words (§7,
    registers 0x09 and 0x0A).
  - The DAC takes the left or the right slot (`SDP_IN_SEL`).
- **Clock: no PLL.** A pre-divider (÷1 to ÷8) and a multiplier (×1, 2, 4, 8) make the internal
  master clock from MCLK or BCLK (§8.3, §8.4). Separate ADC and DAC dividers follow (§8.5).
  The rules, which the driver must check, because the chip reports nothing when they're broken:
  - **pre-divider output:** below 10 MHz at DVDD 3.3 V, below 4.5 MHz at 1.8 V;
  - **internal DAC clock:** at most 35 MHz (3.3 V) or 13 MHz (1.8 V);
  - **DAC clock : LRCK ratio:** at least 256 and a multiple of 16.
  - **Slave mode:** the chip detects the MCLK : LRCK ratio by itself (§8.7). §8.5 tabulates
    register values for common MCLK and LRCK pairs.
- **No status registers** are documented: no lock, no fault, no clock-detect bits. What the driver
  can know about the codec is:
  - that it acknowledges at 0x18;
  - what every register reads back;
  - the chip ID (0xFD/0xFE, used by Espressif's driver). The guide doesn't document it; the board
    reads 0x8311, version 0x01 (2026-10-08).
- **Undocumented registers:** the guide leaves out 0x0B, 0x0C, 0x10 to 0x13 and 0x45. Espressif's
  ES8311 driver writes them, and only bit 1 of 0x12 (`PDN_DAC`) is named. We use Espressif's
  values (`esp_codec_dev`, Apache-2.0, read as reference, not vendored) and record each one with
  its source.
- **Useful built-ins:**
  - DAC volume −95.5 to +32 dB in 0.5 dB steps (0x32), with a soft ramp (0x37, `DAC_RAMPRATE`);
  - mute controls (0x31);
  - dynamic range control (0x34, 0x35);
  - a first-order DAC equalizer (0x37 to 0x43);
  - ADC-to-DAC routing (0x44 bit 7);
  - DAC data back out on ASDOUT (0x44 bits 6:4, "digital feedback", for echo cancellation).
- **Power:**
  - **Start:** set the reset bits and clear `CSM_ON`, wait a few milliseconds, then clear the resets
    and set `CSM_ON` (§9.1).
  - **Standby:** 0x0D, 0x0E and 0x12 power the blocks down (§9.2, §10.2).
  - **Low power:** 0x0F trades performance for current (§9.3).

### The board

- **Pins and power:**
  - **Codec:** ES8311 at I²C 0x18 on the main bus. That bus runs at 100 kHz and also carries
    HDMI DDC and the LT8912B.
  - **I²S:** MCLK GPIO13, BCLK GPIO12, WS GPIO10, DOUT GPIO9, DIN GPIO11.
  - **Codec power:** GPIO6, on by default.
  - The board definition already has all of it (`BoardAudio`).
- **Output and input:**
  - Output only to the 3.5 mm jack; there is no speaker amplifier and no jack detect.
  - The jack's mic contact goes to the ADC.
  - The HDMI bridge has no audio wired.
- **DVDD is 3.3 V** (schematic, Rev C): the codec's own LDO (AP1231-3.3, CE on GPIO6) makes
  +3.3VA for PVDD, DVDD and AVDD. So the 3.3 V clock limits apply. Two FETs disconnect the codec
  from I²C while its supply is off, so a powered-down codec doesn't answer at 0x18 (not a fault).
- **The jack's left and right are OUTP and OUTN,** each AC-coupled (22 µF). Both channels carry
  the one mono signal **in opposite polarity**:
  - with headphones it sounds phasey;
  - a stereo speaker pair loses bass;
  - anything that sums left and right cancels to near silence.

  Software can't undo it with one DAC. `audio status` says so, and the user's listening tests
  should use one channel or reverse one speaker. The planned board should wire a stereo codec's
  outputs single-ended.

### The SoC and ESP-IDF

- **CPU:** the HP cores are `rv32imafc` with PIE SIMD (`xespv`), so they have single-precision
  floating point. They run at 360 MHz; the FreeRTOS tick is 1 kHz.
- **I²S:**
  - **Clock source:** the APLL (`SOC_I2S_SUPPORTS_APLL`), which makes 48 kHz and 44.1 kHz
    multiples exactly (output 5.3 to 125 MHz, `clk_tree_ll.h`).
  - **Frame counter:** a TX sync counter (`SOC_I2S_SUPPORTS_TX_SYNC_CNT`): frames sent, for
    timestamps.
  - **Also present:** TDM and ETM.
- **IDF I²S driver** (`esp_driver_i2s`):
  - **`on_sent`:** the callback hands over the DMA buffer just sent (`i2s_event_data_t::dma_buf`),
    so the engine can write the next audio straight into DMA memory with no copy and without
    `i2s_channel_write()`.
  - **Overflow callbacks:** `on_send_q_ovf` / `on_recv_q_ovf` report dropped buffers.
  - **`auto_clear`:** sends silence when nothing was written.
  - **`i2s_channel_tune_rate()`:** nudges MCLK and reports the frequency it actually reached. This
    is the hook for clock steering (AES67, matching another device).
- **APLL status:** the APLL can't be read as "locked". At start the IDF waits for its calibration
  to finish (`clk_ll_apll_calibration_is_done`, with no time limit). `periph_rtc_apll_freq_set()`
  returns the frequency the coefficients actually give. The RTC calibration counter can measure
  the APLL against the crystal (`CLK_CAL_APLL`).
- **USB:** we run TinyUSB's host stack with only HID vendored. MIDI over USB needs its MIDI host
  class driver; check what upstream offers when we get there.
- **Program interfaces:**
  - **ioctl:** there is no generic ioctl system call. `sdk/crt/linux.c` translates each Linux ioctl
    to a BMDS call, which is how the OSS ioctls will work (§6.2).
  - **Device files:** `Vfs::addDeviceFile()` adds files to `/dev` (the limit, `MaxDeviceFiles` = 4,
    is internal and will grow).
  - **Polling:** `io_poll` exists, so audio fds can be polled.

## 3. Checking errors and status

The codec says almost nothing, so the driver and the engine check everything they can:
- **Codec:**
  - probe 0x18, read and check the chip ID;
  - **read back every register written** and compare it. The bus is shared with whatever a monitor
    puts on DDC, so a write that didn't take is a real possibility;
  - check the clock settings against the §2 rules before writing them;
  - a failed I²C transfer or readback marks the device `Faulted` with the register and the error.
    Output stops (silence), and streams get `IoError` on their next call.
- **Clock:**
  - check the frequency `periph_rtc_apll_freq_set()` reports against the one asked for;
  - measure the APLL against the crystal at start;
  - **measure the real frame rate:** the I²S frame counter against `esp_timer`, over each second
    of play, to show the actual rate and its error in ppm.
- **DMA and the engine:**
  - count overflows reported by the I²S driver;
  - count periods the engine filled late (§5: it compares its position with the DMA's);
  - count underruns per stream;
  - record the engine's time per period: the last, the worst, and the average as CPU load.
- **No polling:** all of this is read at state changes (start, stop, a rate change) and when asked.
  Nothing re-reads registers on a timer.
- **`audio` command:** shows it all, as `usb` does for USB:
  - `audio`: the devices and their state;
  - `audio status [DEV]`: clocks, counters, load, streams;
  - `audio regs [DEV]`: every codec register, with the ones that differ from what we wrote marked;
  - `audio tone [DEV] FREQ [SECONDS]`: a test tone.

## 4. Architecture

```
 programs:  OSS /dev/dsp   native fd API   real-time clients   system sounds   (MIDI §8)
                 \              |                 |                  /
  engine:     streams: convert, resample, gain ──► mixer ──► device gain ──► DMA buffers
  drivers:    AudioDevice: ES8311 + I2S0 (dsp0) │ later: USB audio, AES67, external I²S codec
```

- **`AudioDevice`** (kernel). Numbered: `dsp0` is the ES8311 on I²S0. A device states:
  - its directions;
  - its rates;
  - its sample formats and channels;
  - whether it has hardware volume;
  - its clock domain;
  - a frame counter.

  Later devices are a USB audio class device, AES67 streams, and the user's planned better codec
  on the second board. None of them is special: the board definition says which exist.
- **The engine** (kernel, one per device):
  - mixes the device's streams into its DMA buffers;
  - converts and resamples each stream to the device's format;
  - applies stream and device gain.
- **Clients:** OSS-compatible streams, native streams, real-time clients and system sounds, all of
  them streams of the engine. Exclusive mode (§6.3) is a real-time client with the mixer out of
  the way.
- **Decoders and the synth are not in the engine.** Decoders are a library (§7); the synth is a
  program (§8).

## 5. The engine

- **Its own task:**
  - each device has a real-time task at the highest priority for application work (to be set
    against the display and USB tasks);
  - it is woken by a task notification from the I²S `on_sent` interrupt, so there is no polling;
  - it fills the DMA buffer that plays two periods from now. If the buffer it is about to fill has
    already started playing, that period is counted late and `auto_clear` covers it with silence.
- **Device format:**
  - **rate:** the device's rate is a setting, 48 kHz by default; everything else is resampled;
  - **slots:** I²S Philips, 32-bit stereo slots, with the mix in the left slot, which the ES8311
    DAC takes (`SDP_IN_SEL`). The codec runs 24-bit;
  - **clock:** MCLK = 256 × rate from the APLL, the ES8311 in slave mode;
  - **44.1 kHz:** used when an exclusive client asks for it (§6.3).
- **Mixing:**
  - float32 internally: the cores have an FPU, and effects and synths want float;
  - streams are mixed at full range, and the device gain is applied after;
  - one soft limiter before the conversion to integer, so loud mixes don't wrap;
  - conversions at the edges use PIE where it pays.
  - **To measure first:** float against Q31 for the mix itself, before committing.
- **Period:**
  - a setting, in frames, not limited to powers of two: AES67 sends packets of 1 ms (48 frames)
    or 125 µs (6 frames);
  - **default 5 ms** (240 frames) for desktop use;
  - a real-time client asks for less, down to 1 ms or below, and the device's period follows the
    smallest request.
  - Changing the period reconfigures the I²S channel (a short gap). Whether DMA buffers of the
    smallest size, with the engine woken every k of them, are cheaper is to be measured.
- **Device volume:**
  - uses the codec's DAC volume (0.5 dB steps) with its soft ramp, so changes don't click and cost
    no CPU;
  - a device without hardware volume gets it in the engine;
  - mute ramps down, then mutes the DAC (0x31).
- **Resampling:**
  - a polyphase windowed-sinc resampler, float, one per stream that needs it;
  - integer ratios (8 → 48 kHz is ×6) and fractional ones (44.1 → 48 kHz is 160/147);
  - quality and cost set by the filter length; the value is to be chosen by measuring cost and
    aliasing on the board.
- **Idle:**
  - with no stream open for a while (a setting), the engine ramps to silence, stops I²S and puts
    the codec into standby (§2 power);
  - the first stream wakes it;
  - the user listens for pops at both ends; the codec's soft ramp and VMID charge mode (0x0D) are
    the tools.
- **Memory:**
  - DMA buffers in internal RAM (DMA-capable; the cache is written back after the engine fills
    them);
  - stream rings in PSRAM, sized by the client;
  - no allocation in the engine's loop: a stream's buffers are allocated when it is opened.

## 6. Program interfaces

### 6.1 Native: audio fds (the ABI step)

- **Opening:** a program opens a device file and configures the stream with one call. The
  configuration is a boundary struct beginning with `uint32_t size`:
  - direction;
  - rate (any integer from 8,000 to the device's maximum);
  - format: `u8`, `s8`, `s16le`, `s16be`, `s24le` (in 32 bits), `s24_3le`, `s32le`, `f32le`,
    `mu-law`, `a-law`;
  - channels (1 or 2 now; more later, downmixed);
  - ring size in frames;
  - the start threshold.
- **Writing:** `write()` blocks, or returns `WouldBlock` with `O_NONBLOCK`. `poll()` reports
  writable when a period's worth fits.
- **Calls on an audio fd** (new system calls, appended):
  - configure;
  - info, with what the device supports;
  - position: frames played, and the device frame counter with its time, for A/V sync;
  - latency;
  - pause and resume;
  - drain, and drop;
  - stream volume;
  - status, with the counters of §3.
- **Default device:** `/dev/dsp` is the default device (setting `audio/default`, `dsp0` at
  first). `/dev/dsp0`, `/dev/dsp1`… name each one.

### 6.2 OSS compatibility

- **Device files** with the OSS names: `/dev/dsp`, `/dev/dspN`, `/dev/mixer`, `/dev/mixerN`.
  - **Also `/dev/audio`:** OSS's 8 kHz µ-law device, which is cheap, since µ-law is a format we
    convert anyway.
- **ioctls:** `sdk/crt/linux.c` translates the OSS `SNDCTL_DSP_*` ioctls (`SPEED`, `SETFMT`,
  `CHANNELS`, `GETOSPACE`, `GETODELAY`, `SETFRAGMENT`, `SYNC`, `RESET`, `GETFMTS`, `GETCAPS`, …)
  and `SOUND_MIXER_*` to the §6.1 calls, as it does for terminal ioctls today.
  - **Only translation:** the kernel learns nothing OSS-specific.
- **What it buys:** porting SDL's OSS backend, mpg123, and older games and players with few
  changes.

### 6.3 Real-time clients and exclusive mode (after playback works)

- **Real-time client:** for the pedal and the synth. A program registers a real-time stream and
  runs its own high-priority thread, which loops:
  1. `wait`: blocks until the next period and returns pointers to that period's input and output
     buffers (float);
  2. the thread processes them;
  3. `done`.
- **No callbacks cross the ABI:** the program's code runs on its own thread.
- **Deadline:** missing it counts a late period for that client, and the engine mixes silence for
  it.
- **Exclusive mode:** one real-time client owns the device. The mixer is bypassed, and the client
  sets the rate (44.1 kHz bit-perfect playback) and the period. System sounds wait until it ends.

### 6.4 System sounds, volume and the tray

- **Sounds:** the bell and feedback sounds (click, error, notification) are synthesized from a
  small table: a tone, an envelope, a length. There are no files to load. Later, a sound theme on
  `/boot` can replace them.
- **Playing one:** one system call plays a system sound by number, through the default device's
  mixer. The terminal (a built-in app using only the public API) calls it on BEL.
- **Settings** (sysconf; keys are frozen once shipped):

| Key | Type | Default | Meaning |
|---|---|---|---|
| `audio/default` | string | `dsp0` | The device `/dev/dsp` and system sounds use |
| `audio/dsp0/volume` | int dB, −60 to 0 | −9 | Device volume (§17) |
| `audio/dsp0/mute` | bool | false | |
| `audio/dsp0/rate` | int | 48000 | Device rate for shared use |
| `audio/dsp0/period` | int frames | 240 | Shared-use period |
| `audio/bell` | enum `sound`, `off` | `sound` | The terminal's bell |
| `audio/feedback` | bool | false | UI feedback sounds |

- **Applying changes:** the engine watches its keys (`sysconf_watch`) and applies changes live.
- **Tray control:** the panel's volume control is a tray item. A click toggles mute; a click and
  drag, or the menu, gives a slider. It sets the default device's keys like any other program
  would.

## 7. Decoding: a library in the firmware, for programs

- **Decoders in the firmware:** they are built into the firmware and offered through the SDK, so
  programs don't each carry their own copy.
  - **Kept out of the engine:** they never run inside the engine or a driver.
  - **Who pays:** they run on the calling program's thread, with its memory, so a decoder's cost
    falls on the program using it.
- **One format-blind API:**
  - **Calls:** open (an fd, or a buffer); info (rate, channels, length, tags); decode into a PCM
    buffer; seek; close.
  - **Detection:** the format is found by `file_type()` and the stream's header.
  - **New formats need no new calls:** FLAC, Opus or anything later appends no system calls.
- **Candidates:** each is one file, with a licence compatible with MIT. Flash and memory costs
  are to be measured before choosing (measured and chosen in §19).
  - WAV (ours);
  - MP3: minimp3 (CC0);
  - Ogg Vorbis: stb_vorbis (public domain or MIT);
  - FLAC: dr_flac (public domain or MIT-0).
- **Safety:** decoders parse files from anywhere, so each one is fuzzed on the host with
  ASan/UBSan before shipping, and tested with crafted files, as the PNG decoder is.
- **`play FILE…`:** a built-in program. The file browser's Open uses it for audio types.

## 8. MIDI and the synth (first-class; after the decoders)

- **MIDI ports:**
  - **Device files:** numbered fds, `/dev/midiN`. A raw byte stream, as OSS's raw MIDI is, so
    ported tools work.
  - **Native events:** an event API with timestamps on the audio device's clock, so a synth can
    place a note on the right sample.
  - **Sources:**
    - USB MIDI (a TinyUSB host class driver);
    - serial MIDI on UEXT (31,250 baud; needs an opto-isolated input, as MIDI requires);
    - virtual ports that programs create.
- **Routing:** a small kernel router connects sources to destinations (`midi connect A B`), so a
  keyboard can drive any synth, and one program can play through another. `midi` lists ports and
  connections, and can monitor one.
- **The synth:**
  - a built-in program (`synth`) that is a real-time client (§6.3) and a MIDI destination;
  - first a General MIDI player from a SoundFont on the card, using TinySoundFont (MIT), so any
    program can play MIDI files and get sound;
  - a synth library (oscillators, envelopes, filters, voices) for building instruments, offered
    through the SDK like the decoders.
  - **Polyphony:** limited by CPU and to be measured; the real-time client's load figures show
    the margin.

## 9. Later: other devices and clock domains

- **AES67** (after the network stack; the user's earlier work comes in then):
  - an `AudioDevice` whose clock is steered to PTP with `i2s_channel_tune_rate()` or the APLL's
    fine steps;
  - RTP packets made from the engine's periods (1 ms or 125 µs packet time, hence §5's period
    rule);
  - SAP/SDP announcements.
- **What the engine needs from now on:**
  - a frame counter with timestamps per device;
  - a way to steer a device's clock.
- **USB audio class devices:** stereo, better converters, inputs. Also numbered `dspN`.
- **Two clocks at once:** when two devices run on different clocks, a stream crossing between them
  needs drift-tracking resampling. Not needed until a second device exists.

## 10. Phasing

Each step ends with what the user hears and what the board reports.

1. **Bring-up:**
   - the ES8311 driver: reset sequence, clocks checked against the §2 rules, every write read
     back;
   - I²S0 TX on the APLL;
   - `audio`, `audio status`, `audio regs`, `audio tone`;
   - checks: measured APLL and frame rate; the user listens to tones at 8 to 96 kHz, a sweep and
     levels.
2. **Engine:**
   - mixer, conversions, resampler, device volume and mute;
   - the kernel's own streams, and system sounds and the bell;
   - the `audio/*` settings and the tray control;
   - check: host tests for every conversion and the resampler (§11).
3. **ABI:**
   - native audio fds (§6.1) and `/dev/dsp*` in the VFS;
   - the OSS translation in `linux.c` (§6.2);
   - `play` for WAV;
   - an SDK example.
4. **Decoders:** MP3, Ogg Vorbis, FLAC behind the §7 API; fuzzed; `play` and the file browser's
   Open.
5. **Real-time clients and exclusive mode** (§6.3), with an output-only demo (a tone generator
   driven by the keyboard).
6. **MIDI:** ports, routing, virtual ports, `midi`; USB MIDI; serial MIDI.
7. **Synth:** the GM player and the synth library.
8. **Capture** (when the user has the loopback cable):
   - the ADC path and `record` (WAV);
   - a loopback self-test (tone out, FFT in) and the measured round-trip latency;
   - then the pedal work (input to output real-time clients), on the new board's codec when it
     exists.
9. **AES67, USB audio** (after the network stack).

## 11. Testing

- **Host:**
  - conversions (every format to float and back, exact where it should be);
  - the mixer and limiter;
  - the resampler, measured: frequency response, aliasing and THD+N of a sine through each common
    ratio, by FFT;
  - the WAV parser;
  - the OSS ioctl translation;
  - decoders fuzzed under ASan/UBSan.
- **Board:**
  - register readback;
  - measured clock and frame rate;
  - counters at zero under load (an SD copy, window dragging and the file browser while a stream
    plays);
  - CPU load per period.
- **By ear (the user):**
  - a short checklist per step: tones at each rate, a sweep, volume steps, mute;
  - start and stop pops;
  - a WAV and an MP3 known well.

## 12. Resource budget (estimates; replaced by measurements as steps land)

- **DMA buffers:**
  - internal RAM, one frame = 8 bytes (two 32-bit slots);
  - at the 5 ms default with three periods in flight: 240 × 8 × 3 = 5,760 bytes;
  - at 1 ms: 1,152 bytes.
- **Engine:**
  - mix buffer: frames × 2 channels × 4 bytes per device (1,920 bytes at 240 frames);
  - task stack: internal RAM; size to be measured.
- **Streams:** rings in PSRAM, client-sized. 100 ms of 48 kHz 16-bit stereo is 19,200 bytes.
- **Flash:**
  - engine and driver: to be measured;
  - decoders: to be measured before choosing (§7).
- **CPU:** the engine's measured load is shown by `audio status` and recorded here per step.

## 13. ABI impact

- **New system calls** (appended): the audio fd calls (§6.1), the real-time client calls (§6.3),
  the system sound call (§6.4), the decoder calls (§7), the MIDI calls (§8). Each step lists its
  own.
- **New device files:** `/dev/dsp*`, `/dev/mixer*`, `/dev/audio`, `/dev/midi*`.
- **New sysconf keys:** `audio/*`.
- **New shell commands:** `audio`, `play`, `midi`, `synth`, `record`. Command formats are fixed
  as they ship.
- **OSS:** ioctls through `linux.c`. Linux's numbers are fixed already, so nothing new is frozen
  there beyond what Linux froze.

## 14. Decisions (agreed 2026-10-08)

- **Hardware focus:** focus on the I²S side; the user plans a board with a better codec, and a
  pedal-grade input waits for it.
- **The engine** as §5: per-device real-time task, float mix, 48 kHz default with resampling,
  periods in frames.
- **Program interfaces:**
  - OSS-compatible `/dev/dsp` alongside a native API matched to the hardware;
  - real-time clients and exclusive mode for low latency.
- **Decoders** are a library task: built into the firmware, offered through the SDK, run on the
  caller's thread.
- **MIDI** is first-class: USB, serial, between programs, later in the plan.
- **Capture comes last**, after the user has a loopback cable. The pedal comes after capture, and
  AES67 after the network stack.

## 15. Open questions

- **Engine priority and core:** the engine task's priority and core, against the display, USB and
  the window manager.
- **Small DMA buffers:** fixed small DMA buffers with a variable wake count, against
  reconfiguring per period change (§5).
- **Mix format:** float or Q31 for the mix (§5); measure.
- **Volume:** decided (2026-10-08): in dB, −60 to 0, default −9 (§17).
- **Idle timeout:** before the codec is put into standby, chosen against the pops. (Step 2: 3 s to
  stop, which is silent; standby itself is not used yet.)
- **Flash writes:** decided: flash auto-suspend (§17, as built).
- **Resampler cost:** decided: precomputed rows (§17, as built).

## 16. Step 1: bring-up (design, 2026-10-08)

The user's addition: a command to control the codec, "so we can do things like set left or right
channel to the DAC". It is generic: each device publishes **controls**, and `audio` reads and sets
them, so the planned board's codec needs only its own list.

### Component `bmds_audio`

- **Portable** (host-tested):
  - `AudioControl`: a control's name, type (`bool`, `enum`, `dB`, `int`), range, step and help,
    with its value's text form.
  - The ES8311 clock plan: registers 0x02 to 0x05 for an MCLK and a rate, checked against §2's
    rules.
  - The DAC volume mapping.
  - A sine generator for `audio tone`.
- **Target:**
  - **`Es8311`** (an `AudioCodec`):
    - start: checks the chip ID, then the reset sequence (§2, §9.1 of the guide), every write read
      back;
    - clocks, volume, mute, slot select;
    - standby;
    - a register snapshot, compared with a shadow of what was written.
  - **`AudioDevice` `dsp0`:**
    - created on first use, as `Display` is;
    - owns I²S0 TX on the APLL, the codec and a kernel task.
    - **No engine yet:** in step 1 the task fills buffers from a tone or with silence. Step 2
      replaces the tone with the mixer.
  - **`audio`:** the command.

### Clocks

- **MCLK = 256 × rate** at every rate from 8 to 96 kHz, so the ES8311 runs ÷1 ×1, ratio 256.
- **Registers:**
  - 0x03 = 0x10;
  - 0x04 = 0x20 at 16 kHz and below, 0x10 above (Espressif's table: 0x04, the DAC's oversampling,
    isn't in the guide);
  - 0x05 = 0x00.
- **The guide's 10 MHz limit** on the pre-divider's output is broken by its own table (12.288 MHz
  at ÷1 ×1). Every row that multiplies keeps the divider's output under 10 MHz, so the check
  applies only when multiplying, which MCLK = 256 × rate never needs.

### DMA and the task

- **Buffers:** IDF `i2s_std`, Philips, 32-bit stereo slots, three DMA buffers of one period each
  (240 frames at 48 kHz: 5 ms).
- **The ISR:**
  - `on_sent` (in IRAM) records the finished buffer and a sequence number, and wakes the task;
  - `auto_clear_after_cb` zeroes the buffer first, so a period filled late plays silence, not
    stale audio.
- **The task:**
  - fills that buffer (it plays two periods later), then writes the cache back;
  - if the sequence count has moved two on by the time it finishes, the period was late: counted.
- **Interrupt safety:** `I2S_ISR_IRAM_SAFE`, so the interrupt keeps running while the flash is
  written. The task isn't in IRAM, so a `/boot` write can still cost a gap of silence; to be
  measured.
- **The IDF's send queue overflows** on every buffer when nothing calls `i2s_channel_write()`. So
  `on_send_q_ovf` isn't used; lateness is detected as above.

### Status: what `audio status` reports

- **Codec:** chip ID and version; state (`stopped`, `running` or `faulted`, with the failing
  register and error).
- **Clocks:**
  - the rate;
  - MCLK asked for, and MCLK as the I²S driver set it (`i2s_channel_tune_rate()` with no change);
  - the APLL's frequency (`esp_clk_tree_src_get_freq_hz`);
  - **the measured frame rate:** frames counted against `esp_timer`, with the error in ppm. This
    is the end-to-end check of the APLL against the crystal.
- **Engine:** periods, late periods, and fill time (last and worst).

### Controls (`dsp0`, ES8311)

Names and values are frozen once shipped.

| Control | Type | Register | Meaning |
|---|---|---|---|
| `dac-slot` | enum `left`, `right` | 0x09 bit 7 | The I²S slot the DAC plays |
| `dac-volume` | dB, −95.5 to +32, step 0.5 | 0x32 | DAC digital volume |
| `dac-mute` | bool | 0x31 bits 6:5 | Mute (Espressif's bits) |
| `dac-ramp` | int 0–15 | 0x37 bits 7:4 | Volume soft ramp (0: off; n: 0.25 dB per 2^(n+1) frames) |
| `dac-eq` | bool | 0x37 bit 3 (inverted) | The DAC's equalizer (bypassed by default) |
| `drc` | bool | 0x34 bit 7 | Dynamic range control |

- **Persistence:** values are kept by the driver and applied at each start (a start resets the
  codec). They aren't saved; step 2 decides which become settings.

### Commands (formats frozen once shipped)

- `audio`: one line per device: name, codec, state, rate.
- `audio status [DEV]`: the report above.
- `audio start [DEV] [RATE]`, `audio stop [DEV]`.
- `audio tone [DEV] FREQ [SECONDS] [left|right|both] [LEVEL-dBFS]`:
  - starts the device if it is stopped, and stops it afterwards if the tone started it;
  - waits until the tone has played (to the end of its fade), so `audio tone A; audio tone B`
    plays both in turn. Ctrl-C fades it out at once (the user's report, 2026-10-08: it returned
    at once, so the second tone replaced the first);
  - defaults: 2 s, both slots, −12 dBFS.
- `audio ctl [DEV]`: lists the controls and their values.
- `audio ctl [DEV] NAME VALUE`: sets one.
- `audio regs [DEV]`: every codec register, marked where it differs from what was written.
- `audio reg [DEV] REG [VALUE]`: reads or writes one register raw. Writes go into the shadow, so
  `regs` doesn't flag them.

### As built (2026-10-08)

Checked on the board:
- **Chip:** chip ID 0x8311, version 0x01. Every register read back as written, through starts,
  stops and control changes.
- **Reset values:** match the guide's defaults (0x00 = 0x1F, 0x0D = 0xFC, 0x0E = 0x6A, …).
- **Clocks:** the APLL runs at 2 × MCLK.

| Rate | MCLK set | Measured over 2 to 27 s |
|---|---|---|
| 48 kHz | 12,287,998 Hz (APLL 24,575,996) | 47,999.994 Hz (−0.1 ppm) |
| 96 kHz | 24,575,996 Hz (APLL 49,151,992) | (not timed) |
| 8 kHz | 2,047,999 Hz (APLL 6,143,998) | 7,999.996 Hz (−0.5 ppm, 2 s) |

  The APLL's coefficients put MCLK 0.16 ppm low, and the frame count agrees.
- **Engine:** no late periods in 5,441, a tone included.
  - A period of silence costs 1 to 2 µs.
  - A period of tone (240 frames, `sinf` per frame) costs 144 µs, at most 346 µs: about 3% of
    a core. The step-2 mixer replaces it.
- **Tones:** a tone started on a stopped device stops it afterwards. A tone at or above half the
  rate is refused.
- **By ear** (the user): tones, slots, `dac-slot`, volume and the other rates all work.
- **Clicks:**
  - **Stopping:** stopping the codec clicked loudly. The user stepped through the stop sequence
    by hand and found the click on powering the analog side down (0x0D = 0xFC). VMID collapses,
    and the jack's coupling capacitors pass the step on.
    - A stop now leaves the analog side up (the chip's supply is on all the time anyway). Its
      other steps (mute, DAC off, digital reset, clocks gated) are silent.
  - **Tone endings:** these still clicked faintly. The tone fades over 5 ms, but the stop came as
    soon as the fade was written, two buffers before it played.
    - A faded tone now ends, and the device stops, only once the DMA has sent the buffer that
      holds its fade.
    - A stop during a tone waits for the fade and the queued buffers (25 ms at 48 kHz, 145 ms at
      8 kHz).

## 17. Step 2: the engine (design, 2026-10-08)

Step 1's task filled buffers from one tone. Step 2 puts the engine of §5 in its place: streams
mixed in float, converted and resampled, a limiter, device volume, system sounds and the bell,
settings and the tray. No system calls yet: the kernel's own streams (tones, sounds, a test
sweep) are the clients, and step 3 puts `/dev/dsp` on the same stream class.

### Portable parts (host-tested)

- **`SampleFormat`** (its order becomes ABI in step 3): `u8`, `s8`, `s16le`, `s16be`, `s24le` (24
  bits in 32), `s24-3le`, `s32le`, `f32le`, `mu-law`, `a-law`.
  - **To float:** any format, mono or stereo, into interleaved stereo float (mono to both
    channels). µ-law and A-law through 256-entry tables made at compile time.
  - **From float:** the reverse, for the tests' round trips (and capture later). Integer formats
    round-trip exactly.
- **`Resampler`:** bandlimited interpolation (J. O. Smith's): one Kaiser-windowed sinc table,
  made at compile time into flash (`constexpr`), sampled finely enough that interpolating
  linearly between its points costs less than the stopband allows.
  - Any pair of rates. The position is kept exactly, as a whole input frame and a fraction in
    units of 1/out, so nothing drifts over hours. (AES67 later steers a ratio this way too.)
  - Downsampling stretches the kernel so its cutoff follows the output's Nyquist frequency.
  - A stream at the device's rate skips it.
  - Its input history is the stream's (allocated at open). The engine asks how many input frames
    the next period needs, converts that many in, and the resampler adds its output to the mix
    with the stream's gain.
  - Taps and table density are chosen by measurement: host tests measure passband, imaging and
    aliasing of sines through 8, 11.025, 22.05, 44.1 and 96 kHz to 48 kHz and back, by a
    least-squares fit of the sine and the residual's power (THD+N).
- **`AudioLimiter`:** transparent below full scale. A frame whose peak passes full scale sets the
  gain to bring it to full scale at once (no overshoot); the gain then recovers over ~50 ms.
  Counts the frames it touched, for `audio status`.
- **Output:** float stereo to the DMA's 32-bit slots, clamped. **A mono codec gets (L + R) / 2 in
  both slots**, so a stereo source isn't half lost on the ES8311 (`dac-slot` still picks the slot,
  but both now carry the same mix).
- **`SineTone`:** float output by a rotating phasor (renormalized each period) instead of `sinf`
  per frame.
- **`SystemSound`:** the sounds as a table of notes (frequency, length, level) with 2 ms attack
  and a fade at the end. Numbered, and the numbers become ABI in step 3: 0 bell, 1 click, 2
  error, 3 notify.
- **Volume:** in dB (the user's, 2026-10-08: a first cut in percent, amplitude = (p/100)³, gave
  way to dB as the tray shows it).

### Streams (kernel)

- **`AudioStream`:** what the engine mixes. Two kinds:
  - **PCM:** a lock-free ring (`ByteRing`, PSRAM) in the stream's format, rate and channels; the
    engine converts and resamples from it. A writer blocks while the ring is full and is woken
    by the engine. The ring must reach its start threshold before it plays; an empty ring once
    playing is an underrun: counted, silence mixed.
  - **Generators:** tones and system sounds, which make float at the device's rate in the engine.
- **Attaching:** a device has `MaxStreams` (8) slots. Attaching takes the device's lock, and
  starts the device if it is stopped. The engine reads the slots without a lock.
  - **Detaching:** marks the stream; the engine takes it out at its next period and releases its
    semaphore. A stopped device takes it out at once.
  - **Generators that end:** they fade themselves out. The engine takes them out once the DMA has
    sent the buffer holding their fade, as step 1's tone did, and their waiters are told.
- **Gain:** per stream, applied in the mix.

### The engine

Each period, on the device's task:
1. clear the float mix (internal RAM, `MaxPeriodFrames` × 2);
2. each attached stream adds its frames (converted, resampled, with its gain);
3. the limiter;
4. to the DMA buffer's slots (mono-mixed for a mono codec); write the cache back.

- **Idle:** with no streams for **3 s**, the device stops (silent since step 1). The first stream
  starts it again. `audio start` holds it running until `audio stop`.
- **Control work on the task:** settings changes and the idle stop happen on the device's task
  between periods. It never waits for the device's lock: it tries it, and tries again a period
  later. A volume change is one I²C write (~0.3 ms) inside the period's slack of about two
  periods. Whether that stays true at 1 ms periods is for step 5.

### Device volume and mute

- **The codec's DAC volume** with its soft ramp (`AudioCodec::setVolume()`), so a change costs no
  CPU and doesn't click.
- **`dac-ramp`** now defaults to **3**: 0.25 dB per 16 frames, or 60 dB in 80 ms at 48 kHz.
- **Mute** sets the DAC volume to its floor (−95.5 dB), which the ramp fades to. `dac-mute` (the
  instant mute bit) stays a control of its own.
- `audio ctl dac-volume` and the setting write the same register: the last one wins, until the
  next start applies the setting again.

### Settings (sysconf, frozen once shipped)

The table in §6.4. Decided here:
- `audio/dsp0/volume`: int dB, −60 to 0, default **−9** (the user: "fine; it gets saved anyway").
  Mute is `audio/dsp0/mute`, which the slider's bottom notch sets.
- `audio/dsp0/rate`: 8000–96000, checked against the clock plan.
- `audio/dsp0/period`: 48–480 frames. 480 is the most the I²S driver takes in one DMA buffer of
  4,092 bytes; 48 is 1 ms at 48 kHz. Below that waits for step 5.
- `audio/default`: `dsp0`.
- `audio/bell`: `sound` or `off`.
- `audio/feedback`: whether the click, error and notify sounds play.

Applied: the device watches `audio/` (`SysConf::Watcher`). Volume and mute apply at once. A new
rate or period restarts a running device, a short gap; a stopped one uses it at its next start.

### System sounds and the bell

- **`AudioDevice::playSound(n)`:** one generator per sound, so a sound already playing isn't
  started again: a `cat` of a binary rings once, not hundreds of times.
- **`SystemSounds::play(n)`:** on the `audio/default` device, if `audio/bell` (the bell) or
  `audio/feedback` (the others) allows it. The call becomes a system call in step 3.
- **The bell:** `TerminalScreen` counts BELs (as it counts title changes). The terminal app rings
  when the count moves. The serial console passes BEL on to the host's terminal, as before.

### The tray control

- A panel item **`volume`**, added to `panel/items`' default, using only what any program can:
  the settings.
- Revised with the user (2026-10-08), built as toolkit features (`gui.md` §4s):
  - **Icon:** a speaker; muted, a red circle and slash over it.
  - **Tooltip:** the volume in dBFS.
  - **Click:** a slider from 0 dBFS down to −60 dBFS, its bottom notch "Mute", and a Mute
    button; unmuting returns to the level.
  - **Wheel:** 1 dB a notch.
- It follows `audio/dsp0/...` (watched), so `sysconf set` from a shell moves it too, and it saves
  them 2 s after the last change.

### Commands (appended)

- **`audio sound [DEV] NAME`:** plays a system sound (`bell`, `click`, `error`, `notify`),
  whatever the settings say.
- **`audio sweep [DEV] [RATE] [FORMAT] [SECONDS]`:** a logarithmic sweep, 20 Hz to 0.45 × RATE,
  at −12 dBFS, written by the command into a PCM stream of that rate and format. It checks
  conversion and resampling by ear: a resampler that aliases plays tones falling as the sweep
  rises. It also tests the blocking writer that step 3's `/dev/dsp` uses.
- **`audio tone`** is a generator stream now:
  - tones started together mix;
  - an idle device stops 3 s after the last tone, not at once.
- **`audio status`** adds:
  - the load: the mix's average time a period, as a share of a core;
  - volume and mute;
  - limiter frames;
  - one line per stream (kind, format, rate, channels, fill, underruns).

### As built (2026-10-08)

Measured on the board, and by the host tests (`tests/host/audio.cpp`):
- **Resampler** (16 zero crossings a side, Kaiser β = 8, cutoff 0.90 of the lower Nyquist
  frequency, 256 table entries a crossing; host, a sine at −6 dBFS):
  - **Passband:** flat to 0.75 of the lower Nyquist frequency (−0.006 dB), −1.8 dB at 0.85.
  - **THD+N:** −83 to −99 dB from 8, 11.025, 16, 22.05, 32 and 44.1 kHz to 48 kHz.
  - **Aliasing:** going down, −83 dB or better from 1.08 × Nyquist up. Just above Nyquist it is
    only −40 dB (24.5 kHz from 96 kHz aliases to 23.5 kHz); at an 8 kHz device rate that is
    audible (4.1 kHz → 3.9 kHz).
- **Engine load** (average mixing time a 5 ms period, one sweep stream, `audio status`'s `Load`):

| Stream | Interpolated taps (first cut) | Precomputed rows |
|---|---|---|
| A tone, or 48 kHz PCM (no resampling) | 72 to 80 µs (1.5 %) | the same |
| 44.1 kHz PCM | 389 µs (7.7 %) | 280 µs (5.6 %) |
| 22.05 kHz PCM | 376 µs (7.5 %) | 270 µs (5.4 %) |
| 8 kHz PCM | 374 µs (7.4 %) | 258 µs (5.1 %) |
| 11.025 kHz PCM (640 phases: still interpolated) | 376 µs | 373 µs (7.4 %) |
| 96 kHz PCM (going down: twice the taps) | 1,310 µs (26 %) | 452 µs (9.0 %) |

  - **Precomputed rows** (the user's go-ahead, 2026-10-08): a rational ratio has out / gcd(in,
    out) phases. Each one's coefficients are worked out at `configure()` when they fit in 64 KB
    (`Resampler::MaxRowFloats`, the stream's memory space): 160 rows for 44.1 kHz, 3 for 8, 16
    and 32 kHz, 1 for 96 kHz. An output then reads one row, with no interpolation per tap.
  - The inner loop is ~10 instructions a tap, with fused multiply-adds. Moving the table or the
    history to internal RAM changed nothing (measured). PIE has no float lanes; a fixed-point
    PIE path is what is left, and isn't planned.
- **Lateness:** no late periods with tones, sounds, sweeps at every rate and format, and settings
  changing while playing.
  - A restart by the engine's own task (a new rate or period) leaves a few periods unfilled
    while the codec starts. They play silence, and aren't counted.
- **Flash writes stalled the engine,** until flash auto-suspend (the user's choice,
  2026-10-08):
  - **Before:** 256 KB written to `/boot` during a sweep gave 268 late periods (1.3 s of
    silence); one fill stalled 48 ms. With the cache off while the flash is written, the
    interrupt (IRAM) ran, but the task (flash code, PSRAM rings) didn't.
  - **Now:** `CONFIG_SPI_FLASH_AUTO_SUSPEND`, so the caches stay on and an erase or a write
    suspends when the cache needs the flash. The same write: 0 late periods, 0 underruns.
  - **Integrity:** 512 KB of random data, copied to `/boot` twice during a sweep, read back
    identical after a reboot.
  - **The chip:** IDF allows suspend for Winbond's 0xEF4017 only. The board's W25Q128JV
    (0xEF4018, read with esptool) has the same suspend and resume commands and SUS bit, so
    the board supplies IDF's Winbond driver with that check widened
    (`components/bmds_board/boards/olimex_esp32p4_pc/flashchips.c`, through
    `CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST`). The boot log says "detected chip: winbond",
    "Flash suspend feature is enabled".
  - **Caveat:** IDF's documentation lists neither the P4 nor this chip for the feature (its code
    supports the P4). Recheck `flashchips.c` against IDF's Winbond driver when IDF is updated.
- **Settings:** volume, mute, rate and period apply while playing.
  - 70 % → 0xAC (−9.5 dB), 50 % → 0x9A, mute → 0x00, each read back (before the setting was dB).
  - A new rate or period restarts the device, and streams follow at the new rate.
  - 12,345 Hz is a valid rate: the ES8311 takes any rate at MCLK = 256 × rate.
- **Idle:** the device stops 3 s after its last stream.
- **Sounds:** a bell asked for three times at once plays once.
- **Size:**
  - `bmds_audio`: 2,740 bytes of BSS (the device, its tone and sound streams, the watcher),
    102 bytes of IRAM (the interrupt), 43 KB of flash (20 KB of rodata, 16.4 KB of it the sinc
    table).
  - At the first start: the mix and scratch buffers, 7,680 bytes of internal RAM, and the
    task's 4 KB stack.
- **Icons:** Flat Color has one speaker and no muted one. `icons` aliases the four
  `audio-volume-*` names to it; `icons-glyphs` has `volume-1/2/3` and `volume-mute`. The tray
  item asks for `audio-volume-high`, else `speaker`, and draws its own red circle and slash when
  muted.
- **The tray, on the board** (driven with `win pointer`, seen in screenshots):
  - **Tooltip:** "Volume: -9 dBFS" after a rest.
  - **Click:** the slider opened under the item, the item sunk.
  - **Wheel** over the popup: three notches took −9 to −6.
  - **Drag** to the bottom: muted, the red circle and slash drawn, the level kept.
  - **Closing:** a press outside closed the popup; the middle button unmuted.
  - **Mute button:** muted with the level kept (−13), pressed in; again, back to −13.
  - **Saving:** written to `/boot` 2 s after the last change.

## 18. Step 3: the ABI (design, 2026-10-08)

Programs get the engine: an audio fd per open of `/dev/dsp*`, native calls on it, OSS's ioctls
translated in the SDK, a system sound call, `play` for WAV files and an SDK example. Playback
only; capture is step 8.

### Device files

- **`/dev/dsp0`** for the first device (`/dev/dsp1`… as more appear: no singletons), and
  **`/dev/dsp`**, which opens the device `audio/default` names at the moment of the open.
- **`/dev/audio`:** the same, but opening at OSS's 8 kHz µ-law mono.
- **`/dev/mixer0`, `/dev/mixer`:** for OSS's mixer ioctls only. Reading or writing one fails.
- **Opening for writing** plays. Opening for reading (capture) fails with `ENOTSUP` until step 8.
- Through `Vfs::addDeviceFile()`, each device adding its own at init. Its limit (4, two taken by
  the clipboard) rises to 12. Like `/dev/clip0`, they list as files (`-rw-`).

### The stream behind an fd

- **Each open is a `PcmStream`,** mixed with every other open and the system's sounds. A device
  takes up to 6; one more fails with `EBUSY`. Its slots grow from 8 to 16, for its 4 tones, 4
  sounds, 6 programs and the `audio` command's sweep.
- **Opening** sets OSS's defaults: 8 kHz, `u8`, mono (OSS programs rely on them). Native programs
  configure before writing.
- **The ring** is allocated in PSRAM at the open (OSS's default is 1 KB) and again at each
  configure; the stream is attached to the device at the first write.
- **Writing blocks** until all of it is in the ring, and is interrupted by a signal.
  - **O_NONBLOCK:** the SDK's gate already polls first. For an audio fd it also cuts the write
    to the room in the ring, so a non-blocking write takes what fits, as OSS's does.
  - **Configuring after writing** plays what is queued first, so no audio is lost.
- **`poll()`:** writable when `wake_frames` fit (default: one device period at the stream's
  rate).
- **Closing** (the user's choice, 2026-10-08, as Linux's OSS): the last close waits until what
  is queued has played (at most the ring's length and a second), then frees the stream. A
  program that exits with the fd open plays it out the same way (its exit closes it, on its own
  thread). A signal ends the wait and drops the rest, so a killed program stops at once; so does
  a paused stream.

### Native calls (appended slots 67–70; three calls and the sound, the user's choice)

| Slot | Call |
|---|---|
| 67 | `int32_t audio_config(int32_t fd, const bmds_audio_config_t *set, bmds_audio_config_t *got)` |
| 68 | `int32_t audio_status(int32_t fd, bmds_audio_status_t *status)` |
| 69 | `int32_t audio_control(int32_t fd, uint32_t op, int32_t value)` |
| 70 | `int32_t sound_play(uint32_t sound)` |

- **`audio_config`:** sets (if `set` isn't NULL), then reads back what was chosen (if `got`
  isn't NULL). `bmds_audio_config_t`:
  - `size`;
  - `format`: `BMDS_AUDIO_U8` = 0, `S8`, `S16LE`, `S16BE`, `S24LE`, `S24_3LE`, `S32LE`, `F32LE`,
    `MULAW`, `ALAW` = 9 (`SampleFormat`'s order, now frozen, checked in both places);
  - `channels`: 1 or 2 (more later, downmixed);
  - `rate`: 4,000 to 192,000 Hz, any integer;
  - `ring_frames`: 0 for the default (100 ms); rounded up (the ring is a power of two in bytes);
  - `start_frames`: plays once this many are queued (0: half the ring);
  - `wake_frames`: `poll()`'s writable mark (0: one device period);
  - `flags`: none yet (unknown bits fail).
- **`audio_status`:** everything a player or an OSS ioctl asks, in one copy.
  `bmds_audio_status_t`:
  - `size`, `state` (`IDLE`, `WAITING` for the threshold, `PLAYING`, `PAUSED`, `DRAINING`,
    `MIXER` for a mixer fd);
  - the device: `device` (its number), `device_rate`, `period_frames`, `formats` (a bit per
    format), `min_rate`, `max_rate`, `max_channels`;
  - the stream: `written_frames`, `played_frames` (64-bit), `timestamp_ns` (monotonic, when
    `played_frames` was counted), `queued_frames`, `delay_frames` (queued, plus the DMA buffers
    and the resampler's delay, at the stream's rate: for A/V sync), `ring_frames`, `underruns`,
    `gain_cdb`;
  - the device's counters: `late_periods`, `limited_frames`.
- **`audio_control`** ops: `START` (play before the threshold), `PAUSE`, `RESUME`, `DRAIN`
  (blocks until played; `EINTR` leaves it playing), `DROP`, `GAIN` (`value` in centi-dB, −9600
  to 0; −9600 is silent).
- **`sound_play`:** `BMDS_SOUND_BELL` 0, `CLICK` 1, `ERROR` 2, `NOTIFY` 3 (§17's numbers), on the
  default device, subject to `audio/bell` and `audio/feedback` as the kernel's own calls are.
- **On another fd** the audio calls fail with `ENOTTY`, as ioctls do.
- **Device volume** isn't a call: it is the `audio/dspN/volume` and `mute` settings, which
  programs already have (`bmds_sysconf_*`).

### OSS in the SDK

- **`sdk/crt/oss.c`:** `linux.c`'s `ioctl()` passes the `SNDCTL_DSP_*` and `SOUND_MIXER_*`
  requests to it. It uses only the calls above and sysconf; the kernel knows nothing of OSS.
- **`<sys/soundcard.h>`:** the SDK supplies it (musl has none), written for BMDS with Linux's
  numbers, which Linux froze.
- **Formats:** Linux's set: `AFMT_MU_LAW`, `A_LAW`, `U8`, `S16_LE`, `S16_BE`, `S8`. OSS 4's
  24-bit, 32-bit and float numbers aren't in Linux's header; they wait until checked against
  OSS 4's.

| Request | Does |
|---|---|
| `SPEED`, `SETFMT`, `CHANNELS`, `STEREO` | `audio_config`, returning what was chosen (`AFMT_QUERY` reads) |
| `SETFRAGMENT` (`0xMMMMSSSS`) | ring = M × 2^S bytes, wake = 2^S bytes |
| `GETBLKSIZE` | the wake mark in bytes |
| `GETOSPACE` | fragments free, fragments in all, fragment size, bytes free |
| `GETODELAY`, `GETOPTR` | `delay_frames`; `played_frames` in bytes |
| `SYNC`, `RESET`, `POST` | `DRAIN`, `DROP`, `START` |
| `GETTRIGGER`, `SETTRIGGER` | `PCM_ENABLE_OUTPUT` is `RESUME` or `PAUSE` |
| `NONBLOCK` | the fd's O_NONBLOCK |
| `GETFMTS`, `GETCAPS` | the formats above; `DSP_CAP_TRIGGER` |
| `SOUND_MIXER_READ/WRITE_VOLUME` (and `PCM`) | `audio/dspN/volume` and `mute` |
| `SOUND_MIXER_READ_DEVMASK`, `STEREODEVS`, `RECMASK`, `CAPS`, `SOUND_MIXER_INFO` | `VOLUME` and `PCM`, no recording |

- **Mixer percent:** OSS's 0–100 per side, linear in dB: 100 is 0 dB, 1 is −59.4 dB, 0 is mute.
  A write sets both sides to the louder one (the device has one volume). It changes the setting
  for this boot, not the saved one, as `sysconf set` would.
- **Anything else** fails with `EINVAL`.

### WAV and `play`

- **`WavReader`** (portable, host-tested):
  - RIFF/WAVE; chunks walked with their odd-length padding, unknown ones skipped;
  - `fmt `: PCM (1) at 8, 16, 24 and 32 bits, IEEE float (3) at 32, A-law (6), µ-law (7), and
    `WAVE_FORMAT_EXTENSIBLE` naming one of those;
  - 1 or 2 channels (more later, downmixed);
  - `data`: a size of 0 or 0xFFFFFFFF (streamed files) means to the end;
  - malformed input is an error, never a crash: fuzzed under ASan/UBSan.
- **`play [-d DEVICE] FILE...`:** a built-in command (the user's choice), so it works without a card:
  - prints `FILE: 44100 Hz, s16le, stereo, 3:21.4` per file, then plays it;
  - goes through `/dev/dsp` and the same calls the system calls make;
  - Ctrl+C drops what is queued and stops; exit status 0, or 1 if a file couldn't be played.
  - Step 4 adds MP3, Ogg and FLAC to the same command.

### The SDK example

- **`tone [-n] [FREQ] [SECONDS] [RATE]`:** a sine, through OSS (`open("/dev/dsp")`, `SETFMT`,
  `CHANNELS`, `SPEED`, `write`), or with `-n` through the native calls.
- **`progtest`** gains the board tests: configure and read back, status, every control, `ENOTTY`
  on a file, `EBUSY` past 6 streams, non-blocking writes.

### In the kernel

- **`AudioFile`** (`bmds_audio`): the `Stream` behind an fd, holding one of its device's pooled
  `PcmStream`s.
- **`Stream::audioFile()`**, null by default, finds it from a handle (as `isTerminal()` finds
  consoles: no RTTI).
- **`Process::audioConfig()`, `audioStatus()`, `audioControl()`, `soundPlay()`**; `bmds_loader`
  requires `bmds_audio`, and `bmds_audio` requires `bmds_fs`.
- **`PcmStream` gains:** pause and resume, start now, a written count, the wake mark, and a
  hand-back to the pool when it is over after its fd closed.

### Tests

- **Host:**
  - the WAV reader (every format, odd chunks, streamed sizes, truncation, fuzzing);
  - `oss.c` against a fake system call table;
  - the ABI lock: slots 67–70, both structs' sizes and offsets, every constant.
- **Board:** `progtest`; `tone` both ways; `play` with WAVs at each rate and format; six streams
  at once; a close playing out; a program killed mid-play; counters at zero throughout.
- **By ear:** WAVs the user knows; no pops at start, pause, drop or close.

### ABI

- Slots 67–70; `bmds_audio_config_t`, `bmds_audio_status_t` and their constants in `sys.h`.
- `<sys/soundcard.h>` (Linux's numbers).
- Device files: `/dev/dsp`, `/dev/dspN`, `/dev/audio`, `/dev/mixer`, `/dev/mixerN`.
- The `play` command's format and its output line.

### As built (2026-10-08)

- **Where it lives:**
  - `AudioFile` (`bmds_audio`, allocated in PSRAM per open) is the `Stream` behind each fd.
  - The four slots are in `bmds_loader/src/audiocalls.cpp`, which turns the ABI's structs into
    `AudioFile`'s calls and static-asserts that the ABI's formats, states and sounds are the
    kernel's numbers.
  - `Process::handle()` gives an fd's handle to such calls.
- **Waiting without the lock:** a write takes the file's lock only to push what fits, and waits
  for room outside it, so `audio_status` and `DROP` go on meanwhile (`DROP` wakes the writer).
  A drain doesn't hold it either.
- **The OSS fix found on the board:** `oss.c` set the format, channels and rate by reading the
  configuration back and changing one field, so the 1 KB ring of the open's 8 kHz `u8` stuck
  (six tones each had 1,024 frames). Now it asks for the kernel's sizes afresh each time, or
  for `SETFRAGMENT`'s, which it keeps per fd in bytes and sizes again in the new format's
  frames.
- **Ctrl+C in `play`, found by the user:** a write interrupted after taking part of its data
  returned the count without the error, so `play` went on to the end (the first test had hit a
  drain). The stream's writes now return the count with `Interrupted`, as `Stream` says; a
  program's `write()` still gets the count.
- **Non-blocking:** the SDK's write gate asks an `O_NONBLOCK` fd for its room
  (`audio_status`), which fails at once on any fd that isn't audio.
- **Board checks:**
  - **`progtest audio`** passes. It covers:
    - the open's defaults;
    - configure and read back (ring 4,800 → 8,192 frames, start 4,096);
    - every bad argument (`EINVAL`), `ENOTTY` and `EBADF`;
    - the waiting, playing, paused, draining and idle states, with counts and timestamps;
    - non-blocking writes (8,192 frames taken, then `EAGAIN`), with `poll` agreeing;
    - `EBUSY` on the seventh open;
    - OSS's format, channels, rate, fragments, space, delay and sync, and the mixer's reads;
    - a close waiting 109 ms for a 93 ms ring.
  - **`progtest audioexit`:** 500 ms queued, then exit: it took 0.60 s.
  - **`tone`:** `-n 440 1 44100` played 44,100 frames, 0 underruns, 0 late; OSS at 22,050 Hz
    the same.
  - **`play`:**
    - WAVs of `s16le` 44.1 kHz, `u8` 8 kHz mono, `s24-3le` 48 kHz, `f32le` extensible
      96 kHz and µ-law 8 kHz played one after another, 0 late.
    - A text file is "not a WAV file"; a missing file, a missing device and no arguments each
      give their message; exit status 1.
    - Ctrl+C stops at once (130), no stream left.
  - **A killed program:** `tone -n 440 5`, stopped after 1 s, stopped sounding then; no stream
    left.
  - **Six programs at once:** tones at 44.1, 22.05, 48, 32, 11.025 and 48 kHz:
    - 25.5 % of a core on average while mixing; the longest fill 3.5 ms of a 5 ms period;
    - 0 underruns, 0 late; the limiter took 510 frames of their sum.
- **Host:**
  - the WAV reader: every format plain and extensible, streamed sizes, what it refuses, 20,000
    mangled headers;
  - plus 2,000,000 more under ASan and UBSan, built on their own for the reader, with nothing
    reported;
  - `oss.c` against a fake system call table (6 cases, 97 checks);
  - the ABI lock: slots 67–70, both structs' sizes and offsets, every constant.
- **Size:**
  - `bmds_audio` 50 KB of flash (7 KB more: the file, `play`, the WAV reader) and 2,772 bytes of
    BSS (32 more: the 8 more slots);
  - `/dev`'s table 128 bytes more;
  - per open: the `AudioFile` and its ring in PSRAM.

## 19. Step 4: decoders (design, 2026-10-08)

MP3, Ogg Vorbis and FLAC in the firmware behind one format-blind interface (§7). A program
gets them through the SDK, `play` plays them, and the file browser's Open starts `play`.

### Measured (2026-10-08)

- **How:** each candidate compiled for the P4 (`riscv32-esp-elf-gcc` 15.2, the firmware's
  `-march`) and linked into an SDK program that decodes a whole file from memory on the board.
  - The input: the first 30 s of a 44.1 kHz stereo WAV, encoded by ffmpeg as MP3 at 192 kb/s
    (LAME), Ogg Vorbis at `-q 5`, and FLAC at its default level.
  - Code is what linking added over the same program without a decoder: `-Os`, with the libm
    functions each one pulls in.

  | Library (licence) | Version | Code | Memory per decoder | Stack | 30 s decoded in (`-O2` / `-Os`) | Core (`-O2`) |
  | --- | --- | --- | --- | --- | --- | --- |
  | minimp3 (CC0) | master, fetched 2026-10-08 | 18.4 KB | 6,668 B + 16.2 KB scratch | 17,328 B | 1.585 / 1.919 s | 5.3 % |
  | stb_vorbis (PD or MIT) | 1.22 | 50.6 KB | 195,552 B (this file) | 2,560 B + temp | 1.958 / 2.260 s | 6.5 % |
  | dr_flac (PD or MIT-0) | master (0.13.4), fetched 2026-10-08 | 40.9 KB | 41,264 B (peak) | 4,592 B | 1.140 / 1.339 s | 3.8 % |

- **Notes:**
  - dr_mp3 is minimp3 reworked, at 26.7 KB of object code against minimp3's 18.0 KB; nothing
    gained.
  - dr_flac without Ogg-wrapped FLAC is 14 KB smaller (object code); Ogg FLAC is rare.
  - Vorbis's memory is mostly its codebooks, so it varies with the file; `stb_vorbis_get_info()`
    tells it after the headers.
  - stb_vorbis has 1 KB of BSS (a CRC table built at the first open).
  - **The stack decides one thing:** minimp3 keeps a 16.2 KB scratch area on the stack of
    `mp3dec_decode_frame()`; a program's default stack is 32 KB. Moving it into PSRAM (static, in
    a test build) took 1.578 s against 1.584 s, so nothing is lost by keeping it in the decoder's
    allocation instead.
- **Flash:** the firmware image is 1.08 MB of a 4 MB partition. The three are 110 KB at `-Os`
  linked into a program; built at `-O2` in the firmware, they are measured again there.

### The choice

- **minimp3, stb_vorbis and dr_flac,** each vendored in `thirdparty/` with a `README.md` as
  FatFs's (where from, version, sha256, the BMDS patches in order):
  - **minimp3:** one patch: the scratch area comes from the decoder's own allocation, not the
    stack.
  - **stb_vorbis:** `STB_VORBIS_NO_STDIO`, `STB_VORBIS_NO_PUSHDATA_API`. A patch for reading
    through a callback in place of `FILE *`, which is all its pull API knows besides memory. Its
    memory comes from one PSRAM buffer per decoder (its own `stb_vorbis_alloc`), so it calls
    neither `malloc()` nor `alloca()`: no patch for those (as built: the buffer starts at
    128 KB and doubles until the file's setup fits, to 4 MB).
  - **dr_flac:** `DR_FLAC_NO_STDIO`, `DR_FLAC_NO_OGG`, `DR_FLAC_NO_SIMD`; its read, seek and
    allocation callbacks already fit. CRCs are checked: a damaged frame is passed over, not
    played as noise.
  - Patches for what the tests and the fuzzer found are listed in the libraries' `README.md`
    (as built, below).
- **Built at `-O2`** (as the PNG decoder is), in PSRAM-backed memory, with `-Wshadow` and the
  warnings the libraries trip off for their files only, as littlefs's.
- **Not now:**
  - Opus (libopus is far larger; measured when wanted);
  - AAC and M4A (not looked at yet);
  - Ogg FLAC.

### The kernel: `Decoder`

- **One interface** (`bmds_audio`, portable and host-tested):
  - `open(source)`: reads the headers and fills `Info`; fails with `Invalid` (not this
    format) or `NotSupported` (this format, in a form not played);
  - `info()`: the codec, rate, channels, the length in frames (or unknown), the bitrate, the
    source's sample size;
  - `tag(index)`: a key and a value, UTF-8;
  - `decode(frames)`: interleaved samples in the format asked;
  - `seek(frame)`: to an exact frame.
- **A source** is a read-at callback with a size, as `WavFile::parse` takes, so the decoders
  never see a `Stream` and the host tests feed them memory.
- **Four implementations:**
  - `WavDecoder` on `WavFile` (it converts nothing: its format is the file's);
  - `Mp3Decoder` on minimp3;
  - `VorbisDecoder` on stb_vorbis;
  - `FlacDecoder` on dr_flac.
- **Found by content:** the first bytes, as `file_type()` reads them (`RIFF`/`WAVE`, `OggS`
  with a Vorbis header, `fLaC`, `ID3`), or two MPEG audio headers in a row. Ogg holding Opus or
  FLAC is `NotSupported`.
- **MP3 specifics:**
  - ID3v2 skipped and read for tags (text frames in all four encodings to UTF-8); ID3v1 at the
    end;
  - the length from the Xing or Info frame, or VBRI; failing both, from the size and a constant
    bitrate, marked estimated;
  - gapless: LAME's encoder delay and padding from the Info frame trimmed;
  - an exact seek from an index of frame offsets, built as frames are passed (4 bytes a frame,
    about 150 bytes a second);
  - free-format and MPEG 2.5 as minimp3 plays them.
- **Tags:** ID3 frames as `TITLE`, `ARTIST`, `ALBUM`, `TRACKNUMBER`, `DATE` (the Vorbis
  comment names); Vorbis and FLAC comments as they are. Pictures are skipped.

### Programs: a decoder is an fd (slots 71–73, appended)

- **`audio_decode(int32_t fd, const bmds_decode_config_t *config)`** returns a new fd that reads
  as PCM:
  - **The source:** `fd`, a readable, seekable file (it is duplicated, so the caller may close
    its own); or, with `fd` −1, `config`'s `data` and `length`, a buffer the caller keeps until
    the close.
  - **The format** read: `BMDS_AUDIO_S16LE`, `BMDS_AUDIO_S32LE` or `BMDS_AUDIO_F32LE`, or
    `BMDS_DECODE_NATIVE` for the decoder's own (float for MP3 and Vorbis, the file's for WAV, 16-
    or 32-bit for FLAC by its sample size). Channels and rate are the file's: the engine
    converts.
  - **Errors:** `-BMDS_EINVAL` (not a format known), `-BMDS_ENOTSUP` (a form not played),
    `-BMDS_ENOMEM`, `-BMDS_EBADF`.
- **The new fd:**
  - `read()` decodes on the caller's thread: whole frames, as many as fit;
  - `lseek()` in bytes of that PCM (frame-aligned) seeks exactly;
  - `fstat()`'s size is the PCM's length when it is known;
  - `close()` frees everything; an exit closes it as any fd.
  So copying a decoded fd to `/dev/dsp` is the whole of a player.
- **`audio_decode_info(int32_t fd, bmds_decode_info_t *info)`:** the codec (`BMDS_CODEC_WAV`,
  `MP3`, `VORBIS`, `FLAC`), its name, the rate, channels, the format read, the length in frames
  (`UINT64_MAX` unknown) and whether it is estimated, the bitrate, the source's bits, and the
  number of tags.
- **`audio_decode_tag(int32_t fd, uint32_t index, char *key, uint32_t key_size, char *value,
  uint32_t value_size)`:** a tag, UTF-8, cut at a character's end; `-BMDS_ENOENT` past the
  last.
- **Who pays (§7):** the decoder's memory is PSRAM from the kernel, freed at the close; its time
  is the reading thread's. Nothing runs between reads.
- **A crash in a decoder** is a crash of the firmware, as a program's crash is today; fuzzing is
  what stands against it (below).

### `play`

- **Every format** through `Decoder` (WAV moves onto it): the same line for each file, the
  format field naming the codec (`mp3`, `vorbis`, `flac`, or the WAV's sample format as now).
  New after it, appended: a line of the title and artist when the file has them.
- **The format read** is the native one, so nothing is quantized twice.
- **Errors:** "not a sound file this plays" and "a damaged file" (with the frame's time) beside
  today's messages; exit statuses unchanged.

### The file browser's Open

- `boot/applications/play.desktop` (`Exec=play %f`, `Terminal=true`, `NoDisplay=true`) and
  `audio/*=play.desktop` in `boot/config/mimeapps.list`: a double click plays the file in a
  terminal, where Ctrl+C stops it.
- `file_type()` gains MP3 without ID3 (two MPEG audio headers in a row), appended to its content
  rules.

### The SDK

- `sys.h`: the three calls, the structs (`size` first), `BMDS_CODEC_*`, `BMDS_DECODE_NATIVE`.
- **`player` example:** `player FILE`, the decoded fd copied to `/dev/dsp` after
  `audio_config` from `audio_decode_info`, with the title shown, in about 60 lines.
- **`progtest decode`:** every call's errors; formats; an exact seek; a buffer source; reads
  after the source's fd is closed.

### Tests

- **Host, known answers:** short fixture files in `tests/host/audio/` (a second of tones,
  made once by ffmpeg and checked in, a few KB each): each decoded, its length, its tags, and a
  seek's first samples equal to a straight read's. Peak error against the tone within what
  each codec's quality allows.
- **Host, damage:** truncated files at every header byte, wrong lengths, bad CRCs, an Ogg with
  Opus; each refused or played without a fault.
- **Fuzzing:** a libFuzzer target for the decoders (clang, ASan and UBSan; the host has them) on
  seeds of each format in its forms, run for hours before shipping (as built: about 85 inputs a
  second on three cores, so millions, not tens of millions); the findings kept as regression
  files and fixed as patches in `thirdparty/`.
- **Board:** each format played by `play` while files are copied and windows dragged, 0 late;
  `audio status` load and the decoding thread's time recorded here; the user by ear (an MP3
  known well).

### ABI

- Slots 71–73, their structs and constants: `ABI:` lines, lock tests.
- `play`'s added line and codec names; `play.desktop` and the `audio/*` line in `/boot`'s
  defaults; `file_type()`'s new rule.

### Decisions (the user, 2026-10-08)

- **The libraries:** minimp3, stb_vorbis and dr_flac, as measured above.
- **A decoder is an fd:** `audio_decode` gives a new fd that reads as PCM; two more calls give its
  information and its tags (slots 71–73).
- **The file browser's Open** plays in a terminal (`play.desktop`); a GUI player can replace it
  later.

### As built (2026-10-08)

- **Where it lives:**
  - `Decoder` (`bmds_audio`, portable): `open()` finds the format from the first 64 bytes;
    shared code for the read cache (4 KB, so stb_vorbis's one-byte reads are cheap), the tags
    (4 KB, 64 at most, keys folded to capitals), and the conversion to 16-bit, 32-bit or float
    (256 frames at a time, through `SampleFormats`).
  - `WavDecoder`, `Mp3Decoder`, `VorbisDecoder` and `FlacDecoder` behind it, each allocated in
    PSRAM; the libraries' settings in `components/bmds_audio/codecs.cmake`, shared with the
    fuzzer.
  - `DecodedFile : Stream`, found through the new `Stream::decodedFile()`, as `audioFile()`. Its
    source is a reference to the file's handle, or a copy of the caller's buffer (a buffer the
    kernel kept a pointer to could outlive the program through an inherited fd).
  - The calls in `bmds_loader/src/audiocalls.cpp`; `Process::addHandle()` gives a new fd.
- **MP3:**
  - the first frame searched for in the first 64 KB;
  - a Xing or Info frame's count, with LAME's delay and padding (as minimp3's own example
    reads them, 529 samples of the decoder's own delay added);
  - otherwise the length estimated from the first frame's bitrate (`BMDS_DECODE_ESTIMATED`);
  - ID3v2.2 to 2.4 (text frames in all four encodings) and ID3v1/1.1 (Latin-1), and APEv2 tags
    skipped;
  - exact seeks: back two frames, then far enough for 511 bytes of bit reservoir, those frames'
    output dropped. The index keeps one offset every 16 frames (4 bytes each; frames between are
    found header to header), so it costs about 10 bytes a second of audio.
  - A frame in the other channel mode is mixed down or doubled; one at another rate is dropped.
- **Vorbis:** seeking to the very end isn't asked of stb_vorbis (no frame holds it); files over
  2 GB are `NotSupported` (its offsets are 32-bit).
- **FLAC:**
  - 16-bit and narrower files decode as `s16le`, wider ones as `s32le` (the top 24 bits);
  - an ID3v2 tag before `fLaC` is passed over;
  - the comments read by `flacdecoder.cpp` itself;
  - an allocation that failed inside dr_flac is told as `NoMem`, not `Invalid`.
- **What the tests found:**
  - stb_vorbis's callback reads had to keep memory mode's exact semantics, or seeks failed
    after a stream had been read to the end;
  - stb_vorbis asserted when seeking to the stream's end (now an error, patched);
  - `setFormat()` could keep one of its two buffers after an allocation failed, and use the
    other unallocated next time;
  - `play` read the file's info after closing its decoder.
- **What the fuzzer found** (each patched, its input kept in `tests/host/audio/fuzz/`):
  - stb_vorbis freed a comment list through a null pointer after a damaged count, and could
    write before its vendor string's buffer;
  - dr_flac's seek estimate divided by 0 and made the NaN an integer.
- **Measured on the board:**
  - **`progtest decode`** passes. It covers:
    - each call's errors;
    - the information and tags of `tone.flac`;
    - reads in pieces, `fstat`, seeks into a frame's middle and to the end, and read-only;
    - a buffer copied by the call;
    - close-on-exec.
  - **`play`:**
    - the 30 s MP3, Ogg Vorbis and FLAC clips each took 30.1 to 30.2 s, with 0 late periods
      over 18,078 periods;
    - the tag line shows ID3v2.4, ID3v1 (Latin-1 as UTF-8) and Vorbis comments;
    - Ogg Opus is refused ("a sound file in a form this doesn't play"), and so is a program
      ("not a sound file this plays");
    - Ctrl+C on a WAV gives 130.
  - **`player`** (the SDK example) plays each format.
  - **minimp3's float output** takes 1.441 s for the 30 s clip (4.8 % of a core), less than
    16-bit's 1.585 s.
- **Host:**
  - `tests/host/decoder.cpp` covers:
    - each format found, with its length (MP3 gapless to the sample);
    - the tones' frequencies in their channels;
    - FLAC equal to the WAV's samples, and 24-bit;
    - the tags;
    - exact seeks: every format, nine places, against a straight decode;
    - conversions;
    - cut and damaged files;
    - every allocation failing (`NoMem`, nothing leaked);
    - the fuzzer's finds;
    - `DecodedFile` through a handle, in pieces and sought.
  - The fixtures are 138 KB, made by ffmpeg, and the fuzzer's finds 52 KB.
  - `file_type()` knows bare MPEG audio.
- **Fuzzing** (`tests/fuzz`, clang with ASan and UBSan):
  - about 85 inputs a second on three cores;
  - the seeds from `make-seeds.sh`: MP3 CBR, VBR, without a tag, MPEG-2 and 2.5, MP2; Vorbis
    stereo and 8 kHz mono; FLAC 16-, 24- and 8-bit, with small frames; WAV; Ogg Opus.
  - The run's count is below.
- **Size:**
  - `bmds_audio` 164 KB of flash (114 KB more: the decoders, at `-O2`), 3,796 bytes of BSS
    (1,024 more: stb_vorbis's CRC table);
  - the firmware image from 1,108,448 to 1,239,920 bytes.
  - Per decoder (PSRAM):
    - MP3 about 48 KB (the input buffer 16 KB, the scratch 16 KB, the decoder 6.7 KB, a frame's
      samples 9 KB) and its index;
    - Vorbis 128 KB or more (195,552 bytes for the 30 s clip, so 256 KB);
    - FLAC 41 KB for the 30 s clip;
    - each with 4 KB of cache, and 4 KB of tags when it has any.
