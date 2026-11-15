/**
 * @file      trapentry.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Installs BMDS's trap entry (trapentry.S, devplan/protection.md §4.2) in front of ESP-IDF's:
 * `mtvec` at its exception stub, `mtvt` at its table, whose vector i leads to the stub for
 * wherever IDF's `_mtvt_table[i]` leads. The table is fixed at link time in IRAM's code (the
 * CLIC's load needs execute permission), so it is checked against IDF's first: one that doesn't
 * match (IDF's options changed it), or vectors that aren't IDF's, leave IDF's vectors in place.
 *
 * `mtvec` and `mtvt` are written once by IDF's start-up (`cpu_start.c`) and are ordinary CSRs:
 * replacing them later patches nothing. `mscratch` is the entry's (0 in M-mode); IDF uses it only
 * to save it over light sleep.
 */

#include <sdkconfig.h>
#include <cstdint>
#include <esp_attr.h>
#include <esp_private/startup_internal.h>
#include <riscv/csr_clic.h>
#include <bmds/log.h>
#include "trapentry.h"

extern "C" {
// IDF's (riscv/vectors_clic.S, vectors.S, esp_ipc_isr_handler.S).
extern const uint32_t _mtvt_table[];
extern char           _vector_table[];
extern char           _interrupt_handler[];
extern char           _panic_handler[];
#if CONFIG_ESP_IPC_ISR_ENABLE
extern char esp_ipc_isr_handler[];
#endif
// trapentry.S's.
extern char bmds_trap_exception[];
extern char bmds_trap_irq_interrupt[];
extern char bmds_trap_irq_panic[];
#if CONFIG_ESP_IPC_ISR_ENABLE
extern char bmds_trap_irq_ipc[];
#endif
extern const uint32_t bmds_trap_table[]; // in IRAM's code: the CLIC needs execute permission
}

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr unsigned Cores = 2;
        constexpr unsigned Vectors = RV_EXTERNAL_INT_OFFSET + RV_EXTERNAL_INT_COUNT; // IDF's table: 48
        constexpr uint32_t MtvecModeMask = 0x3f; // the mode and submode bits under the 64-aligned base

        constinit bool             tableBuilt = false;
        constinit TrapEntry::State tableState = TrapEntry::State::Off;
        constinit TrapEntry::State states[Cores] = {};

        uint32_t address(const void *p) {
                return uint32_t(reinterpret_cast<uintptr_t>(p));
        }

        // Where vector i of ours leads, from where IDF's leads; 0 for a target this entry doesn't know.
        uint32_t stubFor(uint32_t idfTarget) {
                if (idfTarget == address(_interrupt_handler)) return address(bmds_trap_irq_interrupt);
                if (idfTarget == address(_panic_handler)) return address(bmds_trap_irq_panic);
#if CONFIG_ESP_IPC_ISR_ENABLE
                if (idfTarget == address(esp_ipc_isr_handler)) return address(bmds_trap_irq_ipc);
#endif
                return 0;
        }

        // Ours (fixed at link time) against IDF's, vector by vector.
        TrapEntry::State checkTable() {
                for (unsigned i = 0; i < Vectors; ++i) {
                        const uint32_t stub = stubFor(_mtvt_table[i]);
                        if (stub == 0 || stub != bmds_trap_table[i]) return TrapEntry::State::UnknownIdf;
                }
                return TrapEntry::State::Installed;
        }

} // namespace

TrapEntry::State TrapEntry::state(unsigned core) {
        return core < Cores ? states[core] : State::Off;
}

namespace {

        // trapentry.S's bmds_trap_bad_sp: an exception in M-mode with an SP no stack can be at.
        struct StormRecord {
                        uint32_t mepc, mcause, mtval, sp, ra, count, magic, reserved;
        };
        static_assert(sizeof(StormRecord) == 32, "trapentry.S's layout");
        constexpr uint32_t StormMagic = 0x4d525453;

} // namespace

BMDS_NAMESPACE_END

extern "C" {
// Kept across resets (RTC memory: a power cycle clears it), one a core.
RTC_NOINIT_ATTR bmds::StormRecord bmds_storm_records[SOC_CPU_CORES_NUM];
// Each core's emergency stack, 2 KB.
alignas(16) uint8_t bmds_storm_stacks[SOC_CPU_CORES_NUM * 2048];
}

BMDS_NAMESPACE_BEGIN

void TrapEntry::logState() {
        // An earlier boot's storm: its first exception, told once.
        for (unsigned core = 0; core < SOC_CPU_CORES_NUM; ++core) {
                StormRecord &r = bmds_storm_records[core];
                if (r.magic != StormMagic) continue;
                bmdsErr("Loader: core %u had an exception with a bad SP before this boot (%u times): mepc 0x%08x, "
                        "mcause 0x%x, mtval 0x%08x, sp 0x%08x, ra 0x%08x",
                        core, unsigned(r.count), unsigned(r.mepc), unsigned(r.mcause), unsigned(r.mtval),
                        unsigned(r.sp), unsigned(r.ra));
                r = StormRecord{};
        }
        for (unsigned core = 0; core < Cores; ++core) {
                switch (states[core]) {
                        case State::Installed: break;
                        case State::Off:
#if CONFIG_BMDS_TRAP_ENTRY
                                bmdsWarn("Loader: core %u: the trap entry didn't run", core);
#endif
                                break;
                        case State::UnknownIdf:
                                bmdsWarn("Loader: core %u: the trap entry's vector table doesn't match IDF's: IDF's "
                                         "vectors kept",
                                         core);
                                break;
                        case State::NotIdfs:
                                bmdsWarn("Loader: core %u: the vectors weren't IDF's at start-up: left alone", core);
                                break;
                }
        }
        return;
}

BMDS_NAMESPACE_END

#if CONFIG_BMDS_TRAP_ENTRY

// Core 0, while core 1 is still paused (the core stage): whether the table matches IDF's.
ESP_SYSTEM_INIT_FN(bmds_trap_entry_table, CORE, BIT(0), 2) {
        bmds::tableState = bmds::checkTable();
        bmds::tableBuilt = true;
        return ESP_OK;
}

// Each core its own CSRs, before the scheduler, interrupts off while both change.
ESP_SYSTEM_INIT_FN(bmds_trap_entry_install, SECONDARY, ESP_SYSTEM_INIT_ALL_CORES, 2) {
        uint32_t core;
        __asm__ volatile("csrr %0, mhartid" : "=r"(core));
        if (core >= bmds::Cores || !bmds::tableBuilt) return ESP_OK;
        if (bmds::tableState != bmds::TrapEntry::State::Installed) {
                bmds::states[core] = bmds::tableState;
                return ESP_OK;
        }
        uint32_t mtvec, mtvt;
        __asm__ volatile("csrr %0, mtvec" : "=r"(mtvec));
        __asm__ volatile("csrr %0, %1" : "=r"(mtvt) : "i"(MTVT_CSR));
        if ((mtvec & ~bmds::MtvecModeMask) != bmds::address(_vector_table) || mtvt != bmds::address(_mtvt_table)) {
                bmds::states[core] = bmds::TrapEntry::State::NotIdfs;
                return ESP_OK;
        }
        uint32_t was;
        __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(was)); // MIE off
        __asm__ volatile("csrw mscratch, zero");
        __asm__ volatile("csrw %0, %1" : : "i"(MTVT_CSR), "r"(bmds::address(bmds_trap_table)) : "memory");
        __asm__ volatile("csrw mtvec, %0"
                         :
                         : "r"(bmds::address(bmds_trap_exception) | (mtvec & bmds::MtvecModeMask))
                         : "memory");
        if (was & 8) __asm__ volatile("csrsi mstatus, 8");
        bmds::states[core] = bmds::TrapEntry::State::Installed;
        return ESP_OK;
}

#endif
