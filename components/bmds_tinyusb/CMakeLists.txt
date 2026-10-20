# bmds_tinyusb — TinyUSB's host and device stacks (vendored in thirdparty/tinyusb), with
# BMDS's configuration (include/tusb_config.h): the ESP32-P4's high-speed
# controller as the host, a hub, HID (devplan/input.md), MIDI (devplan/audio.md §21); the
# full-speed controller as a device, a serial port and MIDI (devplan/audio.md §23).
#
# TinyUSB is upstream code: built with ESP-IDF's flags, not BMDS's warning
# policy (bmds_target_options).

set(BMDS_TINYUSB_DIR ${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/tinyusb/src)

idf_component_register(SRCS ${BMDS_TINYUSB_DIR}/tusb.c
                            ${BMDS_TINYUSB_DIR}/common/tusb_fifo.c
                            ${BMDS_TINYUSB_DIR}/host/usbh.c
                            ${BMDS_TINYUSB_DIR}/host/hub.c
                            ${BMDS_TINYUSB_DIR}/class/hid/hid_host.c
                            ${BMDS_TINYUSB_DIR}/class/midi/midi_host.c
                            ${BMDS_TINYUSB_DIR}/device/usbd.c
                            ${BMDS_TINYUSB_DIR}/class/cdc/cdc_device.c
                            ${BMDS_TINYUSB_DIR}/class/midi/midi_device.c
                            ${BMDS_TINYUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c
                            ${BMDS_TINYUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c
                            ${BMDS_TINYUSB_DIR}/portable/synopsys/dwc2/hcd_dwc2.c
                       INCLUDE_DIRS include ${BMDS_TINYUSB_DIR}
                       REQUIRES freertos
                       PRIV_REQUIRES esp_hw_support esp_mm)
