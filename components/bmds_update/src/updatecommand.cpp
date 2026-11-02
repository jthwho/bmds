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
#include <new>
#include <sdkconfig.h>
#include <bmds/blockwriter.h>
#include <bmds/command.h>
#include <bmds/console.h>
#include <bmds/datetime.h>
#include <bmds/fetch.h>
#include <bmds/memory.h>
#include <bmds/pkgmanifest.h>
#include <bmds/sysconf.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/thread.h>
#include <bmds/update.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Where this board's packages are published (devplan/update.md §17): one URL that names the newest,
        // the project's latest release's for this chip variant (devplan/packages.md §25).
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
#define BMDS_UPDATE_VARIANT "p4r1"
#else
#define BMDS_UPDATE_VARIANT "p4r3"
#endif
        BMDS_SYSCONF(updateUrl, "update/url", Config::Type::String,
                     "https://github.com/jthwho/bmds/releases/latest/download/bmds-" BMDS_UPDATE_VARIANT ".bmup",
                     "Where this board's update packages are published: a URL naming the newest (update fetch)", 0,
                     1023, nullptr, nullptr)

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
                if (p.sectionCount == 0) { // not a package's head: nothing in it to show
                        if (r.problem != nullptr) ctx.printf("Check:    bad: %s\n", r.problem);
                        return;
                }
                ctx.printf("Board:    %s\n", p.board);
                ctx.printf("Version:  %u.%u.%u (%s), installed %s: %s\n", p.version.major, p.version.minor,
                           p.version.patch, p.versionText, r.mainVersion[0] != '\0' ? r.mainVersion : "none",
                           r.order > 0   ? "newer"
                           : r.order < 0 ? "older"
                                         : "the same");
                const DateTime t = DateTime::fromSeconds(p.madeAt);
                ctx.printf("Made:     %04ld-%02u-%02u %02u:%02u:%02u UTC\n", static_cast<long>(t.year), t.month, t.day,
                           t.hour, t.minute, t.second);
                if (err.isOk() || r.devKey) ctx.printf("Signed:   %s\n", r.devKey ? "the dev key" : "a trusted key");
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
                        ctx.eprintf("update: can't read /flash: %s\n", err.desc());
                        return Shell::ExitFailure;
                }
                const SystemInfo info = System::info();
                ctx.printf("Running:  %s, %s\n", System::running() == System::Firmware::Recovery ? "recovery" : "main",
                           info.buildVersion);
                ctx.printf("Pending:  %.*s\n", int(s.pending.isEmpty() ? 4 : s.pending.size()),
                           s.pending.isEmpty() ? "none" : s.pending.view().data());
                if (s.result.isEmpty()) ctx.printf("Last:     none\n");
                // The result's own lines, under "Last:".
                StringView rest = s.result.view();
                while (!rest.isEmpty()) {
                        const size_t     nl = rest.indexOf('\n');
                        const StringView line = rest.left(nl);
                        ctx.printf("Last:     %.*s\n", int(line.size()), line.data());
                        rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                }
                // An update applied from recovery can't replace recovery: the next from the system does.
                ctx.printf("Recovery: %s%s\n", s.recoveryVersion[0] != '\0' ? s.recoveryVersion : "none",
                           s.recoveryVersion[0] == '\0' || std::strcmp(s.recoveryVersion, s.mainVersion) == 0
                                   ? ""
                                   : " (not the system's: the next update applied from the system replaces it)");
                return Shell::ExitSuccess;
        }

        // A package's head, fetched: the first bytes, and the whole file's size.
        struct HeadFetch {
                        uint8_t  head[UpdatePackage::MaxHeadBytes];
                        size_t   bytes = 0;
                        uint64_t total = UINT64_MAX;
        };

        // A package fetched into a file (`NAME.part`, resumed by its size), gathered into whole
        // blocks (`BlockWriter`: the body comes in pieces of any size).
        struct FileFetch {
                        CommandContext *ctx = nullptr;
                        String          path;
                        FileHandle::Ptr file;
                        BlockWriter     gather;
                        uint64_t        written = 0;
                        uint64_t        total = UINT64_MAX;
                        unsigned        shown = 0; ///< Tenths said.
        };

        Error openPart(FileFetch &f, bool resume) {
                f.file = FileHandle::Ptr();
                Vfs::Context context;
                auto [file, err] =
                        Vfs::open(f.path.view(), Vfs::Write | Vfs::Create | (resume ? 0 : Vfs::Truncate), context);
                if (err.isError()) return err;
                f.file = std::move(file);
                f.written = 0;
                if (resume) {
                        auto [end, serr] = f.file->stream().seek(0, Stream::SeekFrom::End);
                        if (serr.isError()) return serr;
                        f.written = uint64_t(end);
                }
                f.gather.reset(f.written);
                return Error::Ok;
        }

        // `update fetch [--check] [URL]`: the package at URL (or update/url) into update/dir, then
        // checked as `update check`; with --check, its head alone, nothing written.
        int fetch(CommandContext &ctx, Span<char *const> args) {
                bool        headOnly = false;
                const char *given = nullptr;
                for (char *const a : args) {
                        if (std::strcmp(a, "--check") == 0)
                                headOnly = true;
                        else if (given == nullptr && a[0] != '-')
                                given = a;
                        else
                                return ctx.usage();
                }
                String url;
                if (given != nullptr)
                        (void)url.append(given);
                else
                        (void)SysConf::get(StringView("update/url"), url);
                if (url.isEmpty()) {
                        ctx.eprintf("update: no URL: give one, or set update/url\n");
                        return Shell::ExitFailure;
                }
                if (&systemFetch == nullptr) {
                        ctx.eprintf("update: this firmware can't fetch (recovery has no network): put the package on "
                                    "the card\n");
                        return Shell::ExitFailure;
                }
                String finalUrl, why;
                if (headOnly) {
                        auto *h = static_cast<HeadFetch *>(Memory::alloc(MemSpace::Psram, sizeof(HeadFetch)));
                        if (h == nullptr) return Shell::ExitFailure;
                        new (h) HeadFetch;
                        FetchSink sink;
                        sink.begin = [](void *arg, uint64_t, uint64_t total) -> Error {
                                static_cast<HeadFetch *>(arg)->total = total;
                                return Error::Ok;
                        };
                        sink.body = [](void *arg, ByteSpan bytes) -> Error {
                                HeadFetch   &f = *static_cast<HeadFetch *>(arg);
                                const size_t n = bytes.size() < sizeof(f.head) - f.bytes ? bytes.size()
                                                                                         : sizeof(f.head) - f.bytes;
                                std::memcpy(f.head + f.bytes, bytes.data(), n);
                                f.bytes += n;
                                return f.bytes == sizeof(f.head) ? Error(Error::Full) : Error(Error::Ok); // enough
                        };
                        sink.arg = h;
                        Error err = systemFetch(url.view(), 0, sink, &finalUrl, why);
                        if (err == Error::Full) err = Error::Ok;
                        int status = Shell::ExitFailure;
                        if (err.isError()) {
                                ctx.eprintf("update: %s\n", why.isEmpty() ? err.desc() : why.cStr());
                        } else if (h->total == UINT64_MAX) {
                                ctx.eprintf("update: %s: the server didn't say the package's size\n", finalUrl.cStr());
                        } else {
                                auto *r = static_cast<Update::Report *>(
                                        Memory::alloc(MemSpace::Psram, sizeof(Update::Report)));
                                if (r != nullptr) {
                                        new (r) Update::Report;
                                        err = Update::checkHead(Span<const uint8_t>(h->head, h->bytes), h->total, *r);
                                        if (err.isOk() || err == Error::CorruptData) {
                                                printReport(ctx, url.view(), *r, err); // not the redirect's
                                                status = err.isOk() ? Shell::ExitSuccess : Shell::ExitFailure;
                                        } else {
                                                ctx.eprintf("update: %s\n", err.desc());
                                        }
                                        Memory::free(r);
                                }
                        }
                        Memory::free(h);
                        return status;
                }

                // Its name: the URL's last part (a release host's "latest" link keeps the file's name).
                StringView path = url.view();
                path = path.left(path.indexOf('?'));
                StringView name = path.mid(path.lastIndexOf('/') + 1);
                if (name.isEmpty() || !PkgManifest::isValidPath(name)) name = "update.bmup";
                String dir;
                (void)SysConf::get(StringView("update/dir"), dir);
                Vfs::Context context;
                (void)Vfs::mkdir(dir.view(), context);
                FileFetch f;
                f.ctx = &ctx;
                String target;
                Error  err = target.appendf("%s/%.*s", dir.cStr(), int(name.size()), name.data());
                if (err.isOk()) err = f.path.appendf("%s.part", target.cStr());
                if (err.isOk()) err = openPart(f, true);
                if (err.isError()) {
                        ctx.eprintf("update: %s: %s\n", f.path.cStr(), err.desc());
                        return Shell::ExitFailure;
                }
                FetchSink sink;
                sink.begin = [](void *arg, uint64_t start, uint64_t total) -> Error {
                        FileFetch &to = *static_cast<FileFetch *>(arg);
                        to.total = total;
                        if (start == to.written) return Error::Ok;
                        return start == 0 ? openPart(to, false) : Error(Error::CorruptData);
                };
                sink.body = [](void *arg, ByteSpan bytes) -> Error {
                        FileFetch  &to = *static_cast<FileFetch *>(arg);
                        const Error werr = to.gather.write(to.file->stream(), bytes);
                        if (werr.isError()) return werr;
                        to.written += bytes.size();
                        if (to.total != UINT64_MAX && to.total != 0) {
                                const unsigned tenths = unsigned(to.written * 10 / to.total);
                                if (tenths > to.shown && tenths < 10)
                                        to.ctx->eprintf("update: fetched %u%%\n", tenths * 10);
                                if (tenths > to.shown) to.shown = tenths;
                        }
                        return Error::Ok;
                };
                sink.arg = &f;
                if (f.written > 0)
                        ctx.eprintf("update: resuming from %llu bytes\n", static_cast<unsigned long long>(f.written));
                err = systemFetch(url.view(), f.written, sink, &finalUrl, why);
                const Error ferr = f.gather.flush(f.file->stream());
                if (err.isOk() && ferr.isError()) {
                        err = ferr;
                        why.clear();
                }
                f.file = FileHandle::Ptr();
                if (err.isError()) { // the .part stays: the next fetch resumes it
                        ctx.eprintf("update: %s\n", why.isEmpty() ? err.desc() : why.cStr());
                        return err == Error::Interrupted ? Shell::ExitInterrupted : Shell::ExitFailure;
                }
                err = Vfs::rename(f.path.view(), target.view(), Vfs::RenameMode::Replace, context);
                if (err.isError()) {
                        ctx.eprintf("update: %s: %s\n", target.cStr(), err.desc());
                        return Shell::ExitFailure;
                }
                return check(ctx, target.cStr());
        }

        int runUpdate(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                if (args.size() == 2 && std::strcmp(args[1], "status") == 0) return status(ctx);
                if (args.size() >= 2 && std::strcmp(args[1], "fetch") == 0)
                        return fetch(ctx, Span<char *const>(args.data() + 2, args.size() - 2));
                if (args.size() == 3 && std::strcmp(args[1], "check") == 0) return check(ctx, args[2]);
                if (args.size() == 3 && std::strcmp(args[1], "apply") == 0) return apply(ctx, args[2], false);
                if (args.size() == 4 && std::strcmp(args[1], "apply") == 0 && std::strcmp(args[2], "--force") == 0)
                        return apply(ctx, args[3], true);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(update, runUpdate, "check FILE | apply [--force] FILE | status | fetch [--check] [URL]",
                   "Check an update package (.bmup), apply it (restarting into recovery to install it; "
                   "--force: also one not newer than what is installed), or show what is pending and the last "
                   "update's result; fetch one (from URL, else the setting update/url) into update/dir and check it "
                   "(--check: its head only, nothing written)")

BMDS_NAMESPACE_END
