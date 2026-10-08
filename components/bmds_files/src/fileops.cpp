/**
 * @file      fileops.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a file manager does to files (devplan/filebrowser.md §4f).
 */

#include <cstdio>
#include <bmds/fileops.h>
#include <bmds/filenames.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/trash.h>

BMDS_NAMESPACE_BEGIN

namespace FileOps {

        namespace {

                constexpr size_t BufferBytes = 32 * 1024; // whole sectors go straight to the card

                // A copy's buffer: DMA-capable internal RAM, or PSRAM when there is none.
                struct Buffer {
                                Buffer() {
                                        data = static_cast<uint8_t *>(
                                                Memory::alloc(MemSpace::InternalDma, BufferBytes));
                                        if (data == nullptr)
                                                data = static_cast<uint8_t *>(
                                                        Memory::alloc(MemSpace::Psram, BufferBytes));
                                }
                                ~Buffer() { Memory::free(data); }
                                Buffer(const Buffer &) = delete;
                                Buffer &operator=(const Buffer &) = delete;

                                uint8_t *data = nullptr;
                };

                // What one operation carries through its tree.
                struct Walk {
                                Walk(const Vfs::Context &c, Progress &p, uint8_t *b = nullptr)
                                    : context(c), progress(p), buffer(b) {}

                                const Vfs::Context &context;
                                Progress           &progress;
                                uint8_t            *buffer = nullptr;
                                Error               first; // the first item's error
                                bool                stopped = false;

                                // Item @p path failed: told, kept if first. @return false (for chaining).
                                bool fail(StringView path, Error err) {
                                        if (err == Error::Interrupted) {
                                                stopped = true;
                                                return false;
                                        }
                                        progress.failed(path, err);
                                        if (first.isOk()) first = err;
                                        return false;
                                }

                                bool cancelled() {
                                        if (!stopped && progress.isCancelled()) stopped = true;
                                        return stopped;
                                }

                                Error result() const { return stopped ? Error(Error::Interrupted) : first; }
                };

                Error join(StringView directory, StringView name, String &out) {
                        out.clear();
                        Error err = out.append(directory);
                        if (err.isOk() && !directory.endsWith('/')) err = out.append('/');
                        if (err.isOk()) err = out.append(name);
                        return err;
                }

                StringView parentOf(StringView path) {
                        const size_t slash = path.lastIndexOf('/');
                        if (slash == StringView::NPos) return StringView(".");
                        return slash == 0 ? StringView("/") : path.left(slash);
                }

                // True if @p inner is @p outer or under it (both normalized).
                bool within(StringView outer, StringView inner) {
                        if (inner == outer) return true;
                        return inner.startsWith(outer) && inner.size() > outer.size() &&
                               (outer.endsWith('/') || inner[outer.size()] == '/');
                }

                // The names in @p directory (read first: the directory isn't changed while open).
                Error namesIn(StringView directory, const Vfs::Context &context, List<String> &out) {
                        auto [dir, err] = Vfs::openDirectory(directory, context);
                        if (err.isError()) return err;
                        for (;;) {
                                auto [info, nerr] = dir.next();
                                if (nerr == Error::EndOfFile) return Error::Ok;
                                if (nerr.isError()) return nerr;
                                Error added = out.append(std::move(info.name));
                                if (added.isError()) return added;
                        }
                }

                bool exists(StringView path, const Vfs::Context &context, bool &isDirectory) {
                        auto [info, err] = Vfs::lstat(path, context);
                        isDirectory = err.isOk() && info.isDirectory();
                        return err.isOk();
                }

                // A file's bytes, told as they go; the copy removed if cut off.
                Error copyData(Walk &w, StringView from, StringView to, const FileInfo &info) {
                        Error err;
                        {
                                auto [in, ierr] = Vfs::open(from, Vfs::Read, w.context);
                                if (ierr.isError()) return ierr;
                                auto [out, oerr] = Vfs::open(to, Vfs::Write | Vfs::Create | Vfs::Exclusive, w.context);
                                if (oerr.isError()) return oerr;
                                for (;;) {
                                        if (w.cancelled()) {
                                                err = Error::Interrupted;
                                                break;
                                        }
                                        auto [n, rerr] = in->read(WritableByteSpan(w.buffer, BufferBytes), WaitForever);
                                        if (rerr == Error::EndOfFile) break;
                                        if (rerr.isError()) {
                                                err = rerr;
                                                break;
                                        }
                                        err = out->write(ByteSpan(w.buffer, n), WaitForever).error();
                                        if (err.isError()) break;
                                        w.progress.advanced(n);
                                }
                                if (err.isOk()) err = out->stream().flush(WaitForever);
                        } // both closed
                        if (err.isError()) {
                                (void)Vfs::remove(to, w.context);
                                return err;
                        }
                        if (info.modified.ns != 0) (void)Vfs::setModified(to, info.modified, w.context);
                        return Error::Ok;
                }

                Error removeTree(Walk &w, StringView path, int depth);

                // @p from copied to @p to, which is free (or, with @p merge, a folder to copy into).
                // @return false if it failed or was cut off.
                bool copyTo(Walk &w, StringView from, StringView to, bool merge, int depth);

                // @p from put into folder @p into under @p name, asking if the name is taken
                // (not asked in a folder just made: @p fresh). @param made [out] where it went.
                bool copyInto(Walk &w, StringView from, StringView into, StringView name, bool fresh, String *made,
                              int depth) {
                        if (w.cancelled()) return false;
                        String to;
                        if (join(into, name, to).isError()) return w.fail(from, Error::NoMem);
                        bool merge = false, isDir = false;
                        if (!fresh && exists(to.view(), w.context, isDir)) {
                                // Onto itself: always beside it.
                                const Resolution r = to.view() == from ? Resolution::KeepBoth
                                                                       : w.progress.conflict(from, to.view(), isDir);
                                if (r == Resolution::Cancel) {
                                        w.stopped = true;
                                        return false;
                                }
                                if (r == Resolution::Skip) return true;
                                if (r == Resolution::KeepBoth) {
                                        auto [free, cerr] = copyName(into, name, w.context);
                                        if (cerr.isError() || join(into, free.view(), to).isError())
                                                return w.fail(from, cerr.isError() ? cerr : Error(Error::NoMem));
                                } else {
                                        // Replace: a folder onto a folder is merged; anything else goes first.
                                        auto [info, ierr] = Vfs::lstat(from, w.context);
                                        if (ierr.isError()) return w.fail(from, ierr);
                                        merge = isDir && info.isDirectory();
                                        if (!merge) {
                                                const Error gone = removeTree(w, to.view(), depth);
                                                if (gone.isError()) return w.fail(to.view(), gone);
                                        }
                                }
                        }
                        if (made != nullptr) (void)made->assign(to.view());
                        return copyTo(w, from, to.view(), merge, depth);
                }

                bool copyTo(Walk &w, StringView from, StringView to, bool merge, int depth) {
                        auto [info, err] = Vfs::lstat(from, w.context);
                        if (err.isError()) return w.fail(from, err);
                        w.progress.began(from, info.isDirectory() ? 0 : info.size);
                        if (info.type == FileType::Link) {
                                auto [target, lerr] = Vfs::readLink(from, w.context);
                                err = lerr.isOk() ? Vfs::symlink(target.view(), to, w.context) : lerr;
                                return err.isOk() || w.fail(from, err);
                        }
                        if (!info.isDirectory()) {
                                err = copyData(w, from, to, info);
                                return err.isOk() || w.fail(from, err);
                        }
                        if (depth >= MaxDepth) return w.fail(from, Error::OutOfRange);
                        if (!merge) {
                                err = Vfs::mkdir(to, w.context);
                                if (err.isError()) return w.fail(to, err);
                        }
                        List<String> names(MemSpace::Psram);
                        err = namesIn(from, w.context, names);
                        if (err.isError()) return w.fail(from, err);
                        bool ok = true;
                        for (const String &n : names) {
                                if (w.cancelled()) return false;
                                String child;
                                if (join(from, n.view(), child).isError()) return w.fail(from, Error::NoMem);
                                ok = copyInto(w, child.view(), to, n.view(), !merge, nullptr, depth + 1) && ok;
                        }
                        if (info.modified.ns != 0) (void)Vfs::setModified(to, info.modified, w.context);
                        return ok;
                }

                Error removeTree(Walk &w, StringView path, int depth) {
                        auto [info, err] = Vfs::lstat(path, w.context);
                        if (err.isError()) return err;
                        if (!info.isDirectory()) return Vfs::remove(path, w.context);
                        if (depth >= MaxDepth) return Error::OutOfRange;
                        List<String> names(MemSpace::Psram);
                        err = namesIn(path, w.context, names);
                        if (err.isError()) return err;
                        for (const String &n : names) {
                                if (w.cancelled()) return Error::Interrupted;
                                String child;
                                if (join(path, n.view(), child).isError()) return Error::NoMem;
                                w.progress.began(child.view(), 0);
                                err = removeTree(w, child.view(), depth + 1);
                                if (err.isError()) return err;
                        }
                        return Vfs::rmdir(path, w.context);
                }

                bool measureTree(Walk &w, StringView path, Totals &totals, int depth) {
                        if (w.cancelled()) return false;
                        auto [info, err] = Vfs::lstat(path, w.context);
                        if (err.isError()) return w.fail(path, err);
                        ++totals.items;
                        if (!info.isDirectory()) {
                                if (info.type == FileType::File) totals.bytes += info.size;
                                return true;
                        }
                        if (depth >= MaxDepth) return w.fail(path, Error::OutOfRange);
                        List<String> names(MemSpace::Psram);
                        err = namesIn(path, w.context, names);
                        if (err.isError()) return w.fail(path, err);
                        bool ok = true;
                        for (const String &n : names) {
                                String child;
                                if (join(path, n.view(), child).isError()) return w.fail(path, Error::NoMem);
                                ok = measureTree(w, child.view(), totals, depth + 1) && ok;
                        }
                        return ok;
                }

                // Normalized @p from and @p into, and @p from's name: what a copy or move starts from.
                Error prepare(StringView from, StringView into, const Vfs::Context &context, String &source,
                              String &target) {
                        auto [s, serr] = Vfs::normalize(from, context.cwd);
                        if (serr.isError()) return serr;
                        auto [t, terr] = Vfs::normalize(into, context.cwd);
                        if (terr.isError()) return terr;
                        source = std::move(s);
                        target = std::move(t);
                        if (source.view() == "/" || within(source.view(), target.view())) return Error::Invalid;
                        auto [info, ierr] = Vfs::stat(target.view(), context);
                        if (ierr.isError()) return ierr;
                        return info.isDirectory() ? Error::Ok : Error::NotDir;
                }

        } // namespace

        StringView baseName(StringView path) {
                while (path.size() > 1 && path.endsWith('/')) path = path.left(path.size() - 1);
                const size_t slash = path.lastIndexOf('/');
                return slash == StringView::NPos ? path : path.mid(slash + 1);
        }

        Result<Totals> measure(Span<const StringView> paths, const Vfs::Context &context, Progress &progress) {
                Walk   w{context, progress};
                Totals totals;
                for (const StringView p : paths)
                        if (!measureTree(w, p, totals, 0) && w.stopped) break;
                return Result<Totals>(totals, w.result());
        }

        Error copy(StringView from, StringView into, const Vfs::Context &context, Progress &progress, String *made) {
                if (made != nullptr) made->clear();
                String      source, target;
                const Error err = prepare(from, into, context, source, target);
                if (err.isError()) return err;
                Buffer buffer;
                if (buffer.data == nullptr) return Error::NoMem;
                Walk w{context, progress, buffer.data};
                (void)copyInto(w, source.view(), target.view(), baseName(source.view()), false, made, 0);
                return w.result();
        }

        Error move(StringView from, StringView into, const Vfs::Context &context, Progress &progress, String *made) {
                if (made != nullptr) made->clear();
                String source, target;
                Error  err = prepare(from, into, context, source, target);
                if (err.isError()) return err;
                const StringView name = baseName(source.view());
                String           to;
                if (join(target.view(), name, to).isError()) return Error::NoMem;
                if (to.view() == source.view()) return Error::Ok; // there already
                Walk w{context, progress};
                bool isDir = false;
                if (exists(to.view(), context, isDir)) {
                        const Resolution r = progress.conflict(source.view(), to.view(), isDir);
                        if (r == Resolution::Cancel) return Error::Interrupted;
                        if (r == Resolution::Skip) return Error::Ok;
                        auto [info, ierr] = Vfs::lstat(source.view(), context);
                        if (ierr.isError()) return ierr;
                        if (r == Resolution::KeepBoth) {
                                auto [free, cerr] = copyName(target.view(), name, context);
                                if (cerr.isError()) return cerr;
                                if (join(target.view(), free.view(), to).isError()) return Error::NoMem;
                        } else if (isDir && info.isDirectory()) {
                                // Folders merged: each item moved in (asking again), then the emptied source.
                                List<String> names(MemSpace::Psram);
                                err = namesIn(source.view(), context, names);
                                if (err.isError()) return err;
                                for (const String &n : names) {
                                        if (w.cancelled()) return Error::Interrupted;
                                        String child;
                                        if (join(source.view(), n.view(), child).isError()) return Error::NoMem;
                                        err = move(child.view(), to.view(), context, progress, nullptr);
                                        if (err == Error::Interrupted) return err;
                                        if (err.isError()) (void)w.fail(child.view(), err);
                                }
                                if (w.first.isOk()) {
                                        const Error gone = Vfs::rmdir(source.view(), context);
                                        if (gone.isError() && gone != Error::NotEmpty)
                                                (void)w.fail(source.view(), gone);
                                }
                                if (made != nullptr) (void)made->assign(to.view());
                                return w.result();
                        } else {
                                err = removeTree(w, to.view(), 0);
                                if (err.isError()) return err;
                        }
                }
                progress.began(source.view(), 0);
                err = Vfs::rename(source.view(), to.view(), context);
                if (err.isOk()) {
                        if (made != nullptr) (void)made->assign(to.view());
                        return Error::Ok;
                }
                if (err != Error::NotSupported) return err;
                // Across filesystems: copied, then the source removed only if all of it arrived.
                Buffer buffer;
                if (buffer.data == nullptr) return Error::NoMem;
                w.buffer = buffer.data;
                if (made != nullptr) (void)made->assign(to.view());
                if (!copyTo(w, source.view(), to.view(), false, 0) || w.result().isError()) return w.result();
                err = removeTree(w, source.view(), 0);
                if (err.isError()) (void)w.fail(source.view(), err);
                return w.result();
        }

        Error remove(StringView path, const Vfs::Context &context, Progress &progress) {
                Walk w{context, progress};
                progress.began(path, 0);
                const Error err = removeTree(w, path, 0);
                if (err == Error::Interrupted || w.stopped) return Error::Interrupted;
                return err;
        }

        Error trash(StringView path, const Vfs::Context &context) {
                return Trash::move(path, context, nullptr);
        }

        Error rename(StringView path, StringView name, const Vfs::Context &context, String *made) {
                if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.size() > 255)
                        return Error::Invalid;
                auto [source, serr] = Vfs::normalize(path, context.cwd);
                if (serr.isError()) return serr;
                String to;
                if (join(parentOf(source.view()), name, to).isError()) return Error::NoMem;
                if (made != nullptr) (void)made->assign(to.view());
                if (to.view() == source.view()) return Error::Ok;
                Error err = Vfs::rename(source.view(), to.view(), context);
                if (err != Error::Exists) return err;
                // Only its case changed (FAT finds the old name under the new): by a free name between.
                auto [a, aerr] = Vfs::lstat(source.view(), context);
                auto [b, berr] = Vfs::lstat(to.view(), context);
                if (aerr.isError() || berr.isError() || a.id != b.id) return Error::Exists;
                String between;
                if (join(parentOf(source.view()), ".rename-tmp", between).isError()) return Error::NoMem;
                err = Vfs::rename(source.view(), between.view(), context);
                if (err.isOk()) err = Vfs::rename(between.view(), to.view(), context);
                return err;
        }

        Result<String> makeNew(StringView directory, NewKind kind, const Vfs::Context &context) {
                const StringView base = kind == NewKind::Folder         ? "New Folder"
                                        : kind == NewKind::TextDocument ? "New Text Document.txt"
                                                                        : "New File";
                const size_t     dot = base.lastIndexOf('.');
                const StringView stem = dot != StringView::NPos ? base.left(dot) : base;
                const StringView extension = dot != StringView::NPos ? base.mid(dot) : StringView();
                String           path;
                Error            err = Error::Exists;
                for (unsigned n = 1; n < 100 && err == Error::Exists; ++n) {
                        char name[64];
                        if (n == 1) {
                                std::snprintf(name, sizeof(name), "%.*s", int(base.size()), base.data());
                        } else {
                                std::snprintf(name, sizeof(name), "%.*s (%u)%.*s", int(stem.size()), stem.data(), n,
                                              int(extension.size()), extension.data());
                        }
                        err = join(directory, StringView(name), path);
                        if (err.isError()) break;
                        if (kind == NewKind::Folder) {
                                bool isDir = false;
                                err = exists(path.view(), context, isDir) ? Error(Error::Exists)
                                                                          : Vfs::mkdir(path.view(), context);
                        } else {
                                err = Vfs::open(path.view(), Vfs::Write | Vfs::Create | Vfs::Exclusive, context)
                                              .error();
                        }
                }
                if (err == Error::Exists) err = Error::Full;
                return Result<String>(std::move(path), err);
        }

        Result<String> copyName(StringView directory, StringView name, const Vfs::Context &context) {
                String path, chosen;
                for (unsigned n = 0; n <= CopyName::MaxCopies; ++n) {
                        char       buf[Vfs::MaxPath + 1];
                        StringView candidate = name;
                        if (n > 0) {
                                auto [made, merr] = CopyName::make(name, n, Span<char>(buf, sizeof(buf)));
                                if (merr.isError()) return makeError<String>(merr);
                                candidate = made;
                        }
                        if (join(directory, candidate, path).isError()) return makeError<String>(Error::NoMem);
                        bool isDir = false;
                        if (exists(path.view(), context, isDir)) continue;
                        if (chosen.assign(candidate).isError()) return makeError<String>(Error::NoMem);
                        return makeResult(std::move(chosen));
                }
                return makeError<String>(Error::Full);
        }

} // namespace FileOps

BMDS_NAMESPACE_END
