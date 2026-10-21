/**
 * @file      netmountlist.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `net/mounts`' entries (devplan/netfs.md step 4): parsed and checked, portable (host-tested).
 */

#include <bmds/netmounts.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The programs that mount a network filesystem.
        constexpr StringView MountPrograms[] = {"sshfs"};

        bool isSpace(char c) {
                return c == ' ' || c == '\t';
        }

        StringView trimmed(StringView s) {
                while (!s.isEmpty() && isSpace(s[0])) s = s.mid(1);
                while (!s.isEmpty() && isSpace(s[s.size() - 1])) s = s.left(s.size() - 1);
                return s;
        }

        Error split(StringView text, NetMounts::Entry &e) {
                e.text = text;
                e.count = 0;
                for (size_t at = 0; at < text.size();) {
                        if (isSpace(text[at])) {
                                ++at;
                                continue;
                        }
                        size_t end = at;
                        while (end < text.size() && !isSpace(text[end])) ++end;
                        if (e.count == NetMounts::MaxWords) return Error::Full;
                        e.words[e.count++] = text.mid(at, end - at);
                        at = end;
                }
                bool known = false;
                for (StringView p : MountPrograms) known |= e.count > 0 && e.words[0] == p;
                // The program, at least a source and a mount point, the point absolute (not "/").
                if (!known || e.count < 3 || !e.mountPoint().startsWith('/') || e.mountPoint().size() < 2)
                        return Error::Invalid;
                return Error::Ok;
        }

} // namespace

Error NetMounts::parse(StringView value, Span<Entry> entries, size_t &count) {
        count = 0;
        while (!value.isEmpty()) {
                const size_t     semi = value.indexOf(';');
                const StringView text = trimmed(semi == StringView::NPos ? value : value.left(semi));
                value = semi == StringView::NPos ? StringView() : value.mid(semi + 1);
                if (text.isEmpty()) continue;
                if (count == entries.size()) return Error::Full;
                Entry      &e = entries[count];
                const Error err = split(text, e);
                if (err.isError()) return err;
                for (size_t i = 0; i < count; ++i)
                        if (entries[i].mountPoint() == e.mountPoint()) return Error::Invalid;
                ++count;
        }
        return Error::Ok;
}

Error NetMounts::check(StringView value) {
        Entry  entries[MaxEntries];
        size_t count = 0;
        return parse(value, Span<Entry>(entries, MaxEntries), count);
}

BMDS_NAMESPACE_END
