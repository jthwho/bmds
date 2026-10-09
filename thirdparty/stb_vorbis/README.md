# stb_vorbis (vendored)

Sean Barrett's Ogg Vorbis decoder, <https://github.com/nothings/stb>, used by
`components/bmds_audio` (`devplan/audio.md` §19). License: at the end of `stb_vorbis.c` (public
domain, or MIT, as one chooses).

- **Version:** 1.22, commit `1ee679ca2ef753a528db5ba6801e1067b40481b8`. `stb_vorbis.c` as
  released: sha256 `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`.
- **Changes:** four BMDS patches, in this order:
  - `patches/bmds-io.diff`: `STB_VORBIS_IO` and `stb_vorbis_open_io()`, a stream read through a
    callback (a positional read) for a file that is neither in memory nor a `FILE *`. It keeps
    memory mode's semantics exactly (a failed read leaves the position; a skip may pass the
    end): with a failed read moving to the end, seeking failed after a stream had been read to
    its end (found by the host tests).
  - `patches/bmds-seek-assert.diff`: `stb_vorbis_seek()` fails, instead of asserting, when the
    frame found doesn't hold the sample (seeking to the stream's end; a damaged file).
  - `patches/bmds-comments.diff`: the comment header's lengths are checked against the file's
    before they are allocated, and the list's length is only what was allocated. A damaged
    count made `vorbis_deinit()` free through a null list; a negative vendor length wrote before
    its buffer (found by the fuzzer).
  - `patches/bmds-lookup1.diff`: `lookup1_values()` compares a codebook's powers as floating
    point, without casting them to `int`: a large codebook's overflowed it, which is undefined
    (found by the fuzzer).
- **Built with:** `STB_VORBIS_IO`, `STB_VORBIS_NO_STDIO`, `STB_VORBIS_NO_PUSHDATA_API`
  (`components/bmds_audio/codecs.cmake`), at `-O2`, `-Wshadow` off for this file (its code
  shadows). All its memory comes from a buffer `vorbisdecoder.cpp` gives it (`stb_vorbis_alloc`),
  so it never calls `malloc()` or `alloca()`.
- **Its own left shifts of negative values** (in `get32_packet` at the end of a packet) are
  defined by GCC, the firmware's compiler; the fuzzer's UBSan is told not to report them.

To update: copy `stb_vorbis.c` from the new release, apply the patches in order, run the host tests
(`tests/host/decoder.cpp`) and the fuzzer (`tests/fuzz`).
