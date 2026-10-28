# Synth Module: the Board as a Dedicated Sound Module

Planned 2026-10-28 (the user's request and its use case).

## 1. Goal

The board as a live sound module, like a rack synth: MIDI in, sound out, no display. For general
use the system synths stay as they are (`fm` by default, the wavetable on TimGM6mb: `audio.md`
§25, §26). This is for playing: the display isn't started, and most of the RAM goes to the synth's
samples, so its instruments can be much better than a 5.7 MB General MIDI font's.

What the user set out (2026-10-28):

- **Live**, not files: notes come from a keyboard or a computer and play at once.
- **At most 16 instruments**, one a MIDI channel, chosen and loaded before playing. A program
  change needn't load anything: the player sets an unused channel up in advance, or loads every
  instrument they'll use first.
- **The next board** brings serial MIDI in and out and a better codec. USB audio would be a good
  addition.

Decided with the user (2026-10-28, §6):

- "module" and "performance" are the names; **a performance is JSON**.
- **Driven over SSH at first;** a web page for configuring it later, on a web server that
  applications in general can use (its own plan: §3h).
- **Voices are set per part,** and each part has a **voice mode** as hardware synths do (steal the
  oldest, keep the highest, a single voice with note priority...): it matters most for mono parts
  (§3d).
- **The network stays up:** SSH to drive it, RTP-MIDI as an input (built), and later the rendered
  audio sent as **AES67**. The USB-C as a MIDI device keeps a serial console on its CDC port, so
  the USB swap costs nothing that matters here.

## 2. Facts (checked 2026-10-28)

- **PSRAM:** 31.94 MB. With the desktop up after boot, 23.7-24.4 MB free. **Headless, 30.0 MB
  free, 29.5 MB of it in one block** (and internal RAM 316 KB free against 157-192 KB); a
  24.1 MB font loads with 6.5 MB left (measured, §2a).
- **Headless is already a script:** `/cfg/autoexec.sh` runs the card's `/sd0/p0/autoexec.sh` when
  there is one, and that is what runs `display start`, `win start` and `panel &`. A card whose
  `autoexec.sh` starts the module instead boots without the display. No new boot stage is needed.
- **The wavetable's cost:** 9.9 µs a voice per 64-frame block (1,333 µs) at 360 MHz (`audio.md`
  §26's benches): 0.74 % of a core a voice. One core holds about 100 voices with nothing else
  running; `SynthEngine::MaxVoices` is 256.
- **The wavetable keeps 16-bit samples** (patched TinySoundFont, `fontSamples` as `short`): a
  font's samples take their own size in PSRAM. SF3 (Vorbis samples) is decoded at load into the
  same 16-bit form. A `Synth` is a `tsf_copy()` of a `SoundFont` (the samples shared), and
  `tsf_channel_set_presetindex()` sets a channel to any preset, by its place in the font.
- **SF2's order:** the RIFF chunks are `INFO`, `sdta` (all the samples, one chunk), then `pdta`
  (presets, instruments, zones, sample headers). Which samples a preset needs is only known after
  reading past all of them, so loading part of a font has to seek: read `pdta` first, then the
  sample ranges it names. `SoundFont::Reader` reads and skips forward only today.
- **The card:** 16.3 MB/s in 64 KB reads; a small read about 0.4 ms (the card's own access time).
  All storage goes through one FatFs lock (`storage.md`).
- **MIDI in today:** USB MIDI host (built, waiting for a device to try), the board as a USB MIDI
  device on the USB-C (`audio.md` §23: the full-speed OTG controller swapped onto it, the serial
  console gone while it is), network MIDI (RTP-MIDI). Serial MIDI waits for the next board.
- **Output today:** the ES8311 is mono, and the jack's left and right are its two outputs in
  opposite polarity (`HARDWARE.md`). Good enough for building this; the next board's codec is the
  real output.
- **USB audio:** none yet. TinyUSB has a USB Audio Class 2 device driver (`class/audio`); it has no
  audio host class. The full-speed OTG controller (already a MIDI device) can carry 48 kHz stereo
  24-bit (288 bytes a millisecond, under an isochronous endpoint's 1,023).

## 2a. Measured (2026-10-28, step 1)

On the board at 360 MHz, with the `synth.module` bench (added for this: 32 to 128 notes of one
instrument started a block apart, each at its own place in its sample, a 64-frame block timed;
`cores=2` runs the same on a second synth on the other core at once), and on the host with a
script that reads an SF2's presets, instruments, zones and sample headers.

**A voice's cost hardly depends on the samples' size.** On one core, about 10 µs a voice a block
whether its samples are TimGM6mb's piano (0.18 MB) or FluidR3's (7.8 MB, read through PSRAM as it
plays): the cache's misses are hidden. Per voice, µs a 64-frame block (1,333 µs):

| | Voices a core | Desktop up | Headless |
|---|---|---|---|
| TimGM6mb piano, one core | 32-128 | 10.2-10.4 | 10.2-10.3 |
| FluidR3 piano, one core | 64-256 | 10.3-10.6 | 9.8-9.9 |
| FluidR3 piano, both cores at once | 64 | 11.8 | 10.7 |
| | 128 | 13.8 | 11.6 |
| | 192 | 15.0 | 12.5 |
| | 256 | 16.4 | 13.0 |
| TimGM6mb piano, both cores at once | 128 | 11.1 | |

- **Both cores reading PSRAM slow each other,** and the display's scan (166 MB/s of PSRAM) makes
  it much worse: with the desktop up, two cores of FluidR3 voices cost 34-55 % more a voice;
  headless, 7-30 %. TimGM6mb's small samples stay in the cache (9 %).
- **So, headless at 64-frame periods,** keeping each core under three quarters of a period (1,000
  µs): one core about 100 voices; **both, about 85 each: some 170 voices.** The design's 128 is
  comfortable (about 64 a core: 690 µs, 52 %); 192 is past what holds.
- **Stereo samples are two voices a note.** 63 % of FluidR3's sample bytes are stereo pairs, its
  piano among them: 128 voices are 64 piano notes. `voices` in a performance counts notes; the
  pool counts voices.
- **Sizes of a better font** (FluidR3 GM, 148 MB of samples, MIT): a melodic preset is 0.63 MB at
  the median, 3.4 MB at the 90th percentile, 7.8 MB at most (the pianos); TimGM6mb's median is
  0.02 MB, its piano 0.18 MB. Picks: piano 7.8, strings 5.6, choir 4.75, electric piano 3.2,
  organ 3.1, brass 2.6, guitar 1.7, alto sax 1.6, bass 1.3, flute 1.3, pads 0.2-0.6, the standard
  drum kit **11.1**. A band (piano, bass, guitar, strings, brass, organ, a pad, drums) is 33.8 MB;
  fifteen parts and drums 45.6 MB. **FluidR3's full instruments don't all fit at once: about
  24-28 MB do** (e.g. piano, strings and drums, 24.5 MB, loaded and played).
- **Every melodic FluidR3 sample is looped;** only the drums are one-shots (10.4 of the kit's
  11.1 MB). Streaming would gain nothing on melodic GM instruments, and a lot on drums: the kit
  with 0.1 s of each hit kept is 1.6 MB, with 0.25 s 2.7 MB, with 0.5 s 4.2 MB.
- **Loading** 24.1 MB from the card: 1.64 s (14.7 MB/s); 13.2 MB 0.99 s.
- **The prototype of §3b's loader** (a host script writing an SF2 of the chosen presets only, their
  samples copied and offsets rebased, the modulators dropped): FluidR3's piano, strings and drums
  as a 24.5 MB file render **bit-identical** to the full 148 MB font in FluidSynth.
- **Fragmentation:** one font is one allocation of all its samples. After a failed 24.5 MB load
  (the cache still held 19 MB of earlier fonts; it let them go and tried again), PSRAM had
  30 MB free but no block over 13 MB, and the load failed again until a reboot. A module can't
  count on one large block late in its life (§3b).

## 3. The design

### 3a. A performance: what the module plays

A **performance** (the usual name on hardware synths) is a file of up to 16 parts, a part a MIDI
channel:

- the instrument: a font file (SF2 or SF3), a bank and a preset in it;
- its voices and voice mode (§3d);
- the part's starting volume, pan, reverb and chorus sends, transpose, and the key and velocity
  range it answers (so two parts on one channel can split or layer the keyboard);
- the module's master gain and effects.

Performances live in the home (`~/synth/performances/NAME.json`, the folder a setting:
`synth/module/performances`). `synth/module/performance` names the one loaded at start. **JSON**
(the user's choice), read and written with `bmds_core`'s reader and writer (`json.md`), so a
program or a web page writes it as easily as `vi`. A sketch (names settled in step 3):

```json
{
        "name": "Stage",
        "gain": -3.0,
        "reverb": {"type": "hall", "time": 2.2},
        "parts": [
                {"channel": 1, "font": "/sd0/p0/share/soundfonts/FluidR3_GM.sf2",
                 "bank": 0, "preset": 0, "voices": 48, "mode": "poly-oldest",
                 "volume": 100, "pan": 64, "reverb": 40},
                {"channel": 2, "font": "/sd0/p0/share/soundfonts/GeneralUser.sf2",
                 "bank": 0, "preset": 38, "voices": 1, "mode": "mono-last", "legato": true,
                 "keys": [0, 59]}
        ]
}
```

Unknown keys are ignored and missing ones take defaults, so later fields (portamento, filters) don't
break older files. A file is untrusted input: fuzzed with the loader.

### 3b. Loading: only the presets the parts use

The memory win is here. A performance's presets are found in their fonts, and **only their
samples are read**, rather than the whole file:

1. read the font's `pdta` (seek past `sdta`), find the part's preset, its instruments' zones and
   the sample headers they use;
2. read those samples' ranges of `sdta` (sorted, adjacent ones merged into one read), into one
   PSRAM buffer for the whole performance;
3. rebase the sample offsets and build one TinySoundFont font in memory, its presets the parts'
   (part N is preset N), with the zones only of what was kept.

So a 141 MB FluidR3 or a 30 MB GeneralUser GS gives a part only its instrument's samples, and the
16 parts from different files make **one font**: one `Synth`, one voice pool, the existing
service's code playing it. `SoundFont::Reader` gains a seek (`SoundFont::load()` keeps reading
forward when the whole font is wanted). Untrusted input as before: the subset loader is fuzzed
alongside the whole-font one.

Loading takes as long as its samples take to read: 1.64 s for 24.1 MB (measured), plus SF3's
decoding. While it loads, the module is silent, and it says so (§3h).

**Its memory, taken once:** a large block is only sure to exist early (§2a: fragmentation). The
module takes its sample arena when `synth/module` starts it (at boot, before other users), sized
by `synth/module/memory` (default: what is free less a margin, about 26 MB headless), and keeps it
while it is on. Performances are loaded into the arena, replacing what it held, never as a new
allocation; the font cache isn't used for them. Once large allocations go through the MMU
(`psramwindow.md`), fragmentation can't fail a load and the arena can go.

### 3c. Where it runs: a third synth service, `module`

`SynthServices` is one service run twice today (`fm`, `wavetable`). The module is a third engine
behind `SynthEngine`, run by the same service as a third port, **`module`**, on while
`synth/module` is on (off by default: nothing is loaded for anyone who doesn't use it).
`synth/system` can name it, so programs and `/dev/midi` play it too. Its inputs are wired as every
port's are (`midi/connect`, the patchbay): a USB keyboard, the USB device's cable, a network
session, later the serial port.

It differs from the other two in what it keeps:

- its performance stays loaded for as long as it is on (not closed after 5 s of quiet: a module
  must answer the first note at once);
- its stream stays open, at the device's smallest period that measures clean (§3f).

### 3d. Voices per part, and voice modes

TinySoundFont has one voice pool and one limit for the whole synth (`synth.cpp` checks
`maxVoices` before a note). The module keeps its own count a part and decides, before TinySoundFont
starts a note, which of that part's voices goes. A note is a key's whole sound: SF2 can start
several voices for one key (layered zones), counted as one note.

- **`voices`:** the part's notes at once (1 to 64; the module's total, `synth/module/voices`, is
  the pool's size and the sum's ceiling).
- **Poly modes** (when a part is full):
  - `poly-oldest`: the note held longest goes (the default; most synths' choice);
  - `poly-quietest`: the note furthest into its release, or lowest in level;
  - `poly-lowest`, `poly-highest`: the lowest or highest key goes (keep the melody on top, or the
    bass);
  - `poly-none`: the new note is dropped.
  A note already released is always taken before a held one.
- **Mono modes** (`voices` 1): a stack of the keys held (up to 16), so letting go of the newest
  returns to the one still held below it:
  - `mono-last`: the newest key sounds (the usual);
  - `mono-highest`, `mono-lowest`: the highest or lowest held key sounds (lead and bass lines as
    on analogue monosynths);
  - `legato` (on or off): a key pressed while another is held changes the pitch without starting
    the sound again (no new attack). TinySoundFont starts a voice per note, so legato is the voice
    moved to the new key's pitch and zone (a patch: when the new key falls in the same zone, a
    pitch change; when it doesn't, a fast crossfade to a new voice);
  - portamento (glide time, CC5/CC65) later.
- **Same key again:** a note on a key that is still sounding takes that voice over (piano-like),
  rather than stacking a second one, unless the part says otherwise.
- **Sustain pedal** keeps notes held for the count and the modes, as the keys do.

The count and the modes are host-tested: a part's notes and the voices TinySoundFont plays,
for every mode, with the pedal, against what each should keep.

### 3e. Two cores

With no display, both cores are free. The voices are rendered on two real-time threads, one a core,
each taking half of the active voices into its own buffer; the service adds them and runs the
effects and the limiter. MIDI events are applied between blocks, so the voice list doesn't change
under the threads. The split is TinySoundFont's render loop given a voice range (a patch, as its
others are).

**Target: 128 voices** at 64-frame periods, measured headless with FluidR3's piano (§2a): 64 a
core at 10.7 µs is about 690 µs, 52 % of each core, which leaves room for the effects and the
period's other work. About 170 voices is the ceiling; stereo samples take two voices a note.

### 3f. Latency

A 64-frame period is 1.33 ms; the service plays an event a period late (`synthservice.cpp`'s
placing), so about 2.7 ms plus the output's own buffer and the input's (USB MIDI's 1 ms frames,
serial MIDI's 1 ms a three-byte message). 32-frame periods halve the synth's share, if two cores
keep up there (to measure: `audio.md` §20 runs clients down to 16).

### 3g. Effects: reverb and chorus

TinySoundFont doesn't do them: CC91 and CC93 are kept and ignored today. A module needs both:

- **reverb:** a feedback delay network (4 or 8 lines, a few hundred KB of delay in internal RAM
  or the TCM if it fits, else PSRAM), stereo out, a send a part (CC91, the performance's level);
- **chorus:** two or three modulated delay taps on a short line, a send a part (CC93);
- after the voices, on whichever core finished first; budget about 10 % of a core together.

They go in `bmds_synth` as their own classes, host-tested, so the wavetable and FM services can
use them later too (with a setting each, off by default there: the system synths' cost stays as
it is).

### 3h. Control without a screen

- **The shell, over SSH first** (the user: how it is driven at first; the USB device's CDC
  console and the serial console work too): `synth module` (status: the performance, its parts,
  memory, voices a part, late periods), `synth module load NAME`, `synth module list`,
  `synth module set PART KEY VALUE` (a part's field changed live, and `synth module save` to
  write it back to the file).
- **MIDI:** a performance chosen by program change on a control channel (`synth/module/channel`,
  off by default: 0), the performances numbered by their order in the folder; a SysEx or two later
  if a need shows.
- **What it says:** the shell's status and the log; nothing on the speakers. A sound or an LED
  when a load ends can come later if playing without a computer asks for it.
- **A web page, later** (the user): the performance edited in a browser on the LAN. It wants a
  **web server applications in general can use**, not one of the module's own: HTTP in the
  kernel or the SDK, an application serving its pages and a small JSON API under its own path,
  behind the same users and passwords as SSH. That is its own plan (`network.md`, roadmap); the
  module's page is one of its first users. `synth module`'s commands are shaped so the page can
  call the same operations (load, list, set, save).

### 3i. Output

- **Now:** dsp0 (the ES8311), mono: the parts' pans are summed. Enough to build and measure on.
- **AES67, eventually** (the user): the module's mix sent as a network stream, the board's audio
  clock following PTP (`network.md` §31, roadmap step 10). The module plays into the engine as
  every client does, so it reaches whatever device or stream the engine sends to; nothing in the
  module is specific to an output. Its inputs can be the network too (RTP-MIDI, built), so a
  module on Ethernet alone, with power, is a complete setup.
- **The next board's codec:** a new `AudioDevice`, in its own plan when the board is here.
- **USB audio** (the user, 2026-10-28: a good addition): the board as a **USB Audio Class 2
  device**, beside its MIDI device on the same OTG controller. A computer then sees one
  class-compliant synth module: MIDI in, stereo audio out, nothing to install. Asynchronous, with
  a feedback endpoint: the board's APLL stays the clock and the computer follows it, as
  `audio.md` §9's clock rules want. It is a new `AudioDevice` (`dsp1`) whose period is the USB
  frame's millisecond. It is useful for every program, not only the module, so it gets its own
  section in `audio.md` (roadmap step 10's USB audio) when it starts. An interface plugged into
  the board (USB audio *host*) is more work (TinyUSB has no audio host class) and comes later.

### 3j. Later: SFZ and streaming

- **SFZ** (text, and WAV or FLAC samples) is the format of most good free sample libraries
  (Salamander Grand Piano, VSCO, Sonatina). A loader that reads an `.sfz` into the same in-memory
  font as §3b (the opcodes TinySoundFont's zones can express, FLAC decoded at load with dr_flac)
  opens them to a part. Its own step, after the module works.
- **Streaming from the card**, only if instruments bigger than RAM are wanted (a large sampled
  piano with many velocity layers and long unlooped decays). In GM fonts only the drums gain
  (§2a: FluidR3's kit 11.1 MB in RAM, 2.7 MB streamed with 0.25 s kept), so drum kits are the
  first candidate. In this mode the card serves only the
  synth, so 100-200 ms of each sample kept in RAM can hide its latency. Bandwidth is the limit: 64
  voices at 44.1 kHz stereo read about 11 MB/s against the card's 16. UHS-I (SDR50, about 40 MB/s;
  `storage.md`'s open question) first.

## 4. Settings (sysconf, frozen once shipped)

| Key | Type | Default | What |
|---|---|---|---|
| `synth/module` | Bool | `false` | The `module` port and service. |
| `synth/module/performances` | Path | `~/synth/performances` | Where performances are. |
| `synth/module/performance` | String | (empty) | The one loaded at start (empty: none, silent). |
| `synth/module/voices` | Int | `128` | The pool: every part's voices together, at most 256. |
| `synth/module/memory` | Int | `0` | The sample arena in MB, taken at start (0: what is free less a margin). |
| `synth/module/channel` | Int | `0` | The control channel for program changes choosing a performance (0: off). |

## 5. Steps

1. **Measure** with the display off. Done (§2a): 30 MB free headless; about 10 µs a voice on
   one core whatever the samples' size, two cores 7-30 % slower each headless (34-55 % with the
   display); FluidR3's instruments' sizes; a 24.1 MB font loaded in 1.64 s; a subset loader
   prototyped on the host.
2. **Loading part of a font** (§3b): the seekable reader, the `pdta`-first loader, the merged
   font; host tests, fuzzing; `synth -p` to try a preset subset from the shell. Measured: a
   FluidR3 and a GeneralUser GS instrument's sizes and load times.
3. **The performance and the `module` service** (§3a, §3c, §3h): the JSON file, the settings,
   the third port, `synth module` over SSH; a sample card `autoexec.sh` that boots into it. The
   user plays it.
4. **Voices per part and the voice modes** (§3d), host-tested mode by mode.
5. **Two cores** (§3e): the split render, 128 voices measured; the period brought down if it
   holds (§3f).
6. **Reverb and chorus** (§3g).
7. **USB audio device** (§3i), designed in `audio.md` first.
8. **SFZ** (§3j).
9. **Streaming** (§3j), only if the user wants instruments bigger than RAM.

Elsewhere, in their own plans: the next board's serial MIDI and codec; the web server for
applications (then the module's page); AES67 (`network.md`, after PTP).

## 6. Decisions (the user, 2026-10-28)

- **Names:** "module" for the port and settings, "performance" for the setup.
- **Performances are JSON** (§3a).
- **Driven over SSH first;** a web page later on a general web server for applications (§3h).
- **Voices are set per part, with voice modes** (poly stealing rules, mono note priorities,
  legato), mono parts the case that matters most (§3d).
- **The USB-C as a device is fine:** the CDC port keeps a serial console; the network (SSH,
  RTP-MIDI, later AES67) stays up beside it.
- Left open until step 3: how a performance is chosen from MIDI (a control channel's program
  change, or SysEx), and whether a load ending makes a sound.

## 7. Budget (estimates)

- **Firmware:** the subset loader and merged font 6 KB, the service's third engine and commands
  4 KB, reverb and chorus 4 KB, USB audio 8-10 KB (TinyUSB's class and ours).
- **RAM:** nothing while `synth/module` is off. On: the sample arena (about 26 MB of PSRAM
  headless), two render threads' stacks and buffers (a few KB of internal RAM), the effects'
  delay lines (up to a few hundred KB).
- **CPU:** about half of each core at 128 voices (measured), the effects about 10 % of one.
