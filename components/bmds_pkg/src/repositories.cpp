/**
 * @file      repositories.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * Where packages come from (devplan/packages.md §8, §21): the sources, their lists in the cache,
 * a folder indexed on the board, and planning over files, names and the lists.
 */

#include <algorithm>
#include <cstring>
#include <bmds/blake2s.h>
#include <bmds/http.h>
#include <bmds/packages.h>
#include <bmds/pkgfile.h>
#include <bmds/pkgindex.h>
#include <bmds/pkgsolver.h>
#include <bmds/pkgversion.h>
#include <bmds/vfs.h>
#include "packagesdetail.h"

BMDS_NAMESPACE_BEGIN

namespace Packages::Detail {

        // The most a list may be: far more than any index has (the project's is 9 KB).
        constexpr size_t MaxListBytes = 4 * 1024 * 1024;

        constexpr StringView FileScheme("file:");

        Error installedForWalk(const Config &config, List<Info> &infos, List<PkgSolver::Package> &out) {
                List<String> names(MemSpace::Psram);
                Error        err = listInstalled(config, names);
                if (err.isOk()) err = infos.reserve(names.size());
                for (size_t i = 0; err.isOk() && i < names.size(); ++i) {
                        Info info;
                        if (readStatus(config, names[i].view(), info).isError()) continue; // a damaged record
                        err = infos.append(std::move(info));
                }
                // After every append: the views are into each record's own text.
                for (size_t i = 0; err.isOk() && i < infos.size(); ++i) {
                        const Info &info = infos[i];
                        err = out.append(PkgSolver::Package{info.name, info.version, info.depends, info.automatic});
                }
                return err;
        }

        Error pickIcon(ByteSpan section, bool named, StringView name, uint16_t side, List<uint8_t> &out) {
                PkgFile::IconReader r(section, named);
                PkgFile::Icon       i;
                ByteSpan            best;
                uint16_t            bestSide = 0;
                while (r.next(i).isOk()) {
                        if (named && i.name != name) continue;
                        // That side, else the next bigger, else the biggest.
                        const bool better = bestSide == 0 ||
                                            (i.side >= side && (bestSide < side || i.side < bestSide)) ||
                                            (i.side < side && bestSide < side && i.side > bestSide);
                        if (better) {
                                best = i.png;
                                bestSide = i.side;
                        }
                }
                if (bestSide == 0) return Error::NotFound;
                out.clear();
                return out.append(best);
        }

        // The sources in @p setting, in turn: each trimmed, empty ones skipped.
        template <typename F> Error eachSource(StringView setting, F f) {
                while (!setting.isEmpty()) {
                        const size_t     semi = setting.indexOf(';');
                        const StringView one = setting.left(semi).trimmed();
                        setting = semi == StringView::NPos ? StringView() : setting.mid(semi + 1);
                        if (one.isEmpty()) continue;
                        const Error err = f(one);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        constexpr size_t MaxSignedBy = 8;

        // One source, read: its folder and the keys it names. Views into its text.
        struct Source {
                        StringView text;
                        StringView folder; ///< A `file:` source's.
                        StringView url;    ///< A network source's: its index's folder.
                        bool       network = false;
                        StringView signedBy[MaxSignedBy];
                        size_t     signedByCount = 0;

                        Span<const StringView> keys() const { return Span<const StringView>(signedBy, signedByCount); }
        };

        bool isKeyId(StringView id) {
                if (id.size() != 2 * SignedContainer::KeyIdBytes) return false;
                for (char c : id) {
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
                }
                return true;
        }

        // Reads source @p text (trimmed): `file:/FOLDER [signed-by=ID,...]`.
        Error parseSource(StringView text, Source &out, String &why) {
                out = Source{};
                out.text = text;
                StringView rest = text;
                bool       first = true;
                while (!rest.isEmpty()) {
                        const size_t     space = rest.indexOf(' ');
                        const StringView word = rest.left(space);
                        rest = space == StringView::NPos ? StringView() : rest.mid(space + 1).trimmed();
                        if (first) {
                                first = false;
                                Http::Url url;
                                if (Http::parseUrl(word, url).isOk()) {
                                        out.url = word;
                                        out.network = true;
                                        continue;
                                }
                                if (!word.startsWith(FileScheme)) {
                                        return fail(why, Error::Invalid,
                                                    "%.*s: a source is file:/FOLDER, or an http: or https: URL",
                                                    int(text.size()), text.data());
                                }
                                out.folder = word.mid(FileScheme.size());
                                continue;
                        }
                        constexpr StringView SignedBy("signed-by=");
                        if (!word.startsWith(SignedBy) || out.signedByCount != 0) {
                                return fail(why, Error::Invalid, "%.*s: %.*s isn't signed-by=ID[,ID...]",
                                            int(text.size()), text.data(), int(word.size()), word.data());
                        }
                        StringView ids = word.mid(SignedBy.size());
                        while (!ids.isEmpty()) {
                                const size_t     comma = ids.indexOf(',');
                                const StringView id = ids.left(comma);
                                ids = comma == StringView::NPos ? StringView() : ids.mid(comma + 1);
                                if (!isKeyId(id) || out.signedByCount == MaxSignedBy) {
                                        return fail(
                                                why, Error::Invalid,
                                                "%.*s: a key's id is 16 hex digits (pkg key list), at most %u of them",
                                                int(text.size()), text.data(), unsigned(MaxSignedBy));
                                }
                                out.signedBy[out.signedByCount++] = id;
                        }
                        if (out.signedByCount == 0)
                                return fail(why, Error::Invalid, "%.*s: signed-by names no key", int(text.size()),
                                            text.data());
                }
                return Error::Ok;
        }

        // A source's id: 16 hex digits of its BLAKE2s, naming its list.
        void sourceId(StringView source, char (&out)[17]) {
                Blake2s h;
                h.update(source.data(), source.size());
                uint8_t digest[Blake2s::DigestBytes];
                h.final(digest);
                hex(digest, 8, out);
                return;
        }

        // CACHE/lists, and a source's list in it with @p suffix (".bmpi", ".source").
        Result<String> listsDir(const Config &config) {
                return join(config.cache, "lists");
        }

        Result<String> listFile(const Config &config, StringView source, StringView suffix) {
                char id[17];
                sourceId(source, id);
                auto [dir, err] = listsDir(config);
                String path;
                if (err.isOk()) {
                        auto [p, jerr] = join(dir.view(), id);
                        path = std::move(p);
                        err = jerr;
                }
                if (err.isOk()) err = path.append(suffix);
                return Result<String>(std::move(path), err);
        }

        // Where a list's `File` is, for a `file:` source's @p folder: a name or a relative path
        // in it. Empty for what a folder can't reach (a URL, an absolute path, `..`).
        Result<String> filePath(StringView folder, StringView file) {
                if (file.isEmpty() || file.startsWith('/') || file.contains(':') || !PkgManifest::isValidPath(file))
                        return Result<String>(String(), Error::Ok);
                return join(folder, file);
        }

        // Whether @p name is a package file's name.
        bool isPackageName(StringView name) {
                return name.size() > 5 && name.endsWith(".bmpk");
        }

        // The `.bmpk` files in @p folder: names and sizes, sorted.
        struct FolderFile {
                        String   name;
                        uint64_t size = 0;
        };

        Error folderPackages(StringView folder, List<FolderFile> &out) {
                auto [dir, err] = Vfs::openDirectory(folder, vfs());
                if (err.isError()) return err;
                for (;;) {
                        auto [entry, nerr] = dir.next();
                        if (nerr == Error::EndOfFile) break;
                        if (nerr.isError()) return nerr;
                        if (entry.type != FileType::File || !isPackageName(entry.name.view())) continue;
                        err = out.append(FolderFile{std::move(entry.name), entry.size});
                        if (err.isError()) return err;
                }
                std::sort(out.begin(), out.end(),
                          [](const FolderFile &a, const FolderFile &b) { return a.name.view() < b.name.view(); });
                return Error::Ok;
        }

        // Reads a list (an index) in @p bytes: its head and its stanzas' text.
        Error readList(const List<uint8_t> &bytes, PkgFile::Index &index, StringView &stanzas) {
                if (PkgFile::parseIndex(bytes.span(), bytes.size(), index) != PkgFile::Verdict::Ok)
                        return Error::CorruptData;
                const PkgFile::Section *s = index.find(PkgFile::IndexSectionType::Stanzas);
                stanzas = s == nullptr ? StringView()
                                       : StringView(reinterpret_cast<const char *>(bytes.data() + s->offset), s->size);
                return Error::Ok;
        }

        Error readBytes(StringView path, size_t limit, List<uint8_t> &out) {
                String text;
                Error  err = readWhole(path, limit, text);
                out.clear();
                if (err.isOk())
                        err = out.append(
                                Span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
                return err;
        }

        // Whether the folder's own index lists exactly its packages, by name and size.
        bool indexMatches(const List<uint8_t> &bytes, const List<FolderFile> &files) {
                PkgFile::Index index;
                StringView     stanzas;
                if (readList(bytes, index, stanzas).isError()) return false;
                PkgIndex::Reader r(stanzas);
                PkgIndex::Stanza s;
                size_t           listed = 0;
                Error            err;
                while ((err = r.next(s)).isOk()) {
                        bool found = false;
                        for (const FolderFile &f : files)
                                found = found || (f.name.view() == s.file && f.size == s.size);
                        if (!found) return false;
                        ++listed;
                }
                return err == Error::EndOfFile && listed == files.size();
        }

        Error appendField(String &out, const char *key, StringView value) {
                if (value.isEmpty()) return Error::Ok;
                return out.appendf("%s: %.*s\n", key, int(value.size()), value.data());
        }

        // The index of @p folder's packages, into @p out; each left out said to @p listener.
        Error indexOf(StringView folder, const List<FolderFile> &files, const Config &config, const Listener &listener,
                      uint32_t &count, List<uint8_t> &out, String &why) {
                count = 0;
                String        stanzas;
                List<uint8_t> icons(MemSpace::Psram);
                Buffer        buf(ChunkBytes);
                if (buf.data() == nullptr) return fail(why, Error::NoMem, "out of memory");
                for (const FolderFile &f : files) {
                        auto [path, jerr] = join(folder, f.name.view());
                        if (jerr.isError()) return fail(why, jerr, "out of memory");
                        Loaded pkg;
                        String skip;
                        Error  err = load(path.view(), config, pkg, skip, LoadMode::Listing);
                        // The whole file's hash, as an index lists it.
                        Blake2s h;
                        if (err.isOk()) {
                                auto [at, serr] = pkg.file->stream().seek(0, Stream::SeekFrom::Start);
                                (void)at;
                                err = serr;
                                for (uint64_t left = pkg.info.fileBytes; err.isOk() && left > 0;) {
                                        const size_t n = left < ChunkBytes ? size_t(left) : ChunkBytes;
                                        err = readNext(*pkg.file, buf.data(), n);
                                        h.update(buf.data(), n);
                                        left -= n;
                                }
                                if (err.isError()) (void)skip.appendf("%s: %s", path.cStr(), err.desc());
                        }
                        if (err.isError()) {
                                note(listener, f.name.view(), skip.view());
                                continue;
                        }
                        uint8_t digest[Blake2s::DigestBytes];
                        h.final(digest);
                        char hash[2 * Blake2s::DigestBytes + 1];
                        hex(digest, sizeof(digest), hash);
                        const Info &i = pkg.info;
                        StringView  line = pkg.description.view().trimmed();
                        line = line.left(line.indexOf('\n')).trimmed();
                        String stanza;
                        err = stanza.appendf("Package: %.*s\nVersion: %.*s\nArchitecture: %.*s\nABI: %u\n",
                                             int(i.name.size()), i.name.data(), int(i.version.size()), i.version.data(),
                                             int(i.arch.size()), i.arch.data(), unsigned(i.abi));
                        if (err.isOk()) err = appendField(stanza, "Depends", i.depends);
                        if (err.isOk()) err = appendField(stanza, "Title", i.title);
                        if (err.isOk()) err = appendField(stanza, "Summary", i.summary);
                        if (err.isOk()) err = appendField(stanza, "Category", i.category);
                        if (err.isOk()) err = appendField(stanza, "License", i.license);
                        if (err.isOk()) err = appendField(stanza, "Homepage", i.homepage);
                        if (err.isOk())
                                err = stanza.appendf("Size: %llu\nInstalled-Size: %llu\nBLAKE2s: %s\nFile: %s\n",
                                                     static_cast<unsigned long long>(i.fileBytes),
                                                     static_cast<unsigned long long>(i.installedBytes), hash,
                                                     f.name.cStr());
                        if (err.isOk()) err = appendField(stanza, "Description", line);
                        if (err.isError()) return fail(why, err, "out of memory");
                        // What the reader refuses (a field the index's rules don't allow) is left out.
                        PkgIndex::Reader check(stanza.view());
                        PkgIndex::Stanza s;
                        if (check.next(s).isError()) {
                                note(listener, i.name, "left out: a field an index can't hold");
                                continue;
                        }
                        err = stanzas.appendf("%s%s", stanzas.isEmpty() ? "" : "\n", stanza.cStr());
                        PkgFile::IconReader r(
                                ByteSpan(reinterpret_cast<const uint8_t *>(pkg.icons.data()), pkg.icons.size()), false);
                        PkgFile::Icon icon;
                        while (err.isOk() && r.next(icon).isOk())
                                err = PkgFile::appendIcon(icons, i.name, icon.side, icon.png);
                        if (err.isError()) return fail(why, err, "out of memory");
                        ++count;
                }
                const Error err = PkgFile::buildIndex(stanzas.view(), icons.span(), count,
                                                      config.now != nullptr ? config.now() : 0, "", out);
                return err.isError() ? fail(why, err, "out of memory") : Error::Ok;
        }

        Error writeBytes(StringView path, const List<uint8_t> &bytes) {
                return writeWhole(path, bytes.span());
        }

        // A network source's index: `URL/index.bmpi`.
        Error indexUrl(const Source &spec, String &out) {
                out.clear();
                Error err = out.append(spec.url);
                if (err.isOk() && !spec.url.endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append("index.bmpi");
                return err;
        }

        // A fetch into a file: written from `written` on, at most `limit` bytes in all.
        struct FileFetch {
                        String          path;
                        FileHandle::Ptr file;
                        uint64_t        written = 0;
                        uint64_t        limit = UINT64_MAX;
                        const Listener *listener = nullptr;
                        StringView      name; ///< For progress.
        };

        Error openFetch(FileFetch &f, bool resume) {
                f.file = FileHandle::Ptr();
                auto [file, err] =
                        Vfs::open(f.path.view(), Vfs::Write | Vfs::Create | (resume ? 0 : Vfs::Truncate), vfs());
                if (err.isError()) return err;
                f.file = std::move(file);
                f.written = 0;
                if (resume) {
                        auto [end, serr] = f.file->stream().seek(0, Stream::SeekFrom::End);
                        if (serr.isError()) return serr;
                        f.written = uint64_t(end);
                }
                return Error::Ok;
        }

        FetchSink sinkOf(FileFetch &f) {
                FetchSink sink;
                sink.begin = [](void *arg, uint64_t start, uint64_t) -> Error {
                        FileFetch &to = *static_cast<FileFetch *>(arg);
                        if (start == to.written) return Error::Ok;
                        if (start != 0) return Error::CorruptData;
                        return openFetch(to, false); // it starts again
                };
                sink.body = [](void *arg, ByteSpan bytes) -> Error {
                        FileFetch &to = *static_cast<FileFetch *>(arg);
                        if (to.written + bytes.size() > to.limit) return Error::OutOfRange;
                        while (!bytes.isEmpty()) {
                                auto [n, err] = to.file->write(bytes, WaitForever);
                                if (err.isError()) return err;
                                if (n == 0) return Error::IoError;
                                bytes = bytes.mid(n);
                                to.written += n;
                        }
                        if (to.listener != nullptr)
                                progress(*to.listener, to.name, Packages::Step::Fetching, to.written, to.limit);
                        return Error::Ok;
                };
                sink.arg = &f;
                return sink;
        }

        // Fetches a network source's index, checks its signature, and keeps it as its list.
        Error updateNetwork(const Source &spec, const Config &config, Packages::SourceResult &result) {
                const StringView source = spec.text;
                if (config.fetch == nullptr)
                        return fail(result.why, Error::NotSupported, "%.*s: no fetching in this system",
                                    int(source.size()), source.data());
                String url;
                auto [bmpi, berr] = listFile(config, source, ".bmpi");
                auto [part, perr] = listFile(config, source, ".part");
                auto [named, nerr] = listFile(config, source, ".source");
                Error err = indexUrl(spec, url);
                if (err.isOk()) err = berr.isError() ? berr : perr.isError() ? perr : nerr;
                if (err.isError()) return fail(result.why, err, "out of memory");
                FileFetch f;
                f.path = std::move(part);
                f.limit = MaxListBytes;
                err = openFetch(f, false);
                if (err.isError()) return fail(result.why, err, "%s: %s", f.path.cStr(), err.desc());
                String why;
                err = config.fetch(url.view(), 0, sinkOf(f), nullptr, why);
                f.file = FileHandle::Ptr();
                List<uint8_t> list(MemSpace::Psram);
                if (err.isOk()) err = readBytes(f.path.view(), MaxListBytes, list);
                if (err.isError()) {
                        (void)removeIfThere(f.path.view());
                        if (!why.isEmpty()) return fail(result.why, err, "%s", why.cStr()); // it names the URL
                        return fail(result.why, err, "%s: %s", url.cStr(),
                                    err == Error::OutOfRange ? "bigger than an index may be" : err.desc());
                }
                PkgFile::Index index;
                StringView     stanzas;
                String         keys;
                for (StringView id : spec.keys())
                        (void)keys.appendf("%s%.*s", keys.isEmpty() ? "" : ", ", int(id.size()), id.data());
                if (readList(list, index, stanzas).isError()) {
                        (void)removeIfThere(f.path.view());
                        return fail(result.why, Error::CorruptData, "%s: not an index", url.cStr());
                }
                // Never unsigned over the network: by a key the source names, else any in the keyring.
                err = SignedContainer::isUnsigned(index) ? Error::CorruptData
                                                         : verifyWith(index, list.span(), config, spec.keys());
                if (err.isError()) {
                        (void)removeIfThere(f.path.view());
                        if (err == Error::NoMem) return fail(result.why, err, "out of memory");
                        return fail(result.why, Error::PermissionDenied, "%s: %s", url.cStr(),
                                    SignedContainer::isUnsigned(index)
                                            ? "unsigned (a network source's index must be signed)"
                                    : keys.isEmpty() ? "not signed by a key in the keyring (pkg key list)"
                                                     : "not signed by the keys the source names");
                }
                result.packages = index.packageCount;
                String text;
                err = text.appendf("source\t%.*s\n", int(source.size()), source.data());
                if (err.isOk()) err = Vfs::rename(f.path.view(), bmpi.view(), Vfs::RenameMode::Replace, vfs());
                if (err.isOk()) err = writeWhole(named.view(), text.view());
                if (err.isError()) return fail(result.why, err, "%s: %s", bmpi.cStr(), err.desc());
                return Error::Ok;
        }

        // Updates one source's list.
        Error updateSource(StringView source, const Config &config, const Listener &listener,
                           Packages::SourceResult &result) {
                Source      spec;
                const Error perr = parseSource(source, spec, result.why);
                if (perr.isError()) return perr;
                if (spec.network) return updateNetwork(spec, config, result);
                const StringView folder = spec.folder;
                List<FolderFile> files(MemSpace::Psram);
                Error            err = folderPackages(folder, files);
                if (err.isError()) {
                        return fail(result.why, err, "%.*s: %s", int(folder.size()), folder.data(),
                                    err == Error::NotExist ? "no such folder" : err.desc());
                }
                List<uint8_t> list(MemSpace::Psram);
                auto [own, jerr] = join(folder, "index.bmpi");
                if (jerr.isOk()) err = readBytes(own.view(), MaxListBytes, list);
                if (jerr.isError()) return fail(result.why, jerr, "out of memory");
                PkgFile::Index index;
                StringView     stanzas;
                const bool usable = err.isOk() && indexMatches(list, files) && readList(list, index, stanzas).isOk();
                if (spec.signedByCount > 0) {
                        // Only an index signed by a key the source names: the board can't sign one.
                        String named;
                        for (StringView id : spec.keys())
                                (void)named.appendf("%s%.*s", named.isEmpty() ? "" : ", ", int(id.size()), id.data());
                        if (!usable) {
                                return fail(result.why, Error::PermissionDenied,
                                            "%.*s: its index.bmpi is missing or out of date, and the source needs one "
                                            "signed by %s (mkpkg index --key signs)",
                                            int(folder.size()), folder.data(), named.cStr());
                        }
                        const Error verr = verifyWith(index, list.span(), config, spec.keys());
                        if (verr == Error::NoMem) return fail(result.why, verr, "out of memory");
                        if (verr.isError()) {
                                return fail(
                                        result.why, Error::PermissionDenied,
                                        "%.*s: its index.bmpi isn't signed by %s (pkg key list: those in the keyring)",
                                        int(folder.size()), folder.data(), named.cStr());
                        }
                }
                if (usable) {
                        result.packages = index.packageCount;
                } else {
                        result.indexedHere = true;
                        err = indexOf(folder, files, config, listener, result.packages, list, result.why);
                        if (err.isError()) return err;
                }
                auto [bmpi, berr] = listFile(config, source, ".bmpi");
                auto [named, nerr] = listFile(config, source, ".source");
                if (berr.isError() || nerr.isError()) return fail(result.why, Error::NoMem, "out of memory");
                String text;
                err = text.appendf("source\t%.*s\n", int(source.size()), source.data());
                if (err.isOk()) err = writeBytes(bmpi.view(), list);
                if (err.isOk()) err = writeWhole(named.view(), text.view());
                if (err.isError()) return fail(result.why, err, "%s: %s", bmpi.cStr(), err.desc());
                return Error::Ok;
        }

        // A list's packages that suit this system, appended to @p out.
        Error readAvailable(StringView source, const Config &config, List<Packages::Available> &out, String &why) {
                auto [path, perr] = listFile(config, source, ".bmpi");
                if (perr.isError()) return fail(why, perr, "out of memory");
                List<uint8_t> bytes(MemSpace::Psram);
                Error         err = readBytes(path.view(), MaxListBytes, bytes);
                if (err == Error::NotExist) return Error::Ok; // not updated yet
                if (err.isError()) return fail(why, err, "%s: %s", path.cStr(), err.desc());
                PkgFile::Index index;
                StringView     stanzas;
                if (readList(bytes, index, stanzas).isError())
                        return fail(why, Error::CorruptData, "%s: damaged: pkg update reads it again", path.cStr());
                Source spec;
                if (parseSource(source, spec, why).isError()) return Error::Invalid;
                const StringView folder = spec.folder;
                String           base; // a network source's index, which its Files are relative to
                if (spec.network && indexUrl(spec, base).isError()) return fail(why, Error::NoMem, "out of memory");
                PkgIndex::Reader r(stanzas);
                PkgIndex::Stanza s;
                while ((err = r.next(s)).isOk()) {
                        if ((s.architecture != config.arch && s.architecture != StringView(PkgFile::ArchAll)) ||
                            s.abi > config.abi)
                                continue;
                        Packages::Available a;
                        Info               &i = a.info;
                        i.name = s.package;
                        i.version = s.version;
                        i.arch = s.architecture;
                        i.abi = s.abi;
                        i.title = s.title;
                        i.summary = s.summary;
                        i.category = s.category;
                        i.license = s.license;
                        i.homepage = s.homepage;
                        i.depends = s.depends;
                        i.installedBytes = s.installedSize;
                        i.fileBytes = s.size;
                        auto [file, ferr] =
                                spec.network ? Http::resolve(base.view(), s.file) : filePath(folder, s.file);
                        if (spec.network && ferr.isError()) continue; // a File that isn't a URL
                        auto [src, serr] = String::fromView(source);
                        auto [desc, derr] = String::fromView(s.description);
                        err = settle(i);
                        if (err.isOk()) err = ferr.isError() ? ferr : serr.isError() ? serr : derr;
                        if (err.isError()) return fail(why, err, "%s: %s", path.cStr(), err.desc());
                        if (file.isEmpty()) continue; // a URL a folder can't reach
                        a.path = std::move(file);
                        a.source = std::move(src);
                        a.description = std::move(desc);
                        a.checkHash = spec.signedByCount > 0 || spec.network;
                        a.remote = spec.network;
                        std::memcpy(a.hash, s.hash, sizeof(a.hash));
                        err = out.append(std::move(a));
                        if (err.isError()) return fail(why, err, "out of memory");
                }
                if (err != Error::EndOfFile)
                        return fail(why, Error::CorruptData, "%s: damaged (line %u): pkg update reads it again",
                                    path.cStr(), unsigned(r.line()));
                return Error::Ok;
        }

        // Looks in @p folder for packages named @p name (`NAME_*.bmpk`), appending each that
        // suits to @p candidates; what didn't, to @p skipped.
        Error findIn(StringView folder, StringView name, const Config &config, List<Packages::Planned> &candidates,
                     String &skipped) {
                auto [dir, err] = Vfs::openDirectory(folder, vfs());
                if (err.isError()) return err == Error::NotExist ? Error::Ok : err;
                for (;;) {
                        auto [entry, nerr] = dir.next();
                        if (nerr == Error::EndOfFile) break;
                        if (nerr.isError()) return nerr;
                        const StringView n = entry.name.view();
                        if (entry.type != FileType::File || n.size() <= name.size() + 1 || !n.startsWith(name) ||
                            n[name.size()] != '_' || !n.endsWith(".bmpk"))
                                continue;
                        auto [path, jerr] = join(folder, n);
                        if (jerr.isError()) return jerr;
                        Packages::Planned found;
                        String            why;
                        const Error       perr = Packages::peekFile(path.view(), config, found.info, why);
                        if (perr.isOk() && found.info.name == name) {
                                found.path = std::move(path);
                                found.automatic = true;
                                err = candidates.append(std::move(found));
                                if (err.isError()) return err;
                        } else if (perr.isError()) {
                                (void)skipped.appendf("%s%.*s: %s", skipped.isEmpty() ? "" : "; ", int(n.size()),
                                                      n.data(), why.cStr());
                        }
                }
                return Error::Ok;
        }

        // A candidate: a file given or found, or a list's package.
        struct Candidate {
                        Packages::Planned planned;
                        bool              fromList = false;
                        bool              checkHash = false; ///< From a source that names keys, or a network one.
                        bool              remote = false;    ///< `planned.path` is a URL.
                        uint8_t           hash[PkgIndex::HashBytes] = {};
        };

        // Whether the whole file at @p path hashes to @p hash.
        Error fileHashMatches(StringView path, const uint8_t (&hash)[PkgIndex::HashBytes], bool &matches) {
                auto [file, err] = Vfs::open(path, Vfs::Read, vfs());
                if (err.isError()) return err;
                Buffer buf(ChunkBytes);
                if (buf.data() == nullptr) return Error::NoMem;
                Blake2s h;
                for (;;) {
                        auto [n, rerr] = file->read(WritableByteSpan(buf.data(), ChunkBytes), WaitForever);
                        if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) break;
                        if (rerr.isError()) return rerr;
                        h.update(buf.data(), n);
                }
                uint8_t digest[Blake2s::DigestBytes];
                h.final(digest);
                matches = std::memcmp(digest, hash, sizeof(digest)) == 0;
                return Error::Ok;
        }

} // namespace Packages::Detail

using namespace Packages::Detail;

Error Packages::checkSources(StringView sources, String &why) {
        why.clear();
        return eachSource(sources, [&](StringView one) -> Error {
                Source      spec;
                const Error err = parseSource(one, spec, why);
                if (err.isError()) return err;
                if (spec.network) return Error::Ok;
                auto [normal, nerr] = Vfs::normalize(spec.folder, "/");
                if (nerr.isError() || !spec.folder.startsWith('/') || normal.view() != spec.folder) {
                        return fail(why, Error::Invalid, "%.*s: the folder must be absolute and normalized",
                                    int(one.size()), one.data());
                }
                return Error::Ok;
        });
}

Error Packages::update(const Config &config, const Listener &listener, List<SourceResult> &out) {
        out.clear();
        Lock lock;
        if (!lock.held()) {
                SourceResult busy;
                busy.error = Error::Busy;
                (void)busy.why.append("another package operation is running");
                (void)out.append(std::move(busy));
                return Error::Busy;
        }
        String why;
        Error  first = checkSources(config.sources, why);
        if (first.isError()) {
                SourceResult bad;
                bad.error = first;
                bad.why = std::move(why);
                (void)out.append(std::move(bad));
                return first;
        }
        auto [dir, derr] = listsDir(config);
        if (derr.isOk()) derr = makeDirectories(dir.view());
        if (derr.isError()) {
                SourceResult bad;
                bad.error = derr;
                (void)bad.why.appendf("%s: %s", dir.cStr(), derr.desc());
                (void)out.append(std::move(bad));
                return derr;
        }
        List<String> keep(MemSpace::Psram); // the lists' file names
        (void)eachSource(config.sources, [&](StringView source) -> Error {
                SourceResult r;
                (void)r.source.append(source);
                r.error = updateSource(source, config, listener, r);
                if (r.error.isError() && first.isOk()) first = r.error;
                char id[17];
                sourceId(source, id);
                for (const char *suffix : {".bmpi", ".source"}) {
                        String name;
                        if (name.appendf("%s%s", id, suffix).isOk()) (void)keep.append(std::move(name));
                }
                return out.append(std::move(r));
        });
        // The lists of sources no longer configured.
        auto [lists, oerr] = Vfs::openDirectory(dir.view(), vfs());
        List<String> stale(MemSpace::Psram);
        for (;;) {
                if (oerr.isError()) break;
                auto [entry, nerr] = lists.next();
                if (nerr.isError()) break;
                bool kept = false;
                for (const String &k : keep) kept = kept || k.view() == entry.name.view();
                if (!kept) (void)stale.append(std::move(entry.name));
        }
        lists.close();
        for (const String &name : stale) {
                auto [path, jerr] = join(dir.view(), name.view());
                if (jerr.isOk()) (void)removeIfThere(path.view());
        }
        return first;
}

Error Packages::indexFolder(StringView dir, const Config &config, const Listener &listener, uint32_t &count,
                            String &why) {
        why.clear();
        count = 0;
        List<FolderFile> files(MemSpace::Psram);
        Error            err = folderPackages(dir, files);
        if (err.isError())
                return fail(why, err, "%.*s: %s", int(dir.size()), dir.data(),
                            err == Error::NotExist ? "no such folder" : err.desc());
        List<uint8_t> index(MemSpace::Psram);
        err = indexOf(dir, files, config, listener, count, index, why);
        if (err.isError()) return err;
        auto [path, jerr] = join(dir, "index.bmpi");
        if (jerr.isOk()) jerr = writeWhole(path.view(), index.span());
        return jerr.isError() ? fail(why, jerr, "%s: %s", path.cStr(), jerr.desc()) : Error::Ok;
}

Error Packages::available(const Config &config, List<Available> &out, String &why) {
        out.clear();
        why.clear();
        return eachSource(config.sources,
                          [&](StringView source) -> Error { return readAvailable(source, config, out, why); });
}

Error Packages::availableIcon(StringView name, uint16_t side, const Config &config, List<uint8_t> &out) {
        Error found = Error::NotFound;
        (void)eachSource(config.sources, [&](StringView source) -> Error {
                auto [path, perr] = listFile(config, source, ".bmpi");
                List<uint8_t>  bytes(MemSpace::Psram);
                PkgFile::Index index;
                StringView     stanzas;
                if (perr.isError() || readBytes(path.view(), MaxListBytes, bytes).isError() ||
                    readList(bytes, index, stanzas).isError())
                        return Error::Ok;
                const PkgFile::Section *icons = index.find(PkgFile::IndexSectionType::Icons);
                if (icons == nullptr) return Error::Ok;
                found = pickIcon(ByteSpan(bytes.data() + icons->offset, icons->size), true, name, side, out);
                return found.isOk() ? Error::EndOfFile : Error::Ok; // the first source that has one
        });
        return found;
}

Error Packages::clean(const Config &config, String &why) {
        why.clear();
        Lock lock;
        if (!lock.held()) return fail(why, Error::Busy, "another package operation is running");
        // The lists, and the packages fetched (and their .parts).
        for (const char *sub : {"lists", "packages"}) {
                auto [dir, err] = join(config.cache, sub);
                if (err.isError()) return fail(why, err, "out of memory");
                auto [listing, oerr] = Vfs::openDirectory(dir.view(), vfs());
                if (oerr == Error::NotExist) continue;
                if (oerr.isError()) return fail(why, oerr, "%s: %s", dir.cStr(), oerr.desc());
                List<String> names(MemSpace::Psram);
                for (;;) {
                        auto [entry, nerr] = listing.next();
                        if (nerr.isError()) break;
                        if (entry.type == FileType::File) (void)names.append(std::move(entry.name));
                }
                listing.close();
                for (const String &name : names) {
                        auto [path, jerr] = join(dir.view(), name.view());
                        if (jerr.isOk()) jerr = removeIfThere(path.view());
                        if (jerr.isError()) return fail(why, jerr, "%s: %s", path.cStr(), jerr.desc());
                }
        }
        return Error::Ok;
}

Error Packages::plan(Span<const Request> requests, PlanMode mode, const Config &config, const Listener &listener,
                     List<Planned> &out, String &why) {
        out.clear();
        why.clear();
        // The files given, each once.
        List<Candidate> candidates(MemSpace::Psram);
        List<String>    folders(MemSpace::Psram);
        for (const Request &r : requests) {
                if (!r.isFile) continue;
                bool again = false;
                for (const Candidate &c : candidates) again = again || c.planned.path.view() == r.text;
                if (again) continue;
                Candidate given;
                Error     err = peekFile(r.text, config, given.planned.info, why);
                if (err.isError()) return err;
                auto [copy, cerr] = String::fromView(r.text);
                given.planned.path = std::move(copy);
                const size_t     slash = r.text.lastIndexOf('/');
                const StringView folder = slash == StringView::NPos ? StringView() : r.text.left(slash);
                bool             known = false;
                for (const String &f : folders) known = known || f.view() == folder;
                if (cerr.isOk() && !known && !folder.isEmpty()) {
                        auto [f, ferr] = String::fromView(folder);
                        cerr = ferr.isOk() ? folders.append(std::move(f)) : ferr;
                }
                if (cerr.isOk()) cerr = candidates.append(std::move(given));
                if (cerr.isError()) return fail(why, cerr, "out of memory");
        }
        const size_t files = candidates.size();

        // Then every list's packages.
        {
                List<Available> listed(MemSpace::Psram);
                Error           err = available(config, listed, why);
                if (err.isError()) return err;
                for (Available &a : listed) {
                        Candidate c;
                        c.planned.info = std::move(a.info);
                        c.planned.path = std::move(a.path);
                        c.fromList = true;
                        c.checkHash = a.checkHash;
                        c.remote = a.remote;
                        std::memcpy(c.hash, a.hash, sizeof(c.hash));
                        err = candidates.append(std::move(c));
                        if (err.isError()) return fail(why, err, "out of memory");
                }
        }
        List<Info>               installedInfos(MemSpace::Psram);
        List<PkgSolver::Package> installed(MemSpace::Psram);
        Error                    err = installedForWalk(config, installedInfos, installed);
        if (err.isError()) return fail(why, err, "the installed packages: %s", err.desc());

        // The newest candidate named @p name, or NPos.
        const auto newest = [&](StringView name) {
                size_t best = List<Candidate>::NPos;
                for (size_t i = 0; i < candidates.size(); ++i) {
                        const Info &c = candidates[i].planned.info;
                        if (c.name != name) continue;
                        if (best == List<Candidate>::NPos ||
                            PkgVersion::compare(c.version, candidates[best].planned.info.version) > 0)
                                best = i;
                }
                return best;
        };
        const auto installedVersion = [&](StringView name) {
                for (const Info &i : installedInfos) {
                        if (i.name == name) return i.version;
                }
                return StringView();
        };

        List<size_t> requested(MemSpace::Psram);
        for (size_t i = 0; err.isOk() && i < files; ++i) err = requested.append(i);
        for (const Request &r : requests) {
                if (r.isFile || err.isError()) continue;
                const size_t best = newest(r.text);
                if (best == List<Candidate>::NPos) {
                        return fail(why, Error::NotFound, "%.*s: no such package in the lists (pkg update reads them)",
                                    int(r.text.size()), r.text.data());
                }
                const Info      &c = candidates[best].planned.info;
                const StringView have = installedVersion(r.text);
                if (!have.isEmpty() && PkgVersion::compare(have, c.version) >= 0) {
                        String text;
                        (void)text.appendf("%.*s is installed, the newest", int(have.size()), have.data());
                        note(listener, c.name, text.view());
                        continue;
                }
                err = requested.append(best);
        }
        if (mode == PlanMode::Upgrade) {
                for (size_t i = 0; err.isOk() && i < installedInfos.size(); ++i) {
                        const size_t best = newest(installedInfos[i].name);
                        if (best != List<Candidate>::NPos &&
                            PkgVersion::compare(candidates[best].planned.info.version, installedInfos[i].version) > 0)
                                err = requested.append(best);
                }
        }
        if (err.isError()) return fail(why, err, "out of memory");
        if (requested.isEmpty()) return Error::Ok;

        // Walk; on a dependency nothing provides, look for it in the files' folders and walk again.
        List<String>          searched(MemSpace::Psram);
        String                skipped;
        List<PkgSolver::Step> steps(MemSpace::Psram);
        for (;;) {
                List<PkgSolver::Package> walked(MemSpace::Psram);
                err = Error::Ok; // the last walk's NotFound is answered
                for (size_t i = 0; err.isOk() && i < candidates.size(); ++i) {
                        const Info &c = candidates[i].planned.info;
                        err = walked.append(PkgSolver::Package{c.name, c.version, c.depends, false});
                }
                if (err.isError()) return fail(why, err, "out of memory");
                StringView wanted;
                err = PkgSolver::plan(installed.span(), walked.span(), requested.span(), steps, why, &wanted);
                if (err != Error::NotFound) break;
                bool tried = false;
                for (const String &n : searched) tried = tried || n.view() == wanted;
                if (tried || folders.isEmpty()) {
                        if (!skipped.isEmpty()) {
                                (void)why.appendf(" (%s)", skipped.cStr());
                        } else if (!folders.isEmpty()) {
                                (void)why.append(" (none in ");
                                for (size_t i = 0; i < folders.size(); ++i)
                                        (void)why.appendf("%s%s", i == 0 ? "" : ", ", folders[i].cStr());
                                (void)why.append(")");
                        }
                        return err;
                }
                auto [name, nerr] = String::fromView(wanted); // before candidates grow
                if (nerr.isOk()) nerr = searched.append(std::move(name));
                if (nerr.isError()) return fail(why, nerr, "out of memory");
                const StringView looking = searched.back().view();
                for (const String &folder : folders) {
                        List<Planned> found(MemSpace::Psram);
                        Error         ferr = findIn(folder.view(), looking, config, found, skipped);
                        for (size_t i = 0; ferr.isOk() && i < found.size(); ++i) {
                                Candidate c;
                                c.planned = std::move(found[i]);
                                ferr = candidates.append(std::move(c));
                        }
                        if (ferr.isError()) return fail(why, ferr, "%s: %s", folder.cStr(), ferr.desc());
                }
        }
        if (err.isError()) return err;

        for (const PkgSolver::Step &step : steps) {
                Candidate &c = candidates[step.candidate];
                c.planned.automatic = step.automatic;
                if (mode == PlanMode::Upgrade) {
                        for (size_t r : requested) c.planned.upgrade = c.planned.upgrade || r == step.candidate;
                }
                if (c.fromList && c.remote) {
                        // Read once fetched (fetchPlanned()): its list's hash says what it must be.
                        c.planned.remote = true;
                        std::memcpy(c.planned.hash, c.hash, sizeof(c.hash));
                } else if (c.fromList) {
                        // The file must be there and be what the list says; and who signed it is said.
                        Info        peeked;
                        String      peekWhy;
                        const Error perr = peekFile(c.planned.path.view(), config, peeked, peekWhy);
                        if (perr == Error::NotExist) {
                                return fail(why, Error::InvalidState, "%s is gone: pkg update reads the lists again",
                                            c.planned.path.cStr());
                        }
                        if (perr.isError()) return fail(why, perr, "%s", peekWhy.cStr());
                        if (peeked.name != c.planned.info.name || peeked.version != c.planned.info.version) {
                                return fail(
                                        why, Error::InvalidState,
                                        "%s holds %.*s %.*s, not what the list says: pkg update reads the lists again",
                                        c.planned.path.cStr(), int(peeked.name.size()), peeked.name.data(),
                                        int(peeked.version.size()), peeked.version.data());
                        }
                        c.planned.info = std::move(peeked);
                        // From a source that names keys: the file its signed index vouches for.
                        bool matches = false;
                        if (c.checkHash) {
                                const Error herr = fileHashMatches(c.planned.path.view(), c.hash, matches);
                                if (herr.isError())
                                        return fail(why, herr, "%s: %s", c.planned.path.cStr(), herr.desc());
                                if (!matches) {
                                        return fail(why, Error::CorruptData,
                                                    "%s doesn't match its signed list's hash: pkg update reads the "
                                                    "lists again",
                                                    c.planned.path.cStr());
                                }
                        }
                }
                err = out.append(std::move(c.planned));
                if (err.isError()) return fail(why, err, "out of memory");
        }
        return Error::Ok;
}

Error Packages::fetchPlanned(Planned &planned, const Config &config, const Listener &listener, String &why) {
        why.clear();
        if (!planned.remote) return Error::Ok;
        const Info &info = planned.info;
        if (config.fetch == nullptr)
                return fail(why, Error::NotSupported, "%.*s: no fetching in this system", int(info.name.size()),
                            info.name.data());
        // CACHE/packages/NAME_VERSION.bmpk (an epoch's ':' is no name FAT holds).
        auto [dir, err] = join(config.cache, "packages");
        String file;
        if (err.isOk()) err = file.appendf("%.*s_", int(info.name.size()), info.name.data());
        for (char c : info.version) {
                if (err.isOk()) err = file.append(c == ':' ? '_' : c);
        }
        if (err.isOk()) err = file.append(".bmpk");
        auto [path, jerr] = join(dir.view(), file.view());
        if (err.isOk()) err = jerr;
        if (err.isOk()) err = makeDirectories(dir.view());
        if (err.isError()) return fail(why, err, "%s: %s", dir.cStr(), err.desc());

        // Fetched before, and still what the list says: kept.
        bool good = false;
        auto [there, serr] = Vfs::lstat(path.view(), vfs());
        if (serr.isOk() && there.size == info.fileBytes) (void)fileHashMatches(path.view(), planned.hash, good);
        if (!good) {
                FileFetch f;
                auto [part, perr] = withSuffix(path.view(), ".part");
                if (perr.isError()) return fail(why, perr, "out of memory");
                f.path = std::move(part);
                f.limit = info.fileBytes;
                f.listener = &listener;
                f.name = info.name;
                err = openFetch(f, true);
                if (err.isOk() && f.written > f.limit) err = openFetch(f, false); // not this file's start
                if (err.isError()) return fail(why, err, "%s: %s", f.path.cStr(), err.desc());
                progress(listener, info.name, Step::Fetching, f.written, f.limit);
                String fetchWhy;
                err = config.fetch(planned.path.view(), f.written, sinkOf(f), nullptr, fetchWhy);
                f.file = FileHandle::Ptr();
                if (err.isError()) // the .part stays: the next try resumes it
                        return fail(why, err, "%.*s: %s", int(info.name.size()), info.name.data(),
                                    fetchWhy.isEmpty() ? err.desc() : fetchWhy.cStr());
                bool matches = false;
                if (f.written == f.limit) err = fileHashMatches(f.path.view(), planned.hash, matches);
                if (err.isError() || !matches) {
                        (void)removeIfThere(f.path.view());
                        return fail(why, Error::CorruptData,
                                    "%s doesn't match its signed list (%llu bytes of %llu): pkg update reads the lists "
                                    "again",
                                    planned.path.cStr(), static_cast<unsigned long long>(f.written),
                                    static_cast<unsigned long long>(f.limit));
                }
                err = Vfs::rename(f.path.view(), path.view(), Vfs::RenameMode::Replace, vfs());
                if (err.isError()) return fail(why, err, "%s: %s", path.cStr(), err.desc());
        }

        // Read as a list's file is: what the list says, and who signed it.
        Info   peeked;
        String peekWhy;
        err = peekFile(path.view(), config, peeked, peekWhy);
        if (err.isError()) return fail(why, err, "%s", peekWhy.cStr());
        if (peeked.name != info.name || peeked.version != info.version)
                return fail(why, Error::CorruptData, "%s isn't what the list says: pkg update reads the lists again",
                            planned.path.cStr());
        planned.info = std::move(peeked);
        planned.path = std::move(path);
        planned.remote = false;
        return Error::Ok;
}

BMDS_NAMESPACE_END
