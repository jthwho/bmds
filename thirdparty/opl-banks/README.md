# OPL instrument banks (vendored)

The FM synth's General MIDI banks (`devplan/audio.md` §26), in DMX's OP2 format (11,908 bytes
each: 128 instruments, 47 percussion entries, their names), staged in `/rom/share/synth` and read
there in place. Made again by `tools/import-opl-banks`.

- **`dmxopl3.op2`**: DMXOPL3 by Sneakernets, <https://github.com/sneakernets/DMXOPL>, the
  repository's `GENMIDI.op2` at commit `e83c144a2e6d3c283e20f9d16d011126931892f5` (2018-07-13, the
  newest), sha256 `92fc03c7734c45b1ece2bfad1f79d1abbd2b27ec6b45fbfb1c5a74f5ce0138c9`. License:
  MIT, in `dmxopl3-LICENSE`; the author asks for the credit "DMXOPL3 by Sneakernets". It uses the
  OPL3's eight waveforms.
- **`freedoom.op2`**: Freedoom's GENMIDI, the `GENMIDI` lump of Freedoom 0.13.0's
  `freedoom2.wad` (`freedoom-0.13.0.zip`, the release `dist/freedoom` packages), sha256
  `0bea035cca1ef715c8bbbc00a7eb207084f0f8c47f8900dc053e9b1923a7d770`. License: BSD-3-Clause, in
  `freedoom-COPYING.txt`. A relative of DMXOPL3 (an earlier version of it, by the bytes: 110 of
  the 175 instruments are the same, and it uses the OPL3's waveforms too).

To update: change the commit or the release in `tools/import-opl-banks`, run it, record the
hashes it prints here, and run the host tests (`tests/host/fmsynth.cpp` reads both banks).
