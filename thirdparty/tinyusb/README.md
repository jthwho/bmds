# TinyUSB (vendored, host side)

Ha Thach's TinyUSB, <https://github.com/hathach/tinyusb>, used by `components/bmds_input` through
`components/bmds_tinyusb` (see `devplan/input.md`). License: `LICENSE` (MIT).

- **Version:** 0.21.0 (`tinyusb-0.21.0.tar.gz` from the GitHub tag, sha256
  `b7cdf35c5ccefb0f61640aff94a732ab4ebcb21498201e4a161545891b23ba01`).
- **Kept:** only what the host side needs, from upstream's `src/`: `tusb.c`, `tusb.h`,
  `tusb_option.h`, `common/`, `osal/osal.h`, `osal/osal_freertos.h`, `osal/osal_none.h`,
  `host/`, `class/hid/hid.h`, `class/hid/hid_host.{c,h}`, and from
  `portable/synopsys/dwc2/` the host driver (`hcd_dwc2.c`), `dwc2_common.{c,h}`,
  `dwc2_type.h` and `dwc2_esp32.h`.
- **Changes:** none. The configuration is BMDS's,
  `components/bmds_tinyusb/include/tusb_config.h`.

To update: unpack the new release, copy the same files over these, and read its release notes
for changed callbacks and options (`tusb_option.h`).
