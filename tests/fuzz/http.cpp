/**
 * @file      http.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for HTTP (devplan/packages.md §23): any bytes as a response, fed whole and in
 * pieces whose sizes the input's first byte chooses; and as URLs, parsed and resolved against one
 * another. What reads must make progress, hand out views inside what was fed, give a body no
 * longer than its Content-Length, and agree whatever the pieces; a resolved URL parses.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <bmds/assert.h>
#include <bmds/http.h>

using bmds::ByteSpan;
using bmds::StringView;
namespace Http = bmds::Http;

// The platform's assert hook (the kernel's on the target): a failed one is a finding.
[[noreturn]] void bmds::Detail::assertFailed(const char *expr, const char *file, int line) {
        std::fprintf(stderr, "BMDS_ASSERT(%s) failed at %s:%d\n", expr, file, line);
        std::abort();
}

namespace {

        // Feeds @p data in pieces of @p step (0: whole); the body read to @p out. Returns the outcome.
        int read(const uint8_t *data, size_t size, size_t step, std::string &out, int &status) {
                Http::ResponseParser p;
                size_t               at = 0;
                while (at < size && !p.isDone()) {
                        const size_t n = step == 0 ? size - at : (step < size - at ? step : size - at);
                        ByteSpan     in(data + at, n);
                        while (!in.isEmpty() && !p.isDone()) {
                                size_t     used = 0;
                                ByteSpan   body;
                                const auto err = p.feed(in, used, body);
                                if (err.isError()) return 1;
                                if (used == 0 || used > in.size()) std::abort(); // progress, within what was fed
                                if (!body.isEmpty() &&
                                    (body.data() < in.data() || body.data() + body.size() > in.data() + used))
                                        std::abort();
                                out.append(reinterpret_cast<const char *>(body.data()), body.size());
                                in = in.mid(used);
                                at += used;
                        }
                }
                status = p.hasHead() ? p.head().status : 0;
                if (p.hasHead() && !p.head().chunked && p.head().length != Http::Unknown &&
                    out.size() > p.head().length)
                        std::abort();
                return p.end().isOk() ? 0 : 2;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        if (size == 0) return 0;
        const size_t step = data[0] % 17;
        std::string  whole, pieces;
        int          s1 = 0, s2 = 0;
        const int    r1 = read(data + 1, size - 1, 0, whole, s1);
        const int    r2 = read(data + 1, size - 1, step, pieces, s2);
        if (r1 != r2 || whole != pieces || s1 != s2) std::abort(); // the same however it arrives

        // Lines as URLs: each parsed; each resolved against the first, which must then parse.
        const StringView text(reinterpret_cast<const char *>(data + 1), size - 1);
        StringView       base;
        StringView       rest = text.left(8192);
        for (int i = 0; i < 16 && !rest.isEmpty(); ++i) {
                const size_t     nl = rest.indexOf('\n');
                const StringView line = rest.left(nl);
                rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                Http::Url u;
                if (Http::parseUrl(line, u).isOk()) {
                        if (u.host.isEmpty() || u.port == 0) std::abort();
                        if (base.isEmpty()) base = line;
                }
                if (!base.isEmpty()) {
                        auto [out, err] = Http::resolve(base, line);
                        Http::Url check;
                        if (err.isOk() && Http::parseUrl(out.view(), check).isError()) std::abort();
                        bmds::String request;
                        if (err.isOk() && Http::buildRequest(check, 7, "fuzz", request).isError()) std::abort();
                }
        }
        return 0;
}
