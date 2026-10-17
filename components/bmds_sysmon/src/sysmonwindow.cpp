/**
 * @file      sysmonwindow.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The System Monitor's window (devplan/sysmon.md §3.5).
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/detailsview.h>
#include <bmds/furniture.h>
#include <bmds/graph.h>
#include <bmds/guiapplication.h>
#include <bmds/iconid.h>
#include <bmds/icontheme.h>
#include <bmds/sys.h>
#include <bmds/tabwidget.h>
#include <bmds/windowmanager.h>

#include "sysmonwindow.h"

BMDS_NAMESPACE_BEGIN

namespace {

        using Align = ItemModel::Align;
        using Emphasis = ItemModel::Emphasis;
        using Text = char[Monitor::TextBytes];

        constexpr uint32_t RefreshMs = 1000;
        constexpr uint32_t KillAfterMs = 2000;
        constexpr size_t   MaxTasks = 96;
        constexpr size_t   HistoryDepth = 120; // samples the registry keeps (devplan/sysmon.md §3.4)
        constexpr size_t   ProblemBatch = 8;
        constexpr int64_t  MessageNs = 4000000000; // a message's time in the status bar
        constexpr int64_t  Unavailable = Stat::Unavailable;

        constexpr const char *TabTitles[SysmonWindow::TabCount] = {"Overview", "CPU", "Processes", "Memory",  "Audio",
                                                                   "Display",  "I/O", "Network",   "Counters"};

        // The counters each tab shows, whose health marks its title.
        constexpr const char *CpuPatterns[] = {"cpu.*", "system.*"};
        constexpr const char *ProcessPatterns[] = {"proc.*"};
        constexpr const char *MemoryPatterns[] = {"mem.*", "cache.*"};
        constexpr const char *AudioPatterns[] = {"audio.*", "synth.*", "midi.*"};
        constexpr const char *DisplayPatterns[] = {"display*", "wm*"};
        constexpr const char *IoPatterns[] = {"storage.*", "usb*", "input.*", "console.*", "clock.*"};
        constexpr const char *NetworkPatterns[] = {"net.*"};
        constexpr const char *AllPatterns[] = {"*"};

        Span<const char *const> patternsOf(size_t tab) {
                switch (tab) {
                        case SysmonWindow::Cpu: return CpuPatterns;
                        case SysmonWindow::Processes: return ProcessPatterns;
                        case SysmonWindow::Memory: return MemoryPatterns;
                        case SysmonWindow::Audio: return AudioPatterns;
                        case SysmonWindow::Display: return DisplayPatterns;
                        case SysmonWindow::Io: return IoPatterns;
                        case SysmonWindow::Network: return NetworkPatterns;
                        case SysmonWindow::Counters: return AllPatterns;
                        default: return Span<const char *const>();
                }
        }

        Emphasis emphasisOf(Stat::Health h) {
                return h == Stat::Health::Critical  ? Emphasis::Critical
                       : h == Stat::Health::Warning ? Emphasis::Warning
                                                    : Emphasis::None;
        }

        Graph::Health graphHealth(Stat::Health h) {
                return h == Stat::Health::Critical  ? Graph::Health::Critical
                       : h == Stat::Health::Warning ? Graph::Health::Warning
                                                    : Graph::Health::Normal;
        }

        Stat::Health stackHealth(uint32_t free) {
                return free < BMDS_TASK_STACK_CRIT   ? Stat::Health::Critical
                       : free < BMDS_TASK_STACK_WARN ? Stat::Health::Warning
                                                     : Stat::Health::Normal;
        }

        // A time run as m:ss.cc (h:mm:ss past an hour).
        const char *duration(Text &out, uint64_t us) {
                const uint64_t cs = us / 10000;
                const uint64_t s = cs / 100;
                if (s >= 3600)
                        std::snprintf(out, sizeof(out), "%" PRIu64 ":%02" PRIu64 ":%02" PRIu64, s / 3600, s / 60 % 60,
                                      s % 60);
                else
                        std::snprintf(out, sizeof(out), "%" PRIu64 ":%02" PRIu64 ".%02" PRIu64, s / 60, s % 60,
                                      cs % 100);
                return out;
        }

        // A span of seconds, short: 45s, 12m, 3h.
        const char *ago(Text &out, int64_t ms) {
                const int64_t s = ms > 0 ? ms / 1000 : 0;
                if (s < 120)
                        std::snprintf(out, sizeof(out), "%" PRId64 "s", s);
                else if (s < 7200)
                        std::snprintf(out, sizeof(out), "%" PRId64 "m", s / 60);
                else
                        std::snprintf(out, sizeof(out), "%" PRId64 "h", s / 3600);
                return out;
        }

        const char *permille(Text &out, uint64_t pm) {
                std::snprintf(out, sizeof(out), "%" PRIu64 ".%" PRIu64 " %%", pm / 10, pm % 10);
                return out;
        }

        const char *taskState(uint8_t state) {
                switch (state) {
                        case 0: return "running";
                        case 1: return "ready";
                        case 2: return "blocked";
                        case 3: return "suspended";
                        default: return "deleted";
                }
        }

        const char *processState(Source::Process::State state) {
                switch (state) {
                        case Source::Process::State::Running: return "running";
                        case Source::Process::State::Vfork: return "vfork";
                        case Source::Process::State::Ending: return "ending";
                        case Source::Process::State::Zombie: return "zombie";
                        case Source::Process::State::Stopped: return "stopped";
                }
                return "?";
        }

        BoxLayout *page(Widget *parent) {
                auto *p = Widget::create<BoxLayout>(parent, BoxLayout::Direction::Vertical);
                if (p != nullptr) {
                        p->setMargins(4, 4, 4, 4);
                        p->setSpacing(4);
                }
                return p;
        }

        BoxLayout *row(Widget *parent) {
                auto *r = Widget::create<BoxLayout>(parent, BoxLayout::Direction::Horizontal);
                if (r != nullptr) r->setSpacing(4);
                return r;
        }

        DetailsView *table(Widget *parent, TableModel &model, Span<const TableModel::Column> columns) {
                model.setColumns(columns);
                auto *view = Widget::create<DetailsView>(parent);
                if (view == nullptr) return nullptr;
                view->setStretch(1);
                view->setModel(&model);
                return view;
        }

        // Insertion sort of @p rows (few) by @p less.
        template <typename T, typename Less> void sortRows(List<T> &rows, Less less) {
                for (size_t i = 1; i < rows.size(); ++i)
                        for (size_t j = i; j > 0 && less(rows[j], rows[j - 1]); --j) {
                                T t = rows[j];
                                rows[j] = rows[j - 1];
                                rows[j - 1] = t;
                        }
                return;
        }

} // namespace

SysmonWindow::SysmonWindow(GuiApplication &app, const Config &config, Monitor &monitor)
    : GuiWindow(app, config), _monitor(monitor) {}

SysmonWindow::~SysmonWindow() {
        if (_timer >= 0) application().stopTimer(_timer);
        if (_endTimer >= 0) application().stopTimer(_endTimer);
        (void)_monitor.source().watch(false);
}

// ---- Building -------------------------------------------------------------------------------

Error SysmonWindow::build() {
        auto *column = Widget::create<BoxLayout>(this, BoxLayout::Direction::Vertical);
        if (column == nullptr) return Error::NoMem;
        setCentral(column);
        _tabs = Widget::create<TabWidget>(column);
        _status = Widget::create<StatusBar>(column);
        if (_tabs == nullptr || _status == nullptr) return Error::NoMem;
        _tabs->setStretch(1);
        (void)_status->addField(0);
        (void)_status->addField(240);
        Error err = buildOverview();
        if (err.isOk()) err = buildCpu();
        if (err.isOk()) err = buildProcesses();
        if (err.isOk()) err = buildMemory();
        if (err.isOk()) err = buildGroup(_audio, Audio, TabTitles[Audio]);
        if (err.isOk()) err = buildGroup(_display, Display, TabTitles[Display]);
        if (err.isOk()) err = buildGroup(_io, Io, TabTitles[Io]);
        if (err.isOk()) err = buildGroup(_network, Network, TabTitles[Network]);
        if (err.isOk()) err = buildCounters();
        if (err.isError()) return err;
        _tabs->setChanged(tabChanged, this);
        IconTheme &theme = application().icons();
        setIcon(theme.find(IconId::UtilitiesSystemMonitor, 16));
        setLargeIcon(theme.find(IconId::UtilitiesSystemMonitor, 32));
        return Error::Ok;
}

Error SysmonWindow::buildOverview() {
        BoxLayout *p = page(_tabs);
        BoxLayout *graphs = p != nullptr ? row(p) : nullptr;
        if (graphs == nullptr) return Error::NoMem;
        for (Graph *&g : _overviewCpu) {
                g = Widget::create<Graph>(graphs);
                if (g == nullptr) return Error::NoMem;
                g->setStretch(1);
                g->setPreferredHeight(96);
        }
        for (size_t i = 0; i < 2; ++i) {
                BoxLayout *line = row(p);
                if (line == nullptr) return Error::NoMem;
                _memoryText[i] = Widget::create<Label>(line);
                _memoryBars[i] = Widget::create<ProgressBar>(line);
                if (_memoryText[i] == nullptr || _memoryBars[i] == nullptr) return Error::NoMem;
                _memoryText[i]->setMinimumWidth(400);
                _memoryBars[i]->setStretch(1);
        }
        for (Label *&l : _headline) {
                l = Widget::create<Label>(p);
                if (l == nullptr) return Error::NoMem;
        }
        static constexpr TableModel::Column Columns[] = {
                {"Counter", 260, Align::Left}, {"Health", 76, Align::Left},  {"State", 64, Align::Left},
                {"Began", 56, Align::Right},   {"Lasted", 64, Align::Right}, {"Count", 64, Align::Right},
                {"Value", 160, Align::Left},
        };
        _problemsView = table(p, _problems, Columns);
        BoxLayout *buttons = row(p);
        if (_problemsView == nullptr || buttons == nullptr) return Error::NoMem;
        auto *clear = Widget::create<Button>(buttons, "C&lear Ended Problems");
        if (clear == nullptr) return Error::NoMem;
        clear->setClicked(clearClicked, this);
        return _tabs->addTab(p, TabTitles[Overview]);
}

Error SysmonWindow::buildCpu() {
        BoxLayout *p = page(_tabs);
        BoxLayout *graphs = p != nullptr ? row(p) : nullptr;
        if (graphs == nullptr) return Error::NoMem;
        for (Graph *&g : _cpuGraphs) {
                g = Widget::create<Graph>(graphs);
                if (g == nullptr) return Error::NoMem;
                g->setStretch(1);
                g->setPreferredHeight(80);
        }
        static constexpr TableModel::Column Columns[] = {
                {"Task", 140, Align::Left},      {"Process", 64, Align::Right},    {"Core", 48, Align::Right},
                {"Priority", 72, Align::Right},  {"State", 88, Align::Left},       {"CPU", 72, Align::Right},
                {"CPU Time", 100, Align::Right}, {"Stack Free", 96, Align::Right},
        };
        _tasksView = table(p, _tasks, Columns);
        if (_tasksView == nullptr) return Error::NoMem;
        _tasksView->setSortAsked(tasksSort, this);
        _tasksView->setSortIndicator(_taskSort, _taskDescending);
        return _tabs->addTab(p, TabTitles[Cpu]);
}

Error SysmonWindow::buildProcesses() {
        BoxLayout *p = page(_tabs);
        if (p == nullptr) return Error::NoMem;
        static constexpr TableModel::Column Columns[] = {
                {"PID", 56, Align::Right},     {"Command", 200, Align::Left},   {"State", 80, Align::Left},
                {"CPU", 72, Align::Right},     {"CPU Time", 100, Align::Right}, {"Memory", 96, Align::Right},
                {"Threads", 72, Align::Right}, {"Parent", 64, Align::Right},
        };
        _procsView = table(p, _procs, Columns);
        BoxLayout *buttons = _procsView != nullptr ? row(p) : nullptr;
        if (buttons == nullptr) return Error::NoMem;
        _procsView->setSingleSelection(true);
        _procsView->setSortAsked(procsSort, this);
        _procsView->setSortIndicator(_procSort, _procDescending);
        _end = Widget::create<Button>(buttons, "&End Process");
        if (_end == nullptr) return Error::NoMem;
        _end->setClicked(endClicked, this);
        return _tabs->addTab(p, TabTitles[Processes]);
}

Error SysmonWindow::buildMemory() {
        BoxLayout *p = page(_tabs);
        BoxLayout *graphs = p != nullptr ? row(p) : nullptr;
        if (graphs == nullptr) return Error::NoMem;
        for (Graph *&g : _memGraphs) {
                g = Widget::create<Graph>(graphs);
                if (g == nullptr) return Error::NoMem;
                g->setStretch(1);
                g->setPreferredHeight(80);
                g->setColor(0, 0x40c0ff);
        }
        static constexpr TableModel::Column Spaces[] = {
                {"Space", 136, Align::Left}, {"Total", 96, Align::Right},   {"Used", 96, Align::Right},
                {"Free", 96, Align::Right},  {"Largest", 96, Align::Right}, {"Least Free", 96, Align::Right},
        };
        static constexpr TableModel::Column Caches[] = {
                {"Cache", 120, Align::Left},        {"Size", 96, Align::Right}, {"Items", 64, Align::Right},
                {"Reclaimable", 104, Align::Right}, {"Hits", 80, Align::Right}, {"Misses", 80, Align::Right},
                {"Reclaims", 80, Align::Right},
        };
        _spacesView = table(p, _spaces, Spaces);
        _cachesView = table(p, _caches, Caches);
        if (_spacesView == nullptr || _cachesView == nullptr) return Error::NoMem;
        return _tabs->addTab(p, TabTitles[Memory]);
}

Error SysmonWindow::buildGroup(GroupPage &group, Tab tab, const char *title) {
        BoxLayout *p = page(_tabs);
        BoxLayout *graphs = p != nullptr ? row(p) : nullptr;
        if (graphs == nullptr) return Error::NoMem;
        for (Graph *&g : group.graphs) {
                g = Widget::create<Graph>(graphs);
                if (g == nullptr) return Error::NoMem;
                g->setStretch(1);
                g->setPreferredHeight(80);
        }
        static constexpr TableModel::Column Columns[] = {
                {"Counter", 260, Align::Left}, {"Value", 140, Align::Right},         {"Rate", 128, Align::Right},
                {"Health", 76, Align::Left},   {"What It Counts", 480, Align::Left},
        };
        group.view = table(p, group.model, Columns);
        if (group.view == nullptr) return Error::NoMem;
        (void)tab;
        return _tabs->addTab(p, title);
}

Error SysmonWindow::buildCounters() {
        BoxLayout *p = page(_tabs);
        if (p == nullptr) return Error::NoMem;
        static constexpr TableModel::Column Columns[] = {
                {"Counter", 260, Align::Left},  {"Value", 140, Align::Right}, {"Rate", 128, Align::Right},
                {"Kind", 56, Align::Left},      {"Rule", 56, Align::Left},    {"Warning", 96, Align::Right},
                {"Critical", 96, Align::Right}, {"Health", 76, Align::Left},
        };
        _countersView = table(p, _counters, Columns);
        if (_countersView == nullptr) return Error::NoMem;
        return _tabs->addTab(p, TabTitles[Counters]);
}

// ---- Running --------------------------------------------------------------------------------

Error SysmonWindow::start() {
        if (_monitor.source().watch(true).isError()) say("The counters' sampler couldn't start: no rules, no history");
        auto [timer, err] = application().startTimer(RefreshMs, true, tick, this);
        if (err.isError()) return err;
        _timer = timer;
        refresh();
        return Error::Ok;
}

void SysmonWindow::raise() {
        if (!isShown()) (void)show();
        activate();
        refresh();
        return;
}

void SysmonWindow::raiseOverview() {
        if (_tabs != nullptr) _tabs->setCurrent(Overview);
        raise();
        return;
}

void SysmonWindow::tick(void *arg) {
        auto *self = static_cast<SysmonWindow *>(arg);
        // Nobody sees it: nothing read (the sampler goes on, so the graphs are whole when shown).
        if (!self->isShown() || self->application().windowManager().isMinimized(self->id())) return;
        self->refresh();
        return;
}

void SysmonWindow::tabChanged(void *arg, TabWidget &tabs, size_t index) {
        (void)tabs;
        (void)index;
        static_cast<SysmonWindow *>(arg)->refresh();
        return;
}

void SysmonWindow::refresh() {
        if (_monitor.refresh().isError()) {
                _status->setText(0, "Out of memory reading the counters");
                return;
        }
        switch (_tabs->current()) {
                case Overview: refreshOverview(); break;
                case Cpu: refreshCpu(); break;
                case Processes: refreshProcesses(); break;
                case Memory: refreshMemory(); break;
                case Audio: refreshGroup(_audio, AudioPatterns); break;
                case Display: refreshGroup(_display, DisplayPatterns); break;
                case Io: refreshGroup(_io, IoPatterns); break;
                case Network: refreshGroup(_network, NetworkPatterns); break;
                case Counters: refreshCounters(); break;
                default: break;
        }
        markTabs();
        const Source::Status &s = _monitor.status();
        char                  text[96];
        std::snprintf(text, sizeof(text), "%u counters, %u watching%s", unsigned(_monitor.count()),
                      unsigned(s.watchers), s.cpuCounted ? "" : "; CPU time isn't counted (sysmon/enable)");
        _status->setText(0, Clock::monotonicNs() < _messageUntilNs ? _message : text);
        std::snprintf(text, sizeof(text), "Sampled every %u ms", unsigned(s.intervalMs));
        _status->setText(1, text);
        return;
}

void SysmonWindow::say(const char *text) {
        std::snprintf(_message, sizeof(_message), "%s", text);
        _messageUntilNs = Clock::monotonicNs() + MessageNs;
        _status->setText(0, _message);
        return;
}

void SysmonWindow::markTabs() {
        for (size_t t = 0; t < TabCount; ++t) {
                Stat::Health worst = Stat::Health::Normal;
                if (t == Overview) {
                        worst = _monitor.status().unseen;
                } else {
                        for (const char *pattern : patternsOf(t)) {
                                const Stat::Health h = _monitor.worst(pattern);
                                worst = h > worst ? h : worst;
                        }
                }
                char title[TabWidget::MaxTitle + 1];
                std::snprintf(title, sizeof(title), "%s%s", TabTitles[t],
                              worst == Stat::Health::Critical  ? " (!!)"
                              : worst == Stat::Health::Warning ? " (!)"
                                                               : "");
                _tabs->setTitle(t, title);
        }
        return;
}

// A graph of counter @p name's history: a total's increases as a rate a second (with
// @p percentOfCore, µs a second as a share of a core), its levels drawn, its health the border.
void SysmonWindow::fillGraph(Graph *graph, StringView name, const char *label, bool percentOfCore) {
        const size_t i = _monitor.find(name);
        char         caption[Graph::MaxCaption + 1];
        if (i == Monitor::NotFound) {
                std::snprintf(caption, sizeof(caption), "%s: none", label);
                graph->setCaption(caption);
                graph->setSamples(0, Span<const int32_t>());
                graph->setHealth(Graph::Health::Normal);
                return;
        }
        const Source::Counter &c = _monitor.counter(i);
        int32_t                samples[HistoryDepth];
        const size_t           n = _monitor.source().history(c.id, Span<int32_t>(samples, HistoryDepth));
        const bool             total = c.kind == Stat::Kind::Total;
        const uint32_t         interval = _monitor.status().intervalMs > 0 ? _monitor.status().intervalMs : 1000;
        if (total)
                for (size_t k = 0; k < n; ++k) samples[k] = int32_t(int64_t(samples[k]) * 1000 / interval);
        graph->setSamples(0, Span<const int32_t>(samples, n));
        graph->setSlots(HistoryDepth);
        const bool levels = c.limits.rule == Stat::Rule::Above || c.limits.rule == Stat::Rule::Below;
        graph->setLevels(levels ? c.limits.warn : Graph::NoLevel, levels ? c.limits.crit : Graph::NoLevel);
        graph->setMaximum(percentOfCore ? 1000000 : 0);
        graph->setHealth(graphHealth(_monitor.health(i)));
        Text text;
        if (percentOfCore) {
                const int64_t r = _monitor.rate(i);
                if (r == Unavailable)
                        std::snprintf(text, sizeof(text), "-");
                else // a share of a core: two reads' timing can make it a little over
                        permille(text, uint64_t(r <= 0 ? 0 : r < 1000000 ? r : 1000000) / 1000);
        } else if (total) {
                _monitor.formatRate(text, i);
        } else {
                _monitor.formatValue(text, i);
        }
        std::snprintf(caption, sizeof(caption), "%s  %s", label, text);
        graph->setCaption(caption);
        return;
}

int64_t SysmonWindow::selectedKey(DetailsView *view, const TableModel &model) const {
        const int row = view->current();
        return row >= 0 && size_t(row) < model.count() && view->selection().isSelected(size_t(row))
                       ? model.key(size_t(row))
                       : INT64_MIN;
}

// The view shows the model filled again, the selected row (by key) kept.
void SysmonWindow::reset(DetailsView *view, TableModel &model, int64_t key) {
        // The same rows as shown: nothing to draw again (the selection stays as it is).
        if (!model.takeChanged()) return;
        const int row = key != INT64_MIN ? model.find(key) : -1;
        if (row >= 0) {
                const size_t rows[] = {size_t(row)};
                view->resetTo(rows, row);
        } else {
                view->resetTo(Span<const size_t>(), -1);
        }
        return;
}

// A task's or process's CPU share since the last refresh, in ‰ of a core.
uint64_t SysmonWindow::shareSince(List<Seen> &seen, int64_t key, uint64_t cpuUs, int64_t elapsedUs) const {
        for (const Seen &s : seen)
                if (s.key == key)
                        return elapsedUs > 0 && cpuUs >= s.cpuUs ? (cpuUs - s.cpuUs) * 1000 / uint64_t(elapsedUs) : 0;
        return 0;
}

void SysmonWindow::refreshOverview() {
        fillGraph(_overviewCpu[0], "cpu.core0.busy_us", "CPU 0", true);
        fillGraph(_overviewCpu[1], "cpu.core1.busy_us", "CPU 1", true);
        static constexpr const char *Spaces[] = {"internal", "psram"};
        static constexpr const char *Names[] = {"Internal RAM", "PSRAM"};
        for (size_t s = 0; s < 2; ++s) {
                char name[48];
                std::snprintf(name, sizeof(name), "mem.%s.total", Spaces[s]);
                const int64_t total = _monitor.valueOf(name);
                std::snprintf(name, sizeof(name), "mem.%s.used", Spaces[s]);
                const int64_t used = _monitor.valueOf(name);
                std::snprintf(name, sizeof(name), "mem.%s.largest", Spaces[s]);
                const int64_t largest = _monitor.valueOf(name);
                Text          t1, t2, t3;
                char          text[Label::MaxText + 1];
                std::snprintf(text, sizeof(text), "%s %s of %s, largest %s", Names[s],
                              Monitor::format(t1, Stat::Unit::Bytes, used),
                              Monitor::format(t2, Stat::Unit::Bytes, total),
                              Monitor::format(t3, Stat::Unit::Bytes, largest));
                _memoryText[s]->setText(text);
                _memoryBars[s]->setMaximum(total > 0 ? uint64_t(total) : 0);
                _memoryBars[s]->setValue(used > 0 ? uint64_t(used) : 0);
        }
        // The headline numbers.
        Text          a, b, c, d;
        const int64_t mix = _monitor.rateOf("audio.dsp0.mix_us");
        const int64_t late = _monitor.valueOf("audio.dsp0.late");
        const int64_t frames = _monitor.rateOf("display0.scanout.frames");
        const int64_t round = _monitor.valueOf("wm0.round_us_max");
        char          text[Label::MaxText + 1];
        std::snprintf(text, sizeof(text), "Audio %s, %s late    Display %s fps    Slowest round %s",
                      mix == Unavailable ? "-" : permille(a, uint64_t(mix > 0 ? mix : 0) / 1000),
                      Monitor::format(b, Stat::Unit::Count, late), Monitor::format(c, Stat::Unit::Count, frames),
                      Monitor::format(d, Stat::Unit::Micros, round));
        _headline[0]->setText(text);
        int64_t read = 0, written = 0;
        for (size_t i = 0; i < _monitor.count(); ++i) {
                const StringView name(_monitor.counter(i).name);
                // Whole devices only (a partition's bytes are its device's too): no 'p' after the number.
                if (!name.startsWith("storage.") || _monitor.rate(i) == Unavailable) continue;
                const StringView dev = name.mid(8, name.indexOf('.', 8) - 8);
                bool             partition = false;
                for (size_t k = 1; k < dev.size(); ++k)
                        if (dev[k] == 'p' && dev[k - 1] >= '0' && dev[k - 1] <= '9') partition = true;
                if (partition) continue;
                if (name.endsWith(".read_bytes")) read += _monitor.rate(i);
                if (name.endsWith(".written_bytes")) written += _monitor.rate(i);
        }
        // Every interface's traffic (net.ip, the stack's, counts packets, not bytes).
        int64_t received = 0, sent = 0;
        for (size_t i = 0; i < _monitor.count(); ++i) {
                const StringView name(_monitor.counter(i).name);
                if (!name.startsWith("net.") || _monitor.rate(i) == Unavailable) continue;
                if (name.endsWith(".rx_bytes")) received += _monitor.rate(i);
                if (name.endsWith(".tx_bytes")) sent += _monitor.rate(i);
        }
        std::snprintf(text, sizeof(text), "Storage: read %s/s, written %s/s",
                      Monitor::format(a, Stat::Unit::Bytes, read), Monitor::format(b, Stat::Unit::Bytes, written));
        _headline[1]->setText(text);
        std::snprintf(text, sizeof(text), "Network: in %s/s, out %s/s", Monitor::format(a, Stat::Unit::Bytes, received),
                      Monitor::format(b, Stat::Unit::Bytes, sent));
        _headline[2]->setText(text);

        // The problems: open ones first, then the rest, newest first (the log gives the oldest first).
        Stat::ProblemLog::Episode       episodes[ProblemBatch];
        List<Stat::ProblemLog::Episode> log(MemSpace::Psram);
        uint64_t                        after = 0;
        for (;;) {
                const size_t n =
                        _monitor.source().problems(after, Span<Stat::ProblemLog::Episode>(episodes, ProblemBatch));
                for (size_t k = 0; k < n; ++k) {
                        after = episodes[k].changed;
                        if (log.append(episodes[k]).isError()) break;
                }
                if (n < ProblemBatch) break;
        }
        const int64_t key = selectedKey(_problemsView, _problems);
        const int64_t nowMs = Clock::monotonicNs() / 1000000;
        _problems.clear();
        for (int pass = 0; pass < 2; ++pass) {
                for (size_t k = log.size(); k-- > 0;) {
                        const Stat::ProblemLog::Episode &e = log[k];
                        if (e.isOpen() != (pass == 0)) continue;
                        const char *state = e.isOpen()                                   ? "open"
                                            : (e.flags & Stat::ProblemLog::Earlier) != 0 ? "before"
                                                                                         : "ended";
                        Text        began, lasted, count, value;
                        ago(began, nowMs - e.startMs);
                        ago(lasted, e.endMs - e.startMs);
                        std::snprintf(count, sizeof(count), "%" PRIu64, e.count);
                        const size_t i = _monitor.find(e.name);
                        if (i != Monitor::NotFound) {
                                Text             first, last;
                                const Stat::Unit u = _monitor.counter(i).unit;
                                std::snprintf(value, sizeof(value), "%s to %s", Monitor::format(first, u, e.firstValue),
                                              Monitor::format(last, u, e.lastValue));
                        } else {
                                std::snprintf(value, sizeof(value), "%" PRId64 " to %" PRId64, e.firstValue,
                                              e.lastValue);
                        }
                        const StringView texts[] = {e.name, Stat::healthName(e.health), state, began, lasted, count,
                                                    value};
                        (void)_problems.add(texts, e.isOpen() ? emphasisOf(e.health) : Emphasis::None, int64_t(e.id));
                }
        }
        reset(_problemsView, _problems, key);
        // Shown: seen (the tray meter stops alarming for them).
        if (isActive()) _monitor.source().markSeen(_monitor.status().lastChange);
        return;
}

void SysmonWindow::refreshCpu() {
        fillGraph(_cpuGraphs[0], "cpu.core0.busy_us", "CPU 0", true);
        fillGraph(_cpuGraphs[1], "cpu.core1.busy_us", "CPU 1", true);
        // In PSRAM: the loop's stack is small.
        List<Source::Task> list(MemSpace::Psram);
        if (list.reserve(MaxTasks).isError()) return;
        for (size_t i = 0; i < MaxTasks; ++i) (void)list.append(Source::Task{});
        Source::Task *tasks = list.data();
        const size_t  total = _monitor.source().tasks(list.span());
        const size_t  n = total < MaxTasks ? total : MaxTasks;
        const int64_t now = Clock::monotonicNs();
        const int64_t elapsedUs = _taskSeenNs != 0 ? (now - _taskSeenNs) / 1000 : 0;
        struct Row {
                        const Source::Task *task;
                        uint64_t            share;
        };
        List<Row> rows(MemSpace::Psram);
        if (rows.reserve(n).isError()) return;
        for (size_t i = 0; i < n; ++i)
                (void)rows.append(Row{&tasks[i], shareSince(_taskSeen, tasks[i].number, tasks[i].cpuUs, elapsedUs)});
        const int  sort = _taskSort;
        const bool down = _taskDescending;
        sortRows(rows, [sort, down](const Row &x, const Row &y) {
                const Source::Task &a = down ? *y.task : *x.task;
                const Source::Task &b = down ? *x.task : *y.task;
                const uint64_t      as = down ? y.share : x.share, bs = down ? x.share : y.share;
                switch (sort) {
                        case 0: return std::strcmp(a.name, b.name) < 0;
                        case 1: return a.pid < b.pid;
                        case 2: return a.core < b.core;
                        case 3: return a.priority < b.priority;
                        case 4: return a.state < b.state;
                        case 5: return as < bs;
                        case 6: return a.cpuUs < b.cpuUs;
                        default: return a.stackFree < b.stackFree;
                }
        });
        const int64_t key = selectedKey(_tasksView, _tasks);
        _tasks.clear();
        const bool counted = _monitor.status().cpuCounted;
        for (const Row &r : rows) {
                const Source::Task &t = *r.task;
                Text                pid, core, prio, share, time, stack;
                std::snprintf(pid, sizeof(pid), t.pid > 0 ? "%d" : "kernel", int(t.pid));
                std::snprintf(core, sizeof(core), t.core >= 0 ? "%d" : "any", int(t.core));
                std::snprintf(prio, sizeof(prio), t.priority != t.basePriority ? "%u (%u)" : "%u", unsigned(t.priority),
                              unsigned(t.basePriority));
                if (counted && elapsedUs > 0)
                        permille(share, r.share);
                else
                        std::snprintf(share, sizeof(share), "-");
                if (counted)
                        duration(time, t.cpuUs);
                else
                        std::snprintf(time, sizeof(time), "-");
                Monitor::format(stack, Stat::Unit::Bytes, t.stackFree);
                const StringView texts[] = {t.name, pid, core, prio, taskState(t.state), share, time, stack};
                (void)_tasks.add(texts, emphasisOf(stackHealth(t.stackFree)), int64_t(t.number));
        }
        reset(_tasksView, _tasks, key);
        _taskSeen.clear();
        for (size_t i = 0; i < n; ++i) (void)_taskSeen.append(Seen{int64_t(tasks[i].number), tasks[i].cpuUs});
        _taskSeenNs = now;
        return;
}

void SysmonWindow::refreshProcesses() {
        const int64_t now = Clock::monotonicNs();
        const int64_t elapsedUs = _procSeenNs != 0 ? (now - _procSeenNs) / 1000 : 0;
        struct Row {
                        Source::Process proc;
                        uint64_t        share;
        };
        List<Row>       rows(MemSpace::Psram);
        Source::Process p;
        for (int32_t pid = 0; _monitor.source().process(pid, true, p).isOk(); pid = p.pid)
                if (rows.append(Row{p, shareSince(_procSeen, p.pid, p.cpuUs, elapsedUs)}).isError()) return;
        const int  sort = _procSort;
        const bool down = _procDescending;
        sortRows(rows, [sort, down](const Row &x, const Row &y) {
                const Row &a = down ? y : x;
                const Row &b = down ? x : y;
                switch (sort) {
                        case 1: return std::strcmp(a.proc.command, b.proc.command) < 0;
                        case 2: return a.proc.state < b.proc.state;
                        case 3: return a.share < b.share;
                        case 4: return a.proc.cpuUs < b.proc.cpuUs;
                        case 5: return a.proc.memory < b.proc.memory;
                        case 6: return a.proc.threads < b.proc.threads;
                        case 7: return a.proc.parent < b.proc.parent;
                        default: return a.proc.pid < b.proc.pid;
                }
        });
        const int64_t key = selectedKey(_procsView, _procs);
        _procs.clear();
        const bool counted = _monitor.status().cpuCounted;
        for (const Row &r : rows) {
                const Source::Process &q = r.proc;
                Text                   pid, share, time, memory, threads, parent;
                std::snprintf(pid, sizeof(pid), "%d", int(q.pid));
                if (counted && elapsedUs > 0 && q.state != Source::Process::State::Zombie)
                        permille(share, r.share);
                else
                        std::snprintf(share, sizeof(share), "-");
                if (counted)
                        duration(time, q.cpuUs);
                else
                        std::snprintf(time, sizeof(time), "-");
                Monitor::format(memory, Stat::Unit::Bytes, int64_t(q.memory));
                std::snprintf(threads, sizeof(threads), "%u", unsigned(q.threads));
                std::snprintf(parent, sizeof(parent),
                              q.parent > 0   ? "%d"
                              : q.parent < 0 ? "kernel"
                                             : "-",
                              int(q.parent));
                const StringView texts[] = {pid,
                                            q.state == Source::Process::State::Zombie ? "<ended>" : q.command,
                                            processState(q.state),
                                            share,
                                            time,
                                            memory,
                                            threads,
                                            parent};
                (void)_procs.add(texts, Emphasis::None, q.pid);
        }
        reset(_procsView, _procs, key);
        _procSeen.clear();
        for (const Row &r : rows) (void)_procSeen.append(Seen{r.proc.pid, r.proc.cpuUs});
        _procSeenNs = now;
        return;
}

void SysmonWindow::refreshMemory() {
        fillGraph(_memGraphs[0], "mem.internal.used", "Internal RAM used");
        fillGraph(_memGraphs[1], "mem.psram.used", "PSRAM used");
        // A fixed scale: each space's size.
        const int64_t internal = _monitor.valueOf("mem.internal.total");
        const int64_t psram = _monitor.valueOf("mem.psram.total");
        _memGraphs[0]->setMaximum(internal > 0 ? internal : 0);
        _memGraphs[1]->setMaximum(psram > 0 ? psram : 0);
        static constexpr const char *Spaces[] = {"internal", "internal_dma", "psram", "psram_dma", "fast"};
        static constexpr const char *Fields[] = {"total", "used", "free", "largest", "min_free"};
        _spaces.clear();
        for (const char *space : Spaces) {
                Text         values[5];
                Stat::Health worst = Stat::Health::Normal;
                for (size_t f = 0; f < 5; ++f) {
                        char name[48];
                        std::snprintf(name, sizeof(name), "mem.%s.%s", space, Fields[f]);
                        const size_t i = _monitor.find(name);
                        Monitor::format(values[f], Stat::Unit::Bytes,
                                        i != Monitor::NotFound ? _monitor.value(i) : Unavailable);
                        if (i != Monitor::NotFound && _monitor.health(i) > worst) worst = _monitor.health(i);
                }
                const StringView texts[] = {space, values[0], values[1], values[2], values[3], values[4]};
                (void)_spaces.add(texts, emphasisOf(worst));
        }
        _spacesView->modelReset();
        static constexpr const char *CacheFields[] = {"bytes", "items", "reclaimable", "hits", "misses", "reclaims"};
        _caches.clear();
        for (size_t i = 0; i < _monitor.count(); ++i) {
                const StringView name(_monitor.counter(i).name);
                if (!name.startsWith("cache.") || !name.endsWith(".bytes")) continue;
                const StringView group = name.left(name.size() - 6);
                Text             values[6];
                for (size_t f = 0; f < 6; ++f) {
                        char full[64];
                        std::snprintf(full, sizeof(full), "%.*s.%s", int(group.size()), group.data(), CacheFields[f]);
                        const size_t k = _monitor.find(full);
                        if (k == Monitor::NotFound)
                                std::snprintf(values[f], sizeof(values[f]), "-");
                        else
                                _monitor.formatValue(values[f], k);
                }
                const StringView texts[] = {group.mid(6), values[0], values[1], values[2],
                                            values[3],    values[4], values[5]};
                (void)_caches.add(texts);
        }
        _cachesView->modelReset();
        return;
}

void SysmonWindow::refreshGroup(GroupPage &group, Span<const char *const> patterns) {
        if (&group == &_audio) {
                fillGraph(group.graphs[0], "audio.dsp0.mix_us", "Mixing", true);
                fillGraph(group.graphs[1], "audio.dsp0.streams", "Streams");
        } else if (&group == &_display) {
                fillGraph(group.graphs[0], "display0.scanout.frames", "Frames a second");
                fillGraph(group.graphs[1], "wm0.round_us", "Compositor", true);
        } else if (&group == &_network) {
                fillGraph(group.graphs[0], "net.eth0.rx_bytes", "eth0 received");
                fillGraph(group.graphs[1], "net.eth0.tx_bytes", "eth0 sent");
        } else {
                fillGraph(group.graphs[0], "storage.sd0.written_bytes", "sd0 written");
                fillGraph(group.graphs[1], "input.pointer_events", "Pointer events");
        }
        const int64_t key = selectedKey(group.view, group.model);
        group.model.clear();
        for (size_t i = 0; i < _monitor.count(); ++i) {
                const Source::Counter &c = _monitor.counter(i);
                bool                   shown = false;
                for (const char *pattern : patterns) shown = shown || Stat::Overrides::matches(pattern, c.name);
                if (!shown) continue;
                Text value, rate;
                _monitor.formatValue(value, i);
                _monitor.formatRate(rate, i);
                const Stat::Health h = _monitor.health(i);
                const StringView   texts[] = {c.name, value, rate, _monitor.isJudged(i) ? Stat::healthName(h) : "-",
                                              c.help};
                (void)group.model.add(texts, emphasisOf(h), int64_t(c.id));
        }
        reset(group.view, group.model, key);
        return;
}

void SysmonWindow::refreshCounters() {
        const int64_t key = selectedKey(_countersView, _counters);
        _counters.clear();
        for (size_t i = 0; i < _monitor.count(); ++i) {
                const Source::Counter &c = _monitor.counter(i);
                Text                   value, rate, warn, crit;
                _monitor.formatValue(value, i);
                _monitor.formatRate(rate, i);
                std::snprintf(warn, sizeof(warn), "-");
                std::snprintf(crit, sizeof(crit), "-");
                if (c.limits.rule == Stat::Rule::Zero) {
                        std::snprintf(c.limits.health == Stat::Health::Critical ? crit : warn, sizeof(warn), "any");
                } else if (c.limits.rule == Stat::Rule::State) {
                        if (c.limits.warn != Stat::NoLevel)
                                std::snprintf(warn, sizeof(warn), "0x%" PRIx64, c.limits.warn);
                        if (c.limits.crit != Stat::NoLevel)
                                std::snprintf(crit, sizeof(crit), "0x%" PRIx64, c.limits.crit);
                } else if (c.limits.rule != Stat::Rule::None) {
                        // A total's levels are for its rate.
                        const char *per = c.kind == Stat::Kind::Total ? "/s" : "";
                        Text        level;
                        if (c.limits.warn != Stat::NoLevel)
                                std::snprintf(warn, sizeof(warn), "%s%s", Monitor::format(level, c.unit, c.limits.warn),
                                              per);
                        if (c.limits.crit != Stat::NoLevel)
                                std::snprintf(crit, sizeof(crit), "%s%s", Monitor::format(level, c.unit, c.limits.crit),
                                              per);
                }
                const Stat::Health h = _monitor.health(i);
                const StringView   texts[] = {c.name,
                                              value,
                                              rate,
                                              Stat::kindName(c.kind),
                                              Stat::ruleName(c.limits.rule),
                                              warn,
                                              crit,
                                            _monitor.isJudged(i) ? Stat::healthName(h) : "-"};
                (void)_counters.add(texts, emphasisOf(h), int64_t(c.id));
        }
        reset(_countersView, _counters, key);
        return;
}

// ---- Actions --------------------------------------------------------------------------------

void SysmonWindow::clearClicked(void *arg, Button &button) {
        (void)button;
        auto *self = static_cast<SysmonWindow *>(arg);
        self->_monitor.source().clearProblems();
        self->refresh();
        return;
}

void SysmonWindow::endClicked(void *arg, Button &button) {
        (void)button;
        auto         *self = static_cast<SysmonWindow *>(arg);
        const int64_t pid = self->selectedKey(self->_procsView, self->_procs);
        if (pid <= 0) {
                self->say("Choose a process to end");
                return;
        }
        const Error err = self->_monitor.source().signal(int32_t(pid), BMDS_SIGTERM);
        char        text[96];
        if (err.isError()) {
                std::snprintf(text, sizeof(text), "Process %d couldn't be ended: %s", int(pid), err.desc());
                self->say(text);
                return;
        }
        std::snprintf(text, sizeof(text), "Process %d asked to end (SIGTERM)", int(pid));
        self->say(text);
        // Still there 2 s on: killed.
        if (self->_endTimer >= 0) self->application().stopTimer(self->_endTimer);
        self->_ending = int32_t(pid);
        auto [timer, terr] = self->application().startTimer(KillAfterMs, false, endLater, self);
        self->_endTimer = terr.isOk() ? timer : -1;
        return;
}

void SysmonWindow::endLater(void *arg) {
        auto *self = static_cast<SysmonWindow *>(arg);
        self->_endTimer = -1;
        Source::Process p;
        if (self->_ending > 0 && self->_monitor.source().process(self->_ending, false, p).isOk() &&
            p.state != Source::Process::State::Zombie) {
                char text[96];
                std::snprintf(text, sizeof(text), "Process %d didn't end: killed (SIGKILL)", int(self->_ending));
                (void)self->_monitor.source().signal(self->_ending, BMDS_SIGKILL);
                self->say(text);
        }
        self->_ending = 0;
        self->refresh();
        return;
}

void SysmonWindow::tasksSort(void *arg, DetailsView &view, size_t column) {
        auto *self = static_cast<SysmonWindow *>(arg);
        self->_taskDescending = int(column) == self->_taskSort ? !self->_taskDescending : column >= 5;
        self->_taskSort = int(column);
        view.setSortIndicator(int(column), self->_taskDescending);
        self->refresh();
        return;
}

void SysmonWindow::procsSort(void *arg, DetailsView &view, size_t column) {
        auto *self = static_cast<SysmonWindow *>(arg);
        self->_procDescending = int(column) == self->_procSort ? !self->_procDescending : column >= 3 && column <= 6;
        self->_procSort = int(column);
        view.setSortIndicator(int(column), self->_procDescending);
        self->refresh();
        return;
}

BMDS_NAMESPACE_END
