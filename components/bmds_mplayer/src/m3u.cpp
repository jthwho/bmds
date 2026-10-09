/**
 * @file      m3u.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include "m3u.h"

#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The next line of @p text from @p at (moved past it), without its CR LF.
        StringView nextLine(StringView text, size_t &at) {
                const size_t start = at;
                while (at < text.size() && text[at] != '\n') ++at;
                StringView line = text.mid(start, at - start);
                if (at < text.size()) ++at;
                if (line.endsWith('\r')) line = line.left(line.size() - 1);
                return line;
        }

        int hexValue(char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
        }

        // A list's line as a path: backslashes as slashes; a file:// URL's path, %XX decoded.
        // False for another URL.
        Error toPath(StringView line, String &out) {
                out.clear();
                bool url = false;
                if (line.size() > 7 && line.left(7).equalsIgnoreCase("file://")) {
                        line = line.mid(7);
                        url = true;
                } else if (line.contains("://")) {
                        return Error::NotSupported;
                }
                for (size_t i = 0; i < line.size(); ++i) {
                        char c = line[i];
                        if (c == '\\') c = '/';
                        if (url && c == '%' && i + 2 < line.size() && hexValue(line[i + 1]) >= 0 &&
                            hexValue(line[i + 2]) >= 0) {
                                c = static_cast<char>(hexValue(line[i + 1]) * 16 + hexValue(line[i + 2]));
                                i += 2;
                        }
                        const Error err = out.append(c);
                        if (err.isError()) return err;
                }
                return Error::Ok;
        }

        // #EXTINF:length[ attributes],Artist - Title
        void parseExtInf(StringView rest, Playlist::Info &info) {
                bool   quoted = false;
                size_t comma = rest.size();
                for (size_t i = 0; i < rest.size(); ++i) {
                        if (rest[i] == '"') quoted = !quoted;
                        if (rest[i] == ',' && !quoted) {
                                comma = i;
                                break;
                        }
                }
                const StringView length = rest.left(comma).trimmed();
                uint32_t         seconds = 0;
                bool             digits = false;
                for (const char c : length) {
                        if (c < '0' || c > '9') break;
                        if (seconds < 1000000) seconds = seconds * 10 + static_cast<uint32_t>(c - '0');
                        digits = true;
                }
                info.lengthMs = digits ? seconds * 1000 : 0;
                const StringView name = comma < rest.size() ? rest.mid(comma + 1).trimmed() : StringView();
                const size_t     dash = name.indexOf(" - ");
                if (dash != StringView::NPos) {
                        info.artist = name.left(dash).trimmed();
                        info.title = name.mid(dash + 3).trimmed();
                } else {
                        info.title = name;
                }
                return;
        }

} // namespace

namespace M3u {

        Result<ReadResult> read(StringView text, StringView folder, Playlist &playlist) {
                ReadResult     result;
                Playlist::Info hint;
                bool           hinted = false;
                String         path(MemSpace::Psram);
                size_t         at = 0;
                if (text.startsWith("\xEF\xBB\xBF")) at = 3;
                while (at < text.size()) {
                        const StringView line = nextLine(text, at).trimmed();
                        if (line.isEmpty()) continue;
                        if (line.startsWith('#')) {
                                if (line.size() > 8 && line.left(8).equalsIgnoreCase("#EXTINF:")) {
                                        hint = Playlist::Info();
                                        parseExtInf(line.mid(8), hint);
                                        hinted = true;
                                }
                                continue;
                        }
                        Error err = toPath(line, path);
                        if (err == Error::NoMem) return makeError<ReadResult>(err);
                        auto [full, nerr] = err.isOk() ? Vfs::normalize(path.view(), folder) : makeError<String>(err);
                        if (nerr == Error::NoMem) return makeError<ReadResult>(nerr);
                        if (nerr.isError()) {
                                ++result.skipped;
                                hinted = false;
                                continue;
                        }
                        err = playlist.append(full.view());
                        if (err.isOk() && hinted) err = playlist.setInfo(playlist.count() - 1, hint);
                        if (err.isError()) return makeError<ReadResult>(err);
                        ++result.added;
                        hinted = false;
                }
                return makeResult(result);
        }

        Error write(const Playlist &playlist, StringView folder, String &out) {
                out.clear();
                Error err = out.append("#EXTM3U\n");
                // Under the folder: the part after "folder/".
                const size_t prefix = folder == "/" ? 1 : folder.size() + 1;
                for (size_t i = 0; err.isOk() && i < playlist.count(); ++i) {
                        const Playlist::Info info = playlist.info(i);
                        if (info.lengthMs != 0 || !info.title.isEmpty()) {
                                const long seconds =
                                        info.lengthMs != 0 ? static_cast<long>((info.lengthMs + 500) / 1000) : -1;
                                err = out.appendf("#EXTINF:%ld,", seconds);
                                if (err.isOk() && !info.artist.isEmpty()) err = out.append(info.artist);
                                if (err.isOk() && !info.artist.isEmpty() && !info.title.isEmpty())
                                        err = out.append(" - ");
                                if (err.isOk()) err = out.append(info.title);
                                if (err.isOk()) err = out.append('\n');
                        }
                        StringView path = playlist.path(i);
                        if (path.size() > prefix && path.startsWith(folder) && path[prefix - 1] == '/')
                                path = path.mid(prefix);
                        if (err.isOk()) err = out.append(path);
                        if (err.isOk()) err = out.append('\n');
                }
                if (err.isError()) out.clear();
                return err;
        }

} // namespace M3u

BMDS_NAMESPACE_END
