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

        // Sends all of @p data (`Http::Tls::send`'s promise).
        Result<size_t> tlsSend(void *state, ByteSpan data) {
                TlsSession &s = *static_cast<TlsSession *>(state);
                size_t      sent = 0;
                while (sent < data.size()) {
                        auto [n, err] = s.send(data.mid(sent), 0);
                        if (err.isError()) return Result<size_t>(sent, err);
                        sent += n;
                }
                return Result<size_t>(sent, Error::Ok);
        }

        Result<size_t> tlsReceive(void *state, WritableByteSpan into) {
                return static_cast<TlsSession *>(state)->receive(into, 0);
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
                TlsSession::destroy(s);
                return err;
        }
        out.state = s;
        out.send = tlsSend;
        out.receive = tlsReceive;
        out.close = tlsClose;
        return Error::Ok;
}

BMDS_NAMESPACE_END
