/**
 * @file      usermode.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs in U-mode (devplan/protection.md §14): the per-process PMP sets, whether the system
 * can run programs in U-mode, and the setting that asks for it. The trap paths are usermode.S's.
 *
 * What a U-mode program is granted (step 3, with today's memory): the stub page (read, run),
 * all of PSRAM (its image, heap, the buffers the kernel hands it, but the kernel's own objects
 * there too, until step 4's window range), and the internal RAM it owns (stacks, `BMDS_MEM_FAST`
 * blocks), each region whole granules. Everything else is denied: the kernel's internal RAM and
 * IRAM, flash, the ROM, the peripherals, CSRs but the counters.
 */

#include "usermode.h"

#include <atomic>
#include <cstddef>
#include <new>
#include <esp_ipc_isr.h>
#include <esp_private/startup_internal.h>
#include <soc/soc.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/regionprotect.h>
#include <bmds/sysconf.h>
#include "trapentry.h"

#if !CONFIG_ESP_IPC_ISR_ENABLE
#error "UserSpace::revoke() interrupts the other core: CONFIG_ESP_IPC_ISR_ENABLE"
#endif

extern "C" {
// usermode.S's, placed by linker.lf (SURROUND).
extern const uint32_t bmds_user_page[];
extern const char     _bmds_user_page_start[];
extern const char     _bmds_user_page_end[];
extern const char     bmds_user_exit[];
extern const char     bmds_user_thread_exit[];
extern char           bmds_user_return[];
[[noreturn]] void     bmds_user_start(bmds::UserBlock *block, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                      uint32_t ra, uint32_t tp);
// systable.c's: the slots themselves.
extern const uint32_t bmds_k_table[];
// The PMP set's stamp each core holds (usermode.S writes it; 0: none).
uint32_t bmds_pmp_loaded[SOC_CPU_CORES_NUM];
}

BMDS_NAMESPACE_BEGIN

namespace {

        // usermodeoffsets.h describes these.
        static_assert(offsetof(UserBlock, t0) == BMDS_UB_T0 && offsetof(UserBlock, t1) == BMDS_UB_T1 &&
                              offsetof(UserBlock, t2) == BMDS_UB_T2 && offsetof(UserBlock, sp) == BMDS_UB_SP &&
                              offsetof(UserBlock, gp) == BMDS_UB_GP && offsetof(UserBlock, pc) == BMDS_UB_PC &&
                              offsetof(UserBlock, ra) == BMDS_UB_RA && offsetof(UserBlock, link) == BMDS_UB_LINK &&
                              offsetof(UserBlock, kernelLo) == BMDS_UB_KERNEL_LO &&
                              offsetof(UserBlock, userLo) == BMDS_UB_USER_LO &&
                              offsetof(UserBlock, userHi) == BMDS_UB_USER_HI &&
                              offsetof(UserBlock, pmp) == BMDS_UB_PMP && sizeof(UserBlock) == BMDS_UB_BYTES,
                      "UserBlock's layout is usermodeoffsets.h's");
        static_assert(offsetof(PmpSet, stamp) == BMDS_PMP_STAMP && offsetof(PmpSet, cfg) == BMDS_PMP_CFG &&
                              offsetof(PmpSet, addr) == BMDS_PMP_ADDR,
                      "PmpSet's layout is usermodeoffsets.h's");
        static_assert(BMDS_K_SYS_MAGIC == BMDS_SYS_MAGIC && BMDS_K_ABI_VERSION == BMDS_ABI_VERSION &&
                              BMDS_K_SYS_COUNT == offsetof(bmds_sys_t, count) &&
                              BMDS_K_SYS_SLOTS == offsetof(bmds_sys_t, slot),
                      "bmds_sys_t as usermode.S builds it");
        static_assert(BMDS_SYS_EXIT == 0 && BMDS_SYS_THREAD_EXIT == 54, "usermode.S's exit stubs");
        static_assert(sizeof(void *) == 4, "the block holds addresses as words");

        // Kernel stack bottoms are 16-aligned, and the block is at a stack's top.
        static_assert(alignof(UserBlock) == 16);

        constexpr uint8_t Rx = PmpSet::Read | PmpSet::Exec;
        constexpr uint8_t Rwx = PmpSet::Read | PmpSet::Write | PmpSet::Exec;

        // A set's stamp: even and never reused (odd: being rewritten; 0: nothing loaded).
        constinit std::atomic<uint32_t> nextStamp{2};

        // Decided once (isAvailable()): 0 not yet, 1 available, 2 not.
        constinit std::atomic<uint8_t> availability{0};
        constinit const char          *unavailableWhy = nullptr;

        uint32_t address(const void *p) {
                return uint32_t(reinterpret_cast<uintptr_t>(p));
        }

        // An interrupt on the other core: its trampoline (if it was in U-mode) or its next one
        // loads the set as it is now.
        void IRAM_ATTR nothing(void *) {
                return;
        }

        const char *checkAvailable() {
#if !CONFIG_BMDS_TRAP_ENTRY
                return "the trap entry isn't built in (CONFIG_BMDS_TRAP_ENTRY)";
#else
                for (unsigned core = 0; core < SOC_CPU_CORES_NUM; ++core) {
                        if (TrapEntry::state(core) != TrapEntry::State::Installed)
                                return "the trap entry isn't installed on both cores";
                        if (!regionProtection(core).isPmpFree())
                                return "the PMP isn't BMDS's (the bootloader predates memory protection)";
                }
                const auto *page = reinterpret_cast<const bmds_sys_t *>(bmds_user_page);
                const auto *direct = reinterpret_cast<const bmds_sys_t *>(bmds_k_table);
                // linker.lf's placement: alone in its page.
                if (address(_bmds_user_page_start) != address(bmds_user_page) ||
                    address(_bmds_user_page_start) % BMDS_K_PAGE_BYTES != 0 ||
                    address(_bmds_user_page_end) - address(_bmds_user_page_start) > BMDS_K_PAGE_BYTES ||
                    page->magic != direct->magic || page->abi_version != direct->abi_version ||
                    page->count != direct->count)
                        return "the stub page isn't as built";
                return nullptr;
#endif
        }

        BMDS_SYSCONF(programsProtect, "programs/protect", Config::Type::Bool, "false",
                     "Programs run in U-mode, kept out of the kernel's memory and the hardware", 0, 0, nullptr, nullptr)

} // namespace

UserSpace *UserSpace::create() {
        void *memory = Memory::allocAligned(MemSpace::Internal, sizeof(UserSpace), alignof(UserSpace));
        if (memory == nullptr) return nullptr;
        auto *space = new (memory) UserSpace;
        taskENTER_CRITICAL(&space->_lock);
        const Error err = space->update();
        taskEXIT_CRITICAL(&space->_lock);
        if (err.isError()) {
                destroy(space);
                return nullptr;
        }
        return space;
}

void UserSpace::destroy(UserSpace *space) {
        if (space == nullptr) return;
        space->~UserSpace();
        Memory::free(space);
        return;
}

// Under the lock: the set from the regions, published with a new stamp; nothing changed if
// they don't fit. Built and published under one lock, so the last change is what is loaded.
Error UserSpace::update() {
        PmpSet set;
        // Entry 0: the stub page. Entry 1: PSRAM (64 MB, aligned: one NAPOT entry).
        Error err = set.add(address(bmds_user_page), BMDS_K_PAGE_BYTES, Rx);
        if (err.isOk()) err = set.add(SOC_EXTRAM_LOW, SOC_EXTRAM_HIGH - SOC_EXTRAM_LOW, Rwx);
        for (unsigned i = 0; err.isOk() && i < _count; ++i)
                err = set.add(_regions[i].base, _regions[i].bytes, _regions[i].access);
        if (err.isError()) return err;
        __atomic_store_n(&_pmp.stamp, 1u, __ATOMIC_SEQ_CST); // odd: a trampoline reading it waits
        for (unsigned i = 0; i < PmpSet::Entries / 4; ++i) _pmp.cfg[i] = set.cfg[i];
        for (unsigned i = 0; i < PmpSet::Entries; ++i) _pmp.addr[i] = set.addr[i];
        _pmp.used = set.used;
        __atomic_store_n(&_pmp.stamp, nextStamp.fetch_add(2), __ATOMIC_SEQ_CST);
        return Error::Ok;
}

Error UserSpace::grant(uintptr_t base, size_t bytes, uint8_t access) {
        taskENTER_CRITICAL(&_lock);
        Error err = _count < MaxRegions ? Error::Ok : Error::Full;
        if (err.isOk()) {
                _regions[_count++] = Region{base, bytes, access};
                err = update();
                if (err.isError()) --_count;
        }
        taskEXIT_CRITICAL(&_lock);
        return err;
}

void UserSpace::revoke(uintptr_t base) {
        bool found = false;
        taskENTER_CRITICAL(&_lock);
        for (unsigned i = 0; i < _count; ++i) {
                if (_regions[i].base != base) continue;
                for (unsigned j = i + 1; j < _count; ++j) _regions[j - 1] = _regions[j];
                --_count;
                found = true;
                (void)update(); // fewer regions than fitted before: it fits
                break;
        }
        taskEXIT_CRITICAL(&_lock);
        // This core loads it at its next trampoline; the other may be in U-mode with the old one:
        // an interrupt there sends it through its trampoline.
        if (found) esp_ipc_isr_call_blocking(nothing, nullptr);
        return;
}

bool UserMode::isAvailable() {
        uint8_t state = availability.load(std::memory_order_acquire);
        if (state == 0) {
                unavailableWhy = checkAvailable();
                state = unavailableWhy == nullptr ? 1 : 2;
                availability.store(state, std::memory_order_release);
        }
        return state == 1;
}

bool UserMode::isWanted() {
        if (!isAvailable()) return false;
        auto [on, err] = SysConf::getBool(bmdsConf_programsProtect.key);
        return err.isOk() && on;
}

const bmds_sys_t *UserMode::table() {
        return reinterpret_cast<const bmds_sys_t *>(bmds_user_page);
}

uint32_t UserMode::exitAddress() {
        return address(bmds_user_exit);
}

uint32_t UserMode::threadExitAddress() {
        return address(bmds_user_thread_exit);
}

void UserMode::start(UserBlock &block, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t ra, uint32_t tp) {
        bmds_user_start(&block, a0, a1, a2, a3, ra, tp);
}

bool UserMode::isReturn(uint32_t pc) {
        return pc == address(bmds_user_return);
}

void UserMode::logState() {
        if (isAvailable()) return;
        auto [on, err] = SysConf::getBool(bmdsConf_programsProtect.key);
        if (err.isOk() && on)
                bmdsWarn("Loader: programs/protect is on, but programs run in M-mode: %s", unavailableWhy);
        return;
}

BMDS_NAMESPACE_END

// Each core: U-mode may read the cycle and instruction counters (`mcounteren`: CY, IR), as a
// program could in M-mode; nothing else.
ESP_SYSTEM_INIT_FN(bmds_user_counters, SECONDARY, ESP_SYSTEM_INIT_ALL_CORES, 3) {
        __asm__ volatile("csrw mcounteren, %0" : : "r"(0x5u));
        return ESP_OK;
}
