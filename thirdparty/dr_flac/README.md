# dr_flac (vendored)

David Reid's FLAC decoder, <https://github.com/mackron/dr_libs>, used by `components/bmds_audio`
(`devplan/audio.md` §19). License: at the end of `dr_flac.h` (public domain, or MIT No
Attribution, as one chooses).

- **Version:** 0.13.4 (unreleased: master), commit `dfe8377631000664666519fdb83da193fd8037f4`
  (2026-08-31, "Fix a seeking error"). `dr_flac.h` as released: sha256
  `111144e778f55738db6851cb226015c419e00d04b916a09506d4856d9cff945c`.
- **Changes:** one BMDS patch:
  - `patches/bmds-seek-estimate.diff`: the binary search's estimate of where a frame is divided
    by the start of the frame it had found, 0 for the first: NaN, made an integer (undefined
    behaviour; found by the fuzzer). It now bisects then, and an estimate past the range is the
    range's end.
- **Built with:** `DR_FLAC_NO_STDIO`, `DR_FLAC_NO_OGG` (Ogg FLAC isn't played: 14 KB less),
  `DR_FLAC_NO_SIMD` (`components/bmds_audio/codecs.cmake`), at `-O2`. Frames' CRCs are checked.
  It is opened without a metadata callback, so it loads no metadata block (a picture can be
  megabytes): `flacdecoder.cpp` reads the comments itself.

To update: copy `dr_flac.h` from the new release, apply the patch, run the host tests
(`tests/host/decoder.cpp`) and the fuzzer (`tests/fuzz`).
