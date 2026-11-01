# Fuzz targets

libFuzzer targets, built with clang, AddressSanitizer and UndefinedBehaviorSanitizer. Not part of
the host tests: they run for as long as one lets them.

- **`decoder_fuzz`:** the decoders (`devplan/audio.md` §19), every format through `Decoder`: open,
  decode, convert, seek, decode again, read the tags, close; no fault, no read out of bounds, no
  undefined behaviour, no leak, no read that gives nothing without an error. MIDI files too
  (`devplan/audio.md` §22), played by the synth on `tests/host/synth/sine.sf2`.
- **`ntp_fuzz`:** NTP's parser (`devplan/network.md` §22) with an arbitrary cookie and T1, and
  what parses through the sample, a clock filter and the discipline; no fault, no overflow.
- **`netclassifier_fuzz`:** the real-time classifier (`devplan/network.md` §30d), which reads every
  frame `eth0` takes while a flow is open: rules and a frame from the input; a match inside the
  frame, and a UDP match written again with the classifier's headers matching the same rule.
- **`ptp_fuzz`:** PTP (`devplan/network.md` §31): each message parsed, built again and parsed to
  the same fields; and an ordinary clock's port driven by the input (messages, the time moving on,
  departure stamps, configurations), its clock stepped and slewed as it asks; what it sends must
  parse, no fault, no overflow, no timer that never moves on.
- **`sftp_fuzz`:** our SFTP server's protocol core (`programs/sftp-server/sftp.c`,
  `devplan/network.md` §24) after an INIT, requests in sequence, the file calls made up (files,
  directories, links, errors, names as long as names get); every reply whole and fitting, every
  handle closed at the end, no fault, no leak.
- **`syslog_fuzz`:** the system log's text forms (`devplan/logging.md`): a message as any program
  may write it to `/dev/log`, its priority, timestamp and tag taken off and its lines cleaned as
  the kernel does, and the input read as a stamped log line; every view inside the input.
- **`fontface_fuzz`:** TrueType faces (`devplan/fonts.md`): open, the names, the character map,
  kerning, glyphs boxed and rendered sharp and smooth at three sizes, with stb_truetype's asserts
  on; no fault, no read outside the file, no box larger than allowed, no leak.
- **`pkg_fuzz`:** packages and indexes (`devplan/packages.md`): any bytes read as a package (the
  head, the icons, the manifest, every file's data blocks decoded), an index (the head, the
  stanzas, the icons), as icons, manifest, dependency and stanza text, and as versions compared (equal to themselves,
  antisymmetric, transitive); every view inside the input, every name, path and version valid;
  an index's stanzas walked as installed packages and candidates (`PkgSolver`): every plan
  installs each package once, after what it needs, with what was asked for not automatic; every
  stanza text written as an unsigned index reads back the same.
- **`http_fuzz`:** HTTP responses (`devplan/packages.md` §23), fed whole and in pieces: the same
  outcome however they arrive, progress on every call, views inside what was fed, no body longer
  than its length; lines as URLs, parsed, resolved against the first, built into a request.
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

```sh
tests/fuzz/make-ttf-seeds.py /mnt/data/tmp/ttf          # small fonts cut from IBM Plex (fontTools)
TMPDIR=/mnt/data/tmp ./fontface_fuzz -fork=3 -ignore_crashes=1 -max_len=16384 -max_total_time=3600 -print_funcs=0 \
        -artifact_prefix=/mnt/data/tmp/ /mnt/data/tmp/tcorpus /mnt/data/tmp/ttf
```

```sh
mkdir -p pkg-seeds && cp ../tests/host/pkg/* pkg-seeds/       # and manifest, stanza and version texts
./pkg_fuzz -fork=3 -ignore_crashes=1 -max_len=131072 -max_total_time=900 -artifact_prefix=./pkg- pkg-corpus pkg-seeds
```

```sh
./ntp_fuzz -max_len=256 -max_total_time=600 -artifact_prefix=./ntp- ntp-corpus ntp-seeds
```

```sh
tests/fuzz/make-ptp-seeds.py ptp-seeds
./ptp_fuzz -max_len=8192 -max_total_time=600 -print_funcs=0 -artifact_prefix=./ptp- ptp-corpus ptp-seeds
```

```sh
./netclassifier_fuzz -max_len=1600 -max_total_time=600 -artifact_prefix=./nc- nc-corpus
```

```sh
tests/fuzz/make-sftp-seeds.py sftp-seeds
./sftp_fuzz -max_len=8192 -max_total_time=600 -rss_limit_mb=1024 -print_funcs=0 \
        -artifact_prefix=./sftp- sftp-corpus sftp-seeds
```

```sh
./syslog_fuzz -max_len=1024 -max_total_time=300 -rss_limit_mb=1024 -print_funcs=0 \
        -artifact_prefix=./syslog- syslog-corpus
```

`-print_funcs=0`: on this machine libFuzzer stalled for seconds printing each new function (its
symbolizer), so the target ran ten inputs a second instead of 28,000.

`ntp-seeds` holds the input layout (8 bytes cookie, 8 bytes T1, little-endian, then the packet):
a good reply and a kiss-o'-death are enough for it to find the rest.

A finding is fixed (a patch in `thirdparty/`, listed in its `README.md`), and the input is kept in
`tests/host/audio/fuzz/`, `tests/host/synth/fuzz/` (fonts), `tests/host/synth/fuzz-midi/` or
`tests/host/fonts/fuzz/` (TrueType), which the host tests replay.
