/**
 * @file      pkgindex.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/detail/pkgtext.h>
#include <bmds/pkgfile.h>
#include <bmds/pkgindex.h>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgversion.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The fields a stanza has, as bits: each once at most.
        enum Field : uint32_t {
                Package = 1u << 0,
                Version = 1u << 1,
                Architecture = 1u << 2,
                Abi = 1u << 3,
                Size = 1u << 4,
                InstalledSize = 1u << 5,
                Hash = 1u << 6,
                File = 1u << 7,
                Depends = 1u << 8,
                Category = 1u << 9,
                Description = 1u << 10,
                Title = 1u << 11,
                Summary = 1u << 12,
                License = 1u << 13,
                Homepage = 1u << 14,
        };

        constexpr uint32_t Required = Package | Version | Architecture | Abi | Size | Hash | File;

        struct Name {
                        const char *text;
                        Field       field;
        };

        constexpr Name Names[] = {
                {"Package", Package},
                {"Version", Version},
                {"Architecture", Architecture},
                {"ABI", Abi},
                {"Size", Size},
                {"Installed-Size", InstalledSize},
                {"BLAKE2s", Hash},
                {"File", File},
                {"Depends", Depends},
                {"Category", Category},
                {"Description", Description},
                {"Title", Title},
                {"Summary", Summary},
                {"License", License},
                {"Homepage", Homepage},
        };

        bool isKey(StringView key) {
                if (key.isEmpty() || key.size() > 32) return false;
                for (char c : key) {
                        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                                return false;
                }
                return true;
        }

        bool isFile(StringView file) {
                if (file.isEmpty() || file.size() > PkgIndex::MaxFileBytes) return false;
                for (char c : file) {
                        if (c <= 0x20 || c >= 0x7f) return false;
                }
                return true;
        }

        bool isDependsList(StringView list) {
                PkgDepends::Reader     reader(list);
                PkgDepends::Dependency d;
                Error                  err;
                while ((err = reader.next(d)).isOk()) {}
                return err.code() == Error::EndOfFile;
        }

        // Sets @p out's field @p field from @p value: false if the value isn't one.
        bool setField(PkgIndex::Stanza &out, Field field, StringView value) {
                uint64_t n = 0;
                switch (field) {
                        case Package: out.package = value; return PkgFile::isValidName(value);
                        case Version: out.version = value; return PkgVersion::isValid(value);
                        case Architecture: out.architecture = value; return PkgFile::isValidArch(value);
                        case Abi:
                                if (!Detail::PkgText::parseDecimal(value, n) || n > 0xffffffffu) return false;
                                out.abi = uint32_t(n);
                                return true;
                        case Size: return Detail::PkgText::parseDecimal(value, out.size);
                        case InstalledSize: return Detail::PkgText::parseDecimal(value, out.installedSize);
                        case Hash: return Detail::PkgText::parseHash(value, out.hash);
                        case File: out.file = value; return isFile(value);
                        case Depends: out.depends = value; return isDependsList(value);
                        case Category: out.category = value; return isKey(value); // as a key is spelled
                        case Description: out.description = value; return true;
                        case Title: out.title = value; return !value.isEmpty();
                        case Summary: out.summary = value; return true;
                        case License: out.license = value; return !value.isEmpty();
                        case Homepage: out.homepage = value; return isFile(value);
                }
                return false;
        }

} // namespace

Error PkgIndex::Reader::next(Stanza &out) {
        // Empty lines before the stanza.
        while (_pos < _text.size() && _text[_pos] == '\n') {
                ++_pos;
                ++_line;
        }
        if (_pos >= _text.size()) return Error::EndOfFile;
        out = Stanza{};
        uint32_t seen = 0;
        while (_pos < _text.size()) {
                size_t end = _text.indexOf('\n', _pos);
                if (end == StringView::NPos) end = _text.size();
                const StringView line = _text.mid(_pos, end - _pos);
                _pos = end + 1;
                ++_line;
                if (line.isEmpty()) break;
                if (!PkgManifest::isCleanText(line)) return Error::CorruptData;
                const size_t colon = line.indexOf(':');
                if (colon == StringView::NPos) return Error::CorruptData;
                const StringView key = line.left(colon);
                if (!isKey(key)) return Error::CorruptData;
                const StringView value = line.mid(colon + 1).trimmed();
                for (const Name &n : Names) {
                        if (key != StringView(n.text)) continue;
                        if ((seen & n.field) != 0 || !setField(out, n.field, value)) return Error::CorruptData;
                        seen |= n.field;
                        break;
                }
        }
        return (seen & Required) == Required ? Error::Ok : Error::CorruptData;
}

BMDS_NAMESPACE_END
