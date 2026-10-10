/**
 * @file      topview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `top`'s frame (devplan/sysmon.md §3.5, step 6): shares, times, sizes, and the lines of its
 * batch form, which is a userspace interface.
 */

#include <doctest.h>

#include <algorithm>
#include <string>

#include <bmds/topview.h>

namespace TopView = bmds::TopView;

TEST_CASE("TopView: a share of a core from two readings") {
        CHECK(TopView::share(1500000, 1000000, 1000000) == 500);
        CHECK(TopView::share(2003000, 1000000, 1000000) == 1000); // a little over: capped
        CHECK(TopView::share(1000000, 1000000, 1000000) == 0);
        CHECK(TopView::share(900, 1000, 1000000) == 0); // went back (a task gone, its number reused)
        CHECK(TopView::share(2000, 1000, 0) == 0);
        CHECK(TopView::share(1000 + 1500, 1000, 1000000) == 2); // 0.15 %, rounded
}

TEST_CASE("TopView: times, uptimes and sizes") {
        char t[TopView::TextBytes];
        CHECK(std::string(TopView::formatTime(t, 68420000)) == "1:08.42");
        CHECK(std::string(TopView::formatTime(t, 0)) == "0:00.00");
        CHECK(std::string(TopView::formatTime(t, 3723000000ull)) == "1:02:03");
        CHECK(std::string(TopView::formatUptime(t, 3723000000ull)) == "1:02:03");
        CHECK(std::string(TopView::formatUptime(t, (86400ull + 61) * 1000000)) == "1 day, 0:01:01");
        CHECK(std::string(TopView::formatUptime(t, 2 * 86400ull * 1000000)) == "2 days, 0:00:00");
        CHECK(std::string(TopView::formatBytes(t, 812)) == "812 B");
        CHECK(std::string(TopView::formatBytes(t, 173 * 1024 + 30)) == "173.0 KB");
        CHECK(std::string(TopView::formatBytes(t, 32ull * 1024 * 1024)) == "32.0 MB");
        CHECK(std::string(TopView::formatShare(t, 999)) == "99.9");
        CHECK(std::string(TopView::formatShare(t, 5)) == "0.5");
}

TEST_CASE("TopView: the batch form's lines") {
        char line[TopView::LineBytes];
        CHECK(std::string(TopView::summaryLine(line, 3723000000ull, 3, 16, 0)) ==
              "top - up 1:02:03, 3 processes, 16 tasks, 0 open problems");
        CHECK(std::string(TopView::summaryLine(line, 0, 1, 1, 1)) ==
              "top - up 0:00:00, 1 process, 1 task, 1 open problem");
        const uint32_t shares[] = {31, 999};
        CHECK(std::string(TopView::coresLine(line, shares, 2)) == "CPU0   3.1%  CPU1  99.9%");
        CHECK(std::string(TopView::memoryLine(line, "psram", 6815744, 33554432, 26214400, 25165824)) ==
              "psram        6.5 MB of 32.0 MB, largest 25.0 MB, lowest free 24.0 MB");
        char heading[TopView::LineBytes];
        CHECK(std::string(TopView::headingLine(heading)) ==
              "  PID NAME                   CPU%      TIME    MEMORY  THR STATE");

        TopView::Row p;
        p.pid = 5;
        p.name = "yes";
        p.share = 999;
        p.cpuUs = 68420000;
        p.memory = 12 * 1024;
        p.threads = 1;
        p.state = "running";
        CHECK(std::string(TopView::rowLine(line, p)) ==
              "    5 yes                    99.9   1:08.42   12.0 KB    1 running");
        TopView::Row t = p;
        t.task = true;
        t.memory = TopView::Row::NoMemory;
        t.threads = -1;
        t.name = "a-name-much-longer-than-the-column";
        CHECK(std::string(TopView::rowLine(line, t)) ==
              "    5   \\_ a-name-much-lon   99.9   1:08.42         -    - running");
        // The columns line up with the heading.
        CHECK(std::string(line).find("99.9") == std::string(heading).find("CPU%"));
        CHECK(std::string(line).find("running") == std::string(heading).find("STATE"));
}

TEST_CASE("TopView: the busiest first, then by pid") {
        TopView::Row rows[3];
        rows[0].pid = 3;
        rows[0].share = 10;
        rows[1].pid = 1;
        rows[1].share = 10;
        rows[2].pid = 7;
        rows[2].share = 900;
        std::sort(rows, rows + 3, TopView::isBefore);
        CHECK(rows[0].pid == 7);
        CHECK(rows[1].pid == 1);
        CHECK(rows[2].pid == 3);
        // A process's tasks of the same share: by name, so the frames are stable.
        TopView::Row tasks[2];
        tasks[0].name = "wm";
        tasks[1].name = "stats";
        std::sort(tasks, tasks + 2, TopView::isBefore);
        CHECK(std::string(tasks[0].name) == "stats");
}
