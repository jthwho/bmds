/**
 * @file      usbhost.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A USB host port on TinyUSB's host stack (devplan/input.md §3).
 */

#include <climits>
#include <cstdio>
#include <driver/gpio.h>
#include <esp_private/usb_phy.h>
#include <tusb.h>
#include <bmds/board.h>
#include <bmds/input.h>
#include <bmds/log.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>
#include <bmds/system.h>
#include <bmds/usbhost.h>
#include "usbmidi.h"

// hcd_dwc2.c's (thirdparty/tinyusb/README.md): halts it retried that it had no case for.
extern "C" volatile uint32_t tuh_dwc2_unhandled_halts;
extern "C" volatile uint32_t tuh_dwc2_unhandled_hcint;
extern "C" uint32_t          tuh_dwc2_channels(uint8_t rhport, uint32_t *interrupts);

BMDS_NAMESPACE_BEGIN

namespace {

        // The hub held in reset this long before it is let go (its datasheet
        // asks for more than 1 ms at power-up; 10 is generous and cheap).
        constexpr unsigned int HubResetMs = 10;

        constexpr uint8_t  RootPort = 1; // TinyUSB's number for the high-speed controller
        constexpr uint16_t EnglishUs = 0x0409;
        constexpr unsigned TaskPriority = 4;
        constexpr size_t   TaskStack = 4096;

        constinit StaticObject<UsbHost> hosts[UsbHost::MaxHosts];

        // A host's counters (devplan/sysmon.md §6): names are an interface once shipped.
        constexpr Stat::Desc UsbStats[] = {
                {"added", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Devices enumerated (hubs included)"},
                {"gone", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Devices disconnected"},
                {"hid_reports",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 Stat::KeepHistory,
                 {},
                 "",
                 "HID reports received"},
                {"keyboards", Stat::Unit::Count, Stat::Kind::Level, 0, {}, "", "Keyboards attached now"},
                {"mice", Stat::Unit::Count, Stat::Kind::Level, 0, {}, "", "Mice attached now"},
                {"hid_rearm_failures",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {},
                 "",
                 "HID transfers that couldn't be asked for again (retried)"},
                {"unhandled_halts",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {Stat::Rule::Zero, Stat::Health::Warning},
                 "",
                 "Controller halts of no known cause (retried: a device would have fallen silent)"},
        };

        // A HID transfer that couldn't be asked for (TinyUSB refused it) is asked again this soon.
        constexpr uint32_t RearmRetryMs = 20;

        void readUsb(void *arg, Span<int64_t> v) {
                const UsbHost::Stats s = static_cast<UsbHost *>(arg)->stats();
                v[0] = s.added;
                v[1] = s.gone;
                v[2] = s.hidReports;
                v[3] = s.keyboards;
                v[4] = s.mice;
                v[5] = s.rearmFailures;
                v[6] = s.unhandledHalts;
                return;
        }

        struct HostCounters {
                        HostCounters(UsbHost &host)
                            : group(UsbStats, sizeof(UsbStats) / sizeof(UsbStats[0]), readUsb, &host) {}
                        ::bmds::Stats::Group group;
        };

        constinit StaticObject<HostCounters> hostCounters[UsbHost::MaxHosts];

} // namespace

namespace detail {

        // The running host: TinyUSB has one.
        UsbHost *activeUsbHost() {
                return hosts[0].isCreated() ? &*hosts[0] : nullptr;
        }

} // namespace detail

namespace {

        UsbHost::Speed speedOf(uint8_t speed) {
                switch (speed) {
                        case TUSB_SPEED_LOW: return UsbHost::Speed::Low;
                        case TUSB_SPEED_HIGH: return UsbHost::Speed::High;
                        default: return UsbHost::Speed::Full;
                }
        }

        // A string descriptor (UTF-16LE after its 2-byte header) as ASCII, '?' for the rest.
        void copyString(const uint16_t *desc, size_t units, char (&out)[UsbHost::MaxName]) {
                const size_t length = (desc[0] & 0xffu) >= 2 ? ((desc[0] & 0xffu) - 2u) / 2u : 0;
                size_t       n = 0;
                for (size_t i = 0; i < length && i + 1 < units && n + 1 < sizeof(out); ++i) {
                        const uint16_t c = desc[i + 1];
                        out[n++] = c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?';
                }
                // Trailing spaces some devices pad with.
                while (n > 0 && out[n - 1] == ' ') --n;
                out[n] = '\0';
                return;
        }

        void setHubReset(const GpioLine &line, bool asserted) {
                (void)gpio_set_level(static_cast<gpio_num_t>(line.gpio), asserted != line.activeLow);
                return;
        }

        void stringDone(tuh_xfer_t *xfer) {
                UsbHost *host = detail::activeUsbHost();
                if (host != nullptr)
                        host->stringArrived(xfer->daddr, xfer->user_data != 0, xfer->result == XFER_RESULT_SUCCESS);
                return;
        }

} // namespace

Result<UsbHost *> UsbHost::host(size_t number) {
        if (number >= MaxHosts || board().usbHost.portCount == 0) return makeError<UsbHost *>(Error::NotSupported);
        if (!hosts[number].isCreated()) {
                hosts[number].create();
                hosts[number]->_number = static_cast<uint8_t>(number);
                hostCounters[number].create(*hosts[number]);
                hostCounters[number]->group.setName("usb", int(number));
                (void)::bmds::Stats::add(hostCounters[number]->group);
        }
        return makeResult(&*hosts[number]);
}

Error UsbHost::init() {
        auto [h, err] = host(0);
        if (err == Error::NotSupported) return Error::Ok; // no USB host on this board
        if (err.isError()) return err;
        err = h->start();
        if (err.isOk()) bmdsInfo("USB: host 0 started (high speed)");
        return err;
}

const char *UsbHost::speedName(Speed speed) {
        switch (speed) {
                case Speed::Low: return "low";
                case Speed::Full: return "full";
                case Speed::High: return "high";
        }
        return "?";
}

Error UsbHost::start() {
        Mutex::Locker lock(_lock, WaitForever);
        if (_running) return Error::InvalidState;
        // The hub in reset until the controller is set up.
        const BoardUsbHost &board = bmds::board().usbHost;
        if (board.hubReset.isValid()) {
                gpio_config_t io = {};
                io.pin_bit_mask = 1ull << board.hubReset.gpio;
                io.mode = GPIO_MODE_OUTPUT;
                setHubReset(board.hubReset, true);
                if (gpio_config(&io) != ESP_OK) return Error::IoError;
                setHubReset(board.hubReset, true);
        }
        // The UTMI PHY, from ESP-IDF; TinyUSB drives the controller.
        usb_phy_config_t phy = {};
        phy.controller = USB_PHY_CTRL_OTG;
        phy.target = USB_PHY_TARGET_UTMI;
        phy.otg_mode = USB_OTG_MODE_HOST;
        phy.otg_speed = USB_PHY_SPEED_UNDEFINED;
        usb_phy_handle_t handle = nullptr;
        const esp_err_t  ret = usb_new_phy(&phy, &handle);
        if (ret != ESP_OK) return Error::fromEspErr(ret);
        _phy = handle;

        tusb_rhport_init_t rh = {};
        rh.role = TUSB_ROLE_HOST;
        rh.speed = TUSB_SPEED_HIGH;
        if (!tuh_rhport_init(RootPort, &rh)) {
                (void)usb_del_phy(handle);
                _phy = nullptr;
                return Error::IoError;
        }
        char         name[configMAX_TASK_NAME_LEN];
        Task::Config tc;
        std::snprintf(name, sizeof(name), "usb%u", _number);
        tc.name = name;
        tc.priority = TaskPriority;
        tc.stackBytes = TaskStack;
        const Error err = _task.start(tc, entry, this);
        if (err.isError()) {
                (void)tuh_deinit(RootPort);
                (void)usb_del_phy(handle);
                _phy = nullptr;
                return err;
        }
        if (board.hubReset.isValid()) {
                Task::sleep(HubResetMs);
                setHubReset(board.hubReset, false);
        }
        _stats = Stats();
        _running = true;
        return Error::Ok;
}

size_t UsbHost::devices(Span<DeviceInfo> out) {
        Mutex::Locker lock(_lock, WaitForever);
        size_t        n = 0;
        for (const Device &d : _devices)
                if (d.present && n < out.size() && tuh_connected(d.info.address)) out[n++] = d.info;
        return n;
}

UsbHost::Stats UsbHost::stats() {
        Mutex::Locker lock(_lock, WaitForever);
        Stats         s = _stats;
        s.unhandledHalts = tuh_dwc2_unhandled_halts;
        s.unhandledHcint = tuh_dwc2_unhandled_hcint;
        if (_running) s.channelsTaken = tuh_dwc2_channels(RootPort, &s.channelInterrupts);
        return s;
}

size_t UsbHost::hids(Span<HidState> out) {
        const auto now = static_cast<uint32_t>(System::uptimeUs() / 1000);
        size_t     n = 0;
        for (const Hid &h : _hid) {
                if (h.kind == Hid::Kind::None || n >= out.size()) continue;
                HidState &o = out[n++];
                o.address = h.address;
                o.instance = h.instance;
                o.keyboard = h.kind == Hid::Kind::Keyboard;
                o.armed = h.armed;
                o.reports = h.reports;
                o.sinceMs = now - h.lastMs;
        }
        return n;
}

bool UsbHost::isOnTask() const {
        return _running && xTaskGetCurrentTaskHandle() == _task.handle();
}

// TinyUSB's events; between them, until the next key repeat is due.
void UsbHost::entry(void *arg) {
        auto *self = static_cast<UsbHost *>(arg);
        for (;;) tuh_task_ext(self->service(), false);
}

// Key repeats due, and HID transfers that couldn't be asked for asked again.
uint32_t UsbHost::service() {
        uint32_t   next = repeatKeys();
        const auto now = static_cast<uint32_t>(System::uptimeUs() / 1000);
        for (Hid &h : _hid) {
                if (h.kind == Hid::Kind::None || h.armed) continue;
                if (int32_t(now - h.retryMs) >= 0) arm(h);
                if (!h.armed) {
                        const uint32_t due = int32_t(h.retryMs - now) > 0 ? h.retryMs - now : 1;
                        if (due < next) next = due;
                }
        }
        return next;
}

// Asks for @p h's next report. A refusal (the endpoint busy, no channel free) would leave the
// device unheard until it is plugged again: counted, and asked again shortly.
void UsbHost::arm(Hid &h) {
        h.armed = tuh_hid_receive_report(h.address, h.instance);
        if (h.armed) return;
        const auto now = static_cast<uint32_t>(System::uptimeUs() / 1000);
        h.retryMs = now + RearmRetryMs;
        uint32_t failures;
        {
                Mutex::Locker lock(_lock, WaitForever);
                failures = ++_stats.rearmFailures;
        }
        if (failures <= 4 || failures % 100 == 0)
                bmdsWarn("USB: %u: a HID report couldn't be asked for (%lu so far): asking again", h.address,
                         static_cast<unsigned long>(failures));
        return;
}

void UsbHost::emitKey(void *, const KeyEvent &event) {
        Input::key(event);
        return;
}

uint32_t UsbHost::repeatKeys() {
        const auto now = static_cast<uint32_t>(System::uptimeUs() / 1000);
        uint32_t   next = KeyboardDecoder::NoRepeat;
        for (Hid &h : _hid) {
                if (h.kind != Hid::Kind::Keyboard) continue;
                const uint32_t due = h.keys.repeat(now, emitKey, this);
                if (due < next) next = due;
        }
        return next;
}

UsbHost::Hid *UsbHost::hid(uint8_t address, uint8_t instance) {
        for (Hid &h : _hid)
                if (h.kind != Hid::Kind::None && h.address == address && h.instance == instance) return &h;
        return nullptr;
}

// The locks are the system's, not a keyboard's: every keyboard gets them, and its LEDs.
void UsbHost::setLocks(uint8_t locks) {
        for (Hid &h : _hid) {
                if (h.kind != Hid::Kind::Keyboard) continue;
                h.keys.setLocks(locks);
                h.leds = locks;
                (void)tuh_hid_set_report(h.address, h.instance, 0, HID_REPORT_TYPE_OUTPUT, &h.leds, 1);
        }
        return;
}

UsbHost::Device *UsbHost::slot(uint8_t address) {
        return address >= 1 && address <= MaxDevices ? &_devices[address - 1] : nullptr;
}

// The configuration descriptor, during enumeration: its interfaces kept.
void UsbHost::configured(uint8_t address, const void *config) {
        const auto   *desc = static_cast<const tusb_desc_configuration_t *>(config);
        Mutex::Locker lock(_lock, WaitForever);
        Device       *d = slot(address);
        if (d == nullptr) return;
        d->info = DeviceInfo();
        const uint8_t *p = static_cast<const uint8_t *>(config);
        const uint8_t *end = p + desc->wTotalLength;
        while (p + 2 <= end && p[0] >= 2 && p + p[0] <= end) {
                if (p[1] == TUSB_DESC_INTERFACE && p[0] >= sizeof(tusb_desc_interface_t)) {
                        const auto *intf = reinterpret_cast<const tusb_desc_interface_t *>(p);
                        if (intf->bAlternateSetting == 0 && d->info.interfaceCount < MaxInterfaces) {
                                Interface &i = d->info.interfaces[d->info.interfaceCount++];
                                i.number = intf->bInterfaceNumber;
                                i.cls = intf->bInterfaceClass;
                                i.subclass = intf->bInterfaceSubClass;
                                i.protocol = intf->bInterfaceProtocol;
                        }
                }
                p += p[0];
        }
        // A hub is never mounted for its user (TinyUSB keeps hubs): present now,
        // and listed while connected.
        bool hub = d->info.cls == TUSB_CLASS_HUB;
        for (size_t i = 0; i < d->info.interfaceCount; ++i) hub |= d->info.interfaces[i].cls == TUSB_CLASS_HUB;
        if (hub) fill(*d, address);
        return;
}

// What TinyUSB knows of @p d at @p address: it is present. With the lock held.
void UsbHost::fill(Device &d, uint8_t address) {
        DeviceInfo &info = d.info;
        info.address = address;
        tusb_desc_device_t desc = {};
        if (tuh_descriptor_get_device_local(address, &desc)) {
                info.vendor = desc.idVendor;
                info.product = desc.idProduct;
                info.cls = desc.bDeviceClass;
        }
        tuh_bus_info_t bus = {};
        if (tuh_bus_info_get(address, &bus)) {
                info.speed = speedOf(bus.speed);
                info.parent = bus.hub_addr;
                info.port = bus.hub_port;
        }
        d.present = true;
        ++_stats.added;
        return;
}

// Enumerated: what TinyUSB knows, then its strings asked for. Not called for hubs.
void UsbHost::mounted(uint8_t address) {
        Device *d = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                d = slot(address);
                if (d == nullptr) return;
                fill(*d, address);
        }
        bmdsInfo("USB: %u: %04x:%04x, %s speed", address, d->info.vendor, d->info.product, speedName(d->info.speed));
        (void)tuh_descriptor_get_manufacturer_string(address, EnglishUs, d->text, sizeof(d->text), stringDone, 0);
        return;
}

void UsbHost::unmounted(uint8_t address) {
        Mutex::Locker lock(_lock, WaitForever);
        Device       *d = slot(address);
        if (d == nullptr || !d->present) return;
        d->present = false;
        ++_stats.gone;
        bmdsInfo("USB: %u: gone", address);
        return;
}

// A string arrived (or failed): the manufacturer's, then the product's.
void UsbHost::stringArrived(uint8_t address, bool product, bool ok) {
        Device *d = nullptr;
        {
                Mutex::Locker lock(_lock, WaitForever);
                d = slot(address);
                if (d == nullptr || !d->present) return;
                if (ok) copyString(d->text, MaxName + 1, product ? d->info.name : d->info.manufacturer);
        }
        if (!product)
                (void)tuh_descriptor_get_product_string(address, EnglishUs, d->text, sizeof(d->text), stringDone, 1);
        else if (ok)
                detail::usbMidiNamed(address, StringView(d->info.name)); // its MIDI ports, if it has any
        return;
}

// A HID interface: a boot keyboard, or a mouse (boot or not), gets a slot and its reports. A mouse
// whose report descriptor gives its layout goes to report protocol (where its wheel is: boot
// protocol may leave it out), decoded by the layout (devplan/input.md §5a).
void UsbHost::hidMounted(uint8_t address, uint8_t instance, Span<const uint8_t> descriptor) {
        const uint8_t     protocol = tuh_hid_interface_protocol(address, instance);
        Hid::Kind         kind = protocol == HID_ITF_PROTOCOL_KEYBOARD ? Hid::Kind::Keyboard
                                 : protocol == HID_ITF_PROTOCOL_MOUSE  ? Hid::Kind::Mouse
                                                                       : Hid::Kind::None;
        const MouseLayout layout = kind == Hid::Kind::Keyboard ? MouseLayout() : MouseLayout::parse(descriptor);
        if (kind == Hid::Kind::None && layout.isValid()) kind = Hid::Kind::Mouse; // in report protocol already
        if (kind == Hid::Kind::None) return; // other report-protocol devices: later (devplan/input.md §8)
        Hid    *free = nullptr;
        uint8_t locks = 0;
        for (Hid &h : _hid) {
                if (h.kind == Hid::Kind::None && free == nullptr) free = &h;
                if (h.kind == Hid::Kind::Keyboard) locks = h.keys.locks();
        }
        if (free == nullptr) {
                bmdsWarn("USB: %u: HID %u: no room (%u keyboards and mice)", address, instance, unsigned(MaxHid));
                return;
        }
        *free = Hid();
        free->kind = kind;
        free->address = address;
        free->instance = instance;
        free->layout = layout;
        free->reportProtocol = kind == Hid::Kind::Mouse && protocol == HID_ITF_PROTOCOL_NONE;
        free->attachedMs = static_cast<uint32_t>(System::uptimeUs() / 1000);
        free->lastMs = free->attachedMs;
        {
                Mutex::Locker lock(_lock, WaitForever);
                ++(kind == Hid::Kind::Keyboard ? _stats.keyboards : _stats.mice);
        }
        if (kind == Hid::Kind::Keyboard) {
                bmdsInfo("USB: %u: keyboard", address);
        } else {
                bmdsInfo("USB: %u: mouse, %u buttons%s%s", address,
                         unsigned(layout.isValid() ? layout.buttonCount() : 3u), layout.hasWheel() ? ", a wheel" : "",
                         layout.isValid() ? "" : " (boot protocol)");
                // To report protocol; decoded as boot reports until the device says it switched.
                if (layout.isValid() && protocol == HID_ITF_PROTOCOL_MOUSE)
                        (void)tuh_hid_set_protocol(address, instance, HID_PROTOCOL_REPORT);
        }
        if (kind == Hid::Kind::Keyboard) {
                free->keys.setLocks(locks);
                free->leds = locks;
                (void)tuh_hid_set_report(address, instance, 0, HID_REPORT_TYPE_OUTPUT, &free->leds, 1);
        }
        arm(*free);
        return;
}

void UsbHost::hidProtocolSet(uint8_t address, uint8_t instance, uint8_t protocol) {
        Hid *h = hid(address, instance);
        if (h != nullptr && h->kind == Hid::Kind::Mouse)
                h->reportProtocol = protocol == HID_PROTOCOL_REPORT && h->layout.isValid();
        return;
}

void UsbHost::hidUnmounted(uint8_t address, uint8_t instance) {
        Hid *h = hid(address, instance);
        if (h == nullptr) return;
        if (h->kind == Hid::Kind::Keyboard) h->keys.releaseAll(emitKey, this);
        if (h->kind == Hid::Kind::Mouse) Input::pointer(PointerEvent()); // its buttons up
        {
                Mutex::Locker lock(_lock, WaitForever);
                --(h->kind == Hid::Kind::Keyboard ? _stats.keyboards : _stats.mice);
        }
        *h = Hid();
        return;
}

void UsbHost::hidReport(uint8_t address, uint8_t instance, const uint8_t *data, uint16_t length) {
        {
                Mutex::Locker lock(_lock, WaitForever);
                ++_stats.hidReports;
        }
        Hid *h = hid(address, instance);
        if (h != nullptr) {
                h->armed = false; // this transfer is done
                h->lastMs = static_cast<uint32_t>(System::uptimeUs() / 1000);
                ++h->reports;
        }
        if (h != nullptr && h->kind == Hid::Kind::Keyboard) {
                const auto now = static_cast<uint32_t>(System::uptimeUs() / 1000);
                // A boot report: modifiers, a reserved byte, then up to six keys down.
                if (now - h->attachedMs < Input::AttachKeyMs)
                        for (uint16_t i = 2; i < length && i < 8; ++i)
                                if (data[i] >= HidKey::A) Input::attachKey(data[i]); // below: none, or errors
                if (h->keys.report(Span<const uint8_t>(data, length), now, emitKey, this)) setLocks(h->keys.locks());
        } else if (h != nullptr && h->kind == Hid::Kind::Mouse && h->reportProtocol) {
                PointerEvent p;
                if (h->layout.decode(Span<const uint8_t>(data, length), p)) Input::pointer(p); // else another ID's
        } else if (h != nullptr && h->kind == Hid::Kind::Mouse && length >= 3) {
                // Boot mouse: buttons, x, y (signed), and a wheel byte on many.
                PointerEvent p;
                p.buttons = data[0];
                p.dx = static_cast<int8_t>(data[1]);
                p.dy = static_cast<int8_t>(data[2]);
                if (length >= 4) p.wheel = static_cast<int8_t>(data[3]);
                Input::pointer(p);
        }
        if (h != nullptr) {
                arm(*h);
        } else {
                (void)tuh_hid_receive_report(address, instance);
        }
        return;
}

BMDS_NAMESPACE_END

// TinyUSB's callbacks (weak in TinyUSB), on the host's task.
extern "C" {

bool tuh_enum_descriptor_configuration_cb(uint8_t daddr, uint8_t, const tusb_desc_configuration_t *desc_config) {
        bmds::UsbHost *host = bmds::detail::activeUsbHost();
        if (host != nullptr) host->configured(daddr, desc_config);
        return true;
}

void tuh_mount_cb(uint8_t daddr) {
        bmds::UsbHost *host = bmds::detail::activeUsbHost();
        if (host != nullptr) host->mounted(daddr);
        return;
}

void tuh_umount_cb(uint8_t daddr) {
        bmds::UsbHost *host = bmds::detail::activeUsbHost();
        if (host != nullptr) host->unmounted(daddr);
        return;
}

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *desc_report, uint16_t desc_len) {
        bmds::UsbHost *host = bmds::detail::activeUsbHost();
        if (host != nullptr) host->hidMounted(dev_addr, idx, bmds::Span<const uint8_t>(desc_report, desc_len));
        return;
}

void tuh_hid_set_protocol_complete_cb(uint8_t dev_addr, uint8_t idx, uint8_t protocol) {
        bmds::UsbHost *host = bmds::detail::activeUsbHost();
        if (host != nullptr) host->hidProtocolSet(dev_addr, idx, protocol);
        return;
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t idx) {
        bmds::UsbHost *host = bmds::detail::activeUsbHost();
        if (host != nullptr) host->hidUnmounted(dev_addr, idx);
        return;
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t idx, const uint8_t *report, uint16_t len) {
        bmds::UsbHost *host = bmds::detail::activeUsbHost();
        if (host != nullptr) host->hidReport(dev_addr, idx, report, len);
        return;
}

} // extern "C"
