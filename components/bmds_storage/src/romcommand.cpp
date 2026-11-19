/**
 * @file      romcommand.cpp
 * @copyright Jason Howard. All rights reserved.
 *
 * SPDX-License-Identifier: MIT
 * See LICENSE file in the project root folder for license information.
 *
 * The `rom` command (devplan/rom.md §5, §12h): everything the ROM's partition holds, shown: the
 * partition and its trees, the files with every field, where each byte of the partition goes,
 * the log of tree versions, what differs from the firmware's manifest or the pending tree, and
 * the files' hashes checked. Each form prints `Name: value` lines or a table with a header, in
 * the same order every time; `--raw` gives exact bytes, offsets and Unix times. Its output format
 * is a userspace interface: keep the lines and their order, and add new ones only at the end.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <bmds/clock.h>
#include <bmds/command.h>
#include <bmds/datetime.h>
#include <bmds/flashwrites.h>
#include <bmds/notifications.h>
#include <bmds/romeditor.h>
#include <bmds/semaphore.h>
#include <bmds/waitlist.h>
#include <bmds/romdisk.h>
#include <bmds/rommanifest.h>
#include <bmds/romspace.h>
#include <bmds/shell.h>
#include <bmds/system.h>
#include <bmds/thread.h>
#include <bmds/list.h>
#include <bmds/memory.h>
#include <bmds/string.h>
#include <bmds/timezone.h>
#include <bmds/vfs.h>

BMDS_NAMESPACE_BEGIN

namespace {

        // The options every form may take, and its operands.
        struct Options {
                        bool              raw = false;
                        bool              longForm = false;  // ls -l
                        bool              recursive = false; // ls -R
                        bool              pending = false;   // ls, diff --pending
                        bool              manifest = false;  // diff --manifest
                        bool              sectors = false;   // map --sectors
                        bool              removeAll = false; // remove -r
                        const char       *align = nullptr;   // install --align SIZE
                        bool              force = false;     // export -f (into a folder with files)
                        bool              fullCompact = false; // compact --full
                        Span<char *const> operands;
                        char             *given[64] = {}; // operands' storage
        };

        // What the forms work on: the tree booted, the pending one (a newer version written
        // since boot) if any, and the firmware's manifest.
        struct Trees {
                        const RomImage *booted = nullptr;
                        RomImage        pendingImage;
                        bool            hasPending = false;

                        const RomImage *pending() const { return hasPending ? &pendingImage : nullptr; }
        };

        int noRom(CommandContext &ctx) {
                if (RomDisk::partitionBytes() == 0) {
                        ctx.eprintf("rom: no flash partition '%s'\n", RomDisk::PartitionLabel);
                } else {
                        ctx.eprintf("rom: no ROM image in its partition (blank or damaged: see 'log show')\n");
                }
                return Shell::ExitFailure;
        }

        bool load(CommandContext &ctx, Trees &trees, int &status) {
                trees.booted = RomDisk::image();
                if (trees.booted == nullptr) {
                        status = noRom(ctx);
                        return false;
                }
                // The tree the next boot takes, as it will choose it.
                auto [now, err] = RomImage::open(RomDisk::mapped(), RomDisk::firmware());
                if (err.isOk() && (now.version().sequence != trees.booted->version().sequence ||
                                   now.version().offset != trees.booted->version().offset)) {
                        trees.pendingImage = now;
                        trees.hasPending = true;
                }
                return true;
        }

        // Bytes as "N KB" (rounded up), or exactly with --raw.
        const char *bytesText(char (&out)[32], const Options &o, uint64_t bytes) {
                if (o.raw) {
                        std::snprintf(out, sizeof(out), "%llu", static_cast<unsigned long long>(bytes));
                } else if (bytes < 1024) {
                        std::snprintf(out, sizeof(out), "%llu B", static_cast<unsigned long long>(bytes));
                } else {
                        std::snprintf(out, sizeof(out), "%llu KB",
                                      static_cast<unsigned long long>((bytes + 1023) / 1024));
                }
                return out;
        }

        // A time in the display zone, or Unix seconds with --raw.
        const char *timeText(char (&out)[40], CommandContext &ctx, const Options &o, Timestamp t) {
                if (o.raw) {
                        std::snprintf(out, sizeof(out), "%lld", static_cast<long long>(t.seconds()));
                        return out;
                }
                String text;
                if (formatTime(text, "%Y-%m-%d %H:%M:%S", ctx.displayZone().localTime(t)).isError()) {
                        std::snprintf(out, sizeof(out), "%lld", static_cast<long long>(t.seconds()));
                } else {
                        std::snprintf(out, sizeof(out), "%s", text.cStr());
                }
                return out;
        }

        const char *hexText(char *out, Span<const uint8_t> bytes, size_t n) {
                for (size_t i = 0; i < n && i < bytes.size(); ++i) {
                        out[2 * i] = "0123456789abcdef"[bytes[i] >> 4];
                        out[2 * i + 1] = "0123456789abcdef"[bytes[i] & 0xf];
                }
                out[2 * (n < bytes.size() ? n : bytes.size())] = '\0';
                return out;
        }

        bool isZero(Span<const uint8_t> bytes) {
                for (uint8_t b : bytes)
                        if (b != 0) return false;
                return true;
        }

        // "for any firmware", "for this firmware", "for firmware 1234abcd...".
        const char *firmwareText(char (&out)[48], Span<const uint8_t> needs) {
                if (needs.size() != RomImage::HashBytes || isZero(needs)) return "any firmware";
                if (std::memcmp(needs.data(), RomDisk::firmware().data(), RomImage::HashBytes) == 0)
                        return "this firmware";
                char id[17];
                std::snprintf(out, sizeof(out), "firmware %s", hexText(id, needs, 8));
                return out;
        }

        // A path as given ("/rom/bin/x", "bin/x", "/" ...) as the tree's ("bin/x", "" the root).
        StringView treePath(const char *given) {
                StringView p(given);
                if (p == RomDisk::Path) return StringView();
                if (p.startsWith(RomDisk::Path) && p.size() > 4 && p[4] == '/') p = p.mid(4);
                while (p.startsWith('/')) p = p.mid(1);
                return p;
        }

        const char *ownerText(uint8_t flags) {
                return (flags & RomImage::User) != 0 ? "user" : "system";
        }

        uint32_t flashAddress(uint32_t offset) {
                const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY,
                                                                    RomDisk::PartitionLabel);
                return p != nullptr ? static_cast<uint32_t>(p->address) + offset : offset;
        }

        // --- info ---------------------------------------------------------------------------

        int showInfo(CommandContext &ctx, const Options &o) {
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                const RomImage &image = *trees.booted;
                RomSpace        space;
                if (space.build(image, trees.pending()).isError()) {
                        ctx.eprintf("rom: out of memory\n");
                        return Shell::ExitFailure;
                }
                const RomSpace::Summary  &sum = space.summary();
                const RomImage::Version  &v = image.version();
                const RomManifest::Counts counts = RomDisk::manifestCounts();
                char                      a[32], b[32], c[32], when[40], fw[48], id[65];
                const esp_partition_t    *part = esp_partition_find_first(
                        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, RomDisk::PartitionLabel);
                uint32_t logVersions = 0;
                uint32_t logErased = 0;
                for (uint32_t at = image.logOffset(); at < image.logOffset() + image.logBytes();) {
                        RomImage::LogRecord r;
                        at = image.logRecord(at, r);
                        if (r.kind == RomImage::LogRecord::Kind::Version) ++logVersions;
                        if (r.kind == RomImage::LogRecord::Kind::Erased) ++logErased;
                }
                ctx.printf("ROM:          %s, format 2, %u files\n", RomDisk::Path,
                           static_cast<unsigned>(image.count()));
                ctx.printf("Partition:    %s at 0x%x in flash, %s\n", RomDisk::PartitionLabel,
                           part != nullptr ? static_cast<unsigned>(part->address) : 0u,
                           bytesText(a, o, sum.partitionBytes));
                ctx.printf("Log:          at 0x%x, %s: %u version%s, %u sector%s erased\n",
                           static_cast<unsigned>(image.logOffset()), bytesText(a, o, image.logBytes()),
                           static_cast<unsigned>(logVersions), logVersions == 1 ? "" : "s",
                           static_cast<unsigned>(logErased), logErased == 1 ? "" : "s");
                ctx.printf("Files from:   0x%x\n", static_cast<unsigned>(image.dataOffset()));
                ctx.printf("Tree:         version %u at 0x%x, %s, written %s, for %s\n",
                           static_cast<unsigned>(v.sequence), static_cast<unsigned>(v.offset), bytesText(a, o, v.bytes),
                           timeText(when, ctx, o, v.written), firmwareText(fw, v.firmware));
                if (trees.hasPending) {
                        const RomImage::Version &p = trees.pendingImage.version();
                        const bool same = RomManifest::compareTrees(image, trees.pendingImage, nullptr, nullptr) == 0;
                        ctx.printf("Pending:      version %u at 0x%x, %u files, written %s (%s)\n",
                                   static_cast<unsigned>(p.sequence), static_cast<unsigned>(p.offset),
                                   static_cast<unsigned>(trees.pendingImage.count()), timeText(when, ctx, o, p.written),
                                   same ? "the booted tree's files: nothing changes" : "rom diff --pending");
                } else {
                        ctx.printf("Pending:      none\n");
                }
                ctx.printf("Used:         %s in %u files\n", bytesText(a, o, sum.filesBytes),
                           static_cast<unsigned>(image.count()));
                ctx.printf("Free now:     %s (largest %s, %llu whole 64 KB pages)\n", bytesText(a, o, sum.freeNow),
                           bytesText(b, o, sum.largestNow),
                           static_cast<unsigned long long>(sum.largestPagesNow / RomImage::PageBytes));
                ctx.printf("After boot:   %s free\n", bytesText(a, o, sum.freeAfterBoot));
                ctx.printf("Holes:        %s; padding %s; overlaps %u\n", bytesText(b, o, sum.holeBytes),
                           bytesText(c, o, sum.paddingBytes), static_cast<unsigned>(sum.overlaps));
                ctx.printf("Manifest:     %u system files as built, %u changed, %u replaced, %u missing, %u extra; "
                           "%u of the user's\n",
                           static_cast<unsigned>(counts.good), static_cast<unsigned>(counts.changed),
                           static_cast<unsigned>(counts.replaced), static_cast<unsigned>(counts.missing),
                           static_cast<unsigned>(counts.extra), static_cast<unsigned>(counts.user));
                ctx.printf("Matches:      %s\n",
                           counts.matches() ? "the firmware" : "not the firmware's (rom diff; an update restores it)");
                const esp_partition_t *running = esp_ota_get_running_partition();
                if (running != nullptr) {
                        const bool main = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0;
                        ctx.printf("Running:      %s (%s) at 0x%x, build %s\n", running->label,
                                   main ? "main" : "recovery", static_cast<unsigned>(running->address),
                                   hexText(id, RomDisk::firmware(), o.raw ? RomImage::HashBytes : 8));
                }
                return Shell::ExitSuccess;
        }

        // --- ls -------------------------------------------------------------------------------

        void lsHeader(CommandContext &ctx) {
                ctx.printf("%-6s %-4s %10s %6s %10s %10s %-19s %-16s %s\n", "OWNER", "KIND", "SIZE", "ALIGN", "OFFSET",
                           "FLASH", "TIME", "HASH", "PATH");
                return;
        }

        void lsFile(CommandContext &ctx, const Options &o, const RomImage &image, uint32_t index, StringView shown) {
                const RomImage::Entry e = image.entry(index);
                if (!o.longForm) {
                        ctx.printf("%.*s\n", static_cast<int>(shown.size()), shown.data());
                        return;
                }
                char when[40], hash[33], size[24];
                std::snprintf(size, sizeof(size), "%llu", static_cast<unsigned long long>(e.data.size()));
                ctx.printf("%-6s %-4s %10s %6u   0x%06x   0x%06x %-19s %-16s %.*s\n", ownerText(e.flags),
                           (e.flags & RomImage::System) != 0 ? "link" : "file", size, 1u << e.alignLog2,
                           static_cast<unsigned>(e.offset), static_cast<unsigned>(flashAddress(e.offset)),
                           timeText(when, ctx, o, e.modified), hexText(hash, e.hash, 8), static_cast<int>(shown.size()),
                           shown.data());
                return;
        }

        void lsDirectory(CommandContext &ctx, const Options &o, const RomImage &image, StringView shown,
                         Timestamp newest) {
                if (!o.longForm) {
                        ctx.printf("%.*s/\n", static_cast<int>(shown.size()), shown.data());
                        return;
                }
                char when[40];
                ctx.printf("%-6s %-4s %10s %6s %10s %10s %-19s %-16s %.*s/\n", "-", "dir", "-", "-", "-", "-",
                           timeText(when, ctx, o, newest), "-", static_cast<int>(shown.size()), shown.data());
                (void)image;
                return;
        }

        int list(CommandContext &ctx, const Options &o) {
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                const RomImage *image = trees.booted;
                if (o.pending) {
                        if (!trees.hasPending) {
                                ctx.eprintf("rom: no pending tree (nothing has changed since boot)\n");
                                return Shell::ExitFailure;
                        }
                        image = &trees.pendingImage;
                }
                if (o.operands.size() > 1) return ctx.usage();
                const StringView path = o.operands.isEmpty() ? StringView() : treePath(o.operands[0]);
                bool             noPath = false;
                auto [node, err] = image->find(path, noPath);
                if (err.isError()) {
                        ctx.eprintf("rom: %s: no such file or directory in the tree\n",
                                    o.operands.isEmpty() ? "/" : o.operands[0]);
                        return Shell::ExitFailure;
                }
                if (o.longForm) lsHeader(ctx);
                if (!node.directory) {
                        lsFile(ctx, o, *image, node.index, image->entry(node.index).path);
                        return Shell::ExitSuccess;
                }
                if (o.recursive) {
                        // Every file and directory under it, by its whole path.
                        for (uint32_t i = node.first; i < node.end; ++i) {
                                const RomImage::Entry e = image->entry(i);
                                // The directories this file starts (those the one before it isn't under).
                                const StringView before = i > node.first ? image->entry(i - 1).path : StringView();
                                for (size_t at = node.prefixBytes; (at = e.path.indexOf('/', at)) != StringView::NPos;
                                     ++at) {
                                        const StringView dir = e.path.left(at);
                                        if (before.startsWith(dir) && before.size() > dir.size() &&
                                            before[dir.size()] == '/')
                                                continue;
                                        bool       unused = false;
                                        const auto d = image->find(dir, unused);
                                        lsDirectory(ctx, o, *image, dir,
                                                    d.error().isOk() ? image->newest(d.value().first, d.value().end)
                                                                     : Timestamp());
                                }
                                lsFile(ctx, o, *image, i, e.path);
                        }
                        return Shell::ExitSuccess;
                }
                RomImage::Child child;
                for (uint32_t at = node.first; (at = image->nextChild(node, at, child)) != RomImage::NoEntry;) {
                        if (child.node.directory) {
                                lsDirectory(ctx, o, *image, child.name,
                                            image->newest(child.node.first, child.node.end));
                        } else {
                                lsFile(ctx, o, *image, child.node.index, child.name);
                        }
                }
                return Shell::ExitSuccess;
        }

        // --- stat ---------------------------------------------------------------------------

        const RomManifestEntry *manifestEntry(StringView path) {
                const Span<const RomManifestEntry> m = RomDisk::manifest();
                size_t                             low = 0;
                size_t                             high = m.size();
                while (low < high) {
                        const size_t     mid = low + (high - low) / 2;
                        const StringView p(m[mid].path);
                        if (p == path) return &m[mid];
                        if (p < path) {
                                low = mid + 1;
                        } else {
                                high = mid;
                        }
                }
                return nullptr;
        }

        int statPaths(CommandContext &ctx, const Options &o) {
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                if (o.operands.isEmpty()) return ctx.usage();
                const RomImage &image = *trees.booted;
                RomSpace        space;
                if (space.build(image, trees.pending()).isError()) {
                        ctx.eprintf("rom: out of memory\n");
                        return Shell::ExitFailure;
                }
                for (size_t k = 0; k < o.operands.size(); ++k) {
                        if (k > 0) ctx.printf("\n");
                        const StringView path = treePath(o.operands[k]);
                        bool             noPath = false;
                        auto [node, err] = image.find(path, noPath);
                        if (err.isError()) {
                                ctx.eprintf("rom: %s: no such file or directory in the tree\n", o.operands[k]);
                                status = Shell::ExitFailure;
                                continue;
                        }
                        char a[32], b[32], when[40];
                        ctx.printf("Path:        %s/%.*s\n", RomDisk::Path, static_cast<int>(path.size()), path.data());
                        if (node.directory) {
                                ctx.printf("Kind:        directory\n");
                                ctx.printf("Files:       %u under it\n", static_cast<unsigned>(node.end - node.first));
                                uint64_t bytes = 0;
                                for (uint32_t i = node.first; i < node.end; ++i) bytes += image.entry(i).data.size();
                                ctx.printf("Size:        %s in them\n", bytesText(a, o, bytes));
                                ctx.printf("Time:        %s (the newest under it)\n",
                                           timeText(when, ctx, o, image.newest(node.first, node.end)));
                                continue;
                        }
                        const RomImage::Entry e = image.entry(node.index);
                        if ((e.flags & RomImage::System) != 0) {
                                String full;
                                (void)full.assign(RomDisk::Path);
                                (void)full.append("/");
                                (void)full.append(path);
                                auto [target, lerr] = Vfs::readLink(full.view(), ctx.vfs());
                                ctx.printf("Kind:        link to %s\n", lerr.isOk() ? target.cStr() : "(unreadable)");
                        } else {
                                ctx.printf("Kind:        file\n");
                        }
                        ctx.printf("Owner:       %s\n", ownerText(e.flags));
                        ctx.printf("Flags:       0x%02x\n", e.flags);
                        ctx.printf("Size:        %llu bytes\n", static_cast<unsigned long long>(e.size()));
                        if (e.compression == RomImage::None) {
                                ctx.printf("Stored:      as it is\n");
                        } else {
                                ctx.printf("Stored:      %llu bytes, compressed (method %u: not read by this firmware)\n",
                                           static_cast<unsigned long long>(e.data.size()),
                                           static_cast<unsigned>(e.compression));
                        }
                        ctx.printf("Alignment:   %u bytes\n", 1u << e.alignLog2);
                        ctx.printf("Offset:      0x%x in the partition\n", static_cast<unsigned>(e.offset));
                        ctx.printf("Flash:       0x%x\n", static_cast<unsigned>(flashAddress(e.offset)));
                        ctx.printf("Mapped at:   %p\n", static_cast<const void *>(e.data.data()));
                        const uint32_t first = e.offset / RomImage::SectorBytes;
                        const uint32_t last =
                                e.data.isEmpty()
                                        ? first
                                        : static_cast<uint32_t>((e.offset + e.data.size() - 1) / RomImage::SectorBytes);
                        ctx.printf(
                                "Sectors:     %u to %u (%u: 0x%06x to 0x%06x), %u of them shared with another file\n",
                                static_cast<unsigned>(first), static_cast<unsigned>(last),
                                static_cast<unsigned>(last - first + 1),
                                static_cast<unsigned>(first * RomImage::SectorBytes),
                                static_cast<unsigned>((last + 1) * RomImage::SectorBytes - 1), [&] {
                                        unsigned shared = 0;
                                        for (uint32_t s = first; s <= last && s < space.sectorCount(); ++s)
                                                shared += space.sectorFiles(s) > 1 ? 1 : 0;
                                        return shared;
                                }());
                        // The free bytes after it, before the next file (its padding).
                        uint64_t padding = 0;
                        for (size_t r = 0; r + 1 < space.regionCount(); ++r) {
                                const RomSpace::Region &here = space.region(r);
                                if (here.kind == RomSpace::Kind::File && here.image == &image &&
                                    here.index == node.index) {
                                        const RomSpace::Region &next = space.region(r + 1);
                                        if (next.kind == RomSpace::Kind::Free && r + 2 < space.regionCount())
                                                padding = next.bytes;
                                        break;
                                }
                        }
                        ctx.printf("Padding:     %s after it\n", bytesText(b, o, padding));
                        ctx.printf("Time:        %s\n", timeText(when, ctx, o, e.modified));
                        char hash[65];
                        ctx.printf("Hash:        %s\n", hexText(hash, e.hash, RomImage::HashBytes));
                        const RomManifestEntry *m = manifestEntry(path);
                        if (m == nullptr) {
                                ctx.printf("Manifest:    not listed\n");
                        } else if (m->size == e.data.size() &&
                                   (m->flags & RomImage::System) == (e.flags & RomImage::System) &&
                                   std::memcmp(m->hash, e.hash.data(), RomImage::HashBytes) == 0 &&
                                   (e.flags & RomImage::User) == 0) {
                                ctx.printf("Manifest:    as built\n");
                        } else {
                                ctx.printf("Manifest:    differs: built %u bytes, %s\n", static_cast<unsigned>(m->size),
                                           hexText(hash, Span<const uint8_t>(m->hash, RomImage::HashBytes),
                                                   RomImage::HashBytes));
                        }
                        const char *state = "as booted";
                        if (trees.hasPending) {
                                bool       unused = false;
                                const auto p = trees.pendingImage.find(path, unused);
                                if (p.error().isError() || p.value().directory) {
                                        state = "removed in the pending tree";
                                } else {
                                        const RomImage::Entry n = trees.pendingImage.entry(p.value().index);
                                        if (n.data.size() != e.data.size() || n.flags != e.flags ||
                                            std::memcmp(n.hash.data(), e.hash.data(), RomImage::HashBytes) != 0) {
                                                state = "replaced in the pending tree";
                                        } else if (n.offset != e.offset) {
                                                state = "moved in the pending tree";
                                        } else {
                                                state = "as booted, the same in the pending tree";
                                        }
                                }
                        }
                        ctx.printf("State:       %s\n", state);
                }
                return status;
        }

        // --- map ------------------------------------------------------------------------------

        bool isErased(uint32_t sector) {
                const ByteSpan all = RomDisk::mapped();
                const uint64_t at = uint64_t(sector) * RomImage::SectorBytes;
                if (at + RomImage::SectorBytes > all.size()) return false;
                const uint32_t *w = reinterpret_cast<const uint32_t *>(all.data() + at);
                for (uint32_t i = 0; i < RomImage::SectorBytes / 4; ++i)
                        if (w[i] != 0xffffffffu) return false;
                return true;
        }

        const char *treesText(uint8_t trees, bool hasPending) {
                if (!hasPending) return "";
                switch (trees) {
                        case RomSpace::Booted: return " (booted only: freed at the next boot)";
                        case RomSpace::Pending: return " (pending only: written since boot)";
                        default: return " (booted and pending)";
                }
        }

        int showMap(CommandContext &ctx, const Options &o) {
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                RomSpace space;
                if (space.build(*trees.booted, trees.pending()).isError()) {
                        ctx.eprintf("rom: out of memory\n");
                        return Shell::ExitFailure;
                }
                char a[32];
                if (o.sectors) {
                        // Runs of sectors alike: reserved, used by one file or several, freed at the
                        // next boot, free (erased or not).
                        ctx.printf("%-8s %-8s %6s %s\n", "FIRST", "LAST", "COUNT", "STATE");
                        const uint32_t reserved = trees.booted->dataOffset() / RomImage::SectorBytes;
                        auto           state = [&](uint32_t s) -> const char           *{
                                if (s < reserved) return "header and log";
                                const uint8_t f = space.sectorFlags(s);
                                if ((f & RomSpace::UsedNow) == 0)
                                        return isErased(s) ? "free, erased" : "free, not erased";
                                const bool several = space.sectorFiles(s) > 1;
                                if ((f & RomSpace::UsedAfter) == 0)
                                        return several ? "files, freed at the next boot"
                                                                 : "a file, freed at the next boot";
                                return several ? "files" : "a file";
                        };
                        uint32_t    start = 0;
                        const char *current = space.sectorCount() > 0 ? state(0) : "";
                        for (uint32_t s = 1; s <= space.sectorCount(); ++s) {
                                const char *next = s < space.sectorCount() ? state(s) : nullptr;
                                if (next != nullptr && std::strcmp(next, current) == 0) continue;
                                ctx.printf("0x%06x 0x%06x %6u %s\n",
                                           static_cast<unsigned>(start * RomImage::SectorBytes),
                                           static_cast<unsigned>(s * RomImage::SectorBytes - 1),
                                           static_cast<unsigned>(s - start), current);
                                start = s;
                                current = next;
                        }
                        return Shell::ExitSuccess;
                }
                ctx.printf("%-10s %-10s %10s %s\n", "OFFSET", "END", "SIZE", "WHAT");
                for (size_t i = 0; i < space.regionCount(); ++i) {
                        const RomSpace::Region &r = space.region(i);
                        ctx.printf("0x%06x   0x%06x   %10s ", static_cast<unsigned>(r.offset),
                                   static_cast<unsigned>(r.offset + r.bytes), bytesText(a, o, r.bytes));
                        switch (r.kind) {
                                case RomSpace::Kind::Header: ctx.printf("header\n"); break;
                                case RomSpace::Kind::Log: ctx.printf("log of tree versions (rom log)\n"); break;
                                case RomSpace::Kind::Reserved: ctx.printf("reserved\n"); break;
                                case RomSpace::Kind::Free: ctx.printf("free\n"); break;
                                case RomSpace::Kind::File: {
                                        const RomImage::Entry e = r.image->entry(r.index);
                                        ctx.printf("%s %.*s, %s%s%s\n",
                                                   (e.flags & RomImage::System) != 0 ? "link" : "file",
                                                   static_cast<int>(e.path.size()), e.path.data(), ownerText(e.flags),
                                                   treesText(r.trees, trees.hasPending),
                                                   r.overlaps ? ", OVERLAPS the one before" : "");
                                        break;
                                }
                        }
                }
                return Shell::ExitSuccess;
        }

        // --- log ------------------------------------------------------------------------------

        int showLog(CommandContext &ctx, const Options &o) {
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                const RomImage &image = *trees.booted;
                ctx.printf("%-10s %7s %-8s %8s %-19s %6s %-18s %s\n", "OFFSET", "SECTORS", "KIND", "SEQUENCE",
                           "WRITTEN", "FILES", "FOR", "STATE");
                uint32_t erased = 0;
                uint32_t other = 0;
                for (uint32_t at = image.logOffset(); at < image.logOffset() + image.logBytes();) {
                        RomImage::LogRecord r;
                        const uint32_t      next = image.logRecord(at, r);
                        at = next;
                        using Kind = RomImage::LogRecord::Kind;
                        if (r.kind == Kind::Erased) {
                                ++erased;
                                continue;
                        }
                        if (r.kind == Kind::Other) {
                                ++other;
                                continue;
                        }
                        const unsigned sectors = (r.bytes + RomImage::SectorBytes - 1) / RomImage::SectorBytes;
                        char           when[40], fw[48];
                        if (r.kind == Kind::Damaged) {
                                ctx.printf("0x%06x   %7u %-8s %8u %-19s %6s %-18s %s\n",
                                           static_cast<unsigned>(r.offset), sectors, "damaged",
                                           static_cast<unsigned>(r.sequence), "-", "-", "-",
                                           "not whole (cut short or damaged): never used");
                                continue;
                        }
                        const char *state = "superseded";
                        if (r.kind == Kind::Wrong) {
                                state = "refused (whole, but its entries aren't good)";
                        } else if (r.offset == image.version().offset) {
                                state = "booted";
                        } else if (trees.hasPending && r.offset == trees.pendingImage.version().offset) {
                                state = "pending (the next boot takes it)";
                        } else if (r.sequence > (trees.hasPending ? trees.pendingImage : image).version().sequence) {
                                // Newer than what the next boot takes, which passes it over.
                                state = "newer, passed over (for another firmware)";
                        }
                        ctx.printf("0x%06x   %7u %-8s %8u %-19s %6u %-18s %s\n", static_cast<unsigned>(r.offset),
                                   sectors, r.kind == Kind::Wrong ? "wrong" : "version",
                                   static_cast<unsigned>(r.sequence), timeText(when, ctx, o, r.written),
                                   static_cast<unsigned>(r.count), firmwareText(fw, r.firmware), state);
                }
                ctx.printf("%u sectors erased (room for the next versions), %u others\n", static_cast<unsigned>(erased),
                           static_cast<unsigned>(other));
                return Shell::ExitSuccess;
        }

        // --- diff, manifest --------------------------------------------------------------------

        int showDiff(CommandContext &ctx, const Options &o) {
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                if (!o.operands.isEmpty() || (o.pending && o.manifest)) return ctx.usage();
                if (o.pending) {
                        if (!trees.hasPending) {
                                ctx.printf("rom: no pending tree: the next boot takes the booted one\n");
                                return Shell::ExitSuccess;
                        }
                        const size_t n = RomManifest::compareTrees(
                                *trees.booted, trees.pendingImage,
                                [](void *c, const RomManifest::Changed &ch) {
                                        static_cast<CommandContext *>(c)->printf(
                                                "%-8s %s/%.*s\n", RomManifest::text(ch.what), RomDisk::Path,
                                                static_cast<int>(ch.path.size()), ch.path.data());
                                },
                                &ctx);
                        ctx.printf("rom: %u paths change at the next boot\n", static_cast<unsigned>(n));
                        return Shell::ExitSuccess;
                }
                const RomManifest::Counts c = RomManifest::compare(
                        *trees.booted, RomDisk::manifest(),
                        [](void *cx, const RomManifest::Found &f) {
                                static_cast<CommandContext *>(cx)->printf(
                                        "%-8s %s/%.*s\n", RomManifest::text(f.what), RomDisk::Path,
                                        static_cast<int>(f.path.size()), f.path.data());
                        },
                        &ctx);
                ctx.printf("rom: %u system files as built, %u changed, %u replaced, %u missing, %u extra; %u of the "
                           "user's\n",
                           static_cast<unsigned>(c.good), static_cast<unsigned>(c.changed),
                           static_cast<unsigned>(c.replaced), static_cast<unsigned>(c.missing),
                           static_cast<unsigned>(c.extra), static_cast<unsigned>(c.user));
                return c.matches() ? Shell::ExitSuccess : Shell::ExitFailure;
        }

        int showManifest(CommandContext &ctx, const Options &o) {
                const Span<const RomManifestEntry> m = RomDisk::manifest();
                ctx.printf("%10s %-4s %-64s %s\n", "SIZE", "KIND", "HASH", "PATH");
                uint64_t total = 0;
                for (const RomManifestEntry &e : m) {
                        char hash[65];
                        ctx.printf("%10u %-4s %-64s %s/%s\n", static_cast<unsigned>(e.size),
                                   (e.flags & RomImage::System) != 0 ? "link" : "file",
                                   hexText(hash, Span<const uint8_t>(e.hash, RomImage::HashBytes), RomImage::HashBytes),
                                   RomDisk::Path, e.path);
                        total += e.size;
                }
                char a[32];
                ctx.printf("rom: the firmware was built with %u system files, %s\n", static_cast<unsigned>(m.size()),
                           bytesText(a, o, total));
                return Shell::ExitSuccess;
        }

        // --- check ----------------------------------------------------------------------------

        int check(CommandContext &ctx, const Options &o) {
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                const RomImage &image = *trees.booted;
                const int64_t   start = Clock::monotonicNs();
                uint32_t        checked = 0;
                uint32_t        bad = 0;
                uint64_t        bytes = 0;
                auto            one = [&](uint32_t i) {
                        const RomImage::Entry e = image.entry(i);
                        ++checked;
                        bytes += e.data.size();
                        if (image.verifyFile(i).isError()) {
                                ++bad;
                                ctx.printf("damaged  %s/%.*s (its hash differs from its entry's)\n", RomDisk::Path,
                                                      static_cast<int>(e.path.size()), e.path.data());
                        }
                };
                if (o.operands.isEmpty()) {
                        for (uint32_t i = 0; i < image.count(); ++i) one(i);
                } else {
                        for (char *given : o.operands) {
                                bool       noPath = false;
                                const auto found = image.find(treePath(given), noPath);
                                if (found.error().isError()) {
                                        ctx.eprintf("rom: %s: no such file or directory in the tree\n", given);
                                        status = Shell::ExitFailure;
                                        continue;
                                }
                                if (found.value().directory) {
                                        for (uint32_t i = found.value().first; i < found.value().end; ++i) one(i);
                                } else {
                                        one(found.value().index);
                                }
                        }
                }
                const int64_t ms = (Clock::monotonicNs() - start) / 1000000;
                char          a[32];
                if (bad > 0) {
                        ctx.eprintf("rom: %u of %u files damaged: flash it again (an update, or idf.py flash)\n",
                                    static_cast<unsigned>(bad), static_cast<unsigned>(checked));
                        return Shell::ExitFailure;
                }
                ctx.printf("rom: content good (%u files, %s hashed in %ld ms)\n", static_cast<unsigned>(checked),
                           bytesText(a, o, bytes), static_cast<long>(ms));
                if (o.operands.isEmpty()) {
                        const RomManifest::Counts c = RomDisk::manifestCounts();
                        if (!c.matches()) ctx.printf("rom: but the system files aren't the firmware's (rom diff)\n");
                }
                return status;
        }

        // --- install, remove, discard, compact (§12b, §12d) -----------------------------------

        // A file on any filesystem as an install's bytes.
        class FileSource final : public RomEditor::Source {
                public:
                        FileSource(FileHandle::Ptr file, uint64_t size) : _file(std::move(file)), _size(size) {}
                        uint64_t       size() const override { return _size; }
                        Result<size_t> read(WritableByteSpan buf) override {
                                auto [n, err] = _file->read(buf, WaitForever);
                                if (err == Error::EndOfFile) return makeResult(size_t(0));
                                return err.isError() ? makeError<size_t>(err) : makeResult(n);
                        }

                private:
                        FileHandle::Ptr _file;
                        uint64_t        _size;
        };

        // Flash writes stall everything reading flash: none while audio holds them (as /flash's).
        bool waitForFlash(CommandContext &ctx) {
                if (!FlashWrites::isHeld()) return true;
                ctx.printf("rom: waiting: audio is playing (a flash write would stall it)\n");
                Semaphore       wake(1, 0);
                WaitList::Entry entry(FlashWrites::releases(), wake);
                while (FlashWrites::isHeld()) {
                        if (ctx.isInterrupted()) return false;
                        (void)wake.acquire(500); // Ctrl+C looked at twice a second
                }
                return true;
        }

        // Progress for a large change: a line at each quarter.
        struct Progress {
                        CommandContext *ctx;
                        unsigned        quarter = 0;
        };

        void tellProgress(void *context, uint64_t done, uint64_t total) {
                auto &p = *static_cast<Progress *>(context);
                if (total < 512 * 1024) return;
                const unsigned q = static_cast<unsigned>(done * 4 / total);
                if (q <= p.quarter || q == 0) return;
                p.quarter = q;
                p.ctx->printf("rom: written %u KB of %u KB\n", static_cast<unsigned>(done / 1024),
                              static_cast<unsigned>(total / 1024));
        }

        void tellShortfall(CommandContext &ctx, const RomEditor::Shortfall &s) {
                Options raw;
                char    a[32], b[32], c[32], d[32];
                ctx.eprintf("rom: no room: needs %s together; %s free now, the largest run %s; %s after the next "
                            "boot (rom remove, rom compact, then reboot)\n",
                            bytesText(a, raw, s.needed), bytesText(b, raw, s.freeNow), bytesText(c, raw, s.largestNow),
                            bytesText(d, raw, s.freeAfterBoot));
                return;
        }

        void notifyPending(const char *what);

        void tellDone(CommandContext &ctx, const char *what, const RomEditor::Report &r) {
                ctx.printf("rom: %s; tree version %u written: it takes effect at the next boot (rom diff --pending)\n",
                           what, static_cast<unsigned>(r.sequence));
                char sentence[128];
                std::snprintf(sentence, sizeof(sentence), "%s", what);
                if (sentence[0] >= 'a' && sentence[0] <= 'z') sentence[0] = char(sentence[0] - 'a' + 'A');
                notifyPending(sentence);
                return;
        }

        int failed(CommandContext &ctx, Error err, const char *verb, StringView path) {
                if (!path.isEmpty()) {
                        ctx.eprintf("rom: %s %s/%.*s: %s\n", verb, RomDisk::Path, static_cast<int>(path.size()),
                                    path.data(),
                                    err == Error::Invalid ? "not a path a file may have there (a directory, a file "
                                                            "along it, given twice, or . and ..)"
                                                          : CommandContext::errorText(err));
                } else {
                        ctx.eprintf("rom: %s: %s\n", verb, CommandContext::errorText(err));
                }
                return Shell::ExitFailure;
        }

        struct InstallJob {
                        CommandContext            *ctx;
                        Span<const RomEditor::Add> adds;
                        RomEditor::Report          report;
                        RomEditor::Shortfall       shortfall;
                        String                     failedPath;
        };

        int install(CommandContext &ctx, const Options &o) {
                if (o.operands.size() < 2) return ctx.usage();
                uint8_t alignLog2 = 6;
                if (o.align != nullptr) {
                        char         *end = nullptr;
                        unsigned long n = std::strtoul(o.align, &end, 0);
                        if (end != nullptr && (*end == 'k' || *end == 'K')) {
                                n *= 1024;
                                ++end;
                        }
                        if (end == nullptr || *end != '\0' || n < 1 || n > (1ul << RomImage::MaxAlignLog2) ||
                            (n & (n - 1)) != 0) {
                                ctx.eprintf("rom: --align %s: a power of two (64, 4k, 64k) up to 16 MB\n", o.align);
                                return Shell::ExitUsage;
                        }
                        alignLog2 = 0;
                        while ((1ul << alignLog2) < n) ++alignLog2;
                        if (alignLog2 < 6) alignLog2 = 6; // a cache line at least
                }
                // DEST: a directory with several sources, a trailing '/', or one in the tree; else
                // the file's path.
                const size_t     sources = o.operands.size() - 1;
                const char      *dest = o.operands[sources];
                const StringView destPath = treePath(dest);
                bool             intoDir = sources > 1 || StringView(dest).endsWith('/') || destPath.isEmpty();
                if (!intoDir) {
                        bool unused = false;
                        auto [now, err] = RomImage::open(RomDisk::mapped(), RomDisk::firmware());
                        if (err.isOk()) {
                                const auto there = now.find(destPath, unused);
                                intoDir = there.error().isOk() && there.value().directory;
                        }
                }
                List<String>         paths;
                List<FileSource>     files;
                List<RomEditor::Add> adds;
                if (paths.reserve(sources).isError() || files.reserve(sources).isError() ||
                    adds.reserve(sources).isError()) {
                        ctx.eprintf("rom: out of memory\n");
                        return Shell::ExitFailure;
                }
                const Timestamp now = Clock::now();
                for (size_t i = 0; i < sources; ++i) {
                        const char *src = o.operands[i];
                        auto [info, serr] = Vfs::stat(src, ctx.vfs());
                        if (serr.isError() || info.isDirectory()) {
                                ctx.eprintf("rom: %s: %s\n", src,
                                            serr.isError() ? CommandContext::errorText(serr)
                                                           : "a directory (give its files)");
                                return Shell::ExitFailure;
                        }
                        auto [file, oerr] = Vfs::open(src, Vfs::Read, ctx.vfs());
                        if (oerr.isError()) {
                                ctx.eprintf("rom: %s: %s\n", src, CommandContext::errorText(oerr));
                                return Shell::ExitFailure;
                        }
                        String path;
                        if (intoDir) {
                                StringView   name(src);
                                const size_t slash = name.lastIndexOf('/');
                                if (slash != StringView::NPos) name = name.mid(slash + 1);
                                (void)path.assign(destPath);
                                if (!destPath.isEmpty() && !destPath.endsWith('/')) (void)path.append("/");
                                (void)path.append(name);
                        } else {
                                (void)path.assign(destPath);
                        }
                        (void)paths.append(std::move(path));
                        (void)files.append(FileSource(std::move(file), info.size));
                }
                for (size_t i = 0; i < sources; ++i) {
                        RomEditor::Add a;
                        a.path = paths[i].view();
                        a.source = &files[i];
                        a.alignLog2 = alignLog2;
                        a.flags = RomImage::User;
                        a.modified = now;
                        (void)adds.append(a);
                }
                if (!waitForFlash(ctx)) return Shell::ExitFailure;
                InstallJob  job{&ctx, Span<const RomEditor::Add>(adds.data(), adds.size()), {}, {}, {}};
                const Error err = RomDisk::edit(
                        [](RomEditor &editor, void *c) {
                                auto    &j = *static_cast<InstallJob *>(c);
                                Progress p{j.ctx};
                                editor.setProgress(tellProgress, &p);
                                const Error e = editor.install(j.adds, j.report);
                                j.shortfall = editor.shortfall();
                                (void)j.failedPath.assign(editor.failedPath());
                                return e;
                        },
                        &job);
                if (err == Error::NoSpace) {
                        tellShortfall(ctx, job.shortfall);
                        return Shell::ExitFailure;
                }
                if (err.isError()) return failed(ctx, err, "install", job.failedPath.view());
                char what[96], kb[32];
                std::snprintf(what, sizeof(what), "%u file%s installed (%s)", static_cast<unsigned>(job.report.written),
                              job.report.written == 1 ? "" : "s", bytesText(kb, o, job.report.bytesWritten));
                tellDone(ctx, what, job.report);
                return Shell::ExitSuccess;
        }

        struct RemoveJob {
                        Span<const StringView> paths;
                        bool                   recursive;
                        RomEditor::Report      report;
                        String                 failedPath;
        };

        int removePaths(CommandContext &ctx, const Options &o) {
                if (o.operands.isEmpty()) return ctx.usage();
                StringView paths[64];
                for (size_t i = 0; i < o.operands.size(); ++i) paths[i] = treePath(o.operands[i]);
                if (!waitForFlash(ctx)) return Shell::ExitFailure;
                RemoveJob   job{Span<const StringView>(paths, o.operands.size()), o.removeAll, {}, {}};
                const Error err = RomDisk::edit(
                        [](RomEditor &editor, void *c) {
                                auto       &j = *static_cast<RemoveJob *>(c);
                                const Error e = editor.remove(j.paths, j.recursive, j.report);
                                (void)j.failedPath.assign(editor.failedPath());
                                return e;
                        },
                        &job);
                if (err == Error::IsDir) {
                        ctx.eprintf("rom: remove %s/%s: a directory (rom remove -r)\n", RomDisk::Path,
                                    job.failedPath.cStr());
                        return Shell::ExitFailure;
                }
                if (err.isError()) return failed(ctx, err, "remove", job.failedPath.view());
                char what[64];
                std::snprintf(what, sizeof(what), "%u file%s removed", static_cast<unsigned>(job.report.removed),
                              job.report.removed == 1 ? "" : "s");
                tellDone(ctx, what, job.report);
                return Shell::ExitSuccess;
        }

        int discardPending(CommandContext &ctx, const Options &o) {
                if (!o.operands.isEmpty()) return ctx.usage();
                if (!waitForFlash(ctx)) return Shell::ExitFailure;
                RomEditor::Report report;
                const Error       err = RomDisk::edit(
                        [](RomEditor &editor, void *c) { return editor.discard(*static_cast<RomEditor::Report *>(c)); },
                        &report);
                if (err.isError()) return failed(ctx, err, "discard", StringView());
                if (report.sequence == 0) {
                        ctx.printf("rom: nothing pending: the next boot takes the booted tree\n");
                } else {
                        ctx.printf("rom: the pending changes dropped (tree version %u: the booted tree's files)\n",
                                   static_cast<unsigned>(report.sequence));
                        notifyPending("The changes were dropped: nothing changes");
                }
                return Shell::ExitSuccess;
        }

        struct CompactJob {
                        CommandContext      *ctx;
                        bool                 full;
                        RomEditor::Report    report;
                        RomEditor::Shortfall shortfall;
                        bool                 more = false;
        };

        int compactFiles(CommandContext &ctx, const Options &o) {
                if (!o.operands.isEmpty()) return ctx.usage();
                if (!waitForFlash(ctx)) return Shell::ExitFailure;
                CompactJob  job{&ctx, o.fullCompact, {}, {}, false};
                const Error err = RomDisk::edit(
                        [](RomEditor &editor, void *c) {
                                auto    &j = *static_cast<CompactJob *>(c);
                                Progress p{j.ctx};
                                editor.setProgress(tellProgress, &p);
                                const Error e = editor.compact(j.report, j.more, j.full);
                                j.shortfall = editor.shortfall();
                                return e;
                        },
                        &job);
                if (err == Error::NoSpace) {
                        tellShortfall(ctx, job.shortfall);
                        return Shell::ExitFailure;
                }
                if (err.isError()) return failed(ctx, err, "compact", StringView());
                if (job.report.moved == 0) {
                        ctx.printf("rom: packed as far as it can be now\n");
                        return Shell::ExitSuccess;
                }
                char what[64];
                std::snprintf(what, sizeof(what), "%u file%s moved %s", static_cast<unsigned>(job.report.moved),
                              job.report.moved == 1 ? "" : "s", o.fullCompact ? "together" : "into the holes below");
                tellDone(ctx, what, job.report);
                if (job.more) ctx.printf("rom: their old places free at that boot: rom compact then may do more\n");
                return Shell::ExitSuccess;
        }

        // --- telling the desktop (§12b): changes take effect at the next boot ------------------

        // One notification, updated as changes follow one another; only while the panel shows
        // them (Notifications::hasServer()).
        constinit uint32_t pendingNotification = 0;

        void notifyPending(const char *what) {
                if (!Notifications::hasServer()) return;
                Notification n;
                n.urgency = Notification::Urgency::Normal;
                n.replaces = pendingNotification;
                std::snprintf(n.app, sizeof(n.app), "ROM");
                std::snprintf(n.icon, sizeof(n.icon), "system-reboot");
                std::snprintf(n.title, sizeof(n.title), "ROM changes pending");
                std::snprintf(n.body, sizeof(n.body), "%s. They take effect at the next restart.", what);
                auto [id, err] = Notifications::post(n, 0, nullptr);
                if (err.isError() && n.replaces != 0) { // the earlier one gone: a new one
                        n.replaces = 0;
                        auto [again, aerr] = Notifications::post(n, 0, nullptr);
                        id = again;
                        err = aerr;
                }
                if (err.isOk()) pendingNotification = id;
                return;
        }

        // --- export, import: the user's files to and from a folder ----------------------------

        // The list export writes beside the files: "ALIGN PATH" a line (import's alignments).
        constexpr const char *ListName = "rom-files.txt";

        Error writeAll(FileHandle &file, ByteSpan bytes, uint8_t *chunk, size_t chunkBytes) {
                for (size_t at = 0; at < bytes.size();) {
                        const size_t n = bytes.size() - at < chunkBytes ? bytes.size() - at : chunkBytes;
                        std::memcpy(chunk, bytes.data() + at, n); // not flash's address to a driver
                        auto [w, err] = file.write(ByteSpan(chunk, n), WaitForever);
                        if (err.isError()) return err;
                        if (w != n) return Error::IoError;
                        at += n;
                }
                return Error::Ok;
        }

        Error makeDirs(StringView path, const Vfs::Context &context) {
                for (size_t i = 1; i <= path.size(); ++i) {
                        if (i < path.size() && path[i] != '/') continue;
                        const Error made = Vfs::mkdir(path.left(i), context);
                        if (made.isError() && made != Error::Exists) return made;
                }
                return Error::Ok;
        }

        // `rom export DIR`: the user's files (the tree the next boot takes) into DIR at their paths,
        // and DIR/rom-files.txt; DIR new or empty (-f: written over).
        int exportFiles(CommandContext &ctx, const Options &o) {
                if (o.operands.size() != 1) return ctx.usage();
                auto [tree, terr] = RomImage::open(RomDisk::mapped(), RomDisk::firmware());
                if (terr.isError()) return noRom(ctx);
                const StringView dir(o.operands[0]);
                const Vfs::Context vctx = ctx.vfs();
                if (!o.force) {
                        auto [d, derr] = Vfs::openDirectory(dir, vctx);
                        if (derr.isOk()) {
                                auto [first, ferr] = d.next();
                                (void)first;
                                if (ferr.isOk()) {
                                        ctx.eprintf("rom: %s isn't empty (rom export -f writes over it)\n", o.operands[0]);
                                        return Shell::ExitFailure;
                                }
                        }
                }
                if (Error err = makeDirs(dir, vctx); err.isError()) return failed(ctx, err, "export", StringView());
                String list;
                uint32_t files = 0;
                uint64_t bytes = 0;
                auto *chunk = static_cast<uint8_t *>(Memory::alloc(MemSpace::Internal, 4096));
                if (chunk == nullptr) return failed(ctx, Error::NoMem, "export", StringView());
                Error err = Error::Ok;
                for (uint32_t i = 0; i < tree.count() && err.isOk(); ++i) {
                        const RomImage::Entry e = tree.entry(i);
                        if ((e.flags & RomImage::User) == 0) continue;
                        if (e.compression != RomImage::None) {
                                ctx.eprintf("rom: export: /rom/%.*s is compressed: left out\n", int(e.path.size()),
                                            e.path.data());
                                continue;
                        }
                        String target;
                        (void)target.appendf("%.*s/%.*s", int(dir.size()), dir.data(), int(e.path.size()), e.path.data());
                        const size_t slash = target.view().lastIndexOf('/');
                        err = makeDirs(target.view().left(slash), vctx);
                        if (err.isError()) break;
                        auto [file, oerr] = Vfs::open(target.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, vctx);
                        if (oerr.isError()) {
                                err = oerr;
                                break;
                        }
                        err = writeAll(*file, e.data, chunk, 4096);
                        (void)list.appendf("%u %.*s\n", 1u << e.alignLog2, int(e.path.size()), e.path.data());
                        ++files;
                        bytes += e.data.size();
                }
                Memory::free(chunk);
                if (err.isOk()) {
                        String listPath;
                        (void)listPath.appendf("%.*s/%s", int(dir.size()), dir.data(), ListName);
                        auto [file, oerr] = Vfs::open(listPath.view(), Vfs::Write | Vfs::Create | Vfs::Truncate, vctx);
                        err = oerr;
                        if (err.isOk()) {
                                auto [w, werr] = file->write(list.view(), WaitForever);
                                err = werr.isOk() && w != list.size() ? Error(Error::IoError) : werr;
                        }
                }
                if (err.isError()) return failed(ctx, err, "export", StringView());
                char kb[32];
                ctx.printf("rom: %u of the user's files (%s) exported to %s, and %s\n", static_cast<unsigned>(files),
                           bytesText(kb, o, bytes), o.operands[0], ListName);
                return Shell::ExitSuccess;
        }

        // A folder's files for import: each one's path under the folder and its source.
        struct ImportFile {
                        String     tree; // its path in the ROM
                        String     source;
                        uint64_t   size = 0;
                        uint8_t    alignLog2 = 6;
        };

        Error collect(StringView dir, StringView under, const Vfs::Context &vctx, List<ImportFile> &out) {
                auto [d, err] = Vfs::openDirectory(dir, vctx);
                if (err.isError()) return err;
                for (;;) {
                        auto [info, nerr] = d.next();
                        if (nerr == Error::EndOfFile) break;
                        if (nerr.isError()) return nerr;
                        const StringView name = info.name.view();
                        if (name == "." || name == "..") continue;
                        String source, tree;
                        (void)source.appendf("%.*s/%.*s", int(dir.size()), dir.data(), int(name.size()), name.data());
                        if (under.isEmpty()) {
                                (void)tree.append(name);
                        } else {
                                (void)tree.appendf("%.*s/%.*s", int(under.size()), under.data(), int(name.size()),
                                                   name.data());
                        }
                        if (info.isDirectory()) {
                                const Error e = collect(source.view(), tree.view(), vctx, out);
                                if (e.isError()) return e;
                                continue;
                        }
                        if (under.isEmpty() && name == ListName) continue; // export's list, not a file
                        ImportFile f;
                        f.tree = std::move(tree);
                        f.source = std::move(source);
                        f.size = info.size;
                        if (out.append(std::move(f)).isError()) return Error::NoMem;
                }
                return Error::Ok;
        }

        // `rom import DIR`: every file under DIR installed at its path under it, one change; the
        // alignments from DIR/rom-files.txt if it is there (rom export's), else 64 bytes.
        int importFiles(CommandContext &ctx, const Options &o) {
                if (o.operands.size() != 1) return ctx.usage();
                const StringView   dir(o.operands[0]);
                const Vfs::Context vctx = ctx.vfs();
                List<ImportFile>   found;
                if (Error err = collect(dir, StringView(), vctx, found); err.isError())
                        return failed(ctx, err, "import", StringView());
                if (found.size() == 0) {
                        ctx.eprintf("rom: import: no files in %s\n", o.operands[0]);
                        return Shell::ExitFailure;
                }
                // Alignments from the list.
                String listPath;
                (void)listPath.appendf("%.*s/%s", int(dir.size()), dir.data(), ListName);
                if (auto [file, lerr] = Vfs::open(listPath.view(), Vfs::Read, vctx); lerr.isOk()) {
                        String text;
                        char   buf[512];
                        for (;;) {
                                auto [n, rerr] = file->read(WritableByteSpan(reinterpret_cast<uint8_t *>(buf), sizeof(buf)),
                                                            WaitForever);
                                if (rerr.isError() || n == 0) break;
                                (void)text.append(StringView(buf, n));
                        }
                        StringView rest = text.view();
                        while (!rest.isEmpty()) {
                                size_t           nl = rest.indexOf('\n');
                                const StringView line = rest.left(nl == StringView::NPos ? rest.size() : nl);
                                rest = nl == StringView::NPos ? StringView() : rest.mid(nl + 1);
                                const size_t sp = line.indexOf(' ');
                                if (sp == StringView::NPos) continue;
                                unsigned long align = std::strtoul(String::fromView(line.left(sp)).value().cStr(), nullptr, 10);
                                const StringView path = line.mid(sp + 1);
                                for (ImportFile &f : found) {
                                        if (f.tree.view() != path) continue;
                                        uint8_t log2 = 0;
                                        while (log2 < RomImage::MaxAlignLog2 && (1ul << log2) < align) ++log2;
                                        f.alignLog2 = log2 < 6 ? 6 : log2;
                                }
                        }
                }
                List<FileSource>     files;
                List<RomEditor::Add> adds;
                if (files.reserve(found.size()).isError() || adds.reserve(found.size()).isError())
                        return failed(ctx, Error::NoMem, "import", StringView());
                const Timestamp now = Clock::now();
                for (ImportFile &f : found) {
                        auto [file, oerr] = Vfs::open(f.source.view(), Vfs::Read, vctx);
                        if (oerr.isError()) {
                                ctx.eprintf("rom: %s: %s\n", f.source.cStr(), CommandContext::errorText(oerr));
                                return Shell::ExitFailure;
                        }
                        (void)files.append(FileSource(std::move(file), f.size));
                }
                for (size_t i = 0; i < found.size(); ++i) {
                        RomEditor::Add a;
                        a.path = found[i].tree.view();
                        a.source = &files[i];
                        a.alignLog2 = found[i].alignLog2;
                        a.flags = RomImage::User;
                        a.modified = now;
                        (void)adds.append(a);
                }
                if (!waitForFlash(ctx)) return Shell::ExitFailure;
                InstallJob  job{&ctx, Span<const RomEditor::Add>(adds.data(), adds.size()), {}, {}, {}};
                const Error err = RomDisk::edit(
                        [](RomEditor &editor, void *c) {
                                auto    &j = *static_cast<InstallJob *>(c);
                                Progress p{j.ctx};
                                editor.setProgress(tellProgress, &p);
                                const Error e = editor.install(j.adds, j.report);
                                j.shortfall = editor.shortfall();
                                (void)j.failedPath.assign(editor.failedPath());
                                return e;
                        },
                        &job);
                if (err == Error::NoSpace) {
                        tellShortfall(ctx, job.shortfall);
                        return Shell::ExitFailure;
                }
                if (err.isError()) return failed(ctx, err, "import", job.failedPath.view());
                char what[96], kb[32];
                std::snprintf(what, sizeof(what), "%u file%s imported (%s)", static_cast<unsigned>(job.report.written),
                              job.report.written == 1 ? "" : "s", bytesText(kb, o, job.report.bytesWritten));
                tellDone(ctx, what, job.report);
                return Shell::ExitSuccess;
        }

        // --- repack: in recovery (§12s) ----------------------------------------------------------

        // `rom repack`: every file packed tight, by recovery (the booted tree can't move): the
        // request left on /flash, then a restart into recovery, which comes back when it's done.
        int repackFiles(CommandContext &ctx, const Options &o) {
                if (!o.operands.isEmpty()) return ctx.usage();
                Trees trees;
                int   status = 0;
                if (!load(ctx, trees, status)) return status;
                RomSpace space;
                if (space.build(trees.hasPending ? trees.pendingImage : *trees.booted, nullptr).isOk()) {
                        char    a[32], b[32];
                        Options human;
                        ctx.printf("rom: %s of padding and %s of holes to gather; most of the ROM's %s moves\n",
                                   bytesText(a, human, space.summary().paddingBytes),
                                   bytesText(b, human, space.summary().holeBytes),
                                   bytesText(a, human, space.summary().filesBytes));
                }
                Vfs::Context context;
                const Error  made = Vfs::mkdir("/flash/update", context);
                Error        err = made.isError() && made != Error::Exists ? made : Error(Error::Ok);
                if (err.isOk()) {
                        auto [file, oerr] =
                                Vfs::open(RomDisk::RepackPath, Vfs::Write | Vfs::Create | Vfs::Truncate, context);
                        err = oerr;
                        if (err.isOk()) {
                                auto [n, werr] = file->write(StringView("repack\n"), WaitForever);
                                err = werr;
                                (void)n;
                        }
                }
                if (err.isOk()) err = Vfs::sync();
                if (err.isOk()) err = System::setNextFirmware(System::Firmware::Recovery);
                if (err.isError()) {
                        (void)Vfs::remove(RomDisk::RepackPath, context);
                        return failed(ctx, err, "repack", StringView());
                }
                ctx.printf("rom: restarting into recovery to repack the ROM; it restarts the system when done\n");
                Thread::sleep(500); // the line out first
                System::restart();
        }

        // --- the command ----------------------------------------------------------------------

        int runRom(CommandContext &ctx) {
                const Span<char *const> args = ctx.args();
                const char             *form = args.size() > 1 && args[1][0] != '-' ? args[1] : "info";
                const size_t            first = args.size() > 1 && args[1][0] != '-' ? 2 : 1;
                Options                 o;
                size_t                  count = 0;
                bool                    optionsDone = false;
                // Options anywhere (before "--"), operands kept in order.
                for (size_t i = first; i < args.size(); ++i) {
                        const StringView a(args[i]);
                        if (optionsDone || a.size() < 2 || a[0] != '-') {
                                if (count == sizeof(o.given) / sizeof(o.given[0])) return ctx.usage();
                                o.given[count++] = args[i];
                        } else if (a == "--") {
                                optionsDone = true;
                        } else if (a == "--raw") {
                                o.raw = true;
                        } else if (a == "--pending") {
                                o.pending = true;
                        } else if (a == "--manifest") {
                                o.manifest = true;
                        } else if (a == "--sectors") {
                                o.sectors = true;
                        } else if (a == "--full") {
                                o.fullCompact = true;
                        } else if (a == "--align") {
                                if (i + 1 >= args.size()) return ctx.usage();
                                o.align = args[++i];
                        } else if (a[1] != '-') {
                                for (size_t k = 1; k < a.size(); ++k) {
                                        if (a[k] == 'l') {
                                                o.longForm = true;
                                        } else if (a[k] == 'R') {
                                                o.recursive = true;
                                        } else if (a[k] == 'r') {
                                                o.removeAll = true;
                                        } else if (a[k] == 'f') {
                                                o.force = true;
                                        } else {
                                                return ctx.usage();
                                        }
                                }
                        } else {
                                return ctx.usage();
                        }
                }
                o.operands = Span<char *const>(o.given, count);
                const StringView f(form);
                const bool       listing = f == "ls";
                if ((o.longForm || o.recursive) && !listing) return ctx.usage();
                if (o.pending && !listing && f != "diff") return ctx.usage();
                if (o.manifest && f != "diff") return ctx.usage();
                if (o.sectors && f != "map") return ctx.usage();
                if (o.removeAll && f != "remove") return ctx.usage();
                if (o.force && f != "export") return ctx.usage();
                if (o.fullCompact && f != "compact") return ctx.usage();
                if (o.align != nullptr && f != "install") return ctx.usage();
                if (f == "info") return o.operands.isEmpty() ? showInfo(ctx, o) : ctx.usage();
                if (listing) return list(ctx, o);
                if (f == "stat") return statPaths(ctx, o);
                if (f == "map") return o.operands.isEmpty() ? showMap(ctx, o) : ctx.usage();
                if (f == "log") return o.operands.isEmpty() ? showLog(ctx, o) : ctx.usage();
                if (f == "diff") return showDiff(ctx, o);
                if (f == "manifest") return o.operands.isEmpty() ? showManifest(ctx, o) : ctx.usage();
                if (f == "check") return check(ctx, o);
                if (f == "install") return install(ctx, o);
                if (f == "remove") return removePaths(ctx, o);
                if (f == "discard") return discardPending(ctx, o);
                if (f == "compact") return compactFiles(ctx, o);
                if (f == "export") return exportFiles(ctx, o);
                if (f == "import") return importFiles(ctx, o);
                if (f == "repack") return repackFiles(ctx, o);
                return ctx.usage();
        }

} // namespace

BMDS_SHELL_COMMAND(rom, runRom,
                   "[info | ls [-lR] [--pending] [PATH] | stat PATH... | map [--sectors] | log | "
                   "diff [--manifest | --pending] | manifest | check [PATH...] | install [--align SIZE] SRC... DEST | "
                   "remove [-r] PATH... | discard | compact [--full] | repack | export [-f] DIR | import DIR] [--raw]",
                   "Show everything /rom holds (the files that come with the firmware): the partition, its "
                   "trees, each file, where each byte goes, the log of versions, what differs from the "
                   "firmware or the pending tree; check the files' hashes; change it (each change takes effect at "
                   "the next boot); or copy the user's files to a folder and back")

BMDS_NAMESPACE_END
