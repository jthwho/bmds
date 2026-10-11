# TJpgDec (vendored)

ChaN's Tiny JPEG Decompressor, <http://elm-chan.org/fsw/tjpgd/>, used by `components/bmds_core`
(`SoftJpeg`) for baseline JPEGs too big for memory at full size: it decodes by MCU, a few KB of
working memory whatever the picture's size, and scales by 1/2, 1/4 or 1/8 as it decodes
(`devplan/images.md` §3b). The P4's ROM has no copy (other ESP32 chips' do).

- **Version:** R0.03 (2021-07-01), `tjpgd3.zip` sha256
  `052fe3efbc9a8be29f31597ad009c5b51a4f6905878eb28569e0ab3d46d0c013`. `tjpgd.c` and `tjpgd.h` as
  released but for the patch (CRLF line ends kept); the originals' sha256 `3a0091b6...e347a9` and
  `12d7b17c...1337a2`.
- **License:** the notice at the top of `tjpgd.c` (free for any use; keep the notice).
- **Changes:** one BMDS patch, `patches/bmds-mcu-output.diff`: with `JD_MCU_OUTPUT` defined, each
  MCU goes to that function instead of TJpgDec's `mcu_output` (BMDS's averages in YCbCr before
  converting when reducing, and writes straight into the picture, PIE where it pays:
  `components/bmds_core/src/softjpeg.cpp`). `tjpgdcnf.h` holds BMDS's settings: `JD_SZBUF` 4096
  (fewer input calls), `JD_FORMAT` 0 (RGB888), `JD_USE_SCALE` 1, `JD_TBLCLIP` 1, `JD_FASTDECODE` 2
  (Huffman tables: the fastest, about 10 KB of working memory), and `JD_MCU_OUTPUT`.
- **Built** by `components/bmds_core/src/tjpgd.c`, at `-O2` with `-Wno-shadow` (its code shadows
  variables) and `-Wno-unused-function` (its `mcu_output`, replaced).
- **Not vendored:** the documentation and the sample program.

To update: copy `tjpgd.c` and `tjpgd.h` from the new release, apply the patch, keep `tjpgdcnf.h`'s
settings, run the host tests (`tests/host/images.cpp`) and `simd test softjpeg` on the board.
