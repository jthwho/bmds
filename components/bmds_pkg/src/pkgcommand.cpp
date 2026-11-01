/**
 * @file      pkgcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `pkg` command (devplan/packages.md §9): arguments, the plan and its question, and output
 * as text or JSON Lines.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/datetime.h>
#include <bmds/glob.h>
#include <bmds/http.h>
#include <bmds/jsonwriter.h>
#include <bmds/pkgcommand.h>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgversion.h>
#include <bmds/shell.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        using Packages::Info;

        struct Options {
                        bool        json = false;
                        bool        yes = false;
                        bool        dryRun = false;
                        bool        overwrite = false;
                        bool        files = false;
                        bool        installed = false;  ///< list --installed
                        bool        upgradable = false; ///< list --upgradable
                        uint16_t    size = 32;
                        const char *output = nullptr;
        };

        // Where output goes, and how.
        struct Out {
                        CommandContext &ctx;
                        bool            json;

                        // A JSON record, written by @p fill between its braces, as one line.
                        template <typename F> void record(const char *type, F fill) {
                                String     line;
                                JsonWriter w(line);
                                w.beginObject();
                                w.member("type", type);
                                fill(w);
                                w.endObject();
                                (void)w.finish();
                                (void)line.append('\n');
                                ctx.print(line.view());
                                return;
                        }
        };

        const char *stepName(Packages::Step step) {
                switch (step) {
                        case Packages::Step::Checking: return "checking";
                        case Packages::Step::Writing: return "writing";
                        case Packages::Step::Replacing: return "replacing";
                        case Packages::Step::Removing: return "removing";
                        case Packages::Step::Fetching: return "fetching";
                }
                return "?";
        }

        void keyIdText(const Info &info, char (&out)[2 * SignedContainer::KeyIdBytes + 1]) {
                constexpr char Digits[] = "0123456789abcdef";
                for (size_t i = 0; i < SignedContainer::KeyIdBytes; ++i) {
                        out[2 * i] = Digits[info.keyId[i] >> 4];
                        out[2 * i + 1] = Digits[info.keyId[i] & 15];
                }
                out[sizeof(out) - 1] = '\0';
                return;
        }

        const char *signerKind(Packages::Signer signer) {
                switch (signer) {
                        case Packages::Signer::Trusted: return "trusted";
                        case Packages::Signer::Dev: return "dev";
                        case Packages::Signer::Added: return "added";
                }
                return "dev";
        }

        void packageRecord(Out &out, const Packages::Config &config, const Info &info, bool installed,
                           StringView available = StringView(), bool upgradable = false) {
                char id[2 * SignedContainer::KeyIdBytes + 1];
                keyIdText(info, id);
                String name, what;
                Packages::describeSigner(config, info, name, what);
                out.record("package", [&](JsonWriter &w) {
                        w.member("name", info.name);
                        w.member("version", info.version);
                        w.member("arch", info.arch);
                        w.member("title", info.title);
                        w.member("summary", info.summary);
                        w.member("category", info.category);
                        w.member("license", info.license);
                        w.member("homepage", info.homepage);
                        w.member("depends", info.depends);
                        w.member("abi", info.abi);
                        w.member("installedSize", info.installedBytes);
                        w.member("size", info.fileBytes);
                        w.member("made", info.madeAt);
                        w.member("signer", signerKind(info.signer));
                        w.member("keyId", id);
                        w.member("installed", installed);
                        w.member("automatic", info.automatic);
                        w.member("files", info.files);
                        w.member("links", info.links);
                        w.member("directories", info.directories);
                        w.member("available", available);
                        w.member("upgradable", upgradable);
                        w.member("keyName", name.view());
                });
                return;
        }

        void errorOut(Out &out, StringView package, Error err, StringView why) {
                if (out.json) {
                        out.record("error", [&](JsonWriter &w) {
                                w.member("package", package);
                                w.member("error", err.name());
                                w.member("text", why.isEmpty() ? StringView(err.desc()) : why);
                        });
                } else if (why.isEmpty()) {
                        out.ctx.eprintf("pkg: %.*s%s%s\n", int(package.size()), package.data(),
                                        package.isEmpty() ? "" : ": ", err.desc());
                } else {
                        out.ctx.eprintf("pkg: %.*s\n", int(why.size()), why.data());
                }
                return;
        }

        // The listener: notes always, progress as records in JSON.
        void onProgress(void *arg, StringView package, Packages::Step step, uint64_t done, uint64_t total) {
                Out &out = *static_cast<Out *>(arg);
                if (!out.json) return;
                out.record("progress", [&](JsonWriter &w) {
                        w.member("package", package);
                        w.member("step", stepName(step));
                        w.member("done", done);
                        w.member("total", total);
                });
                return;
        }

        void onNote(void *arg, StringView package, StringView text) {
                Out &out = *static_cast<Out *>(arg);
                if (out.json) {
                        out.record("note", [&](JsonWriter &w) {
                                w.member("package", package);
                                w.member("text", text);
                        });
                } else {
                        out.ctx.printf("%.*s: %.*s\n", int(package.size()), package.data(), int(text.size()),
                                       text.data());
                }
                return;
        }

        Packages::Listener listenerFor(Out &out) {
                Packages::Listener l;
                l.progress = onProgress;
                l.note = onNote;
                l.arg = &out;
                return l;
        }

        // Asks a yes-or-no question on the terminal: true for y.
        bool ask(CommandContext &ctx, const char *question) {
                ctx.printf("%s [y/N] ", question);
                char   line[16];
                size_t n = 0;
                for (;;) {
                        char c;
                        auto [got, err] = ctx.read(WritableByteSpan(reinterpret_cast<uint8_t *>(&c), 1));
                        if (err.isError() || got == 0 || c == '\n' || c == '\r') break;
                        if (n < sizeof(line) - 1) line[n++] = c;
                }
                ctx.print("\n");
                return n > 0 && (line[0] == 'y' || line[0] == 'Y');
        }

        bool isPackageFile(StringView arg) {
                return arg.contains('/') || arg.endsWith(".bmpk");
        }

        void printInfo(CommandContext &ctx, const Packages::Config &config, const Info &info, StringView description,
                       bool installed) {
                const auto line = [&](const char *label, StringView value) {
                        if (!value.isEmpty()) ctx.printf("%-10s%.*s\n", label, int(value.size()), value.data());
                };
                ctx.printf("%-10s%.*s %.*s (%.*s)%s\n", "Package:", int(info.name.size()), info.name.data(),
                           int(info.version.size()), info.version.data(), int(info.arch.size()), info.arch.data(),
                           installed ? (info.automatic ? ", installed as a dependency" : ", installed") : "");
                line("Title:", info.title);
                line("Summary:", info.summary);
                line("Category:", info.category);
                line("License:", info.license);
                line("Homepage:", info.homepage);
                line("Depends:", info.depends);
                ctx.printf("%-10s%llu bytes in %u files, %u links, %u directories\n",
                           "Size:", static_cast<unsigned long long>(info.installedBytes), unsigned(info.files),
                           unsigned(info.links), unsigned(info.directories));
                const DateTime t = DateTime::fromSeconds(info.madeAt);
                ctx.printf("%-10s%04ld-%02u-%02u %02u:%02u:%02u UTC\n", "Made:", static_cast<long>(t.year), t.month,
                           t.day, t.hour, t.minute, t.second);
                char id[2 * SignedContainer::KeyIdBytes + 1];
                keyIdText(info, id);
                String name, what;
                Packages::describeSigner(config, info, name, what);
                ctx.printf("%-10s%s (%s; key %s)\n", "Signed:", name.cStr(), what.cStr(), id);
                if (!description.isEmpty()) {
                        ctx.printf("%-10s", "About:");
                        StringView rest = description.trimmed();
                        bool       first = true;
                        while (!rest.isEmpty()) {
                                const size_t     nl = rest.indexOf('\n');
                                const StringView l = rest.left(nl);
                                ctx.printf("%s%.*s\n", first ? "" : "          ", int(l.size()), l.data());
                                first = false;
                                rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                        }
                }
                return;
        }

        void printFiles(Out &out, StringView package, StringView manifest) {
                PkgManifest::Reader r(manifest);
                PkgManifest::Entry  e;
                while (r.next(e).isOk()) {
                        if (e.kind == PkgManifest::Kind::Field) continue;
                        if (out.json) {
                                out.record("file", [&](JsonWriter &w) {
                                        w.member("package", package);
                                        w.member("path", e.path);
                                        w.member("kind", e.kind == PkgManifest::Kind::File        ? "file"
                                                         : e.kind == PkgManifest::Kind::Directory ? "directory"
                                                                                                  : "link");
                                        if (e.kind == PkgManifest::Kind::File) {
                                                w.member("mode", e.mode);
                                                w.member("size", e.size);
                                        }
                                        if (e.kind == PkgManifest::Kind::Link) w.member("target", e.target);
                                });
                        } else if (e.kind == PkgManifest::Kind::File) {
                                out.ctx.printf("  %4o %10llu %.*s\n", unsigned(e.mode),
                                               static_cast<unsigned long long>(e.size), int(e.path.size()),
                                               e.path.data());
                        } else if (e.kind == PkgManifest::Kind::Link) {
                                out.ctx.printf("  link            %.*s -> %.*s\n", int(e.path.size()), e.path.data(),
                                               int(e.target.size()), e.target.data());
                        } else {
                                out.ctx.printf("  dir             %.*s/\n", int(e.path.size()), e.path.data());
                        }
                }
                return;
        }

        // One package as `list` and `search` show it: what is installed, and the lists' newest.
        struct Row {
                        Info   installed;
                        bool   have = false;
                        size_t newest = List<Packages::Available>::NPos;
        };

        // Every package installed or in the lists, by name, into @p rows.
        Error collectRows(const Packages::Config &config, List<Packages::Available> &listed, List<Row> &rows,
                          String &why) {
                auto [names, err] = Packages::installed(config);
                if (err.isOk()) err = Packages::available(config, listed, why);
                if (err.isError()) return err;
                List<StringView> all(MemSpace::Psram);
                for (const String &n : names) err = err.isOk() ? all.append(n.view()) : err;
                for (const Packages::Available &a : listed) {
                        if (err.isOk() && !all.contains(a.info.name)) err = all.append(a.info.name);
                }
                if (err.isError()) return err;
                std::sort(all.begin(), all.end());
                for (StringView name : all) {
                        Row row;
                        row.have = Packages::readInstalled(name, config, row.installed).isOk();
                        for (size_t i = 0; i < listed.size(); ++i) {
                                if (listed[i].info.name != name) continue;
                                if (row.newest == List<Packages::Available>::NPos ||
                                    PkgVersion::compare(listed[i].info.version, listed[row.newest].info.version) > 0)
                                        row.newest = i;
                        }
                        if (!row.have && row.newest == List<Packages::Available>::NPos) continue;
                        err = rows.append(std::move(row));
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        bool isUpgradable(const Row &row, const List<Packages::Available> &listed) {
                return row.have && row.newest != List<Packages::Available>::NPos &&
                       PkgVersion::compare(listed[row.newest].info.version, row.installed.version) > 0;
        }

        // Whether @p text holds @p word, case-blind.
        bool containsWord(StringView text, StringView word) {
                if (word.size() > text.size()) return false;
                for (size_t i = 0; i + word.size() <= text.size(); ++i) {
                        if (text.mid(i, word.size()).equalsIgnoreCase(word)) return true;
                }
                return false;
        }

        // The rows @p keep accepts: a line each (name, version, state, summary), or package records.
        template <typename Keep>
        void printRows(Out &out, const Packages::Config &config, const List<Row> &rows,
                       const List<Packages::Available> &listed, Keep keep) {
                const auto versionText = [&](const Row &row, String &text) {
                        const Info &shown = row.have ? row.installed : listed[row.newest].info;
                        (void)text.append(shown.version);
                        if (isUpgradable(row, listed))
                                (void)text.appendf(" -> %.*s", int(listed[row.newest].info.version.size()),
                                                   listed[row.newest].info.version.data());
                };
                int nameWidth = 0, versionWidth = 0;
                for (const Row &row : rows) {
                        if (out.json || !keep(row)) continue;
                        String v;
                        versionText(row, v);
                        const Info &shown = row.have ? row.installed : listed[row.newest].info;
                        nameWidth = std::max(nameWidth, int(shown.name.size()));
                        versionWidth = std::max(versionWidth, int(v.size()));
                }
                for (const Row &row : rows) {
                        if (!keep(row)) continue;
                        const Info      &shown = row.have ? row.installed : listed[row.newest].info;
                        const StringView newest = row.newest == List<Packages::Available>::NPos
                                                          ? StringView()
                                                          : listed[row.newest].info.version;
                        const bool       upgradable = isUpgradable(row, listed);
                        if (out.json) {
                                packageRecord(out, config, shown, row.have, newest, upgradable);
                                continue;
                        }
                        String v;
                        versionText(row, v);
                        const char      *state = upgradable                ? "upgradable"
                                                 : !row.have               ? "available"
                                                 : row.installed.automatic ? "auto"
                                                                           : "installed";
                        const StringView about = !shown.summary.isEmpty() ? shown.summary : shown.title;
                        out.ctx.printf("%-*.*s  %-*s  %-10s  %.*s\n", nameWidth, int(shown.name.size()),
                                       shown.name.data(), versionWidth, v.cStr(), state, int(about.size()),
                                       about.data());
                }
                return;
        }

        int list(Out &out, const Packages::Config &config, const Options &o, StringView pattern) {
                List<Packages::Available> listed(MemSpace::Psram);
                List<Row>                 rows(MemSpace::Psram);
                String                    why;
                const Error               err = collectRows(config, listed, rows, why);
                if (err.isError()) {
                        errorOut(out, "", err, why.view());
                        return Shell::ExitFailure;
                }
                printRows(out, config, rows, listed, [&](const Row &row) {
                        const StringView name = row.have ? row.installed.name : listed[row.newest].info.name;
                        if (!pattern.isEmpty() && !Glob::match(pattern, name)) return false;
                        if (o.installed && !row.have) return false;
                        return !o.upgradable || isUpgradable(row, listed);
                });
                return Shell::ExitSuccess;
        }

        int search(Out &out, const Packages::Config &config, Span<char *const> words) {
                List<Packages::Available> listed(MemSpace::Psram);
                List<Row>                 rows(MemSpace::Psram);
                String                    why;
                const Error               err = collectRows(config, listed, rows, why);
                if (err.isError()) {
                        errorOut(out, "", err, why.view());
                        return Shell::ExitFailure;
                }
                size_t found = 0;
                printRows(out, config, rows, listed, [&](const Row &row) {
                        const Info      &i = row.have ? row.installed : listed[row.newest].info;
                        const StringView description = row.newest == List<Packages::Available>::NPos
                                                               ? StringView()
                                                               : listed[row.newest].description.view();
                        for (char *const w : words) {
                                const StringView word(w);
                                if (!containsWord(i.name, word) && !containsWord(i.title, word) &&
                                    !containsWord(i.summary, word) && !containsWord(description, word))
                                        return false;
                        }
                        ++found;
                        return true;
                });
                return found > 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        int info(Out &out, const Packages::Config &config, StringView what, const Options &o) {
                Info   info;
                String manifest, description, why;
                Error  err;
                bool   installed = false;
                if (isPackageFile(what)) {
                        auto [path, nerr] = Vfs::normalize(what, out.ctx.cwd());
                        err = nerr.isError()
                                      ? nerr
                                      : Packages::inspectFile(path.view(), config, info, &manifest, &description, why);
                } else {
                        installed = true;
                        err = Packages::readInstalled(what, config, info);
                        if (err.isOk() && o.files) err = Packages::readManifest(what, config, manifest);
                        if (err.isOk()) (void)Packages::readDescription(what, config, description);
                }
                if (err == Error::NotFound) {
                        // Not installed: the lists' newest, read from its file.
                        installed = false;
                        List<Packages::Available> listed(MemSpace::Psram);
                        err = Packages::available(config, listed, why);
                        size_t newest = List<Packages::Available>::NPos;
                        for (size_t i = 0; err.isOk() && i < listed.size(); ++i) {
                                if (listed[i].info.name != what) continue;
                                if (newest == List<Packages::Available>::NPos ||
                                    PkgVersion::compare(listed[i].info.version, listed[newest].info.version) > 0)
                                        newest = i;
                        }
                        if (err.isOk() && newest == List<Packages::Available>::NPos) {
                                err = Error::NotFound;
                                (void)why.appendf("%.*s isn't installed or in the lists", int(what.size()),
                                                  what.data());
                        } else if (err.isOk()) {
                                err = Packages::inspectFile(listed[newest].path.view(), config, info, &manifest,
                                                            &description, why);
                        }
                }
                if (err.isError()) {
                        errorOut(out, what, err, why.view());
                        return Shell::ExitFailure;
                }
                if (out.json) {
                        packageRecord(out, config, info, installed);
                        if (!description.isEmpty()) {
                                out.record("description", [&](JsonWriter &w) {
                                        w.member("package", info.name);
                                        w.member("text", description.view());
                                });
                        }
                } else {
                        printInfo(out.ctx, config, info, description.view(), installed);
                }
                if (o.files) printFiles(out, info.name, manifest.view());
                return Shell::ExitSuccess;
        }

        // What installing @p p does, said before asking.
        void planInstall(Out &out, const Packages::Planned &p, const Packages::Config &config) {
                const Info &info = p.info;
                Info        old;
                const bool  have = Packages::readInstalled(info.name, config, old).isOk();
                const int   order = have ? PkgVersion::compare(info.version, old.version) : 1;
                const char *action = !have ? "install" : order > 0 ? "upgrade" : order < 0 ? "downgrade" : "reinstall";
                if (out.json) {
                        out.record("plan", [&](JsonWriter &w) {
                                w.member("action", action);
                                w.member("name", info.name);
                                w.member("version", info.version);
                                if (have) w.member("from", old.version);
                                w.member("installedSize", info.installedBytes);
                                w.member("signer", p.remote ? "" : signerKind(info.signer));
                                w.member("automatic", p.automatic);
                                w.member("file", p.path.view());
                                if (p.remote) w.member("fetch", info.fileBytes);
                        });
                        return;
                }
                out.ctx.printf("%s %.*s %.*s", action, int(info.name.size()), info.name.data(),
                               int(info.version.size()), info.version.data());
                if (have && order != 0) out.ctx.printf(" (from %.*s)", int(old.version.size()), old.version.data());
                if (p.remote) {
                        // Who signed it shows once it is here; its list was signed.
                        Http::Url url;
                        (void)Http::parseUrl(p.path.view(), url);
                        out.ctx.printf(", %llu bytes, %llu to fetch from %.*s%s\n",
                                       static_cast<unsigned long long>(info.installedBytes),
                                       static_cast<unsigned long long>(info.fileBytes), int(url.host.size()),
                                       url.host.data(), p.automatic ? " (a dependency)" : "");
                        return;
                }
                String name, what;
                Packages::describeSigner(config, info, name, what);
                out.ctx.printf(", %llu bytes, signed by %s (%s)%s\n",
                               static_cast<unsigned long long>(info.installedBytes), name.cStr(), what.cStr(),
                               p.automatic ? " (a dependency)" : "");
                return;
        }

        void doneRecord(Out &out, const char *action, StringView name, StringView version, bool automatic = false) {
                if (out.json) {
                        out.record("done", [&](JsonWriter &w) {
                                w.member("action", action);
                                w.member("name", name);
                                w.member("version", version);
                                if (automatic) w.member("automatic", true);
                        });
                } else {
                        out.ctx.printf("%.*s %.*s: %s%s\n", int(name.size()), name.data(), int(version.size()),
                                       version.data(), action, automatic ? " (a dependency)" : "");
                }
                return;
        }

        // Says which automatic packages nothing needs once @p removing are gone.
        void noteUnneeded(Out &out, const Packages::Config &config, Span<const StringView> removing, bool dryRun) {
                List<String> names(MemSpace::Psram);
                if (Packages::unneeded(removing, config, names).isError()) return;
                for (const String &name : names) {
                        const char *text = dryRun ? "would be needed no more (pkg autoremove removes it)"
                                                  : "no longer needed (pkg autoremove removes it)";
                        if (out.json) {
                                out.record("note", [&](JsonWriter &w) {
                                        w.member("package", name.view());
                                        w.member("text", text);
                                });
                        } else {
                                out.ctx.printf("%s: %s\n", name.cStr(), text);
                        }
                }
                return;
        }

        // Plans @p args (files and names; none for an upgrade), then asks once and installs.
        int install(Out &out, const Packages::Config &config, Span<char *const> args, Packages::PlanMode mode,
                    const Options &o) {
                List<String> paths(MemSpace::Psram); // the files, absolute
                for (char *const arg : args) {
                        if (!isPackageFile(arg)) continue;
                        auto [path, err] = Vfs::normalize(arg, out.ctx.cwd());
                        if (err.isError()) {
                                errorOut(out, arg, err, StringView());
                                return Shell::ExitFailure;
                        }
                        (void)paths.append(std::move(path));
                }
                const bool asking = !o.yes && !o.dryRun;
                if (asking && out.json) {
                        errorOut(out, "", Error::Invalid, "--json doesn't ask: give -y or --dry-run");
                        return Shell::ExitFailure;
                }
                // The plan: what was asked for and what it needs, each after its dependencies.
                Packages::Listener      listener = listenerFor(out);
                List<Packages::Planned> plan(MemSpace::Psram);
                {
                        List<Packages::Request> requests(MemSpace::Psram);
                        size_t                  file = 0;
                        for (char *const arg : args) {
                                const bool isFile = isPackageFile(arg);
                                (void)requests.append(
                                        Packages::Request{isFile ? paths[file++].view() : StringView(arg), isFile});
                        }
                        String      why;
                        const Error err = Packages::plan(requests.span(), mode, config, listener, plan, why);
                        if (err.isError()) {
                                errorOut(out, "", err, why.view());
                                return Shell::ExitFailure;
                        }
                }
                if (plan.isEmpty()) {
                        if (!out.json)
                                out.ctx.printf("%s\n", mode == Packages::PlanMode::Upgrade ? "nothing to upgrade"
                                                                                           : "nothing to do");
                        return Shell::ExitSuccess;
                }
                if (asking || o.dryRun) {
                        for (const Packages::Planned &p : plan) planInstall(out, p, config);
                }
                if (asking && !ask(out.ctx, "Go ahead?")) return Shell::ExitFailure;
                // What comes from the network, fetched: then who signed it is known.
                for (Packages::Planned &p : plan) {
                        if (!p.remote || o.dryRun) continue;
                        if (!out.json)
                                out.ctx.printf("fetching %.*s %.*s (%llu bytes)\n", int(p.info.name.size()),
                                               p.info.name.data(), int(p.info.version.size()), p.info.version.data(),
                                               static_cast<unsigned long long>(p.info.fileBytes));
                        String      why;
                        const Error err = Packages::fetchPlanned(p, config, listener, why);
                        if (err.isError()) {
                                errorOut(out, p.info.name, err, why.view());
                                return err == Error::Interrupted ? Shell::ExitInterrupted : Shell::ExitFailure;
                        }
                }
                // pkg/dev ask: its own question about what anyone could have made; -y answers it.
                if (config.dev == Packages::DevPolicy::Ask && !o.yes && !o.dryRun) {
                        String dev;
                        for (const Packages::Planned &p : plan) {
                                if (p.info.signer == Packages::Signer::Dev)
                                        (void)dev.appendf("%s%.*s", dev.isEmpty() ? "" : ", ", int(p.info.name.size()),
                                                          p.info.name.data());
                        }
                        if (!dev.isEmpty()) {
                                out.ctx.printf("Signed by the dev key, so anyone could have made them: %s\n",
                                               dev.cStr());
                                if (!ask(out.ctx, "Install them anyway?")) return Shell::ExitFailure;
                        }
                }

                Packages::InstallOptions options;
                options.overwrite = o.overwrite;
                options.dryRun = o.dryRun;
                for (size_t i = 0; i < plan.size(); ++i) {
                        const Packages::Planned &p = plan[i];
                        // Automatic in the engine keeps an upgraded package's mark as it was.
                        options.automatic = p.automatic || p.upgrade;
                        options.earlier = Span<const Packages::Planned>(plan.data(), i);
                        if (p.remote) { // a dry run: not fetched, so not read
                                doneRecord(out, "would install", p.info.name, p.info.version, p.automatic);
                                continue;
                        }
                        Info        info;
                        String      why;
                        const Error err = Packages::installFile(p.path.view(), config, options, listener, info, why);
                        if (err.isError()) {
                                errorOut(out, p.path.view(), err, why.view());
                                return Shell::ExitFailure;
                        }
                        doneRecord(out, o.dryRun ? "would install" : "installed", info.name, info.version, p.automatic);
                        if (out.ctx.isInterrupted()) return Shell::ExitInterrupted;
                }
                return Shell::ExitSuccess;
        }

        // Removes @p all, which go together.
        int removeAll(Out &out, const Packages::Config &config, Span<const StringView> all) {
                Packages::Listener listener = listenerFor(out);
                for (StringView name : all) {
                        Info info;
                        (void)Packages::readInstalled(name, config, info);
                        String      why;
                        const Error err = Packages::remove(name, all, config, listener, why);
                        if (err.isError()) {
                                errorOut(out, name, err, why.view());
                                return Shell::ExitFailure;
                        }
                        doneRecord(out, "removed", name, info.version);
                        if (out.ctx.isInterrupted()) return Shell::ExitInterrupted;
                }
                return Shell::ExitSuccess;
        }

        int remove(Out &out, const Packages::Config &config, Span<char *const> names, const Options &o) {
                List<StringView> all(MemSpace::Psram);
                for (char *const name : names) (void)all.append(StringView(name));
                // Everything checked before the plan (installed, needed by nothing staying), so a
                // dry run or a "no" tells the truth.
                for (StringView name : all) {
                        String      why;
                        const Error err = Packages::checkRemove(name, all.span(), config, why);
                        if (err.isError()) {
                                errorOut(out, name, err, why.view());
                                return Shell::ExitFailure;
                        }
                }
                for (StringView name : all) {
                        Info info;
                        (void)Packages::readInstalled(name, config, info);
                        if (out.json) {
                                out.record("plan", [&](JsonWriter &w) {
                                        w.member("action", "remove");
                                        w.member("name", info.name);
                                        w.member("version", info.version);
                                });
                        } else if (!o.yes) {
                                out.ctx.printf("remove %.*s %.*s\n", int(info.name.size()), info.name.data(),
                                               int(info.version.size()), info.version.data());
                        }
                }
                const bool asking = !o.yes && !o.dryRun;
                if (asking && out.json) {
                        errorOut(out, "", Error::Invalid, "--json doesn't ask: give -y or --dry-run");
                        return Shell::ExitFailure;
                }
                if (o.dryRun) {
                        noteUnneeded(out, config, all.span(), true);
                        return Shell::ExitSuccess;
                }
                if (asking && !ask(out.ctx, "Go ahead?")) return Shell::ExitFailure;
                const int status = removeAll(out, config, all.span());
                if (status == Shell::ExitSuccess) noteUnneeded(out, config, Span<const StringView>(), false);
                return status;
        }

        int autoremove(Out &out, const Packages::Config &config, const Options &o) {
                List<String> names(MemSpace::Psram);
                Error        err = Packages::unneeded(Span<const StringView>(), config, names);
                if (err.isError()) {
                        errorOut(out, "", err, "");
                        return Shell::ExitFailure;
                }
                if (names.isEmpty()) {
                        if (!out.json) out.ctx.printf("nothing to remove\n");
                        return Shell::ExitSuccess;
                }
                List<StringView> all(MemSpace::Psram);
                for (const String &name : names) (void)all.append(name.view());
                for (StringView name : all) {
                        Info info;
                        (void)Packages::readInstalled(name, config, info);
                        if (out.json) {
                                out.record("plan", [&](JsonWriter &w) {
                                        w.member("action", "remove");
                                        w.member("name", info.name);
                                        w.member("version", info.version);
                                });
                        } else if (!o.yes) {
                                out.ctx.printf("remove %.*s %.*s (needed no more)\n", int(info.name.size()),
                                               info.name.data(), int(info.version.size()), info.version.data());
                        }
                }
                const bool asking = !o.yes && !o.dryRun;
                if (asking && out.json) {
                        errorOut(out, "", Error::Invalid, "--json doesn't ask: give -y or --dry-run");
                        return Shell::ExitFailure;
                }
                if (o.dryRun) return Shell::ExitSuccess;
                if (asking && !ask(out.ctx, "Go ahead?")) return Shell::ExitFailure;
                return removeAll(out, config, all.span());
        }

        int mark(Out &out, const Packages::Config &config, StringView how, Span<char *const> names) {
                const bool automatic = how == StringView("auto");
                if (!automatic && how != StringView("manual")) return out.ctx.usage();
                for (char *const name : names) {
                        String      why;
                        const Error err = Packages::setAutomatic(name, automatic, config, why);
                        if (err.isError()) {
                                errorOut(out, name, err, why.view());
                                return Shell::ExitFailure;
                        }
                        Info info;
                        (void)Packages::readInstalled(name, config, info);
                        doneRecord(out, automatic ? "marked automatic" : "marked manual", info.name, info.version);
                }
                return Shell::ExitSuccess;
        }

        int update(Out &out, const Packages::Config &config) {
                List<Packages::SourceResult> results(MemSpace::Psram);
                const Error                  err = Packages::update(config, listenerFor(out), results);
                for (const Packages::SourceResult &r : results) {
                        if (out.json) {
                                out.record("source", [&](JsonWriter &w) {
                                        w.member("source", r.source.view());
                                        w.member("packages", r.packages);
                                        w.member("indexedHere", r.indexedHere);
                                        if (r.error.isError()) {
                                                w.member("error", r.error.name());
                                                w.member("text", r.why.view());
                                        }
                                });
                        } else if (r.error.isError()) {
                                out.ctx.eprintf("pkg: %s\n", r.why.cStr());
                        } else {
                                out.ctx.printf("%s: %u packages%s\n", r.source.cStr(), unsigned(r.packages),
                                               r.indexedHere
                                                       ? " (indexed here: its index.bmpi is missing or out of date)"
                                                       : "");
                        }
                }
                if (!out.json && results.isEmpty()) out.ctx.printf("no sources (the setting pkg/sources)\n");
                return err.isError() ? Shell::ExitFailure : Shell::ExitSuccess;
        }

        int index(Out &out, const Packages::Config &config, StringView dir) {
                auto [path, nerr] = Vfs::normalize(dir, out.ctx.cwd());
                uint32_t count = 0;
                String   why;
                Error    err = nerr;
                if (err.isOk()) err = Packages::indexFolder(path.view(), config, listenerFor(out), count, why);
                if (err.isError()) {
                        errorOut(out, dir, err, why.view());
                        return Shell::ExitFailure;
                }
                if (out.json) {
                        out.record("done", [&](JsonWriter &w) {
                                w.member("action", "indexed");
                                w.member("name", path.view());
                                w.member("packages", count);
                        });
                } else {
                        out.ctx.printf("%s/index.bmpi: %u packages\n", path.cStr(), unsigned(count));
                }
                return Shell::ExitSuccess;
        }

        int clean(Out &out, const Packages::Config &config) {
                String      why;
                const Error err = Packages::clean(config, why);
                if (err.isError()) {
                        errorOut(out, "", err, why.view());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

        void keyRecord(Out &out, const Packages::KeyEntry &e, const char *action) {
                char           id[2 * SignedContainer::KeyIdBytes + 1];
                constexpr char Digits[] = "0123456789abcdef";
                for (size_t i = 0; i < SignedContainer::KeyIdBytes; ++i) {
                        id[2 * i] = Digits[e.id[i] >> 4];
                        id[2 * i + 1] = Digits[e.id[i] & 15];
                }
                id[sizeof(id) - 1] = '\0';
                const char *kind = signerKind(e.kind);
                if (out.json) {
                        out.record(action == nullptr ? "key" : "done", [&](JsonWriter &w) {
                                if (action != nullptr) w.member("action", action);
                                w.member("id", id);
                                w.member("kind", kind);
                                w.member("comment", e.comment.view());
                        });
                } else if (action == nullptr) {
                        out.ctx.printf("%s  %-7s  %s\n", id, kind, e.comment.cStr());
                } else {
                        out.ctx.printf("%s: %s%s%s\n", id, action, e.comment.isEmpty() ? "" : ": ", e.comment.cStr());
                }
                return;
        }

        int key(Out &out, const Packages::Config &config, Span<char *const> args) {
                const StringView what = args.size() > 0 ? StringView(args[0]) : StringView();
                if (what == StringView("list") && args.size() == 1) {
                        List<Packages::KeyEntry> ring(MemSpace::Psram);
                        const Error              err = Packages::keyring(config, ring);
                        if (err.isError()) {
                                errorOut(out, "", err, "");
                                return Shell::ExitFailure;
                        }
                        for (const Packages::KeyEntry &e : ring) keyRecord(out, e, nullptr);
                        return Shell::ExitSuccess;
                }
                if ((what == StringView("add") || what == StringView("remove")) && args.size() == 2) {
                        Packages::KeyEntry e;
                        String             why;
                        Error              err;
                        if (what == StringView("add")) {
                                auto [path, nerr] = Vfs::normalize(args[1], out.ctx.cwd());
                                err = nerr.isError() ? nerr : Packages::addKey(path.view(), config, e, why);
                        } else {
                                err = Packages::removeKey(args[1], config, e, why);
                        }
                        if (err.isError()) {
                                errorOut(out, args[1], err, why.view());
                                return Shell::ExitFailure;
                        }
                        keyRecord(out, e, what == StringView("add") ? "added" : "removed");
                        return Shell::ExitSuccess;
                }
                return out.ctx.usage();
        }

        struct VerifyState {
                        Out       *out;
                        StringView package;
                        size_t     problems;
        };

        void onProblem(void *arg, StringView path, Packages::Problem problem) {
                VerifyState &s = *static_cast<VerifyState *>(arg);
                ++s.problems;
                const char *what = problem == Packages::Problem::Missing   ? "missing"
                                   : problem == Packages::Problem::Changed ? "changed"
                                                                           : "not-link";
                if (s.out->json) {
                        s.out->record("problem", [&](JsonWriter &w) {
                                w.member("package", s.package);
                                w.member("path", path);
                                w.member("problem", what);
                        });
                } else {
                        s.out->ctx.printf("%.*s: %.*s: %s\n", int(s.package.size()), s.package.data(), int(path.size()),
                                          path.data(),
                                          problem == Packages::Problem::NotLink ? "no longer a link" : what);
                }
                return;
        }

        int verify(Out &out, const Packages::Config &config, Span<char *const> names) {
                List<String> all(MemSpace::Psram);
                if (names.size() == 0) {
                        auto [installed, err] = Packages::installed(config);
                        if (err.isError()) {
                                errorOut(out, "", err, "");
                                return Shell::ExitFailure;
                        }
                        all = std::move(installed);
                } else {
                        for (char *const n : names) {
                                auto [s, err] = String::fromView(n);
                                if (err.isOk()) (void)all.append(std::move(s));
                        }
                }
                size_t problems = 0;
                for (const String &name : all) {
                        VerifyState s{&out, name.view(), 0};
                        const Error err = Packages::verify(name.view(), config, onProblem, &s);
                        if (err.isError()) {
                                String why;
                                if (err == Error::NotFound) (void)why.appendf("%s isn't installed", name.cStr());
                                errorOut(out, name.view(), err, why.view());
                                return Shell::ExitFailure;
                        }
                        problems += s.problems;
                        if (out.ctx.isInterrupted()) return Shell::ExitInterrupted;
                }
                if (!out.json && problems == 0) out.ctx.printf("every file matches\n");
                return problems == 0 ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        int icon(Out &out, const Packages::Config &config, StringView name, const Options &o) {
                if (o.output == nullptr) return out.ctx.usage();
                List<uint8_t> png(MemSpace::Psram);
                Error         err = Packages::icon(name, o.size, config, png);
                if (err == Error::NotFound) err = Packages::availableIcon(name, o.size, config, png);
                if (err.isOk()) {
                        auto [path, nerr] = Vfs::normalize(o.output, out.ctx.cwd());
                        err = nerr;
                        if (err.isOk()) {
                                auto [file, oerr] =
                                        Vfs::open(path.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, out.ctx.vfs());
                                err = oerr;
                                if (err.isOk()) {
                                        auto [n, werr] = file->write(png.span(), WaitForever);
                                        err = werr.isOk() && n != png.size() ? Error::IoError : werr;
                                }
                        }
                }
                if (err.isError()) {
                        String why;
                        if (err == Error::NotFound)
                                (void)why.appendf("%.*s: no icon, installed or in the lists", int(name.size()),
                                                  name.data());
                        errorOut(out, name, err, why.view());
                        return Shell::ExitFailure;
                }
                return Shell::ExitSuccess;
        }

} // namespace

int PkgCommand::run(CommandContext &ctx, const Packages::Config &config) {
        const Span<char *const> args = ctx.args();
        Options                 o;
        List<char *>            operands(MemSpace::Psram);
        for (size_t i = 1; i < args.size(); ++i) {
                const StringView a(args[i]);
                if (a == StringView("--json"))
                        o.json = true;
                else if (a == StringView("-y") || a == StringView("--yes"))
                        o.yes = true;
                else if (a == StringView("--dry-run"))
                        o.dryRun = true;
                else if (a == StringView("--overwrite"))
                        o.overwrite = true;
                else if (a == StringView("--files"))
                        o.files = true;
                else if (a == StringView("--installed"))
                        o.installed = true;
                else if (a == StringView("--upgradable"))
                        o.upgradable = true;
                else if (a == StringView("--size") && i + 1 < args.size())
                        o.size = uint16_t(std::atoi(args[++i]));
                else if (a == StringView("-o") && i + 1 < args.size())
                        o.output = args[++i];
                else if (a.startsWith('-') && a.size() > 1)
                        return ctx.usage();
                else
                        (void)operands.append(args[i]);
        }
        if (operands.isEmpty()) return ctx.usage();
        Out                     out{ctx, o.json};
        const StringView        cmd(operands[0]);
        const Span<char *const> rest(operands.data() + 1, operands.size() - 1);
        if (cmd == StringView("list") && rest.size() <= 1) return list(out, config, o, rest.size() == 1 ? rest[0] : "");
        if (cmd == StringView("search") && rest.size() > 0) return search(out, config, rest);
        if (cmd == StringView("update") && rest.size() == 0) return update(out, config);
        if (cmd == StringView("upgrade") && rest.size() == 0)
                return install(out, config, rest, Packages::PlanMode::Upgrade, o);
        if (cmd == StringView("index") && rest.size() == 1) return index(out, config, rest[0]);
        if (cmd == StringView("clean") && rest.size() == 0) return clean(out, config);
        if (cmd == StringView("key")) return key(out, config, rest);
        if (cmd == StringView("info") && rest.size() == 1) return info(out, config, rest[0], o);
        if (cmd == StringView("install") && rest.size() > 0)
                return install(out, config, rest, Packages::PlanMode::Install, o);
        if (cmd == StringView("remove") && rest.size() > 0) return remove(out, config, rest, o);
        if (cmd == StringView("autoremove") && rest.size() == 0) return autoremove(out, config, o);
        if (cmd == StringView("mark") && rest.size() > 1)
                return mark(out, config, rest[0], Span<char *const>(rest.data() + 1, rest.size() - 1));
        if (cmd == StringView("verify")) return verify(out, config, rest);
        if (cmd == StringView("icon") && rest.size() == 1) return icon(out, config, rest[0], o);
        return ctx.usage();
}

BMDS_NAMESPACE_END
