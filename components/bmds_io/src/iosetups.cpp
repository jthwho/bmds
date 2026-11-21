/**
 * @file      iosetups.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What io/gpio and the ports share (devplan/io.md §10, §11).
 */

#include <cstdio>
#include <cstring>
#include <bmds/io.h>
#include <bmds/sysconf.h>

#include "iosetups.h"

BMDS_NAMESPACE_BEGIN

namespace IoSetups {

        IoResource instanceOf(const IoChip &chip, uint16_t signal, bool input) {
                const Span<const uint8_t> groups = input ? chip.inputGroups : chip.outputGroups;
                if (signal >= groups.size() || groups[signal] >= chip.groups.size()) return NoIoResource;
                const StringView group(chip.groups[groups[signal]]);
                // The group's name as a resource's (`UART1` → `uart1`), else with a 0 (`EMAC` → `emac0`).
                char   name[24];
                size_t n = 0;
                for (; n < group.size() && n + 2 < sizeof(name); ++n) {
                        const char c = group[n];
                        name[n] = c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
                }
                name[n] = '\0';
                auto [r, err] = chip.findResource(StringView(name, n));
                if (err.isOk()) return r;
                name[n] = '0';
                auto [r0, err0] = chip.findResource(StringView(name, n + 1));
                return err0.isOk() ? r0 : NoIoResource;
        }

        bool takenElsewhere(IoResource r, const char *self) {
                IoHolder h;
                if (Io::registry().holder(r, h).isError()) return false;
                if (h.isHeld()) return std::strcmp(h.owner, self) != 0;
                return h.resourceClass == IoClass::Fixed || h.resourceClass == IoClass::System;
        }

        Error refuseTaken(IoResource r, const char *signal) {
                char busy[160];
                char text[SysConf::ReasonBytes];
                (void)Io::busyText(r, busy, sizeof(busy));
                if (signal != nullptr) {
                        char name[24];
                        (void)ioChip().resourceName(r, name, sizeof(name));
                        std::snprintf(text, sizeof(text), "%s is %s's: %s Add force to route it anyway.", signal, name,
                                      busy);
                } else {
                        std::snprintf(
                                text, sizeof(text),
                                "%s Changing it will probably break that hardware: add force to change it anyway.",
                                busy);
                }
                SysConf::refuse(StringView(text));
                return Error::Busy;
        }

} // namespace IoSetups

BMDS_NAMESPACE_END
