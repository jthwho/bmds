/**
 * @file      tlsserverkeys.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The board's own TLS certificate (devplan/network.md §37a S3): an ECDSA P-256 key and a
 * self-signed certificate for the host name, made with mbedTLS's writers the first time a server
 * session wants them, kept as PEM files in `net/tls/server-keys`.
 */

#include <cstdio>
#include <cstring>
#include <esp_random.h>
#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <bmds/clock.h>
#include <bmds/datetime.h>
#include <bmds/log.h>
#include <bmds/memory.h>
#include <bmds/mutex.h>
#include <bmds/sysconf.h>
#include <bmds/tlsserverkeys.h>
#include <bmds/tlsservice.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace TlsServerKeys {

        namespace {

                constexpr const char *KeyName = "server-key.pem";
                constexpr const char *CertName = "server-cert.pem";
                constexpr size_t      MaxFileBytes = 4096; // a P-256 key is 227 bytes as PEM, its certificate ~700
                constexpr int         Years = 10;

                const Vfs::Context Root{"/", nullptr};

                Mutex &lock() {
                        static Mutex m;
                        return m;
                }

                int random(void *, unsigned char *out, size_t bytes) {
                        esp_fill_random(out, bytes);
                        return 0;
                }

                Error readWhole(StringView path, String &out) {
                        out.clear();
                        auto [file, err] = Vfs::open(path, Vfs::Read, Root);
                        if (err.isError()) return err;
                        char buf[256];
                        for (;;) {
                                auto [n, rerr] = file->read(
                                        WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)), WaitForever);
                                if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) return Error::Ok;
                                if (rerr.isError()) return rerr;
                                if (out.size() + n > MaxFileBytes) return Error::OutOfRange;
                                if (Error aerr = out.append(StringView(buf, n)); aerr.isError()) return aerr;
                        }
                }

                // @p text to @p path whole: written under a temporary name, then renamed over it.
                Error writeWhole(const String &path, StringView text) {
                        String temp(MemSpace::Psram);
                        if (temp.appendf("%s.new", path.cStr()).isError()) return Error::NoMem;
                        {
                                auto [file, err] =
                                        Vfs::open(temp.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, Root);
                                if (err.isError()) return err;
                                size_t at = 0;
                                while (at < text.size()) {
                                        auto [n, werr] = file->write(
                                                ByteSpan(reinterpret_cast<const uint8_t *>(text.data()) + at,
                                                         text.size() - at),
                                                WaitForever);
                                        if (werr.isError()) return werr;
                                        if (n == 0) return Error::IoError;
                                        at += n;
                                }
                        }
                        return Vfs::rename(temp.view(), path.view(), Vfs::RenameMode::Replace, Root);
                }

                struct Paths {
                                String folder{MemSpace::Psram};
                                String key{MemSpace::Psram};
                                String cert{MemSpace::Psram};
                };

                Error paths(Paths &p) {
                        if (Error err = TlsService::serverKeysFolder(p.folder); err.isError()) return err;
                        while (p.folder.size() > 1 && p.folder.view().endsWith("/"))
                                p.folder.truncate(p.folder.size() - 1);
                        if (p.key.appendf("%s/%s", p.folder.cStr(), KeyName).isError() ||
                            p.cert.appendf("%s/%s", p.folder.cStr(), CertName).isError())
                                return Error::NoMem;
                        return Error::Ok;
                }

                // "YYYYMMDDhhmmss", as mbedTLS's writer takes a validity.
                void stamp(int64_t seconds, char (&out)[15]) {
                        const DateTime t = DateTime::fromSeconds(seconds);
                        char           text[48];
                        (void)std::snprintf(text, sizeof(text), "%04d%02u%02u%02u%02u%02u", int(t.year),
                                            unsigned(t.month), unsigned(t.day), unsigned(t.hour), unsigned(t.minute),
                                            unsigned(t.second));
                        std::memcpy(out, text, sizeof(out) - 1);
                        out[sizeof(out) - 1] = '\0';
                        return;
                }

                // A new key and certificate into @p p's files. Lock held.
                Error make(const Paths &p, String &why) {
                        const Error made = Vfs::mkdir(p.folder.view(), Root);
                        if (made.isError() && made != Error::Exists) {
                                (void)why.appendf("%s: %s", p.folder.cStr(), made.desc());
                                return made;
                        }
                        char host[HostName::MaxBytes + 1];
                        if (SysConf::hostname(host) == 0) (void)std::snprintf(host, sizeof(host), "bmds");

                        mbedtls_pk_context     key;
                        mbedtls_x509write_cert crt;
                        mbedtls_pk_init(&key);
                        mbedtls_x509write_crt_init(&crt);
                        auto *keyPem = static_cast<unsigned char *>(Memory::alloc(MemSpace::Psram, 2 * MaxFileBytes));
                        unsigned char *certPem = keyPem != nullptr ? keyPem + MaxFileBytes : nullptr;
                        int            ret = keyPem != nullptr ? 0 : MBEDTLS_ERR_PK_ALLOC_FAILED;
                        if (ret == 0) ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
                        if (ret == 0)
                                ret = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), random,
                                                          nullptr);
                        if (ret == 0) ret = mbedtls_pk_write_key_pem(&key, keyPem, MaxFileBytes);

                        // The certificate: CN the host name, SANs NAME and NAME.local, ten years from
                        // now (from 2026-01-01 while the clock isn't set), not a CA, for signing.
                        char subject[HostName::MaxBytes + 16];
                        (void)std::snprintf(subject, sizeof(subject), "CN=%s,O=BMDS", host);
                        char local[HostName::MaxBytes + 8];
                        (void)std::snprintf(local, sizeof(local), "%s.local", host);
                        mbedtls_x509_san_list sans[2] = {};
                        sans[0].node.type = MBEDTLS_X509_SAN_DNS_NAME;
                        sans[0].node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(host);
                        sans[0].node.san.unstructured_name.len = std::strlen(host);
                        sans[0].next = &sans[1];
                        sans[1].node.type = MBEDTLS_X509_SAN_DNS_NAME;
                        sans[1].node.san.unstructured_name.p = reinterpret_cast<unsigned char *>(local);
                        sans[1].node.san.unstructured_name.len = std::strlen(local);
                        const int64_t from = Clock::isSet()
                                                     ? Clock::now().seconds()
                                                     : Calendar::daysFromCivil(2026, 1, 1) * Calendar::SecondsPerDay;
                        char          notBefore[15], notAfter[15];
                        stamp(from, notBefore);
                        stamp(from + int64_t(Years) * 365 * Calendar::SecondsPerDay, notAfter);
                        uint8_t serial[16];
                        esp_fill_random(serial, sizeof(serial));
                        serial[0] = uint8_t((serial[0] & 0x7f) | 0x01); // positive, and 16 bytes long
                        if (ret == 0) {
                                mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
                                mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
                                mbedtls_x509write_crt_set_subject_key(&crt, &key);
                                mbedtls_x509write_crt_set_issuer_key(&crt, &key);
                                ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
                        }
                        if (ret == 0) ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
                        if (ret == 0) ret = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial));
                        if (ret == 0) ret = mbedtls_x509write_crt_set_validity(&crt, notBefore, notAfter);
                        if (ret == 0) ret = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
                        if (ret == 0)
                                ret = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
                        if (ret == 0) ret = mbedtls_x509write_crt_set_subject_alternative_name(&crt, sans);
                        if (ret == 0) ret = mbedtls_x509write_crt_pem(&crt, certPem, MaxFileBytes, random, nullptr);
                        mbedtls_x509write_crt_free(&crt);
                        mbedtls_pk_free(&key);
                        Error err = ret == 0                             ? Error::Ok
                                    : ret == MBEDTLS_ERR_PK_ALLOC_FAILED ? Error::NoMem
                                                                         : Error::IoError;
                        if (err.isError()) {
                                (void)why.appendf("making the board's TLS certificate failed (mbedTLS -0x%04x)",
                                                  unsigned(-ret));
                        } else {
                                // The key first: a certificate is never there without its key.
                                err = writeWhole(p.key, StringView(reinterpret_cast<const char *>(keyPem)));
                                if (err.isOk())
                                        err = writeWhole(p.cert, StringView(reinterpret_cast<const char *>(certPem)));
                                if (err.isError()) (void)why.appendf("%s: %s", p.folder.cStr(), err.desc());
                        }
                        if (keyPem != nullptr) {
                                std::memset(keyPem, 0, MaxFileBytes); // the private key's text
                                Memory::free(keyPem);
                        }
                        if (err.isOk())
                                bmdsInfo("tls: made the board's certificate for %s in %s", host, p.folder.cStr());
                        return err;
                }

                Error load(String &cert, String &key, String &why, bool renewing) {
                        why.clear();
                        Paths p;
                        if (Error err = paths(p); err.isError()) return err;
                        Mutex::Locker locker(lock(), WaitForever);
                        const bool    missing =
                                Vfs::stat(p.key.view(), Root).isError() || Vfs::stat(p.cert.view(), Root).isError();
                        if (renewing || missing) {
                                if (Error err = make(p, why); err.isError()) return err;
                        }
                        Error err = readWhole(p.key.view(), key);
                        if (err.isOk()) err = readWhole(p.cert.view(), cert);
                        if (err.isError()) (void)why.appendf("%s: %s", p.folder.cStr(), err.desc());
                        return err;
                }

                int64_t unixSeconds(const mbedtls_x509_time &t) {
                        return Calendar::daysFromCivil(t.year, unsigned(t.mon), unsigned(t.day)) *
                                       Calendar::SecondsPerDay +
                               t.hour * 3600 + t.min * 60 + t.sec;
                }

                // A private key's text cleared before its memory goes back.
                void wipe(String &s) {
                        if (!s.isEmpty()) std::memset(const_cast<char *>(s.cStr()), 0, s.size());
                        s.clear();
                        return;
                }

        } // namespace

        Error pem(String &cert, String &key, String &why) {
                return load(cert, key, why, false);
        }

        Error renew(String &why) {
                String      cert(MemSpace::Psram), key(MemSpace::Psram);
                const Error err = load(cert, key, why, true);
                wipe(key);
                return err;
        }

        Error describe(Description &out, String &why) {
                out = Description{};
                String cert(MemSpace::Psram), key(MemSpace::Psram);
                Error  err = load(cert, key, why, false);
                wipe(key);
                if (err.isError()) return err;
                Paths p;
                if (paths(p).isOk()) (void)std::snprintf(out.folder, sizeof(out.folder), "%s", p.folder.cStr());
                mbedtls_x509_crt crt;
                mbedtls_x509_crt_init(&crt);
                // mbedTLS's PEM parser wants the NUL counted.
                const int ret = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(cert.cStr()),
                                                       cert.size() + 1);
                if (ret == 0) {
                        (void)mbedtls_sha256(crt.raw.p, crt.raw.len, out.sha256, 0);
                        if (mbedtls_x509_dn_gets(out.subject, sizeof(out.subject), &crt.subject) < 0)
                                out.subject[0] = '\0';
                        out.notBefore = unixSeconds(crt.valid_from);
                        out.notAfter = unixSeconds(crt.valid_to);
                } else {
                        (void)why.appendf("%s/%s doesn't parse (mbedTLS -0x%04x)", out.folder, CertName,
                                          unsigned(-ret));
                        err = Error::IoError;
                }
                mbedtls_x509_crt_free(&crt);
                return err;
        }

} // namespace TlsServerKeys

BMDS_NAMESPACE_END
