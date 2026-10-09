# TinySoundFont (vendored)

Bernhard Schelling's SoundFont 2 synthesizer, <https://github.com/schellingb/TinySoundFont>, used by
`components/bmds_synth` (`devplan/audio.md` §22). Licenses: `tsf.h` MIT (`LICENSE`, and its
header), `tml.h` zlib (its header).

- **Version:** v0.9, commit `853a0a171759f1ddba0de1442133a75912bbeffa` (2026-07-19). As released:
  `tsf.h` sha256 `70d55963c98f60ebb81518eaa1f25d46888d5180eb5f5289fd6b74ffc177d197`, `tml.h`
  sha256 `93257db259e0efb2ea2037d7157841bec8cb4a2d7986286e43c8090705326546`.
- **Changes to `tsf.h`:** three BMDS patches, in this order (CRLF line endings, as released):
  - `patches/bmds-render.diff`: the voice loop for a single-precision FPU (the ESP32-P4's):
    samples kept as 16-bit (half the memory, no conversion at load), the position in 32.32 fixed
    point (it was `double`, done in software here), the filter in `float`, the gain ramped across
    each block, a voice's pitch ratio kept between blocks instead of a `pow()` each, and
    `TSF_TANF` for the filter's cutoff. With `synthmath.h`'s `powf`/`expf`/`tanf` a voice went
    from 542 to about 14 µs a 64-frame block on the board (§22).
  - `patches/bmds-sf3.diff`: SF3 (Vorbis) samples decoded to 16-bit, stb_vorbis's memory from one
    buffer grown until the stream opens; an SF2's sample chunk becomes the samples without a copy.
  - `patches/bmds-bounds.diff` (found by `tests/fuzz/soundfont_fuzz`): the hydra's indices checked
    before presets are built; instruments and samples that exist (not the terminal records);
    no more regions than counted; sample positions inside the samples with room for the next one;
    compressed and raw sample offsets inside the chunk; times converted to sample counts without
    overflow (`tsf_samples()`); each list read once (a second one leaked the first); a NaN pitch
    ratio (from a damaged font) played as 0.

- **Changes to `tml.h`:** `patches/bmds-tml.diff` (found by `tests/fuzz/decoder_fuzz`, which plays
  MIDI files through the synth): a zero division refused (the header check had `&&` for `||`, and
  the tempo divided by it), the tracks' and a track's buffer checked when allocated, an empty
  track not parsed (arithmetic on a null buffer), a message's time in milliseconds kept to an
  `int`. The decoder reads files with `tml_load()` and a reader of the right type:
  `tml_load_memory()` calls its reader through a pointer of another type.

To update: take the new release's `tsf.h` and `tml.h`, apply the patches in order (`patch -p1`),
and fuzz.
