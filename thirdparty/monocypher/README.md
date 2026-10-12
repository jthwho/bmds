# Monocypher (vendored)

Loup Vaillant's Monocypher, <https://monocypher.org/>: X25519 and Ed25519 for Dropbear (patch
0003 of `dist/dropbear`), in place of its TweetNaCl code, which is 11 to 37 times slower on the
board (`devplan/network.md` §26).

- **Version:** 4.0.2, `monocypher-4.0.2.tar.gz` sha256
  `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`. `src/monocypher.c`,
  `src/monocypher.h` and `src/optional/monocypher-ed25519.c` and `.h` (Ed25519 with SHA-512,
  as SSH's) as released; their sha256:
  `monocypher.c` `afe2b098...734c59`,
  `monocypher.h` `f78bb312...43f374`,
  `monocypher-ed25519.c` `7c9b1605...abdee2`,
  `monocypher-ed25519.h` `bd546edc...05121b`.
- **License:** `LICENCE.md`: BSD 2-clause or CC0, either.
- **Changes:** none.
- **Built** by `dist/dropbear/build.sh`, at `-O2`, into Dropbear's link.
- **Not vendored:** the tests, the manual, the other optional code.

To update: copy the four files from the new release, then on the board time a login and check
`ssh` with an Ed25519 key both ways.
