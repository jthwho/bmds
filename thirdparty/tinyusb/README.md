# TinyUSB (vendored, host side)

Ha Thach's TinyUSB, <https://github.com/hathach/tinyusb>, used by `components/bmds_input` through
`components/bmds_tinyusb` (see `devplan/input.md`). License: `LICENSE` (MIT).

- **Version:** 0.21.0 (`tinyusb-0.21.0.tar.gz` from the GitHub tag, sha256
  `b7cdf35c5ccefb0f61640aff94a732ab4ebcb21498201e4a161545891b23ba01`).
- **Kept:** only what the host side needs, from upstream's `src/`: `tusb.c`, `tusb.h`,
  `tusb_option.h`, `common/`, `osal/osal.h`, `osal/osal_freertos.h`, `osal/osal_none.h`,
  `host/`, `class/hid/hid.h`, `class/hid/hid_host.{c,h}`, `class/midi/midi.h`,
  `class/midi/midi_host.{c,h}` and `class/audio/audio.h` (MIDI, `devplan/audio.md` §21), and from
  `portable/synopsys/dwc2/` the host driver (`hcd_dwc2.c`), `dwc2_common.{c,h}`,
  `dwc2_type.h` and `dwc2_esp32.h`.
- **Changes:** one, in `hcd_dwc2.c` (marked `BMDS:`): a channel that halts in buffer DMA mode for a
  cause none of the IN or OUT handlers covers is retried as a transaction error (then failed, so
  the class driver asks again), instead of being left halted with its endpoint busy for good; such
  halts are counted (`tuh_dwc2_unhandled_halts`, the last `hcint` kept) for the `usb` command. A
  keyboard went silent with Doom in full screen (`devplan/games.md` §20). The configuration is
  BMDS's, `components/bmds_tinyusb/include/tusb_config.h`.

To update: unpack the new release, copy the same files over these, and read its release notes
for changed callbacks and options (`tusb_option.h`).
