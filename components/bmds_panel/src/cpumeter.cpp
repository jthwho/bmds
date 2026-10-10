/**
 * @file      cpumeter.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The tray's CPU meter (devplan/sysmon.md §3.5, step 5): the kernel's counters, read every
 * sampler interval, as a picture and a tooltip.
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <utility>
#include <bmds/clock.h>
#include <bmds/icon.h>
#include <bmds/menulayout.h>
#include <bmds/stats.h>
#include <bmds/sysconf.h>

#include "cpumeter.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr const char *ReadNames[] = {
                "system.cpu_counted", "mem.internal.used", "mem.internal.total", "mem.psram.used", "mem.psram.total",
        };

        constexpr size_t ProblemBatch = 8; // the newest changes looked through for the problem told

        // @p bytes as "812 KB" or "3.1 MB".
        void formatBytes(char (&out)[24], int64_t bytes) {
                if (bytes < 0) bytes = 0;
                if (bytes < 1024 * 1024) {
                        std::snprintf(out, sizeof(out), "%" PRId64 " KB", (bytes + 512) / 1024);
                } else {
                        const int64_t tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
                        std::snprintf(out, sizeof(out), "%" PRId64 ".%" PRId64 " MB", tenths / 10, tenths % 10);
                }
                return;
        }

        // @p ms as "12 s", "3 min" or "2 h".
        void formatSpan(char (&out)[16], int64_t ms) {
                const int64_t s = ms > 0 ? ms / 1000 : 0;
                if (s < 120)
                        std::snprintf(out, sizeof(out), "%" PRId64 " s", s);
                else if (s < 2 * 3600)
                        std::snprintf(out, sizeof(out), "%" PRId64 " min", s / 60);
                else
                        std::snprintf(out, sizeof(out), "%" PRId64 " h", s / 3600);
                return;
        }

} // namespace

CpuMeter::CpuMeter(GuiApplication &app, StartMonitor startMonitor, void *arg)
    : _app(app), _icon(app), _menu(app), _start(startMonitor), _startArg(arg) {
        _icon.setCategory(TrayIcon::Category::System);
        _icon.setActivated(activated, this);
        _icon.setContextMenu(&_menu);
        _menu.setTriggered(chosen, this);
        fillMenu();
}

CpuMeter::~CpuMeter() {
        _app.cancelPosts(this); // a Remove from Panel not taken yet
        if (_timer >= 0) _app.stopTimer(_timer);
        if (_watching) Stats::unwatch();
}

Error CpuMeter::start() {
        if (!_watching) {
                const Error err = Stats::watch();
                if (err.isError()) return err;
                _watching = true;
        }
        refresh(); // the picture before it is shown
        return _icon.show();
}

void CpuMeter::setForm(CpuMeterPicture::Form form) {
        if (form == _form) return;
        _form = form;
        fillMenu();
        if (_watching) refresh();
        return;
}

void CpuMeter::restartTimer(uint32_t intervalMs) {
        if (_timer >= 0 && intervalMs == _intervalMs) return;
        if (_timer >= 0) _app.stopTimer(_timer);
        const Result<int> timer = _app.startTimer(intervalMs, true, tick, this);
        _timer = timer.isOk() ? timer.value() : -1;
        _intervalMs = intervalMs;
        return;
}

void CpuMeter::tick(void *arg) {
        static_cast<CpuMeter *>(arg)->refresh();
        return;
}

void CpuMeter::findCounters() {
        _generation = Stats::generation();
        _coreCount = 0;
        for (size_t c = 0; c < MaxCores; ++c) {
                char name[32];
                std::snprintf(name, sizeof(name), "cpu.core%u.busy_us", unsigned(c));
                const uint32_t id = Stats::find(StringView(name));
                if (id == Stats::NoId) break;
                _cores[_coreCount++] = id;
        }
        for (size_t i = 0; i < ReadCount; ++i) _ids[i] = Stats::find(StringView(ReadNames[i]));
        return;
}

// The newest problem that is open, or ended and not one from before anyone watched: "NAME
// HEALTH, 12 s" (open, for how long) or "..., 40 s ago". @return The bytes written (0: none).
size_t CpuMeter::writeProblem(char *out, size_t size) {
        const uint64_t            last = Stats::lastProblemChange();
        Stat::ProblemLog::Episode episodes[ProblemBatch];
        const size_t n = Stats::problems(last > ProblemBatch ? last - ProblemBatch : 0, Span(episodes, ProblemBatch));
        const Stat::ProblemLog::Episode *newest = nullptr;
        for (size_t i = 0; i < n; ++i) {
                const Stat::ProblemLog::Episode &e = episodes[i];
                if (!e.isOpen() && (e.flags & Stat::ProblemLog::Earlier) != 0) continue;
                if (newest == nullptr || e.changed > newest->changed) newest = &e;
        }
        if (newest == nullptr) return 0;
        const int64_t nowMs = int64_t(Clock::monotonicNs() / 1000000);
        char          span[16];
        formatSpan(span, newest->isOpen() ? nowMs - newest->startMs : nowMs - newest->endMs);
        const int w = std::snprintf(out, size, "%s %s, %s%s", newest->name, Stat::healthName(newest->health), span,
                                    newest->isOpen() ? "" : " ago");
        return w < 0 ? 0 : size_t(w) < size ? size_t(w) : size - 1;
}

void CpuMeter::refresh() {
        const uint32_t intervalMs = Stats::interval();
        restartTimer(intervalMs);
        if (Stats::generation() != _generation || _coreCount == 0) findCounters();
        int64_t values[ReadCount];
        (void)Stats::read(Span<const uint32_t>(_ids, ReadCount), Span<int64_t>(values, ReadCount));
        const bool counted = values[Counted] == 1;
        _alarm = Stats::unseenProblems();

        // The picture.
        int32_t                samples[MaxCores][CpuMeterPicture::Samples];
        CpuMeterPicture::Input in;
        in.coreCount = _coreCount;
        for (size_t c = 0; c < _coreCount; ++c) {
                const size_t got = Stats::history(_cores[c], Span<int32_t>(samples[c], CpuMeterPicture::Samples));
                in.cores[c] = Span<const int32_t>(samples[c], got);
        }
        in.full = int64_t(intervalMs) * 1000;
        in.alarm = _alarm;
        in.counted = counted;
        in.form = _form;
        CpuMeterPicture::draw(in, _pixels);
        (void)_icon.setIcon(Icon{CpuMeterPicture::Side, CpuMeterPicture::Side, _pixels});

        // The tooltip: the cores' newest shares, the memory, the problem told.
        size_t n = 0;
        if (!counted) {
                n += size_t(std::snprintf(_tip, sizeof(_tip), "CPU time isn't measured (sysmon/enable)"));
        } else {
                n += size_t(std::snprintf(_tip, sizeof(_tip), "CPU"));
                for (size_t c = 0; c < _coreCount && n < sizeof(_tip); ++c) {
                        const Span<const int32_t> s = in.cores[c];
                        const char               *sep = c == 0 ? " " : " · ";
                        if (s.size() == 0) {
                                n += size_t(std::snprintf(_tip + n, sizeof(_tip) - n, "%s-", sep));
                        } else {
                                int64_t pct = (int64_t(s[s.size() - 1]) * 100 + in.full / 2) / in.full;
                                pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
                                n += size_t(std::snprintf(_tip + n, sizeof(_tip) - n, "%s%" PRId64 "%%", sep, pct));
                        }
                }
        }
        if (n < sizeof(_tip)) {
                int64_t used = 0, total = 0;
                for (const auto &[u, t] : {std::pair{InternalUsed, InternalTotal}, std::pair{PsramUsed, PsramTotal}}) {
                        if (values[u] != Stat::Unavailable) used += values[u];
                        if (values[t] != Stat::Unavailable) total += values[t];
                }
                char a[24], b[24];
                formatBytes(a, used);
                formatBytes(b, total);
                n += size_t(std::snprintf(_tip + n, sizeof(_tip) - n, "\nMemory: %s of %s", a, b));
        }
        if (_alarm != Stat::Health::Normal && n + 1 < sizeof(_tip)) {
                _tip[n++] = '\n';
                const size_t w = writeProblem(_tip + n, sizeof(_tip) - n);
                n = w > 0 ? n + w : n - 1;
                _tip[n < sizeof(_tip) ? n : sizeof(_tip) - 1] = '\0';
        }
        _icon.setToolTip(StringView(_tip));
        return;
}

// A click: the monitor raised (at Overview while alarming: shown, the problems are seen), else
// started (a new one opens at Overview).
void CpuMeter::activated(void *arg, TrayIcon &, TrayIcon::Reason reason) {
        if (reason == TrayIcon::Reason::Trigger)
                static_cast<CpuMeter *>(arg)->openMonitor(); // a double click came as one first
        return;                                              // Context: its menu, opened by the icon
}

void CpuMeter::openMonitor() {
        const StringView message(_alarm != Stat::Health::Normal ? "overview" : "raise");
        const Error      sent = _app.windowManager().sendToService(
                StringView(MonitorService),
                ByteSpan(reinterpret_cast<const uint8_t *>(message.data()), message.size()));
        if (sent.isError() && _start != nullptr) _start(_startArg);
        return;
}

void CpuMeter::fillMenu() {
        _menu.clear();
        (void)_menu.addItem("&System Monitor", ChooseMonitor);
        (void)_menu.addSeparator();
        (void)_menu.addItem("Graph Each &Core", ChooseCores,
                            _form == CpuMeterPicture::Form::Cores ? MenuLayout::Checked : 0);
        (void)_menu.addItem("Graph &Both Together", ChooseCombined,
                            _form == CpuMeterPicture::Form::Combined ? MenuLayout::Checked : 0);
        (void)_menu.addSeparator();
        (void)_menu.addItem("&Remove from Panel", ChooseRemove);
        return;
}

// Set, and kept across a restart. The panel follows its settings: the meter is told the form
// (setForm()), or destroyed when panel/tray/cpu goes false.
void CpuMeter::save(const char *key, StringView value) {
        if (SysConf::set(StringView(key), value).isOk()) (void)SysConf::save(StringView(key));
        return;
}

void CpuMeter::chosen(void *arg, int item) {
        auto *self = static_cast<CpuMeter *>(arg);
        switch (item) {
                case ChooseMonitor: self->openMonitor(); break;
                case ChooseCores: self->save(GraphKey, "cores"); break;
                case ChooseCombined: self->save(GraphKey, "combined"); break;
                case ChooseRemove:
                        // After the menu's call returns: the panel destroys the meter (and this menu).
                        if (self->_app.post(removeDue, self).isError()) removeDue(self);
                        break;
                default: break;
        }
        return;
}

void CpuMeter::removeDue(void *arg) {
        static_cast<CpuMeter *>(arg)->save(ShownKey, "false"); // the panel's menu shows it again
        return;
}

BMDS_NAMESPACE_END
