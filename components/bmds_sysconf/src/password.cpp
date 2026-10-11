/**
 * @file      password.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The machine's password (devplan/network.md §24): `system/password`, SHA-512
 * crypt, and /etc/passwd, /etc/group and /etc/shadow.
 */

#include <bmds/password.h>
#include <bmds/random.h>
#include <bmds/shacrypt.h>
#include <bmds/sysconf.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // Only a hash or nothing goes in: a plain password can't be stored by mistake.
        Error checkHash(StringView value) {
                return value.isEmpty() || ShaCrypt::isHash(value) ? Error::Ok : Error::Invalid;
        }

        BMDS_SYSCONF(systemPassword, "system/password", Config::Type::Secret, "",
                     "The user's password, SHA-512 crypt ($6$); set with passwd (empty: none)", 0, 0, nullptr,
                     checkHash)

        Error passwdFile(void *, String &out) {
                return out.appendf("root:x:0:0:root:%.*s:/bin/sh\n", int(Vfs::HomePath.size()), Vfs::HomePath.data());
        }

        Error groupFile(void *, String &out) {
                return out.append("root:x:0:\n");
        }

        Error shadow(void *, String &out) {
                return Password::shadowFile(out);
        }

} // namespace

Error Password::init() {
        static constexpr Vfs::EtcFile Files[] = {
                {"passwd", &passwdFile, nullptr},
                {"group", &groupFile, nullptr},
                {"shadow", &shadow, nullptr},
        };
        for (const Vfs::EtcFile &f : Files) {
                const Error err = Vfs::addEtcFile(f);
                if (err.isError() && err != Error::Exists) return err;
        }
        return Error::Ok;
}

const Config::Param &Password::declaration() {
        return bmdsConf_systemPassword;
}

bool Password::isSet() {
        String value(MemSpace::Psram);
        return SysConf::get(StringView(Key), value).isOk() && !value.isEmpty();
}

Error Password::set(StringView plain) {
        String hash(MemSpace::Psram);
        if (!plain.isEmpty()) {
                // The salt: random characters of the hash's alphabet.
                static constexpr char Chars[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
                uint8_t               bytes[SaltChars];
                Random::fill(bytes, sizeof(bytes));
                char salt[SaltChars];
                for (size_t i = 0; i < SaltChars; ++i) salt[i] = Chars[bytes[i] & 0x3f];
                const Error err = ShaCrypt::hash(plain, StringView(salt, SaltChars), Rounds, hash);
                if (err.isError()) return err;
        }
        Error err = SysConf::set(StringView(Key), hash.view());
        if (err.isOk()) err = SysConf::save(StringView(Key));
        return err;
}

Error Password::shadowFile(String &out) {
        String hash(MemSpace::Psram);
        if (SysConf::getSecret(StringView(Key), hash).isError() || hash.isEmpty()) (void)hash.assign("!");
        return out.appendf("root:%s:::::::\n", hash.cStr());
}

BMDS_NAMESPACE_END
