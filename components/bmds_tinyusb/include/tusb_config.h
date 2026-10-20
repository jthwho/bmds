/**
 * @file      tusb_config.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TinyUSB's configuration for BMDS (devplan/input.md): a host on the
 * ESP32-P4's high-speed controller (TinyUSB's root port 1), behind the
 * board's hub, with hubs and HID. C, read by TinyUSB's sources.
 */

#ifndef BMDS_TUSB_CONFIG_H
#define BMDS_TUSB_CONFIG_H

#define CFG_TUSB_MCU OPT_MCU_ESP32P4
#define CFG_TUSB_OS OPT_OS_FREERTOS

// Root port 1 (the high-speed controller) as a high-speed host: low- and
// full-speed devices behind the hub through its transaction translator
// (split transactions).
#define CFG_TUSB_RHPORT1_MODE (OPT_MODE_HOST | OPT_MODE_HIGH_SPEED)

// Root port 0 (the full-speed controller) as a device, while `usb/device` is on: swapped onto
// the USB-C's PHY (devplan/audio.md §23). Started and stopped by tud_rhport_init()/tud_deinit().
#define CFG_TUSB_RHPORT0_MODE (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUH_ENABLED 1
#define CFG_TUH_MAX_SPEED OPT_MODE_HIGH_SPEED

// The controller's FIFOs read and written by the CPU (slave mode): no DMA,
// no cache maintenance; keyboards and mice move a few bytes a millisecond.
#define CFG_TUH_DWC2_DMA_ENABLE 0

#define CFG_TUSB_DEBUG 0

// Devices and hubs: the board's hub and its four ports, and a hub or two more.
#define CFG_TUH_HUB 3
#define CFG_TUH_DEVICE_MAX 8
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// HID interfaces (a keyboard is often two: keys, and media keys).
#define CFG_TUH_HID 8
#define CFG_TUH_HID_EPIN_BUFSIZE 64
#define CFG_TUH_HID_EPOUT_BUFSIZE 8

// USB MIDI devices (devplan/audio.md §21). TinyUSB gives each 1 KB of endpoint buffers whatever
// the device's packet size; its receive FIFO must hold a whole packet (512 bytes for a high-speed
// device, as audio interfaces with MIDI often are), its send FIFO a few messages.
#define CFG_TUH_MIDI 2
#define CFG_TUH_MIDI_RX_BUFSIZE 512
#define CFG_TUH_MIDI_TX_BUFSIZE 64

// The device: a serial port (the console) and MIDI, its cables (up to 4) on one interface. Its
// buffers are static (internal RAM): kept small, the console's output waits for room.
#define CFG_TUD_ENDPOINT0_SIZE 64
// tud_task_ext() returns after each event (else only on a timeout), so the device's thread sees
// a stop asked for by a deferred call.
#define CFG_TUD_TASK_EVENTS_PER_RUN 1
#define CFG_TUD_CDC 1
#define CFG_TUD_CDC_RX_BUFSIZE 128
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_EP_BUFSIZE 64
#define CFG_TUD_MIDI 1
#define CFG_TUD_MIDI_RX_BUFSIZE 128
#define CFG_TUD_MIDI_TX_BUFSIZE 256
#define CFG_TUD_MIDI_EP_BUFSIZE 64

#endif // BMDS_TUSB_CONFIG_H
