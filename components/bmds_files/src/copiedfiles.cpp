/**
 * @file      copiedfiles.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The files a file manager put on the clipboard (devplan/filebrowser.md §4j, FB11).
 */

#include <algorithm>
#include <bmds/clipboardtype.h>
#include <bmds/copiedfiles.h>
#include <bmds/filenames.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The next line of @p rest (LF or CRLF ended), @p rest moved past it.
        StringView nextLine(StringView &rest) {
                const size_t end = rest.indexOf('\n');
                StringView   line = end == StringView::NPos ? rest : rest.left(end);
                rest = end == StringView::NPos ? StringView() : rest.mid(end + 1);
                if (line.endsWith('\r')) line = line.left(line.size() - 1);
                return line;
        }

        // `file://` and @p path percent-encoded.
        Error appendUri(String &out, StringView path) {
                char buffer[3 * Vfs::MaxPath + 1];
                auto [n, err] = Uri::encodePath(path, Span<char>(buffer, sizeof(buffer)));
                if (err.isOk()) err = out.append("file://");
                if (err.isOk()) err = out.append(StringView(buffer, n));
                return err;
        }

} // namespace

Error CopiedFiles::set(StringView paths, bool cut) {
        clear();
        Error err = _paths.assign(paths);
        if (err.isOk() && cut) {
                // Sorted once: views ask for each row they draw.
                StringView rest = _paths.view();
                while (err.isOk() && !rest.isEmpty()) {
                        const StringView line = nextLine(rest);
                        if (!line.isEmpty()) err = _sorted.append(line);
                }
                std::sort(_sorted.begin(), _sorted.end());
        }
        if (err.isError()) {
                clear();
                return err;
        }
        _cut = cut;
        return Error::Ok;
}

void CopiedFiles::clear() {
        _paths.clear();
        _sorted.clear();
        _cut = false;
        return;
}

bool CopiedFiles::isCut(StringView folder, StringView name) const {
        if (!_cut) return false;
        // @p folder, a / unless it ends with one, and @p name, compared with @p path as one string.
        const StringView slash = folder.endsWith('/') ? StringView() : StringView("/");
        const auto       compare = [&](StringView path) {
                const StringView parts[] = {folder, slash, name};
                size_t           at = 0;
                for (const StringView part : parts) {
                        for (const char c : part) {
                                if (at == path.size()) return -1; // path is shorter
                                const auto a = uint8_t(path[at++]), b = uint8_t(c);
                                if (a != b) return a < b ? -1 : 1;
                        }
                }
                return at == path.size() ? 0 : 1;
        };
        const StringView *at = std::lower_bound(_sorted.begin(), _sorted.end(), folder,
                                                [&](StringView path, StringView) { return compare(path) < 0; });
        return at != _sorted.end() && compare(*at) == 0;
}

bool CopiedFiles::isCut(StringView path) const {
        if (!_cut) return false;
        const StringView *at = std::lower_bound(_sorted.begin(), _sorted.end(), path);
        return at != _sorted.end() && *at == path;
}

Error CopiedFiles::render(StringView type, String &out) const {
        const bool gnome = ClipboardType::equal(type, GnomeFiles);
        if (!gnome && !ClipboardType::equal(type, UriList)) return out.assign(_paths.view());
        out.clear();
        Error err = gnome ? out.append(_cut ? "cut" : "copy") : Error(Error::Ok);
        // GNOME's lines end with LF, the next one starting each; a URI list's lines with CRLF.
        StringView rest = _paths.view();
        while (err.isOk() && !rest.isEmpty()) {
                const StringView path = nextLine(rest);
                if (path.isEmpty()) continue;
                if (gnome) err = out.append('\n');
                if (err.isOk()) err = appendUri(out, path);
                if (err.isOk() && !gnome) err = out.append("\r\n");
        }
        return err;
}

Error CopiedFiles::parse(StringView type, StringView data, bool &cut, String &paths) {
        cut = false;
        paths.clear();
        StringView rest = data;
        if (ClipboardType::equal(type, GnomeFiles)) cut = nextLine(rest).trimmed() == "cut";
        Error err;
        while (err.isOk() && !rest.isEmpty()) {
                StringView line = nextLine(rest).trimmed();
                if (line.isEmpty() || line.startsWith('#') || !line.startsWith("file://")) continue;
                line = line.mid(7); // file://HOST/PATH: the path
                const size_t slash = line.indexOf('/');
                if (slash == StringView::NPos) continue;
                char path[Vfs::MaxPath + 1];
                auto [n, derr] = Uri::decode(line.mid(slash), Span<char>(path, sizeof(path)));
                if (derr.isError() || n == 0) continue;
                if (!paths.isEmpty()) err = paths.append('\n');
                if (err.isOk()) err = paths.append(StringView(path, n));
        }
        if (err.isError()) return err;
        return paths.isEmpty() ? Error(Error::Empty) : Error(Error::Ok);
}

BMDS_NAMESPACE_END
