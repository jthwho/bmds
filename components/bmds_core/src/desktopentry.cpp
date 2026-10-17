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
                } else if (key == "MimeType") {
                        // Kept to the last whole type that fits: a long list isn't the entry's fault.
                        StringView list = item.value;
                        if (list.size() >= sizeof(e.mimeTypes)) {
                                list = list.left(sizeof(e.mimeTypes) - 1);
                                const size_t semi = list.lastIndexOf(';');
                                list = semi == StringView::NPos ? StringView() : list.left(semi + 1);
                        }
                        std::memcpy(e.mimeTypes, list.data(), list.size());
                        e.mimeTypes[list.size()] = '\0';
                        continue;
                } else if (key == "Terminal") {
                        flag = &e.terminal;
                } else if (key == "NoDisplay") {
                        flag = &e.noDisplay;
                } else if (key == "Hidden") {
                        flag = &e.hidden;
                } else if (key == "X-BMDS-Id") {
                        out = e.id;
                        outBytes = sizeof(e.id);
                } else if (key == "X-BMDS-Command") {
                        out = e.command;
                        outBytes = sizeof(e.command);
                } else if (key == "X-BMDS-Display") {
                        if (item.value == "Window") {
                                e.display = Display::Window;
                        } else if (item.value == "FullScreen") {
                                e.display = Display::FullScreen;
                        } else {
                                complain(warn, warnArg, item.line, "not Window or FullScreen");
                        }
                        continue;
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
        const StringView one[] = {file};
        return splitExec(exec, buffer, bufferBytes, args,
                         file.isEmpty() ? Span<const StringView>() : Span<const StringView>(one));
}

Result<size_t> DesktopEntry::splitExec(StringView exec, char *buffer, size_t bufferBytes, Span<StringView> args,
                                       Span<const StringView> files) {
        size_t count = 0;
        size_t used = 0;
        size_t i = 0;
        bool   placed = files.isEmpty(); // the files put in by a field code (or none to put)
        // A whole argument @p text, copied.
        const auto add = [&](StringView text) -> Error {
                if (count == args.size() || used + text.size() + 1 > bufferBytes) return Error::BufferTooSmall;
                std::memcpy(buffer + used, text.data(), text.size());
                args[count++] = StringView(buffer + used, text.size());
                used += text.size();
                buffer[used++] = '\0';
                return Error::Ok;
        };
        for (;;) {
                while (i < exec.size() && (exec[i] == ' ' || exec[i] == '\t')) ++i;
                if (i == exec.size()) break;
                const size_t start = used;
                bool         fieldOnly = true; // an argument that was only field codes goes
                bool         any = false;
                bool         quoted = false;
                bool         listed = false; // %F or %U alone: each file an argument of its own
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
                                if (code != '%' && !files.isEmpty() && StringView("fFuU").contains(code)) {
                                        placed = true;
                                        any = true;
                                        const bool alone = used == start && (code == 'F' || code == 'U') &&
                                                           (i == exec.size() || exec[i] == ' ' || exec[i] == '\t');
                                        if (alone) {
                                                for (const StringView f : files) {
                                                        const Error err = add(f);
                                                        if (err.isError()) return makeError<size_t>(err);
                                                }
                                                listed = true;
                                                continue;
                                        }
                                        if (used + files[0].size() + 1 > bufferBytes)
                                                return makeError<size_t>(Error::BufferTooSmall);
                                        for (const char c : files[0]) buffer[used++] = c;
                                        fieldOnly = false;
                                        continue;
                                }
                                if (code != '%') {
                                        any = true;
                                        continue; // a field code: nothing to pass
                                }
                        }
                        if (used + 1 >= bufferBytes) return makeError<size_t>(Error::BufferTooSmall);
                        buffer[used++] = ch;
                        fieldOnly = false;
                        any = true;
                }
                if (quoted) return makeError<size_t>(Error::Invalid);
                if (listed) continue;
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
                // No field code for them: the files last.
                for (const StringView f : files) {
                        const Error err = add(f);
                        if (err.isError()) return makeError<size_t>(err);
                }
        }
        return makeResult(count);
}

bool DesktopEntry::takesFileList(StringView exec) {
        bool single = false;
        for (size_t i = 0; i + 1 < exec.size(); ++i) {
                if (exec[i] != '%') continue;
                const char code = exec[++i];
                if (code == 'F' || code == 'U') return true;
                if (code == 'f' || code == 'u') single = true;
        }
        return !single;
}

bool DesktopEntry::hasMimeType(StringView mimeTypes, StringView type) {
        const size_t slash = type.indexOf('/');
        for (StringView item = IniReader::nextListItem(mimeTypes); !item.isEmpty();
             item = IniReader::nextListItem(mimeTypes)) {
                if (item.equalsIgnoreCase(type)) return true;
                if (slash != StringView::NPos && item.size() == slash + 2 && item.endsWith("/*") &&
                    item.left(slash).equalsIgnoreCase(type.left(slash)))
                        return true;
        }
        return false;
}

bool DesktopEntry::hasCategory(StringView categories, StringView category) {
        for (StringView item = IniReader::nextListItem(categories); !item.isEmpty();
             item = IniReader::nextListItem(categories))
                if (item == category) return true;
        return false;
}

BMDS_NAMESPACE_END
