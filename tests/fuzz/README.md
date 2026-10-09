# Fuzz targets

libFuzzer targets, built with clang, AddressSanitizer and UndefinedBehaviorSanitizer. Not part of
the host tests: they run for as long as one lets them.

- **`decoder_fuzz`:** the decoders (`devplan/audio.md` §19), every format through `Decoder`: open,
  decode, convert, seek, decode again, read the tags, close; no fault, no read out of bounds, no
  undefined behaviour, no leak, no read that gives nothing without an error.

```sh
CC=clang CXX=clang++ cmake -S tests/fuzz -B build-fuzz -G Ninja
cd build-fuzz && build
tests/fuzz/make-seeds.sh /mnt/data/tmp/seeds        # ffmpeg: each format in its forms, small
mkdir -p /mnt/data/tmp/corpus
./decoder_fuzz -fork=3 -ignore_crashes=1 -max_len=65536 -max_total_time=10800 \
        -artifact_prefix=/mnt/data/tmp/ /mnt/data/tmp/corpus /mnt/data/tmp/seeds ../tests/host/audio
```

A finding is fixed (a patch in `thirdparty/`, listed in its `README.md`), and the input is kept in
`tests/host/audio/fuzz/`, which the host tests replay.
