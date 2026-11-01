/**
 * @file      rxcopies.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Received frames copied into PSRAM (rxcopies.h, devplan/network.md §32).
 */

#include <cstring>
#include <bmds/memory.h>

#include "rxcopies.h"

BMDS_NAMESPACE_BEGIN

namespace {

        struct Slot {
                        struct pbuf_custom custom; // first: lwIP's pbuf is its address
                        Slot              *nextFree;
                        uint8_t            data[RxCopies::SlotBytes];
        };

        struct Pool {
                        Slot           *slots = nullptr;
                        Slot           *free = nullptr;
                        RxCopies::Usage usage;
        };

        Pool pool;

        void freeSlot(struct pbuf *p) {
                Slot *s = reinterpret_cast<Slot *>(p);
                s->nextFree = pool.free;
                pool.free = s;
                --pool.usage.used;
                return;
        }

} // namespace

Error RxCopies::init(size_t bytes) {
        if (pool.usage.used != 0) return Error::Busy;
        Memory::free(pool.slots);
        pool = Pool{};
        const size_t count = bytes / sizeof(Slot);
        if (count == 0) return Error::Ok;
        pool.slots = static_cast<Slot *>(Memory::alloc(MemSpace::Psram, count * sizeof(Slot)));
        if (pool.slots == nullptr) return Error::NoMem;
        for (size_t i = count; i-- > 0;) {
                Slot &s = pool.slots[i];
                std::memset(&s.custom, 0, sizeof(s.custom));
                s.custom.custom_free_function = &freeSlot;
                s.nextFree = pool.free;
                pool.free = &s;
        }
        pool.usage.slots = count;
        return Error::Ok;
}

struct pbuf *RxCopies::copy(const uint8_t *data, size_t length) {
        if (length > SlotBytes) return nullptr;
        Slot *s = pool.free;
        if (s == nullptr) {
                if (pool.usage.slots != 0) ++pool.usage.empty;
                return nullptr;
        }
        pool.free = s->nextFree;
        if (++pool.usage.used > pool.usage.most) pool.usage.most = pool.usage.used;
        std::memcpy(s->data, data, length);
        return pbuf_alloced_custom(PBUF_RAW, static_cast<u16_t>(length), PBUF_REF, &s->custom, s->data, SlotBytes);
}

bool RxCopies::isCopy(const struct pbuf *p) {
        const auto *s = reinterpret_cast<const Slot *>(p);
        return s >= pool.slots && s < pool.slots + pool.usage.slots;
}

RxCopies::Usage RxCopies::usage() {
        return pool.usage;
}

BMDS_NAMESPACE_END
