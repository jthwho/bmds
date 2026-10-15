/**
 * @file      panelentries.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The panel's Desktop Entry files, read from directories (devplan/gui.md §4c, §4m).
 */

#include <cstring>
#include <bmds/apps.h>
#include <bmds/memory.h>
#include <bmds/string.h>

#include "panelentries.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxEntryBytes = 4096;

        // Up to MaxEntryBytes of @p path into @p text, @p got of them; false if it can't be opened.
        bool readText(StringView path, const Vfs::Context &context, char *text, size_t &got) {
                auto [file, oerr] = Vfs::open(path, Vfs::Read, context);
                got = 0;
                while (oerr.isOk() && got < MaxEntryBytes) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxEntryBytes - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                return oerr.isOk();
        }

        // @p exec with its program (the first word, quoted or not) replaced by @p program.
        Error replaceProgram(char *exec, size_t bytes, StringView program) {
                const StringView all(exec);
                size_t           i = 0;
                while (i < all.size() && (all[i] == ' ' || all[i] == '\t')) ++i;
                bool quoted = false;
                for (; i < all.size() && (quoted || (all[i] != ' ' && all[i] != '\t')); ++i) {
                        if (all[i] == '\\' && quoted && i + 1 < all.size())
                                ++i;
                        else if (all[i] == '"')
                                quoted = !quoted;
                }
                const StringView rest = all.mid(i);
                if (program.size() + rest.size() + 1 > bytes) return Error::BufferTooSmall;
                char out[DesktopEntry::MaxExec + 1];
                std::memcpy(out, program.data(), program.size());
                std::memcpy(out + program.size(), rest.data(), rest.size());
                out[program.size() + rest.size()] = '\0';
                std::memcpy(exec, out, program.size() + rest.size() + 1);
                return Error::Ok;
        }

        // A program's name in its folder: a relative path that stays in it.
        bool isInsideName(StringView path) {
                if (path.isEmpty() || path[0] == '/') return false;
                for (StringView rest = path; !rest.isEmpty();) {
                        const size_t     slash = rest.indexOf('/');
                        const StringView part = slash == StringView::NPos ? rest : rest.left(slash);
                        if (part.isEmpty() || part == "." || part == "..") return false;
                        rest = slash == StringView::NPos ? StringView() : rest.mid(slash + 1);
                }
                return true;
        }

        // readAppFolder() with @p text (MaxEntryBytes) to read into.
        bool readAppFolderInto(StringView folder, const Vfs::Context &context, StringView program, DesktopEntry &out,
                               char *text) {
                String path;
                if (path.append(folder).isError() || path.append('/').isError() ||
                    path.append(Apps::DescriptionFile).isError())
                        return false;
                size_t got = 0;
                if (!readText(path.view(), context, text, got)) return false;
                auto [entry, perr] = DesktopEntry::parse(StringView(text, got));
                if (perr.isError() || entry.hidden) return false;
                String own;
                if (program.isEmpty()) {
                        // Exec's first word, a file in the folder.
                        char       argText[DesktopEntry::MaxExec + 1];
                        StringView args[DesktopEntry::MaxArgs];
                        const auto split = DesktopEntry::splitExec(StringView(entry.exec), argText, sizeof(argText),
                                                                   Span<StringView>(args, DesktopEntry::MaxArgs));
                        if (split.error().isError() || !isInsideName(args[0])) return false;
                        if (own.append(folder).isError() || own.append('/').isError() || own.append(args[0]).isError())
                                return false;
                        program = own.view();
                }
                if (replaceProgram(entry.exec, sizeof(entry.exec), program).isError()) return false;
                out = entry;
                return true;
        }

        // The installed applications, through /apps: each link leads to a program in its NAME.app
        // folder, whose app.desktop is the entry.
        void readApps(const Vfs::Context &context, PanelEntry *entries, size_t &count, size_t max, char *text) {
                auto [dir, err] = Vfs::openDirectory(Vfs::AppsPath, context);
                if (err.isError()) return;
                for (;;) {
                        auto [info, nerr] = dir.next();
                        if (nerr.isError()) break;
                        const StringView command = info.name.view();
                        String           link;
                        if (link.append(Vfs::AppsPath).isError() || link.append('/').isError() ||
                            link.append(command).isError())
                                continue;
                        auto [program, lerr] = Vfs::readLink(link.view(), context);
                        if (lerr.isError()) continue;
                        // Its folder: the NAME.app the program is in, at any depth (Exec=bin/x).
                        StringView folder = program.view();
                        while (!folder.isEmpty() && !Apps::isFolderName(folder.mid(folder.lastIndexOf('/') + 1))) {
                                const size_t slash = folder.lastIndexOf('/');
                                folder = slash == StringView::NPos || slash == 0 ? StringView() : folder.left(slash);
                        }
                        if (folder.isEmpty()) continue;
                        String path;
                        if (path.append(folder).isError() || path.append('/').isError() ||
                            path.append(Apps::DescriptionFile).isError() || path.size() >= sizeof(PanelEntry::path))
                                continue;
                        // Hidden or unusable: the apps service has said why.
                        DesktopEntry entry;
                        if (!readAppFolderInto(folder, context, link.view(), entry, text)) continue;
                        // Its id, as the system's (devplan/apps.md §3): X-BMDS-Id, else the folder's name.
                        const StringView name = folder.mid(folder.lastIndexOf('/') + 1);
                        const StringView id = entry.id[0] != '\0' ? StringView(entry.id)
                                                                  : name.left(name.size() - Apps::Extension.size());
                        char             file[sizeof(PanelEntry::file)];
                        const size_t     fileBytes = id.size() + 8; // ".desktop"
                        if (fileBytes >= sizeof(file)) continue;
                        std::memcpy(file, id.data(), id.size());
                        std::memcpy(file + id.size(), ".desktop", 9);
                        bool seen = false;
                        for (size_t i = 0; i < count; ++i)
                                seen = seen || StringView(entries[i].file) == StringView(file);
                        if (seen || count == max) continue;
                        PanelEntry &e = entries[count++];
                        e = PanelEntry();
                        e.entry = entry;
                        std::memcpy(e.file, file, fileBytes + 1);
                        std::memcpy(e.path, path.data(), path.size() + 1);
                }
                return;
        }

} // namespace

bool readAppFolder(StringView folder, const Vfs::Context &context, StringView program, DesktopEntry &entry) {
        char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxEntryBytes));
        if (text == nullptr) return false;
        const bool ok = readAppFolderInto(folder, context, program, entry, text);
        Memory::free(text);
        return ok;
}

bool readEntryDirectory(StringView directory, const Vfs::Context &context, PanelEntry *entries, size_t &count,
                        size_t max, PanelEntryReport report, void *arg, bool allFiles) {
        if (directory == Vfs::AppsPath) {
                char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxEntryBytes));
                if (text == nullptr) return true;
                readApps(context, entries, count, max, text);
                Memory::free(text);
                return true; // always there: installed applications come and go on their own
        }
        auto [dir, err] = Vfs::openDirectory(directory, context);
        if (err.isError()) return false;
        char *text = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxEntryBytes));
        if (text == nullptr) return true;
        for (;;) {
                auto [info, nerr] = dir.next();
                if (nerr.isError()) break;
                const StringView name = info.name.view();
                const bool       launcher = info.type == FileType::File && name.endsWith(StringView(".desktop"));
                const bool       other = allFiles && !launcher && !name.startsWith('.') &&
                                   (info.type == FileType::File || info.type == FileType::Directory);
                if ((!launcher && !other) || name.size() >= sizeof(PanelEntry::file)) continue;
                // The first directory's file of a name wins (Hidden=true there hides it).
                bool seen = false;
                for (size_t i = 0; i < count; ++i) seen = seen || StringView(entries[i].file) == name;
                if (seen || count == max) continue;
                String path;
                if (path.append(directory).isError() || path.append('/').isError() || path.append(name).isError())
                        continue;
                if (other) {
                        // Another file: its name and type (devplan/filetypes.md T10).
                        auto [type, terr] = Vfs::identify(path.view(), 0, context);
                        if (terr.isError() || path.size() >= sizeof(PanelEntry::path)) continue;
                        PanelEntry &e = entries[count++];
                        e = PanelEntry();
                        std::memcpy(e.file, name.data(), name.size());
                        const size_t nameBytes = name.size() < DesktopEntry::MaxName ? name.size() : 0;
                        std::memcpy(e.entry.name, name.data(), nameBytes); // sorted by it
                        std::memcpy(e.path, path.data(), path.size());
                        const size_t typeBytes = type.size() < sizeof(e.type) ? type.size() : 0;
                        std::memcpy(e.type, type.data(), typeBytes);
                        if (typeBytes == 0) std::memcpy(e.type, "application/octet-stream", 25);
                        continue;
                }
                size_t got = 0;
                if (!readText(path.view(), context, text, got)) continue;
                auto [entry, perr] = DesktopEntry::parse(StringView(text, got));
                if (perr.isError()) {
                        if (perr != Error::NotSupported && report != nullptr) report(arg, path.cStr());
                        continue;
                }
                PanelEntry &e = entries[count++]; // a hidden one too: it hides the later ones of its name
                std::memcpy(e.file, name.data(), name.size());
                e.file[name.size()] = '\0';
                const size_t n = path.size() < sizeof(e.path) ? path.size() : 0; // too long: none
                std::memcpy(e.path, path.data(), n);
                e.path[n] = '\0';
                e.entry = entry;
        }
        Memory::free(text);
        return true;
}

void keepShownEntries(PanelEntry *entries, size_t &count) {
        size_t n = 0;
        for (size_t i = 0; i < count; ++i)
                if (!entries[i].entry.hidden && !entries[i].entry.noDisplay) entries[n++] = entries[i];
        count = n;
        for (size_t i = 1; i < count; ++i) {
                const PanelEntry e = entries[i];
                size_t           k = i;
                for (; k > 0 && std::strcmp(entries[k - 1].entry.name, e.entry.name) > 0; --k)
                        entries[k] = entries[k - 1];
                entries[k] = e;
        }
        return;
}

BMDS_NAMESPACE_END
