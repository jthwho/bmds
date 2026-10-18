/**
 * @file      trash.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The trash (devplan/files.md F2, F3; devplan/filebrowser.md §4k): freedesktop.org's layout
 * under each mount's .bmds/trash.
 */

#include <cstring>
#include <initializer_list>
#include <bmds/clock.h>
#include <bmds/trash.h>

#include "fslayer.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr Vfs::Context Root{"/", nullptr};
        constexpr StringView   InfoSuffix(".trashinfo");
        constexpr size_t       MaxInfoBytes = 1024;
        constexpr int          MaxDepth = 32;

        Error join(StringView a, StringView b, String &out) {
                out.clear();
                Error err = out.append(a);
                if (err.isOk() && !a.endsWith('/')) err = out.append('/');
                if (err.isOk()) err = out.append(b);
                return err;
        }

        bool exists(StringView path) {
                return Vfs::lstat(path, Root).isOk();
        }

        bool isAtOrBelow(StringView path, StringView dir) {
                return path == dir || (path.size() > dir.size() && path.startsWith(dir) && path[dir.size()] == '/');
        }

        Error makeDirectory(StringView path) {
                const Error err = Vfs::mkdir(path, Root);
                return err == Error::Exists ? Error(Error::Ok) : err;
        }

        // @p path's folders made, those that are missing (not @p path itself).
        Error makeParents(StringView path) {
                for (size_t at = path.indexOf('/', 1); at != StringView::NPos; at = path.indexOf('/', at + 1)) {
                        const StringView folder = path.left(at);
                        if (exists(folder)) continue;
                        const Error err = Vfs::mkdir(folder, Root);
                        if (err.isError() && err != Error::Exists) return err;
                }
                return Error::Ok;
        }

        Error readSmall(StringView path, String &out) {
                auto [file, err] = Vfs::open(path, Vfs::Read, Root);
                if (err.isError()) return err;
                char buf[256];
                for (;;) {
                        auto [n, rerr] = file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                    WaitForever);
                        if (rerr == Error::EndOfFile) return Error::Ok;
                        if (rerr.isError()) return rerr;
                        if (out.size() + n > MaxInfoBytes) return Error::CorruptData;
                        const Error aerr = out.append(StringView(buf, n));
                        if (aerr.isError()) return aerr;
                }
        }

        Error writeNew(StringView path, StringView text) {
                auto [file, err] = Vfs::open(path, Vfs::Write | Vfs::Create | Vfs::Exclusive, Root);
                if (err.isError()) return err;
                return file->write(text, WaitForever).error();
        }

        // The first name in directory @p path but . and .. (empty: none).
        Error firstName(StringView path, String &out) {
                out.clear();
                auto [dir, err] = Vfs::openDirectory(path, Root);
                if (err.isError()) return err;
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr == Error::EndOfFile) return Error::Ok;
                        if (nerr.isError()) return nerr;
                        if (info.name.view() == "." || info.name.view() == "..") continue;
                        return out.assign(info.name.view());
                }
        }

        // @p path removed, a directory with all it holds: a name at a time, the directory closed
        // between (never changed while it is open).
        Error removeTree(StringView path, int depth) {
                auto [info, err] = Vfs::lstat(path, Root);
                if (err.isError()) return err;
                if (!info.isDirectory()) return Vfs::remove(path, Root);
                if (depth > MaxDepth) return Error::OutOfRange;
                String name, child;
                for (;;) {
                        err = firstName(path, name);
                        if (err.isError()) return err;
                        if (name.isEmpty()) break;
                        err = join(path, name.view(), child);
                        if (err.isOk()) err = removeTree(child.view(), depth + 1);
                        if (err.isError()) return err;
                }
                return Vfs::rmdir(path, Root);
        }

        // The trash's directories of @p mount: .bmds/trash/files and .bmds/trash/info.
        Error trashPaths(StringView mount, String &files, String &info) {
                String trash;
                Error  err = join(mount, Trash::Directory, trash);
                if (err.isOk()) err = join(trash.view(), "files", files);
                if (err.isOk()) err = join(trash.view(), "info", info);
                return err;
        }

        // The info file's path of @p real in @p mount's trash.
        Error infoPathOf(StringView mount, StringView real, String &out) {
                String files, info;
                Error  err = trashPaths(mount, files, info);
                if (err.isOk()) err = join(info.view(), real, out);
                if (err.isOk()) err = out.append(InfoSuffix);
                return err;
        }

        // Fills @p e from @p real's info in @p mount's trash (its path made absolute).
        Error describe(StringView mount, StringView real, Trash::Entry &e) {
                String files, info, filePath, infoPath, text;
                Error  err = trashPaths(mount, files, info);
                if (err.isOk()) err = join(files.view(), real, filePath);
                if (err.isOk()) err = infoPathOf(mount, real, infoPath);
                if (err.isError()) return err;
                auto [what, serr] = Vfs::lstat(filePath.view(), Root);
                if (serr.isError()) return serr;
                err = readSmall(infoPath.view(), text);
                if (err.isError()) return err;
                char path[Vfs::MaxPath + 1];
                char date[TrashInfo::DateBytes + 1];
                err = TrashInfo::parse(text.view(), Span<char>(path, sizeof(path)), date);
                if (err.isError()) return err;
                // Relative to the filesystem's root (FB19), or absolute as before.
                const StringView p(path);
                err = p.startsWith('/') ? e.path.assign(p) : join(mount, p, e.path);
                if (err.isOk()) err = e.mount.assign(mount);
                if (err.isOk()) err = e.real.assign(real);
                if (err.isOk()) err = e.date.assign(StringView(date));
                e.isDirectory = what.isDirectory();
                return err;
        }

} // namespace

Error Trash::mountOf(StringView path, String &mount) {
        Vfs::MountInfo mounts[Vfs::MaxMounts];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts, Vfs::MaxMounts));
        size_t         best = 0;
        bool           found = false;
        for (size_t i = 0; i < n && i < Vfs::MaxMounts; ++i) {
                const StringView m(mounts[i].path);
                if (isAtOrBelow(path, m) && (!found || m.size() > best)) {
                        best = m.size();
                        found = true;
                        (void)mount.assign(m);
                }
        }
        return found ? Error(Error::Ok) : Error(Error::NotSupported);
}

Error Trash::move(StringView path, const Vfs::Context &context, String *name) {
        return moveAs(path, StringView(), context, name);
}

Error Trash::moveAs(StringView path, StringView as, const Vfs::Context &context, String *name) {
        auto [normalized, nerr] = Vfs::normalize(path, context.cwd);
        if (nerr.isError()) return nerr;
        // Its folder's physical path (through links, as /home is), its own name kept: a link is
        // trashed itself, and the path recorded is where it really was.
        const StringView logical = normalized.view();
        if (isAtOrBelow(logical, Vfs::TrashPath)) return Error::Invalid; // in the trash already
        const size_t     slash = logical.lastIndexOf('/');
        String           physical;
        if (slash != StringView::NPos && slash > 0) {
                auto [parent, rerr] = Vfs::realPath(logical.left(slash), context);
                if (rerr.isError()) return rerr;
                physical = std::move(parent);
                nerr = physical.append(logical.mid(slash));
                if (nerr.isError()) return nerr;
        } else {
                nerr = physical.assign(logical);
                if (nerr.isError()) return nerr;
        }
        const StringView p = physical.view();
        String           mount;
        Error            err = mountOf(p, mount);
        if (err.isError()) return err;
        String meta;
        err = join(mount.view(), Vfs::MetaDirectory, meta);
        if (err.isError()) return err;
        // Not a mount's root, nor .bmds (the trash) or anything in it.
        if (p == mount.view() || isAtOrBelow(p, meta.view())) return Error::Invalid;
        if (!exists(p)) return Error::NotExist;
        String trash, files, info;
        err = join(mount.view(), Directory, trash);
        if (err.isOk()) err = trashPaths(mount.view(), files, info);
        if (err.isOk()) err = makeDirectory(meta.view());
        if (err.isOk()) err = makeDirectory(trash.view());
        if (err.isOk()) err = makeDirectory(files.view());
        if (err.isOk()) err = makeDirectory(info.view());
        if (err.isError()) return err;
        // A free name: the one asked for (else its own), then " (2)", " (3)"... free in this trash
        // and in /trash (so another trash's entry keeps its name there).
        const StringView base =
                !as.isEmpty() && as.indexOf('/') == StringView::NPos ? as : p.mid(p.lastIndexOf('/') + 1);
        String           chosen, target, infoPath, shown;
        bool             free = false;
        for (unsigned k = 1; k <= CopyName::MaxCopies && !free; ++k) {
                chosen.clear();
                err = k == 1 ? chosen.assign(base) : chosen.appendf("%.*s (%u)", int(base.size()), base.data(), k);
                if (err.isOk()) err = join(files.view(), chosen.view(), target);
                if (err.isOk()) err = infoPathOf(mount.view(), chosen.view(), infoPath);
                if (err.isOk()) err = join(Vfs::TrashPath, chosen.view(), shown);
                if (err.isError()) return err;
                free = !exists(target.view()) && !exists(infoPath.view()) && !exists(shown.view());
        }
        if (!free) return Error::Full;
        // The info first: a cut-off delete leaves an info without its file (skipped), never a
        // file that can't be put back. Its path relative to the filesystem's root (FB19).
        char       text[MaxInfoBytes];
        const auto local = Clock::localTime(Clock::now());
        auto [body, ferr] = TrashInfo::format(p.mid(mount.size() + 1), local.local, Span<char>(text, sizeof(text)));
        if (ferr.isError()) return ferr;
        err = writeNew(infoPath.view(), body);
        if (err.isError()) return err;
        err = Fs::renameRaw(p, target.view(), Vfs::RenameMode::NoReplace, Root);
        if (err.isError()) {
                (void)Vfs::remove(infoPath.view(), Root);
                return err;
        }
        if (name != nullptr) *name = std::move(chosen);
        return Error::Ok;
}

Error Trash::list(Listed fn, void *arg) {
        auto [dir, err] = Vfs::openDirectory(Vfs::TrashPath, Root);
        if (err.isError()) return err;
        Entry  e;
        String mount, real;
        for (;;) {
                auto [info, nerr] = dir.next();
                if (nerr == Error::EndOfFile) return Error::Ok;
                if (nerr.isError()) return nerr;
                if (Fs::locateTrashEntry(info.name.view(), mount, real).isError()) continue;
                if (describe(mount.view(), real.view(), e).isError()) continue;
                if (e.name.assign(info.name.view()).isError()) return Error::NoMem;
                fn(arg, e);
        }
}

Error Trash::find(StringView name, Entry &entry) {
        String      mount, real;
        const Error err = Fs::locateTrashEntry(name, mount, real);
        if (err.isError()) return err;
        const Error described = describe(mount.view(), real.view(), entry);
        if (described.isError()) return described;
        return entry.name.assign(name);
}

Error Trash::restore(StringView name, Conflict conflict, String *restoredTo) {
        Entry e;
        Error err = find(name, e);
        if (err.isError()) return err;
        String files, info, filePath, infoPath;
        err = trashPaths(e.mount.view(), files, info);
        if (err.isOk()) err = join(files.view(), e.real.view(), filePath);
        if (err.isOk()) err = infoPathOf(e.mount.view(), e.real.view(), infoPath);
        if (err.isError()) return err;
        // Its place: as it was, or a copy's name beside it.
        String place;
        err = place.assign(e.path.view());
        if (err.isError()) return err;
        if (exists(place.view())) {
                if (conflict == Conflict::Refuse) return Error::Exists;
                if (conflict == Conflict::KeepBoth) {
                        const StringView original = e.path.view();
                        const size_t     cut = original.lastIndexOf('/');
                        bool             free = false;
                        for (unsigned n = 1; n <= CopyName::MaxCopies && !free; ++n) {
                                char copy[256];
                                auto [made, merr] =
                                        CopyName::make(original.mid(cut + 1), n, Span<char>(copy, sizeof(copy)));
                                if (merr.isError()) return merr;
                                err = join(cut == 0 ? StringView("/") : original.left(cut), made, place);
                                if (err.isError()) return err;
                                free = !exists(place.view());
                        }
                        if (!free) return Error::Full;
                }
        }
        err = makeParents(place.view());
        if (err.isError()) return err;
        const Vfs::RenameMode mode =
                conflict == Conflict::Replace ? Vfs::RenameMode::Replace : Vfs::RenameMode::NoReplace;
        err = Fs::renameRaw(filePath.view(), place.view(), mode, Root);
        if (err.isError()) return err;
        (void)Vfs::remove(infoPath.view(), Root);
        if (restoredTo != nullptr) *restoredTo = std::move(place);
        return Error::Ok;
}

Error Trash::empty(StringView mount, size_t *removed) {
        if (removed != nullptr) *removed = 0;
        Vfs::MountInfo mounts[Vfs::MaxMounts];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts, Vfs::MaxMounts));
        bool           matched = mount.isEmpty();
        Error          first;
        for (size_t i = 0; i < n && i < Vfs::MaxMounts; ++i) {
                const StringView m(mounts[i].path);
                if (!mount.isEmpty() && m != mount) continue;
                matched = true;
                String files, info, name, child;
                Error  err = trashPaths(m, files, info);
                if (err.isError()) return err;
                // The files, then the info files (an info without its file is dropped anyway).
                for (const StringView dir : {files.view(), info.view()}) {
                        for (;;) {
                                err = firstName(dir, name);
                                if (err == Error::NotExist || name.isEmpty()) break;
                                if (err.isOk()) err = join(dir, name.view(), child);
                                if (err.isOk()) err = removeTree(child.view(), 0);
                                if (err.isError()) {
                                        if (first.isOk()) first = err;
                                        break;
                                }
                                if (removed != nullptr && dir == files.view()) ++*removed;
                        }
                }
        }
        if (!matched) return Error::NotExist;
        return first;
}

BMDS_NAMESPACE_END
