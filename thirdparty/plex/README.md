# IBM Plex (vendored)

IBM's typeface family, <https://github.com/IBM/plex>: the system's TrueType fonts
(`devplan/fonts.md`), staged in `/rom/share/fonts` and read there in place. License: the SIL Open
Font License 1.1, in `LICENSE.txt` (staged as `/rom/share/licenses/ibm-plex/LICENSE.txt`).

- **Files:** `fonts/*.ttf`, as released (`fonts/complete/ttf/` of each package), kept in Git LFS.
  Not subset or otherwise changed: the license reserves the name "Plex", which a modified font
  may not use.
  - `IBMPlexSans-Regular.ttf`, `-Bold.ttf`, `-Italic.ttf`: `@ibm/plex-sans@1.1.0`
    (`ibm-plex-sans.zip`, sha256 `fb365d910566e6d199cc2c15579a7dd9a267128e18431a394ed81f1970c69200`).
  - `IBMPlexMono-Regular.ttf`, `-Bold.ttf`: `@ibm/plex-mono@2.5.0` (`ibm-plex-mono.zip`, sha256
    `6d23f01257663d8cc49a0d64c22ced630b79e0e2a0ac08a0da86e9a38bbc481c`).
  - `IBMPlexSerif-Regular.ttf`: `@ibm/plex-serif@2.0.0` (`ibm-plex-serif.zip`, sha256
    `c006b095c47b919c6c8d78319e9e628745033f5db94507c1c9dfe4b35dd51f1e`).
- **Size:** 1.17 MB together, in the ROM (no RAM: read where they lie).

To update: download the release zips from the repository's releases, copy the same files from
`fonts/complete/ttf/`, record the versions and hashes here, and run the host tests
(`tests/host/fonts.cpp`, whose kerning figures come from the files).
