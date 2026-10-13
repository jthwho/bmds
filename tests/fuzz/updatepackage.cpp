/**
 * @file      updatepackage.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for update packages' heads (bmds/updatepackage.h, devplan/update.md §2): any
 * bytes, read as a whole file; what parses must have its sections after the head and inside the
 * file, and a signature check must only read the head. Seed it with tests/host/update/test.bmup.
 */

#include <cstdint>
#include <cstdlib>
#include <bmds/updatepackage.h>

namespace UpdatePackage = bmds::UpdatePackage;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        static UpdatePackage::Package   p;
        const bmds::Span<const uint8_t> bytes(data, size);
        size_t                          head = 0;
        if (UpdatePackage::headBytes(bytes, head) == UpdatePackage::Verdict::Ok && head > UpdatePackage::MaxHeadBytes)
                std::abort();
        const UpdatePackage::Verdict v = UpdatePackage::parse(bytes, size, p);
        (void)UpdatePackage::verdictText(v);
        if (v != UpdatePackage::Verdict::Ok) return 0;
        if (p.headBytes > size || p.sectionCount > UpdatePackage::MaxSections) std::abort();
        for (size_t i = 0; i < p.sectionCount; ++i) {
                const UpdatePackage::Section &s = p.sections[i];
                if (s.offset < p.headBytes || uint64_t(s.offset) + s.size > size) std::abort();
        }
        const UpdatePackage::Key key;
        (void)UpdatePackage::verify(p, bmds::Span<const uint8_t>(data, p.headBytes),
                                    bmds::Span<const UpdatePackage::Key>(&key, 1));
        return 0;
}
