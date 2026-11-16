/**
 * @file      httptls.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * TLS for fetching (devplan/packages.md §23): a `TlsSession` (tlssession.h) over the connection's
 * socket, its handshake done before `get()` sends the request.
 */

#include <bmds/httpclient.h>
#include <bmds/tlssession.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // A session's errors as `Http::get()` documents them.
        Error httpError(Error err) {
                if (err == Error::KeyRejected) return Error::PermissionDenied;
                if (err == Error::ProtocolError || err == Error::BadMessage) return Error::CorruptData;
                return err;
        }

        // Sends all of @p data (`Http::Tls::send`'s promise).
        Result<size_t> tlsSend(void *state, ByteSpan data) {
                TlsSession &s = *static_cast<TlsSession *>(state);
                size_t      sent = 0;
                while (sent < data.size()) {
                        auto [n, err] = s.send(data.mid(sent), 0);
                        if (err.isError()) return Result<size_t>(sent, httpError(err));
                        sent += n;
                }
                return Result<size_t>(sent, Error::Ok);
        }

        Result<size_t> tlsReceive(void *state, WritableByteSpan into) {
                auto [n, err] = static_cast<TlsSession *>(state)->receive(into, 0);
                return Result<size_t>(n, httpError(err));
        }

        void tlsClose(void *state) {
                TlsSession *s = static_cast<TlsSession *>(state);
                s->closeNotify();
                TlsSession::destroy(s);
                return;
        }

} // namespace

Error Http::openTls(Socket &socket, StringView host, String &why, Tls &out) {
        TlsSession::Config config;
        config.serverName = host;
        auto [s, err] = TlsSession::create(socket, config, why);
        if (err.isError()) return err;
        err = s->handshake(0);
        if (err.isError()) {
                why.clear();
                (void)why.append(s->why());
                // No clock: `NotReady`, as documented (nothing was sent).
                const bool clock = (s->verifyFlags() & TlsSession::VerifyClock) != 0;
                TlsSession::destroy(s);
                return clock ? Error(Error::NotReady) : httpError(err);
        }
        out.state = s;
        out.send = tlsSend;
        out.receive = tlsReceive;
        out.close = tlsClose;
        return Error::Ok;
}

BMDS_NAMESPACE_END
