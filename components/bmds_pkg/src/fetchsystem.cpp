/**
 * @file      fetchsystem.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The system's fetcher (bmds/fetch.h, devplan/packages.md §23): `Http::get()` over the network
 * stack, with TLS. In the main firmware only: recovery has no network (`update fetch` and `pkg`
 * find `systemFetch` null there).
 */

#include <bmds/fetch.h>
#include <bmds/httpclient.h>
#include <bmds/netstack.h>
#include <bmds/system.h>

BMDS_NAMESPACE_BEGIN

Error systemFetch(StringView url, uint64_t from, const FetchSink &sink, String *finalUrl, String &why) {
        String agent;
        (void)agent.appendf("BMDS/%s", System::info().buildVersion);
        Http::Environment env;
        env.resolve = NetStack::resolve;
        env.openTls = Http::openTls;
        env.userAgent = agent.view();
        Http::Sink s;
        s.begin = sink.begin;
        s.body = sink.body;
        s.arg = sink.arg;
        Http::Fetched fetched;
        const Error   err = Http::get(url, from, env, s, fetched, why);
        if (finalUrl != nullptr) *finalUrl = std::move(fetched.url);
        return err;
}

BMDS_NAMESPACE_END
