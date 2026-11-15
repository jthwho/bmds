/**
 * @file      usermode.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Programs in U-mode (devplan/protection.md §14): the per-program PMP sets, whether the system
 * can run programs in U-mode, and the setting that asks for it. The trap paths are usermode.S's.
 *
 * What a U-mode program is granted (§15): the stub page (read, run) and its own memory
 * (`UserMemory`): its window ranges (its code read and run, the rest read and write) and the
 * internal RAM it owns (stacks, `BMDS_MEM_FAST` blocks), each region whole granules. Everything
 * else is denied: the kernel's memory, other programs', flash, the ROM, the peripherals, CSRs but
 * the counters.
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
#include <bmds/process.h>
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
                              offsetof(UserBlock, pmp) == BMDS_UB_PMP && offsetof(UserBlock, space) == BMDS_UB_SPACE &&
                              sizeof(UserBlock) == BMDS_UB_BYTES,
                      "UserBlock's layout is usermodeoffsets.h's");
        static_assert(offsetof(PmpSet, stamp) == BMDS_PMP_STAMP && offsetof(PmpSet, cfg) == BMDS_PMP_CFG &&
                              offsetof(PmpSet, addr) == BMDS_PMP_ADDR,
                      "PmpSet's layout is usermodeoffsets.h's");
        static_assert(BMDS_K_SYS_MAGIC == BMDS_SYS_MAGIC && BMDS_K_ABI_VERSION == BMDS_ABI_VERSION &&
                              BMDS_K_SYS_COUNT == offsetof(bmds_sys_t, count) &&
                              BMDS_K_SYS_SLOTS == offsetof(bmds_sys_t, slot),
                      "bmds_sys_t as usermode.S builds it");
        static_assert(BMDS_SYS_EXIT == 0 && BMDS_SYS_THREAD_EXIT == 54, "usermode.S's exit stubs");
        static_assert(BMDS_SYS_CLOCK_GET == BMDS_K_SYS_CLOCK_GET, "usermode.S's fast clock_get");
        static_assert(sizeof(void *) == 4, "the block holds addresses as words");

        // Kernel stack bottoms are 16-aligned, and the block is at a stack's top.
        static_assert(alignof(UserBlock) == 16);

        constexpr uint8_t Rx = PmpSet::Read | PmpSet::Exec;

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

        BMDS_SYSCONF(programsProtect, "programs/protect", Config::Type::Bool, "true",
                     "Programs run in U-mode, kept out of the kernel's memory and the hardware", 0, 0, nullptr, nullptr)

} // namespace

UserSpace *UserSpace::create() {
        void *memory = Memory::allocAligned(MemSpace::Internal, sizeof(UserSpace), alignof(UserSpace));
        if (memory == nullptr) return nullptr;
        auto *space = new (memory) UserSpace;
        taskENTER_CRITICAL(&space->_lock);
        const Error err = space->update(0);
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
// they don't fit with @p keepFree entries to spare. Built and published under one lock, so the
// last change is what is loaded.
Error UserSpace::update(unsigned keepFree) {
        PmpSet set;
        Error  err = set.add(address(bmds_user_page), BMDS_K_PAGE_BYTES, Rx); // entry 0: the stub page
        for (unsigned i = 0; err.isOk() && i < _count; ++i)
                err = set.add(_regions[i].base, _regions[i].bytes, _regions[i].access);
        if (err.isOk() && PmpSet::Entries - set.used < keepFree) err = Error::Full;
        if (err.isError()) return err;
        __atomic_store_n(&_pmp.stamp, 1u, __ATOMIC_SEQ_CST); // odd: a trampoline reading it waits
        for (unsigned i = 0; i < PmpSet::Entries / 4; ++i) _pmp.cfg[i] = set.cfg[i];
        for (unsigned i = 0; i < PmpSet::Entries; ++i) _pmp.addr[i] = set.addr[i];
        _pmp.used = set.used;
        // The same regions decoded, for the slots' checks (§16): they don't overlap, so the order
        // the PMP decides by doesn't matter.
        _spans[0] = Span{address(bmds_user_page), address(bmds_user_page) + BMDS_K_PAGE_BYTES, Rx};
        for (unsigned i = 0; i < _count; ++i)
                _spans[i + 1] = Span{uint32_t(_regions[i].base), uint32_t(_regions[i].base + _regions[i].bytes),
                                     _regions[i].access};
        _spanCount = uint8_t(_count + 1);
        __atomic_store_n(&_pmp.stamp, nextStamp.fetch_add(2), __ATOMIC_SEQ_CST);
        return Error::Ok;
}

Error UserSpace::grant(uintptr_t base, size_t bytes, uint8_t access, unsigned keepFree) {
        taskENTER_CRITICAL(&_lock);
        Error err = _count < MaxRegions ? Error::Ok : Error::Full;
        if (err.isOk()) {
                _regions[_count++] = Region{base, bytes, access};
                err = update(keepFree);
                if (err.isError()) --_count;
        }
        taskEXIT_CRITICAL(&_lock);
        // The other core may run a sibling thread with the old set: an interrupt there sends it
        // through its trampoline, which loads this one.
        if (err.isOk()) esp_ipc_isr_call_blocking(nothing, nullptr);
        return err;
}

Error UserSpace::resize(uintptr_t base, size_t bytes) {
        Error err = Error::NotExist;
        taskENTER_CRITICAL(&_lock);
        for (unsigned i = 0; i < _count; ++i) {
                if (_regions[i].base != base) continue;
                const size_t old = _regions[i].bytes;
                _regions[i].bytes = bytes;
                err = update(0);
                if (err.isError()) _regions[i].bytes = old;
                break;
        }
        taskEXIT_CRITICAL(&_lock);
        if (err.isOk()) esp_ipc_isr_call_blocking(nothing, nullptr);
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
                (void)update(0); // fewer regions than fitted before: it fits
                break;
        }
        taskEXIT_CRITICAL(&_lock);
        // This core loads it at its next trampoline; the other may be in U-mode with the old one.
        if (found) esp_ipc_isr_call_blocking(nothing, nullptr);
        return;
}

unsigned UserSpace::entriesFor(uintptr_t base, size_t bytes) const {
        return _pmp.entriesFor(base, bytes);
}

bool UserSpace::allows(uintptr_t address, size_t bytes, uint8_t access) {
        if (bytes == 0) return true;
        if (address + bytes < address) return false;
        for (;;) {
                const uint32_t before = __atomic_load_n(&_pmp.stamp, __ATOMIC_ACQUIRE);
                if ((before & 1u) != 0) continue;
                uintptr_t       at = address;
                const uintptr_t end = address + bytes;
                bool            ok = true;
                while (ok && at < end) {
                        ok = false;
                        for (unsigned i = 0; i < _spanCount; ++i) {
                                const Span &s = _spans[i];
                                if (at < s.lo || at >= s.hi) continue;
                                if ((s.access & access) == access) {
                                        at = s.hi;
                                        ok = true;
                                }
                                break;
                        }
                }
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (__atomic_load_n(&_pmp.stamp, __ATOMIC_RELAXED) == before) return ok;
        }
}

unsigned UserSpace::freeEntries() const {
        return PmpSet::Entries - _pmp.used;
}

// Without the lock (a critical section writes the CLIC's threshold here: about 1,000 cycles, more
// than the rest of a system call): the set read between two looks at its stamp, again if a change
// was under way (odd) or came meanwhile.
size_t UserSpace::extent(uintptr_t address, uint8_t access) {
        for (;;) {
                const uint32_t before = __atomic_load_n(&_pmp.stamp, __ATOMIC_ACQUIRE);
                if ((before & 1u) != 0) continue;
                size_t n = 0;
                for (unsigned i = 0; i < _spanCount; ++i) {
                        const Span &s = _spans[i];
                        if (address < s.lo || address >= s.hi) continue;
                        n = (s.access & access) == access ? s.hi - address : 0;
                        break;
                }
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                if (__atomic_load_n(&_pmp.stamp, __ATOMIC_RELAXED) == before) return n;
        }
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

// clock_get from U-mode, inside its own `ecall` trap with interrupts off (usermode.S): pure and
// short, nothing in it can trap (integer arithmetic, the timer's registers, a sequence lock), so
// the trap is left by its own `mret` straight back to U-mode, two trap round trips saved
// (devplan/protection.md §18). No hold on the program's memory is needed: with this core's
// interrupts off, another core's revoke waits for its interrupt here before anything goes.
extern "C" int32_t bmds_user_clock(uint32_t clock, int64_t *ns, bmds::UserBlock *block) {
        if (ns == nullptr) return -BMDS_EFAULT;
        if (!block->space->allows(reinterpret_cast<uintptr_t>(ns), sizeof(*ns),
                                  bmds::PmpSet::Read | bmds::PmpSet::Write))
                return -BMDS_EFAULT;
        return bmds::Process::clockGet(clock, ns);
}

// Each core: U-mode may read the cycle and instruction counters (`mcounteren`: CY, IR), as a
// program could in M-mode; nothing else.
ESP_SYSTEM_INIT_FN(bmds_user_counters, SECONDARY, ESP_SYSTEM_INIT_ALL_CORES, 3) {
        __asm__ volatile("csrw mcounteren, %0" : : "r"(0x5u));
        return ESP_OK;
}
