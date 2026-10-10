/**
 * @file      cpumeter.h
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
#include <bmds/guiapplication.h>
#include <bmds/menu.h>
#include <bmds/stat.h>
#include <bmds/trayicon.h>

#include "cpumeterpicture.h"

BMDS_NAMESPACE_BEGIN

/**
 * @brief The tray's CPU meter (`devplan/sysmon.md` §3.5, step 5): a tray
 *        icon in the panel's process drawing each core's busy share over the
 *        last samples (`CpuMeterPicture`), its frame amber or red while the
 *        system has problems not yet seen. Only the kernel's public `Stats`,
 *        as any program could.
 *
 * It watches the counters for its life (the rules run and the history fills
 * while it is there) and reads them every `sysmon/interval`. Its tooltip is
 * each core's share, the memory used and the newest problem; a click raises
 * the System Monitor (at Overview while alarming), or starts it. A right
 * click opens System Monitor, the graph's form (`panel/tray/cpu-graph`) and
 * Remove from Panel (`panel/tray/cpu` false); the settings are saved.
 *
 * @par Thread Safety
 * The application's lock.
 */
class CpuMeter {
        public:
                /** @brief Asked to start the System Monitor (none is running). */
                using StartMonitor = void (*)(void *arg);

                /** @brief The monitor's service, and what the meter sends it. */
                static constexpr const char *MonitorService = "sysmon";

                /** @brief The settings its menu changes (the panel's). */
                static constexpr const char *ShownKey = "panel/tray/cpu";
                static constexpr const char *GraphKey = "panel/tray/cpu-graph";

                CpuMeter(GuiApplication &app, StartMonitor startMonitor, void *arg);
                ~CpuMeter();
                CpuMeter(const CpuMeter &) = delete;
                CpuMeter &operator=(const CpuMeter &) = delete;

                /** @brief Starts watching and shows its icon in the tray. @return `Ok`, `NoMem` or the tray's error. */
                Error start();

                /** @brief Sets how the cores are drawn (`panel/tray/cpu-graph`). */
                void setForm(CpuMeterPicture::Form form);

        private:
                static constexpr size_t MaxCores = 2;

                // The counters it reads, by id (found again when the registry changes).
                enum Read : size_t {
                        Counted,
                        InternalUsed,
                        InternalTotal,
                        PsramUsed,
                        PsramTotal,
                        ReadCount,
                };

                enum Choice : int {
                        ChooseMonitor = 1,
                        ChooseCores,
                        ChooseCombined,
                        ChooseRemove,
                };

                static void tick(void *arg);
                static void activated(void *arg, TrayIcon &icon, TrayIcon::Reason reason);
                static void chosen(void *arg, int item);
                static void removeDue(void *arg);
                void        openMonitor();
                void        fillMenu();
                void        save(const char *key, StringView value);
                void        refresh();
                void        findCounters();
                void        restartTimer(uint32_t intervalMs);
                size_t      writeProblem(char *out, size_t size);

                GuiApplication       &_app;
                TrayIcon              _icon;
                Menu                  _menu;
                StartMonitor          _start;
                void                 *_startArg;
                CpuMeterPicture::Form _form = CpuMeterPicture::Form::Cores;
                Stat::Health          _alarm = Stat::Health::Normal;
                bool                  _watching = false;
                int                   _timer = -1;
                uint32_t              _intervalMs = 0;
                uint32_t              _generation = 0;
                size_t                _coreCount = 0;
                uint32_t              _cores[MaxCores] = {};
                uint32_t              _ids[ReadCount] = {};
                uint32_t              _pixels[CpuMeterPicture::Side * CpuMeterPicture::Side] = {};
                char                  _tip[128] = {};
};

BMDS_NAMESPACE_END
