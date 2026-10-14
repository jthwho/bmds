/**
 * @file      flashwrites.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The hold on the writes that can wait (devplan/cfg.md §3).
 */

#include <atomic>
#include <cstdint>
#include <bmds/flashwrites.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constinit std::atomic<uint32_t> holds{0};
        constinit WaitListOf<2>         released; // /flash's worker

} // namespace

void FlashWrites::hold() {
        holds.fetch_add(1, std::memory_order_acq_rel);
        return;
}

void FlashWrites::release() {
        if (holds.fetch_sub(1, std::memory_order_acq_rel) == 1) released.notify();
        return;
}

bool FlashWrites::isHeld() {
        return holds.load(std::memory_order_acquire) != 0;
}

WaitList &FlashWrites::releases() {
        return released;
}

BMDS_NAMESPACE_END
