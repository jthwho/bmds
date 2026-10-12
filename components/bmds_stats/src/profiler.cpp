/**
 * @file      profiler.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The sampling profiler (devplan/sysmon.md §9): a FreeRTOS tick hook reading the interrupted
 * context from the frame ESP-IDF's RISC-V port saved at the task's pxTopOfStack.
 */

#include <atomic>
#include <cstring>
#include <esp_freertos_hooks.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <bmds/memory.h>
#include <bmds/profiler.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The saved frame's first words (riscv/rvruntime-frames.h): mepc, then ra.
        constexpr size_t FramePc = 0;
        constexpr size_t FrameRa = 1;

        constinit Profiler::Sample     *samples = nullptr;
        constinit std::atomic<size_t>   taken{0};
        constinit std::atomic<bool>     running{false};
        constinit std::atomic<uint32_t> hooked{0}; // the cores whose hook is in
        constinit Profiler::Locator     locator = nullptr;
        constinit Profiler::Module      modules[Profiler::MaxModules] = {};
        constinit std::atomic<uint32_t> moduleBases[Profiler::MaxModules] = {}; // published last
        constinit std::atomic<size_t>   moduleUsed{1};

        // A name for the dump's space-separated fields: a task's with '_' for its spaces, or a
        // command's first word.
        void IRAM_ATTR copyName(char *to, const char *from, size_t bytes, bool firstWord) {
                size_t i = 0;
                for (; i + 1 < bytes && from != nullptr && from[i] != '\0'; ++i) {
                        if (from[i] == ' ' && firstWord) break;
                        to[i] = from[i] == ' ' ? '_' : from[i];
                }
                to[i] = '\0';
                return;
        }

        // The module of an image at @p base, added if new (a race may add one twice: harmless).
        uint8_t IRAM_ATTR moduleOf(uint32_t base, uint32_t bytes, const char *name) {
                const size_t used = moduleUsed.load(std::memory_order_acquire);
                for (size_t i = 1; i < used && i < Profiler::MaxModules; ++i)
                        if (moduleBases[i].load(std::memory_order_acquire) == base) return static_cast<uint8_t>(i);
                const size_t i = moduleUsed.fetch_add(1, std::memory_order_acq_rel);
                if (i >= Profiler::MaxModules) return 0;
                modules[i].base = base;
                modules[i].bytes = bytes;
                copyName(modules[i].name, name, sizeof(modules[i].name), true);
                moduleBases[i].store(base, std::memory_order_release);
                return static_cast<uint8_t>(i);
        }

        void IRAM_ATTR tick() {
                if (!running.load(std::memory_order_relaxed)) return;
                const size_t i = taken.fetch_add(1, std::memory_order_relaxed);
                if (i >= Profiler::MaxSamples) {
                        taken.store(Profiler::MaxSamples, std::memory_order_relaxed);
                        return;
                }
                Profiler::Sample &s = samples[i];
                s.core = static_cast<uint8_t>(xPortGetCoreID());
                s.reserved = 0;
                // A tick that interrupted an interrupt: the task's frame is an older one.
                if (xPortInterruptedFromISRContext() > 1) {
                        s.pc = 0;
                        s.ra = 0;
                        s.module = 0;
                        std::strncpy(s.task, "(isr)", sizeof(s.task));
                        return;
                }
                TaskHandle_t    task = xTaskGetCurrentTaskHandle();
                const uint32_t *frame = *reinterpret_cast<uint32_t *const *>(task); // pxTopOfStack
                s.pc = frame[FramePc];
                s.ra = frame[FrameRa];
                copyName(s.task, pcTaskGetName(task), sizeof(s.task), false);
                uint32_t    base = 0, bytes = 0;
                const char *name = nullptr;
                const bool  inProgram = locator != nullptr && locator(task, base, bytes, name) && base != 0;
                s.module = inProgram ? moduleOf(base, bytes, name) : 0;
                return;
        }

} // namespace

void Profiler::setLocator(Locator l) {
        locator = l;
        return;
}

Error Profiler::start(uint32_t coreMask) {
        if (coreMask == 0 || coreMask >= (1u << portNUM_PROCESSORS)) return Error::Invalid;
        stop();
        if (samples == nullptr)
                samples = static_cast<Sample *>(Memory::alloc(MemSpace::Psram, MaxSamples * sizeof(Sample)));
        if (samples == nullptr) return Error::NoMem;
        taken.store(0);
        modules[0] = Module{0, 0, "kernel"};
        for (size_t i = 1; i < MaxModules; ++i) moduleBases[i].store(0);
        moduleUsed.store(1);
        for (uint32_t core = 0; core < portNUM_PROCESSORS; ++core) {
                if ((coreMask & (1u << core)) == 0) continue;
                const esp_err_t e = esp_register_freertos_tick_hook_for_cpu(&tick, core);
                if (e != ESP_OK) {
                        stop();
                        return Error::fromEspErr(e);
                }
                hooked.fetch_or(1u << core);
        }
        running.store(true);
        return Error::Ok;
}

void Profiler::stop() {
        running.store(false);
        const uint32_t cores = hooked.exchange(0);
        for (uint32_t core = 0; core < portNUM_PROCESSORS; ++core)
                if (cores & (1u << core)) esp_deregister_freertos_tick_hook_for_cpu(&tick, core);
        if (cores != 0) vTaskDelay(2); // a hook already running on the other core finishes
        return;
}

bool Profiler::isRunning() {
        return running.load();
}

size_t Profiler::sampleCount() {
        const size_t n = taken.load();
        return samples == nullptr ? 0 : (n < MaxSamples ? n : MaxSamples);
}

const Profiler::Sample &Profiler::sample(size_t index) {
        return samples[index];
}

size_t Profiler::moduleCount() {
        const size_t n = moduleUsed.load();
        return n < MaxModules ? n : MaxModules;
}

const Profiler::Module &Profiler::module(size_t index) {
        return modules[index];
}

void Profiler::clear() {
        stop();
        Memory::free(samples);
        samples = nullptr;
        taken.store(0);
        return;
}

BMDS_NAMESPACE_END
