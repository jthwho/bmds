/**
 * @file      input.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Where keyboards' and mice's events go (devplan/input.md §4).
 */

#include <atomic>
#include <bmds/input.h>
#include <bmds/mutex.h>
#include <bmds/staticobject.h>
#include <bmds/stats.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Held while an event is delivered, so setSink() waits for it.
        constinit StaticObject<Mutex> lock;
        constinit Input::Sink         current;

        // The counters (devplan/sysmon.md §6): names are an interface once shipped.
        constinit std::atomic<uint32_t> keys{0};
        constinit std::atomic<uint32_t> pointers{0};
        constinit std::atomic<uint32_t> dropped{0};

        constexpr Stat::Desc InputStats[] = {
                {"keys", Stat::Unit::Count, Stat::Kind::Total, 0, {}, "", "Key events from keyboards"},
                {"pointer_events",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 Stat::KeepHistory,
                 {},
                 "",
                 "Pointer events from mice"},
                {"dropped",
                 Stat::Unit::Count,
                 Stat::Kind::Total,
                 0,
                 {},
                 "",
                 "Events with nowhere to go (no window manager running)"},
        };

        void readInput(void *, Span<int64_t> v) {
                v[0] = keys.load(std::memory_order_relaxed);
                v[1] = pointers.load(std::memory_order_relaxed);
                v[2] = dropped.load(std::memory_order_relaxed);
                return;
        }

        constinit StaticObject<Stats::Group> counters;

        constinit std::atomic<Input::AttachKey> attachKeyHandler{nullptr};

} // namespace

Error Input::init() {
        if (!lock.isCreated()) lock.create();
        if (!counters.isCreated()) {
                counters.create(InputStats, sizeof(InputStats) / sizeof(InputStats[0]), readInput, nullptr);
                counters->setName("input");
                (void)Stats::add(*counters);
        }
        return Error::Ok;
}

void Input::setSink(const Sink *sink) {
        Mutex::Locker locker(*lock, WaitForever);
        current = sink != nullptr ? *sink : Sink();
        return;
}

void Input::key(const KeyEvent &event) {
        keys.fetch_add(1, std::memory_order_relaxed);
        Mutex::Locker locker(*lock, WaitForever);
        if (current.key != nullptr)
                current.key(current.arg, event);
        else
                dropped.fetch_add(1, std::memory_order_relaxed);
        return;
}

void Input::pointer(const PointerEvent &event) {
        pointers.fetch_add(1, std::memory_order_relaxed);
        Mutex::Locker locker(*lock, WaitForever);
        if (current.pointer != nullptr)
                current.pointer(current.arg, event);
        else
                dropped.fetch_add(1, std::memory_order_relaxed);
        return;
}

void Input::setAttachKeyHandler(AttachKey handler) {
        attachKeyHandler.store(handler, std::memory_order_release);
        return;
}

void Input::attachKey(uint8_t usage) {
        const AttachKey handler = attachKeyHandler.load(std::memory_order_acquire);
        if (handler != nullptr) handler(usage);
        return;
}

BMDS_NAMESPACE_END
