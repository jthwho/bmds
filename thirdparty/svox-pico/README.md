# SVOX Pico (vendored)

SVOX AG's text-to-speech engine as Android shipped it, used by `components/bmds_speech`
(`devplan/speech.md`). License: Apache-2.0, the code (`NOTICE`, and each file's header) and the
voices (`lang/NOTICE`).

- **Source:** <https://android.googlesource.com/platform/external/svox>, commit
  `c5a33e4fdc55b63d4aa33aecefcc91b63da921e7` (2018-02-14, the last): `pico/lib` as `lib/`, its
  `NOTICE`; `pico/lang/*.bin` as `lang/`, `pico_resources/NOTICE` beside them. The sources as
  released, concatenated in name order (`cat lib/*.c lib/*.h`), hash to sha256
  `5b5c280e71421007430d31d9b75ec293472d8b5d6071bfc07dd3368c144acdfb`.
- **The voices** (Git LFS; a voice is its text analysis `_ta` and its signal generation `_sg`):

  | File | sha256 |
  |---|---|
  | `de-DE_gl0_sg.bin` | `39147b816d08bbbe64b23797804cf691a761ab73a7689ca9e2b6f87ace722654` |
  | `de-DE_ta.bin` | `df292826754713d5efb6c95d60582c17753e95856b4c735c3150db722b2c218a` |
  | `en-GB_kh0_sg.bin` | `8f9f5d13a920a9f3371517178d42ce89b835f1e587760c2db280beb593c388fa` |
  | `en-GB_ta.bin` | `57998f6423236dea25cd572a73b13f371e64155840e31b571eba7fca0b521b7e` |
  | `en-US_lh0_sg.bin` | `d6946526696d9eb085f7b23b1fbdf7a9986caca658b645e9eb73c6514fd4789d` |
  | `en-US_ta.bin` | `3a2a4d23de2a44dbbff50f69dcb9486be53b6d0a871d77cdc35f45c3e25e82aa` |
  | `es-ES_ta.bin` | `b18c60baf249466bf8785d333d8cb4cf1f6b62e5545986f621c66568f1851961` |
  | `es-ES_zl0_sg.bin` | `82e724d3c788767ec95f0d22b0ab738e677ad397f26ec0bbbf2c07aafdf29000` |
  | `fr-FR_nk0_sg.bin` | `85e2d63cf9f5522ea2a483d25ddd1cb0c6bfeecc39c92884aecfe7d3f179837a` |
  | `fr-FR_ta.bin` | `d9da5eb3cd7aac7071ab8ff7fee8d716f58572ee5393c1f0a8db946d6bf32f97` |
  | `it-IT_cm0_sg.bin` | `c48be4dc1f5a3c77a3dc4c8a457949b69bc433d674fac4fac91d49c01770e7e4` |
  | `it-IT_ta.bin` | `9b243c3470660293691d0d4e000901d7c5d0fa71976b33986e92e93a860ade33` |

- **Changes:** BMDS patches, applied to `lib/` in this order (`patch -p1` from this directory
  reproduces it from the release):
  - `patches/bmds-memory.diff`: `pico_loadResourceInPlace()`, a voice file used where its bytes
    lie (a mapping of `/rom`), nothing copied; every length in its header checked against its
    size. Pico reads the knowledge a byte at a time, so any alignment does.
  - `patches/bmds-engine.diff`: per engine, `pico_setEngineMarkup()` (tags in the text obeyed,
    or read as text: Pico's own switch, which nothing set) and `pico_setEngineProsody()` (speed,
    pitch and volume, put before the next character as the tags would be).
  - `patches/bmds-pal.diff`: `PICO_NO_FILES`, a platform layer that opens no files (the
    firmware's: resources come in memory, and markup naming files, `<genfile>`, `<usesig>`,
    `<play>`, finds none); a generic platform (upstream names `PICO_GENERIC` but never defines
    it); `<stdint.h>` for the `uintptr_t` several files use.
  - `patches/bmds-fixes.diff` (found by the host tests and valgrind): allocations zeroed (Pico
    reads some of what it allocates before writing it, so the same text sounded different with
    what the memory held before); "knowledge base missing" set the message's format, not its base,
    and copied from a null pointer; an engine refused unless its voice has every knowledge base
    the processing units use without checking (a voice missing one crashed in `cepInitialize`);
    `acphAccGetNrsLeft`'s read of the word before the first (index -1). And, found by
    `tests/fuzz/speech.cpp`: `<spell>` made items without their lowercase buffers and wrote through
    the pointer; every lowercase copy was bounded by 255 bytes instead of the item's room (a word
    growing when lowercased ran into the next block); the preprocessor's memory manager in a byte
    array aligned to 8; `picoos_StrRead`'s byte initialized (GCC -O2 warned).
  - `patches/bmds-float.diff`: the fast exponential in single precision (`picopal_quick_expf`, the
    same approximation with 23 bits of mantissa where the double one set 20 and left the rest
    uninitialized), and the signal code's `exp()` calls in `float`: the ESP32-P4 has no double
    FPU, and the double one was a tenth of the synthesis. The speech differs from the double
    version's by 90.6 dB SNR.
  - `patches/bmds-overflow.diff` (found by ear on the board: clicks in slow, low-pitched speech,
    then UBSan over every voice, pitch and speed): the TD-PSOLA sum of overlapping pulses
    (`td_psola2`) passed 2^31 at low pitch and wrapped, flipping a sample's sign; each pulse is now
    added in at a quarter and the sum shifted that much less (12 dB of headroom, the output the same
    to rounding: 113 dB SNR at normal pitch). The mel-to-linear interpolation's product
    (`mel_2_lin_lookup`) overflowed on most sentences and wrapped a spectral bin; and the output's
    scaling product could too: both in 64 bits. No overflow left in 270 runs (six voices, pitches
    50-200, speeds 20-500, three texts).
