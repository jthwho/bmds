/**
 * @file      pmpset.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/pmpset.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr uint8_t ModeShift = 3;
        constexpr uint8_t ModeMask = 0x18;
        constexpr uint8_t AccessMask = PmpSet::Read | PmpSet::Write | PmpSet::Exec;

        PmpSet::Mode modeOf(uint8_t config) {
                return PmpSet::Mode((config & ModeMask) >> ModeShift);
        }

} // namespace

void PmpSet::clear() {
        for (uint32_t &c : cfg) c = 0;
        for (uint32_t &a : addr) a = 0;
        used = 0;
        return;
}

void PmpSet::set(unsigned i, uint32_t address, uint8_t config) {
        addr[i] = address;
        const unsigned shift = 8 * (i % 4);
        cfg[i / 4] = (cfg[i / 4] & ~(0xffu << shift)) | (uint32_t(config) << shift);
        return;
}

bool PmpSet::isNapot(uintptr_t base, size_t bytes) const {
        return bytes >= Granule && (bytes & (bytes - 1)) == 0 && (base & (bytes - 1)) == 0;
}

unsigned PmpSet::entriesFor(uintptr_t base, size_t bytes) const {
        if (isNapot(base, bytes)) return 1;
        // A TOR entry's bottom is the entry before's address: a TOR region (or nothing, for
        // entry 0 at 0) that ends where this one starts gives it for free.
        if (used == 0) return base == 0 ? 1 : 2;
        const uint8_t prev = config(used - 1u);
        const bool    torBelow = modeOf(prev) == Tor || modeOf(prev) == Off;
        return torBelow && addr[used - 1u] == uint32_t(base >> 2) ? 1 : 2;
}

Error PmpSet::add(uintptr_t base, size_t bytes, uint8_t access) {
        if (bytes == 0 || base % Granule != 0 || bytes % Granule != 0) return Error::Invalid;
        if (uint64_t(base) + bytes > (uint64_t(1) << 32)) return Error::Invalid; // past 4 GB
        const unsigned need = entriesFor(base, bytes);
        if (used + need > Entries) return Error::Full;
        const uint8_t rights = uint8_t(access & AccessMask);
        if (isNapot(base, bytes)) {
                set(used++, uint32_t((base | (bytes / 2 - 1)) >> 2), uint8_t(rights | (Napot << ModeShift)));
        } else {
                if (need == 2) set(used++, uint32_t(base >> 2), uint8_t(Off << ModeShift));
                // The top is exclusive; 4 GB itself can't be written (>> 2 keeps it in 32 bits).
                set(used++, uint32_t((uint64_t(base) + bytes) >> 2), uint8_t(rights | (Tor << ModeShift)));
        }
        return Error::Ok;
}

bool PmpSet::bounds(unsigned i, uint64_t &lo, uint64_t &hi) const {
        switch (modeOf(config(i))) {
                case Off: return false;
                case Tor:
                        lo = i == 0 ? 0 : uint64_t(addr[i - 1]) << 2;
                        hi = uint64_t(addr[i]) << 2;
                        return true;
                case Na4:
                        lo = uint64_t(addr[i]) << 2;
                        hi = lo + 4;
                        return true;
                case Napot: {
                        // Trailing ones: n of them make a region of 2^(n+3) bytes.
                        const uint32_t a = addr[i];
                        const uint32_t ones = uint32_t(__builtin_ctz(~a));
                        const uint64_t size = uint64_t(8) << ones;
                        lo = (uint64_t(a) << 2) & ~(size - 1);
                        hi = lo + size;
                        return true;
                }
        }
        return false;
}

size_t PmpSet::extent(uintptr_t address, uint8_t access) const {
        for (unsigned i = 0; i < used; ++i) {
                uint64_t lo = 0, hi = 0;
                if (!bounds(i, lo, hi) || address < lo || address >= hi) continue;
                if ((config(i) & access & AccessMask) != (access & AccessMask)) return 0;
                const uint64_t n = hi - address;
                return n > SIZE_MAX ? SIZE_MAX : size_t(n);
        }
        return 0;
}

bool PmpSet::allows(uintptr_t address, size_t bytes, uint8_t access) const {
        if (bytes == 0) return true;
        if (uint64_t(address) + bytes > (uint64_t(1) << 32)) return false;
        while (bytes > 0) {
                const size_t n = extent(address, access);
                if (n == 0) return false;
                if (n >= bytes) return true;
                address += n;
                bytes -= n;
        }
        return true;
}

bool PmpSet::permits(uintptr_t address, size_t bytes, uint8_t access) const {
        if (bytes == 0) return false;
        const uint64_t first = address;
        const uint64_t last = uint64_t(address) + bytes - 1;
        for (unsigned i = 0; i < used; ++i) {
                const uint8_t c = config(i);
                uint64_t      lo = 0, hi = 0; // [lo, hi)
                if (!bounds(i, lo, hi)) continue;
                const bool firstIn = first >= lo && first < hi;
                const bool lastIn = last >= lo && last < hi;
                if (!firstIn && !lastIn) continue;
                // Matching part of the access only fails it (the spec).
                if (!firstIn || !lastIn) return false;
                return (c & access & AccessMask) == (access & AccessMask);
        }
        return false;
}

BMDS_NAMESPACE_END
