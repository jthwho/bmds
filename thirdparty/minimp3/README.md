# minimp3 (vendored)

lieff's MP3 decoder, <https://github.com/lieff/minimp3>, used by `components/bmds_audio` for MPEG
audio layers I to III (`devplan/audio.md` §19). License: `LICENSE` (CC0 1.0).

- **Version:** master, commit `ea99364f61c14656440e8d77e9c233ccf3124633` (2026-07-27). `minimp3.h`
  as released: sha256 `57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad`.
- **Changes:** one BMDS patch:
  - `patches/bmds-scratch.diff`: `mp3dec_decode_frame_scratch()`, the decoder with its 16 KB
    scratch area supplied by the caller, and `mp3dec_scratch_bytes()`. `mp3dec_decode_frame()`
    kept its 17,328 bytes of stack (measured with `-fstack-usage`), more than half a program's
    default stack; in PSRAM the scratch decodes as fast (1.578 s against 1.584 s for 30 s on the
    board).
- **Built with:** `MINIMP3_FLOAT_OUTPUT` and `MINIMP3_NO_SIMD`
  (`components/bmds_audio/codecs.cmake`), at `-O2`.
- **Not vendored:** `minimp3_ex.h` (file reading, seeking and tags are BMDS's own, in
  `mp3decoder.cpp`), the tests and the players.

To update: copy `minimp3.h` and `LICENSE` from the new commit, apply the patch, run the host tests
(`tests/host/decoder.cpp`) and the fuzzer (`tests/fuzz`).
