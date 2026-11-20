/**
 * @file      io.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's I/O registry and pad reads (devplan/io.md §3.1-§3.4).
 */

#include <cstdio>
#include <bmds/board.h>
#include <bmds/io.h>
#include <bmds/log.h>
#include <bmds/psramobject.h>

#include "iochipops.h"

BMDS_NAMESPACE_BEGIN

namespace {

        // Claims are made in task context and never from a cache-disabled path: PSRAM is fine.
        constinit PsramObject<IoRegistry> theRegistry;

        IoBoardUse boardUse(BoardPinUse use) {
                switch (use) {
                        case BoardPinUse::Fixed: return IoBoardUse::Fixed;
                        case BoardPinUse::Device: return IoBoardUse::Device;
                        case BoardPinUse::Free: return IoBoardUse::Free;
                }
                return IoBoardUse::None;
        }

} // namespace

namespace Io {

        Error init() {
                const IoChip &chip = ioChip();
                if (chip.resourceCount() > IoRegistry::MaxResources) {
                        bmdsErr("io: the %s has %u resources, more than %u", chip.name,
                                static_cast<unsigned>(chip.resourceCount()),
                                static_cast<unsigned>(IoRegistry::MaxResources));
                        return Error::Full;
                }
                IoRegistry *r = theRegistry.create(chip);
                if (r == nullptr) return Error::NoMem;
                for (const BoardPin &p : board().pins) {
                        if (r->setBoardUse(pad(p.gpio), boardUse(p.use)).isError())
                                bmdsWarn("io: the board lists GPIO%d, which the %s hasn't", p.gpio, chip.name);
                }
                return Error::Ok;
        }

        bool isReady() {
                return theRegistry.isCreated();
        }

        IoRegistry &registry() {
                return *theRegistry;
        }

        IoResource instance(IoKind kind, unsigned number) {
                auto [r, err] = ioChip().resource(kind, number);
                return err.isOk() ? r : NoIoResource;
        }

        Result<IoClaim> claim(StringView owner, Span<const IoUse> uses, unsigned flags, IoResource *blocked) {
                if (!isReady()) return makeError<IoClaim>(Error::NotReady);
                // Leave out what the board hasn't (a GpioLine without a GPIO).
                constexpr size_t MaxUses = 32; // Ethernet's 13 are the most so far
                IoUse            kept[MaxUses];
                size_t           n = 0;
                for (const IoUse &u : uses) {
                        if (u.resource == NoIoResource) continue;
                        if (n == MaxUses) return makeError<IoClaim>(Error::Invalid);
                        kept[n++] = u;
                }
                return theRegistry->claim(owner, IoOwnerKind::System, Span<const IoUse>(kept, n), flags, blocked);
        }

        size_t busyText(IoResource resource, char *out, size_t size) {
                if (out == nullptr || size == 0) return 0;
                out[0] = '\0';
                if (!isReady()) return 0;
                char name[24];
                (void)ioChip().resourceName(resource, name, sizeof(name));
                IoHolder h;
                if (theRegistry->holder(resource, h).isError()) {
                        const int n = std::snprintf(out, size, "%s doesn't exist", name);
                        return n < 0 ? 0 : (static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1);
                }
                const bool  isPad = resource < ioChip().padCount();
                const char *what = isPad ? "pin" : "resource";
                int         n = 0;
                if (h.isHeld() && h.role[0] != '\0') {
                        n = std::snprintf(out, size, "%s is a %s %s: %s uses it as %s.", name,
                                          ioClassName(h.resourceClass), what, h.owner, h.role);
                } else if (h.isHeld()) {
                        n = std::snprintf(out, size, "%s is a %s %s: %s uses it.", name, ioClassName(h.resourceClass),
                                          what, h.owner);
                } else if (isPad) {
                        const char *label = "";
                        for (const BoardPin &p : board().pins) {
                                if (pad(p.gpio) == resource) label = p.label;
                        }
                        n = std::snprintf(out, size, "%s is a %s pin: the board wires it to %s.", name,
                                          ioClassName(h.resourceClass), label[0] != '\0' ? label : "a device");
                } else {
                        n = std::snprintf(out, size, "%s is free.", name);
                }
                return n < 0 ? 0 : (static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1);
        }

        Error readPad(unsigned pad, IoPadState &out) {
                out = IoPadState();
                if (pad >= ioChip().padCount()) return Error::Invalid;
                IoChipOps::readPad(pad, out);
                return Error::Ok;
        }

        void readInputSources(Span<int16_t> out) {
                const size_t n = ioChip().inputSignals.size();
                for (size_t i = 0; i < out.size() && i < n; ++i)
                        out[i] =
                                ioChip().inputName(i) != nullptr ? static_cast<int16_t>(IoChipOps::inputSource(i)) : -1;
                return;
        }

} // namespace Io

BMDS_NAMESPACE_END
