/**
 * @file      trash.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The trash (devplan/files.md F2, F3): freedesktop.org's layout under each mount's .trash.
 */

#include <cstring>
#include <initializer_list>
#include <bmds/clock.h>
#include <bmds/trash.h>

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

        Error makeDirectory(StringView path) {
                const Error err = Vfs::mkdir(path, Root);
                return err == Error::Exists ? Error(Error::Ok) : err;
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

        // The trash's directories of @p mount: .trash, .trash/files, .trash/info.
        Error trashPaths(StringView mount, String &files, String &info) {
                String trash;
                Error  err = join(mount, Trash::Directory, trash);
                if (err.isOk()) err = join(trash.view(), "files", files);
                if (err.isOk()) err = join(trash.view(), "info", info);
                return err;
        }

} // namespace

Error Trash::mountOf(StringView path, String &mount) {
        Vfs::MountInfo mounts[8];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts, 8));
        size_t         best = 0;
        bool           found = false;
        for (size_t i = 0; i < n && i < 8; ++i) {
                const StringView m(mounts[i].path);
                const bool under = path == m || (path.startsWith(m) && path.size() > m.size() && path[m.size()] == '/');
                if (under && (!found || m.size() > best)) {
                        best = m.size();
                        found = true;
                        (void)mount.assign(m);
                }
        }
        return found ? Error(Error::Ok) : Error(Error::NotSupported);
}

Error Trash::move(StringView path, const Vfs::Context &context, String *name) {
        auto [normalized, nerr] = Vfs::normalize(path, context.cwd);
        if (nerr.isError()) return nerr;
        // Its folder's physical path (through links, as /home is), its own name kept: a link is
        // trashed itself, and the path recorded is where it really was.
        const StringView logical = normalized.view();
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
        String trash;
        err = join(mount.view(), Directory, trash);
        if (err.isError()) return err;
        // Not a mount's root, nor the trash or anything in it.
        const StringView t = trash.view();
        if (p == mount.view() || p == t || (p.size() > t.size() && p.startsWith(t) && p[t.size()] == '/'))
                return Error::Invalid;
        if (!exists(p)) return Error::NotExist;
        String files, info;
        err = trashPaths(mount.view(), files, info);
        if (err.isOk()) err = makeDirectory(t);
        if (err.isOk()) err = makeDirectory(files.view());
        if (err.isOk()) err = makeDirectory(info.view());
        if (err.isError()) return err;
        // A free name: its own, then " (2)", " (3)"...
        const StringView base = p.mid(p.lastIndexOf('/') + 1);
        String           chosen, target, infoPath;
        bool             free = false;
        for (unsigned k = 1; k <= CopyName::MaxCopies && !free; ++k) {
                chosen.clear();
                err = k == 1 ? chosen.assign(base) : chosen.appendf("%.*s (%u)", int(base.size()), base.data(), k);
                if (err.isOk()) err = join(files.view(), chosen.view(), target);
                if (err.isOk()) err = join(info.view(), chosen.view(), infoPath);
                if (err.isOk()) err = infoPath.append(InfoSuffix);
                if (err.isError()) return err;
                free = !exists(target.view()) && !exists(infoPath.view());
        }
        if (!free) return Error::Full;
        // The info first: a cut-off delete leaves an info without its file (skipped), never a
        // file that can't be put back.
        char       text[MaxInfoBytes];
        const auto local = Clock::localTime(Clock::now());
        auto [body, ferr] = TrashInfo::format(p, local.local, Span<char>(text, sizeof(text)));
        if (ferr.isError()) return ferr;
        err = writeNew(infoPath.view(), body);
        if (err.isError()) return err;
        err = Vfs::rename(p, target.view(), Vfs::RenameMode::NoReplace, Root);
        if (err.isError()) {
                (void)Vfs::remove(infoPath.view(), Root);
                return err;
        }
        if (name != nullptr) *name = std::move(chosen);
        return Error::Ok;
}

Error Trash::list(Listed fn, void *arg) {
        Vfs::MountInfo mounts[8];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts, 8));
        for (size_t i = 0; i < n && i < 8; ++i) {
                String files, info;
                Error  err = trashPaths(StringView(mounts[i].path), files, info);
                if (err.isError()) return err;
                auto [dir, derr] = Vfs::openDirectory(info.view(), Root);
                if (derr == Error::NotExist) continue;
                if (derr.isError()) return derr;
                for (;;) {
                        auto [entry, eerr] = dir.next();
                        if (eerr == Error::EndOfFile) break;
                        if (eerr.isError()) return eerr;
                        const StringView infoName = entry.name.view();
                        if (!infoName.endsWith(InfoSuffix) || infoName.size() == InfoSuffix.size()) continue;
                        const StringView name = infoName.left(infoName.size() - InfoSuffix.size());
                        String           infoPath, filePath, text;
                        if (join(info.view(), infoName, infoPath).isError() ||
                            join(files.view(), name, filePath).isError())
                                return Error::NoMem;
                        auto [what, serr] = Vfs::lstat(filePath.view(), Root);
                        if (serr.isError() || readSmall(infoPath.view(), text).isError()) continue;
                        char path[Vfs::MaxPath + 1];
                        char date[TrashInfo::DateBytes + 1];
                        if (TrashInfo::parse(text.view(), Span<char>(path, sizeof(path)), date).isError()) continue;
                        Entry e;
                        e.mount = StringView(mounts[i].path);
                        e.name = name;
                        e.path = StringView(path);
                        e.date = StringView(date);
                        e.isDirectory = what.isDirectory();
                        fn(arg, e);
                }
        }
        return Error::Ok;
}

Error Trash::restore(StringView mount, StringView name, const Vfs::Context &context) {
        (void)context;
        if (name.isEmpty() || name.contains('/')) return Error::NotExist;
        String files, info, filePath, infoPath, text;
        Error  err = trashPaths(mount, files, info);
        if (err.isOk()) err = join(files.view(), name, filePath);
        if (err.isOk()) err = join(info.view(), name, infoPath);
        if (err.isOk()) err = infoPath.append(InfoSuffix);
        if (err.isError()) return err;
        if (!exists(filePath.view())) return Error::NotExist;
        err = readSmall(infoPath.view(), text);
        if (err.isError()) return err == Error::NotExist ? Error(Error::NotExist) : err;
        char path[Vfs::MaxPath + 1];
        char date[TrashInfo::DateBytes + 1];
        err = TrashInfo::parse(text.view(), Span<char>(path, sizeof(path)), date);
        if (err.isError()) return err;
        if (exists(StringView(path))) return Error::Exists;
        err = Vfs::rename(filePath.view(), StringView(path), Vfs::RenameMode::NoReplace, Root);
        if (err.isError()) return err;
        (void)Vfs::remove(infoPath.view(), Root);
        return Error::Ok;
}

Error Trash::empty(StringView mount, size_t *removed) {
        if (removed != nullptr) *removed = 0;
        Vfs::MountInfo mounts[8];
        const size_t   n = Vfs::listMounts(Span<Vfs::MountInfo>(mounts, 8));
        bool           matched = mount.isEmpty();
        Error          first;
        for (size_t i = 0; i < n && i < 8; ++i) {
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
