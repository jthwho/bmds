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
#include <bmds/memory.h>
#include <bmds/string.h>

#include "panelentries.h"

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr size_t MaxEntryBytes = 4096;

} // namespace

bool readEntryDirectory(StringView directory, const Vfs::Context &context, PanelEntry *entries, size_t &count,
                        size_t max, PanelEntryReport report, void *arg, bool allFiles) {
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
                auto [file, oerr] = Vfs::open(path.view(), Vfs::Read, context);
                size_t got = 0;
                while (oerr.isOk() && got < MaxEntryBytes) {
                        auto [n, rerr] = file->read(
                                WritableByteSpan(reinterpret_cast<uint8_t *>(text) + got, MaxEntryBytes - got),
                                WaitForever);
                        if (rerr.isError() || n == 0) break;
                        got += n;
                }
                if (oerr.isError()) continue;
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
