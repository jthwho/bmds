/**
 * @file      chipserial.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * A serial number from the chip (devplan/notifications.md §2): its factory MAC address, burned
 * into eFuse block 1 by Espressif for every chip (the ESP32-P4 has the field for its Ethernet).
 * The optional 128-bit unique id is left alone: it may be unburned.
 */

#include <esp_mac.h>
#include <bmds/board.h>

BMDS_NAMESPACE_BEGIN

size_t chipMacSerial(char *out, size_t size) {
        if (out == nullptr || size < 13) {
                if (out != nullptr && size > 0) out[0] = '\0';
                return 0;
        }
        uint8_t mac[6] = {};
        if (esp_efuse_mac_get_default(mac) != ESP_OK) {
                out[0] = '\0';
                return 0;
        }
        static constexpr char Hex[] = "0123456789abcdef";
        for (size_t i = 0; i < 6; ++i) {
                out[2 * i] = Hex[mac[i] >> 4];
                out[2 * i + 1] = Hex[mac[i] & 0xf];
        }
        out[12] = '\0';
        return 12;
}

BMDS_NAMESPACE_END
