/**
 * @file      netmounts.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * `net/mounts`' entries (devplan/netfs.md step 4): split, checked.
 */

#include <doctest.h>

#include <bmds/netmounts.h>

using bmds::Error;
using bmds::Span;
using bmds::StringView;
namespace NetMounts = bmds::NetMounts;

TEST_CASE("netmounts: entries split into words; the checks") {
        NetMounts::Entry             e[NetMounts::MaxEntries];
        size_t                       n = 99;
        const Span<NetMounts::Entry> all(e, NetMounts::MaxEntries);
        CHECK(NetMounts::parse("", all, n) == Error::Ok);
        CHECK(n == 0);
        CHECK(NetMounts::parse(" ; ;", all, n) == Error::Ok);
        CHECK(n == 0);

        CHECK(NetMounts::parse(" sshfs  jth@pc:src /net/pc ;sshfs -p 2222 -y me@server: /net/server", all, n) ==
              Error::Ok);
        REQUIRE(n == 2);
        CHECK(e[0].text == "sshfs  jth@pc:src /net/pc");
        CHECK(e[0].count == 3);
        CHECK(e[0].words[0] == "sshfs");
        CHECK(e[0].words[1] == "jth@pc:src");
        CHECK(e[0].mountPoint() == "/net/pc");
        CHECK(e[1].count == 6);
        CHECK(e[1].words[2] == "2222");
        CHECK(e[1].mountPoint() == "/net/server");

        // Refused: an unknown program, no mount point, a relative or root one, two at one point.
        CHECK(NetMounts::check("rm -rf /") == Error::Invalid);
        CHECK(NetMounts::check("sshfs pc:") == Error::Invalid);
        CHECK(NetMounts::check("sshfs pc: net/pc") == Error::Invalid);
        CHECK(NetMounts::check("sshfs pc: /") == Error::Invalid);
        CHECK(NetMounts::check("sshfs a: /net/x; sshfs b: /net/x") == Error::Invalid);
        // Too many entries, too many words.
        CHECK(NetMounts::check("sshfs a: /a; sshfs b: /b; sshfs c: /c; sshfs d: /d; sshfs e: /e") == Error::Full);
        CHECK(NetMounts::check("sshfs -y -y -y -y -y -y -y -y -y -y -y -y -y -y a: /a") == Error::Full);
        CHECK(NetMounts::check("sshfs a: /a; sshfs b: /b; sshfs c: /c; sshfs d: /d") == Error::Ok);
}
