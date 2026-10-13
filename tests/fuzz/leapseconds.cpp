/**
 * @file      leapseconds.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for the leap-second list's parser (bmds/leapseconds.h, devplan/network.md
 * §31e): any text; what parses must be in order and answer for any date. Seed it with
 * thirdparty/tzdata/leap-seconds.list (its hash only matches unchanged, so the fuzzer mostly
 * explores the refusals; the hash's own path is reached with the real file).
 */

#include <cstdint>
#include <cstdlib>
#include <bmds/leapseconds.h>

namespace LeapSeconds = bmds::LeapSeconds;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        static LeapSeconds::Table  t;
        const LeapSeconds::Verdict v =
                LeapSeconds::parse(bmds::StringView(reinterpret_cast<const char *>(data), size), t);
        (void)LeapSeconds::verdictText(v);
        if (v != LeapSeconds::Verdict::Ok) return 0;
        if (t.count == 0 || t.count > LeapSeconds::MaxEntries) std::abort();
        for (size_t i = 1; i < t.count; ++i)
                if (t.entries[i].at <= t.entries[i - 1].at) std::abort();
        for (int64_t at : {INT64_MIN / 2, int64_t(0), t.entries[0].at, t.expires, INT64_MAX / 2}) {
                (void)LeapSeconds::offsetAt(t, at);
                (void)LeapSeconds::nextAfter(t, at);
        }
        return 0;
}
