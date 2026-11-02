/**
 * @file      pkgrecord.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <doctest.h>

#include <string>
#include <bmds/pkgrecord.h>

using bmds::Error;
using bmds::PkgRecord;
using bmds::StringView;

namespace {

        std::string str(const bmds::String &s) {
                return std::string(s.view().data(), s.view().size());
        }

} // namespace

TEST_CASE("PkgRecord: the records pkg --json writes") {
        PkgRecord r;
        // As the board printed them.
        REQUIRE(r.parse(R"({"type":"package","name":"bmds-standard","version":"1-1","arch":"all",)"
                        R"("title":"BMDS standard set","summary":"The standard set","category":"System",)"
                        R"("license":"MIT","homepage":"","depends":"busybox, dropbear","abi":1,)"
                        R"("installedSize":0,"size":1234,"made":1761955200,"signer":"dev","keyId":"1edc0f2cc301cc37",)"
                        R"("installed":false,"automatic":false,"files":0,"links":0,"directories":0,)"
                        R"("available":"1-1","upgradable":false,"keyName":"the dev key"})")
                        .isOk());
        CHECK(r.type == PkgRecord::Type::Package);
        CHECK(str(r.name) == "bmds-standard");
        CHECK(str(r.version) == "1-1");
        CHECK(str(r.title) == "BMDS standard set");
        CHECK(str(r.summary) == "The standard set");
        CHECK(str(r.category) == "System");
        CHECK(str(r.depends) == "busybox, dropbear");
        CHECK(str(r.available) == "1-1");
        CHECK(str(r.signer) == "dev");
        CHECK(r.size == 1234);
        CHECK_FALSE(r.installed);

        REQUIRE(r.parse(R"({"type":"plan","action":"install","name":"busybox","version":"1.37.0-1","from":"1.36.1-1",)"
                        R"("installedSize":2200000,"signer":"","automatic":true,"file":"/x/busybox.bmpk","fetch":900000})")
                        .isOk());
        CHECK(r.type == PkgRecord::Type::Plan);
        CHECK(str(r.action) == "install");
        CHECK(str(r.from) == "1.36.1-1");
        CHECK(r.installedSize == 2200000);
        CHECK(r.fetch == 900000);
        CHECK(r.automatic);
        CHECK(str(r.title).empty()); // emptied from the record before

        REQUIRE(r.parse(R"({"type":"progress","package":"busybox","step":"fetching","done":512,"total":4096})").isOk());
        CHECK(r.type == PkgRecord::Type::Progress);
        CHECK(str(r.name) == "busybox"); // "package" names it here
        CHECK(str(r.step) == "fetching");
        CHECK(r.done == 512);
        CHECK(r.total == 4096);

        REQUIRE(r.parse(R"({"type":"done","action":"install","name":"busybox","version":"1.37.0-1","automatic":true})")
                        .isOk());
        CHECK(r.type == PkgRecord::Type::Done);

        REQUIRE(r.parse(R"({"type":"error","package":"lib","error":"NotFound","text":"lib: no such package"})").isOk());
        CHECK(r.type == PkgRecord::Type::Error);
        CHECK(str(r.error) == "NotFound");
        CHECK(str(r.text) == "lib: no such package");

        REQUIRE(r.parse(R"({"type":"source","source":"file:/sd0/p0/packages","packages":17,"indexedHere":false})")
                        .isOk());
        CHECK(r.type == PkgRecord::Type::Source);
        CHECK(str(r.source) == "file:/sd0/p0/packages");
        CHECK(r.packages == 17);
        CHECK(str(r.error).empty());

        REQUIRE(r.parse(R"({"type":"source","source":"https://x/","packages":0,"indexedHere":false,)"
                        R"("error":"Timeout","text":"https://x/index.bmpi: no answer"})")
                        .isOk());
        CHECK(str(r.error) == "Timeout");
        CHECK(str(r.text) == "https://x/index.bmpi: no answer");

        REQUIRE(r.parse(R"({"type":"note","package":"set","text":"it has nothing to install"})").isOk());
        CHECK(r.type == PkgRecord::Type::Note);
}

TEST_CASE("PkgRecord: what it doesn't know is passed over, what isn't a record refused") {
        PkgRecord r;
        // A type and fields from a later pkg, nested values included.
        REQUIRE(r.parse(R"({"type":"someday","name":"x","extra":{"a":[1,2,{"b":null}]},"more":[true],"n":null})")
                        .isOk());
        CHECK(r.type == PkgRecord::Type::Other);
        CHECK(str(r.name) == "x");
        // A number too big for its field reads as 0 rather than failing the record.
        REQUIRE(r.parse(R"({"type":"progress","done":-5,"total":1e3})").isOk());
        CHECK(r.done == 0);

        CHECK(r.parse("") == Error::CorruptData);
        CHECK(r.parse("[1]") == Error::CorruptData);
        CHECK(r.parse(R"({"type":"done")") == Error::CorruptData);     // cut short
        CHECK(r.parse(R"({"type":"done"} {})") == Error::CorruptData); // two on a line
        CHECK(r.parse("not json") == Error::CorruptData);
        CHECK(r.type == PkgRecord::Type::Other); // cleared
        std::string huge = R"({"type":"note","text":")" + std::string(PkgRecord::MaxString + 10, 'a') + "\"}";
        CHECK(r.parse(StringView(huge.data(), huge.size())) == Error::BufferTooSmall);
}
