/**
 * @file      packages.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Installing and removing packages (devplan/packages.md §7): the checks, the writes in an order a
 * power cut can be finished from, and the database.
 */

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <bmds/blake2s.h>
#include <bmds/detail/pkgtext.h>
#include <bmds/memory.h>
#include <bmds/packages.h>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgsolver.h>
#include <bmds/pkgversion.h>
#include <bmds/vfs.h>
#include "packagesdetail.h"

BMDS_NAMESPACE_BEGIN

namespace Packages::Detail {

        std::atomic<bool> busy{false};

        using Packages::Config;
        using Packages::Info;
        using Packages::Listener;
        using Packages::Step;


        const Vfs::Context &vfs() {
                static const Vfs::Context context{"/", nullptr};
                return context;
        }

        Error fail(String &why, Error err, const char *fmt, ...) {
                why.clear();
                va_list args;
                va_start(args, fmt);
                (void)why.vappendf(fmt, args);
                va_end(args);
                return err;
        }

        Result<String> join(StringView dir, StringView rel) {
                String out;
                Error  err = out.reserve(dir.size() + 1 + rel.size());
                if (err.isOk()) err = out.append(dir);
                if (err.isOk() && !dir.endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(rel);
                return Result<String>(std::move(out), err);
        }

        // @p path with @p suffix after it (".pkgnew").
        Result<String> withSuffix(StringView path, StringView suffix) {
                String out;
                Error  err = out.reserve(path.size() + suffix.size());
                if (err.isOk()) err = out.append(path);
                if (err.isOk()) err = out.append(suffix);
                return Result<String>(std::move(out), err);
        }

        // The database file of package @p name with @p suffix (".status").
        Result<String> dbFile(const Config &config, StringView name, StringView suffix) {
                auto [path, err] = join(config.db, name);
                if (err.isOk()) err = path.append(suffix);
                return Result<String>(std::move(path), err);
        }

        void progress(const Listener &l, StringView package, Step step, uint64_t done, uint64_t total) {
                if (l.progress != nullptr) l.progress(l.arg, package, step, done, total);
                return;
        }

        void note(const Listener &l, StringView package, StringView text) {
                if (l.note != nullptr) l.note(l.arg, package, text);
                return;
        }

        Error readAt(FileHandle &file, uint64_t offset, uint8_t *to, size_t bytes) {
                auto [at, serr] = file.stream().seek(int64_t(offset), Stream::SeekFrom::Start);
                (void)at;
                if (serr.isError()) return serr;
                while (bytes > 0) {
                        auto [n, err] = file.read(WritableByteSpan(to, bytes), WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::EndOfFile;
                        to += n;
                        bytes -= n;
                }
                return Error::Ok;
        }

        Error readNext(FileHandle &file, uint8_t *to, size_t bytes) {
                while (bytes > 0) {
                        auto [n, err] = file.read(WritableByteSpan(to, bytes), WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::EndOfFile;
                        to += n;
                        bytes -= n;
                }
                return Error::Ok;
        }

        Error writeAll(FileHandle &file, ByteSpan data) {
                while (!data.isEmpty()) {
                        auto [n, err] = file.write(data, WaitForever);
                        if (err.isError()) return err;
                        if (n == 0) return Error::IoError;
                        data = data.mid(n);
                }
                return Error::Ok;
        }

        // Reads all of @p path (at most @p limit bytes) into @p out.
        Error readWhole(StringView path, size_t limit, String &out) {
                auto [file, err] = Vfs::open(path, Vfs::Read, vfs());
                if (err.isError()) return err;
                out.clear();
                char buf[512];
                for (;;) {
                        auto [n, rerr] = file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                    WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) return Error::Ok;
                        if (rerr.isError()) return rerr;
                        if (out.size() + n > limit) return Error::OutOfRange;
                        const Error aerr = out.append(StringView(buf, n));
                        if (aerr.isError()) return aerr;
                }
        }

        // Writes @p data to @p path whole: to `PATH.new`, then renamed over it.
        Error writeWhole(StringView path, ByteSpan data) {
                auto [temp, err] = withSuffix(path, ".new");
                if (err.isError()) return err;
                {
                        auto [file, oerr] = Vfs::open(temp.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, vfs());
                        if (oerr.isError()) return oerr;
                        err = writeAll(*file, data);
                }
                if (err.isOk()) err = Vfs::rename(temp.view(), path, Vfs::RenameMode::Replace, vfs());
                if (err.isError()) (void)Vfs::remove(temp.view(), vfs());
                return err;
        }

        Error writeWhole(StringView path, StringView text) {
                return writeWhole(path, ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
        }

        Error removeIfThere(StringView path) {
                const Error err = Vfs::remove(path, vfs());
                return err == Error::NotExist ? Error::Ok : err;
        }

        // Makes @p path and the directories above it.
        Error makeDirectories(StringView path) {
                for (size_t i = 1; i <= path.size(); ++i) {
                        if (i < path.size() && path[i] != '/') continue;
                        const Error err = Vfs::mkdir(path.left(i), vfs());
                        if (err.isError() && err != Error::Exists) return err;
                }
                return Error::Ok;
        }

        // ---- The set of paths a manifest names (case-blind, as FAT is) --------------------------


        // ---- The status file ------------------------------------------------------------------

        void hex(const uint8_t *bytes, size_t n, char *out) {
                constexpr char Digits[] = "0123456789abcdef";
                for (size_t i = 0; i < n; ++i) {
                        out[2 * i] = Digits[bytes[i] >> 4];
                        out[2 * i + 1] = Digits[bytes[i] & 15];
                }
                out[2 * n] = '\0';
                return;
        }

        Error field(String &out, StringView key, StringView value) {
                if (value.isEmpty()) return Error::Ok;
                Error err = out.append(key);
                if (err.isOk()) err = out.append('\t');
                if (err.isOk()) err = out.append(value);
                if (err.isOk()) err = out.append('\n');
                return err;
        }

        Error number(String &out, StringView key, uint64_t value) {
                char buf[24];
                std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(value));
                return field(out, key, buf);
        }

        const char *signerName(Packages::Signer signer) {
                switch (signer) {
                        case Packages::Signer::Trusted: return "trusted";
                        case Packages::Signer::Dev: return "dev";
                        case Packages::Signer::Added: return "added";
                }
                return "dev";
        }

        // The status text of @p info's fields (views anywhere).
        Error formatStatus(const Info &info, String &out) {
                out.clear();
                char id[2 * SignedContainer::KeyIdBytes + 1];
                hex(info.keyId, sizeof(info.keyId), id);
                Error err = field(out, "name", info.name);
                if (err.isOk()) err = field(out, "version", info.version);
                if (err.isOk()) err = field(out, "arch", info.arch);
                if (err.isOk()) err = number(out, "abi", info.abi);
                if (err.isOk()) err = field(out, "title", info.title);
                if (err.isOk()) err = field(out, "summary", info.summary);
                if (err.isOk()) err = field(out, "category", info.category);
                if (err.isOk()) err = field(out, "license", info.license);
                if (err.isOk()) err = field(out, "homepage", info.homepage);
                if (err.isOk()) err = field(out, "depends", info.depends);
                if (err.isOk()) err = number(out, "installed", info.installedBytes);
                if (err.isOk()) err = number(out, "size", info.fileBytes);
                if (err.isOk()) err = number(out, "made", uint64_t(info.madeAt));
                if (err.isOk()) err = field(out, "signer", signerName(info.signer));
                if (err.isOk()) err = field(out, "key-id", id);
                if (err.isOk()) err = field(out, "automatic", info.automatic ? "1" : "0");
                if (err.isOk()) err = number(out, "files", info.files);
                if (err.isOk()) err = number(out, "links", info.links);
                if (err.isOk()) err = number(out, "directories", info.directories);
                return err;
        }

        // Points @p info's views into its own `text` (status lines).
        Error parseStatus(Info &info) {
                PkgManifest::Reader r(info.text.view());
                PkgManifest::Entry  e;
                Error               err;
                while ((err = r.next(e)).isOk()) {
                        if (e.kind != PkgManifest::Kind::Field) return Error::CorruptData;
                        uint64_t   n = 0;
                        const bool isNumber = ::bmds::Detail::PkgText::parseDecimal(e.value, n);
                        if (e.key == StringView("name"))
                                info.name = e.value;
                        else if (e.key == StringView("version"))
                                info.version = e.value;
                        else if (e.key == StringView("arch"))
                                info.arch = e.value;
                        else if (e.key == StringView("abi") && isNumber)
                                info.abi = uint32_t(n);
                        else if (e.key == StringView("title"))
                                info.title = e.value;
                        else if (e.key == StringView("summary"))
                                info.summary = e.value;
                        else if (e.key == StringView("category"))
                                info.category = e.value;
                        else if (e.key == StringView("license"))
                                info.license = e.value;
                        else if (e.key == StringView("homepage"))
                                info.homepage = e.value;
                        else if (e.key == StringView("depends"))
                                info.depends = e.value;
                        else if (e.key == StringView("installed") && isNumber)
                                info.installedBytes = n;
                        else if (e.key == StringView("size") && isNumber)
                                info.fileBytes = n;
                        else if (e.key == StringView("made") && isNumber)
                                info.madeAt = int64_t(n);
                        else if (e.key == StringView("signer"))
                                info.signer = e.value == StringView("trusted") ? Packages::Signer::Trusted
                                              : e.value == StringView("added") ? Packages::Signer::Added
                                                                               : Packages::Signer::Dev;
                        else if (e.key == StringView("key-id") && e.value.size() == 2 * SignedContainer::KeyIdBytes) {
                                for (size_t i = 0; i < SignedContainer::KeyIdBytes; ++i) {
                                        const int hi = ::bmds::Detail::PkgText::hexValue(e.value[2 * i]);
                                        const int lo = ::bmds::Detail::PkgText::hexValue(e.value[2 * i + 1]);
                                        info.keyId[i] = uint8_t((hi < 0 ? 0 : hi) * 16 + (lo < 0 ? 0 : lo));
                                }
                        } else if (e.key == StringView("automatic"))
                                info.automatic = e.value == StringView("1");
                        else if (e.key == StringView("files") && isNumber)
                                info.files = uint32_t(n);
                        else if (e.key == StringView("links") && isNumber)
                                info.links = uint32_t(n);
                        else if (e.key == StringView("directories") && isNumber)
                                info.directories = uint32_t(n);
                }
                if (err != Error::EndOfFile) return Error::CorruptData;
                return PkgFile::isValidName(info.name) && PkgVersion::isValid(info.version) ? Error::Ok
                                                                                            : Error::CorruptData;
        }

        // Re-points @p info into a text of its own, formatted from its current views.
        Error settle(Info &info) {
                String text;
                Error  err = formatStatus(info, text);
                if (err.isError()) return err;
                const bool automatic = info.automatic;
                info = Info{};
                info.text = std::move(text);
                err = parseStatus(info);
                info.automatic = automatic;
                return err;
        }

        // ---- A package file, loaded and checked ----------------------------------------------


        Error readSection(FileHandle &file, const PkgFile::Section &s, size_t limit, String &out) {
                if (s.size > limit) return Error::OutOfRange;
                out.clear();
                Error err = out.reserve(s.size);
                if (err.isError()) return err;
                char buf[512];
                for (uint32_t at = 0; at < s.size;) {
                        const size_t n = s.size - at < sizeof(buf) ? s.size - at : sizeof(buf);
                        err = readAt(file, uint64_t(s.offset) + at, reinterpret_cast<uint8_t *>(buf), n);
                        if (err.isError()) return err;
                        err = out.append(StringView(buf, n));
                        if (err.isError()) return err;
                        at += uint32_t(n);
                }
                return Error::Ok;
        }

        bool hashMatches(const void *data, size_t bytes, const uint8_t (&hash)[SignedContainer::HashBytes]) {
                Blake2s h;
                h.update(data, bytes);
                uint8_t digest[Blake2s::DigestBytes];
                h.final(digest);
                return std::memcmp(digest, hash, sizeof(digest)) == 0;
        }

        // Hashes section @p s of @p file in chunks through @p buf.
        Error sectionHashMatches(FileHandle &file, const PkgFile::Section &s, uint8_t *buf, bool &matches) {
                Blake2s h;
                for (uint32_t at = 0; at < s.size;) {
                        const size_t n = s.size - at < ChunkBytes ? s.size - at : ChunkBytes;
                        const Error  err = readAt(file, uint64_t(s.offset) + at, buf, n);
                        if (err.isError()) return err;
                        h.update(buf, n);
                        at += uint32_t(n);
                }
                uint8_t digest[Blake2s::DigestBytes];
                h.final(digest);
                matches = std::memcmp(digest, s.hash, sizeof(digest)) == 0;
                return Error::Ok;
        }


        Error load(StringView path, const Config &config, Loaded &out, String &why, LoadMode mode) {
                const bool peek = mode != LoadMode::Full && mode != LoadMode::Check;
                auto [file, err] = Vfs::open(path, Vfs::Read, vfs());
                if (err.isError()) return fail(why, err, "%.*s: %s", int(path.size()), path.data(), err.desc());
                out.file = std::move(file);
                auto [size, serr] = out.file->stream().seek(0, Stream::SeekFrom::End);
                if (serr.isError()) return fail(why, serr, "%.*s: %s", int(path.size()), path.data(), serr.desc());
                uint8_t      head[PkgFile::MaxPackageHeadBytes];
                const size_t headRead = size < sizeof(head) ? size_t(size) : sizeof(head);
                err = readAt(*out.file, 0, head, headRead);
                if (err.isError()) return fail(why, err, "%.*s: %s", int(path.size()), path.data(), err.desc());
                const PkgFile::Verdict v = PkgFile::parsePackage(Span<const uint8_t>(head, headRead), size, out.head);
                if (v != PkgFile::Verdict::Ok) {
                        return fail(why, Error::CorruptData, "%.*s: %s", int(path.size()), path.data(),
                                    PkgFile::verdictText(v));
                }
                const PkgFile::Package &p = out.head;
                Packages::Signer        signer = Packages::Signer::Dev;
                err = verifyHead(p, Span<const uint8_t>(head, p.headBytes), config, signer);
                if (err == Error::NoMem) return fail(why, err, "%s: out of memory", p.name);
                if (err.isError())
                        return fail(why, Error::CorruptData, "%s: not signed by a key this system accepts", p.name);
                if (signer == Packages::Signer::Dev && config.dev == Packages::DevPolicy::Refuse) {
                        return fail(why, Error::PermissionDenied,
                                    "%s: signed by the dev key (anyone could have), which the setting pkg/dev refuses",
                                    p.name);
                }
                if (StringView(p.arch) != config.arch && StringView(p.arch) != StringView(PkgFile::ArchAll)) {
                        return fail(why, Error::CorruptData, "%s: for %s, not %.*s", p.name, p.arch,
                                    int(config.arch.size()), config.arch.data());
                }
                if (p.abiVersion > config.abi) {
                        return fail(why, Error::CorruptData,
                                    "%s needs a newer BMDS (ABI %u; this is %u): update the firmware first", p.name,
                                    unsigned(p.abiVersion), unsigned(config.abi));
                }

                // Every section's hash: the small ones read whole, the data in chunks.
                const PkgFile::Section *manifest = p.find(PkgFile::SectionType::Manifest);
                const PkgFile::Section *description = p.find(PkgFile::SectionType::Description);
                const PkgFile::Section *icons = p.find(PkgFile::SectionType::Icons);
                out.data = p.find(PkgFile::SectionType::Data);
                if (manifest == nullptr || out.data == nullptr) {
                        return fail(why, Error::CorruptData, "%s: no manifest or data", p.name);
                }
                const struct {
                                const PkgFile::Section *section;
                                size_t                  limit;
                                String                 *into;
                } small[] = {{manifest, MaxManifestBytes, &out.manifest},
                             {description, MaxDescriptionBytes, &out.description},
                             {icons, MaxIconsBytes, &out.icons}};
                for (const auto &s : small) {
                        if (s.section == nullptr || (mode == LoadMode::Peek && s.into != &out.manifest)) continue;
                        err = readSection(*out.file, *s.section, s.limit, *s.into);
                        if (err.isError()) return fail(why, err, "%s: %s", p.name, err.desc());
                        if (!hashMatches(s.into->data(), s.into->size(), s.section->hash)) {
                                return fail(why, Error::CorruptData, "%s: damaged (a hash doesn't match)", p.name);
                        }
                }
                if (mode == LoadMode::Full) {
                        Buffer buf(ChunkBytes);
                        if (buf.data() == nullptr) return fail(why, Error::NoMem, "%s: out of memory", p.name);
                        bool matches = false;
                        err = sectionHashMatches(*out.file, *out.data, buf.data(), matches);
                        if (err.isError()) return fail(why, err, "%s: %s", p.name, err.desc());
                        if (!matches)
                                return fail(why, Error::CorruptData, "%s: damaged (a hash doesn't match)", p.name);
                }

                // The manifest whole: its fields, its entries, the sizes adding up.
                Info               &info = out.info;
                PkgManifest::Reader r(out.manifest.view());
                PkgManifest::Entry  e;
                uint64_t            total = 0;
                while ((err = r.next(e)).isOk()) {
                        switch (e.kind) {
                                case PkgManifest::Kind::Field:
                                        if (e.key == StringView("title")) info.title = e.value;
                                        if (e.key == StringView("summary")) info.summary = e.value;
                                        if (e.key == StringView("category")) info.category = e.value;
                                        if (e.key == StringView("license")) info.license = e.value;
                                        if (e.key == StringView("homepage")) info.homepage = e.value;
                                        if (e.key == StringView("depends")) info.depends = e.value;
                                        break;
                                case PkgManifest::Kind::File:
                                        ++info.files;
                                        total += e.size;
                                        break;
                                case PkgManifest::Kind::Link: ++info.links; break;
                                case PkgManifest::Kind::Directory: ++info.directories; break;
                        }
                }
                if (err != Error::EndOfFile) {
                        return fail(why, Error::CorruptData, "%s: its manifest is damaged (line %u)%s", p.name,
                                    unsigned(r.line()), err == Error::NotSupported ? ": a newer format" : "");
                }
                if (total != p.installedBytes) {
                        return fail(why, Error::CorruptData, "%s: its files don't add up to its size", p.name);
                }
                PkgDepends::Reader     deps(info.depends);
                PkgDepends::Dependency d;
                while ((err = deps.next(d)).isOk()) {}
                if (err != Error::EndOfFile)
                        return fail(why, Error::CorruptData, "%s: its dependencies are damaged", p.name);
                err = peek ? Error::Ok : out.paths.build(out.manifest.view());
                if (err == Error::Exists) return fail(why, Error::CorruptData, "%s: names a path twice", p.name);
                if (err.isError()) return fail(why, err, "%s: %s", p.name, err.desc());

                info.name = p.name;
                info.version = p.version;
                info.arch = p.arch;
                info.abi = p.abiVersion;
                info.installedBytes = p.installedBytes;
                info.fileBytes = size;
                info.madeAt = p.madeAt;
                info.signer = signer;
                std::memcpy(info.keyId, p.keyId, sizeof(info.keyId));
                return Error::Ok;
        }

        // ---- The database -----------------------------------------------------------------------

        Error readStatus(const Config &config, StringView name, Info &out) {
                auto [path, err] = dbFile(config, name, ".status");
                if (err.isError()) return err;
                out = Info{};
                err = readWhole(path.view(), MaxDescriptionBytes, out.text);
                if (err == Error::NotExist) return Error::NotFound;
                if (err.isError()) return err;
                return parseStatus(out);
        }

        Error listInstalled(const Config &config, List<String> &out) {
                auto [dir, err] = Vfs::openDirectory(config.db, vfs());
                if (err == Error::NotExist) return Error::Ok;
                if (err.isError()) return err;
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr == Error::EndOfFile) break;
                        if (nerr.isError()) return nerr;
                        const StringView n = info.name.view();
                        if (info.type != FileType::File || !n.endsWith(".status")) continue;
                        auto [name, ferr] = String::fromView(n.left(n.size() - 7));
                        if (ferr.isError()) return ferr;
                        if (!PkgFile::isValidName(name.view())) continue;
                        err = out.append(std::move(name));
                        if (err.isError()) return err;
                }
                std::sort(out.begin(), out.end(), [](const String &a, const String &b) { return a.view() < b.view(); });
                return Error::Ok;
        }

        // Whether installed package @p name (with @p depends) needs @p wanted.
        bool needs(StringView depends, StringView wanted) {
                PkgDepends::Reader     r(depends);
                PkgDepends::Dependency d;
                while (r.next(d).isOk()) {
                        if (d.name == wanted) return true;
                }
                return false;
        }

        // Removes the entries of @p manifest that @p keep doesn't name (null: all), deepest
        // directories last, emptied ones only.
        Error removeEntries(const Config &config, StringView package, StringView manifest, const PathSet *keep,
                            const Listener &listener, String &why) {
                PathSet old;
                Error   err = old.build(manifest);
                if (err.isError() && err != Error::Exists)
                        return fail(why, err, "%.*s: its record is damaged", int(package.size()), package.data());
                List<StringView>            directories(MemSpace::Psram);
                const Span<const PathEntry> entries = old.entries();
                size_t                      done = 0;
                for (const PathEntry &e : entries) {
                        ++done;
                        if (keep != nullptr && keep->find(e.path) != nullptr) continue;
                        if (e.kind == PkgManifest::Kind::Directory) {
                                err = directories.append(e.path);
                                if (err.isError()) return fail(why, err, "out of memory");
                                continue;
                        }
                        auto [full, jerr] = join(config.root, e.path);
                        if (jerr.isError()) return fail(why, jerr, "out of memory");
                        err = removeIfThere(full.view());
                        if (err.isError()) {
                                return fail(why, err, "%.*s: %s", int(full.size()), full.data(),
                                            err == Error::Busy ? "in use: quit the program using it" : err.desc());
                        }
                        progress(listener, package, Step::Removing, done, entries.size());
                }
                std::sort(directories.begin(), directories.end(),
                          [](StringView a, StringView b) { return a.size() > b.size(); });
                for (StringView d : directories) {
                        auto [full, jerr] = join(config.root, d);
                        if (jerr.isError()) return fail(why, jerr, "out of memory");
                        (void)Vfs::rmdir(full.view(), vfs()); // kept if anything else is in it
                }
                return Error::Ok;
        }

        // Writes file entry @p e from the data section (at the file's current position) to
        // `PATH.pkgnew`, checking its hash.
        Error writeNew(Loaded &pkg, const PkgManifest::Entry &e, StringView full, uint8_t *in, uint8_t *out,
                       String &why) {
                auto [temp, err] = withSuffix(full, ".pkgnew");
                if (err.isError()) return fail(why, err, "out of memory");
                auto [file, oerr] = Vfs::open(temp.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, vfs());
                if (oerr.isError()) {
                        return fail(why, oerr, "%.*s: %s", int(temp.size()), temp.data(), oerr.desc());
                }
                Blake2s  h;
                uint64_t left = e.size;
                while (left > 0) {
                        const size_t want = left < PkgFile::BlockBytes ? size_t(left) : PkgFile::BlockBytes;
                        uint8_t      word[4];
                        err = readNext(*pkg.file, word, sizeof(word));
                        PkgFile::Block b;
                        if (err.isOk()) {
                                const uint32_t w = uint32_t(word[0]) | (uint32_t(word[1]) << 8) |
                                                   (uint32_t(word[2]) << 16) | (uint32_t(word[3]) << 24);
                                err = PkgFile::readBlock(w, want, b);
                        }
                        if (err.isOk()) err = readNext(*pkg.file, in, b.bytes);
                        if (err.isOk())
                                err = PkgFile::decodeBlock(b, ByteSpan(in, b.bytes), WritableByteSpan(out, want));
                        if (err.isError()) {
                                return fail(why, Error::CorruptData, "%s: %.*s: its data is damaged", pkg.head.name,
                                            int(e.path.size()), e.path.data());
                        }
                        h.update(out, want);
                        err = writeAll(*file, ByteSpan(out, want));
                        if (err.isError()) {
                                return fail(why, err, "%.*s: %s", int(temp.size()), temp.data(),
                                            err == Error::NoSpace ? "the card is full" : err.desc());
                        }
                        left -= want;
                }
                uint8_t digest[Blake2s::DigestBytes];
                h.final(digest);
                if (std::memcmp(digest, e.hash, sizeof(digest)) != 0) {
                        return fail(why, Error::CorruptData, "%s: %.*s doesn't match its hash", pkg.head.name,
                                    int(e.path.size()), e.path.data());
                }
                return Error::Ok;
        }

        // Whether the unowned file at @p full holds what entry @p e would put there.
        bool sameAs(StringView full, const PkgManifest::Entry &e, uint8_t *buf) {
                if (e.kind == PkgManifest::Kind::Link) {
                        auto [target, err] = Vfs::readLink(full, vfs());
                        return err.isOk() && target.view() == e.target;
                }
                auto [info, serr] = Vfs::lstat(full, vfs());
                if (serr.isError() || info.type != FileType::File || info.size != e.size) return false;
                auto [file, oerr] = Vfs::open(full, Vfs::Read, vfs());
                if (oerr.isError()) return false;
                Blake2s h;
                for (uint64_t left = e.size; left > 0;) {
                        const size_t n = left < ChunkBytes ? size_t(left) : ChunkBytes;
                        if (readNext(*file, buf, n).isError()) return false;
                        h.update(buf, n);
                        left -= n;
                }
                uint8_t digest[Blake2s::DigestBytes];
                h.final(digest);
                return std::memcmp(digest, e.hash, sizeof(digest)) == 0;
        }

        Error installLocked(StringView path, const Config &config, const Packages::InstallOptions &options,
                            const Listener &listener, Info *result, String &why);

        Error removeLocked(StringView name, Span<const StringView> alsoRemoving, const Config &config,
                           const Listener &listener, String &why);

        Error recoverLocked(const Config &config, const Listener &listener, String &why) {
                auto [dir, err] = Vfs::openDirectory(config.db, vfs());
                if (err == Error::NotExist) return Error::Ok;
                if (err.isError())
                        return fail(why, err, "%.*s: %s", int(config.db.size()), config.db.data(), err.desc());
                List<String> pending(MemSpace::Psram);
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr == Error::EndOfFile) break;
                        if (nerr.isError()) return fail(why, nerr, "%s", nerr.desc());
                        const StringView n = info.name.view();
                        if (!n.endsWith(".pending")) continue;
                        auto [name, ferr] = String::fromView(n.left(n.size() - 8));
                        if (ferr.isOk()) ferr = pending.append(std::move(name));
                        if (ferr.isError()) return fail(why, ferr, "out of memory");
                }
                dir.close();
                for (const String &name : pending) {
                        auto [file, ferr] = dbFile(config, name.view(), ".pending");
                        if (ferr.isError()) return fail(why, ferr, "out of memory");
                        String text;
                        err = readWhole(file.view(), 4096, text);
                        if (err.isError()) return fail(why, err, "%s: %s", file.cStr(), err.desc());
                        StringView          action, from;
                        PkgManifest::Reader r(text.view());
                        PkgManifest::Entry  e;
                        while (r.next(e).isOk()) {
                                if (e.kind != PkgManifest::Kind::Field) continue;
                                if (e.key == StringView("action")) action = e.value;
                                if (e.key == StringView("file")) from = e.value;
                        }
                        if (action == StringView("remove")) {
                                note(listener, name.view(), "finishing its removal, which was cut short");
                                err = removeLocked(name.view(), Span<const StringView>(), config, listener, why);
                                if (err == Error::NotFound) err = removeIfThere(file.view());
                        } else {
                                note(listener, name.view(), "installing it again: its installation was cut short");
                                Info                     old;
                                Packages::InstallOptions again;
                                again.automatic = readStatus(config, name.view(), old).isOk() && old.automatic;
                                err = installLocked(from, config, again, listener, nullptr, why);
                                if (err == Error::NotExist) {
                                        (void)removeIfThere(file.view());
                                        return fail(why, Error::InvalidState,
                                                    "%s: its installation was cut short, and %.*s is gone: install "
                                                    "it again",
                                                    name.cStr(), int(from.size()), from.data());
                                }
                        }
                        // A failure that won't go away by itself (damaged, refused) isn't tried again
                        // every time; a file in use is.
                        if (err.isError() && err != Error::Busy) (void)removeIfThere(file.view());
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        Error installLocked(StringView path, const Config &config, const Packages::InstallOptions &options,
                            const Listener &listener, Info *result, String &why) {
                Loaded pkg;
                // A dry run reads the data (68 MB for the standard set) only when it installs.
                Error err = load(path, config, pkg, why, options.dryRun ? LoadMode::Check : LoadMode::Full);
                if (err.isError()) return err;
                const StringView name = pkg.info.name;
                progress(listener, name, Step::Checking, 0, 1);

                // What it needs, installed (or, for a dry run, earlier in the plan).
                {
                        String                 missing;
                        PkgDepends::Reader     deps(pkg.info.depends);
                        PkgDepends::Dependency d;
                        while (deps.next(d).isOk()) {
                                const auto suits = [&](StringView version) {
                                        return d.minVersion.isEmpty() ||
                                               PkgVersion::compare(version, d.minVersion) >= 0;
                                };
                                Info have;
                                if (readStatus(config, d.name, have).isOk() && suits(have.version)) continue;
                                bool planned = false;
                                for (const Packages::Planned &e : options.earlier)
                                        planned = planned || (e.info.name == d.name && suits(e.info.version));
                                if (planned && options.dryRun) continue;
                                (void)missing.appendf("%s%.*s", missing.isEmpty() ? "" : ", ", int(d.name.size()),
                                                      d.name.data());
                                if (!d.minVersion.isEmpty())
                                        (void)missing.appendf(" (>= %.*s)", int(d.minVersion.size()),
                                                              d.minVersion.data());
                        }
                        if (!missing.isEmpty()) {
                                return fail(why, Error::InvalidState, "%.*s needs %s: install that first",
                                            int(name.size()), name.data(), missing.cStr());
                        }
                }

                // The version installed now, and what it owns.
                Info       old;
                String     oldManifest;
                const bool upgrading = readStatus(config, name, old).isOk();
                if (upgrading) {
                        auto [file, ferr] = dbFile(config, name, ".manifest");
                        if (ferr.isOk()) ferr = readWhole(file.view(), MaxManifestBytes, oldManifest);
                        if (ferr.isError())
                                return fail(why, ferr, "%.*s: its record is damaged", int(name.size()), name.data());
                        const int order = PkgVersion::compare(pkg.info.version, old.version);
                        if (order < 0) {
                                String text;
                                (void)text.appendf("going back from %.*s to %.*s", int(old.version.size()),
                                                   old.version.data(), int(pkg.info.version.size()),
                                                   pkg.info.version.data());
                                note(listener, name, text.view());
                        }
                }
                PathSet oldPaths;
                if (upgrading && oldPaths.build(oldManifest.view()).isError() && !oldManifest.isEmpty()) {
                        return fail(why, Error::CorruptData, "%.*s: its record is damaged", int(name.size()),
                                    name.data());
                }

                // Files another package owns.
                {
                        List<String> others(MemSpace::Psram);
                        err = listInstalled(config, others);
                        if (err.isError()) return fail(why, err, "%s", err.desc());
                        for (const String &other : others) {
                                if (other.view() == name) continue;
                                auto [file, ferr] = dbFile(config, other.view(), ".manifest");
                                String text;
                                if (ferr.isOk()) ferr = readWhole(file.view(), MaxManifestBytes, text);
                                if (ferr.isError()) continue; // a record without a manifest owns nothing
                                PkgManifest::Reader r(text.view());
                                PkgManifest::Entry  e;
                                while (r.next(e).isOk()) {
                                        if (e.kind == PkgManifest::Kind::Field) continue;
                                        const PathEntry *mine = pkg.paths.find(e.path);
                                        if (mine == nullptr) continue;
                                        if (mine->kind == PkgManifest::Kind::Directory &&
                                            e.kind == PkgManifest::Kind::Directory)
                                                continue;
                                        return fail(why, Error::InvalidState, "%.*s: %.*s belongs to %s",
                                                    int(name.size()), name.data(), int(e.path.size()), e.path.data(),
                                                    other.cStr());
                                }
                        }
                }

                Buffer in(PkgFile::BlockBytes);
                Buffer out(PkgFile::BlockBytes);
                if (in.data() == nullptr || out.data() == nullptr) return fail(why, Error::NoMem, "out of memory");

                // Unowned files in the way: the same bytes are taken over; others only if forced.
                {
                        PkgManifest::Reader r(pkg.manifest.view());
                        PkgManifest::Entry  e;
                        while (r.next(e).isOk()) {
                                if (e.kind == PkgManifest::Kind::Field) continue;
                                if (upgrading && oldPaths.find(e.path) != nullptr) continue;
                                auto [full, jerr] = join(config.root, e.path);
                                if (jerr.isError()) return fail(why, jerr, "out of memory");
                                auto [there, serr] = Vfs::lstat(full.view(), vfs());
                                if (serr == Error::NotExist) continue;
                                if (serr.isError()) return fail(why, serr, "%s: %s", full.cStr(), serr.desc());
                                if (e.kind == PkgManifest::Kind::Directory) {
                                        if (there.type == FileType::Directory) continue;
                                        return fail(why, Error::InvalidState, "%s is in the way of a directory",
                                                    full.cStr());
                                }
                                if (there.type == FileType::Directory) {
                                        return fail(why, Error::InvalidState, "%s is a directory, in the way",
                                                    full.cStr());
                                }
                                if (sameAs(full.view(), e, in.data())) continue;
                                if (!options.overwrite) {
                                        return fail(why, Error::InvalidState,
                                                    "%s is in the way (no package's, and different): --overwrite "
                                                    "replaces it",
                                                    full.cStr());
                                }
                        }
                }

                // Room for it.
                {
                        auto [fs, ferr] = Vfs::statfs(config.root, vfs());
                        if (ferr.isOk() && fs.freeBytes < pkg.info.installedBytes) {
                                return fail(why, Error::NoSpace, "%.*s needs %llu bytes; %llu are free",
                                            int(name.size()), name.data(),
                                            static_cast<unsigned long long>(pkg.info.installedBytes),
                                            static_cast<unsigned long long>(fs.freeBytes));
                        }
                }
                // What is reported: the new version, and whether it was asked for.
                const auto report = [&]() -> Error {
                        if (result == nullptr) return Error::Ok;
                        pkg.info.automatic = options.automatic && (!upgrading || old.automatic);
                        String text;
                        Error  ferr = formatStatus(pkg.info, text);
                        if (ferr.isError()) return ferr;
                        *result = Info{};
                        result->text = std::move(text);
                        return parseStatus(*result);
                };
                if (options.dryRun) return report();

                // From here on a power cut is finished by recover().
                auto [pendingPath, perr] = dbFile(config, name, ".pending");
                if (perr.isOk()) perr = makeDirectories(config.db);
                if (perr.isError())
                        return fail(why, perr, "%.*s: %s", int(config.db.size()), config.db.data(), perr.desc());
                {
                        auto [absolute, aerr] = Vfs::normalize(path, "/");
                        String text;
                        if (aerr.isOk()) aerr = text.appendf("action\tinstall\nfile\t%s\n", absolute.cStr());
                        if (aerr.isOk()) aerr = writeWhole(pendingPath.view(), text.view());
                        if (aerr.isError()) return fail(why, aerr, "%s: %s", pendingPath.cStr(), aerr.desc());
                }
                err = makeDirectories(config.root);
                if (err.isError())
                        return fail(why, err, "%.*s: %s", int(config.root.size()), config.root.data(), err.desc());

                // Directories, then each file beside its place.
                const auto cleanup = [&]() {
                        PkgManifest::Reader r(pkg.manifest.view());
                        PkgManifest::Entry  e;
                        while (r.next(e).isOk()) {
                                if (e.kind != PkgManifest::Kind::File) continue;
                                auto [full, jerr] = join(config.root, e.path);
                                if (jerr.isOk()) jerr = full.append(".pkgnew");
                                if (jerr.isOk()) (void)removeIfThere(full.view());
                        }
                        (void)removeIfThere(pendingPath.view());
                };
                {
                        auto [at, serr] = pkg.file->stream().seek(int64_t(pkg.data->offset), Stream::SeekFrom::Start);
                        (void)at;
                        if (serr.isError()) return fail(why, serr, "%s", serr.desc());
                        PkgManifest::Reader r(pkg.manifest.view());
                        PkgManifest::Entry  e;
                        uint64_t            done = 0;
                        while (r.next(e).isOk()) {
                                if (e.kind == PkgManifest::Kind::Field || e.kind == PkgManifest::Kind::Link) continue;
                                auto [full, jerr] = join(config.root, e.path);
                                if (jerr.isError()) {
                                        cleanup();
                                        return fail(why, jerr, "out of memory");
                                }
                                if (e.kind == PkgManifest::Kind::Directory) {
                                        err = makeDirectories(full.view());
                                } else {
                                        const size_t slash = full.view().lastIndexOf('/');
                                        err = makeDirectories(full.view().left(slash));
                                        if (err.isOk()) err = writeNew(pkg, e, full.view(), in.data(), out.data(), why);
                                        done += e.size;
                                        progress(listener, name, Step::Writing, done, pkg.info.installedBytes);
                                }
                                if (err.isError()) {
                                        if (why.isEmpty()) (void)why.appendf("%s: %s", full.cStr(), err.desc());
                                        cleanup();
                                        return err;
                                }
                        }
                }
                pkg.file = FileHandle::Ptr(); // closed: it may be on the same card, and is read no more

                // Into place: files renamed over the old, links remade.
                {
                        PkgManifest::Reader r(pkg.manifest.view());
                        PkgManifest::Entry  e;
                        size_t              done = 0;
                        const size_t        total = pkg.info.files + pkg.info.links;
                        while (r.next(e).isOk()) {
                                if (e.kind != PkgManifest::Kind::File && e.kind != PkgManifest::Kind::Link) continue;
                                auto [full, jerr] = join(config.root, e.path);
                                if (jerr.isError()) return fail(why, jerr, "out of memory");
                                if (e.kind == PkgManifest::Kind::File) {
                                        auto [temp, terr] = withSuffix(full.view(), ".pkgnew");
                                        if (terr.isOk())
                                                terr = Vfs::rename(temp.view(), full.view(), Vfs::RenameMode::Replace,
                                                                   vfs());
                                        err = terr;
                                } else {
                                        auto [target, lerr] = Vfs::readLink(full.view(), vfs());
                                        if (lerr.isOk() && target.view() == e.target) {
                                                err = Error::Ok;
                                        } else {
                                                err = removeIfThere(full.view());
                                                if (err.isOk()) err = Vfs::symlink(e.target, full.view(), vfs());
                                        }
                                }
                                if (err.isError()) {
                                        // Left pending: recover() installs it again.
                                        return fail(why, err, "%s: %s", full.cStr(),
                                                    err == Error::Busy ? "in use: quit the program using it, then run "
                                                                         "pkg again"
                                                                       : err.desc());
                                }
                                progress(listener, name, Step::Replacing, ++done, total);
                        }
                }

                // What the old version had that this one doesn't.
                if (upgrading) {
                        err = removeEntries(config, name, oldManifest.view(), &pkg.paths, listener, why);
                        if (err.isError()) return err;
                }

                // The records, the status last: it is what says the package is installed.
                Info &info = pkg.info;
                info.automatic = options.automatic && (!upgrading || old.automatic);
                String status;
                err = formatStatus(info, status);
                const struct {
                                const char *suffix;
                                StringView  text;
                } records[] = {{".manifest", pkg.manifest.view()},
                               {".description", pkg.description.view()},
                               {".icons", pkg.icons.view()},
                               {".status", status.view()}};
                for (const auto &rec : records) {
                        if (err.isError()) break;
                        auto [file, ferr] = dbFile(config, name, rec.suffix);
                        if (ferr.isOk())
                                ferr = rec.text.isEmpty() ? removeIfThere(file.view())
                                                          : writeWhole(file.view(), rec.text);
                        err = ferr;
                }
                if (err.isError())
                        return fail(why, err, "%.*s: its record: %s", int(config.db.size()), config.db.data(),
                                    err.desc());
                err = removeIfThere(pendingPath.view());
                return err.isError() ? err : report();
        }

        // Whether @p name can go: installed, and needed by nothing staying.
        Error checkRemoveLocked(StringView name, Span<const StringView> alsoRemoving, const Config &config,
                                String &why) {
                Info  info;
                Error err = readStatus(config, name, info);
                if (err == Error::NotFound)
                        return fail(why, err, "%.*s isn't installed", int(name.size()), name.data());
                if (err.isError()) return fail(why, err, "%.*s: its record is damaged", int(name.size()), name.data());

                // Nothing staying may need it.
                {
                        List<String> others(MemSpace::Psram);
                        err = listInstalled(config, others);
                        if (err.isError()) return fail(why, err, "%s", err.desc());
                        String needers;
                        for (const String &other : others) {
                                if (other.view() == name) continue;
                                bool going = false;
                                for (StringView a : alsoRemoving) going = going || a == other.view();
                                if (going) continue;
                                Info o;
                                if (readStatus(config, other.view(), o).isOk() && needs(o.depends, name)) {
                                        (void)needers.appendf("%s%s", needers.isEmpty() ? "" : ", ", other.cStr());
                                }
                        }
                        if (!needers.isEmpty()) {
                                return fail(why, Error::InvalidState, "%s need%s %.*s: remove %s too", needers.cStr(),
                                            needers.view().contains(',') ? "" : "s", int(name.size()), name.data(),
                                            needers.view().contains(',') ? "them" : "it");
                        }
                }
                return Error::Ok;
        }

        Error removeLocked(StringView name, Span<const StringView> alsoRemoving, const Config &config,
                           const Listener &listener, String &why) {
                Error err = checkRemoveLocked(name, alsoRemoving, config, why);
                if (err.isError()) return err;

                auto [pendingPath, perr] = dbFile(config, name, ".pending");
                if (perr.isOk()) perr = writeWhole(pendingPath.view(), StringView("action\tremove\n"));
                if (perr.isError()) return fail(why, perr, "%s: %s", pendingPath.cStr(), perr.desc());
                auto [file, ferr] = dbFile(config, name, ".manifest");
                String manifest;
                if (ferr.isOk()) ferr = readWhole(file.view(), MaxManifestBytes, manifest);
                if (ferr.isError() && ferr != Error::NotExist)
                        return fail(why, ferr, "%s: %s", file.cStr(), ferr.desc());
                err = removeEntries(config, name, manifest.view(), nullptr, listener, why);
                if (err.isError()) return err;
                for (const char *suffix : {".manifest", ".description", ".icons", ".status"}) {
                        auto [record, rerr] = dbFile(config, name, suffix);
                        if (rerr.isOk()) rerr = removeIfThere(record.view());
                        if (rerr.isError()) return fail(why, rerr, "%s: %s", record.cStr(), rerr.desc());
                }
                return removeIfThere(pendingPath.view());
        }

} // namespace Packages::Detail

using namespace Packages::Detail;

Error Packages::recover(const Config &config, const Listener &listener, String &why) {
        Lock lock;
        if (!lock.held()) return fail(why, Error::Busy, "another package operation is running");
        return recoverLocked(config, listener, why);
}

Error Packages::inspectFile(StringView path, const Config &config, Info &out, String *manifest, String *description,
                            String &why) {
        Loaded      pkg;
        const Error err = load(path, config, pkg, why, LoadMode::Full);
        if (err.isError()) return err;
        out = std::move(pkg.info);
        const Error serr = settle(out);
        if (manifest != nullptr) *manifest = std::move(pkg.manifest);
        if (description != nullptr) *description = std::move(pkg.description);
        return serr;
}

Error Packages::peekFile(StringView path, const Config &config, Info &out, String &why) {
        Loaded      pkg;
        const Error err = load(path, config, pkg, why, LoadMode::Peek);
        if (err.isError()) return err;
        out = std::move(pkg.info);
        return settle(out); // its own text: it outlives the manifest it viewed
}

Error Packages::installFile(StringView path, const Config &config, const InstallOptions &options,
                            const Listener &listener, Info &out, String &why) {
        Lock lock;
        if (!lock.held()) return fail(why, Error::Busy, "another package operation is running");
        Error err = recoverLocked(config, listener, why);
        if (err.isError()) return err;
        why.clear();
        return installLocked(path, config, options, listener, &out, why);
}

Error Packages::remove(StringView name, Span<const StringView> alsoRemoving, const Config &config,
                       const Listener &listener, String &why) {
        Lock lock;
        if (!lock.held()) return fail(why, Error::Busy, "another package operation is running");
        Error err = recoverLocked(config, listener, why);
        if (err.isError()) return err;
        why.clear();
        return removeLocked(name, alsoRemoving, config, listener, why);
}

Error Packages::checkRemove(StringView name, Span<const StringView> alsoRemoving, const Config &config, String &why) {
        Lock lock;
        if (!lock.held()) return fail(why, Error::Busy, "another package operation is running");
        why.clear();
        return checkRemoveLocked(name, alsoRemoving, config, why);
}

Error Packages::setAutomatic(StringView name, bool automatic, const Config &config, String &why) {
        Lock lock;
        if (!lock.held()) return fail(why, Error::Busy, "another package operation is running");
        why.clear();
        Info  info;
        Error err = PkgFile::isValidName(name) ? readStatus(config, name, info) : Error::NotFound;
        if (err == Error::NotFound) return fail(why, err, "%.*s isn't installed", int(name.size()), name.data());
        if (err.isError()) return fail(why, err, "%.*s: its record is damaged", int(name.size()), name.data());
        if (info.automatic == automatic) return Error::Ok;
        info.automatic = automatic;
        String text;
        err = formatStatus(info, text);
        auto [file, ferr] = dbFile(config, name, ".status");
        if (err.isOk()) err = ferr;
        if (err.isOk()) err = writeWhole(file.view(), text.view());
        if (err.isError()) return fail(why, err, "%s: %s", file.cStr(), err.desc());
        return Error::Ok;
}

Error Packages::unneeded(Span<const StringView> removing, const Config &config, List<String> &out) {
        out.clear();
        List<Info>               infos(MemSpace::Psram);
        List<PkgSolver::Package> installed(MemSpace::Psram);
        List<size_t>             found(MemSpace::Psram);
        Error                    err = installedForWalk(config, infos, installed);
        if (err.isOk()) err = PkgSolver::unneeded(installed.span(), removing, found);
        for (size_t i = 0; err.isOk() && i < found.size(); ++i) {
                auto [name, nerr] = String::fromView(installed[found[i]].name);
                err = nerr.isOk() ? out.append(std::move(name)) : nerr;
        }
        return err; // in the records' order: sorted
}

Result<List<String>> Packages::installed(const Config &config) {
        List<String> out(MemSpace::Psram);
        const Error  err = listInstalled(config, out);
        return Result<List<String>>(std::move(out), err);
}

Error Packages::readInstalled(StringView name, const Config &config, Info &out) {
        if (!PkgFile::isValidName(name)) return Error::NotFound;
        return readStatus(config, name, out);
}

Error Packages::readManifest(StringView name, const Config &config, String &out) {
        if (!PkgFile::isValidName(name)) return Error::NotFound;
        auto [file, err] = dbFile(config, name, ".manifest");
        if (err.isOk()) err = readWhole(file.view(), MaxManifestBytes, out);
        return err == Error::NotExist ? Error::NotFound : err;
}

Error Packages::readDescription(StringView name, const Config &config, String &out) {
        if (!PkgFile::isValidName(name)) return Error::NotFound;
        auto [file, err] = dbFile(config, name, ".description");
        if (err.isOk()) err = readWhole(file.view(), MaxDescriptionBytes, out);
        return err == Error::NotExist ? Error::NotFound : err;
}

Error Packages::verify(StringView name, const Config                                    &config,
                       void (*found)(void *arg, StringView path, Problem problem), void *arg) {
        String manifest;
        Error  err = readManifest(name, config, manifest);
        if (err.isError()) return err;
        Buffer buf(ChunkBytes);
        if (buf.data() == nullptr) return Error::NoMem;
        PkgManifest::Reader r(manifest.view());
        PkgManifest::Entry  e;
        while ((err = r.next(e)).isOk()) {
                if (e.kind == PkgManifest::Kind::Field || e.kind == PkgManifest::Kind::Directory) continue;
                auto [full, jerr] = join(config.root, e.path);
                if (jerr.isError()) return jerr;
                auto [there, serr] = Vfs::lstat(full.view(), vfs());
                if (serr.isError()) {
                        found(arg, e.path, Problem::Missing);
                } else if (e.kind == PkgManifest::Kind::Link && there.type != FileType::Link) {
                        found(arg, e.path, Problem::NotLink);
                } else if (!sameAs(full.view(), e, buf.data())) {
                        found(arg, e.path, Problem::Changed);
                }
        }
        return err == Error::EndOfFile ? Error::Ok : Error::CorruptData;
}

Error Packages::icon(StringView name, uint16_t side, const Config &config, List<uint8_t> &out) {
        if (!PkgFile::isValidName(name)) return Error::NotFound;
        auto [file, err] = dbFile(config, name, ".icons");
        String icons;
        if (err.isOk()) err = readWhole(file.view(), MaxIconsBytes, icons);
        if (err == Error::NotExist) return Error::NotFound;
        if (err.isError()) return err;
        return pickIcon(ByteSpan(reinterpret_cast<const uint8_t *>(icons.data()), icons.size()), false, StringView(),
                        side, out);
}

BMDS_NAMESPACE_END
