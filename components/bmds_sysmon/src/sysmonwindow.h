/**
 * @file      sysmonwindow.h
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <bmds/config.h>
#include <bmds/guiwindow.h>
#include <bmds/list.h>

#include "monitor.h"
#include "tablemodel.h"

BMDS_NAMESPACE_BEGIN

class Button;
class DetailsView;
class Graph;
class Label;
class ProgressBar;
class StatusBar;
class TabWidget;

/**
 * @brief The System Monitor's window (`devplan/sysmon.md` §3.5): tabs for the
 *        overview, CPU, processes, memory, audio, display, I/O and every
 *        counter, refreshed each second while it is shown and not minimized;
 *        whatever is out of normal is drawn amber or red, its tab marked.
 *
 * @par Thread Safety
 * The application's loop (its lock held), as every window.
 */
class SysmonWindow final : public GuiWindow {
        public:
                /** @brief The tabs, in order. */
                enum Tab : size_t {
                        Overview,
                        Cpu,
                        Processes,
                        Memory,
                        Audio,
                        Display,
                        Io,
                        Counters,
                        TabCount,
                };

                SysmonWindow(GuiApplication &app, const Config &config, Monitor &monitor);
                ~SysmonWindow() override;

                /** @brief Makes the widgets. @return `Ok` or `NoMem`. */
                Error build();

                /** @brief Starts watching and refreshing. @return `Ok`, or why the timer couldn't start. */
                Error start();

                /** @brief Shows it again and raises it (a second `sysmon`). */
                void raise();

        private:
                // A tab of a subsystem's: two graphs over its groups' counters.
                struct GroupPage {
                                Graph       *graphs[2] = {};
                                DetailsView *view = nullptr;
                                TableModel   model;
                };

                // A task's or process's CPU time at the last refresh, for its share since.
                struct Seen {
                                int64_t  key = 0;
                                uint64_t cpuUs = 0;
                };

                static void tick(void *arg);
                static void tabChanged(void *arg, TabWidget &tabs, size_t index);
                static void clearClicked(void *arg, Button &button);
                static void endClicked(void *arg, Button &button);
                static void endLater(void *arg);
                static void tasksSort(void *arg, DetailsView &view, size_t column);
                static void procsSort(void *arg, DetailsView &view, size_t column);

                Error buildOverview();
                Error buildCpu();
                Error buildProcesses();
                Error buildMemory();
                Error buildGroup(GroupPage &page, Tab tab, const char *title);
                Error buildCounters();

                void     refresh();
                void     refreshOverview();
                void     refreshCpu();
                void     refreshProcesses();
                void     refreshMemory();
                void     refreshGroup(GroupPage &page, Span<const char *const> patterns);
                void     refreshCounters();
                void     markTabs();
                void     say(const char *text); // in the status bar for a few seconds
                void     fillGraph(Graph *graph, StringView name, const char *label, bool percentOfCore = false);
                void     reset(DetailsView *view, const TableModel &model, int64_t selectedKey);
                int64_t  selectedKey(DetailsView *view, const TableModel &model) const;
                uint64_t shareSince(List<Seen> &seen, int64_t key, uint64_t cpuUs, int64_t elapsedUs) const;

                Monitor   &_monitor;
                TabWidget *_tabs = nullptr;
                StatusBar *_status = nullptr;
                int        _timer = -1;
                int        _endTimer = -1;
                int32_t    _ending = 0;       // a pid sent SIGTERM, SIGKILL next if still there
                char       _message[96] = {}; // shown in the status bar until _messageUntilNs
                int64_t    _messageUntilNs = 0;

                // Overview.
                Graph       *_overviewCpu[2] = {};
                ProgressBar *_memoryBars[2] = {};
                Label       *_memoryText[2] = {};
                Label       *_headline[2] = {};
                DetailsView *_problemsView = nullptr;
                TableModel   _problems;

                // CPU.
                Graph       *_cpuGraphs[2] = {};
                DetailsView *_tasksView = nullptr;
                TableModel   _tasks;
                List<Seen>   _taskSeen{MemSpace::Psram};
                int64_t      _taskSeenNs = 0;
                int          _taskSort = 5; // CPU %
                bool         _taskDescending = true;

                // Processes.
                DetailsView *_procsView = nullptr;
                TableModel   _procs;
                Button      *_end = nullptr;
                List<Seen>   _procSeen{MemSpace::Psram};
                int64_t      _procSeenNs = 0;
                int          _procSort = 0; // pid
                bool         _procDescending = false;

                // Memory.
                Graph       *_memGraphs[2] = {};
                DetailsView *_spacesView = nullptr;
                TableModel   _spaces;
                DetailsView *_cachesView = nullptr;
                TableModel   _caches;

                GroupPage _audio;
                GroupPage _display;
                GroupPage _io;

                DetailsView *_countersView = nullptr;
                TableModel   _counters;
};

BMDS_NAMESPACE_END
