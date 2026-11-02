/**
 * @file      pkgrecord.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <bmds/pkgrecord.h>

#include <bmds/jsonreader.h>
#include <bmds/memory.h>

BMDS_NAMESPACE_BEGIN

namespace {

        struct TypeName {
                        const char     *name;
                        PkgRecord::Type type;
        };

        constexpr TypeName Types[] = {
                {"package", PkgRecord::Type::Package},   {"description", PkgRecord::Type::Description},
                {"file", PkgRecord::Type::File},         {"plan", PkgRecord::Type::Plan},
                {"progress", PkgRecord::Type::Progress}, {"done", PkgRecord::Type::Done},
                {"note", PkgRecord::Type::Note},         {"source", PkgRecord::Type::Source},
                {"error", PkgRecord::Type::Error},
        };

        PkgRecord::Type typeOf(StringView name) {
                for (const TypeName &t : Types)
                        if (name == StringView(t.name)) return t.type;
                return PkgRecord::Type::Other;
        }

} // namespace

void PkgRecord::clear() {
        *this = PkgRecord();
        return;
}

Error PkgRecord::parse(StringView line) {
        clear();
        // Strings up to MaxString, decoded into the heap: not on a worker's stack.
        char *scratch = static_cast<char *>(Memory::alloc(MemSpace::Psram, MaxString + 1));
        if (scratch == nullptr) return Error::NoMem;
        JsonReader json(line, scratch, MaxString + 1);
        Error      err = json.next() == JsonReader::Token::BeginObject ? Error() : Error(Error::CorruptData);
        String     key;
        while (err.isOk()) {
                const JsonReader::Token t = json.next();
                if (t == JsonReader::Token::EndObject) break;
                if (t != JsonReader::Token::Key) {
                        err = t == JsonReader::Token::Error ? json.error() : Error(Error::CorruptData);
                        break;
                }
                err = key.assign(json.text());
                if (err.isError()) break;
                const StringView        k = key.view();
                const JsonReader::Token v = json.next();
                if (v == JsonReader::Token::Error) {
                        err = json.error();
                        break;
                }
                if (v == JsonReader::Token::BeginObject || v == JsonReader::Token::BeginArray) {
                        err = json.skip(); // nothing pkg writes nests yet
                        continue;
                }
                if (v == JsonReader::Token::String) {
                        String *field = k == StringView("type")                                 ? nullptr
                                        : k == StringView("name") || k == StringView("package") ? &name
                                        : k == StringView("version")                            ? &version
                                        : k == StringView("from")                               ? &from
                                        : k == StringView("title")                              ? &title
                                        : k == StringView("summary")                            ? &summary
                                        : k == StringView("category")                           ? &category
                                        : k == StringView("depends")                            ? &depends
                                        : k == StringView("available")                          ? &available
                                        : k == StringView("signer")                             ? &signer
                                        : k == StringView("action")                             ? &action
                                        : k == StringView("step")                               ? &step
                                        : k == StringView("source")                             ? &source
                                        : k == StringView("error")                              ? &error
                                        : k == StringView("text")                               ? &text
                                                                                                : nullptr;
                        if (k == StringView("type")) type = typeOf(json.text());
                        if (field != nullptr) err = field->assign(json.text());
                } else if (v == JsonReader::Token::Number) {
                        uint64_t *field = k == StringView("size")            ? &size
                                          : k == StringView("installedSize") ? &installedSize
                                          : k == StringView("fetch")         ? &fetch
                                          : k == StringView("done")          ? &done
                                          : k == StringView("total")         ? &total
                                          : k == StringView("packages")      ? &packages
                                                                             : nullptr;
                        if (field != nullptr && json.toUint64(*field).isError()) *field = 0;
                } else if (v == JsonReader::Token::True || v == JsonReader::Token::False) {
                        bool *field = k == StringView("installed")    ? &installed
                                      : k == StringView("automatic")  ? &automatic
                                      : k == StringView("upgradable") ? &upgradable
                                                                      : nullptr;
                        if (field != nullptr) *field = v == JsonReader::Token::True;
                } else if (v != JsonReader::Token::Null) {
                        err = Error::CorruptData;
                }
        }
        // One object and nothing after it.
        if (err.isOk() && json.next() != JsonReader::Token::End) err = Error::CorruptData;
        Memory::free(scratch);
        return err;
}

BMDS_NAMESPACE_END
