/**
 * @file      pkg.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * libFuzzer target for packages (devplan/packages.md): any bytes, read every way: as a package
 * (its head, its icons, then its manifest and its data blocks decoded file by file), as an index
 * (its head, stanzas and icons), as icons, as manifest, dependency list and stanza text, and as
 * versions (lines) compared; an index's stanzas walked as installed packages and candidates.
 * What reads must lie inside the input and hold the rules: valid names, paths and versions, a
 * file's blocks decoding to its size, versions ordered consistently, a plan installing each
 * package once and after what it needs, an index written of any stanza text reading back the
 * same, a key line's comment clean text within it. Seed it with
 * tests/host/pkg.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bmds/pkgfile.h>
#include <bmds/pkgindex.h>
#include <bmds/pkgmanifest.h>
#include <bmds/pkgsolver.h>
#include <bmds/pkgversion.h>

using bmds::Error;
using bmds::StringView;
namespace PkgFile = bmds::PkgFile;
namespace PkgIndex = bmds::PkgIndex;
namespace PkgManifest = bmds::PkgManifest;
namespace PkgDepends = bmds::PkgDepends;
namespace PkgVersion = bmds::PkgVersion;
namespace PkgSolver = bmds::PkgSolver;

// The platform's assert hook (the kernel's on the target): a failed one is a finding.
[[noreturn]] void bmds::Detail::assertFailed(const char *expr, const char *file, int line) {
        std::fprintf(stderr, "BMDS_ASSERT(%s) failed at %s:%d\n", expr, file, line);
        std::abort();
}

namespace {

        // Aborts unless @p v lies within [@p base, @p base + @p size).
        void inside(StringView v, const char *base, size_t size) {
                if (v.isEmpty()) return;
                if (v.data() < base || v.data() + v.size() > base + size) std::abort();
                return;
        }

        uint32_t u32(const uint8_t *p) {
                return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }

        void readDepends(StringView list) {
                PkgDepends::Reader     r(list);
                PkgDepends::Dependency d;
                for (int i = 0; i < 1000 && r.next(d).isOk(); ++i) {
                        inside(d.name, list.data(), list.size());
                        inside(d.minVersion, list.data(), list.size());
                        if (!PkgFile::isValidName(d.name)) std::abort();
                        if (!d.minVersion.isEmpty() && !PkgVersion::isValid(d.minVersion)) std::abort();
                }
                return;
        }

        // The manifest's entries, and each file's blocks decoded from @p data.
        void readManifest(StringView text, const uint8_t *data, size_t dataBytes) {
                static uint8_t      out[PkgFile::BlockBytes];
                PkgManifest::Reader r(text);
                PkgManifest::Entry  e;
                size_t              at = 0;
                bool                dataGood = data != nullptr;
                for (int i = 0; i < 100000 && r.next(e).isOk(); ++i) {
                        inside(e.key, text.data(), text.size());
                        inside(e.value, text.data(), text.size());
                        inside(e.path, text.data(), text.size());
                        inside(e.target, text.data(), text.size());
                        if (e.kind != PkgManifest::Kind::Field && !PkgManifest::isValidPath(e.path)) std::abort();
                        if (e.kind == PkgManifest::Kind::File && e.mode > 07777) std::abort();
                        if (e.kind == PkgManifest::Kind::Field && e.key == StringView("depends")) readDepends(e.value);
                        if (e.kind != PkgManifest::Kind::File || !dataGood) continue;
                        uint64_t left = e.size;
                        while (left > 0 && dataGood) {
                                const size_t   want = left < PkgFile::BlockBytes ? size_t(left) : PkgFile::BlockBytes;
                                PkgFile::Block b;
                                if (at + 4 > dataBytes || PkgFile::readBlock(u32(data + at), want, b).isError() ||
                                    b.bytes > dataBytes - at - 4) {
                                        dataGood = false;
                                        break;
                                }
                                if (b.bytes > PkgFile::BlockBytes || (b.stored && b.bytes != want)) std::abort();
                                if (PkgFile::decodeBlock(b, bmds::ByteSpan(data + at + 4, b.bytes),
                                                         bmds::WritableByteSpan(out, want))
                                            .isError()) {
                                        dataGood = false;
                                        break;
                                }
                                at += 4 + b.bytes;
                                left -= want;
                        }
                }
                return;
        }

        // Every dependency of @p p met by what is installed or planned before it.
        bool met(const PkgSolver::Package &p, const PkgSolver::Package *installed, size_t installedCount,
                 const PkgSolver::Package *before, const bmds::List<PkgSolver::Step> &steps, size_t upTo) {
                PkgDepends::Reader     r(p.depends);
                PkgDepends::Dependency d;
                while (r.next(d).isOk()) {
                        bool found = false;
                        for (size_t k = 0; k < upTo && !found; ++k) {
                                const PkgSolver::Package &q = before[steps[k].candidate];
                                found = q.name == d.name &&
                                        (d.minVersion.isEmpty() || PkgVersion::compare(q.version, d.minVersion) >= 0);
                        }
                        for (size_t k = 0; k < installedCount && !found; ++k) {
                                const PkgSolver::Package &q = installed[k];
                                found = q.name == d.name &&
                                        (d.minVersion.isEmpty() || PkgVersion::compare(q.version, d.minVersion) >= 0);
                        }
                        if (!found) return false;
                }
                return true;
        }

        // Stanzas as packages: the first quarter installed, all candidates; each plan checked.
        void walk(const PkgSolver::Package *all, size_t n) {
                const size_t                installedCount = n / 4;
                const PkgSolver::Package   *installed = all;
                bmds::List<PkgSolver::Step> steps;
                bmds::String                why;
                for (size_t first = 0; first < n && first < 8; ++first) {
                        const size_t requested[2] = {first, n - 1};
                        const Error  err =
                                PkgSolver::plan(bmds::Span<const PkgSolver::Package>(installed, installedCount),
                                                bmds::Span<const PkgSolver::Package>(all, n),
                                                bmds::Span<const size_t>(requested, 2), steps, why);
                        if (err.isError()) {
                                if (why.isEmpty()) std::abort();
                                continue;
                        }
                        bool sawFirst = false, sawLast = false;
                        for (size_t i = 0; i < steps.size(); ++i) {
                                const size_t c = steps[i].candidate;
                                if (c >= n) std::abort();
                                for (size_t j = 0; j < i; ++j) {
                                        if (all[steps[j].candidate].name == all[c].name) std::abort(); // once
                                }
                                if (!met(all[c], installed, installedCount, all, steps, i)) std::abort();
                                sawFirst = sawFirst || (c == first && !steps[i].automatic);
                                sawLast = sawLast || (c == n - 1 && !steps[i].automatic);
                        }
                        if (!sawFirst || !sawLast) std::abort();
                }
                bmds::List<size_t> unneeded;
                const StringView   removing[1] = {all[0].name};
                if (PkgSolver::unneeded(bmds::Span<const PkgSolver::Package>(installed, installedCount),
                                        bmds::Span<const StringView>(removing, 1), unneeded)
                            .isOk()) {
                        for (size_t i : unneeded) {
                                if (i >= installedCount || !installed[i].automatic) std::abort();
                        }
                }
                return;
        }

        void readStanzas(StringView text) {
                static PkgSolver::Package packages[64];
                size_t                    n = 0;
                PkgIndex::Reader          r(text);
                PkgIndex::Stanza          s;
                for (int i = 0; i < 100000 && r.next(s).isOk(); ++i) {
                        if (n < 64) packages[n++] = PkgSolver::Package{s.package, s.version, s.depends, (i & 1) != 0};
                        for (StringView v :
                             {s.package, s.version, s.architecture, s.depends, s.category, s.file, s.description}) {
                                inside(v, text.data(), text.size());
                        }
                        if (!PkgFile::isValidName(s.package) || !PkgVersion::isValid(s.version) ||
                            !PkgFile::isValidArch(s.architecture) || s.file.isEmpty()) {
                                std::abort();
                        }
                        readDepends(s.depends);
                }
                if (n > 0) walk(packages, n);
                // Written as an unsigned index, it reads back the same.
                bmds::List<uint8_t> index;
                if (PkgFile::buildIndex(text, bmds::ByteSpan(), uint32_t(n), 0, "", index).isOk()) {
                        static PkgFile::Index head;
                        if (PkgFile::parseIndex(index.span(), index.size(), head) != PkgFile::Verdict::Ok ||
                            !bmds::SignedContainer::isUnsigned(head))
                                std::abort();
                        const PkgFile::Section *s = head.find(PkgFile::IndexSectionType::Stanzas);
                        if (s == nullptr || s->size != text.size() ||
                            (s->size > 0 && std::memcmp(index.data() + s->offset, text.data(), s->size) != 0))
                                std::abort();
                } else if (!text.isEmpty()) {
                        std::abort(); // only memory may refuse it
                }
                return;
        }

        void readIcons(bmds::ByteSpan section, bool named) {
                PkgFile::IconReader r(section, named);
                PkgFile::Icon       icon;
                const auto         *base = reinterpret_cast<const char *>(section.data());
                for (int i = 0; i < 100000 && r.next(icon).isOk(); ++i) {
                        inside(icon.name, base, section.size());
                        inside(StringView(reinterpret_cast<const char *>(icon.png.data()), icon.png.size()), base,
                               section.size());
                        if (icon.side == 0 || icon.side > PkgFile::MaxIconSide ||
                            icon.png.size() > PkgFile::MaxIconBytes)
                                std::abort();
                        if (named != PkgFile::isValidName(icon.name)) std::abort();
                }
                return;
        }

        int sign(int x) {
                return x < 0 ? -1 : x > 0 ? 1 : 0;
        }

        // The input's first three lines as versions: each valid one equal to itself, and the order
        // antisymmetric and transitive.
        void compareVersions(StringView text) {
                StringView v[3];
                size_t     n = 0;
                size_t     at = 0;
                while (n < 3 && at <= text.size()) {
                        size_t end = text.indexOf('\n', at);
                        if (end == StringView::NPos) end = text.size();
                        v[n] = text.mid(at, end - at);
                        if (!PkgVersion::isValid(v[n])) return;
                        ++n;
                        at = end + 1;
                }
                for (size_t i = 0; i < n; ++i) {
                        if (PkgVersion::compare(v[i], v[i]) != 0) std::abort();
                        for (size_t j = 0; j < n; ++j) {
                                if (sign(PkgVersion::compare(v[i], v[j])) != -sign(PkgVersion::compare(v[j], v[i])))
                                        std::abort();
                        }
                }
                if (n == 3) {
                        const int ab = PkgVersion::compare(v[0], v[1]);
                        const int bc = PkgVersion::compare(v[1], v[2]);
                        const int ac = PkgVersion::compare(v[0], v[2]);
                        if (ab <= 0 && bc <= 0 && ac > 0) std::abort();
                        if (ab >= 0 && bc >= 0 && ac < 0) std::abort();
                }
                return;
        }

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        static PkgFile::Package         p;
        static PkgFile::Index           index;
        const bmds::Span<const uint8_t> bytes(data, size);
        const auto                      text = [&](const PkgFile::Section *s) {
                return StringView(reinterpret_cast<const char *>(data + s->offset), s->size);
        };

        if (PkgFile::parsePackage(bytes, size, p) == PkgFile::Verdict::Ok) {
                if (p.headBytes > size || !PkgFile::isValidName(p.name) || !PkgVersion::isValid(p.version) ||
                    !PkgFile::isValidArch(p.arch)) {
                        std::abort();
                }
                const PkgFile::Section *manifest = p.find(PkgFile::SectionType::Manifest);
                const PkgFile::Section *blocks = p.find(PkgFile::SectionType::Data);
                const PkgFile::Section *icons = p.find(PkgFile::SectionType::Icons);
                if (icons != nullptr) readIcons(bmds::ByteSpan(data + icons->offset, icons->size), false);
                if (manifest != nullptr) {
                        readManifest(text(manifest), blocks != nullptr ? data + blocks->offset : nullptr,
                                     blocks != nullptr ? blocks->size : 0);
                }
        }
        if (PkgFile::parseIndex(bytes, size, index) == PkgFile::Verdict::Ok) {
                const PkgFile::Section *stanzas = index.find(PkgFile::IndexSectionType::Stanzas);
                if (stanzas != nullptr) readStanzas(text(stanzas));
                const PkgFile::Section *icons = index.find(PkgFile::IndexSectionType::Icons);
                if (icons != nullptr) readIcons(bmds::ByteSpan(data + icons->offset, icons->size), true);
        }

        const StringView all(reinterpret_cast<const char *>(data), size);
        readIcons(bytes, false);
        readIcons(bytes, true);
        readManifest(all, nullptr, 0);
        readStanzas(all);
        readDepends(all.left(256));
        compareVersions(all);
        // Key lines: a comment read lies in the line and is clean text.
        for (StringView rest = all.left(4096); !rest.isEmpty();) {
                const size_t     nl = rest.indexOf('\n');
                const StringView line = rest.left(nl);
                rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                bmds::SignedContainer::Key key;
                StringView                 comment;
                if (bmds::SignedContainer::parseKey(line, key, comment).isOk()) {
                        inside(comment, line.data(), line.size());
                        if (!PkgManifest::isCleanText(comment) ||
                            comment.size() > bmds::SignedContainer::MaxKeyCommentBytes)
                                std::abort();
                }
        }
        for (int i = 0; i <= int(PkgFile::Verdict::BadHash); ++i) {
                if (std::strlen(PkgFile::verdictText(PkgFile::Verdict(i))) == 0) std::abort();
        }
        return 0;
}
