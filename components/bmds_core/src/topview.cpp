/**
 * @file      topview.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `top`'s frame (devplan/sysmon.md §3.5, step 6).
 */

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <bmds/topview.h>

BMDS_NAMESPACE_BEGIN

namespace TopView {

        uint32_t share(uint64_t nowUs, uint64_t thenUs, int64_t elapsedUs) {
                if (elapsedUs <= 0 || nowUs <= thenUs) return 0;
                const uint64_t permille = ((nowUs - thenUs) * 1000 + uint64_t(elapsedUs) / 2) / uint64_t(elapsedUs);
                return permille > 1000 ? 1000 : uint32_t(permille); // two readings' timing can make it a little over
        }

        bool isBefore(const Row &a, const Row &b) {
                if (a.share != b.share) return a.share > b.share;
                if (a.pid != b.pid) return a.pid < b.pid;
                return std::strcmp(a.name != nullptr ? a.name : "", b.name != nullptr ? b.name : "") < 0;
        }

        const char *formatShare(char (&out)[TextBytes], uint32_t permille) {
                std::snprintf(out, sizeof(out), "%u.%u", unsigned(permille / 10), unsigned(permille % 10));
                return out;
        }

        const char *formatTime(char (&out)[TextBytes], uint64_t us) {
                const uint64_t hundredths = us / 10000;
                const uint64_t s = hundredths / 100;
                if (s < 3600)
                        std::snprintf(out, sizeof(out), "%" PRIu64 ":%02" PRIu64 ".%02" PRIu64, s / 60, s % 60,
                                      hundredths % 100);
                else
                        std::snprintf(out, sizeof(out), "%" PRIu64 ":%02" PRIu64 ":%02" PRIu64, s / 3600, s / 60 % 60,
                                      s % 60);
                return out;
        }

        const char *formatUptime(char (&out)[TextBytes], uint64_t us) {
                const uint64_t s = us / 1000000;
                const uint64_t days = s / 86400;
                const uint64_t h = s / 3600 % 24, m = s / 60 % 60, sec = s % 60;
                if (days == 0)
                        std::snprintf(out, sizeof(out), "%" PRIu64 ":%02" PRIu64 ":%02" PRIu64, h, m, sec);
                else
                        std::snprintf(out, sizeof(out), "%" PRIu64 " day%s, %" PRIu64 ":%02" PRIu64 ":%02" PRIu64, days,
                                      days == 1 ? "" : "s", h, m, sec);
                return out;
        }

        const char *formatBytes(char (&out)[TextBytes], uint64_t bytes) {
                if (bytes < 1024) {
                        std::snprintf(out, sizeof(out), "%" PRIu64 " B", bytes);
                } else if (bytes < 1024 * 1024) {
                        const uint64_t tenths = (bytes * 10 + 512) / 1024;
                        std::snprintf(out, sizeof(out), "%" PRIu64 ".%" PRIu64 " KB", tenths / 10, tenths % 10);
                } else {
                        const uint64_t tenths = (bytes * 10 + 512 * 1024) / (1024 * 1024);
                        std::snprintf(out, sizeof(out), "%" PRIu64 ".%" PRIu64 " MB", tenths / 10, tenths % 10);
                }
                return out;
        }

        namespace {
                constexpr const char *RowFormat = "%5s %-20s %6s %9s %9s %4s %s";
        } // namespace

        const char *headingLine(char (&out)[LineBytes]) {
                std::snprintf(out, sizeof(out), RowFormat, "PID", "NAME", "CPU%", "TIME", "MEMORY", "THR", "STATE");
                return out;
        }

        const char *rowLine(char (&out)[LineBytes], const Row &row) {
                char name[TextBytes + 8];
                std::snprintf(name, sizeof(name), "%s%s", row.task ? "  \\_ " : "", row.name);
                name[20] = '\0'; // the column's width
                char share[TextBytes], time[TextBytes], memory[TextBytes], threads[16];
                formatShare(share, row.share);
                formatTime(time, row.cpuUs);
                if (row.memory == Row::NoMemory)
                        std::snprintf(memory, sizeof(memory), "-");
                else
                        formatBytes(memory, row.memory);
                if (row.threads < 0)
                        std::snprintf(threads, sizeof(threads), "-");
                else
                        std::snprintf(threads, sizeof(threads), "%ld", long(row.threads));
                char pid[16];
                std::snprintf(pid, sizeof(pid), "%ld", long(row.pid));
                std::snprintf(out, sizeof(out), RowFormat, pid, name, share, time, memory, threads, row.state);
                return out;
        }

        const char *summaryLine(char (&out)[LineBytes], uint64_t uptimeUs, size_t processes, size_t tasks,
                                size_t openProblems) {
                char up[TextBytes];
                std::snprintf(out, sizeof(out), "top - up %s, %u process%s, %u task%s, %u open problem%s",
                              formatUptime(up, uptimeUs), unsigned(processes), processes == 1 ? "" : "es",
                              unsigned(tasks), tasks == 1 ? "" : "s", unsigned(openProblems),
                              openProblems == 1 ? "" : "s");
                return out;
        }

        const char *coresLine(char (&out)[LineBytes], const uint32_t *shares, size_t cores) {
                size_t n = 0;
                out[0] = '\0';
                for (size_t c = 0; c < cores && n < sizeof(out); ++c) {
                        char      s[TextBytes];
                        const int w = std::snprintf(out + n, sizeof(out) - n, "%sCPU%u %5s%%", c == 0 ? "" : "  ",
                                                    unsigned(c), formatShare(s, shares[c]));
                        if (w < 0) break;
                        n += size_t(w);
                }
                return out;
        }

        const char *memoryLine(char (&out)[LineBytes], StringView space, uint64_t used, uint64_t total,
                               uint64_t largest, uint64_t lowestFree) {
                char u[TextBytes], t[TextBytes], l[TextBytes], f[TextBytes];
                std::snprintf(out, sizeof(out), "%-8.*s %10s of %s, largest %s, lowest free %s", int(space.size()),
                              space.data(), formatBytes(u, used), formatBytes(t, total), formatBytes(l, largest),
                              formatBytes(f, lowestFree));
                return out;
        }

} // namespace TopView

BMDS_NAMESPACE_END
