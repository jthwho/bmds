/**
 * @file      updatecommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `update` command (devplan/update.md §3): check a package, apply it, show what is pending and
 * the last result. Its output is a userspace interface (the headless contract, and what scripts
 * read): keep the lines and their order, and add new ones only at the end.
 */

#include <cstring>
#include <bmds/command.h>
#include <bmds/console.h>
#include <bmds/datetime.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/thread.h>
#include <bmds/update.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // How long the console gets to show the last lines before a restart.
        constexpr unsigned RestartFlushMs = 500;
        constexpr unsigned RecoveryPauseMs = 1500;

        const char *sectionLabel(uint32_t type) {
                switch (static_cast<UpdatePackage::SectionType>(type)) {
                        case UpdatePackage::SectionType::Firmware: return "Firmware:";
                        case UpdatePackage::SectionType::Rom: return "ROM:";
                        case UpdatePackage::SectionType::Recovery: return "Recovery:";
                        default: return "Section:";
                }
        }

        const char *partitionOf(uint32_t type) {
                switch (static_cast<UpdatePackage::SectionType>(type)) {
                        case UpdatePackage::SectionType::Firmware: return "ota_0";
                        case UpdatePackage::SectionType::Rom: return "rom";
                        case UpdatePackage::SectionType::Recovery: return "ota_1";
                        default: return nullptr;
                }
        }

        void printReport(CommandContext &ctx, StringView path, const Update::Report &r, Error err) {
                const UpdatePackage::Package &p = r.package;
                ctx.printf("Package:  %.*s\n", int(path.size()), path.data());
                if (p.sectionCount == 0 && r.problem == nullptr) return; // it couldn't be read at all
                ctx.printf("Board:    %s\n", p.board);
                ctx.printf("Version:  %u.%u.%u (%s), installed %s: %s\n", p.version.major, p.version.minor,
                           p.version.patch, p.versionText, r.mainVersion[0] != '\0' ? r.mainVersion : "none",
                           r.order > 0   ? "newer"
                           : r.order < 0 ? "older"
                                         : "the same");
                const DateTime t = DateTime::fromSeconds(p.madeAt);
                ctx.printf("Made:     %04ld-%02u-%02u %02u:%02u:%02u UTC\n", static_cast<long>(t.year), t.month, t.day,
                           t.hour, t.minute, t.second);
                if (err.isOk() || r.devKey) ctx.printf("Signed:   %s\n", r.devKey ? "the dev key" : "a release key");
                for (size_t i = 0; i < p.sectionCount; ++i) {
                        const UpdatePackage::Section &s = p.sections[i];
                        const char                   *partition = partitionOf(s.type);
                        if (static_cast<UpdatePackage::SectionType>(s.type) == UpdatePackage::SectionType::Notes) {
                                ctx.printf("Notes:    %u bytes\n", static_cast<unsigned>(s.size));
                        } else if (partition == nullptr) {
                                ctx.printf("Section:  type %u, %u bytes, not one this firmware knows (skipped)\n",
                                           static_cast<unsigned>(s.type), static_cast<unsigned>(s.size));
                        } else {
                                ctx.printf("%-9s %u bytes, %s %s\n", sectionLabel(s.type),
                                           static_cast<unsigned>(s.size),
                                           r.flashed[i] == Update::Flashed::Same      ? "the same as"
                                           : r.flashed[i] == Update::Flashed::Differs ? "differs from"
                                                                                      : "not compared with",
                                           partition);
                        }
                }
                if (r.problem != nullptr) {
                        ctx.printf("Check:    bad: %s\n", r.problem);
                } else if (err.isError()) {
                        ctx.printf("Check:    failed: %s\n", err.desc());
                } else {
                        ctx.printf("Check:    good\n");
                }
                return;
        }

        // The path as the shell's directory makes it: the intent names it whole.
        bool absolute(CommandContext &ctx, const char *arg, String &out) {
                auto [path, err] = Vfs::normalize(arg, ctx.cwd());
                if (err.isError()) {
                        ctx.eprintf("update: %s: %s\n", arg, err.desc());
                        return false;
                }
                out = std::move(path);
                return true;
        }

        int check(CommandContext &ctx, const char *arg) {
                String path;
                if (!absolute(ctx, arg, path)) return Shell::ExitFailure;
                Update::Report r;
                const Error    err = Update::check(path.view(), r, nullptr, nullptr);
                if (err.isError() && err != Error::CorruptData) {
                        ctx.eprintf("update: %s: %s\n", arg, err.desc());
                        return Shell::ExitFailure;
                }
                printReport(ctx, path.view(), r, err);
                return err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        struct ApplyProgress {
                        CommandContext *ctx;
                        Update::Stage   stage;
                        bool            started;
        };

        void showProgress(void *arg, Update::Stage stage, uint64_t, uint64_t) {
                auto *p = static_cast<ApplyProgress *>(arg);
                if (!p->started || p->stage != stage) p->ctx->eprintf("update: %s...\n", Update::stageText(stage));
                p->started = true;
                p->stage = stage;
                return;
        }

        int apply(CommandContext &ctx, const char *arg, bool force) {
                String path;
                if (!absolute(ctx, arg, path)) return Shell::ExitFailure;
                Update::Report r;
                ApplyProgress  progress = {&ctx, Update::Stage::Checking, false};
                const Error    err = Update::apply(path.view(), force, r, showProgress, &progress);
                if (err == Error::Exists) {
                        const bool wrote = progress.started && progress.stage == Update::Stage::WritingRecovery;
                        ctx.printf(wrote ? "update: the recovery firmware written; the rest is installed already\n"
                                         : "update: everything in it is installed already\n");
                        return Shell::ExitSuccess;
                }
                if (err.isError()) {
                        if (r.problem != nullptr) {
                                ctx.eprintf("update: %s: %s\n", arg, r.problem);
                        } else {
                                ctx.eprintf("update: %s: %s\n", arg, err.desc());
                        }
                        return Shell::ExitFailure;
                }
                ctx.printf(System::running() == System::Firmware::Recovery
                                   ? "update: %s installed; restarting\n"
                                   : "update: restarting into recovery to install %s\n",
                           r.package.versionText);
                (void)Vfs::sync();
                (void)ctx.shell().console().flush(RestartFlushMs);
                // Recovery's screen shows how it ended for a moment, as after an update at boot.
                if (System::running() == System::Firmware::Recovery) Thread::sleep(RecoveryPauseMs);
                System::restart();
        }

        int status(CommandContext &ctx) {
                Update::Status s;
                const Error    err = Update::status(s);
                if (err.isError()) {
                        ctx.eprintf("update: can't read /boot: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                const SystemInfo info = System::info();
                ctx.printf("Running:  %s, %s\n", System::running() == System::Firmware::Recovery ? "recovery" : "main",
                           info.buildVersion);
                ctx.printf("Pending:  %.*s\n", int(s.pending.isEmpty() ? 4 : s.pending.size()),
                           s.pending.isEmpty() ? "none" : s.pending.view().data());
                if (s.result.isEmpty()) {
                        ctx.printf("Last:     none\n");
                        return Shell::ExitSuccess;
                }
                // The result's own lines, under "Last:".
                StringView rest = s.result.view();
                while (!rest.isEmpty()) {
                        const size_t     nl = rest.indexOf('\n');
                        const StringView line = rest.left(nl);
                        ctx.printf("Last:     %.*s\n", int(line.size()), line.data());
                        rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                }
                return Shell::ExitSuccess;
        }

        int runUpdate(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() == 2 && std::strcmp(args[1], "status") == 0) return status(ctx);
                if (args.size() == 3 && std::strcmp(args[1], "check") == 0) return check(ctx, args[2]);
                if (args.size() == 3 && std::strcmp(args[1], "apply") == 0) return apply(ctx, args[2], false);
                if (args.size() == 4 && std::strcmp(args[1], "apply") == 0 && std::strcmp(args[2], "--force") == 0)
                        return apply(ctx, args[3], true);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(update, runUpdate, "check FILE | apply [--force] FILE | status",
                   "Check an update package (.bmup), apply it (restarting into recovery to install it; "
                   "--force: also one not newer than what is installed), or show what is pending and the last "
                   "update's result")

BMDS_NAMESPACE_END
