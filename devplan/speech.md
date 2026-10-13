# Speech: Text to Speech in the OS and the SDK

Planned 2026-10-12 (the user's request and choices).

## 1. Goal

Text to speech for scripts and programs: a `say` command that speaks through the audio system as
`play` does, or writes what it says to an audio file; and the same for programs through the SDK.
The engine is SVOX Pico (the user's choice: others have run it on the ESP32), its voices in `/rom`
read in place, not loaded.

## 2. Facts (checked 2026-10-12, on the host)

- **The source:** Android's `platform/external/svox` (`pico/lib`, 64 files, 1.48 MB of C;
  commit `c5a33e4fdc55b63d4aa33aecefcc91b63da921e7`, 2018-02-14, the last). Apache-2.0, the
  voices too (`pico/NOTICE`): fine beside MIT, with its NOTICE kept. eSpeak, the usual other
  choice, is GPL-3; Flite's small voices sound worse.
- **The voices** (a voice is two files, text analysis `_ta` and signal generation `_sg`):

  | Voice | `_ta.bin` | `_sg.bin` | Together |
  |---|---|---|---|
  | en-US | 650,668 | 777,396 (lh0) | 1.43 MB |
  | en-GB | 412,248 | 584,436 (kh0) | 1.00 MB |
  | de-DE | 440,732 | 634,996 (gl0) | 1.08 MB |
  | es-ES | 256,744 | 605,280 (zl0) | 0.86 MB |
  | fr-FR | 381,936 | 833,236 (nk0) | 1.22 MB |
  | it-IT | 252,044 | 628,268 (cm0) | 0.88 MB |

  All six are 6.46 MB; the ROM image is 2.05 MB of its 9.375 MB partition today.
- **Output:** 16 kHz, mono, signed 16-bit; the engine (`audio.md` §17) resamples it.
- **Memory:** Pico allocates only from pools its caller gives it, never `malloc`.
  - `pico_loadResource()` copies a voice file into the system's pool. The knowledge in it is
    read a byte at a time, never through a wider pointer, so it can stay where it lies at any
    alignment (to be checked: a read-only mapping at an odd address in the host tests).
  - **An engine is a fixed 1,000,000-byte pool** (`PICOCTRL_DEFAULT_ENGINE_SIZE`), the same
    32- and 64-bit. 789 KB of it is one unit's buffers (`picocep`, sized by
    `PICOCEP_MAXWINLEN` = 10,000 frames: the longest sentence it smooths). The rest of the
    system pool (headers, the knowledge bases' indexes) is a few KB beyond the voice's files.
  - **One engine to a system** (`pico_system_t` holds one), so two speakers are two systems; with
    the voices in place, a second one costs its engine and a few KB, not the voice again.
- **Speed (the host, a 32-bit build, -O2):** 24.5 s of speech (70 words of prose) in 0.50 s of
  CPU, 49 times real time. The board is to be measured (§7).
- **Rate:** that prose is 171 words a minute in en-US, 196 in en-GB, 165 in de-DE.
- **Markup:** Pico reads tags in the text (`<speed level='200'>`, `<pitch>`, `<volume>`,
  `<break time='500ms'/>`, `<p>`, `<s>`, `<spell>`, `<phoneme>`, ...) and spells out ones it
  doesn't know. A space after `<` doesn't stop one (`< p>` is still `<p>`). It has a switch for
  markup (`markupHandlingMode` in `picotok.c`), always on, set nowhere.

## 3. Design

### The library (`thirdparty/svox-pico`)

- `lib/` as released, `NOTICE`, `README.md` (version, hashes, the patches); the voices in
  `lang/` (Git LFS, as the icons' PNGs are).
- **BMDS patches** (`patches/bmds-*.diff`, applied in the tree as TinySoundFont's are):
  - `bmds-memory.diff`: `pico_loadResourceInPlace(system, data, size, &resource)`, a voice file
    used where it lies (the header parsed from memory, nothing copied, nothing freed at unload).
  - `bmds-engine.diff`: per engine, `pico_setEngineMarkup()` and `pico_setEngineProsody()` (speed,
    pitch and volume put before the next character, as the tags would be: injecting tags can't
    work with markup off, which the tokenizer checks as it reads, after `put()`).
  - `bmds-pal.diff`: `PICO_NO_FILES`, a platform layer that opens no files (in the firmware no
    `pico_loadResource()` from a path, and no `<genfile>`/`<usesig>`/`<play>` markup reaching one).
  - `bmds-fixes.diff`: what testing found (§11).
- `-fwrapv`: its fixed-point signal code lets signed products wrap (UBSan reports it on every
  sentence); its warnings off, as TinySoundFont's are. `-O2` if it measures faster (§7).

### The pieces (`components/bmds_speech`)

- **`Speaker`** (portable, host-tested): a Pico system and engine in one PSRAM pool (the engine's
  1,000,000 bytes and the system's), over a voice's two spans. `put(text)` takes UTF-8 (a
  character split across calls is fine), `render(samples)` makes what it can. Speed, pitch and
  volume are Pico's markup put before the text (the engine's own markup stays off unless asked).
- **`Voices`**: finds a voice by name in `speech/path`'s directories (`LANG_ta.bin` and one
  `LANG_*_sg.bin` beside it); `/rom`'s through `Vfs::map()`, nothing copied; one on a filesystem
  that can't map (the card) read into PSRAM once, shared, freed with its last speaker.
- **The speech fd** (`SpeechFile`, a `Stream` like `SynthFile`): text written, samples read.
- **`say`** (a shell command and a `/bin` applet), §4.
- **WAV writing** in `bmds_audio` (`WavFile::header()`): 44 bytes before PCM; not an encoder. A
  file that can't seek back (a pipe, stdout) gets the "length unknown" sizes, which `WavFile::parse()`
  already reads to the end.

### Data flow

`say` (or a program) writes text to a speech fd and reads 16 kHz samples; to play, it writes them
to an audio stream at 16 kHz as `play` does (the engine resamples and mixes); to save, it writes
them after a WAV header. Synthesis runs in the reader's thread, at its priority: nothing of the
kernel's runs it.

## 4. Interfaces (the ABI: frozen once shipped)

### The `say` command

```
say [-v VOICE] [-r WPM] [-p PITCH] [--markup] [-o FILE [--raw]] [-f FILE | TEXT...]
say -v ?
```

- Modelled on macOS's `say` (`-v`, `-r`, `-o`, `-f`), so scripts written there mostly work.
- **TEXT**, else `-f FILE` (`-` is stdin), else stdin.
- **`-v VOICE`** (`en-US`; `speech/voice` by default); `-v ?` lists the voices found, one a line:
  the name, then where it lies.
- **`-r WPM`:** words a minute, as macOS. 170 is Pico's own speed (speed 100 %), the range
  34 to 850 (Pico's 20 to 500 %).
- **`-p PITCH`:** percent, 50 to 200 (macOS has no such option).
- **`--markup`:** the text's Pico tags are obeyed; without it they are read as text.
- **`-o FILE`:** a WAV file (16-bit, mono, 16 kHz) instead of the speakers; `-o -` is stdout.
  `--raw` writes the samples without the header.
- **Playing** waits until it has been heard (as `play` does); Ctrl+C stops at once. Exit 0, or 1
  with a message.

### Settings

- `speech/path` (string): the directories voices are looked for in, `:` between them;
  default `/rom/share/speech:/sd0/p0/share/speech`.
- `speech/voice` (string): the voice used when none is named; default `en-US`.

### The ROM and the card

```
/rom/share/speech/en-US_ta.bin          en-US, read in place
/rom/share/speech/en-US_lh0_sg.bin
/rom/share/licenses/svox-pico/NOTICE    Pico's notice and the Apache License
```

The other five voices are a package, `speech-voices` (`dist/`, from `thirdparty/svox-pico/lang`,
no compiler), into `/sd0/p0/share/speech/`.

### Program calls (slots 114-115, appended)

```c
#define BMDS_SYS_AUDIO_SPEAK 114u      /* int32_t audio_speak(const bmds_speak_config_t *config): a new fd */
#define BMDS_SYS_AUDIO_SPEAK_INFO 115u /* int32_t audio_speak_info(int32_t fd, bmds_speak_info_t *info) */

#define BMDS_SPEAK_MARKUP 0x1000u /* config.flags: obey Pico's tags in the text */

typedef struct bmds_speak_config_t {
        uint32_t    size;   /* sizeof(bmds_speak_config_t) */
        uint32_t    flags;  /* BMDS_O_CLOEXEC, BMDS_SPEAK_MARKUP, or 0 */
        const char *voice;  /* "en-US"; NULL: speech/voice */
        uint32_t    speed;  /* percent, 20 to 500 (0: 100) */
        uint32_t    pitch;  /* percent, 50 to 200 (0: 100) */
        uint32_t    volume; /* percent, 1 to 500 (0: 100) */
} bmds_speak_config_t;

typedef struct bmds_speak_info_t {
        uint32_t size;      /* sizeof(bmds_speak_info_t) */
        uint32_t rate;      /* 16000 */
        uint32_t channels;  /* 1 */
        uint32_t format;    /* BMDS_AUDIO_S16LE */
        uint64_t frames;    /* read so far */
        uint32_t speed, pitch, volume; /* as configured */
        uint32_t flags;     /* BMDS_SPEAK_MARKUP if configured */
        char     voice[32]; /* BMDS_SPEAK_VOICE_MAX: the voice's name */
} bmds_speak_info_t;
```

- **The fd:** `write()` takes UTF-8 text and returns how much it took (a short count when Pico's
  input is full, `EAGAIN` when nothing fit: read first). `read()` returns whole samples, as many
  as it can make up to the buffer's size, and 0 when it has said everything it can without more
  text. **A NUL byte** ends what is before it: said in full. Without one the last sentence waits
  for more text, even after its full stop (Pico's: a sentence's end is said once text follows it).
  Neither waits; synthesis happens in `read()`, in the caller's thread. Closing stops it.
- **`bmds_say(const char *text)`** (inline, in `sys.h`, on the calls above and an audio fd): says
  @p text in the default voice and returns when it has been heard; 0 or `-errno`.
- **Errors:** `EINVAL` (a size, flag or range), `ENOENT` (no such voice), `ENOMEM`, `EMFILE`;
  `EIO` for a damaged voice file.

## 5. IDF usage

None: Pico is portable C over its own pools. The voices are read through the VFS (`Vfs::map()`
for `/rom`).

## 6. Resource budget (estimates; measured in §7 and as built)

- **Flash:** the firmware 169 KB more (measured, §12); the ROM 1.43 MB more (en-US).
- **PSRAM:** a speaker about 1.05 MB while its fd is open (the engine's fixed pool); a voice from
  the card its files' size (0.86-1.43 MB) while it speaks. Nothing at rest.
- **Internal RAM:** none beyond the reader's stack (Pico's deepest call measured on the board).
- **CPU:** in the reader's thread only; the host's 49 times real time suggests a few percent of a
  core on the board, to be measured.

## 7. Measurements first (step 2, on the board)

- Real-time factor and the delay to the first sample of a sentence; `-O2` against `-Os`.
- The voice in place in flash against copied into PSRAM: speed, and the desktop's frame time
  while it speaks (the window-move test: 19.2-19.7 ms a round, `foundation.md` §9). Pico reads
  its 1.4 MB of knowledge all over; from flash those reads share the cache with the firmware's
  code. If the UI lags, the voice goes in PSRAM (the user's rule: UI before memory).
- The stack Pico needs, and what an fd costs in PSRAM.

## 8. Testing

- **Host:** `Speaker` over the en-US voice mapped read-only at an odd address (every byte read in
  place, a write a fault); samples made, their count stable; markup on and off (`<speed>` changes
  the length only when on); UTF-8 split across `put()`s; NUL ending text without a full stop;
  two speakers at once; a damaged voice an error, not a crash. `WavFile::header()` read back by
  `WavFile::parse()`.
- **Fuzz:** `speech_fuzz`: text in (markup on and off, NULs, broken UTF-8), as the decoders and
  the synth were. Damaged voices too, for the card's (the ROM's are ours).
- **Board:** `say` to the speakers and to a file (played back with `play`), `-v ?`, a card voice;
  `progtest speech` (the calls, their errors, `bmds_say`).

## 9. Steps

1. **The library and `Speaker`:** vendored, patched, host-tested, fuzzed; its size.
2. **The kernel:** `Voices`, the speech fd, `say` (speakers and files), the settings, the ROM;
   the measurements of §7.
3. **The ABI:** slots 114-115, `sys.h`, `bmds_say`, `progtest speech`, `docs/ABI.md`.
4. **The voices package.**

## 10. Open questions and later

- `PICOCEP_MAXWINLEN` smaller (2,500 frames would save about 590 KB a speaker) if long sentences
  are split rather than lost; to try after step 1.
- Speech for the desktop (a screen reader, notifications spoken): later, on the same fd.
- A program's list of voices: `say -v ?` for now; a call if a program needs one.

## 11. As built: step 1 (2026-10-12)

- **Vendored** in `thirdparty/svox-pico` (`lib/`, the six voices in `lang/` under Git LFS, both
  notices), four patches (`README.md` there), built from `components/bmds_speech/pico.cmake`
  (`picodbg.c` and `picokdbg.c` left out: empty without `PICO_DEBUG`).
- **`Speaker`** (`bmds_speech`, portable): one Pico system and engine in a 1 MiB PSRAM pool.
  Measured on the host: the pool's peak 1,015,560 bytes; a voice in place costs its system
  14.6 KB of indexes. `poolUsed()` reads Pico's own count (`picopool.c`: Pico's internal headers
  aren't C++).
- **Facts found:**
  - Pico says a sentence once text follows it, or at a NUL: `say` and the fd's users end their
    text with one.
  - A voice's two files may come in either order (the voice is their knowledge together); two
    languages' halves make a voice too (it speaks).
  - The voices hold the same knowledge bases (the en-US list minus the optional FST_WPHO and
    FST_SPHO extras some have); no voice has FST_SVOXPA_PARSE, and the code checks for it.
- **Found and fixed** (`bmds-fixes.diff`):
  - Pico reads some of what it allocates before writing it (valgrind: `pr_getOutput`,
    `acphAccGetNrsLeft`, `transDurWeighted`): the same text sounded different with what the pool
    held before. Allocations are zeroed.
  - `<spell>` made items without their lowercase buffers and wrote through the pointer
    (`picobase_lowercase_utf8_str` from `pr_appendItemToOutItemList`): before the zeroing, into
    whatever the pool held. And every lowercase copy was bounded by 255 bytes, not the item's
    room, so a word growing when lowercased (U+023A, 2 bytes, to U+2C65, 3) ran into the next
    block. Found by `speech_fuzz`'s first seeds.
  - A voice missing a knowledge base crashed: "knowledge base missing" set the message's format,
    not its base, and copied from a null pointer; past that, `cepInitialize` read through the
    missing base. Engines are refused unless every base the units use unchecked is there.
  - `acphAccGetNrsLeft` read the word before the first (index -1).
  - The preprocessor's memory manager sat in a byte array at a 4-byte boundary: right on the
    32-bit target, misaligned on a 64-bit host (UBSan); aligned to 8.
- **Tests:** `tests/host/speech.cpp` (the voice mapped read-only at odd addresses; markup;
  prosody; UTF-8 split; NUL; reset; two speakers; damaged and mismatched voices);
  `WavFile::header()` read back by `parse()`. `tests/fuzz/speech.cpp` (`speech_fuzz`): text with
  markup on and off, four prosodies, pieces of every size, each run at most 8 s of speech (it is
  slow: one or two runs a second, mostly the signal generation under the sanitizers); after the
  fixes above, 10 minutes and then an hour (9,654 inputs, a corpus of 1,262) with no fault. The
  hour ran on the build before `bmds-float.diff` (§12).
- **Cost:** 157 KB of flash (Pico's code and tables), 8 bytes of internal RAM.

## 12. As built: step 2 (2026-10-12)

- **`Voices`** (`voices.cpp`): `speech/path` and `speech/voice`; a voice found as `LANG_ta.bin`
  and the first `LANG_*_sg.bin` beside it; up to four in use, shared, mapped through `Vfs::map()`
  or read into PSRAM (freed with the last user); the table a `PsramObject`; boot stage `speech`.
- **`say`** (`saycommand.cpp`) as §4; its text ends with a NUL. **`WavFile::header()`** in
  `bmds_audio`; a file that seeks gets its real sizes afterwards, stdout and pipes keep "to the end".
- **The ROM:** `share/speech/en-US_ta.bin`, `en-US_lh0_sg.bin`, `share/licenses/svox-pico/` (both
  notices): the image 2.05 → 3.50 MB.
- **Measured on the board** (24.5 s of prose, `say -o` to tmpfs, three runs each):

  | Build | Time | Real time |
  |---|---|---|
  | `-Os`, double exponential | 5.22 s | 4.7× |
  | `-Os`, float (`bmds-float.diff`) | 4.63 s | 5.3× |
  | all of Pico `-O2` | 3.84-4.03 s | 6.2× (+29 KB of flash) |
  | **the signal files `-O2`** (kept) | 3.96-4.15 s | 6.0× (+12 KB) |
  | the same, the voice copied into PSRAM | 3.83 s | 6.4× |

  Speaking aloud is a sixth of a core's time. Starting (the voice found, the engine made, "Hi."
  said) takes 0.20 s, in place or not. The profile is spread over Pico's units (`getFromPdf` 7.6 %,
  `picocep_fixptmult` 6.5 %, the FFT 5.8 %, ...); the double exponential was 12.7 % (ROM libgcc).
- **The voice in place against PSRAM, for the rest of the system** (`win bench icons`, the blend
  from the ROM and from PSRAM, five runs): alone 296 / 148-163 µs an icon; `say` flat out from flash
  380 / 171; from PSRAM 322 / 178; speaking aloud from flash 332 / 181, from PSRAM 346 / 169. Flat
  out, the voice in flash costs other flash readers 28 % (9 % from PSRAM); at real time the
  difference is lost in the noise. **Kept in place** (no 1.4 MB copy while speaking).
- **Stack:** a foreground `say` used about 5 KB of the shell's 64 KB (a background one 3.8 KB of
  a worker's 32 KB). **Memory:** 1.10 MB of PSRAM while speaking (the 1 MiB pool, `say`'s
  buffers, a background job's stack), 388 bytes of internal RAM (a background job's task); all
  back afterwards.
- **Firmware:** 1.95 MB. ESP-IDF's size check compares it with the smallest app partition (the
  2 MB recovery slot), not `ota_0`'s 4 MB: 7 % "free" now. To fix before it fails the build.

## 13. As built: step 3, the ABI (2026-10-12, the user's go)

- **Slots 114-115** (`audio_speak`, `audio_speak_info`) as §4, with `BMDS_SPEAK_MARKUP` and
  `BMDS_SPEAK_VOICE_MAX`; locked in `abilock.c`. The info has the speed, pitch, volume and flags
  as configured in place of the planned `pending` (Pico doesn't say how much text it holds).
- **The fd** (`SpeechFile`, `bmds_speech`; `Stream::speechFile()`): a write takes what Pico's input
  holds (`EAGAIN` when nothing: on the board 16 KB of text was taken in part), a read renders whole
  samples (an odd address through a block of 256), 0 when all up to the last NUL is said. A
  damaged voice is `EIO`; a bad name, flag or range `EINVAL`.
- **`bmds_say()`** (inline in `sys.h`, which now includes `bmds/errno.h`): a speech fd and
  `/dev/dsp` at 16 kHz mono with a 0.5 s ring; it returns when the closing `/dev/dsp` has played
  out.
- **`progtest speech [quiet]`**: the errors, a sentence (40,576 samples), the info, the NUL,
  odd reads, markup read (98,944) and obeyed (14,720) against plain text (26,432) and speed 300
  (14,720, the tag's), a full input, two fds; the same counts as the host's. Then `bmds_say`
  aloud. All pass on the board; PSRAM back to the byte after each run.
- **The SDK example `speak`**: `bmds_say`, or the samples from the fd saved as a WAV file (`play`
  plays it).
- **Found on the board (sysmon):**
  - `audio.dsp0.limited` warned on every spoken sentence: Pico's output at its own volume 100
    peaks at full scale and clips (231 samples at ±32767 in one sentence of en-US; es-ES would
    reach about +4.5 dBFS). Pico applies its volume before it clips, so a `volume` of 100 is a
    lower Pico volume (`Speaker::PicoVolumeAt100`, below).
  - `cpu.core1.busy_us` went critical: `say -o` (and `progtest speech quiet`) make speech as
    fast as they can, a core over 98 % for more than the rule's 5 s. Speaking aloud doesn't
    (a sixth of a core). Left to the user (any long CPU-bound job does the same).
- **The full rate and pitch range** (the user's request; `-r` 34-850, `-p` 50-200, alone and at
  the corners, into files and aloud):
  - **Low pitch clipped:** at pitch 50 Pico's peaks rise up to 3.8 dB (de-DE). And Pico's output
    peaks up to about 2 dB higher between samples than on them (energy near 8 kHz), which any
    correct resampler reconstructs (the engine's and scipy's alike: +2.07 and +2.18 dB on one
    file), so the limiter caught a sentence whose samples peaked at -1.94 dBFS. Now Pico's volume
    is 40 at `volume` 100, and in proportion to the pitch below 100 (20 at pitch 50). Measured on
    the host over six voices, three texts, pitches 50-200 and speeds 20-500 (324 cases), the
    highest peak between samples is -1.83 dBFS; speech is about 3 dB quieter at pitch 50 than
    at 100.
  - **Slow speech underran:** Pico makes a sentence's first samples only after working through
    it, and at speed 20 that took about 540 ms on the board (67 ms on the host, 8 times faster),
    past `say`'s 0.5 s ring. `say` and `bmds_say` now queue 2 s (64 KB of PSRAM), still playing
    from 250 ms.
  - **The top of `-r` falls short:** Pico's speed shortens the speech itself as asked (its
    voiced part 4.0 times shorter at 500 %, 5 times longer at 20 %), but not the pauses at
    commas and full stops: a sentence with them was only 2.95 times shorter at 500 % (the
    `<speed>` tag the same). So `-r 850` says about 410 words a minute of such text; the slow
    end is close (34 asked, 36 measured).
  - After: all 25 combinations of `-r` 34, 100, 170, 400, 850 and `-p` 50, 75, 100, 150, 200
    spoken aloud with no underrun, nothing limited, no problem in `stats`; `progtest speech`
    unchanged.
- **Clicks in slow, low-pitched speech** (the user's ear): Pico's own output, not the playback (the
  same with and without `bmds-float.diff`). At pitch 50 single samples near a peak flipped sign
  (12330, -12808, -12961, 12039): 8 steps over 0.25 of full scale in one sentence at normal speed,
  24 at the slowest; none at pitch 100 (largest step 0.083). UBSan without `-fwrapv`: the TD-PSOLA
  sum of overlapping pulses (`td_psola2`, `picosig2.c`) passed 2^31 (1484846739 + 721696418) and
  wrapped; at low pitch the pulses are larger. Its volume comes after, so lowering ours couldn't
  help. `bmds-overflow.diff`: each pulse added in at a quarter and the sum shifted that much less
  (normal pitch unchanged to rounding, 113 dB SNR); the mel-to-linear interpolation's product, which
  overflowed on most sentences (24 % of the samples differ once fixed, 33 dB SNR), and the output's
  scaling product in 64 bits. Then no overflow in 270 UBSan runs (six voices, pitches 50-200, speeds
  20-500, three texts), no step over 0.25 at pitch 50 (the board's slowest, lowest: largest 0.131),
  and the same speed (3.95-4.16 s for the prose). The steps over 0.25 left in the 324-case sweep
  (467, nearly all en-GB) are fricative noise (half the samples cross zero), not sign flips. Low
  pitch now peaks at most -4.6 dBFS between samples (it was -2.2: part of it was the wraps).
- **The underruns, why:** Pico produces speech a sentence at a time. Its pipeline takes in the
  whole sentence and smooths its parameters over all of it (`picocep`: matrix work on every frame
  of the sentence) before the signal unit makes the first sample, so between the last sample of
  one sentence and the first of the next, `read()` returns nothing for a while: about 70 ms on
  the board at normal speed, about 540 ms at speed 20, where a sentence has five times the frames.
  Overall Pico is still 6 times faster than real time; only that gap must be covered by what is
  queued. 0.5 s wasn't enough at the slowest rate; 2 s is (0 underruns since).
- **As loud as it can be** (the user's question): after the overflow fix the pitch rule above
  left low and high pitches 2.6-3.2 dB below the limit, and three voices quieter than the rest.
  Measured at a fixed Pico volume (six voices × pitches 50, 60, 70, 85, 100, 125, 150, 175, 200 ×
  speeds 20, 100, 500 × five texts, 810 runs, peaks between samples): the loudest voice's worst
  peak runs from -4.6 dBFS at pitch 50 to -10.1 at 200, and en-US, en-GB and fr-FR peak 1.7, 3.1
  and 3.7 dB below the loudest (es-ES, it-IT, de-DE) at every pitch. Now `Speaker::picoVolume()`
  takes Pico's volume from a curve over pitch that puts the loudest voice's worst peak at
  -1.5 dBFS at `volume` 100 (interpolated between the measured pitches), and `Voices` boosts
  Pico's three quieter voices by those amounts (another voice gets none). Checked end to end at
  the volumes the code computes (972 runs, pitches between the measured ones too, a sixth text not
  in the calibration): every voice and pitch peaks between -1.0 and -3.0 dBFS, nothing clipped.
  Against the build before: pitch 50 +3.2 dB, 100 +0.2, 200 +2.6, and the boosts on top. On the
  board, 15 combinations of `-r` 34, 170, 850 and `-p` 50, 65, 100, 140, 200 aloud: no underrun,
  nothing limited, no problem.

