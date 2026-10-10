/**
 * @file      fileformat.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * What a file is, by its name or its first bytes (devplan/filetypes.md).
 */

#include <cstring>
#include <bmds/fileformat.h>
#include <bmds/utf8.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A type's kinds.
        enum : uint8_t {
                Text = 0x1,    // text-like (`isText()`)
                Whole = 0x2,   // the name is a whole file name, not an extension
                Archive = 0x4, // its icon is a package's
        };

        struct Name {
                        const char *name; // lowercase
                        const char *type;
                        uint8_t     kinds;
        };

        // By extension (or whole name), freedesktop.org's types. Scripts and data formats are
        // text-like, as freedesktop.org makes them subclasses of text/plain.
        constexpr Name Names[] = {
                {"txt", "text/plain", Text},
                {"text", "text/plain", Text},
                {"log", "text/x-log", Text},
                {"md", "text/markdown", Text},
                {"csv", "text/csv", Text},
                {"html", "text/html", Text},
                {"htm", "text/html", Text},
                {"css", "text/css", Text},
                {"c", "text/x-csrc", Text},
                {"h", "text/x-chdr", Text},
                {"cpp", "text/x-c++src", Text},
                {"cc", "text/x-c++src", Text},
                {"cxx", "text/x-c++src", Text},
                {"hpp", "text/x-c++hdr", Text},
                {"s", "text/x-asm", Text},
                {"py", "text/x-python", Text},
                {"lua", "text/x-lua", Text},
                {"diff", "text/x-patch", Text},
                {"patch", "text/x-patch", Text},
                {"cmake", "text/x-cmake", Text},
                {"makefile", "text/x-makefile", Text | Whole},
                {"cmakelists.txt", "text/x-cmake", Text | Whole},
                {"readme", "text/plain", Text | Whole},
                {"conf", "text/plain", Text},
                {"cfg", "text/plain", Text},
                {"ini", "text/plain", Text},
                {"list", "text/plain", Text},
                {"sh", "application/x-shellscript", Text},
                {"js", "text/javascript", Text},
                {"json", "application/json", Text},
                {"xml", "application/xml", Text},
                {"toml", "application/toml", Text},
                {"yaml", "application/x-yaml", Text},
                {"yml", "application/x-yaml", Text},
                {"desktop", "application/x-desktop", Text},
                {"trashinfo", "application/x-trash", Text},
                {"png", "image/png", 0},
                {"jpg", "image/jpeg", 0},
                {"jpeg", "image/jpeg", 0},
                {"gif", "image/gif", 0},
                {"bmp", "image/bmp", 0},
                {"svg", "image/svg+xml", 0},
                {"ico", "image/vnd.microsoft.icon", 0},
                {"wav", "audio/x-wav", 0},
                {"mp3", "audio/mpeg", 0},
                {"ogg", "audio/ogg", 0},
                {"flac", "audio/flac", 0},
                {"mid", "audio/midi", 0},
                {"midi", "audio/midi", 0},
                {"m3u", "audio/x-mpegurl", Text},
                {"m3u8", "audio/x-mpegurl", Text},
                {"mp4", "video/mp4", 0},
                {"avi", "video/x-msvideo", 0},
                {"mkv", "video/x-matroska", 0},
                {"pdf", "application/pdf", 0},
                {"zip", "application/zip", Archive},
                {"gz", "application/gzip", Archive},
                {"tgz", "application/x-compressed-tar", Archive},
                {"tar", "application/x-tar", Archive},
                {"bz2", "application/x-bzip2", Archive},
                {"xz", "application/x-xz", Archive},
                {"elf", "application/x-executable", 0},
                {"bin", "application/octet-stream", 0},
        };

        // Text-like types found only by content.
        constexpr const char *TextTypes[] = {"application/x-zerosize", "application/x-shellscript"};

        struct Magic {
                        uint16_t    offset;
                        const char *bytes;
                        uint8_t     length;
                        const char *type;
        };

        // Magic numbers, the longer before the shorter they start.
        constexpr Magic Magics[] = {
                {0,
                 "\x7f"
                 "ELF",
                 4, "application/x-executable"},
                {0, "\x89PNG\r\n\x1a\n", 8, "image/png"},
                {0, "\xff\xd8\xff", 3, "image/jpeg"},
                {0, "GIF87a", 6, "image/gif"},
                {0, "GIF89a", 6, "image/gif"},
                {0, "%PDF-", 5, "application/pdf"},
                {0, "PK\x03\x04", 4, "application/zip"},
                {0, "\x1f\x8b", 2, "application/gzip"},
                {0,
                 "\xfd"
                 "7zXZ\0",
                 6, "application/x-xz"},
                {0, "BZh", 3, "application/x-bzip2"},
                {0, "OggS", 4, "audio/ogg"},
                {0, "fLaC", 4, "audio/flac"},
                {0, "MThd", 4, "audio/midi"},
                {0, "ID3", 3, "audio/mpeg"},
                {0, "[Desktop Entry]", 15, "application/x-desktop"},
                {0, "<?xml", 5, "application/xml"},
                {257, "ustar", 5, "application/x-tar"},
        };

        bool startsAt(Span<const uint8_t> head, size_t offset, const char *bytes, size_t length) {
                return head.size() >= offset + length && std::memcmp(head.data() + offset, bytes, length) == 0;
        }

        // The bytes of the MPEG audio frame whose header is at @p h, or 0 if it isn't one (or free format).
        size_t mpegFrameBytes(const uint8_t *h) {
                static constexpr uint16_t Kbps[2][3][15] = {
                        {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
                         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
                         {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
                        {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
                         {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
                         {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
                };
                static constexpr uint16_t Rates[3] = {44100, 48000, 32000};
                const unsigned            version = (h[1] >> 3) & 3, layer = 4 - ((h[1] >> 1) & 3);
                const unsigned            index = h[2] >> 4, rateIndex = (h[2] >> 2) & 3;
                if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0 || version == 1 || layer == 4 || index == 0 || index == 15 ||
                    rateIndex == 3)
                        return 0;
                const unsigned rate = Rates[rateIndex] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
                const unsigned kbps = Kbps[version == 3 ? 1 : 0][layer - 1][index];
                const unsigned padding = (h[2] >> 1) & 1;
                if (layer == 1) return (12 * kbps * 1000 / rate + padding) * 4;
                const unsigned samples = layer == 3 && version != 3 ? 576 : 1152;
                return samples / 8 * kbps * 1000 / rate + padding;
        }

        // MPEG audio without an ID3 tag: a frame header, and the next one where the first frame ends
        // (when the head reaches that far), of the same version, layer and rate.
        bool looksLikeMpegAudio(Span<const uint8_t> head) {
                if (head.size() < 4) return false;
                const size_t bytes = mpegFrameBytes(head.data());
                if (bytes == 0) return false;
                if (bytes + 4 > head.size()) return true;
                const uint8_t *next = head.data() + bytes;
                return mpegFrameBytes(next) != 0 && (next[1] & 0xfe) == (head[1] & 0xfe) &&
                       (next[2] & 0x0c) == (head[2] & 0x0c);
        }

        bool startsIgnoringCase(Span<const uint8_t> head, StringView with) {
                if (head.size() < with.size()) return false;
                return StringView(reinterpret_cast<const char *>(head.data()), with.size()).equalsIgnoreCase(with);
        }

        // Text: valid UTF-8 (a character cut at the end allowed unless @p whole), no NUL, and only
        // the controls text has (tab, LF, CR, form feed, backspace, escape).
        bool looksLikeText(Span<const uint8_t> head, bool whole) {
                const StringView text(reinterpret_cast<const char *>(head.data()), head.size());
                size_t           at = 0;
                while (at < text.size()) {
                        const uint8_t c = head[at];
                        if (c < 0x80) {
                                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\b' &&
                                    c != 0x1b)
                                        return false;
                                if (c == 0x7f) return false;
                                ++at;
                                continue;
                        }
                        const size_t   start = at;
                        const uint32_t cp = Utf8::decode(text, at);
                        if (cp == Utf8::Replacement && at == start + 1) {
                                // Malformed (a U+FFFD written out takes three bytes), unless a character
                                // cut at the buffer's end that the file goes on with.
                                const size_t need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 0;
                                bool         cut = !whole && need != 0 && start + need > text.size();
                                for (size_t i = start + 1; cut && i < text.size(); ++i)
                                        cut = Utf8::isContinuation(head[i]);
                                return cut;
                        }
                }
                return true;
        }

} // namespace

namespace FileFormat {

        StringView byName(StringView name) {
                const size_t slash = name.lastIndexOf('/');
                if (slash != StringView::NPos) name = name.mid(slash + 1);
                const size_t     dot = name.lastIndexOf('.');
                const StringView extension = dot != StringView::NPos && dot > 0 ? name.mid(dot + 1) : StringView();
                // A whole name first (CMakeLists.txt isn't any .txt), then the extension.
                for (const bool whole : {true, false}) {
                        const StringView key = whole ? name : extension;
                        if (key.isEmpty()) continue;
                        for (const Name &n : Names)
                                if (((n.kinds & Whole) != 0) == whole && key.equalsIgnoreCase(n.name))
                                        return StringView(n.type);
                }
                return StringView();
        }

        StringView byContent(Span<const uint8_t> head, bool whole) {
                if (head.size() == 0) return whole ? Empty : Binary;
                for (const Magic &m : Magics)
                        if (startsAt(head, m.offset, m.bytes, m.length)) return StringView(m.type);
                // RIFF's kind is at 8.
                if (startsAt(head, 0, "RIFF", 4)) {
                        if (startsAt(head, 8, "WAVE", 4)) return "audio/x-wav";
                        if (startsAt(head, 8, "AVI ", 4)) return "video/x-msvideo";
                }
                if (looksLikeMpegAudio(head)) return "audio/mpeg";
                // BMP: "BM", then reserved words that are zero.
                if (startsAt(head, 0, "BM", 2) && startsAt(head, 6, "\0\0\0\0", 4)) return "image/bmp";
                if (!looksLikeText(head, whole)) return Binary;
                // Text: a script by its interpreter, HTML by its start.
                if (startsAt(head, 0, "#!", 2)) {
                        size_t end = 2;
                        while (end < head.size() && head[end] != '\n') ++end;
                        const StringView line(reinterpret_cast<const char *>(head.data()), end);
                        if (line.contains(StringView("python"))) return "text/x-python";
                        if (line.contains(StringView("lua"))) return "text/x-lua";
                        return "application/x-shellscript";
                }
                if (startsIgnoringCase(head, "<!doctype html") || startsIgnoringCase(head, "<html")) return "text/html";
                return PlainText;
        }

        bool isText(StringView type) {
                if (type.startsWith("text/")) return true;
                for (const Name &n : Names)
                        if ((n.kinds & Text) != 0 && type == StringView(n.type)) return true;
                for (const char *t : TextTypes)
                        if (type == StringView(t)) return true;
                return false;
        }

        StringView iconName(StringView type, Span<char> out) {
                if (type.size() + 1 > out.size()) return StringView();
                for (size_t i = 0; i < type.size(); ++i) out[i] = type[i] == '/' ? '-' : type[i];
                out[type.size()] = '\0';
                return StringView(out.data(), type.size());
        }

        StringView genericIcon(StringView type) {
                if (type == Directory) return "folder";
                if (type == StringView("application/x-executable")) return "application-x-executable";
                for (const Name &n : Names)
                        if ((n.kinds & Archive) != 0 && type == StringView(n.type)) return "package-x-generic";
                if (type.startsWith("image/")) return "image-x-generic";
                if (type.startsWith("audio/")) return "audio-x-generic";
                if (type.startsWith("video/")) return "video-x-generic";
                if (isText(type)) return "text-x-generic";
                return "unknown";
        }

} // namespace FileFormat

BMDS_NAMESPACE_END
