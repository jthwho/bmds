# Images: decoders, the JPEG codec, an image viewer

Planned 2026-10-10. A picture opened from the file browser (double click) shows in an image viewer,
scaled to its window with its aspect kept, and the folder's other pictures are a key away. Under it,
the system gains one way to read pictures, for itself and for programs: PNG, BMP and JPEG, JPEG by
the P4's hardware codec, built so that encoding fits in later.

## 1. Goals

- **A viewer**, `/bin/imageview` ("Image Viewer"), opening `image/*` from the file browser (the
  `mimeapps.list` line, a desktop entry). Built in like the media player, on the public toolkit
  only; nothing in the OS knows it (`builtin-apps-not-special`).
- **Fit to the window** with the aspect kept (the default), or **actual size** with panning.
- **Browsing:** the folder's pictures in the file browser's order, Next and Previous by key,
  wheel and buttons; the next one decoded ahead so a step is quick.
- **One image library** for the OS and programs (SDK calls): what a file is and its size, decoded
  into the pixel format and stride the caller asks for, scaled down on the way where the format
  allows (JPEG's 1/2, 1/4, 1/8).
- **The JPEG codec as a device** (`jpeg0`), flexible and efficient, for the viewer and anything
  else (a camera, MJPEG, thumbnails, screenshots as JPEG): zero-copy where the caller's buffer
  allows it, a bounce buffer where it doesn't, interrupt-driven, shared safely.
- **Encoding planned now** (the API's shape, the SDK's room), built in a later step.

## 2. What exists to build on (checked 2026-10-10)

- **`Png::decode()`** (bmds_core, `gui.md` §4h): the icons' decoder into 0xAARRGGBB. It refuses
  16-bit samples, greyscale under 8 bits and interlacing (`NotSupported`). **`Png::write()`**
  (bmds_ui): the screenshot's encoder, B, G, R in.
- **`FileFormat`** (bmds_core): `image/png`, `image/jpeg`, `image/gif`, `image/bmp` by extension;
  PNG, JPEG and GIF by magic (BMP's `BM` is too short to trust alone, the extension decides).
- **The screen** is 24-bit, bytes B, G, R (`display.md`). The painter draws through a strip in
  internal DMA RAM, its moves done by the 2D-DMA. It has `image()` for icons (ARGB, blended per
  pixel, x on a 4-pixel column), nothing for a large opaque picture.
- **ESP-IDF's `esp_driver_jpeg`:** a decoder engine and an encoder engine over the one JPEG block.
  - **Decoding** is baseline only (SOF0; progressive and the others are refused). It writes the
    whole picture, its width and height rounded up to 16. Out: RGB888 (either byte order),
    RGB565, grey, YUV 4:4:4/4:2:2/4:2:0. The output buffer must be cache-line aligned in address
    and size (64 bytes in PSRAM); the input has no rule. It waits for the block's interrupt on a
    semaphore (no polling), with a timeout.
  - **Encoding** is baseline. In: RGB888, RGB565, grey, YUV. Out: 4:4:4, 4:2:2, 4:2:0 or grey,
    quality 1-100. Its output must be cache-line aligned.
  - **No scaling** on either side.
- **The P4's ROM has no TJpgDec** (other chips' ROMs do). Software JPEG has to come with us.
- **The PPA** (pixel-processing accelerator: scale, rotate, mirror, blend; `esp_driver_ppa`) is
  unused so far.
- **The audio decoder's calls** (`audio_decode`, slots 71-73) are the SDK's pattern for a codec:
  a file by fd, or bytes in memory with fd -1; a config struct; an info struct.
- **PSRAM is 32 MB.** A 12-megapixel photo is 36 MB as RGB888: **a camera's photo can't be held
  whole at full size.** The design has to decode big pictures smaller.

## 3. The image library

### 3a. Pixels (bmds_core, pure)

- **`PixelFormat`**, named by bytes in memory so the name never depends on endianness:
  `Bgr24` (the screen's), `Rgb24`, `Bgra32` (0xAARRGGBB as a little-endian word, the icons'),
  `Rgb565`, `Gray8`. YUV formats are appended when the codec's YUV output is wanted (a camera,
  video).
- **`ImageBuffer`**: pixels, width, height, stride in bytes, format. It doesn't own its pixels.
- **`ImageInfo`**: codec, width, height, flags (has alpha, progressive, can use the hardware),
  EXIF orientation (1-8), and the decoded size the hardware will write (rounded up to 16).

### 3b. Decoders

| Format | How | Notes |
|--------|-----|-------|
| PNG | `Png::decode()`, extended | 16-bit samples (taken to 8), greyscale of 1, 2 and 4 bits, Adam7 interlacing. Out to any `PixelFormat`. |
| BMP | `Bmp` (new, pure) | 1, 4, 8 bits with palette; 16 (5-5-5, 5-6-5 by bit fields); 24; 32 (with alpha when its mask says); RLE4 and RLE8; bottom-up and top-down. |
| JPEG, baseline | `JpegCodec` (`jpeg0`) | The hardware, when the output fits the memory budget (§3d). |
| JPEG, baseline, big | TJpgDec (vendored, software; Elm-Chan's licence: keep the notice) | Streams by MCU: a few KB of memory whatever the size, scaled 1/2, 1/4 or 1/8 as it decodes (DC-only at 1/8). |
| JPEG, progressive | Later (§9) | Needs every coefficient in memory: up to a size budget only, with a decoder still to choose. Until then it is refused with a message that says so. |
| GIF | Later | Not asked for. |

- **`JpegHeader`** (bmds_core, pure, host-tested): parses the markers up to the first scan.
  It returns the size, the components and sampling, baseline or progressive, restart interval and
  EXIF orientation (APP1). It never touches the hardware: `info()` is cheap, and the code that
  picks a path decides with it.
- **The decoders are host-tested** against files made by Pillow (each kind of PNG and BMP; JPEGs
  at each sampling, with restart markers, grey). The JPEG paths are tested against TJpgDec's
  output, within a small error.

### 3c. `JpegCodec` (bmds_drivers, a numbered device)

- **`JpegCodec::codec(0)`**: the P4's one block, made at boot (the board lists it). The engines
  are made on first use, not at boot (they take a little internal DMA memory). Calls are
  serialized by the device's mutex: one picture at a time, others wait, `tryDecode()` doesn't.
- **`decode(ByteSpan jpeg, ImageBuffer &out, const JpegDecode &how)`**: `how` is the out format
  (`Bgr24`, `Rgb24`, `Rgb565`, `Gray8`, later YUV) and the YUV-to-RGB standard (BT.601 default).
  - **Zero-copy** when `out` is aligned (64 bytes in address, size and stride), its stride covers
    the width rounded up to 16 and it has the height rounded up to 16 rows. The codec writes
    straight into it.
  - **Otherwise a bounce buffer** (`PsramDma`), copied row by row into `out`, freed after. Its
    cost (a copy) is reported in the stats, so callers know to allocate better.
  - **`bufferFor(info, format)`** says the size, stride and alignment for zero-copy. The viewer
    and the SDK use it.
- **`encode(const ImageBuffer &in, const JpegEncode &how, sink)`**, planned (§7): quality and
  sampling, the same buffer rules, written through a sink as `Png::write()` is.
- **Counters** in `Stats` (`sysmon.md`): pictures, bytes, time busy, bounce copies, errors.
- **Hardware truth:** the block's presence comes from the board definition. The driver is ESP-IDF's
  (its interrupt and 2D-DMA); our layer adds the buffer rules, the lock, the numbering and the
  pure header parser.

### 3d. `Images` (the front, bmds_files)

- **`Images::info(file)`** and **`Images::decode(file, out, options)`**, `file` as bytes or a
  path. `options` holds the out format, the stride, and **`atLeast` (width, height): the smallest
  picture the caller needs**. The front picks the decoder and the scale:
  - A JPEG whose full size is far more than `atLeast` is decoded by TJpgDec at the largest
    reduction still at least `atLeast` (a 4000 × 3000 photo for a 1280 × 720 window: 1/2, to
    2000 × 1500). It is quicker, and takes 9 MB instead of 36.
  - Otherwise the hardware, at full size, when its output fits the budget (a setting,
    `image/decode-max`, in megapixels; default 8). Past the budget TJpgDec at a reduction.
  - PNG and BMP decode at full size: they have no cheap reduction. A PNG past the budget is
    refused (`TooBig`), with the budget named.
- **The result says** what was done: the decoded size, the reduction, the path (hardware or
  software) and the time.
- **EXIF orientation** is reported, and applied when asked (`options.orient`), by the scaler's
  rotation or the PPA's (§3e).

### 3e. Scaling (`ImageScaler`, bmds_core, pure)

- **Down:** area averaging (each output pixel the average of the input pixels it covers),
  separable, fixed-point: no aliasing on big reductions, which a sampling scaler gives.
- **Up:** bilinear.
- **Rotation by 90° steps and mirroring**, for EXIF orientation.
- **Bgr24 to Bgr24 first** (the viewer's path), the others as they're needed.
- **The PPA later** (§9): its scale-rotate-mirror in hardware for up-scaling and modest
  reductions, measured against the software. It samples rather than averages, so a big reduction
  still goes through the JPEG reduction or area averaging first.

### 3f. The SDK (appended calls)

As built (slots 93-95; `sdk/include/bmds/sys.h` has the structs, `abilock.c` locks them).
Following `audio_decode`'s pattern: a file by fd (read from where it is to its end), or bytes in
memory with fd -1.

- **`bmds_image_info(fd, data, length, want, info)`**: what the picture is (codec, size, alpha,
  progressive, EXIF orientation) and what decoding it as `want` asks gives: the size (reduced,
  turned), the decoder and reduction, and the layout of a buffer the codec decodes into without
  a copy (`align`, `stride`, `bytes`).
- **`bmds_image_decode(fd, d, info)`**: into `d.out` in any `BMDS_PIXEL_*` format and stride;
  `min_width`/`min_height` (no smaller than) or `box_width`/`box_height` (fitted in) let a JPEG
  come back reduced; `BMDS_IMAGE_ORIENT` undoes EXIF's orientation; `background` is what alpha is
  blended over in formats without it. A buffer from `bmds_mem_alloc(bytes, align, 0)` laid out
  as `image_info` says is decoded into by the codec without a copy. `-BMDS_EOVERFLOW`: too small.
- **`bmds_image_encode(fd, e)`**: a picture written as PNG (levels; stored) or JPEG (the codec:
  quality, 4:2:0/4:2:2/4:4:4/grey); returns the bytes written.

### 3g. A command

`image info FILE...` (codec, size, sampling, progressive, orientation, the path `decode` would
take) and `image convert [-s W H] IN OUT.png` (decode, scale, write a PNG). They are the board's
test bench and a tool: timings print as `screenshot`'s do. `OUT.jpg` comes with encoding. The
output format is frozen once shipped.

## 4. The viewer (`/bin/imageview`, bmds_imageview)

### 4a. The window

- **The picture**, centred on a dark background, with a **toolbar**: Previous, Next, Fit / Actual
  Size, Zoom In, Zoom Out, Rotate. A **status line** shows the name, the picture's size, the zoom,
  and "3 of 12".
- **The title** is "name — Image Viewer".
- **The window** opens sized to the picture, within most of the work area (nothing remembered:
  no settings file).

### 4b. Scaling

- **Fit** (the default): the picture scaled to the window, aspect kept, never above 100% unless
  Zoom In is used. It is scaled again on a resize, after the resize settles (a timer), with the
  old picture stretched cheaply meanwhile.
- **Actual size**: 1:1, panned by dragging, the arrow keys and the scroll bars.
- **Zoom** steps of √2, from fit to 800%. Up-scaling past 100% is bilinear.
- **The shown picture** is kept as `Bgr24` at the size shown, and drawn with **`Painter::pixels()`**
  (new, public in the toolkit). It copies a rectangle of a PSRAM picture onto the window's visible
  pieces by the 2D-DMA, with no CPU per pixel. Its x is on the painter's 4-pixel column: the
  picture's position is rounded to one.

### 4c. Browsing

- **The list:** the opened file's folder, read when it opens. Its pictures are the files whose
  type is `image/*` and that the library can decode, in the file browser's order (names
  naturally sorted). The folder is watched (`FolderWatches`): a picture added or removed is
  picked up.
- **Next and Previous:** Right, Left, Space, Backspace, Page Down/Up, the wheel in fit mode, and
  the toolbar's buttons. Home and End go to the first and last. Opening several files limits the
  list to them.
- **Decoding** is on a thread of the viewer's, so the window keeps answering. "Loading…" shows
  after 150 ms, and the old picture stays until the new one is ready. The next picture in the
  direction of travel is decoded ahead, and the previous one is kept, two pictures at most.
- **A file that won't decode** shows its name and why ("Progressive JPEG: not supported yet"), and
  browsing goes on.

### 4d. Keys and more

- **Keys:** F (fit), 1 (actual size), + and − (zoom), R and Shift+R (rotate, for the view only,
  not saved), Enter (maximize/restore), Escape (close). Matched by character, else by key (an
  injected key may carry none). Zoomed past the view, the arrows pan; else Left and Right step.
- **Double click** maximizes or restores.
- **Later:** Delete (to the trash, once the file browser has one), open with, copy to the
  clipboard as an image, a slideshow.

### 4e. How it's opened

`imageview [FILE...]`. The desktop entry `imageview.desktop` (MimeType `image/png;image/jpeg;
image/bmp;`, Icon `image-viewer`), and `image/*=imageview.desktop` in `boot/config/mimeapps.list`
(the user allows `/boot` edits). Each run is a window of its own (as the file browser's): no
service for now.

## 5. Budget (estimates, to be measured)

- **A 1920 × 1080 JPEG** by the hardware: 6 MB Bgr24 (zero-copy), a few tens of ms expected.
  Scaled to 1280 × 720: about 2.8 MB more.
- **A 4000 × 3000 photo** by TJpgDec at 1/2: 9 MB, about a second in software (to measure).
  Scaled to the window: 2.8 MB.
- **The viewer:** the shown picture, the current decode, the one decoded ahead (a cap, so a big
  picture doesn't prefetch).
- **Code:** TJpgDec about 10 KB of flash, `Bmp` a few KB, the scaler a few KB.
- **Internal RAM:** the JPEG engines' descriptors (made on first use), and TJpgDec's work area
  (about 4 KB, internal for speed) while it decodes.

## 6. ABI impact

- **Calls** (appended, slots 93-95): `image_info`, `image_decode`, `image_encode`; their structs
  and constants (`sdk/include/bmds/sys.h`, locked in `abilock.c`).
- **Setting:** `image/decode-max`.
- **Shell:** `image` (its output frozen).
- **Files:** `imageview.desktop`; `mimeapps.list` gains `image/*`.
- **Toolkit** (in-firmware, not yet the SDK's): `Painter::pixels()`.

## 7. Encoding (planned, not built now)

- **`JpegCodec::encode()`**: Bgr24, Rgb24, Rgb565, Gray8 in; 4:2:0, 4:2:2, 4:4:4 or grey out;
  quality 1-100; zero-copy input when aligned (the hardware's input has no alignment rule, only
  its output does: a bounce buffer for the stream, then the sink).
- **`Images::encode()`**: JPEG or PNG (`Png::write()`), by codec.
- **`image_encode`** (an SDK call, appended then), `image convert IN OUT.jpg`.
- **Screenshots as JPEG** when `screenshot/path` ends in `.jpg` (`templates.md`).
- **Later users:** a camera (MIPI-CSI on the board), MJPEG.

## 8. Phasing

1. **Pure parts:** `PixelFormat`, `ImageBuffer`, `JpegHeader` (with EXIF orientation), `Bmp`,
   `Png` extended, `ImageScaler` (area down, bilinear up, 90° steps). Host-tested.
2. **`JpegCodec`** (`jpeg0`): decode by the hardware, zero-copy and bounce, the lock, the
   counters. The `image` command (`info`, `convert` to PNG). Timed on the board.
3. **TJpgDec** vendored for big baseline JPEGs at 1/2-1/8. `Images::info/decode` picks the path.
   `image/decode-max`.
4. **The SDK calls** (slots 93-94), progtest's `image` mode.
5. **`Painter::pixels()`** and the viewer: fit, actual size and pan, zoom, browsing with the
   decode thread and one ahead, the desktop entry, mimeapps.
6. **EXIF orientation** applied, rotation keys, and the PPA's scaling measured against the
   software.
7. **Encoding** (§7), when wanted.

## 9. As built (2026-10-10), and what the hardware taught

- **Steps 1-4 done.** The pure parts (`PixelFormat`, `Pixels`, `Png` extended, `Bmp`, `JpegHeader`,
  `SoftJpeg`, `ImagePlan`, `ImageScaler`; host-tested against Pillow and libjpeg and an exact
  area average); `JpegCodec` (decode and encode); `Images` and `image`; the calls (slots 93-95,
  `image_encode` with them, since encoding came at no cost). `progtest image` passes.
- **ESP-IDF's YUV-to-RGB is limited range** (its 2D-DMA tables are video's, Y 16-235): every
  JPEG came out contrast-stretched. The codec's decodes load full-range JFIF coefficients
  instead: ESP-IDF's `dma2d_configure_color_space_conversion` is wrapped by the link
  (`-Wl,--wrap`) and, only while the codec decodes, rewrites RX channel 0's (the only one that
  converts colour). A smooth 4:2:0 picture then matches libjpeg within 4 (mean 0.5).
- **The colour range is a choice** (2026-10-10, the user's): `JpegCodec::Decode::limitedRange` keeps
  ESP-IDF's video range (Y 16-235) for MJPEG and video later; JPEG files take full range.
- **The PPA can't convert JPEG colour on this chip:** its full-range YUV input takes no 4:2:2
  (this revision: "unsupported color mode") and its 4:2:0 writes the picture but never ends.
  Its scale factors are 16ths, so it can't scale to an exact size either: the PPA isn't used.
- **The codec writes rows padded to the MCU** (8 or 16 by the sampling, not always 16), and
  ESP-IDF refuses pictures whose width × height isn't a multiple of 8 (TJpgDec takes those).
- **Timings** (the board, 360 MHz, PSRAM at 80 MHz): a 1920 × 1080 4:2:0 JPEG by the codec 71
  ms (zero-copy), 1024 × 768 27 ms; a 4000 × 3000 photo by TJpgDec at 1/4 5.1 s (TJpgDec does
  its whole IDCT below 1/8: Huffman and IDCT on 12 MP); scaling 1920 × 1080 to 1280 × 720 0.73 s
  (compute-bound: about 130 cycles an input pixel); PNG decode of 1280 × 720 0.58 s (our
  inflate); JPEG encode of 1280 × 720 45 ms, 1920 × 1080 94 ms.
- **Pillow's "box" filter isn't an area average** (it places its support differently): ours
  matches an exact area average to within rounding.
- **Step 5 done.** `Painter::pixels()` (a PSRAM picture to the window by the 2D-DMA, x and width
  on 4-pixel columns, waited for so the picture may change after); `/bin/imageview`
  (`bmds_imageview`, its `ViewLayout` host-tested), `imageview.desktop`, `image/*` in
  `mimeapps.list`. The worker thread decodes (fitted: a big JPEG reduced to the view; zoomed: the
  whole picture, within `image/decode-max`), turns, and scales only the part that shows into a
  picture of the view's size; the next one is decoded ahead. A zoom keeps the view's centre;
  a drag moves the part already made, which is made again when the drag ends. The status bar's
  zoom is against the file's pixels. Seen on the board: a double click on `turned.jpg` in the file
  browser opened it upright (EXIF 6) in a window sized to it; Right stepped through the folder
  in the browser's order; a 4000 × 3000 photo showed "Loading..." then itself (TJpgDec at 1/4);
  1:1 and a drag panned; R turned; Home went to the first; the progressive JPEG said why; Escape
  and the close box ended it (memory back where it was).
- **Steps 6 and 7 done.** EXIF orientation and turning are `ImageScaler::orient()`'s (the PPA isn't
  used, above). Encoding: `JpegCodec::encode()` (B, G, R or grey in without a copy; the others
  converted: the codec's "RGB888" reads B, G, R, its "RGB565" byte order is unchecked),
  `Images::encode()`, `image_encode`, `image convert OUT.jpg`, and screenshots as JPEG when the
  name ends in `.jpg` (1280 × 720 in 120 ms, against 540 for PNG).
- **SIMD and speed-ups since** (`simd.md` §3a): scaling 1080p to 720p 291 ms, a 12 MP photo at 1/4
  3.86 s, PNG decode 283 ms, turning a 1080p picture 277 ms. **The encoder's cache bug** (random
  corrupt streams) fixed there.
- **To do, measured:** big photos with restart markers in strips by the codec.

## 10. Open questions

- **Progressive JPEG:** which software decoder (stb_image's JPEG is progressive-capable and
  public domain, but holds every coefficient), and up to what size. Web pictures are often
  progressive; cameras' aren't.
- **GIF** (first frame, then animation), **WebP**: when asked for.
- **The PPA, to explore further** (the user's, 2026-10-10: not written off): why its 4:2:0 input
  never ends (a descriptor or burst setting of ours or ESP-IDF's?), what a newer chip revision
  changes (4:2:2), its blending and fills for the window manager, rotation for EXIF and video, and
  scaling where 16ths suffice (thumbnails, video). A subsystem of its own if it pays.

## 11. Decisions (the user, 2026-10-10)

- **Formats:** PNG, JPEG and BMP. JPEG by the hardware decoder, with software for photos too big
  for memory.
- **Built in like the media player**, on the public toolkit.
- **The JPEG codec flexible and efficient**, usable elsewhere and through the SDK, with encoding
  planned (even if built later).
