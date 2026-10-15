/**
 * @file      usbhost.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/error.h>
#include <bmds/keyboarddecoder.h>
#include <bmds/mouselayout.h>
#include <bmds/mutex.h>
#include <bmds/result.h>
#include <bmds/span.h>
#include <bmds/task.h>

BMDS_NAMESPACE_BEGIN

/**
 * @brief A USB host port and the devices on it (`devplan/input.md`).
 *
 * Runs TinyUSB's host stack (vendored, `thirdparty/tinyusb`) on the
 * ESP32-P4's high-speed controller. The board's ports are behind a
 * high-speed hub: low- and full-speed devices are reached through its
 * transaction translator (split transactions). Starting it holds the hub
 * in reset until the controller is set up, then lets it go.
 *
 * One task (`usbN`) runs the stack: enumeration, hubs, class drivers and
 * their callbacks.
 *
 * @par Thread Safety
 * All public functions from any task.
 */
class UsbHost {
        public:
                /** @brief Host ports there can be (TinyUSB runs one). */
                static constexpr size_t MaxHosts = 1;

                /** @brief Devices tracked per host (hubs included). */
                static constexpr size_t MaxDevices = 16;

                /** @brief Interfaces kept per device. */
                static constexpr size_t MaxInterfaces = 4;

                /** @brief Keyboards and mice at once (boot-protocol HID interfaces). */
                static constexpr size_t MaxHid = 8;

                /** @brief Bytes kept of a string descriptor (ASCII, NUL included). */
                static constexpr size_t MaxName = 32;

                /** @brief A device's bus speed. */
                enum class Speed : uint8_t {
                        Low,  ///< 1.5 Mbit/s.
                        Full, ///< 12 Mbit/s.
                        High, ///< 480 Mbit/s.
                };

                /** @brief An interface's class triple. */
                struct Interface {
                                uint8_t number = 0;
                                uint8_t cls = 0;
                                uint8_t subclass = 0;
                                uint8_t protocol = 0;
                };

                /** @brief What is known of a connected device. */
                struct DeviceInfo {
                                uint8_t   address = 0; ///< Its bus address.
                                uint8_t   parent = 0;  ///< The hub's address it is on; 0: the root port.
                                uint8_t   port = 0;    ///< The hub port it is on (1 up); 0: the root port.
                                Speed     speed = Speed::Full;
                                uint16_t  vendor = 0;         ///< idVendor.
                                uint16_t  product = 0;        ///< idProduct.
                                uint8_t   cls = 0;            ///< bDeviceClass (0: by interface).
                                uint8_t   interfaceCount = 0; ///< In `interfaces` (at most `MaxInterfaces`).
                                Interface interfaces[MaxInterfaces];
                                char      manufacturer[MaxName] = {}; ///< ASCII, '?' for the rest; may be empty.
                                char      name[MaxName] = {};         ///< The product string; may be empty.
                };

                /** @brief Counters since start. */
                struct Stats {
                                uint32_t added = 0;      ///< Devices enumerated (hubs included).
                                uint32_t gone = 0;       ///< Devices disconnected.
                                uint32_t hidReports = 0; ///< HID reports received.
                                uint32_t keyboards = 0;  ///< Keyboards attached now.
                                uint32_t mice = 0;       ///< Mice attached now.
                                uint32_t rearmFailures =
                                        0; ///< HID transfers that couldn't be asked for again (retried).
                                uint32_t unhandledHalts = 0; ///< The controller's halts of no known cause (retried).
                                uint32_t unhandledHcint = 0; ///< The last one's channel interrupt bits.
                };

                /** @brief A HID interface's state (`hids()`). */
                struct HidState {
                                uint8_t  address = 0;
                                uint8_t  instance = 0;
                                bool     keyboard = false; ///< Else a mouse.
                                bool     armed = false;    ///< A report transfer is asked for.
                                uint32_t reports = 0;      ///< Reports received.
                                uint32_t sinceMs = 0;      ///< Since the last report (or attaching).
                };

                /**
                 * @brief Returns host @p number (created, stopped, on first use).
                 * @return It, or `NotSupported` (past `MaxHosts`, or the board has no USB host).
                 */
                static Result<UsbHost *> host(size_t number);

                /** @brief Boot stage: starts host 0 if the board has one. */
                static Error init();

                /** @brief Returns a speed's name (`low`, `full`, `high`). */
                static const char *speedName(Speed speed);

                UsbHost() = default;
                UsbHost(const UsbHost &) = delete;
                UsbHost &operator=(const UsbHost &) = delete;

                /**
                 * @brief Sets up the PHY and the stack, starts its task and lets the
                 *        hub out of reset. @blocking
                 * @return `InvalidState` if running, or the PHY's, stack's or task's error.
                 */
                Error start();

                /** @brief Returns true once started. */
                bool isRunning() const { return _running; }

                /**
                 * @brief Copies the connected devices into @p out, by address.
                 * @return How many were copied.
                 */
                size_t devices(Span<DeviceInfo> out);

                /**
                 * @brief Copies the HID interfaces' states into @p out (read as the host's task
                 *        changes them: a diagnostic).
                 * @return How many were copied.
                 */
                size_t hids(Span<HidState> out);

                /** @brief Returns the counters. */
                Stats stats();

                /** @brief Returns true when called on the host's task (TinyUSB's own). @nonblocking */
                bool isOnTask() const;

                // TinyUSB's callbacks, on the host's task. Not for other callers.
                /// @cond INTERNAL
                void configured(uint8_t address, const void *config);
                void mounted(uint8_t address);
                void unmounted(uint8_t address);
                void stringArrived(uint8_t address, bool product, bool ok);
                void hidMounted(uint8_t address, uint8_t instance, Span<const uint8_t> descriptor);
                void hidProtocolSet(uint8_t address, uint8_t instance, uint8_t protocol);
                void hidUnmounted(uint8_t address, uint8_t instance);
                void hidReport(uint8_t address, uint8_t instance, const uint8_t *data, uint16_t length);
                /// @endcond

        private:
                struct Device {
                                bool       present = false;
                                DeviceInfo info;
                                uint16_t   text[MaxName + 1] = {}; // a string descriptor arriving (UTF-16LE)
                };

                // A keyboard or mouse: a boot-protocol HID interface. On the host's task only.
                struct Hid {
                                enum class Kind : uint8_t {
                                        None,
                                        Keyboard,
                                        Mouse
                                };
                                Kind            kind = Kind::None;
                                uint8_t         address = 0;
                                uint8_t         instance = 0;
                                uint8_t         leds = 0;               // the LED report being sent (it must stay put)
                                bool            reportProtocol = false; // a mouse's reports decoded by layout
                                uint32_t        attachedMs = 0;         // uptime when it was attached
                                bool            armed = false;          // a report transfer asked for
                                uint32_t        retryMs = 0;            // when to ask again (not armed)
                                uint32_t        lastMs = 0;             // uptime at its last report (or attaching)
                                uint32_t        reports = 0;
                                KeyboardDecoder keys;
                                MouseLayout     layout; // a mouse's, from its report descriptor
                };

                static void entry(void *arg);
                static void emitKey(void *arg, const KeyEvent &event);
                Device     *slot(uint8_t address);
                void        fill(Device &d, uint8_t address);
                Hid        *hid(uint8_t address, uint8_t instance);
                uint32_t    repeatKeys(); // ms until the next repeat is due
                uint32_t    service();    // repeats and re-arms: ms until the next is due
                void        arm(Hid &h);  // asks for the next report
                void        setLocks(uint8_t locks);

                Mutex   _lock;
                Device  _devices[MaxDevices]; // by address - 1
                Hid     _hid[MaxHid];
                Stats   _stats;
                void   *_phy = nullptr; // usb_phy_handle_t
                Task    _task;
                uint8_t _number = 0;
                bool    _running = false;
};

BMDS_NAMESPACE_END
