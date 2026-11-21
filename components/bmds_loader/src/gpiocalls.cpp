/**
 * @file      gpiocalls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `gpio_ctl` (slot 139, devplan/io.md §3.9): `/dev/gpiochip0`'s operations. Linux's GPIO
 * character device is translated onto it by the SDK's `linux.c`; the pin and connector
 * operations say what Linux has no words for. Reading takes nothing: every answer is `Io`'s.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/board.h>
#include <bmds/errno.h>
#include <bmds/gpiochip.h>
#include <bmds/io.h>
#include <bmds/memory.h>
#include <bmds/process.h>
#include <bmds/sys.h>
#include "running.h"
#include "structcopy.h"

using bmds::BoardConnector;
using bmds::IoClass;
using bmds::IoPinDetail;
using bmds::IoPinField;
using bmds::IoPinRow;
using bmds::KernelCall;
using bmds::MemSpace;
using bmds::Span;
using bmds::StringView;
using bmds::detail::copyIn;
using bmds::detail::copyOut;
using bmds::detail::copyText;

namespace {

        static_assert(uint8_t(IoClass::Free) == BMDS_GPIO_CLASS_FREE &&
                              uint8_t(IoClass::Fixed) == BMDS_GPIO_CLASS_FIXED &&
                              uint8_t(IoClass::System) == BMDS_GPIO_CLASS_SYSTEM &&
                              uint8_t(IoClass::User) == BMDS_GPIO_CLASS_USER,
                      "IoClass is the ABI's BMDS_GPIO_CLASS_...");

        // The most a pin array may say each pin is (a newer SDK's bigger record is cut to ours).
        constexpr uint32_t MaxPinSize = 1024;

        uint32_t pinFlags(const IoPinRow &r) {
                const bmds::IoPadState &s = r.state;
                uint32_t                f = 0;
                if (s.inputEnabled) f |= BMDS_GPIO_PIN_INPUT;
                if (s.outputEnabled) f |= BMDS_GPIO_PIN_OUTPUT;
                if (bmds::ioPeripheralDriven(bmds::ioChip(), r.pad, s)) f |= BMDS_GPIO_PIN_PERIPHERAL;
                if (s.openDrain) f |= BMDS_GPIO_PIN_OPEN_DRAIN;
                if (s.pullUp) f |= BMDS_GPIO_PIN_PULL_UP;
                if (s.pullDown) f |= BMDS_GPIO_PIN_PULL_DOWN;
                if (s.lpIo) f |= BMDS_GPIO_PIN_LP_IO;
                if (r.strapping) f |= BMDS_GPIO_PIN_STRAPPING;
                if (r.forced) f |= BMDS_GPIO_PIN_FORCED;
                if (r.watched) f |= BMDS_GPIO_PIN_EDGES;
                return f;
        }

        void toAbi(const IoPinRow &r, bmds_gpio_pin_t &out) {
                out = bmds_gpio_pin_t{};
                out.offset = r.pad;
                out.flags = pinFlags(r);
                out.pin_class = uint8_t(r.resourceClass);
                out.level = r.level;
                out.function = r.state.function;
                out.drive = r.state.drive;
                copyText(r.name, out.name, sizeof(out.name));
                copyText(r.owner, out.owner, sizeof(out.owner));
                copyText(r.function, out.use, sizeof(out.use));
                copyText(r.route, out.route, sizeof(out.route));
                copyText(r.direction, out.direction, sizeof(out.direction));
                copyText(r.pull, out.pull, sizeof(out.pull));
                copyText(r.where, out.where, sizeof(out.where));
                return;
        }

        // One pad's row, read alone.
        bool readRow(uint32_t offset, IoPinRow &out) {
                return bmds::Io::readPins(offset, Span<IoPinRow>(&out, 1)) == 1;
        }

        int32_t chipInfo(bmds::GpioChip &chip, bmds_gpio_chip_info_t *arg) {
                bmds_gpio_chip_info_t c;
                if (const int32_t r = copyIn(arg, c); r < 0) return r;
                c.lines = uint32_t(bmds::ioChip().padCount());
                c.connectors = uint32_t(bmds::board().connectors.size());
                c.index = chip.index();
                char name[BMDS_GPIO_NAME_MAX];
                std::snprintf(name, sizeof(name), "gpiochip%u", unsigned(chip.index()));
                copyText(name, c.name, sizeof(c.name));
                copyText(bmds::ioChip().name, c.label, sizeof(c.label));
                copyText(bmds::board().name, c.board, sizeof(c.board));
                return copyOut(c, arg);
        }

        int32_t lineInfo(bmds_gpio_line_info_t *arg) {
                bmds_gpio_line_info_t l;
                if (const int32_t r = copyIn(arg, l); r < 0) return r;
                if (l.offset >= bmds::ioChip().padCount()) return -BMDS_EINVAL;
                IoPinRow row;
                if (!readRow(l.offset, row)) return -BMDS_EINVAL;
                const bmds::IoPadState &s = row.state;
                uint64_t                f = 0;
                if (row.resourceClass != IoClass::Free) f |= BMDS_GPIO_LINE_USED;
                // Linux's line is one or the other: an output reads back too.
                if (s.outputEnabled)
                        f |= BMDS_GPIO_LINE_OUTPUT;
                else if (s.inputEnabled)
                        f |= BMDS_GPIO_LINE_INPUT;
                if (s.openDrain) f |= BMDS_GPIO_LINE_OPEN_DRAIN;
                if (s.pullUp) f |= BMDS_GPIO_LINE_BIAS_PULL_UP;
                if (s.pullDown) f |= BMDS_GPIO_LINE_BIAS_PULL_DOWN;
                if (!s.pullUp && !s.pullDown) f |= BMDS_GPIO_LINE_BIAS_DISABLED;
                l.flags = f;
                l.debounce_us = 0;
                l.reserved = 0;
                // Linux's line names are the board's (its device tree's gpio-line-names).
                const bmds::IoPinBoard board = bmds::Io::boardOf(l.offset);
                copyText(board.label[0] != '\0' ? StringView(board.label) : StringView(row.name), l.name,
                         sizeof(l.name));
                copyText(row.owner, l.consumer, sizeof(l.consumer));
                return copyOut(l, arg);
        }

        int32_t pins(bmds_gpio_pins_t *arg) {
                bmds_gpio_pins_t p;
                if (const int32_t r = copyIn(arg, p); r < 0) return r;
                if (p.size < sizeof(bmds_gpio_pins_t) || p.pin_size < sizeof(uint32_t) || p.pin_size > MaxPinSize)
                        return -BMDS_EINVAL;
                const uint32_t lines = uint32_t(bmds::ioChip().padCount());
                if (p.first > lines) return -BMDS_EINVAL;
                const uint32_t n = p.count < lines - p.first ? p.count : lines - p.first;
                if (n == 0) return 0;
                if (!bmds::detail::canWrite(p.pins, size_t(n) * p.pin_size)) return -BMDS_EFAULT;
                // In PSRAM: every pad's row (~170 bytes each) is more than a call's stack wants.
                auto *rows = static_cast<IoPinRow *>(bmds::Memory::alloc(MemSpace::Psram, n * sizeof(IoPinRow)));
                if (rows == nullptr) return -BMDS_ENOMEM;
                for (uint32_t i = 0; i < n; ++i) ::new (&rows[i]) IoPinRow();
                const size_t got = bmds::Io::readPins(p.first, Span<IoPinRow>(rows, n));
                const size_t keep = p.pin_size < sizeof(bmds_gpio_pin_t) ? p.pin_size : sizeof(bmds_gpio_pin_t);
                auto        *out = reinterpret_cast<uint8_t *>(p.pins);
                for (size_t i = 0; i < got; ++i) {
                        bmds_gpio_pin_t pin;
                        toAbi(rows[i], pin);
                        uint8_t *at = out + i * p.pin_size;
                        std::memcpy(at, &pin, keep);
                        if (p.pin_size > keep) std::memset(at + keep, 0, p.pin_size - keep);
                }
                bmds::Memory::free(rows);
                return int32_t(got);
        }

        int32_t pinDetail(bmds_gpio_pin_detail_t *arg) {
                // In place: the header read, then as many lines as its size has room for.
                if (arg == nullptr || !bmds::detail::canRead(arg, offsetof(bmds_gpio_pin_detail_t, lines)))
                        return -BMDS_EFAULT;
                const uint32_t size = arg->size;
                const uint32_t offset = arg->offset;
                const uint32_t header = offsetof(bmds_gpio_pin_detail_t, lines);
                if (size < header) return -BMDS_EINVAL;
                if (offset >= bmds::ioChip().padCount()) return -BMDS_EINVAL;
                const uint32_t room = (size - header) / sizeof(bmds_gpio_detail_line_t);
                const uint32_t want = room < BMDS_GPIO_DETAIL_MAX ? room : BMDS_GPIO_DETAIL_MAX;
                if (!bmds::detail::canWrite(arg, header + want * sizeof(bmds_gpio_detail_line_t))) return -BMDS_EFAULT;
                auto *d = static_cast<IoPinDetail *>(bmds::Memory::alloc(MemSpace::Psram, sizeof(IoPinDetail)));
                if (d == nullptr) return -BMDS_ENOMEM;
                ::new (d) IoPinDetail();
                const bmds::Error err = bmds::Io::readPin(offset, *d);
                uint32_t          count = 0;
                if (err.isOk()) {
                        for (size_t f = 0; f < bmds::IoPinFieldCount && count < want; ++f) {
                                const auto field = static_cast<IoPinField>(f);
                                // Note, Forced and Edges are given only when there is one (as `io pin`).
                                if (bmds::ioPinFieldIsOptional(field) && d->at(field)[0] == '\0') continue;
                                bmds_gpio_detail_line_t line{};
                                copyText(bmds::ioPinFieldName(field), line.label, sizeof(line.label));
                                copyText(d->at(field), line.text, sizeof(line.text));
                                std::memcpy(&arg->lines[count++], &line, sizeof(line));
                        }
                }
                bmds::Memory::free(d);
                if (err.isError()) return -BMDS_EINVAL;
                arg->count = count;
                arg->reserved = 0;
                return 0;
        }

        int32_t connector(bmds_gpio_connector_t *arg) {
                bmds_gpio_connector_t c;
                if (const int32_t r = copyIn(arg, c); r < 0) return r;
                const Span<const BoardConnector> all = bmds::board().connectors;
                if (c.index >= all.size()) return -BMDS_EINVAL;
                const BoardConnector &b = all[c.index];
                c.pins = uint32_t(b.pins.size());
                c.columns = b.columns > 0 ? b.columns : 1;
                c.flags = b.fitted ? BMDS_GPIO_CONNECTOR_FITTED : 0;
                c.reserved = 0;
                copyText(b.name, c.name, sizeof(c.name));
                copyText(b.description, c.description, sizeof(c.description));
                return copyOut(c, arg);
        }

        int32_t connectorPin(bmds_gpio_connector_pin_t *arg) {
                bmds_gpio_connector_pin_t p;
                if (const int32_t r = copyIn(arg, p); r < 0) return r;
                const Span<const BoardConnector> all = bmds::board().connectors;
                if (p.connector >= all.size() || p.pin >= all[p.connector].pins.size()) return -BMDS_EINVAL;
                const bmds::BoardConnectorPin &b = all[p.connector].pins[p.pin];
                p.gpio = b.gpio >= 0 ? int32_t(b.gpio) : -1;
                copyText(b.label, p.label, sizeof(p.label));
                return copyOut(p, arg);
        }

        int32_t edges(bmds_gpio_edges_t *arg) {
                bmds_gpio_edges_t e;
                if (const int32_t r = copyIn(arg, e); r < 0) return r;
                if (e.size < sizeof(bmds_gpio_edges_t) || e.edge_size < sizeof(int64_t) || e.edge_size > MaxPinSize)
                        return -BMDS_EINVAL;
                if (e.offset >= bmds::ioChip().padCount()) return -BMDS_EINVAL;
                // A batch at a time on the stack (16 × 16 bytes), into the caller's array.
                constexpr size_t Batch = 16;
                bmds::Io::Edge   got[Batch];
                const size_t     keep = e.edge_size < sizeof(bmds_gpio_edge_t) ? e.edge_size : sizeof(bmds_gpio_edge_t);
                auto            *out = reinterpret_cast<uint8_t *>(e.edges);
                if (e.count > 0 && !bmds::detail::canWrite(e.edges, size_t(e.count) * e.edge_size)) return -BMDS_EFAULT;
                uint32_t after = e.after, lostAll = 0;
                size_t   total = 0;
                while (total < e.count) {
                        const size_t room = e.count - total < Batch ? e.count - total : Batch;
                        uint32_t     next = after, lost = 0;
                        auto [n, err] =
                                bmds::Io::readEdges(e.offset, after, Span<bmds::Io::Edge>(got, room), next, lost);
                        if (err.isError()) return -BMDS_ENOENT;
                        lostAll += lost;
                        for (size_t i = 0; i < n; ++i) {
                                bmds_gpio_edge_t edge{};
                                edge.time_ns = got[i].timeNs;
                                edge.seq = got[i].seq;
                                edge.rising = got[i].rising ? 1 : 0;
                                edge.level = got[i].level ? 1 : 0;
                                uint8_t *at = out + (total + i) * e.edge_size;
                                std::memcpy(at, &edge, keep);
                                if (e.edge_size > keep) std::memset(at + keep, 0, e.edge_size - keep);
                        }
                        total += n;
                        after = next;
                        if (n < room) break; // none newer
                }
                if (e.count == 0) {
                        // Room for none: whether the pad is watched, and where its numbers are.
                        uint32_t next = after, lost = 0;
                        auto [n, err] = bmds::Io::readEdges(e.offset, after, Span<bmds::Io::Edge>(), next, lost);
                        (void)n;
                        if (err.isError()) return -BMDS_ENOENT;
                }
                e.next = after;
                e.lost = lostAll;
                if (const int32_t r = copyOut(e, arg); r < 0) return r;
                return int32_t(total);
        }

} // namespace

extern "C" int32_t bmds_k_gpio_ctl(int32_t fd, uint32_t op, void *arg) {
        KernelCall                  call;
        const bmds::FileHandle::Ptr handle = call->handle(fd);
        if (!handle) return -BMDS_EBADF;
        bmds::GpioChip *chip = handle->stream().gpioChip();
        if (chip == nullptr) return -BMDS_ENOTTY;
        if (!bmds::Io::isReady()) return -BMDS_ENODEV;
        if (arg == nullptr) return -BMDS_EFAULT;
        switch (op) {
                case BMDS_GPIO_CHIP_INFO: return chipInfo(*chip, static_cast<bmds_gpio_chip_info_t *>(arg));
                case BMDS_GPIO_LINE_INFO: return lineInfo(static_cast<bmds_gpio_line_info_t *>(arg));
                case BMDS_GPIO_PINS: return pins(static_cast<bmds_gpio_pins_t *>(arg));
                case BMDS_GPIO_PIN_DETAIL: return pinDetail(static_cast<bmds_gpio_pin_detail_t *>(arg));
                case BMDS_GPIO_CONNECTOR: return connector(static_cast<bmds_gpio_connector_t *>(arg));
                case BMDS_GPIO_CONNECTOR_PIN: return connectorPin(static_cast<bmds_gpio_connector_pin_t *>(arg));
                case BMDS_GPIO_EDGES: return edges(static_cast<bmds_gpio_edges_t *>(arg));
                default: return -BMDS_EINVAL;
        }
}
