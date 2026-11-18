/**
 * @file      rommanifest.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 */

#include <cstring>
#include <bmds/rommanifest.h>

BMDS_NAMESPACE_BEGIN

namespace RomManifest {

        Counts compare(const RomImage &tree, Span<const RomManifestEntry> manifest, Visit visit, void *context) {
                Counts   counts;
                uint32_t t = 0;
                size_t   m = 0;
                auto tell = [&](Difference what, StringView path, uint32_t index, const RomManifestEntry *expected) {
                        switch (what) {
                                case Difference::Changed: ++counts.changed; break;
                                case Difference::Replaced: ++counts.replaced; break;
                                case Difference::Missing: ++counts.missing; break;
                                case Difference::Extra: ++counts.extra; break;
                        }
                        if (visit != nullptr) visit(context, Found{what, path, index, expected});
                };
                // Both sorted by path: walked together.
                while (t < tree.count() || m < manifest.size()) {
                        const RomImage::Entry   e = t < tree.count() ? tree.entry(t) : RomImage::Entry();
                        const RomManifestEntry *want = m < manifest.size() ? &manifest[m] : nullptr;
                        const StringView        wanted = want != nullptr ? StringView(want->path) : StringView();
                        if (want == nullptr || (t < tree.count() && e.path < wanted)) {
                                // In the tree alone.
                                if ((e.flags & RomImage::User) != 0) {
                                        ++counts.user;
                                } else {
                                        tell(Difference::Extra, e.path, t, nullptr);
                                }
                                ++t;
                        } else if (t >= tree.count() || wanted < e.path) {
                                tell(Difference::Missing, wanted, RomImage::NoEntry, want);
                                ++m;
                        } else {
                                if ((e.flags & RomImage::User) != 0) {
                                        tell(Difference::Replaced, e.path, t, want);
                                } else if (e.data.size() != want->size ||
                                           (e.flags & RomImage::System) != (want->flags & RomImage::System) ||
                                           std::memcmp(e.hash.data(), want->hash, RomImage::HashBytes) != 0) {
                                        tell(Difference::Changed, e.path, t, want);
                                } else {
                                        ++counts.good;
                                }
                                ++t;
                                ++m;
                        }
                }
                return counts;
        }

        size_t compareTrees(const RomImage &before, const RomImage &after, VisitChange visit, void *context) {
                size_t   changes = 0;
                uint32_t b = 0;
                uint32_t a = 0;
                auto     tell = [&](Change what, StringView path, uint32_t was, uint32_t is) {
                        ++changes;
                        if (visit != nullptr) visit(context, Changed{what, path, was, is});
                };
                while (b < before.count() || a < after.count()) {
                        const RomImage::Entry was = b < before.count() ? before.entry(b) : RomImage::Entry();
                        const RomImage::Entry is = a < after.count() ? after.entry(a) : RomImage::Entry();
                        if (a >= after.count() || (b < before.count() && was.path < is.path)) {
                                tell(Change::Removed, was.path, b++, RomImage::NoEntry);
                        } else if (b >= before.count() || is.path < was.path) {
                                tell(Change::Added, is.path, RomImage::NoEntry, a++);
                        } else {
                                if (was.data.size() != is.data.size() || was.flags != is.flags ||
                                    std::memcmp(was.hash.data(), is.hash.data(), RomImage::HashBytes) != 0) {
                                        tell(Change::Replaced, is.path, b, a);
                                } else if (was.offset != is.offset) {
                                        tell(Change::Moved, is.path, b, a);
                                }
                                ++b;
                                ++a;
                        }
                }
                return changes;
        }

        bool sameSystemFiles(const RomImage &tree, const RomImage &package) {
                uint32_t matched = 0;
                for (uint32_t i = 0; i < tree.count(); ++i) {
                        const RomImage::Entry e = tree.entry(i);
                        bool                  noPath = false;
                        const auto            found = package.find(e.path, noPath);
                        if ((e.flags & RomImage::User) != 0) {
                                if (found.error().isOk()) return false; // the user's where the system's goes
                                continue;
                        }
                        if (found.error().isError() || found.value().directory) return false;
                        const RomImage::Entry p = package.entry(found.value().index);
                        if (p.data.size() != e.data.size() || p.flags != e.flags ||
                            std::memcmp(p.hash.data(), e.hash.data(), RomImage::HashBytes) != 0)
                                return false;
                        ++matched;
                }
                return matched == package.count();
        }

        const char *text(Change what) {
                switch (what) {
                        case Change::Added: return "added";
                        case Change::Removed: return "removed";
                        case Change::Replaced: return "replaced";
                        case Change::Moved: return "moved";
                }
                return "?";
        }

        const char *text(Difference what) {
                switch (what) {
                        case Difference::Changed: return "changed";
                        case Difference::Replaced: return "replaced";
                        case Difference::Missing: return "missing";
                        case Difference::Extra: return "extra";
                }
                return "?";
        }

} // namespace RomManifest

BMDS_NAMESPACE_END
