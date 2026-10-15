/**
 * @file      apps.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The installed applications (devplan/apps.md §4, §11): NAME.app folders found by scanning the
 * roots, kept current by watches, published as /apps's links.
 */

#include <cstring>
#include <utility>
#include <bmds/apps.h>
#include <bmds/memory.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        const Vfs::Context Root{"/", nullptr};

        // FNV-1a, to see whether a scan changed anything.
        uint64_t hashAppend(uint64_t hash, StringView text) {
                for (const char c : text) hash = (hash ^ uint8_t(c)) * 1099511628211ull;
                return (hash ^ 0xffu) * 1099511628211ull; // a separator
        }

        bool lessIgnoringCase(StringView a, StringView b) {
                const size_t n = a.size() < b.size() ? a.size() : b.size();
                for (size_t i = 0; i < n; ++i) {
                        char x = a[i], y = b[i];
                        if (x >= 'A' && x <= 'Z') x = char(x + ('a' - 'A'));
                        if (y >= 'A' && y <= 'Z') y = char(y + ('a' - 'A'));
                        if (x != y) return uint8_t(x) < uint8_t(y);
                }
                return a.size() < b.size();
        }

        // A program's name in its folder: a relative path that stays in it.
        bool isInsideName(StringView path) {
                if (path.isEmpty() || path[0] == '/') return false;
                size_t start = 0;
                while (start <= path.size()) {
                        size_t end = path.indexOf('/', start);
                        if (end == StringView::NPos) end = path.size();
                        const StringView part = path.mid(start, end - start);
                        if (part.isEmpty() || part == "." || part == "..") return false;
                        start = end + 1;
                }
                return true;
        }

        // @p path's file, up to @p max bytes, into @p out. BufferTooSmall: longer.
        Error readSmallFile(StringView path, size_t max, String &out) {
                auto [file, err] = Vfs::open(path, Vfs::Read, Root);
                if (err.isError()) return err;
                char *buf = static_cast<char *>(Memory::alloc(MemSpace::Psram, max + 1));
                if (buf == nullptr) return Error::NoMem;
                size_t got = 0;
                while (got <= max) {
                        const Result<size_t> r = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(buf) + got, max + 1 - got), WaitForever);
                        if (r.error() == Error::EndOfFile) break;
                        if (r.error().isError()) {
                                err = r.error();
                                break;
                        }
                        if (r.value() == 0) break;
                        got += r.value();
                }
                if (err.isOk() && got > max) err = Error::BufferTooSmall;
                if (err.isOk()) err = out.assign(StringView(buf, got));
                Memory::free(buf);
                return err;
        }

} // namespace

// One scan's results.
struct Apps::Scan {
                List<App>     apps{MemSpace::Psram};
                List<Problem> problems{MemSpace::Psram};
                uint64_t      hash = 14695981039346656037ull;

                bool takenId(StringView id) const {
                        for (const App &a : apps)
                                if (StringView(a.id) == id) return true;
                        return false;
                }

                bool takenCommand(StringView command) const {
                        for (const App &a : apps)
                                if (StringView(a.command) == command) return true;
                        return false;
                }

                void problem(StringView folder, const char *why) {
                        Problem p;
                        p.why = why;
                        if (p.folder.assign(folder).isOk()) (void)problems.append(std::move(p));
                        hash = hashAppend(hashAppend(hash, folder), why);
                        return;
                }
};

bool Apps::isFolderName(StringView name) {
        const size_t ext = Extension.size();
        return name.size() > ext && name.mid(name.size() - ext).equalsIgnoreCase(Extension);
}

bool Apps::isValidName(StringView name) {
        if (name.isEmpty() || name.size() > DesktopEntry::MaxId || name[0] == '.' || name[0] == '-') return false;
        for (const char c : name) {
                const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                c == '.' || c == '_' || c == '+' || c == '-';
                if (!ok) return false;
        }
        return true;
}

Error Apps::start(const Config &config) {
        if (_started) return Error::InvalidState;
        if (config.roots == nullptr) return Error::Invalid;
        _config = config;
        _stopping.store(false, std::memory_order_release);
        // Every scan on the thread: its stack is the one sized for it (a Desktop Entry, a FAT or
        // exFAT lookup), not the caller's.
        Thread::Config tc;
        tc.name = config.name;
        tc.stackBytes = StackBytes;
        const Error err = _thread.start(tc, threadEntry, this);
        if (err.isError()) return err;
        _started = true;
        (void)_scanned.acquire(WaitForever);
        return Error::Ok;
}

void Apps::stop() {
        if (_started) {
                _stopping.store(true, std::memory_order_release);
                _wake.release();
                _thread.join();
                _started = false;
        }
        for (const String &path : _watched) _watches.remove(path.view());
        _watched.clear();
        return;
}

void Apps::rescan() {
        _wake.release();
        return;
}

void Apps::wakeEntry(void *arg) {
        static_cast<Apps *>(arg)->_wake.release(); // under the storage lock: nothing more
        return;
}

void Apps::threadEntry(void *arg) {
        static_cast<Apps *>(arg)->run();
        return;
}

// A change, then quiet for SettleMs (a folder being copied in changes again and again), at most
// MaxSettleMs: then a scan.
void Apps::run() {
        scan();
        _scanned.release();
        while (!_stopping.load(std::memory_order_acquire)) {
                (void)_wake.acquire(WaitForever);
                unsigned int waited = 0;
                while (!_stopping.load(std::memory_order_acquire) && waited < MaxSettleMs &&
                       _wake.acquire(SettleMs).isOk())
                        waited += SettleMs;
                if (_stopping.load(std::memory_order_acquire)) break;
                scan();
        }
        return;
}

void Apps::visit(Visit fn, void *arg) {
        Mutex::Locker locker(_lock, WaitForever);
        for (const App &a : _apps) fn(arg, a);
        return;
}

void Apps::visitProblems(VisitProblem fn, void *arg) {
        Mutex::Locker locker(_lock, WaitForever);
        for (const Problem &p : _problems) fn(arg, p);
        return;
}

size_t Apps::count() {
        Mutex::Locker locker(_lock, WaitForever);
        return _apps.size();
}

bool Apps::idOf(StringView folder, char (&out)[DesktopEntry::MaxId + 1]) {
        Mutex::Locker locker(_lock, WaitForever);
        for (const App &a : _apps) {
                if (!a.folder.view().equalsIgnoreCase(folder)) continue;
                std::memcpy(out, a.id, sizeof(out));
                return true;
        }
        return false;
}

// Watches @p path from now (before it is read), and keeps it watched after the scan.
Error Apps::watchLocked(StringView path) {
        for (const String &s : _seen)
                if (s.view() == path) return Error::Ok;
        String copy{MemSpace::Psram};
        Error  err = copy.assign(path);
        if (err.isOk()) err = _seen.append(std::move(copy));
        bool watched = false;
        for (const String &s : _watched) watched = watched || s.view() == path;
        if (err.isOk() && !watched) err = _watches.add(path);
        return err;
}

// Undoes this scan's watchLocked(@p path): not watched after it.
void Apps::unwatchLocked(StringView path) {
        for (size_t i = 0; i < _seen.size(); ++i) {
                if (_seen[i].view() != path) continue;
                _seen.removeAt(i);
                break;
        }
        bool watched = false;
        for (const String &s : _watched) watched = watched || s.view() == path;
        if (!watched) _watches.remove(path); // added by this scan
        return;
}

void Apps::scan() {
        Scan   s;
        String roots{MemSpace::Psram};
        if (_config.roots(_config.rootsArg, roots).isError()) roots.clear();
        _seen.clear();
        _watches.take([](void *, StringView, uint8_t) {}, nullptr); // read now: what was told is done
        StringView rest = roots.view();
        uint8_t    rootIndex = 0;
        while (!rest.isEmpty()) {
                const size_t     semi = rest.indexOf(';');
                const StringView root = semi == StringView::NPos ? rest : rest.left(semi);
                rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
                if (root.isEmpty() || root[0] != '/') continue;
                const uint8_t index = rootIndex++;
                (void)watchLocked(root);
                // The folders, sorted (a FAT folder's order is the order they were made in).
                List<String> names{MemSpace::Psram};
                {
                        auto [dir, err] = Vfs::openDirectory(root, Root);
                        if (err.isError()) {
                                // Not there (yet): its parent says when it comes. Its own watch goes:
                                // one placed on a folder since removed isn't told of a new one there.
                                unwatchLocked(root);
                                const size_t slash = root.lastIndexOf('/');
                                if (slash > 0) (void)watchLocked(root.left(slash));
                                continue;
                        }
                        for (;;) {
                                auto [info, nerr] = dir.next();
                                if (nerr.isError()) break;
                                if (!isFolderName(info.name.view())) continue;
                                String name{MemSpace::Psram};
                                if (name.assign(info.name.view()).isError()) continue;
                                size_t at = names.size();
                                while (at > 0 && lessIgnoringCase(name.view(), names[at - 1].view())) --at;
                                (void)names.insert(at, std::move(name));
                        }
                }
                for (const String &name : names) {
                        String folder{MemSpace::Psram};
                        if (folder.assign(root).isError() || folder.append('/').isError() ||
                            folder.append(name.view()).isError())
                                continue;
                        const Result<FileInfo> info = Vfs::stat(folder.view(), Root);
                        if (info.error().isError() || !info.value().isDirectory()) continue;
                        (void)watchLocked(folder.view());
                        String path{MemSpace::Psram};
                        String text{MemSpace::Psram};
                        Error  err = path.assign(folder.view());
                        if (err.isOk()) err = path.append('/');
                        if (err.isOk()) err = path.append(DescriptionFile);
                        if (err.isOk()) err = readSmallFile(path.view(), MaxDescription, text);
                        if (err.isError()) {
                                s.problem(folder.view(), err == Error::NotExist         ? "no app.desktop"
                                                         : err == Error::BufferTooSmall ? "app.desktop too large"
                                                                                        : "app.desktop unreadable");
                                continue;
                        }
                        auto [entry, perr] = DesktopEntry::parse(text.view());
                        if (perr.isError()) {
                                s.problem(folder.view(), "app.desktop is not an application's Desktop Entry");
                                continue;
                        }
                        if (entry.hidden) continue;
                        App app;
                        app.entry = entry;
                        app.root = index;
                        const StringView base = name.view().left(name.size() - Extension.size());
                        const StringView id = entry.id[0] != '\0' ? StringView(entry.id) : base;
                        const StringView command = entry.command[0] != '\0' ? StringView(entry.command) : id;
                        if (!isValidName(id)) {
                                s.problem(folder.view(), "not a valid id");
                                continue;
                        }
                        if (!isValidName(command)) {
                                s.problem(folder.view(), "not a valid command name");
                                continue;
                        }
                        if (s.takenId(id)) {
                                s.problem(folder.view(), "its id is another application's");
                                continue;
                        }
                        if (s.takenCommand(command)) {
                                s.problem(folder.view(), "its command is another application's");
                                continue;
                        }
                        std::memcpy(app.id, id.data(), id.size());
                        std::memcpy(app.command, command.data(), command.size());
                        // The program: Exec's first word, a file in the folder.
                        char       argText[DesktopEntry::MaxExec + 1];
                        StringView args[DesktopEntry::MaxArgs];
                        const auto split = DesktopEntry::splitExec(StringView(entry.exec), argText, sizeof(argText),
                                                                   Span<StringView>(args, DesktopEntry::MaxArgs));
                        if (split.error().isError() || !isInsideName(args[0])) {
                                s.problem(folder.view(), "Exec is not a program in the folder");
                                continue;
                        }
                        err = app.folder.assign(folder.view());
                        if (err.isOk()) err = app.program.assign(folder.view());
                        if (err.isOk()) err = app.program.append('/');
                        if (err.isOk()) err = app.program.append(args[0]);
                        if (err.isError()) continue;
                        const Result<FileInfo> prog = Vfs::stat(app.program.view(), Root);
                        if (prog.error().isError() || prog.value().isDirectory()) {
                                s.problem(folder.view(), "its program is missing");
                                continue;
                        }
                        s.hash = hashAppend(hashAppend(hashAppend(s.hash, app.id), app.command), app.program.view());
                        (void)s.apps.append(std::move(app));
                }
        }
        // The folders no longer seen aren't watched.
        for (const String &old : _watched) {
                bool kept = false;
                for (const String &now : _seen) kept = kept || now.view() == old.view();
                if (!kept) _watches.remove(old.view());
        }
        std::swap(_watched, _seen);
        publish(s);
        return;
}

// The scan's lists in place; /apps and the notify told if anything changed.
void Apps::publish(Scan &s) {
        bool changed;
        {
                Mutex::Locker locker(_lock, WaitForever);
                changed = s.hash != _hash || _generation.load(std::memory_order_relaxed) == 0;
                std::swap(_apps, s.apps);
                std::swap(_problems, s.problems);
                _hash = s.hash;
        }
        if (!changed) return;
        if (_config.links) {
                List<Vfs::AppLink> links{MemSpace::Psram};
                {
                        Mutex::Locker locker(_lock, WaitForever);
                        if (links.reserve(_apps.size()).isOk())
                                for (const App &a : _apps)
                                        (void)links.append(Vfs::AppLink{StringView(a.command), a.program.view()});
                        // Under the lock: the views are the list's. Names and targets are valid
                        // (isValidName(), absolute): only NoMem fails, and /apps keeps what it had.
                        (void)Vfs::setAppLinks(Span<const Vfs::AppLink>(links.data(), links.size()));
                }
        }
        _generation.fetch_add(1, std::memory_order_acq_rel);
        if (_config.notify != nullptr) _config.notify(_config.notifyArg);
        return;
}

BMDS_NAMESPACE_END
