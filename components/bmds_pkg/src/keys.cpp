/**
 * @file      keys.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The keyring (devplan/packages.md §6, §22): the built-in keys, those added on the board, and who
 * signed a package, in words.
 */

#include <cstring>
#include <bmds/packages.h>
#include <bmds/vfs.h>
#include "packagesdetail.h"

BMDS_NAMESPACE_BEGIN

namespace Packages::Detail {

        // A key file is a line or a few: far less than this.
        constexpr size_t MaxKeyFileBytes = 4096;

        bool sameKey(const SignedContainer::Key &a, const SignedContainer::Key &b) {
                return std::memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
        }

        // The first key line of @p text into @p key and @p comment.
        Error firstKey(StringView text, SignedContainer::Key &key, StringView &comment) {
                while (!text.isEmpty()) {
                        const size_t     nl = text.indexOf('\n');
                        const StringView line = text.left(nl);
                        text = nl == StringView::NPos ? StringView() : text.mid(nl + 1);
                        if (SignedContainer::parseKey(line, key, comment).isOk()) return Error::Ok;
                }
                return Error::CorruptData;
        }

        void idText(const uint8_t (&id)[SignedContainer::KeyIdBytes],
                    char (&out)[2 * SignedContainer::KeyIdBytes + 1]) {
                hex(id, sizeof(id), out);
                return;
        }

        Error keysOf(const Config &config, List<Packages::KeyEntry> &ring, List<SignedContainer::Key> &keys) {
                Error err = Packages::keyring(config, ring);
                for (size_t i = 0; err.isOk() && i < ring.size(); ++i) err = keys.append(ring[i].key);
                return err;
        }

        Error verifyHead(const SignedContainer::Head &parsed, Span<const uint8_t> head, const Config &config,
                         Packages::Signer &signer) {
                List<Packages::KeyEntry>   ring(MemSpace::Psram);
                List<SignedContainer::Key> keys(MemSpace::Psram);
                const Error                err = keysOf(config, ring, keys);
                if (err.isError()) return err;
                size_t index = 0;
                if (SignedContainer::verify(parsed, head, keys.span(), index) != SignedContainer::Verdict::Ok)
                        return Error::CorruptData;
                signer = ring[index].kind;
                return Error::Ok;
        }

        Error verifyWith(const SignedContainer::Head &parsed, Span<const uint8_t> head, const Config &config,
                         Span<const StringView> ids) {
                List<Packages::KeyEntry>   ring(MemSpace::Psram);
                List<SignedContainer::Key> all(MemSpace::Psram);
                Error                      err = keysOf(config, ring, all);
                List<SignedContainer::Key> named(MemSpace::Psram);
                for (size_t i = 0; err.isOk() && i < ring.size(); ++i) {
                        char id[2 * SignedContainer::KeyIdBytes + 1];
                        idText(ring[i].id, id);
                        bool wanted = ids.size() == 0;
                        for (StringView w : ids) wanted = wanted || w == StringView(id);
                        if (wanted) err = named.append(ring[i].key);
                }
                if (err.isError()) return err;
                size_t index = 0;
                return SignedContainer::verify(parsed, head, named.span(), index) == SignedContainer::Verdict::Ok
                               ? Error::Ok
                               : Error::CorruptData;
        }

} // namespace Packages::Detail

using namespace Packages::Detail;

Error Packages::keyring(const Config &config, List<KeyEntry> &out) {
        out.clear();
        const auto listed = [&](const SignedContainer::Key &key) {
                for (const KeyEntry &e : out) {
                        if (sameKey(e.key, key)) return true;
                }
                return false;
        };
        Error err;
        for (size_t i = 0; err.isOk() && i < config.keys.size(); ++i) {
                KeyEntry e;
                e.key = config.keys[i];
                SignedContainer::keyId(e.key, e.id);
                e.kind = config.isDevKey != nullptr && config.isDevKey(i) ? Signer::Dev : Signer::Trusted;
                if (config.keyComment != nullptr) err = e.comment.append(config.keyComment(i));
                if (err.isOk()) err = out.append(std::move(e));
        }
        if (err.isError() || config.keysDir.isEmpty()) return err;
        auto [dir, derr] = Vfs::openDirectory(config.keysDir, vfs());
        if (derr.isError()) return Error::Ok; // none added
        List<String> names(MemSpace::Psram);
        for (;;) {
                auto [entry, nerr] = dir.next();
                if (nerr.isError()) break;
                if (entry.type == FileType::File && entry.name.view().endsWith(".pub"))
                        err = err.isOk() ? names.append(std::move(entry.name)) : err;
        }
        dir.close();
        for (size_t i = 0; err.isOk() && i < names.size(); ++i) {
                auto [path, jerr] = join(config.keysDir, names[i].view());
                String     text;
                KeyEntry   e;
                StringView comment;
                if (jerr.isError() || readWhole(path.view(), MaxKeyFileBytes, text).isError() ||
                    firstKey(text.view(), e.key, comment).isError())
                        continue;
                // The dev key is built in or refused, never added; a key listed already counts once.
                if ((config.devKey != nullptr && sameKey(e.key, *config.devKey)) || listed(e.key)) continue;
                SignedContainer::keyId(e.key, e.id);
                e.kind = Signer::Added;
                err = e.comment.append(comment);
                if (err.isOk()) err = out.append(std::move(e));
        }
        return err;
}

Error Packages::addKey(StringView path, const Config &config, KeyEntry &added, String &why) {
        why.clear();
        String text;
        Error  err = readWhole(path, MaxKeyFileBytes, text);
        if (err.isError()) return fail(why, err, "%.*s: %s", int(path.size()), path.data(), err.desc());
        StringView comment;
        added = KeyEntry{};
        if (firstKey(text.view(), added.key, comment).isError()) {
                return fail(why, Error::CorruptData, "%.*s: no key in it (a line: ed25519 HEX COMMENT)",
                            int(path.size()), path.data());
        }
        SignedContainer::keyId(added.key, added.id);
        added.kind = Signer::Added;
        err = added.comment.append(comment);
        char id[2 * SignedContainer::KeyIdBytes + 1];
        idText(added.id, id);
        if (config.devKey != nullptr && sameKey(added.key, *config.devKey)) {
                return fail(why, Error::PermissionDenied,
                            "%s is the dev key: built into a dev firmware, and refused by a trusted one", id);
        }
        List<KeyEntry> ring(MemSpace::Psram);
        if (err.isOk()) err = keyring(config, ring);
        if (err.isError()) return fail(why, err, "out of memory");
        for (const KeyEntry &e : ring) {
                if (sameKey(e.key, added.key))
                        return fail(why, Error::Exists, "%s is in the keyring already (%s)", id,
                                    e.kind == Signer::Added ? "added" : "built in");
        }
        String file;
        err = file.appendf("%s.pub", id);
        auto [to, jerr] = join(config.keysDir, file.view());
        String line;
        if (err.isOk()) err = jerr;
        if (err.isOk()) err = line.append("ed25519 ");
        for (size_t i = 0; err.isOk() && i < sizeof(added.key.bytes); ++i)
                err = line.appendf("%02x", unsigned(added.key.bytes[i]));
        if (err.isOk() && !comment.isEmpty()) err = line.appendf(" %.*s", int(comment.size()), comment.data());
        if (err.isOk()) err = line.append('\n');
        if (err.isOk()) err = makeDirectories(config.keysDir);
        if (err.isOk()) err = writeWhole(to.view(), line.view());
        if (err.isError()) return fail(why, err, "%s: %s", to.cStr(), err.desc());
        return Error::Ok;
}

Error Packages::removeKey(StringView id, const Config &config, KeyEntry &removed, String &why) {
        why.clear();
        if (id.size() < 4 || id.size() > 2 * SignedContainer::KeyIdBytes) {
                return fail(why, Error::Invalid, "%.*s: a key's id is 4 to 16 hex digits (pkg key list)",
                            int(id.size()), id.data());
        }
        List<KeyEntry> ring(MemSpace::Psram);
        Error          err = keyring(config, ring);
        if (err.isError()) return fail(why, err, "out of memory");
        size_t found = List<KeyEntry>::NPos, matches = 0;
        for (size_t i = 0; i < ring.size(); ++i) {
                char text[2 * SignedContainer::KeyIdBytes + 1];
                idText(ring[i].id, text);
                if (!StringView(text).startsWith(id)) continue;
                ++matches;
                found = i;
        }
        if (matches == 0)
                return fail(why, Error::NotFound, "%.*s: no such key (pkg key list)", int(id.size()), id.data());
        if (matches > 1)
                return fail(why, Error::Invalid, "%.*s: more than one key's id starts so", int(id.size()), id.data());
        char text[2 * SignedContainer::KeyIdBytes + 1];
        idText(ring[found].id, text);
        if (ring[found].kind != Signer::Added)
                return fail(why, Error::PermissionDenied, "%s is built into the firmware: it can't be removed", text);
        // The file is found by its key: it may have been put there under another name.
        auto [dir, derr] = Vfs::openDirectory(config.keysDir, vfs());
        if (derr.isError())
                return fail(why, derr, "%.*s: %s", int(config.keysDir.size()), config.keysDir.data(), derr.desc());
        List<String> files(MemSpace::Psram);
        for (;;) {
                auto [entry, nerr] = dir.next();
                if (nerr.isError()) break;
                if (entry.type == FileType::File && entry.name.view().endsWith(".pub"))
                        (void)files.append(std::move(entry.name));
        }
        dir.close();
        for (const String &name : files) {
                auto [path, jerr] = join(config.keysDir, name.view());
                String               content;
                SignedContainer::Key key;
                StringView           comment;
                if (jerr.isError() || readWhole(path.view(), MaxKeyFileBytes, content).isError() ||
                    firstKey(content.view(), key, comment).isError() || !sameKey(key, ring[found].key))
                        continue;
                err = removeIfThere(path.view());
                if (err.isError()) return fail(why, err, "%s: %s", path.cStr(), err.desc());
        }
        removed = std::move(ring[found]);
        return Error::Ok;
}

void Packages::describeSigner(const Config &config, const Info &info, String &name, String &what) {
        name.clear();
        what.clear();
        char id[2 * SignedContainer::KeyIdBytes + 1];
        idText(info.keyId, id);
        if (info.signer == Signer::Dev) {
                (void)name.append("the dev key");
                (void)what.append("anyone could have");
                return;
        }
        List<KeyEntry> ring(MemSpace::Psram);
        (void)keyring(config, ring);
        for (const KeyEntry &e : ring) {
                if (std::memcmp(e.id, info.keyId, sizeof(e.id)) != 0) continue;
                if (e.comment.isEmpty())
                        (void)name.appendf("key %s", id);
                else
                        (void)name.append(e.comment.view());
                (void)what.append(e.kind == Signer::Added ? "an added key" : "a built-in trusted key");
                return;
        }
        (void)name.appendf("key %s", id);
        (void)what.append("no longer in the keyring");
        return;
}

BMDS_NAMESPACE_END
