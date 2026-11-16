/**
 * @file      tlssettings.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TLS's settings in sysconf (devplan/network.md §35c): `net/tls/sessions`,
 * `net/tls/handshake-timeout` and `net/tls/ca`, read whenever a TLS socket starts, and the roots
 * files `net/tls/ca` names.
 */

#include <bmds/log.h>
#include <bmds/sysconf.h>
#include <bmds/tlsservice.h>
#include <bmds/vfs.h>
#include "tlssettings.h"

BMDS_NAMESPACE_BEGIN

namespace TlsService {

        namespace {

                enum Setting : size_t {
                        Sessions,
                        HandshakeTimeout,
                        Ca,
                        SettingCount
                };

                // A roots file's largest: the whole of Mozilla's set as PEM is about 230 KB.
                constexpr size_t MaxRootsBytes = 256 * 1024;

                constinit SysConf::Id ids[SettingCount] = {SysConf::NoId, SysConf::NoId, SysConf::NoId};

                Error readWhole(StringView path, String &out) {
                        static const Vfs::Context context{"/", nullptr};
                        auto [file, err] = Vfs::open(path, Vfs::Read, context);
                        if (err.isError()) return err;
                        char buf[512];
                        for (;;) {
                                auto [n, rerr] = file->read(
                                        WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)), WaitForever);
                                if (rerr == Error::EndOfFile || (rerr.isOk() && n == 0)) return Error::Ok;
                                if (rerr.isError()) return rerr;
                                if (out.size() + n > MaxRootsBytes) return Error::OutOfRange;
                                if (Error aerr = out.append(StringView(buf, n)); aerr.isError()) return aerr;
                        }
                }

        } // namespace

        Error declareSettings() {
                static constexpr struct {
                                const char  *key;
                                Config::Type type;
                                const char  *defaultValue;
                                const char  *help;
                                int32_t      min;
                                int32_t      max;
                } Decls[SettingCount] = {
                        {"net/tls/sessions", Config::Type::Int, "8", "TLS sockets open at once, at most", 1, 64},
                        {"net/tls/handshake-timeout", Config::Type::Int, "30",
                         "Seconds a TLS handshake may take before it fails", 1, 600},
                        {"net/tls/ca", Config::Type::Paths, "",
                         "PEM files of more trusted roots for every TLS socket (';'-separated): a home CA", 0, 0},
                };
                for (size_t i = 0; i < SettingCount; ++i) {
                        Config::Param p;
                        p.key = Decls[i].key;
                        p.type = Decls[i].type;
                        p.defaultValue = Decls[i].defaultValue;
                        p.help = Decls[i].help;
                        p.min = Decls[i].min;
                        p.max = Decls[i].max;
                        auto [id, err] = SysConf::declareKernel(p);
                        if (err.isError()) {
                                bmdsErr("tls: setting %s not declared: %s", Decls[i].key, err.desc());
                                return err;
                        }
                        ids[i] = id;
                }
                return Error::Ok;
        }

        Settings settings() {
                Settings s;
                if (auto [n, err] = SysConf::getInt(ids[Sessions]); err.isOk()) s.maxSessions = size_t(n);
                if (auto [n, err] = SysConf::getInt(ids[HandshakeTimeout]); err.isOk())
                        s.handshakeTimeoutMs = unsigned(n) * 1000;
                return s;
        }

        Error systemRoots(String &out, String &why) {
                out.clear();
                why.clear();
                String paths;
                if (ids[Ca] == SysConf::NoId || SysConf::get(ids[Ca], paths).isError()) return Error::Ok;
                StringView rest = paths.view();
                while (!rest.isEmpty()) {
                        const size_t     semi = rest.indexOf(';');
                        const StringView path = semi == StringView::NPos ? rest : rest.left(semi);
                        rest = semi == StringView::NPos ? StringView() : rest.mid(semi + 1);
                        if (path.isEmpty()) continue;
                        const Error err = readWhole(path, out);
                        if (err.isError()) {
                                (void)why.appendf("net/tls/ca: %.*s: %s", int(path.size()), path.data(), err.desc());
                                return err;
                        }
                        // PEM files one after the other: a line between them.
                        if (Error aerr = out.append("\n"); aerr.isError()) return aerr;
                }
                return Error::Ok;
        }

} // namespace TlsService

BMDS_NAMESPACE_END
