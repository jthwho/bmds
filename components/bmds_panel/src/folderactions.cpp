/**
 * @file      folderactions.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a folder window and the desktop both do to files (devplan/filebrowser.md §4h, FB8):
 * moved here from the panel's desktop, written once.
 */

#include <cstdio>
#include <cstring>
#include <new>
#include <bmds/apps.h>
#include <bmds/clipboard.h>
#include <bmds/filenames.h>
#include <bmds/fdtable.h>
#include <bmds/fileformat.h>
#include <bmds/guiclipboard.h>
#include <bmds/inireader.h>
#include <bmds/memory.h>
#include <bmds/messagebox.h>
#include <bmds/mimeapps.h>
#include <bmds/programlauncher.h>
#include <bmds/vfs.h>
#include <bmds/windowmanager.h>

#include "folderactions.h"
#include "panelentries.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView DesktopSuffix(".desktop");
        constexpr size_t     MaxCandidates = 64; // launcher entries looked through for a MimeType

        // @p path's file, read whole up to @p max bytes into @p out (PSRAM). @return The bytes.
        size_t readFile(StringView path, const Vfs::Context &vc, char *out, size_t max) {
                size_t got = 0;
                auto [file, err] = Vfs::open(path, Vfs::Read, vc);
                while (err.isOk() && got < max) {
                        auto [n, rerr] = file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(out) + got, max - got),
                                                    WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                return got;
        }

} // namespace

void FolderActions::setApplications(StringView directories) {
        std::snprintf(_applications, sizeof(_applications), "%.*s", int(directories.size()), directories.data());
        return;
}

void FolderActions::setMimeApps(StringView path) {
        std::snprintf(_mimeApps, sizeof(_mimeApps), "%.*s", int(path.size()), path.data());
        return;
}

FolderActions::~FolderActions() {
        if (_askMenu != nullptr) {
                _askMenu->~Menu();
                Memory::free(_askMenu);
        }
}

void FolderActions::say(const char *a, const char *b, const char *c, const char *d) {
        FileHandle *err = _context.fds != nullptr ? _context.fds->handle(FdTable::Stderr) : nullptr;
        if (err == nullptr) return;
        for (const char *text : {a, b, c, d})
                (void)err->write(ByteSpan(reinterpret_cast<const uint8_t *>(text), std::strlen(text)), WaitForever);
        return;
}

void FolderActions::tell(StringView title, StringView message, bool error) {
        if (MessageBox::show(_app, error ? MessageBox::Kind::Error : MessageBox::Kind::Information, title, message,
                             MessageBox::Buttons::Ok, nullptr, nullptr)
                    .isOk())
                return;
        char line[200];
        std::snprintf(line, sizeof(line), "panel: %.*s\n", int(message.size()), message.data());
        say(line);
        return;
}

Error FolderActions::programPath(StringView command, String &out) const {
        out.clear();
        if (command.contains('/')) return out.append(command);
        StringView searchPath;
        for (const StringView &v : _context.environment)
                if (v.startsWith(StringView("PATH="))) searchPath = v.mid(5);
        const Vfs::Context vc{_context.cwd, nullptr};
        return Vfs::findProgram(command, searchPath, vc, out);
}

// The entry's command started (in a terminal if it asks), not waited for; given @p file, its
// field codes are it (devplan/filetypes.md T7).
void FolderActions::launch(const DesktopEntry &entry, StringView file) {
        if (_context.launcher == nullptr) return;
        char         buffer[DesktopEntry::MaxExec + Vfs::MaxPath + 1];
        StringView   args[DesktopEntry::MaxArgs + 2];
        const size_t front = entry.terminal ? 2 : 0;
        auto [n, err] = DesktopEntry::splitExec(StringView(entry.exec), buffer, sizeof(buffer),
                                                Span<StringView>(args + front, DesktopEntry::MaxArgs), file);
        if (err.isError()) {
                say("panel: ", entry.name, ": its Exec line can't be used\n");
                return;
        }
        if (entry.terminal) {
                args[0] = "terminal";
                args[1] = "-e";
        }
        const size_t count = n + front;
        String       found;
        err = programPath(args[0], found);
        if (err.isOk()) {
                ProgramLauncher::Request request;
                request.path = found.view();
                request.args = Span<const StringView>(args, count);
                request.environment = _context.environment;
                request.fds = _context.fds;
                request.cwd = _context.cwd;
                request.console = _context.console;
                request.session = true; // not the panel's terminal's: its signals aren't the program's
                auto [pid, serr] = _context.launcher->start(request);
                err = serr;
                if (err.isOk() && _context.process != nullptr) _context.process->disownChild(pid);
                if (err.isOk()) { // the pointer shows it starting until its window opens (windows.md §4l)
                        auto [wm, werr] = WindowManager::manager(0);
                        if (werr.isOk() && wm != nullptr)
                                wm->programStarting(pid, _context.launcher, WindowManager::StartingTimeoutMs);
                }
        }
        if (err.isError())
                say("panel: ", entry.name, ": ", err == Error::NotExist ? "not found\n" : "can't start it\n");
        return;
}

// @p entry on @p files (a line each): one start with them all if its command takes a list (%F,
// %U, or no field code), else one a file (%f, %u), MaxStartsPerDrop at most.
void FolderActions::launchFiles(const DesktopEntry &entry, StringView files) {
        List<StringView> list(MemSpace::Psram);
        size_t           bytes = 0;
        for (StringView rest = files; !rest.isEmpty();) {
                const size_t     end = rest.indexOf('\n');
                const StringView f = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (f.isEmpty() || list.append(f).isError()) continue;
                bytes += f.size() + 1;
        }
        if (list.isEmpty()) return;
        if (!DesktopEntry::takesFileList(StringView(entry.exec))) {
                for (size_t i = 0; i < list.size() && i < MaxStartsPerDrop; ++i) launch(entry, list[i]);
                return;
        }
        if (_context.launcher == nullptr) return;
        const size_t size = DesktopEntry::MaxExec + bytes + 1;
        char        *buffer = static_cast<char *>(Memory::alloc(MemSpace::Psram, size));
        if (buffer == nullptr) return;
        StringView   args[DesktopEntry::MaxArgs + 2];
        const size_t front = entry.terminal ? 2 : 0;
        auto [n, err] = DesktopEntry::splitExec(StringView(entry.exec), buffer, size,
                                                Span<StringView>(args + front, DesktopEntry::MaxArgs), list.span());
        if (err.isError()) {
                say("panel: ", entry.name, ": too many files for its command\n");
                Memory::free(buffer);
                return;
        }
        if (entry.terminal) {
                args[0] = "terminal";
                args[1] = "-e";
        }
        String found;
        err = programPath(args[0], found);
        if (err.isOk()) {
                ProgramLauncher::Request request;
                request.path = found.view();
                request.args = Span<const StringView>(args, n + front);
                request.environment = _context.environment;
                request.fds = _context.fds;
                request.cwd = _context.cwd;
                request.console = _context.console;
                request.session = true;
                auto [pid, serr] = _context.launcher->start(request);
                err = serr;
                if (err.isOk() && _context.process != nullptr) _context.process->disownChild(pid);
                if (err.isOk()) {
                        auto [wm, werr] = WindowManager::manager(0);
                        if (werr.isOk() && wm != nullptr)
                                wm->programStarting(pid, _context.launcher, WindowManager::StartingTimeoutMs);
                }
        }
        if (err.isError())
                say("panel: ", entry.name, ": ", err == Error::NotExist ? "not found\n" : "can't start it\n");
        Memory::free(buffer);
        return;
}

void FolderActions::open(StringView path) {
        const Vfs::Context vc{_context.cwd, nullptr};
        auto [info, err] = Vfs::stat(path, vc);
        if (err.isError()) {
                char message[160];
                std::snprintf(message, sizeof(message), "%.*s isn't there.", int(path.size()), path.data());
                tell("Open", StringView(message));
                return;
        }
        if (info.type == FileType::Directory) {
                // An application's folder runs it (devplan/apps.md §7); one it can't run opens.
                DesktopEntry entry;
                if (Apps::isFolderName(path.mid(path.lastIndexOf('/') + 1)) &&
                    readAppFolder(path, vc, StringView(), entry)) {
                        launch(entry);
                        return;
                }
                _host.openFolder(path, StringView());
                return;
        }
        if (path.endsWith(DesktopSuffix)) {
                // A launcher: its command (another Desktop Entry, a link, opens as text).
                char        *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, 4096));
                const size_t got = text != nullptr ? readFile(path, vc, text, 4096) : 0;
                auto [entry, perr] = DesktopEntry::parse(StringView(text != nullptr ? text : "", got));
                Memory::free(text);
                if (perr.isOk() && entry.exec[0] != '\0') {
                        launch(entry);
                        return;
                }
        }
        auto [type, ierr] = Vfs::identify(path, Vfs::IdentifyName | Vfs::IdentifyContent, vc);
        openFile(path, ierr.isOk() ? type : FileFormat::Binary);
        return;
}

// By the application mimeapps.list names for its type (devplan/filetypes.md T6): the type's, its
// media's, text/plain's for text, then application/octet-stream's.
void FolderActions::openFile(StringView path, StringView type) {
        constexpr size_t   MaxBytes = 4096;
        const Vfs::Context vc{_context.cwd, nullptr};
        char              *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxBytes));
        if (text == nullptr) return;
        const size_t got = readFile(StringView(_mimeApps), vc, text, MaxBytes);
        bool         opened = false;
        StringView   types[] = {type, FileFormat::isText(type) ? FileFormat::PlainText : StringView(),
                                FileFormat::Binary};
        for (size_t t = 0; t < 3 && !opened; ++t) {
                if (types[t].isEmpty() || (t > 0 && types[t] == type)) continue;
                StringView   ids[8];
                const size_t n = MimeApps::lookup(StringView(text, got), types[t], Span<StringView>(ids));
                for (size_t i = 0; i < n && !opened; ++i) opened = openWith(ids[i], path);
                // None set for it: an application that says it opens it (MimeType=, T6's fallback,
                // apps.md §7); not for application/octet-stream, which says nothing.
                if (!opened && t < 2) opened = openByMimeType(path, types[t]);
        }
        Memory::free(text);
        if (!opened) {
                char message[200];
                std::snprintf(message, sizeof(message), "There's no application set to open %.*s files.",
                              int(type.size()), type.data());
                tell("Open", StringView(message), false);
        }
        return;
}

// Application @p id (a Desktop Entry file in the launcher's directories, the first found) started
// on @p file. False if it isn't there.
bool FolderActions::openWith(StringView id, StringView file) {
        const Vfs::Context vc{_context.cwd, nullptr};
        StringView         dirs(_applications);
        for (StringView d = IniReader::nextListItem(dirs); !d.isEmpty(); d = IniReader::nextListItem(dirs)) {
                if (d == Vfs::AppsPath) {
                        // The installed applications: their entries are named ID.desktop.
                        auto *entries = static_cast<PanelEntry *>(
                                Memory::alloc(MemSpace::Psram, sizeof(PanelEntry) * MaxCandidates));
                        if (entries == nullptr) continue;
                        size_t count = 0;
                        for (size_t i = 0; i < MaxCandidates; ++i) ::new (&entries[i]) PanelEntry();
                        (void)readEntryDirectory(d, vc, entries, count, MaxCandidates, nullptr, nullptr);
                        bool found = false;
                        for (size_t i = 0; i < count && !found; ++i) {
                                if (StringView(entries[i].file) != id) continue;
                                found = true;
                                launch(entries[i].entry, file);
                        }
                        Memory::free(entries);
                        if (found) return true;
                        continue;
                }
                String path;
                if (path.append(d).isError() || path.append('/').isError() || path.append(id).isError()) continue;
                char         text[1024];
                const size_t got = readFile(path.view(), vc, text, sizeof(text));
                if (got == 0) continue;
                auto [entry, perr] = DesktopEntry::parse(StringView(text, got));
                if (perr.isError()) continue;
                if (entry.hidden) return false; // hides the later ones of its name
                launch(entry, file);
                return true;
        }
        return false;
}

// The first launcher entry (the launcher's directories in order, the first of a name) whose
// MimeType has @p type, started on @p path. False if none has it.
bool FolderActions::openByMimeType(StringView path, StringView type) {
        auto *entries = static_cast<PanelEntry *>(Memory::alloc(MemSpace::Psram, sizeof(PanelEntry) * MaxCandidates));
        if (entries == nullptr) return false;
        for (size_t i = 0; i < MaxCandidates; ++i) ::new (&entries[i]) PanelEntry();
        const Vfs::Context vc{_context.cwd, nullptr};
        size_t             count = 0;
        StringView         dirs(_applications);
        for (StringView d = IniReader::nextListItem(dirs); !d.isEmpty(); d = IniReader::nextListItem(dirs))
                (void)readEntryDirectory(d, vc, entries, count, MaxCandidates, nullptr, nullptr);
        bool opened = false;
        for (size_t i = 0; i < count && !opened; ++i) {
                const DesktopEntry &e = entries[i].entry;
                if (e.hidden || !DesktopEntry::hasMimeType(StringView(e.mimeTypes), type)) continue;
                launch(e, path);
                opened = true;
        }
        Memory::free(entries);
        return opened;
}

// ---- The clipboard (devplan/clipboard.md §4f, devplan/filebrowser.md §4j)

void FolderActions::copy(StringView paths) {
        offer(paths, false);
        return;
}

void FolderActions::cut(StringView paths) {
        offer(paths, true);
        return;
}

// @p paths offered as a URI list, GNOME's list (copied or cut) and text, rendered when asked for.
void FolderActions::offer(StringView paths, bool cut) {
        GuiClipboard *clipboard = _app.clipboard();
        if (clipboard == nullptr || paths.isEmpty()) return;
        const bool wasCut = _copied.isCut();
        if (_copied.set(paths, cut).isError()) return;
        clipboard->setName("panel");
        clipboard->setLost(lost, this);
        const StringView types[] = {CopiedFiles::UriList, CopiedFiles::GnomeFiles, ClipboardType::Text};
        if (clipboard->copy(Clipboard::Selection::Clipboard, Span<const StringView>(types), render, this).isError()) {
                say("panel: can't copy\n");
                _copied.clear();
        }
        if (wasCut || _copied.isCut()) _host.cutChanged();
        return;
}

Error FolderActions::render(void *arg, StringView type, String &out) {
        return static_cast<FolderActions *>(arg)->_copied.render(type, out);
}

// Another program's copy replaced this panel's: nothing is cut now.
void FolderActions::lost(void *arg, Clipboard::Selection selection) {
        auto *self = static_cast<FolderActions *>(arg);
        if (selection != Clipboard::Selection::Clipboard) return;
        const bool wasCut = self->_copied.isCut();
        self->_copied.clear();
        if (wasCut) self->_host.cutChanged();
        return;
}

bool FolderActions::canPaste() {
        GuiClipboard *clipboard = _app.clipboard();
        return clipboard != nullptr && (clipboard->canPaste(Clipboard::Selection::Clipboard, CopiedFiles::GnomeFiles) ||
                                        clipboard->canPaste(Clipboard::Selection::Clipboard, CopiedFiles::UriList));
}

// GNOME's list when offered (it says whether the files were cut), else a URI list (copied).
void FolderActions::paste(StringView directory) {
        GuiClipboard *clipboard = _app.clipboard();
        if (clipboard == nullptr || _pasteTo.assign(directory).isError()) return;
        const StringView type = clipboard->canPaste(Clipboard::Selection::Clipboard, CopiedFiles::GnomeFiles)
                                        ? CopiedFiles::GnomeFiles
                                        : CopiedFiles::UriList;
        if (clipboard->paste(Clipboard::Selection::Clipboard, type, pasted, this).isError())
                say("panel: can't paste\n");
        return;
}

// The pasted list: copied into the folder on the worker, or, cut, moved (renamed at once when that
// is all it takes). A cut pasted empties the clipboard: its files are no longer where it says.
void FolderActions::pasted(void *arg, Error err, StringView type, StringView data) {
        auto *self = static_cast<FolderActions *>(arg);
        if (err.isError()) {
                self->say("panel: paste: ", err.desc(), "\n");
                return;
        }
        bool   cut = false;
        String paths(MemSpace::Psram);
        if (CopiedFiles::parse(type, data, cut, paths).isError()) return;
        const StringView directory = self->_pasteTo.view();
        if (!cut) {
                self->_jobs.copy(paths.view(), directory);
                return;
        }
        if (!self->moveNow(paths.view(), directory)) self->_jobs.move(paths.view(), directory);
        GuiClipboard *clipboard = self->_app.clipboard();
        if (clipboard != nullptr) (void)clipboard->clear(Clipboard::Selection::Clipboard); // if still this panel's
        if (self->_copied.isCut()) {
                self->_copied.clear();
                self->_host.cutChanged();
        }
        return;
}

// @p paths moved into @p directory now, on the loop, if each is a rename to a free name (the same
// filesystem). @return False, with nothing moved, if any isn't: the worker moves them, asking.
bool FolderActions::moveNow(StringView paths, StringView directory) {
        const Vfs::Context vc{_context.cwd, nullptr};
        auto [into, ierr] = Vfs::stat(directory, vc);
        if (ierr.isError() || !into.isDirectory()) return false;
        String     to(MemSpace::Psram);
        StringView rest = paths;
        while (!rest.isEmpty()) {
                const size_t     end = rest.indexOf('\n');
                const StringView path = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (path.isEmpty()) continue;
                auto [info, lerr] = Vfs::lstat(path, vc);
                if (lerr.isError() || info.device != into.device) return false;
                if (to.assign(directory).isError()) return false;
                if (!directory.endsWith('/') && to.append('/').isError()) return false;
                if (to.append(FileOps::baseName(path)).isError()) return false;
                if (to.view() != path && Vfs::lstat(to.view(), vc).isOk()) return false; // taken: asked
        }
        FileOps::Progress quiet; // nothing taken, nothing to ask
        Error             first;
        char              failed[Vfs::MaxPath + 1] = {};
        rest = paths;
        while (!rest.isEmpty()) {
                const size_t     end = rest.indexOf('\n');
                const StringView path = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (path.isEmpty()) continue;
                const Error err = FileOps::move(path, directory, vc, quiet);
                if (err.isError() && first.isOk()) {
                        first = err;
                        std::snprintf(failed, sizeof(failed), "%.*s", int(path.size()), path.data());
                }
                const StringView folder = path.left(path.lastIndexOf('/') > 0 ? path.lastIndexOf('/') : 1);
                _host.folderChanged(folder);
        }
        _host.folderChanged(directory);
        if (first.isError()) {
                char message[200];
                std::snprintf(message, sizeof(message), "Can't move %.120s: %s.", failed, first.desc());
                tell("Move", StringView(message));
        }
        return true;
}

// ---- Drag and drop (devplan/clipboard.md §5e)

bool FolderActions::dragFiles(StringView paths, GuiDrag &drag) {
        if (paths.isEmpty() || _dragged.set(paths, false).isError()) return false;
        static constexpr StringView Types[] = {CopiedFiles::UriList, ClipboardType::Text};
        drag.types = Span<const StringView>(Types);
        drag.provider = renderDrag;
        drag.providerArg = this;
        drag.actions = DropActions::CanCopy | DropActions::CanMove | DropActions::CanLink;
        drag.preferred = DropAction::Copy;
        drag.ended = dragEnded;
        drag.endedArg = this;
        return true;
}

Error FolderActions::renderDrag(void *arg, StringView type, String &out) {
        return static_cast<FolderActions *>(arg)->_dragged.render(type, out);
}

// The drag over: the files forgotten (a target that moved them said so: the watches see it).
void FolderActions::dragEnded(void *arg, DropAction done) {
        (void)done;
        static_cast<FolderActions *>(arg)->_dragged.clear();
        return;
}

// What @p path is to a drop: a folder, an application (a launcher with a command, a NAME.app
// folder: @p entry filled if given), or neither.
FolderActions::Target FolderActions::targetOf(StringView path, DesktopEntry *entry) const {
        const Vfs::Context vc{_context.cwd, nullptr};
        auto [info, err] = Vfs::stat(path, vc);
        if (err.isError()) return Target::None;
        DesktopEntry e;
        if (info.isDirectory()) {
                if (!Apps::isFolderName(FileOps::baseName(path))) return Target::Folder;
                if (!readAppFolder(path, vc, StringView(), e)) return Target::Folder;
        } else {
                if (!path.endsWith(DesktopSuffix)) return Target::None;
                char         text[1024];
                const size_t got = readFile(path, vc, text, sizeof(text));
                auto [parsed, perr] = DesktopEntry::parse(StringView(text, got));
                if (perr.isError() || parsed.exec[0] == '\0') return Target::None;
                e = parsed;
        }
        if (entry != nullptr) *entry = e;
        return Target::Application;
}

DropAction FolderActions::dropAction(StringView target, const GuiDropEvent &event) {
        if (!event.hasType(CopiedFiles::UriList)) return DropAction::None;
        const Target kind = targetOf(target, nullptr);
        if (kind == Target::None) return DropAction::None;
        if (kind == Target::Application)
                return DropActions::allows(event.actions(), DropAction::Copy) ? DropAction::Copy : DropAction::None;
        // A folder: this panel's own drag is known (its files): not into one of them, nor where all
        // of them are; moved on their filesystem, else copied (Windows' rule). Another's: copied.
        DropAction         fallback = DropAction::Copy;
        const StringView   paths = event.isOwn() ? _dragged.paths() : StringView();
        const Vfs::Context vc{_context.cwd, nullptr};
        if (!paths.isEmpty()) {
                auto [into, ierr] = Vfs::stat(target, vc);
                bool allHere = true, sameDevice = false, first = true;
                for (StringView rest = paths; !rest.isEmpty();) {
                        const size_t     end = rest.indexOf('\n');
                        const StringView p = end == StringView::NPos ? rest : rest.left(end);
                        rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                        if (p.isEmpty()) continue;
                        if (target == p ||
                            (target.startsWith(p) && target.size() > p.size() && target[p.size()] == '/'))
                                return DropAction::None; // into itself
                        const size_t     slash = p.lastIndexOf('/');
                        const StringView parent = p.left(slash > 0 ? slash : 1);
                        const StringView to =
                                target.size() > 1 && target.endsWith('/') ? target.left(target.size() - 1) : target;
                        allHere = allHere && parent == to;
                        if (first && ierr.isOk()) {
                                auto [info, lerr] = Vfs::lstat(p, vc);
                                sameDevice = lerr.isOk() && info.device == into.device;
                        }
                        first = false;
                }
                if (allHere) return DropAction::None;
                if (sameDevice) fallback = DropAction::Move;
        }
        return event.suggested(fallback);
}

bool FolderActions::drop(StringView target, GuiDropEvent &event, uint16_t screenX, uint16_t screenY) {
        const DropAction action = dropAction(target, event);
        if (action == DropAction::None || _dropDrag != 0) return false; // one at a time
        if (_dropTo.assign(target).isError()) return false;
        if (event.read(CopiedFiles::UriList, dropRead, this).isError()) return false;
        _dropDrag = event.drag();
        _dropAction = action;
        _dropAsk = event.isAsk() && targetOf(target, nullptr) == Target::Folder;
        _dropX = screenX;
        _dropY = screenY;
        return true;
}

// The drop's files read: done at once, or asked first (a right drag's menu).
void FolderActions::dropRead(void *arg, Error err, StringView type, StringView data) {
        auto *self = static_cast<FolderActions *>(arg);
        bool  cut = false;
        if (err.isOk()) err = CopiedFiles::parse(type, data, cut, self->_dropPaths);
        if (err.isError() || self->_dropPaths.isEmpty()) {
                self->dropDone(DropAction::None);
                return;
        }
        if (!self->_dropAsk) {
                self->dropDone(self->_dropAction);
                return;
        }
        if (self->_askMenu == nullptr) {
                void *mem = Memory::alloc(MemSpace::Psram, sizeof(Menu));
                if (mem == nullptr) {
                        self->dropDone(DropAction::None);
                        return;
                }
                self->_askMenu = ::new (mem) Menu(self->_app);
                self->_askMenu->setTriggered(askChosen, self);
                self->_askMenu->setDismissed(askDismissed, self);
        }
        // What the source allows, the choice by the keys first.
        Menu &m = *self->_askMenu;
        m.clear();
        (void)m.addItem("&Move Here", int(DropAction::Move));
        (void)m.addItem("&Copy Here", int(DropAction::Copy));
        (void)m.addItem("Create &Link Here", int(DropAction::Link));
        (void)m.addSeparator();
        (void)m.addItem("Cancel", int(DropAction::None));
        self->_askPending = true;
        if (m.popup(Rect{self->_dropX, self->_dropY, 0, 0}).isError()) {
                self->_askPending = false;
                self->dropDone(DropAction::None);
        }
        return;
}

void FolderActions::askChosen(void *arg, int item) {
        auto *self = static_cast<FolderActions *>(arg);
        if (!self->_askPending) return;
        self->_askPending = false;
        self->dropDone(static_cast<DropAction>(item));
        return;
}

// Closed: a choice comes right after, if one was made (the menu tells it after closing).
void FolderActions::askDismissed(void *arg, Menu &) {
        (void)static_cast<FolderActions *>(arg)->_app.post(askCheck, arg);
        return;
}

void FolderActions::askCheck(void *arg) {
        auto *self = static_cast<FolderActions *>(arg);
        if (!self->_askPending) return;
        self->_askPending = false;
        self->dropDone(DropAction::None); // dismissed: cancelled
        return;
}

// The drop done as @p action (None: cancelled), and finished.
void FolderActions::dropDone(DropAction action) {
        const uint32_t drag = _dropDrag;
        _dropDrag = 0;
        DropAction   done = DropAction::None;
        DesktopEntry entry;
        const Target kind = action != DropAction::None ? targetOf(_dropTo.view(), &entry) : Target::None;
        if (kind == Target::Application) {
                launchFiles(entry, _dropPaths.view());
                done = DropAction::Copy;
        } else if (kind == Target::Folder) {
                const StringView directory = _dropTo.view();
                _host.dropping(directory, _dropPaths.view());
                if (action == DropAction::Copy) {
                        _jobs.copy(_dropPaths.view(), directory);
                        done = DropAction::Copy;
                } else if (action == DropAction::Move) {
                        if (!moveNow(_dropPaths.view(), directory)) _jobs.move(_dropPaths.view(), directory);
                        done = DropAction::Moved; // here: the source has nothing to remove
                } else if (action == DropAction::Link) {
                        link(_dropPaths.view(), directory);
                        done = DropAction::Link;
                }
        }
        _dropPaths.clear();
        (void)_app.windowManager().finishDrop(drag, done);
        return;
}

// A symbolic link in @p directory to each of @p paths, named as it is (a copy's name if taken).
void FolderActions::link(StringView paths, StringView directory) {
        const Vfs::Context vc{_context.cwd, nullptr};
        String             to(MemSpace::Psram);
        Error              first;
        for (StringView rest = paths; !rest.isEmpty();) {
                const size_t     end = rest.indexOf('\n');
                const StringView path = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (path.isEmpty()) continue;
                const StringView name = FileOps::baseName(path);
                char             buffer[Vfs::MaxPath + 1];
                Error            err = Error::Exists;
                for (unsigned n = 0; n <= CopyName::MaxCopies && err == Error::Exists; ++n) {
                        StringView use = name;
                        if (n > 0) {
                                auto [made, merr] = CopyName::make(name, n, Span<char>(buffer));
                                if (merr.isError()) break;
                                use = made;
                        }
                        if (to.assign(directory).isError() || (!directory.endsWith('/') && to.append('/').isError()) ||
                            to.append(use).isError())
                                break;
                        err = Vfs::lstat(to.view(), vc).isOk() ? Error(Error::Exists)
                                                               : Vfs::symlink(path, to.view(), vc);
                }
                if (err.isError() && first.isOk()) first = err;
        }
        _host.folderChanged(directory);
        if (first.isError()) {
                char message[160];
                std::snprintf(message, sizeof(message), "Can't make a link: %s.", first.desc());
                tell("Link", StringView(message));
        }
        return;
}

// ---- Files

void FolderActions::removeForGood(StringView paths) {
        _jobs.removeAsking(paths);
        return;
}

void FolderActions::trash(StringView paths) {
        // To their filesystems' trash (devplan/files.md F8): `trash -r` brings them back.
        const Vfs::Context vc{_context.cwd, nullptr};
        Error              first;
        char               failed[Vfs::MaxPath + 1] = {};
        StringView         rest = paths, changed;
        while (!rest.isEmpty()) {
                const size_t     end = rest.indexOf('\n');
                const StringView path = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (path.isEmpty()) continue;
                const Error err = FileOps::trash(path, vc);
                if (err.isError() && first.isOk()) {
                        first = err;
                        std::snprintf(failed, sizeof(failed), "%.*s", int(path.size()), path.data());
                }
                // The folders told once each: a list's items are mostly in one.
                const StringView folder = path.left(path.lastIndexOf('/') > 0 ? path.lastIndexOf('/') : 1);
                if (folder != changed) _host.folderChanged(folder);
                changed = folder;
        }
        if (first.isError()) {
                char message[200];
                std::snprintf(message, sizeof(message), "Can't move %.120s to the trash: %s.", failed, first.desc());
                tell("Delete", StringView(message));
        }
        return;
}

Result<String> FolderActions::makeNew(StringView directory, FileOps::NewKind kind) {
        const Vfs::Context vc{_context.cwd, nullptr};
        (void)Vfs::mkdir(directory, vc); // there already, most likely
        auto made = FileOps::makeNew(directory, kind, vc);
        if (made.error().isError()) {
                char message[160];
                std::snprintf(message, sizeof(message), "Can't make it: %s.", made.error().desc());
                tell("New", StringView(message));
                return made;
        }
        _host.folderChanged(directory);
        return made;
}

bool FolderActions::rename(StringView path, StringView name, StringView suffix, size_t maxName, String &to,
                           Span<char> problem) {
        problem[0] = '\0';
        const StringView file = path.mid(path.lastIndexOf('/') + 1);
        const StringView old = file.endsWith(suffix) ? file.left(file.size() - suffix.size()) : file;
        name = name.trimmed();
        if (name.isEmpty() || name == old) return false; // nothing to do
        bool dots = true;
        for (const char ch : name) dots = dots && (ch == '.' || ch == ' ');
        if (dots) {
                std::snprintf(problem.data(), problem.size(), "A name needs more than dots and spaces.");
                return false;
        }
        if (name.size() + suffix.size() > maxName) {
                std::snprintf(problem.data(), problem.size(), "That name is too long: %zu bytes at most.",
                              maxName - suffix.size());
                return false;
        }
        for (const char ch : name) {
                if (!StringView("/\\:*?\"<>|").contains(ch)) continue;
                std::snprintf(problem.data(), problem.size(), "A name can't hold any of these: / \\ : * ? \" < > |");
                return false;
        }
        Error err = to.assign(path.left(path.lastIndexOf('/') + 1));
        if (err.isOk()) err = to.append(name);
        if (err.isOk()) err = to.append(suffix);
        const Vfs::Context vc{_context.cwd, nullptr};
        // Taken by another file (a name differing only in case is this one: FAT).
        if (err.isOk() && !name.equalsIgnoreCase(old) && Vfs::lstat(to.view(), vc).isOk()) {
                std::snprintf(problem.data(), problem.size(), "There is already a file named \"%.*s\" here.",
                              int(name.size()), name.data());
                return false;
        }
        if (err.isOk()) err = FileOps::rename(path, to.view().mid(to.view().lastIndexOf('/') + 1), vc);
        if (err.isError()) {
                std::snprintf(problem.data(), problem.size(), "Can't rename it: %s.", err.desc());
                return false;
        }
        _host.folderChanged(path.left(path.lastIndexOf('/') > 0 ? path.lastIndexOf('/') : 1));
        return true;
}

BMDS_NAMESPACE_END
