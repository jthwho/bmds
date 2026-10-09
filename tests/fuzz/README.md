# Fuzz targets

libFuzzer targets, built with clang, AddressSanitizer and UndefinedBehaviorSanitizer. Not part of
the host tests: they run for as long as one lets them.

- **`decoder_fuzz`:** the decoders (`devplan/audio.md` §19), every format through `Decoder`: open,
  decode, convert, seek, decode again, read the tags, close; no fault, no read out of bounds, no
  undefined behaviour, no leak, no read that gives nothing without an error. MIDI files too
  (`devplan/audio.md` §22), played by the synth on `tests/host/synth/sine.sf2`.
- **`soundfont_fuzz`:** SoundFonts (§22): load, play notes on every preset, the drums and across
  the keyboard with controllers and bends, render; every sample finite, no leak.

```sh
CC=clang CXX=clang++ cmake -S tests/fuzz -B build-fuzz -G Ninja
cd build-fuzz && build
tests/fuzz/make-seeds.sh /mnt/data/tmp/seeds        # ffmpeg: each format in its forms, small
mkdir -p /mnt/data/tmp/corpus
./decoder_fuzz -fork=3 -ignore_crashes=1 -max_len=65536 -max_total_time=10800 \
        -artifact_prefix=/mnt/data/tmp/ /mnt/data/tmp/corpus /mnt/data/tmp/seeds ../tests/host/audio
```

```sh
tests/fuzz/make-font-seeds.py /mnt/data/tmp/fonts       # small SoundFonts made by the script
tests/fuzz/make-midi-seeds.py /mnt/data/tmp/midi        # a MIDI file: notes, chords, drums, tempo
./soundfont_fuzz -fork=3 -ignore_crashes=1 -max_len=65536 -max_total_time=3600 \
        -artifact_prefix=/mnt/data/tmp/ /mnt/data/tmp/fcorpus /mnt/data/tmp/fonts
```

A finding is fixed (a patch in `thirdparty/`, listed in its `README.md`), and the input is kept in
`tests/host/audio/fuzz/`, `tests/host/synth/fuzz/` (fonts) or `tests/host/synth/fuzz-midi/`, which
the host tests replay.
