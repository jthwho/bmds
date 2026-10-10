/**
 * @file      notifycommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `notify` command (devplan/notifications.md §3.3): posts a notification, as notify-send does,
 * and lists, closes or clears those kept. Its output format is a userspace interface: keep the
 * columns and their order, and add new ones only at the end.
 */

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/notifications.h>
#include <bmds/programlauncher.h>
#include <bmds/shell.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Args = Span<char *const>;

        int32_t ownerOf(CommandContext &ctx) {
                const ProcessControl *p = ctx.shell().process();
                return p != nullptr ? p->processId() : 0;
        }

        template <size_t N> void copy(char (&out)[N], const char *text) {
                std::snprintf(out, N, "%s", text);
                return;
        }

        const char *urgencyName(Notification::Urgency u) {
                return u == Notification::Urgency::Low        ? "low"
                       : u == Notification::Urgency::Critical ? "critical"
                                                              : "normal";
        }

        // "12s", "3m", "2h", "4d": how long ago.
        void ago(char (&out)[16], int64_t ns) {
                const int64_t s = ns > 0 ? ns / 1000000000 : 0;
                if (s < 120)
                        std::snprintf(out, sizeof(out), "%" PRId64 "s", s);
                else if (s < 2 * 3600)
                        std::snprintf(out, sizeof(out), "%" PRId64 "m", s / 60);
                else if (s < 2 * 86400)
                        std::snprintf(out, sizeof(out), "%" PRId64 "h", s / 3600);
                else
                        std::snprintf(out, sizeof(out), "%" PRId64 "d", s / 86400);
                return;
        }

        int list(CommandContext &ctx) {
                uint32_t     ids[NotificationLog::Capacity];
                const size_t total = Notifications::ids(Span<uint32_t>(ids, NotificationLog::Capacity));
                const size_t n = total < NotificationLog::Capacity ? total : NotificationLog::Capacity;
                ctx.printf("%6s %-8s %-7s %-4s %5s %-16s %s\n", "ID", "URGENCY", "STATE", "READ", "AGO", "APP",
                           "TITLE");
                const int64_t now = Clock::now().ns;
                for (size_t i = 0; i < n; ++i) {
                        Notification note;
                        if (Notifications::get(ids[i], note).isError()) continue;
                        char a[16];
                        ago(a, now - note.postedNs);
                        ctx.printf("%6" PRIu32 " %-8s %-7s %-4s %5s %-16s %s\n", note.id, urgencyName(note.urgency),
                                   note.balloon ? "balloon" : "kept", note.read ? "yes" : "no", a,
                                   note.app[0] != '\0' ? note.app : "-", note.title);
                }
                return Shell::ExitSuccess;
        }

        int post(CommandContext &ctx, Args args) {
                Notification n;
                copy(n.app, "notify");
                size_t i = 1;
                for (; i < args.size() && args[i][0] == '-' && args[i][1] != '\0'; ++i) {
                        const StringView o(args[i]);
                        if (o == "--") {
                                ++i;
                                break;
                        }
                        if (i + 1 >= args.size()) return ctx.usage();
                        const char *v = args[++i];
                        if (o == "-u") {
                                const StringView u(v);
                                if (u == "low") {
                                        n.urgency = Notification::Urgency::Low;
                                } else if (u == "normal") {
                                        n.urgency = Notification::Urgency::Normal;
                                } else if (u == "critical") {
                                        n.urgency = Notification::Urgency::Critical;
                                } else {
                                        return ctx.usage();
                                }
                        } else if (o == "-t") {
                                char      *end = nullptr;
                                const long ms = std::strtol(v, &end, 10);
                                if (end == v || *end != '\0' || ms < -1) return ctx.usage();
                                n.timeoutMs = int32_t(ms);
                        } else if (o == "-i") {
                                copy(n.icon, v);
                        } else if (o == "-a") {
                                copy(n.app, v);
                        } else if (o == "-r") {
                                n.replaces = uint32_t(std::strtoul(v, nullptr, 10));
                        } else if (o == "-o" || o == "-s") {
                                n.target = o == "-o" ? Notification::Target::Open : Notification::Target::Show;
                                copy(n.targetText, v);
                        } else if (o == "-l") {
                                n.target = Notification::Target::Launch;
                                copy(n.targetText, v);
                        } else {
                                return ctx.usage();
                        }
                }
                if (i >= args.size() || args.size() - i > 2) return ctx.usage();
                copy(n.title, args[i]);
                if (i + 1 < args.size()) copy(n.body, args[i + 1]);
                auto [id, err] = Notifications::post(n, ownerOf(ctx), nullptr);
                if (err.isError()) {
                        ctx.eprintf("notify: %s\n",
                                    err == Error::PermissionDenied ? "not yours to replace" : err.desc());
                        return Shell::ExitFailure;
                }
                ctx.printf("%" PRIu32 "\n", id);
                return Shell::ExitSuccess;
        }

        int runNotify(CommandContext &ctx) {
                const Args args = ctx.args();
                if (args.size() < 2) return ctx.usage();
                const StringView word(args[1]);
                if (word == "list" && args.size() == 2) return list(ctx);
                if (word == "clear" && args.size() == 2) {
                        Notifications::clear();
                        return Shell::ExitSuccess;
                }
                if (word == "close" && args.size() == 3) {
                        const Error err =
                                Notifications::close(uint32_t(std::strtoul(args[2], nullptr, 10)), ownerOf(ctx));
                        if (err.isError()) {
                                ctx.eprintf("notify: %s: %s\n", args[2],
                                            err == Error::PermissionDenied ? "not yours" : "no such notification");
                                return Shell::ExitFailure;
                        }
                        return Shell::ExitSuccess;
                }
                return post(ctx, args);
        }

} // namespace

BMDS_SHELL_COMMAND(notify, runNotify,
                   "[-u low|normal|critical] [-t MS] [-i ICON] [-a APP] [-r ID] [-o PATH | -s PATH | -l ENTRY] TITLE "
                   "[BODY] | list | close ID | clear",
                   "Post a notification (its id printed), or list, close or clear those kept")

BMDS_NAMESPACE_END
