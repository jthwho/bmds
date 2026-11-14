/**
 * @file      regionprotect.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The ESP32-P4's PMA and PMP, set by BMDS (devplan/protection.md §4.1). ESP-IDF's own setup
 * (cpu_region_protect.c) locks 14 PMP entries with rights that would apply to U-mode as well,
 * so it is turned off (CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n, which gates it in the
 * bootloader and the app alike) and this runs instead, on each core:
 *
 * - The PMA, which binds every privilege mode, gets IDF's holes (minus two it can no longer fit)
 *   and the kernel's W^X that IDF's MEMPROT kept in the PMP: IRAM text RX and the rest of
 *   internal RAM RW, flash text RX and the rest of the flash window R. Every entry is locked.
 * - The PMP is cleared and left unlocked: M-mode is unrestricted by an empty PMP and U-mode is
 *   denied everything, until the loader grants a program its memory.
 *
 * Entries match lowest number first. TOR entries chain: each starts where the one before ends.
 *
 * A bootloader built before this (region protection on) locks IDF's layout before the app runs:
 * then IDF's own setup runs instead, as it always did, and the record says so (`idfLayout`). Only
 * an update that carries the bootloader (devplan/update.md §20) changes that.
 */

#include <sdkconfig.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <esp_cpu.h>
#include <esp_private/startup_internal.h>
#include <riscv/csr.h>
#include <soc/ext_mem_defs.h>
#include <soc/soc.h>
#include <bmds/regionprotect.h>

#if !CONFIG_IDF_TARGET_ESP32P4
#error "regionprotect.cpp describes the ESP32-P4's address space"
#endif

#if CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE
#error "BMDS sets the PMP and PMA itself: turn CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE off"
#endif

#if !CONFIG_ESP_SYSTEM_MEMPROT
#error "CONFIG_ESP_SYSTEM_MEMPROT aligns _iram_text_end for the PMA's IRAM split: keep it on"
#endif

// From IDF's linker script: the end of code in internal RAM (aligned to the PMP's 128 bytes
// because MEMPROT is on) and the end of code in the flash window.
extern "C" int _iram_text_end;
extern "C" int _instruction_reserved_end;

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned Entries = 16;
        constexpr unsigned Cores = 2;

        constexpr uint32_t None = PMA_L | PMA_EN;
        constexpr uint32_t Rd = None | PMA_R;
        constexpr uint32_t Rx = None | PMA_R | PMA_X;
        constexpr uint32_t Rw = None | PMA_R | PMA_W;
        constexpr uint32_t Rwx = None | PMA_R | PMA_W | PMA_X;

        constexpr uint32_t PmpLock = 0x80;

        /** @brief A CSR read or write by its number, which must be a constant. */
        template <uint32_t Csr> uint32_t csrRead() {
                uint32_t v;
                __asm__ volatile("csrr %0, %1" : "=r"(v) : "i"(Csr));
                return v;
        }

        template <uint32_t Csr> void csrWrite(uint32_t v) {
                __asm__ volatile("csrw %0, %1" : : "i"(Csr), "r"(v) : "memory");
                return;
        }

        using Reader = uint32_t (*)();
        using Writer = void (*)(uint32_t);

        template <uint32_t Base, size_t... I>
        constexpr std::array<Reader, sizeof...(I)> readers(std::index_sequence<I...>) {
                return {&csrRead<Base + I>...};
        }

        template <uint32_t Base, size_t... I>
        constexpr std::array<Writer, sizeof...(I)> writers(std::index_sequence<I...>) {
                return {&csrWrite<Base + I>...};
        }

        // Per-entry accessors by run-time index, in rodata.
        constexpr auto PmaCfgRead = readers<CSR_PMACFG0>(std::make_index_sequence<Entries>());
        constexpr auto PmaCfgWrite = writers<CSR_PMACFG0>(std::make_index_sequence<Entries>());
        constexpr auto PmaAddrRead = readers<CSR_PMAADDR0>(std::make_index_sequence<Entries>());
        constexpr auto PmaAddrWrite = writers<CSR_PMAADDR0>(std::make_index_sequence<Entries>());
        constexpr auto PmpCfgRead = readers<CSR_PMPCFG0>(std::make_index_sequence<Entries / 4>());
        constexpr auto PmpCfgWrite = writers<CSR_PMPCFG0>(std::make_index_sequence<Entries / 4>());

        struct Entry {
                        uint32_t addr; ///< The address register's value (byte address >> 2, or NAPOT's form).
                        uint32_t cfg;
        };

        constexpr uint32_t tor(uint32_t top) {
                return top >> 2;
        }

        constexpr uint32_t napot(uint32_t base, uint32_t size) {
                return (base | (size / 2 - 1)) >> 2;
        }

        constexpr uint32_t roundUp(uint32_t v, uint32_t to) {
                return (v + to - 1) & ~(to - 1);
        }

        constinit RegionProtection records[Cores];

        // The layout's indices that split code from data: their tops are the only boundaries that
        // aren't at a large power of two, and a top that moved would fault the kernel.
        constexpr unsigned FlashTextEntry = 5;
        constexpr unsigned IramTextEntry = 10;

        /**
         * Writes the PMA in address order, each entry reset, its address written and read back
         * before its rights are set. The cacheable range (13) is written before 14 and 15 are reset:
         * the ROM keeps that range cacheable through entry 15 until then (as IDF's setup orders it).
         *
         * A split's top that doesn't read back exactly (a granularity coarser than the linker's
         * alignment) would move the boundary into code or data: the entry after it then gets both
         * sides' rights instead, and the record says that split is missing. Never rounded: rounding
         * the IRAM split up made the start of .data read-only and faulted the first store there.
         */
        void setPma(RegionProtection &record) {
                const uint32_t iramEnd = uint32_t(reinterpret_cast<uintptr_t>(&_iram_text_end));
                const uint32_t flashTextEnd =
                        roundUp(uint32_t(reinterpret_cast<uintptr_t>(&_instruction_reserved_end)), SOC_MMU_PAGE_SIZE);

                // Dropped from IDF's holes for want of entries: TCM to the CPU peripherals
                // (0x30102000-0x3ff00000) and PSRAM to the ROM (0x4c000000-0x4fc00000).
                const Entry layout[Entries] = {
                        {napot(0, SOC_CPU_SUBSYSTEM_LOW), PMA_NAPOT | None}, // 0: null and below
                        {tor(SOC_CPU_SUBSYSTEM_HIGH), None},                 // 1: base
                        {tor(SOC_TCM_LOW), PMA_TOR | None},                  // 2: CPU subsystem to TCM
                        {tor(CPU_PERIPH_HIGH), None},                        // 3: base
                        {tor(SOC_IROM_LOW), PMA_TOR | None},                 // 4: CPU peripherals to flash
                        {tor(flashTextEnd), PMA_TOR | Rx},                   // 5: flash text
                        {tor(SOC_DROM_HIGH), PMA_TOR | Rd},                  // 6: rodata, mapped partitions
                        {tor(SOC_EXTRAM_LOW), PMA_TOR | None},               // 7: flash window to PSRAM
                        {tor(SOC_IROM_MASK_HIGH), None},                     // 8: base
                        {tor(SOC_IRAM_LOW), PMA_TOR | None},                 // 9: ROM to internal RAM
                        {tor(iramEnd), PMA_TOR | Rx},                        // 10: IRAM text
                        {tor(SOC_DRAM_HIGH), PMA_TOR | Rw},                  // 11: the rest of internal RAM
                        {tor(SOC_PERIPHERAL_LOW), PMA_TOR | None},           // 12: internal RAM to peripherals
                        {napot(SOC_IROM_LOW, SOC_PERIPHERAL_LOW - SOC_IROM_LOW), PMA_NAPOT | Rwx}, // 13: cacheable
                        {tor(uint32_t(SOC_DRAM_HIGH) + uint32_t(SOC_NON_CACHEABLE_OFFSET)), None}, // 14: base
                        {tor(UINT32_MAX), PMA_TOR | None}, // 15: uncached internal RAM to the top
                };

                bool     flashSplit = true, iramSplit = true;
                uint16_t mismatch = 0;
                for (unsigned i = 0; i < Entries; ++i) {
                        uint32_t cfg = layout[i].cfg;
                        if (i == FlashTextEntry + 1 && !flashSplit) cfg |= PMA_X;
                        if (i == IramTextEntry + 1 && !iramSplit) cfg |= PMA_X;
                        PmaCfgWrite[i](0);
                        PmaAddrWrite[i](layout[i].addr);
                        const uint32_t back = PmaAddrRead[i]();
                        if (i == FlashTextEntry) flashSplit = back == layout[i].addr;
                        if (i == IramTextEntry) {
                                iramSplit = back == layout[i].addr;
                                record.iramTextEnd = back << 2;
                        }
                        PmaCfgWrite[i](cfg);
                        // A locked entry (set before us) keeps its own: told, not fixed.
                        if (PmaCfgRead[i]() != cfg) mismatch |= uint16_t(1u << i);
                }
                record.pmaMismatch = mismatch;
                record.flashSplit = flashSplit;
                record.iramSplit = iramSplit;
                return;
        }

        void clearPmp(RegionProtection &record) {
                uint16_t locked = 0;
                for (unsigned r = 0; r < Entries / 4; ++r) {
                        const uint32_t cfg = PmpCfgRead[r]();
                        uint32_t       keep = 0;
                        for (unsigned b = 0; b < 4; ++b) {
                                if ((cfg >> (8 * b)) & PmpLock) {
                                        keep |= 0xffu << (8 * b);
                                        locked |= uint16_t(1u << (4 * r + b));
                                }
                        }
                        PmpCfgWrite[r](cfg & keep);
                }
                record.pmpLocked = locked;
                return;
        }

        // Locked entries mean a bootloader from before BMDS took this over (region protection on in its
        // build) set IDF's layout already: what it locked stays until reset. IDF's own app setup then runs
        // as it did before, so an update without the bootloader keeps the protection it had. The
        // bootloader runs on core 0 alone, so core 0 decides for both (bootloaderLocked, set before
        // core 1 runs its hooks).
        constinit bool bootloaderLocked = false;

        bool lockedByBootloader() {
                for (unsigned i = 0; i < Entries; ++i) {
                        if (PmaCfgRead[i]() & PMA_L) return true;
                }
                for (unsigned r = 0; r < Entries / 4; ++r) {
                        if (PmpCfgRead[r]() & 0x80808080u) return true;
                }
                return false;
        }

        uint16_t pmpLockedMask() {
                uint16_t locked = 0;
                for (unsigned r = 0; r < Entries / 4; ++r) {
                        const uint32_t cfg = PmpCfgRead[r]();
                        for (unsigned b = 0; b < 4; ++b) {
                                if ((cfg >> (8 * b)) & PmpLock) locked |= uint16_t(1u << (4 * r + b));
                        }
                }
                return locked;
        }

} // namespace

const RegionProtection &regionProtection(unsigned core) {
        static constexpr RegionProtection NotDone{};
        return core < Cores ? records[core] : NotDone;
}

BMDS_NAMESPACE_END

// Linked whenever bmds_board is (CMakeLists.txt's -u): the hook below has no other reference
// in an app without the loader (recovery), and without it the app would have no protection.
extern "C" const char bmds_region_protect_anchor = 0;

// Core 0, while core 1 is still paused (the core stage): which layout both cores take.
ESP_SYSTEM_INIT_FN(bmds_region_detect, CORE, BIT(0), 1) {
        bmds::bootloaderLocked = bmds::lockedByBootloader();
        return ESP_OK;
}

// Each core sets its own (the PMA and PMP are per-core CSRs), before the scheduler starts.
ESP_SYSTEM_INIT_FN(bmds_region_protect, SECONDARY, ESP_SYSTEM_INIT_ALL_CORES, 1) {
        uint32_t core;
        __asm__ volatile("csrr %0, mhartid" : "=r"(core));
        if (core >= bmds::Cores) return ESP_OK;
        bmds::RegionProtection &record = bmds::records[core];
        if (bmds::bootloaderLocked || bmds::lockedByBootloader()) {
                esp_cpu_configure_region_protection(); // IDF's layout, as before (MEMPROT's split)
                record.idfLayout = true;
                record.pmpLocked = bmds::pmpLockedMask();
        } else {
                bmds::setPma(record);
                bmds::clearPmp(record);
        }
        record.done = true;
        return ESP_OK;
}
