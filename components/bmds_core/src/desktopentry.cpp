/**
 * @file      desktopentry.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/desktopentry.h>
#include <bmds/inireader.h>

BMDS_NAMESPACE_BEGIN

namespace {

        constexpr StringView Group = "Desktop Entry";

        void complain(DesktopEntry::Warn warn, void *arg, uint32_t line, const char *message) {
                if (warn != nullptr) warn(arg, line, message);
                return;
        }

} // namespace

Result<DesktopEntry> DesktopEntry::parse(StringView text, Warn warn, void *warnArg) {
        DesktopEntry e;
        IniReader    reader(text);
        bool         inGroup = false;
        bool         seenGroup = false;
        bool         hasName = false;
        bool         hasExec = false;
        for (IniReader::Item item = reader.next(); item.kind != IniReader::Kind::End; item = reader.next()) {
                if (item.kind == IniReader::Kind::Invalid) {
                        complain(warn, warnArg, item.line, "not a group or a key=value line");
                        continue;
                }
                if (item.kind == IniReader::Kind::Group) {
                        inGroup = item.name == Group;
                        seenGroup = seenGroup || inGroup;
                        continue;
                }
                if (!inGroup) continue;
                const StringView key = item.key;
                char            *out = nullptr;
                size_t           outBytes = 0;
                bool            *flag = nullptr;
                if (key == "Type") {
                        if (item.value != "Application") return makeError<DesktopEntry>(Error::NotSupported);
                        continue;
                } else if (key == "Name") {
                        out = e.name;
                        outBytes = sizeof(e.name);
                        hasName = true;
                } else if (key == "Comment") {
                        out = e.comment;
                        outBytes = sizeof(e.comment);
                } else if (key == "Exec") {
                        out = e.exec;
                        outBytes = sizeof(e.exec);
                        hasExec = true;
                } else if (key == "Icon") {
                        out = e.icon;
                        outBytes = sizeof(e.icon);
                } else if (key == "Categories") {
                        out = e.categories;
                        outBytes = sizeof(e.categories);
                } else if (key == "Terminal") {
                        flag = &e.terminal;
                } else if (key == "NoDisplay") {
                        flag = &e.noDisplay;
                } else if (key == "Hidden") {
                        flag = &e.hidden;
                } else {
                        continue; // localized (Name[de]) and keys not used
                }
                if (flag != nullptr) {
                        if (IniReader::toBool(item.value, *flag).isError())
                                complain(warn, warnArg, item.line, "not a boolean");
                        continue;
                }
                // Categories keep their \; (a list); the others are strings.
                if (out == e.categories) {
                        if (item.value.size() >= outBytes) return makeError<DesktopEntry>(Error::BufferTooSmall);
                        for (size_t i = 0; i < item.value.size(); ++i) out[i] = item.value[i];
                        out[item.value.size()] = '\0';
                } else {
                        const Error err = IniReader::unescape(item.value, out, outBytes);
                        if (err.isError()) return makeError<DesktopEntry>(err);
                }
        }
        if (!seenGroup || (!e.hidden && (!hasName || !hasExec || e.name[0] == '\0' || e.exec[0] == '\0')))
                return makeError<DesktopEntry>(Error::Invalid);
        return makeResult(e);
}

Result<size_t> DesktopEntry::splitExec(StringView exec, char *buffer, size_t bufferBytes, Span<StringView> args,
                                       StringView file) {
        size_t count = 0;
        size_t used = 0;
        size_t i = 0;
        bool   placed = file.isEmpty(); // the file put in by a field code (or none to put)
        for (;;) {
                while (i < exec.size() && (exec[i] == ' ' || exec[i] == '\t')) ++i;
                if (i == exec.size()) break;
                const size_t start = used;
                bool         fieldOnly = true; // an argument that was only field codes goes
                bool         any = false;
                bool         quoted = false;
                while (i < exec.size() && (quoted || (exec[i] != ' ' && exec[i] != '\t'))) {
                        char ch = exec[i++];
                        if (ch == '"') {
                                quoted = !quoted;
                                fieldOnly = false;
                                any = true;
                                continue;
                        }
                        if (quoted && ch == '\\' && i < exec.size()) {
                                ch = exec[i++];
                        } else if (!quoted && ch == '%' && i < exec.size()) {
                                const char code = exec[i++];
                                if (code != '%' && !file.isEmpty() && StringView("fFuU").contains(code)) {
                                        if (used + file.size() + 1 > bufferBytes)
                                                return makeError<size_t>(Error::BufferTooSmall);
                                        for (const char c : file) buffer[used++] = c;
                                        placed = true;
                                        fieldOnly = false;
                                        any = true;
                                        continue;
                                }
                                if (code != '%') {
                                        any = true;
                                        continue; // a field code: nothing to pass yet
                                }
                        }
                        if (used + 1 >= bufferBytes) return makeError<size_t>(Error::BufferTooSmall);
                        buffer[used++] = ch;
                        fieldOnly = false;
                        any = true;
                }
                if (quoted) return makeError<size_t>(Error::Invalid);
                if (!any || fieldOnly) {
                        used = start;
                        continue;
                }
                if (count == args.size() || used + 1 > bufferBytes) return makeError<size_t>(Error::BufferTooSmall);
                args[count++] = StringView(buffer + start, used - start);
                buffer[used++] = '\0';
        }
        if (count == 0) return makeError<size_t>(Error::Invalid);
        if (!placed) {
                // No field code for it: the file last.
                if (count == args.size() || used + file.size() + 1 > bufferBytes)
                        return makeError<size_t>(Error::BufferTooSmall);
                std::memcpy(buffer + used, file.data(), file.size());
                args[count++] = StringView(buffer + used, file.size());
                buffer[used + file.size()] = '\0';
        }
        return makeResult(count);
}

bool DesktopEntry::hasCategory(StringView categories, StringView category) {
        for (StringView item = IniReader::nextListItem(categories); !item.isEmpty();
             item = IniReader::nextListItem(categories))
                if (item == category) return true;
        return false;
}

BMDS_NAMESPACE_END
