/**
 * @file      pmpset.h
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

BMDS_NAMESPACE_BEGIN

/**
 * @brief The PMP entries a U-mode process is granted (`devplan/protection.md` §4.1), encoded as
 *        the RISC-V privileged spec gives them, ready for the trampoline to load.
 *
 * Regions are added in order, each one NAPOT entry when it is a power of two aligned to its
 * size, else a TOR pair (one entry when it starts where the previous TOR region ended). All are
 * unlocked: they bind U-mode only, and an address no entry matches is denied to it. Every
 * boundary must be a whole number of `Granule`s: the hardware's granularity, so a boundary that
 * isn't would grant what lies next to it.
 *
 * Portable: no CSRs (the kernel's trampoline loads them), no heap. `stamp` and the arrays come
 * first in this layout; `usermode.S` reads them at fixed offsets.
 *
 * @par Thread Safety
 * Not thread-safe: its owner's lock.
 */
struct PmpSet {
                /** @brief The PMP's entries. */
                static constexpr unsigned Entries = 16;

                /** @brief The hardware's granularity (the P4's: measured, protection.md §12). */
                static constexpr uint32_t Granule = 128;

                /** @brief A region's rights (a config byte's R, W and X bits). */
                enum Access : uint8_t {
                        Read = 0x1,
                        Write = 0x2,
                        Exec = 0x4,
                };

                /** @brief An entry's address-matching mode (a config byte's A field). */
                enum Mode : uint8_t {
                        Off = 0,
                        Tor = 1,
                        Na4 = 2,
                        Napot = 3,
                };

                uint32_t stamp = 0;             ///< The owner's: which version is loaded where.
                uint32_t cfg[Entries / 4] = {}; ///< `pmpcfg0`-`pmpcfg3`: a byte per entry.
                uint32_t addr[Entries] = {};    ///< `pmpaddr0`-`pmpaddr15`: the address >> 2.
                uint8_t  used = 0;              ///< Entries taken, from 0.

                /** @brief No regions (every entry off). `stamp` is kept. */
                void clear();

                /**
                 * @brief Grants [@p base, @p base + @p bytes) with @p access (`Access` bits) in the
                 *        next free entries.
                 * @return `Error::Invalid` for an empty region, one past 4 GB, or a boundary that
                 *         isn't a multiple of `Granule`; `Error::Full` if its entries don't fit
                 *         (nothing changed).
                 */
                Error add(uintptr_t base, size_t bytes, uint8_t access);

                /** @brief The entries [@p base, @p base + @p bytes) takes, added now. */
                unsigned entriesFor(uintptr_t base, size_t bytes) const;

                /** @brief Entry @p i's config byte. */
                uint8_t config(unsigned i) const { return uint8_t(cfg[i / 4] >> (8 * (i % 4))); }

                /**
                 * @brief True if a U-mode access of @p bytes at @p address with @p access is allowed:
                 *        the lowest entry that matches decides, and it must match all of it (the
                 *        spec's rule; the host tests' model of the hardware).
                 */
                bool permits(uintptr_t address, size_t bytes, uint8_t access) const;

        private:
                void set(unsigned i, uint32_t address, uint8_t config);
                bool isNapot(uintptr_t base, size_t bytes) const;
};

BMDS_NAMESPACE_END
